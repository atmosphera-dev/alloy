// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_CLIPBOARD_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_CLIPBOARD_H_
#include <functional>
#include <optional>
#include "base/unguessable_token.h"
#include "cef/include/cef_frame.h"
namespace alloy::guest::integration {
bool IsEditCommand(int command);
// Uses CEF's real native command lane with captured document admission and the
// same native UI-interaction bookkeeping as Chromium's own editing commands.
void DispatchEditCaptured(CefRefPtr<CefFrame> original, int command,
    std::function<bool()> admitted, std::function<void(bool)> completion,
    std::optional<base::UnguessableToken> edit_context = std::nullopt);
}  // namespace alloy::guest::integration
#endif
