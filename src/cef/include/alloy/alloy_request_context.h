// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// This header follows CEF's translator conventions. C wrappers are generated.
#ifndef ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
#define ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
#pragma once

#include "include/cef_request_context.h"

#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Creates an isolated stateless request context. Storage is
/// refused at admission, including cookies and in-memory site storage/cache.
/// Each tab retains only its own URL-only Back/Forward stack until it closes.
/// |network_state| selects context-lifetime working network state (KEEP) or
/// fresh managed network state for each new top-level document (CUT).
/// Same-document fragment and history API navigations do not rotate it.
/// Reload and Back/Forward load fresh documents and do rotate it. OS resolver
/// caches and remote server observations are outside this managed-state scope.
/// The mode is immutable and is preserved by CEF's sharing-context API.
/// Returns NULL for an invalid option or invalid CEF initialization state.
/// |handler| is optional. Calling semantics match CefRequestContext::CreateContext.
///
/*--cef(added=experimental,optional_param=handler)--*/
CefRefPtr<CefRequestContext> AlloyCreateStatelessRequestContext(
    alloy_network_state_t network_state,
    CefRefPtr<CefRequestContextHandler> handler);
#endif

#endif  // ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
