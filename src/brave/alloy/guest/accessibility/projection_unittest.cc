// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/projection.h"
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::guest::accessibility {
namespace {
base::DictValue Update(int sequence, bool snapshot, const std::string& name) {
  base::DictValue node, tree, update;
  node.Set("id", 1); node.Set("generation", "1"); node.Set("role", "button"); node.Set("name", name);
  node.Set("value", ""); node.Set("description", ""); node.Set("child_tree", ""); node.Set("ignored", false);
  node.Set("bounds_available", true); node.Set("children", base::ListValue());
  node.Set("states", base::ListValue()); node.Set("actions", base::ListValue()); node.Set("attributes", base::DictValue());
  base::ListValue bounds;
  bounds.Append(0); bounds.Append(0); bounds.Append(100); bounds.Append(40); node.Set("bounds", std::move(bounds));
  base::ListValue nodes; nodes.Append(std::move(node));
  tree.Set("id", "tree"); tree.Set("generation", "1"); tree.Set("parent", ""); tree.Set("focused_tree", "tree");
  tree.Set("root", 1); tree.Set("focus", 1); tree.Set("loaded", true); tree.Set("nodes", std::move(nodes));
  base::ListValue trees; trees.Append(std::move(tree));
  update.Set("sequence", base::NumberToString(sequence)); update.Set("snapshot", snapshot);
  update.Set("document", "1"); update.Set("geometry", "2"); update.Set("capture_us", "3");
  update.Set("root_tree", "tree"); update.Set("view_focused", true); update.Set("scale", 1.5);
  update.Set("trees", std::move(trees)); update.Set("events", base::ListValue());
  return update;
}
TEST(GuestAccessibilityProjection, SlowConsumerGetsLatestCompleteProjectionNotHistory) {
  Projection projection;
  ASSERT_EQ(projection.Receive(Update(1, true, "one")), Projection::Result::kQueued);
  auto in_flight = projection.Take(); ASSERT_TRUE(in_flight);
  EXPECT_EQ(*in_flight->FindString("sequence"), "1");
  EXPECT_EQ(projection.Receive(Update(2, false, "two")), Projection::Result::kQueued);
  EXPECT_EQ(projection.Receive(Update(3, false, "three")), Projection::Result::kQueued);
  EXPECT_EQ(projection.Receive(Update(4, false, "four")), Projection::Result::kQueued);
  auto latest = projection.Take(); ASSERT_TRUE(latest);
  EXPECT_EQ(*latest->FindString("sequence"), "4"); EXPECT_TRUE(*latest->FindBool("snapshot"));
  EXPECT_EQ(*latest->FindString("coalesced_updates"), "2");
  EXPECT_EQ(*(*(*latest->FindList("trees"))[0].GetDict().FindList("nodes"))[0].GetDict().FindString("name"), "four");
  EXPECT_FALSE(projection.Take()); EXPECT_FALSE(projection.pending());
}
TEST(GuestAccessibilityProjection, SequenceGapRequestsResyncAndReceivesCurrentSnapshot) {
  Projection projection;
  ASSERT_EQ(projection.Receive(Update(1, true, "one")), Projection::Result::kQueued);
  ASSERT_TRUE(projection.Take());
  EXPECT_EQ(projection.Receive(Update(3, false, "gap")), Projection::Result::kResync);
  EXPECT_FALSE(projection.ready()); EXPECT_FALSE(projection.Take());
  EXPECT_EQ(projection.Receive(Update(4, false, "still-gap")), Projection::Result::kIgnored);
  EXPECT_EQ(projection.Receive(Update(5, true, "recovered")), Projection::Result::kQueued);
  EXPECT_TRUE(projection.ready()); auto latest = projection.Take(); ASSERT_TRUE(latest);
  EXPECT_EQ(*latest->FindString("sequence"), "5");
}
TEST(GuestAccessibilityProjection, InvalidResyncSnapshotReportsUnavailableInsteadOfStayingStale) {
  Projection projection;
  ASSERT_EQ(projection.Receive(Update(1, true, "one")), Projection::Result::kQueued);
  ASSERT_TRUE(projection.Take());
  ASSERT_EQ(projection.Receive(Update(3, false, "gap")), Projection::Result::kResync);
  auto invalid = Update(4, true, "invalid");
  (*(*invalid.FindList("trees"))[0].GetDict().FindList("nodes"))[0].GetDict().Remove("children");
  EXPECT_EQ(projection.Receive(invalid), Projection::Result::kUnavailable);
  EXPECT_TRUE(projection.TakeUnavailable()); EXPECT_FALSE(projection.TakeUnavailable());
  EXPECT_FALSE(projection.ready()); EXPECT_FALSE(projection.Take());
}
TEST(GuestAccessibilityProjection, OwnerRetirementClearsPendingStateAndResync) {
  Projection projection;
  ASSERT_EQ(projection.Receive(Update(1, true, "one")), Projection::Result::kQueued);
  projection.Clear(); EXPECT_FALSE(projection.pending()); EXPECT_FALSE(projection.ready());
  EXPECT_TRUE(projection.RequestResync()); EXPECT_FALSE(projection.RequestResync());
  projection.Clear(); EXPECT_TRUE(projection.RequestResync());
}
}
}
