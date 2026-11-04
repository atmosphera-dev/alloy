// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_NETWORK_RETIREMENT_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_NETWORK_RETIREMENT_H_

#include "base/functional/callback.h"
#include "content/common/content_export.h"
#include "net/base/alloy_network_record_target.h"

namespace content {
class BrowserContext;

// UI-only internal coordinator. Reuses the existing shared/CUT context traversal.
// Prepare every participating BrowserContext before invoking any returned commit.
// Destroying a prepared commit still revokes its original native snapshot.
CONTENT_EXPORT void PrepareAlloyNetworkServiceRetirement(
    BrowserContext* context,
    const net::AlloyNetworkRecordTarget& target,
    base::OnceCallback<void(base::OnceClosure)> prepared,
    base::OnceCallback<void(bool)> drained);
}  // namespace content
#endif
