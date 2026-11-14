// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_VIEW_ADOPTION_H_
#define BRAVE_ALLOY_GUEST_SESSION_VIEW_ADOPTION_H_
#include <functional>
#include "cef/include/cef_base.h"
class AlloyGuestViewHandler;
namespace alloy::guest {
// Typed client adapter hook, not a public pointer bridge or a new authority.
// Only a view already offered by the authenticated native session can adopt a
// handler. All feature adapters use the same session-owned adoption path.
using ViewAdoption = std::function<bool(uint64_t, CefRefPtr<AlloyGuestViewHandler>)>;
}  // namespace alloy::guest
#endif
