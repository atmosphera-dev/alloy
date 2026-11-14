// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/ax_actions.h"
#include "brave/alloy/guest/accessibility/tree_stream.h"
#include <string_view>
#include <utility>
#include "base/strings/utf_string_conversions.h"
#include "base/strings/string_util.h"
#include "ui/accessibility/ax_tree_id.h"
namespace alloy::guest::accessibility {
namespace {
constexpr std::pair<std::string_view, ax::mojom::Action> kKinds[] = {
    {"default", ax::mojom::Action::kDoDefault}, {"focus", ax::mojom::Action::kFocus},
    {"blur", ax::mojom::Action::kBlur}, {"expand", ax::mojom::Action::kExpand},
    {"collapse", ax::mojom::Action::kCollapse}, {"increment", ax::mojom::Action::kIncrement},
    {"decrement", ax::mojom::Action::kDecrement}, {"scroll-visible", ax::mojom::Action::kScrollToMakeVisible},
    {"scroll-up", ax::mojom::Action::kScrollUp}, {"scroll-down", ax::mojom::Action::kScrollDown},
    {"scroll-left", ax::mojom::Action::kScrollLeft}, {"scroll-right", ax::mojom::Action::kScrollRight},
    {"scroll-forward", ax::mojom::Action::kScrollForward}, {"scroll-backward", ax::mojom::Action::kScrollBackward},
    {"set-value", ax::mojom::Action::kSetValue}, {"selection", ax::mojom::Action::kSetSelection},
};
std::optional<ax::mojom::Action> ActionForName(const std::string& name) {
  for (const auto& [kind, action] : kKinds) if (name == kind) return action;
  return std::nullopt;
}
}
bool IsActionKind(const std::string& kind) { return ActionForName(kind).has_value(); }
std::optional<ui::AXActionData> ParseAction(const base::DictValue& details) {
  const auto* tree = details.FindString("tree");
  const auto* kind = details.FindString("action");
  auto node = details.FindInt("node");
  if (!tree || !kind || !node || *node == ui::kInvalidAXNodeID) return std::nullopt;
  ui::AXActionData action;
  auto id = ParseTreeId(*tree);
  if (!id) return std::nullopt;
  action.target_tree_id = *id;
  action.target_node_id = *node;
  if (action.target_tree_id == ui::AXTreeIDUnknown()) return std::nullopt;
  auto operation = ActionForName(*kind);
  if (!operation) return std::nullopt;
  action.action = *operation;
  if (*kind == "set-value") {
    const auto* value = details.FindString("value");
    if (!value || !base::IsStringUTF8(*value)) return std::nullopt;
    action.action = ax::mojom::Action::kSetValue;
    action.value = *value;
  } else if (*kind == "selection") {
    auto anchor = details.FindInt("anchor");
    auto focus = details.FindInt("focus");
    if (!anchor || !focus || *anchor < 0 || *focus < 0) return std::nullopt;
    action.action = ax::mojom::Action::kSetSelection;
    action.anchor_node_id = action.focus_node_id = *node;
    action.anchor_offset = *anchor; action.focus_offset = *focus;
  }
  return action;
}
}  // namespace alloy::guest::accessibility
