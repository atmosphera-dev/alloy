// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_INTEGRATION_CLIENT_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_INTEGRATION_CLIENT_H_
#include "brave/alloy/guest/session/extensions.h"
#include "cef/include/alloy/alloy_guest_integration.h"
namespace alloy::guest::integration {
CefRefPtr<AlloyGuestIntegration> MakeClient(ExtensionEndpoint endpoint);
}  // namespace alloy::guest::integration
#endif
