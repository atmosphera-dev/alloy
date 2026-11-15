// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/model.h"
#include <cmath>
#include <set>
#include <vector>
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
namespace alloy::guest::accessibility {
namespace {
bool Counter(const base::DictValue& value, const char* name, uint64_t* output) {
  const auto* text = value.FindString(name);
  return text && base::StringToUint64(*text, output);
}
bool Text(const base::DictValue& value, const char* name) {
  const auto* text = value.FindString(name);
  return text && base::IsStringUTF8(*text);
}
bool TextList(const base::DictValue& value, const char* name) {
  const auto* values = value.FindList(name);
  if (!values) return false;
  for (const auto& text : *values) if (!text.is_string() || !base::IsStringUTF8(text.GetString())) return false;
  return true;
}
bool Attributes(const base::DictValue& data) {
  for (const auto [key, value] : data) {
    if (!base::IsStringUTF8(key)) return false;
    if (value.is_bool() || value.is_int()) continue;
    if (value.is_double() && std::isfinite(value.GetDouble())) continue;
    if (value.is_string() && base::IsStringUTF8(value.GetString())) continue;
    if (!value.is_list()) return false;
    bool integers = true, strings = true;
    for (const auto& item : value.GetList()) {
      integers &= item.is_int();
      strings &= item.is_string() && base::IsStringUTF8(item.GetString());
    }
    if (!integers && !strings) return false;
  }
  return true;
}
}
bool ValidateTree(const base::DictValue& tree) {
  uint64_t generation = 0;
  const auto* id = tree.FindString("id");
  const auto* nodes = tree.FindList("nodes");
  auto root = tree.FindInt("root");
  auto focus = tree.FindInt("focus");
  if (!id || id->empty() || !base::IsStringUTF8(*id) || !nodes || !root || *root == 0 || !focus ||
      !Counter(tree, "generation", &generation) || !generation ||
      !Text(tree, "parent") || !Text(tree, "focused_tree") || !tree.FindBool("loaded")) return false;
  std::map<int, const base::ListValue*> children;
  for (const auto& item : *nodes) {
    if (!item.is_dict()) return false;
    const auto& node = item.GetDict();
    auto number = node.FindInt("id");
    const auto* ids = node.FindList("children");
    const auto* bounds = node.FindList("bounds");
    if (!number || *number == 0 || !ids || !children.emplace(*number, ids).second ||
        !Counter(node, "generation", &generation) || !generation ||
        !Text(node, "role") || !Text(node, "name") || !Text(node, "value") ||
        !Text(node, "description") || !Text(node, "child_tree") ||
        !TextList(node, "states") || !TextList(node, "actions") ||
        !node.FindBool("ignored") || !node.FindBool("bounds_available") ||
        !bounds || bounds->size() != 4 || !node.FindDict("attributes") ||
        !Attributes(*node.FindDict("attributes"))) return false;
    for (size_t i = 0; i < bounds->size(); ++i) {
      if (!(*bounds)[i].is_double() && !(*bounds)[i].is_int()) return false;
      double value = (*bounds)[i].GetDouble();
      if (!std::isfinite(value) || (i >= 2 && value < 0)) return false;
    }
  }
  if (!children.contains(*root) || (*focus != 0 && !children.contains(*focus))) return false;
  std::set<int> visited;
  std::vector<int> work{*root};
  while (!work.empty()) {
    int node = work.back(); work.pop_back();
    auto found = children.find(node);
    if (found == children.end() || !visited.insert(node).second) return false;
    for (const auto& child : *found->second) {
      if (!child.is_int() || child.GetInt() == 0) return false;
      work.push_back(child.GetInt());
    }
  }
  return visited.size() == children.size();
}
Model::Result Model::Apply(const base::DictValue& update) {
  uint64_t sequence = 0, geometry = 0, document = 0, time = 0;
  auto snapshot = update.FindBool("snapshot");
  const auto* changed = update.FindList("trees");
  auto scale = update.FindDouble("scale");
  if (!Counter(update, "sequence", &sequence) || !sequence ||
      !Counter(update, "geometry", &geometry) || !geometry ||
      !Counter(update, "document", &document) || !Counter(update, "capture_us", &time) ||
      !Text(update, "root_tree") ||
      !update.FindBool("view_focused") ||
      !snapshot || !changed || !scale || !std::isfinite(*scale) || *scale <= 0 ||
      !update.FindList("events")) { Clear(); return Result::kResync; }
  if (sequence <= sequence_) return Result::kOld;
  if (!*snapshot && (!ready_ || sequence != sequence_ + 1)) {
    Clear(); return Result::kResync;
  }
  std::map<std::string, base::DictValue> candidate;
  if (!*snapshot) for (const auto& [id, tree] : trees_) candidate.emplace(id, tree.Clone());
  if (const auto* removed = update.FindList("removed")) for (const auto& id : *removed) {
    if (!id.is_string()) { Clear(); return Result::kResync; }
    candidate.erase(id.GetString());
  }
  std::set<std::string> ids;
  for (const auto& tree : *changed) {
    if (!tree.is_dict() || !ValidateTree(tree.GetDict())) { Clear(); return Result::kResync; }
    const auto& id = *tree.GetDict().FindString("id");
    if (!ids.insert(id).second) { Clear(); return Result::kResync; }
    candidate.insert_or_assign(id, tree.GetDict().Clone());
  }
  std::map<std::string, size_t> hosts;
  for (const auto& [id, tree] : candidate) {
    for (const auto& node : *tree.FindList("nodes")) {
      const auto& child = *node.GetDict().FindString("child_tree");
      if (!child.empty() && candidate.contains(child)) {
        if (*candidate.at(child).FindString("parent") != id || ++hosts[child] != 1) {
          Clear(); return Result::kResync;
        }
      }
    }
    std::set<std::string> seen{id};
    const auto* parent = tree.FindString("parent");
    while (parent && !parent->empty() && candidate.contains(*parent)) {
      if (!seen.insert(*parent).second) { Clear(); return Result::kResync; }
      parent = candidate.at(*parent).FindString("parent");
    }
  }
  trees_ = std::move(candidate); sequence_ = sequence; ready_ = true;
  return Result::kApplied;
}
void Model::Clear() { ready_ = false; trees_.clear(); }
base::DictValue Model::Snapshot(const base::DictValue& context) const {
  auto result = context.Clone();
  base::ListValue trees;
  for (const auto& [id, tree] : trees_) trees.Append(tree.Clone());
  result.Set("trees", std::move(trees)); result.Set("snapshot", true);
  result.Remove("removed");
  return result;
}
UpdateParts::Result UpdateParts::Add(uint64_t sequence, uint64_t total,
    uint64_t offset, const std::string& part, std::string* complete) {
  if (!complete || !sequence || !total || part.empty() || total > partial_.max_size() ||
      offset > total || part.size() > total - offset ||
      (!offset && !partial_.empty())) { Clear(); return Result::kResync; }
  if (!offset) { sequence_ = sequence; total_ = total; }
  if (sequence != sequence_ || total != total_ || offset != partial_.size()) {
    Clear(); return Result::kResync;
  }
  partial_ += part;
  if (partial_.size() < total_) return Result::kPending;
  *complete = std::move(partial_); Clear();
  return Result::kReady;
}
void UpdateParts::Clear() { partial_.clear(); sequence_ = total_ = 0; }
}  // namespace alloy::guest::accessibility
