// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_SUBSCRIPTION_FETCH_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_SUBSCRIPTION_FETCH_H_

#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace alloy::site_records {
// The browser owner supplies its own loader factory and original-generation
// admission. No system factory, download history or plaintext staging file.
// The returned cancellation closure owns each request; run it before the owner
// acknowledges teardown. All operations and callbacks use the owning sequence.
ProfileFilterPreferences::SubscriptionFetch MakeProfileSubscriptionFetch(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    base::RepeatingCallback<bool()> admitted);
}  // namespace alloy::site_records
#endif
