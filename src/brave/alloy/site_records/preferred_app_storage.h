// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_STORAGE_H_
#define BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_STORAGE_H_

#include "brave/alloy/site_records/site_pref_store.h"
#include "components/services/app_service/public/cpp/preferred_apps_impl.h"

namespace alloy::site_records {
// Selected by the browser composition root; no default-file fallback exists.
// All operations and notifications stay on the native PrefStore's sequence.
std::shared_ptr<apps::PreferredAppsImpl::Storage> MakePreferredAppStorage(
    scoped_refptr<SitePrefStore> store);
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_STORAGE_H_
