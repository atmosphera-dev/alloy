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

///
/// EXPERIMENTAL: Creates an isolated memory-only request context. Site storage,
/// cookies and caches are retained only for this context's lifetime. Nothing is
/// read from the persistent parent or written to disk. Independent contexts do
/// not share state; CEF's sharing-context API preserves this context and mode.
/// Closing the last browser alone does not release a retained context
/// reference. Release all sharing references to end the session and retire its
/// state. Returns NULL for an invalid CEF initialization state. |handler| is
/// optional. Calling semantics match CefRequestContext::CreateContext.
///
/*--cef(added=experimental,optional_param=handler)--*/
CefRefPtr<CefRequestContext> AlloyCreateMemoryRequestContext(
    CefRefPtr<CefRequestContextHandler> handler);
#endif

#endif  // ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
