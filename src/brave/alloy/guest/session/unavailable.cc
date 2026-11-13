// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cef/include/alloy/alloy_guest_session.h"
#include "cef/libcef/common/api_version_util.h"

// Retain the translated API on platforms without a backend; no fallback to an
// in-process browser or a falsely protected guest is selected.
CefRefPtr<AlloyGuestSettings> AlloyCreateGuestSettings(
    const CefString&, const CefString&, const CefString&, bool, const CefString&) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  return nullptr;
}
CefRefPtr<AlloyGuestSession> AlloyStartGuestSession(
    CefRefPtr<AlloyGuestSettings>, CefRefPtr<AlloyGuestSessionHandler>) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  return nullptr;
}
int AlloyRunGuestHost(const CefMainArgs&) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  return 125;
}
