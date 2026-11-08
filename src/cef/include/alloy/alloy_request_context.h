// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// This header follows CEF's translator conventions. C wrappers are generated.
#ifndef ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
#define ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
#pragma once

#include "include/cef_request_context.h"
#include "include/alloy/alloy_profile.h"

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

///
/// EXPERIMENTAL: Completion of encrypted request-context creation on browser UI.
/// Only OK supplies a context. Cancellation never selects another profile or a
/// replacement unlock generation. The callback may reenter or release owners.
///
/*--cef(source=client,added=experimental)--*/
class AlloyRequestContextCallback : public virtual CefBaseRefCounted {
 public:
  ///
  /// Complete creation with |result| and the admitted |request_context|, or NULL.
  ///
  /*--cef(optional_param=request_context)--*/
  virtual void OnComplete(alloy_site_settings_result_t result,
                         CefRefPtr<CefRequestContext> request_context) = 0;
};

///
/// EXPERIMENTAL: Queue an encrypted context for the explicit unlocked |profile|.
/// Call on browser UI after CefInitialize. The profile's existing asynchronous
/// settings projection supplies native inputs before Profile construction. The
/// native context retains that original owner until teardown; locking retires
/// its browsers and cancels access. CEF sharing retains the same native owner.
/// Returns true if accepted; |callback| reports creation asynchronously on UI.
/// A locked profile reports CANCELLED with NULL, never an OTR/global fallback.
/// Returns false for invalid initialization/thread or missing required arguments.
/// |handler| is optional.
///
/*--cef(added=experimental,optional_param=handler)--*/
bool AlloyCreateEncryptedRequestContext(
    CefRefPtr<AlloyProfile> profile,
    CefRefPtr<CefRequestContextHandler> handler,
    CefRefPtr<AlloyRequestContextCallback> callback);

///
/// EXPERIMENTAL: Read the considered-first-party list for |outer_url| in the
/// explicit original |context|. Browser UI only, after context initialization.
/// True means queued; |callback| receives copied canonical domains on UI.
/// Memory contexts use their own volatile settings; encrypted contexts use the
/// same record-backed owner as AlloyProfile. Stateless, missing, locked and
/// retired contexts report CANCELLED with no entries, without parent fallback.
/// Wrong-thread, invalid initialization or missing-callback calls return false.
///
/*--cef(added=experimental,optional_param=context)--*/
bool AlloyGetFirstPartyEmbeds(CefRefPtr<CefRequestContext> context,
                             const CefString& outer_url,
                             CefRefPtr<AlloySiteSettingsCallback> callback);

///
/// EXPERIMENTAL: Atomically replace the outer site's list in the original
/// |context| using absolute URLs in |embedded|. Empty clears it; invalid input
/// preserves the prior list. Thread and callback semantics match the getter.
/// Callback OK means durable publication for encrypted profiles, or completed
/// memory-only publication for guest contexts. Setters return no entries.
///
/*--cef(added=experimental,optional_param=context)--*/
bool AlloySetFirstPartyEmbeds(CefRefPtr<CefRequestContext> context,
                             const CefString& outer_url,
                             const std::vector<CefString>& embedded,
                             CefRefPtr<AlloySiteSettingsCallback> callback);

///
/// EXPERIMENTAL: Read the original context's native AdBlock custom filters.
/// Browser UI only, after context initialization. Returns false for stateless,
/// missing, locked, closing or unavailable contexts and clears |filters|.
/// True with empty |filters| is an admitted empty list. Memory contexts retain
/// filters only in RAM; encrypted contexts use their original encrypted owner.
/// No global or parent preference fallback is selected. The caller owns the
/// returned plaintext string and its lifetime.
///
/*--cef(added=experimental,optional_param=context)--*/
bool AlloyGetAdBlockCustomFilters(CefRefPtr<CefRequestContext> context,
                                CefString& filters);

///
/// EXPERIMENTAL: Replace the original context's native AdBlock custom filters.
/// An empty string clears the complete list.
/// Thread, context and refusal rules match AlloyGetAdBlockCustomFilters. True
/// means the native mutation was admitted, not that encrypted I/O or the
/// asynchronous engine rebuild has completed. Profile lock completion remains
/// the durable flush/native retirement fence. The caller owns input plaintext.
///
/*--cef(added=experimental,optional_param=context,optional_param=filters)--*/
bool AlloySetAdBlockCustomFilters(CefRefPtr<CefRequestContext> context,
                                const CefString& filters);

///
/// EXPERIMENTAL: Selects the embedded-state lifetime for an explicit initialized
/// request context. Must be called on browser UI before the first network
/// document is admitted. |scope| is "per-outer-site" (the default, including a
/// 30-second keepalive after its last tab closes) or "per-tab" (tab + outer site,
/// immediate retirement on tab close or moving to a different outer site).
/// Back after a per-tab scope ends starts fresh. Sharing contexts share this
/// immutable choice. Returns false for a null/uninitialized context, wrong
/// thread, invalid option, retired context or a choice made after admission.
/// Stateless contexts still refuse all storage in either scope.
///
/*--cef(added=experimental)--*/
bool AlloySetEmbeddedStateScope(CefRefPtr<CefRequestContext> context,
                                const CefString& scope);
#endif

#endif  // ALLOY_INCLUDE_ALLOY_REQUEST_CONTEXT_H_
