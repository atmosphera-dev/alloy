// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_ALLOY_EMBEDDED_STATE_H_
#define CONTENT_BROWSER_ALLOY_EMBEDDED_STATE_H_

#include <memory>
#include <optional>

#include "brave/alloy/site_records/embedded_state.h"
#include "content/browser/renderer_host/code_cache_host_impl.h"
#include "content/common/content_export.h"
#include "content/public/browser/alloy_document_state.h"

namespace content {

class BrowserContext;
class NavigationRequest;
class RenderFrameHost;
class RenderFrameHostImpl;
class WebContents;
class StoragePartitionImpl;

// The caller supplies original browser-captured scope authority. A closed
// selection returns no receiver set and must never fall through to real data.
CONTENT_EXPORT CodeCacheHostImpl::ReceiverSet*
GetAlloyEmbeddedCodeCacheReceivers(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner);

// UI-only, explicit native context, before its first network-document
// admission. Context sharing aliases this owner. Once admitted, configuration
// is immutable even after the last tab closes; changing it cannot silently
// discard a jar.
CONTENT_EXPORT bool ConfigureAlloyEmbeddedStateScope(
    BrowserContext& context,
    alloy::site_records::EmbeddedStateScope scope);
CONTENT_EXPORT bool ConfigureAlloyEmbeddedFirstPartyPolicy(
    BrowserContext& context,
    std::unique_ptr<alloy::site_records::EmbeddedFirstPartyPolicy> policy);
CONTENT_EXPORT void InitializeAlloyEmbeddedTab(WebContents* contents);
CONTENT_EXPORT void ShutdownAlloyEmbeddedState(BrowserContext& context);
// Engine deletion/decay hook; does not instantiate an unused registry. Native
// callers supply the trusted site principal, never a renderer-provided nonce.
CONTENT_EXPORT void RevokeAlloyEmbeddedSite(BrowserContext& context,
                                            const GURL& document_url,
                                           const url::Origin& document_origin);
CONTENT_EXPORT base::OnceClosure BeginAlloyFirstPartyRetirement(
    BrowserContext& context, const GURL& site);

// These resolve browser-owned principals and the canonical navigation/document
// owner before binding any native store. The selection travels with content's
// existing NavigationOrDocumentHandle. A retained old document cannot acquire
// the replacement document's scope. No request header or renderer nonce selects
// a backend, and this module never opens a record or consults a parent profile.
CONTENT_EXPORT AlloyEmbeddedResolution
GetAlloyEmbeddedStateForNavigation(NavigationRequest* navigation,
                                   const url::Origin& origin);
// Only an explicitly supplied navigation selects navigation authority. Omit it
// for current-document work, including a same-document commit while a different
// cross-document navigation is pending on the frame-tree node.
CONTENT_EXPORT AlloyEmbeddedResolution
GetAlloyEmbeddedStateForCandidate(RenderFrameHostImpl* frame,
                                  const url::Origin& origin,
                                  NavigationRequest* navigation = nullptr);
CONTENT_EXPORT std::optional<base::UnguessableToken>
GetAlloyEmbeddedNonceForCandidate(RenderFrameHostImpl* frame,
                                  const url::Origin& origin,
                                  NavigationRequest* navigation = nullptr);
CONTENT_EXPORT std::optional<base::UnguessableToken>
GetAlloyEmbeddedNonceForNavigation(NavigationRequest* navigation,
                                   const url::Origin& origin);

}  // namespace content

#endif  // CONTENT_BROWSER_ALLOY_EMBEDDED_STATE_H_
