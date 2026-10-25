// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/public/browser/alloy_document_state.h"

#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/navigation_or_document_handle.h"
#include "content/browser/alloy_network_generation.h"
#include "content/public/browser/storage_partition.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/public/browser/browser_context.h"

namespace content {
bool AlloyDocumentStateAdmitted(RenderFrameHost* frame) {
  return frame && !frame->GetBrowserContext()->ShutdownStarted() &&
         static_cast<RenderFrameHostImpl*>(frame)->IsAlloyStateAdmissionOpen();
}

base::WeakPtr<base::SupportsUserData> AlloyDocumentStateOwner(RenderFrameHost* frame) {
  return AlloyDocumentStateAdmitted(frame)
      ? static_cast<RenderFrameHostImpl*>(frame)->GetNavigationOrDocumentHandle()->GetAlloyStateOwner()
      : base::WeakPtr<base::SupportsUserData>();
}

base::WeakPtr<base::SupportsUserData> AlloyNavigationStateOwner(NavigationHandle* navigation) {
  return navigation ? static_cast<NavigationRequest*>(navigation)->GetAlloyStateOwner()
                    : base::WeakPtr<base::SupportsUserData>();
}

base::WeakPtr<base::SupportsUserData> AlloyPendingNavigationStateOwner(FrameTreeNodeId id) {
  auto* node = FrameTreeNode::GloballyFindByID(id);
  return node ? AlloyNavigationStateOwner(node->navigation_request())
              : base::WeakPtr<base::SupportsUserData>();
}

base::SupportsUserData* AlloyStateOwnerForContext(
    const base::WeakPtr<base::SupportsUserData>& owner, BrowserContext* context) {
  if (!owner || !context) return nullptr;
  auto* handle = static_cast<NavigationOrDocumentHandle*>(owner.get());
  return handle->GetAlloyStateBrowserContext() == context ? owner.get() : nullptr;
}

network::mojom::NetworkContext* AlloyNetworkContextForStateOwner(
    const base::WeakPtr<base::SupportsUserData>& owner, BrowserContext* context) {
  if (!AlloyStateOwnerForContext(owner, context)) return nullptr;
  auto* handle = static_cast<NavigationOrDocumentHandle*>(owner.get());
  if (auto* frame = handle->GetDocument()) {
    return GetAlloyNetworkContextForDocument(static_cast<RenderFrameHostImpl*>(frame));
  }
  if (auto* navigation = handle->GetNavigationRequest()) {
    auto generation = navigation->GetAlloyNetworkGeneration();
    return generation ? generation->GetNetworkContext()
                      : navigation->frame_tree_node()->current_frame_host()->GetStoragePartition()->GetNetworkContext();
  }
  return nullptr;
}
}  // namespace content
