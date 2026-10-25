// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CHROME_BROWSER_UI_PAGE_INFO_ALLOY_DOCUMENT_PERMISSION_QUERY_H_
#define CHROME_BROWSER_UI_PAGE_INFO_ALLOY_DOCUMENT_PERMISSION_QUERY_H_

#include <optional>

#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/permission_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "url/origin.h"

namespace alloy::site_records {

// Both native PageInfo delegates query the captured document, including native
// policy/security checks, rather than infer an owner from the visible URL.
inline content::PermissionResult PageInfoDocumentPermissionResult(
    content::WeakDocumentPtr document,
    blink::PermissionType permission,
    const url::Origin& origin,
    const std::optional<url::Origin>& requesting_origin = std::nullopt) {
  auto* frame = document.AsRenderFrameHostIfValid();
  if (!content::AlloyDocumentStateAdmitted(frame) ||
      origin != frame->GetLastCommittedOrigin() ||
      (requesting_origin &&
       *requesting_origin != frame->GetLastCommittedOrigin())) {
    // Cross-origin storage access cannot be granted in stateless mode. Retain
    // native current-document security checks instead of a context-free lookup.
    return content::PermissionResult(blink::mojom::PermissionStatus::DENIED);
  }
  return frame->GetBrowserContext()
      ->GetPermissionController()
      ->GetPermissionResultForCurrentDocument(
          content::PermissionDescriptorUtil::
              CreatePermissionDescriptorForPermissionType(permission),
          frame);
}

}  // namespace alloy::site_records
#endif  // CHROME_BROWSER_UI_PAGE_INFO_ALLOY_DOCUMENT_PERMISSION_QUERY_H_
