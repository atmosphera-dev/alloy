// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_IDENTITY_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_IDENTITY_H_

#include "base/token.h"
#include "content/common/content_export.h"

namespace content {
class NavigationHandle;
class RenderFrameHost;

// Browser-owned, document-lifetime noise identity. No URL/profile lookup or
// retained seed table. Subframes use the current outermost document's identity.
CONTENT_EXPORT base::Token AlloyDocumentNoiseIdentity(RenderFrameHost* frame);
// Pending-navigation identity, available before the native DocumentToken is
// populated. It is handed to document data after native commit-time reset.
CONTENT_EXPORT base::Token AlloyDocumentNoiseIdentityForNavigation(
    NavigationHandle* navigation);
CONTENT_EXPORT void AlloyCommitDocumentNoiseIdentity(
    RenderFrameHost* frame, NavigationHandle* navigation);
}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_ALLOY_DOCUMENT_IDENTITY_H_
