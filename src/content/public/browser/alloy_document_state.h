// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_

#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "brave/alloy/site_records/embedded_state.h"
#include "content/common/content_export.h"
#include "content/public/browser/frame_tree_node_id.h"

namespace network::mojom {
class NetworkContext;
}

namespace content {
class RenderFrameHost;
class NavigationHandle;
class BrowserContext;
// R12: the final committed primary top-level document, never an embed/worker,
// same-document update, failed navigation or intermediate redirect hop.
CONTENT_EXPORT void RecordAlloyCommittedSiteVisit(NavigationHandle* navigation);

using AlloyEmbeddedResolution =
    alloy::site_records::EmbeddedStateRegistry::Resolution;
// The same canonical captured selection used by native storage. This is not a
// second classifier or authority recovered from a renderer nonce.
CONTENT_EXPORT AlloyEmbeddedResolution
GetAlloyEmbeddedStateForDocument(RenderFrameHost* frame);

// Admission belongs to the canonical document, not the lifetime of a weak RFH
// or an outstanding callback. This query never creates document user data.
CONTENT_EXPORT bool AlloyDocumentStateAdmitted(RenderFrameHost* frame);
// Key retirement can precede native frame destruction. Close the same canonical
// owner used by all native grants/callbacks before initiating that destruction.
CONTENT_EXPORT void RetireAlloyDocumentState(RenderFrameHost* frame);
// These are weak views of content's existing NavigationOrDocumentHandle. They
// transfer with native commit and are revoked before teardown callbacks.
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData> AlloyDocumentStateOwner(
    RenderFrameHost* frame);
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData> AlloyNavigationStateOwner(
    NavigationHandle* navigation);
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData>
AlloyPendingNavigationStateOwner(FrameTreeNodeId node);
CONTENT_EXPORT base::SupportsUserData* AlloyStateOwnerForContext(
    const base::WeakPtr<base::SupportsUserData>& owner,
    BrowserContext* context);
CONTENT_EXPORT AlloyEmbeddedResolution AlloyEmbeddedStateForStateOwner(
    const base::WeakPtr<base::SupportsUserData>& owner,
    BrowserContext* context);
// The original captured list exception, never a new policy lookup. Cookie
// consumers must confine this authority to this exact origin on every hop.
CONTENT_EXPORT std::optional<url::Origin> AlloyConsideredFirstPartyCookieOrigin(
    const base::WeakPtr<base::SupportsUserData>& owner,
    BrowserContext* context);
CONTENT_EXPORT network::mojom::NetworkContext* AlloyNetworkContextForStateOwner(
    const base::WeakPtr<base::SupportsUserData>& owner,
    BrowserContext* context);
}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_
