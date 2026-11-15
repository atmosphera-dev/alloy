// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_ACCESSIBILITY_AX_ACTIONS_H_
#define BRAVE_ALLOY_GUEST_ACCESSIBILITY_AX_ACTIONS_H_
#include <optional>
#include "base/values.h"
#include "ui/accessibility/ax_action_data.h"
namespace alloy::guest::accessibility {
bool IsActionKind(const std::string& kind);
// The portable action grammar has no script, native pointer or arbitrary
// Chromium action. Both SDK admission and native dispatch use this validator.
std::optional<ui::AXActionData> ParseAction(const base::DictValue& details);
}  // namespace alloy::guest::accessibility
#endif
