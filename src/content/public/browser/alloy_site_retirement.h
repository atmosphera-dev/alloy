// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_SITE_RETIREMENT_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_SITE_RETIREMENT_H_

#include "base/functional/callback.h"
#include "content/common/content_export.h"
#include "url/origin.h"

namespace content {
class BrowserContext;
class WebContents;
// Native first-party backends join the same R13 barrier AFTER old documents,
// workers and network writers have drained, while new admission stays closed.
// Completion reports native deletion failure rather than acknowledging it.
CONTENT_EXPORT void RetireAlloyFirstPartyStores(
    BrowserContext* context, const GURL& site,
    base::OnceCallback<void(bool)> done);

// UI-only, after durable key retirement. The browser supplies its native close
// action; content owns frame/worker enumeration and actual teardown completion.
// The predicate is engine-created record ownership, never renderer input.
// Preparation invokes no external callbacks. Prepare every native context of a
// shared settings owner before running any returned revocation/drain action.
CONTENT_EXPORT base::OnceClosure PrepareAlloySiteRetirement(
    BrowserContext* context, base::RepeatingCallback<bool(const url::Origin&)> owns,
    base::RepeatingCallback<void(WebContents*)> close_page,
    base::OnceCallback<void(bool, base::OnceClosure)> done);
CONTENT_EXPORT void RetireAlloySiteState(
    BrowserContext* context, base::RepeatingCallback<bool(const url::Origin&)> owns,
    base::RepeatingCallback<void(WebContents*)> close_page,
    base::OnceCallback<void(bool, base::OnceClosure)> done);
}  // namespace content
#endif
