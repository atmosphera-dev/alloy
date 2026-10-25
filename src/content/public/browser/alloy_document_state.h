// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_

#include "content/common/content_export.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "content/public/browser/frame_tree_node_id.h"

namespace network::mojom { class NetworkContext; }

namespace content {
class RenderFrameHost;
class NavigationHandle;
class BrowserContext;

// Admission belongs to the canonical document, not the lifetime of a weak RFH
// or an outstanding callback. This query never creates document user data.
CONTENT_EXPORT bool AlloyDocumentStateAdmitted(RenderFrameHost* frame);
// These are weak views of content's existing NavigationOrDocumentHandle. They
// transfer with native commit and are revoked before teardown callbacks.
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData> AlloyDocumentStateOwner(
    RenderFrameHost* frame);
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData> AlloyNavigationStateOwner(
    NavigationHandle* navigation);
CONTENT_EXPORT base::WeakPtr<base::SupportsUserData> AlloyPendingNavigationStateOwner(
    FrameTreeNodeId node);
CONTENT_EXPORT base::SupportsUserData* AlloyStateOwnerForContext(
    const base::WeakPtr<base::SupportsUserData>& owner, BrowserContext* context);
CONTENT_EXPORT network::mojom::NetworkContext* AlloyNetworkContextForStateOwner(
    const base::WeakPtr<base::SupportsUserData>& owner, BrowserContext* context);
}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_STATE_H_
