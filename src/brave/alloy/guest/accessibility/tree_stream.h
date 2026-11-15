// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_ACCESSIBILITY_TREE_STREAM_H_
#define BRAVE_ALLOY_GUEST_ACCESSIBILITY_TREE_STREAM_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include "base/values.h"
#include "ui/accessibility/ax_tree.h"
#include "ui/accessibility/ax_tree_observer.h"
#include "ui/accessibility/ax_updates_and_events.h"
#include "ui/accessibility/ax_location_and_scroll_updates.h"

namespace alloy::guest::accessibility {
// AXTreeID::FromString CHECKs malformed nonempty input on this pin. Public
// action input must use this non-fatal token parser before native dispatch.
std::optional<ui::AXTreeID> ParseTreeId(const std::string& text);
// Native trees only. No platform manager, AT-SPI object or retained event
// history. Node incarnations survive data changes but never deletion/reuse.
class TreeStream {
 public:
  TreeStream();
  ~TreeStream();
  bool Update(const ui::AXUpdatesAndEvents& update);
  bool Location(const ui::AXTreeID& id, const ui::AXLocationAndScrollUpdates& update);
  void Remove(const ui::AXTreeID& id);
  void Prune(const std::function<bool(const ui::AXTreeID&)>& owns);
  void Clear();
  const ui::AXTree* Find(const ui::AXTreeID& id) const;
  const ui::AXNode* Resolve(const std::string& id, uint64_t tree_generation,
                            int node, uint64_t node_generation) const;
  // Bounds are root-frame-relative DIP, including scroll and frame transforms.
  std::optional<gfx::RectF> Bounds(const ui::AXTreeID& id, int node) const;
  base::DictValue Snapshot(double pixel_scale = 1, bool bounds_ready = true) const;
  // A changed tree is a replacement subtree in the portable delta. Other frame
  // trees remain intact. This avoids exposing Chromium's mutation algorithm.
  base::DictValue Delta(const ui::AXTreeID& id, double pixel_scale = 1, bool bounds_ready = true) const;
 private:
  struct Tree final : ui::AXTreeObserver {
    explicit Tree(uint64_t generation);
    ~Tree() override;
    void OnNodeWillBeDeleted(ui::AXTree*, ui::AXNode* node) override;
    void OnNodeCreated(ui::AXTree*, ui::AXNode* node) override;
    ui::AXTree value;
    const uint64_t generation;
    uint64_t next_node = 1;
    std::map<int, uint64_t> nodes;
  };
  base::DictValue Serialize(const ui::AXTreeID& id, const Tree& tree,
                             double pixel_scale, bool bounds_ready) const;
  std::map<ui::AXTreeID, std::unique_ptr<Tree>> trees_;
  uint64_t next_tree_ = 1;
};
}  // namespace alloy::guest::accessibility
#endif
