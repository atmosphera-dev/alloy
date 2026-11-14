// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_STREAM_CLIENT_H_
#define BRAVE_ALLOY_GUEST_STREAM_STREAM_CLIENT_H_
#include "brave/alloy/guest/session/extensions.h"
#include "brave/alloy/guest/session/view_adoption.h"
#include "cef/include/alloy/alloy_guest_stream.h"
namespace alloy::guest::stream {
CefRefPtr<AlloyGuestStream> MakeClient(ExtensionEndpoint endpoint, ViewAdoption adopt);
}  // namespace alloy::guest::stream
#endif
