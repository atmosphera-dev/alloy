// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/tree_stream.h"
#include "brave/alloy/guest/accessibility/model.h"
#include "brave/alloy/guest/accessibility/ax_actions.h"
#include "brave/alloy/guest/protocol/session.h"
#include <limits>
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/ax_node_data.h"
namespace alloy::guest::accessibility {
namespace {
ui::AXUpdatesAndEvents Initial(ui::AXTreeID id) {
  ui::AXUpdatesAndEvents result;
  result.ax_tree_id = id;
  ui::AXTreeUpdate update;
  update.root_id = 1; update.has_tree_data = true;
  update.tree_data.tree_id = id; update.tree_data.loaded = true;
  ui::AXNodeData root, button;
  root.id = 1; root.role = ax::mojom::Role::kRootWebArea; root.child_ids = {2};
  button.id = 2; button.role = ax::mojom::Role::kButton;
  button.AddStringAttribute(ax::mojom::StringAttribute::kName, "Native button");
  button.relative_bounds.bounds = gfx::RectF(10, 20, 100, 30);
  update.nodes.push_back(root); update.nodes.push_back(button);
  result.updates.push_back(std::move(update));
  return result;
}
base::DictValue Message(TreeStream& trees, int sequence, bool snapshot) {
  auto update = trees.Snapshot();
  update.Set("sequence", std::to_string(sequence)); update.Set("snapshot", snapshot);
  update.Set("document", "1"); update.Set("geometry", "2"); update.Set("capture_us", "30");
  update.Set("root_tree", std::string(*(*update.FindList("trees"))[0].GetDict().FindString("id")));
  update.Set("view_focused", true);
  update.Set("scale", 1.5); update.Set("events", base::ListValue());
  return update;
}
uint64_t NodeGeneration(TreeStream& trees, int id) {
  auto snapshot = trees.Snapshot();
  for (const auto& item : *(*snapshot.FindList("trees"))[0].GetDict().FindList("nodes")) {
    if (item.GetDict().FindInt("id") == id) {
      uint64_t generation = 0;
      EXPECT_TRUE(base::StringToUint64(*item.GetDict().FindString("generation"), &generation));
      return generation;
    }
  }
  return 0;
}
TEST(GuestAccessibility, SnapshotPreservesNativeSemanticsAndBounds) {
  TreeStream trees;
  auto id = ui::AXTreeID::CreateNewAXTreeID();
  ASSERT_TRUE(trees.Update(Initial(id)));
  auto message = Message(trees, 1, true);
  Model client;
  EXPECT_EQ(client.Apply(message), Model::Result::kApplied);
  const auto& tree = (*message.FindList("trees"))[0].GetDict();
  EXPECT_EQ(*tree.FindString("id"), id.ToString());
  const auto& node = (*tree.FindList("nodes"))[1].GetDict();
  EXPECT_EQ(*node.FindString("name"), "Native button");
  EXPECT_EQ(trees.Bounds(id, 2), gfx::RectF(10, 20, 100, 30));
  EXPECT_NE(trees.Resolve(id.ToString(), 1, 2, NodeGeneration(trees, 2)), nullptr);
}
TEST(GuestAccessibility, DeletedNodeCannotActOnReusedNumericId) {
  TreeStream trees;
  auto id = ui::AXTreeID::CreateNewAXTreeID();
  ASSERT_TRUE(trees.Update(Initial(id)));
  const auto original = NodeGeneration(trees, 2);
  ui::AXUpdatesAndEvents change;
  change.ax_tree_id = id;
  ui::AXTreeUpdate remove;
  ui::AXNodeData root;
  root.id = 1; root.role = ax::mojom::Role::kRootWebArea;
  remove.nodes.push_back(root); change.updates.push_back(remove);
  ASSERT_TRUE(trees.Update(change));
  ASSERT_TRUE(trees.Update(Initial(id)));
  EXPECT_EQ(trees.Resolve(id.ToString(), 1, 2, original), nullptr);
  EXPECT_NE(original, NodeGeneration(trees, 2));
  EXPECT_NE(trees.Resolve(id.ToString(), 1, 2, NodeGeneration(trees, 2)), nullptr);
}
TEST(GuestAccessibility, TreeRetirementCannotResurrectOriginalGeneration) {
  TreeStream trees;
  auto id = ui::AXTreeID::CreateNewAXTreeID();
  ASSERT_TRUE(trees.Update(Initial(id)));
  const auto original = NodeGeneration(trees, 2);
  trees.Clear(); ASSERT_TRUE(trees.Update(Initial(id)));
  EXPECT_EQ(trees.Resolve(id.ToString(), 1, 2, original), nullptr);
  EXPECT_NE(trees.Resolve(id.ToString(), 2, 2, NodeGeneration(trees, 2)), nullptr);
}
TEST(GuestAccessibility, DeltaGapRequiresFullResync) {
  TreeStream trees;
  ASSERT_TRUE(trees.Update(Initial(ui::AXTreeID::CreateNewAXTreeID())));
  Model client;
  EXPECT_EQ(client.Apply(Message(trees, 1, true)), Model::Result::kApplied);
  EXPECT_EQ(client.Apply(Message(trees, 3, false)), Model::Result::kResync);
  EXPECT_FALSE(client.ready());
  EXPECT_EQ(client.Apply(Message(trees, 4, false)), Model::Result::kResync);
  EXPECT_EQ(client.Apply(Message(trees, 5, true)), Model::Result::kApplied);
  EXPECT_TRUE(client.ready());
  EXPECT_EQ(client.Apply(Message(trees, 4, true)), Model::Result::kOld);
}
TEST(GuestAccessibility, InvalidRelationshipNeverPublishesPartialTree) {
  TreeStream trees;
  ASSERT_TRUE(trees.Update(Initial(ui::AXTreeID::CreateNewAXTreeID())));
  auto update = Message(trees, 1, true);
  auto& node = (*(*update.FindList("trees"))[0].GetDict().FindList("nodes"))[1].GetDict();
  node.FindList("children")->Append(1);
  Model client;
  EXPECT_EQ(client.Apply(update), Model::Result::kResync);
  EXPECT_FALSE(client.ready());
}
TEST(GuestAccessibility, DuplicateNativeChildIsRejected) {
  TreeStream trees;
  auto update = Initial(ui::AXTreeID::CreateNewAXTreeID());
  update.updates.front().nodes.front().child_ids.push_back(2);
  EXPECT_FALSE(trees.Update(update));
}
TEST(GuestAccessibility, NativeScrollMovesDescendantBounds) {
  TreeStream trees;
  auto id = ui::AXTreeID::CreateNewAXTreeID();
  auto initial = Initial(id);
  initial.updates.front().nodes.front().relative_bounds.bounds = gfx::RectF(0, 0, 1000, 1000);
  initial.updates.front().nodes[1].relative_bounds.offset_container_id = 1;
  ASSERT_TRUE(trees.Update(initial));
  ui::AXLocationAndScrollUpdates changes;
  changes.scroll_changes.emplace_back(1, 0, 10);
  ASSERT_TRUE(trees.Location(id, changes));
  ASSERT_TRUE(trees.Bounds(id, 2));
  EXPECT_EQ(trees.Bounds(id, 2)->y(), 10);
}
TEST(GuestAccessibility, GeneratedRendererIdsRemainSignedAcrossTreesAndActions) {
  TreeStream trees;
  const auto id = ui::AXTreeID::CreateNewAXTreeID();
  auto initial = Initial(id);
  const int generated = ui::kFirstGeneratedRendererNodeID;
  initial.updates[0].nodes[0].child_ids = {generated};
  initial.updates[0].nodes[1].id = generated;
  initial.updates[0].tree_data.focus_id = generated;
  ASSERT_TRUE(trees.Update(initial));
  Model model;
  EXPECT_EQ(model.Apply(Message(trees, 1, true)), Model::Result::kApplied);
  EXPECT_NE(trees.Resolve(id.ToString(), 1, generated, NodeGeneration(trees, generated)), nullptr);
  base::DictValue details;
  details.Set("tree", id.ToString()); details.Set("node", generated); details.Set("action", "default");
  auto action = ParseAction(details); ASSERT_TRUE(action);
  EXPECT_EQ(action->target_node_id, generated);
}
TEST(GuestAccessibility, LocationsMayPrecedeTheirInitialTreeOrNewNodes) {
  TreeStream trees;
  const auto id = ui::AXTreeID::CreateNewAXTreeID();
  ui::AXLocationAndScrollUpdates changes;
  changes.scroll_changes.emplace_back(100, 0, 10);
  EXPECT_TRUE(trees.Location(id, changes));
  ASSERT_TRUE(trees.Update(Initial(id)));
  EXPECT_TRUE(trees.Location(id, changes));
  EXPECT_NE(trees.Find(id), nullptr);
}
TEST(GuestAccessibility, NativePixelBoundsNormalizeOnceAndPendingGeometryIsUnavailable) {
  TreeStream trees;
  const auto id = ui::AXTreeID::CreateNewAXTreeID();
  auto initial = Initial(id);
  initial.updates[0].nodes[1].relative_bounds.bounds = gfx::RectF(60, 120, 270, 60);
  ASSERT_TRUE(trees.Update(initial));
  auto ready = trees.Snapshot(1.5, true);
  const auto& node = (*(*ready.FindList("trees"))[0].GetDict().FindList("nodes"))[1].GetDict();
  EXPECT_TRUE(*node.FindBool("bounds_available"));
  const auto& bounds = *node.FindList("bounds");
  EXPECT_DOUBLE_EQ(bounds[0].GetDouble(), 40); EXPECT_DOUBLE_EQ(bounds[1].GetDouble(), 80);
  EXPECT_DOUBLE_EQ(bounds[2].GetDouble(), 180); EXPECT_DOUBLE_EQ(bounds[3].GetDouble(), 40);
  auto pending = trees.Snapshot(1.5, false);
  EXPECT_FALSE(*(*(*pending.FindList("trees"))[0].GetDict().FindList("nodes"))[1].GetDict().FindBool("bounds_available"));
}
TEST(GuestAccessibility, ActionGrammarOnlyAdmitsTypedNativeOperations) {
  base::DictValue details;
  details.Set("tree", ui::AXTreeID::CreateNewAXTreeID().ToString()); details.Set("node", 2);
  details.Set("action", "default");
  ASSERT_TRUE(ParseAction(details));
  EXPECT_EQ(ParseAction(details)->action, ax::mojom::Action::kDoDefault);
  details.Set("action", "increment"); ASSERT_TRUE(ParseAction(details));
  EXPECT_EQ(ParseAction(details)->action, ax::mojom::Action::kIncrement);
  EXPECT_TRUE(IsActionKind("decrement")); EXPECT_TRUE(IsActionKind("scroll-forward"));
  details.Set("action", "javascript"); EXPECT_FALSE(ParseAction(details));
  details.Set("action", "selection"); details.Set("anchor", -1); details.Set("focus", 0);
  EXPECT_FALSE(ParseAction(details));
  details.Set("anchor", 0); EXPECT_TRUE(ParseAction(details));
}
TEST(GuestAccessibility, MalformedTreeTokensNeverReachNativeCheckConstructors) {
  base::DictValue details;
  details.Set("tree", "not-a-native-token"); details.Set("node", 2); details.Set("action", "default");
  EXPECT_FALSE(ParseAction(details));
  EXPECT_FALSE(ParseTreeId("not-a-native-token"));
  TreeStream trees;
  EXPECT_EQ(trees.Resolve("not-a-native-token", 1, 2, 1), nullptr);
  auto update = Initial(ui::AXTreeID::CreateNewAXTreeID());
  update.updates[0].nodes[0].AddStringAttribute(ax::mojom::StringAttribute::kChildTreeId, "not-a-native-token");
  EXPECT_FALSE(trees.Update(update));
}
TEST(GuestAccessibility, MultipartForestIsNotLimitedToOneEnvelope) {
  UpdateParts parts;
  const std::string original(kMaxControlPayload + 100, 'x');
  std::string complete;
  size_t offset = 0;
  while (offset < original.size()) {
    auto part = original.substr(offset, ALLOY_GUEST_CONTROL_CHUNK_SIZE);
    auto result = parts.Add(1, original.size(), offset, part, &complete);
    offset += part.size();
    EXPECT_EQ(result, offset == original.size() ? UpdateParts::Result::kReady : UpdateParts::Result::kPending);
  }
  EXPECT_EQ(complete, original);
}
TEST(GuestAccessibility, MultipartGapAndWrongSizesRequestResync) {
  UpdateParts parts;
  std::string complete;
  EXPECT_EQ(parts.Add(1, 9, 0, "abc", &complete), UpdateParts::Result::kPending);
  EXPECT_EQ(parts.Add(1, 9, 6, "ghi", &complete), UpdateParts::Result::kResync);
  EXPECT_TRUE(complete.empty());
  EXPECT_EQ(parts.Add(2, 3, 0, "new", &complete), UpdateParts::Result::kReady);
  EXPECT_EQ(complete, "new");
  EXPECT_EQ(parts.Add(3, std::numeric_limits<uint64_t>::max(), 0, "x", &complete), UpdateParts::Result::kResync);
  EXPECT_EQ(parts.Add(4, 2, 0, "abc", &complete), UpdateParts::Result::kResync);
}
TEST(GuestAccessibility, CoalescedSnapshotIncludesUnchangedFrameTrees) {
  TreeStream trees;
  const auto first = ui::AXTreeID::CreateNewAXTreeID();
  const auto second = ui::AXTreeID::CreateNewAXTreeID();
  ASSERT_TRUE(trees.Update(Initial(first))); ASSERT_TRUE(trees.Update(Initial(second)));
  Model model;
  ASSERT_EQ(model.Apply(Message(trees, 1, true)), Model::Result::kApplied);
  auto delta = Message(trees, 2, false);
  base::ListValue changed;
  for (const auto& tree : *delta.FindList("trees")) {
    if (tree.GetDict().FindString("id") && *tree.GetDict().FindString("id") == second.ToString()) {
      auto replacement = tree.GetDict().Clone();
      (*replacement.FindList("nodes"))[1].GetDict().Set("name", "changed");
      changed.Append(std::move(replacement));
    }
  }
  ASSERT_EQ(changed.size(), 1u);
  delta.Set("trees", std::move(changed));
  ASSERT_EQ(model.Apply(delta), Model::Result::kApplied);
  auto snapshot = model.Snapshot(delta);
  EXPECT_TRUE(*snapshot.FindBool("snapshot"));
  ASSERT_EQ(snapshot.FindList("trees")->size(), 2u);
  Model receiver;
  EXPECT_EQ(receiver.Apply(snapshot), Model::Result::kApplied);
}
TEST(GuestAccessibility, CrossFrameLinksCannotContradictNativeParentOwnership) {
  TreeStream trees;
  const auto id = ui::AXTreeID::CreateNewAXTreeID();
  ASSERT_TRUE(trees.Update(Initial(id)));
  auto message = Message(trees, 1, true);
  auto& root = (*(*message.FindList("trees"))[0].GetDict().FindList("nodes"))[0].GetDict();
  root.Set("child_tree", id.ToString());
  Model client;
  EXPECT_EQ(client.Apply(message), Model::Result::kResync);
  EXPECT_FALSE(client.ready());
  EXPECT_EQ(client.Apply(Message(trees, 2, true)), Model::Result::kApplied);
}
}  // namespace
}  // namespace alloy::guest::accessibility
