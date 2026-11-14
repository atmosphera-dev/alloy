// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/tree_stream.h"

#include <cmath>
#include <limits>
#include <set>
#include "base/json/values_util.h"
#include "base/strings/string_number_conversions.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_node.h"
#include "ui/accessibility/ax_tree_id.h"

namespace alloy::guest::accessibility {
namespace {
bool Finite(const gfx::RectF& rectangle) {
  return std::isfinite(rectangle.x()) && std::isfinite(rectangle.y()) &&
      std::isfinite(rectangle.width()) && std::isfinite(rectangle.height()) &&
      rectangle.width() >= 0 && rectangle.height() >= 0;
}
bool Valid(const ui::AXTreeUpdate& update) {
  std::set<int> ids;
  for (const auto& node : update.nodes) {
    if (node.id == ui::kInvalidAXNodeID || !ids.insert(node.id).second || !Finite(node.relative_bounds.bounds)) return false;
    std::set<int> children;
    for (int child : node.child_ids)
      if (child == ui::kInvalidAXNodeID || child == node.id || !children.insert(child).second) return false;
    for (const auto& [attribute, value] : node.float_attributes)
      if (!std::isfinite(value)) return false;
    const auto& child_tree = node.GetStringAttribute(ax::mojom::StringAttribute::kChildTreeId);
    if (!child_tree.empty() && !ParseTreeId(child_tree)) return false;
  }
  return true;
}
base::ListValue Rectangle(const gfx::RectF& bounds) {
  base::ListValue result;
  result.Append(bounds.x()); result.Append(bounds.y());
  result.Append(bounds.width()); result.Append(bounds.height());
  return result;
}
std::string IdString(const ui::AXTreeID& id) {
  return id == ui::AXTreeIDUnknown() ? std::string() : id.ToString();
}
}

TreeStream::Tree::Tree(uint64_t epoch) : generation(epoch) { value.AddObserver(this); }
TreeStream::Tree::~Tree() { value.RemoveObserver(this); }
void TreeStream::Tree::OnNodeWillBeDeleted(ui::AXTree*, ui::AXNode* node) { nodes.erase(node->id()); }
void TreeStream::Tree::OnNodeCreated(ui::AXTree*, ui::AXNode* node) {
  if (next_node != std::numeric_limits<uint64_t>::max()) nodes[node->id()] = next_node++;
}
TreeStream::TreeStream() = default;
TreeStream::~TreeStream() = default;
std::optional<ui::AXTreeID> ParseTreeId(const std::string& text) {
  auto token = base::ValueToUnguessableToken(base::Value(text));
  return token ? std::optional(ui::AXTreeID::FromToken(*token)) : std::nullopt;
}
bool TreeStream::Update(const ui::AXUpdatesAndEvents& update) {
  if (update.ax_tree_id == ui::AXTreeIDUnknown()) return false;
  auto found = trees_.find(update.ax_tree_id);
  if (found == trees_.end()) {
    if (update.updates.empty() || !update.updates.front().root_id ||
        next_tree_ == std::numeric_limits<uint64_t>::max()) return false;
    found = trees_.emplace(update.ax_tree_id, std::make_unique<Tree>(next_tree_++)).first;
  }
  for (const auto& change : update.updates) {
    if (!Valid(change) ||
        (change.has_tree_data && change.tree_data.tree_id != update.ax_tree_id) ||
        !found->second->value.Unserialize(change)) {
      trees_.erase(found);
      return false;
    }
  }
  return found->second->value.root() != nullptr;
}
bool TreeStream::Location(const ui::AXTreeID& id, const ui::AXLocationAndScrollUpdates& update) {
  auto found = trees_.find(id);
  if (found == trees_.end()) return true;
  // Apply one atomic native update so descendants inherit changed scroll/offset
  // containers, rather than patching only the visible node's screen rectangle.
  std::map<int, ui::AXNodeData> changed;
  for (const auto& location : update.location_changes) {
    auto* node = found->second->value.GetFromId(location.id);
    if (!Finite(location.new_location.bounds)) return false;
    if (!node) continue;
    changed.try_emplace(location.id, node->data()).first->second.relative_bounds = location.new_location;
  }
  for (const auto& scroll : update.scroll_changes) {
    auto* node = found->second->value.GetFromId(scroll.id);
    if (!node) continue;
    auto& data = changed.try_emplace(scroll.id, node->data()).first->second;
    data.AddIntAttribute(ax::mojom::IntAttribute::kScrollX, scroll.scroll_x);
    data.AddIntAttribute(ax::mojom::IntAttribute::kScrollY, scroll.scroll_y);
  }
  ui::AXTreeUpdate change;
  for (auto& [node, data] : changed) change.nodes.push_back(std::move(data));
  return found->second->value.Unserialize(change);
}
void TreeStream::Remove(const ui::AXTreeID& id) { trees_.erase(id); }
void TreeStream::Prune(const std::function<bool(const ui::AXTreeID&)>& owns) {
  for (auto found = trees_.begin(); found != trees_.end();) {
    if (!owns(found->first)) found = trees_.erase(found);
    else ++found;
  }
}
void TreeStream::Clear() { trees_.clear(); }
const ui::AXTree* TreeStream::Find(const ui::AXTreeID& id) const {
  auto found = trees_.find(id);
  return found == trees_.end() ? nullptr : &found->second->value;
}
const ui::AXNode* TreeStream::Resolve(const std::string& id, uint64_t generation,
                                      int node, uint64_t incarnation) const {
  auto token = ParseTreeId(id);
  if (!token) return nullptr;
  auto found = trees_.find(*token);
  if (found == trees_.end() || found->second->generation != generation) return nullptr;
  auto current = found->second->nodes.find(node);
  return current != found->second->nodes.end() && current->second == incarnation
      ? found->second->value.GetFromId(node) : nullptr;
}
std::optional<gfx::RectF> TreeStream::Bounds(const ui::AXTreeID& id, int node) const {
  auto* tree = Find(id);
  auto* native = tree ? tree->GetFromId(node) : nullptr;
  if (!native) return std::nullopt;
  auto result = tree->GetTreeBounds(native, nullptr, false);
  std::set<ui::AXTreeID> visited{id};
  while (tree->data().parent_tree_id != ui::AXTreeIDUnknown()) {
    auto child_id = tree->GetAXTreeID();
    auto parent_id = tree->data().parent_tree_id;
    if (!visited.insert(parent_id).second || !(tree = Find(parent_id))) return std::nullopt;
    auto hosts = tree->GetNodeIdsForChildTreeId(child_id);
    if (hosts.size() != 1) return std::nullopt;
    auto* host = tree->GetFromId(*hosts.begin());
    result = tree->RelativeToTreeBounds(host, result, nullptr, false);
  }
  return Finite(result) ? std::optional(result) : std::nullopt;
}
base::DictValue TreeStream::Serialize(const ui::AXTreeID& id, const Tree& tree,
    double pixel_scale, bool bounds_ready) const {
  base::DictValue result;
  const auto& data = tree.value.data();
  result.Set("id", id.ToString()); result.Set("generation", base::NumberToString(tree.generation));
  result.Set("parent", IdString(data.parent_tree_id));
  result.Set("focused_tree", IdString(data.focused_tree_id));
  result.Set("focus", data.focus_id); result.Set("loaded", data.loaded);
  result.Set("root", tree.value.root()->id());
  result.Set("selection_anchor", data.sel_anchor_object_id);
  result.Set("selection_anchor_offset", data.sel_anchor_offset);
  result.Set("selection_focus", data.sel_focus_object_id);
  result.Set("selection_focus_offset", data.sel_focus_offset);
  base::ListValue nodes;
  // Iterative traversal avoids recursion on deeply nested untrusted pages.
  std::vector<const ui::AXNode*> work{tree.value.root()};
  while (!work.empty()) {
    auto* node = work.back(); work.pop_back();
    const auto& value = node->data();
    base::DictValue item;
    item.Set("id", node->id());
    auto incarnation = tree.nodes.find(node->id());
    item.Set("generation", base::NumberToString(incarnation == tree.nodes.end() ? 0 : incarnation->second));
    item.Set("role", ui::ToString(value.role));
    item.Set("name", value.GetStringAttribute(ax::mojom::StringAttribute::kName));
    item.Set("value", value.GetStringAttribute(ax::mojom::StringAttribute::kValue));
    item.Set("description", value.GetStringAttribute(ax::mojom::StringAttribute::kDescription));
    item.Set("child_tree", value.GetStringAttribute(ax::mojom::StringAttribute::kChildTreeId));
    item.Set("ignored", node->IsIgnored());
    base::ListValue states, actions, children;
    for (int i = 0; i <= static_cast<int>(ax::mojom::State::kMaxValue); ++i)
      if (value.HasState(static_cast<ax::mojom::State>(i))) states.Append(ui::ToString(static_cast<ax::mojom::State>(i)));
    for (int i = 1; i <= static_cast<int>(ax::mojom::Action::kMaxValue); ++i)
      if (value.HasAction(static_cast<ax::mojom::Action>(i))) actions.Append(ui::ToString(static_cast<ax::mojom::Action>(i)));
    for (const ui::AXNode* child : node->children()) { children.Append(child->id()); work.push_back(child); }
    item.Set("states", std::move(states)); item.Set("actions", std::move(actions));
    item.Set("children", std::move(children));
    auto bounds = bounds_ready ? Bounds(id, node->id()) : std::nullopt;
    if (bounds) bounds->Scale(1.0f / static_cast<float>(pixel_scale));
    item.Set("bounds_available", bounds.has_value());
    item.Set("bounds", Rectangle(bounds.value_or(gfx::RectF())));
    base::DictValue attributes;
    for (const auto& [attribute, text] : value.string_attributes) attributes.Set(ui::ToString(attribute), text);
    for (const auto& [attribute, number] : value.int_attributes) attributes.Set(ui::ToString(attribute), number);
    for (const auto& [attribute, number] : value.float_attributes) attributes.Set(ui::ToString(attribute), number);
    value.bool_attributes.ForEach([&attributes](ax::mojom::BoolAttribute attribute, bool flag) {
      attributes.Set(ui::ToString(attribute), flag);
    });
    for (const auto& [attribute, values] : value.intlist_attributes) {
      base::ListValue list;
      for (int number : values) list.Append(number);
      attributes.Set(ui::ToString(attribute), std::move(list));
    }
    for (const auto& [attribute, values] : value.stringlist_attributes) {
      base::ListValue list;
      for (const auto& text : values) list.Append(text);
      attributes.Set(ui::ToString(attribute), std::move(list));
    }
    item.Set("attributes", std::move(attributes));
    nodes.Append(std::move(item));
  }
  result.Set("nodes", std::move(nodes));
  return result;
}
base::DictValue TreeStream::Snapshot(double pixel_scale, bool bounds_ready) const {
  base::DictValue result;
  base::ListValue trees;
  for (const auto& [id, tree] : trees_) trees.Append(Serialize(id, *tree, pixel_scale, bounds_ready));
  result.Set("trees", std::move(trees));
  return result;
}
base::DictValue TreeStream::Delta(const ui::AXTreeID& id, double pixel_scale, bool bounds_ready) const {
  // Derived view bounds can change in other trees when an embedding node or
  // scroll container moves. Replace all currently known frame subtrees in this
  // delta so every consumer sees the same geometry, without independent math.
  auto result = Snapshot(pixel_scale, bounds_ready);
  base::ListValue removed;
  auto found = trees_.find(id);
  if (found == trees_.end()) removed.Append(id.ToString());
  result.Set("removed", std::move(removed));
  return result;
}
}  // namespace alloy::guest::accessibility
