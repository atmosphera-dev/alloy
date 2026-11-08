// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_SITE_PROFILE_CONFIGURATION_H_
#define BRAVE_ALLOY_SITE_RECORDS_SITE_PROFILE_CONFIGURATION_H_

#include <memory>
#include <string_view>

#include "brave/alloy/site_records/native_pref_view.h"
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "brave/alloy/site_records/site_pref_store.h"

namespace alloy::site_records {

// Site settings are one consumer of the common native profile lifetime. These
// bindings and origin/pref projections belong to 6a, not the shared foundation.
std::unique_ptr<NativeProfileConfiguration> MakeSiteProfileConfiguration(
    RecordCapability original, scoped_refptr<NativePrefView> view,
    scoped_refptr<SitePrefStore> settings, base::OnceClosure release_owner,
    base::OnceCallback<void(base::SupportsUserData&)> attach_consumer = {},
    base::RepeatingCallback<bool()> consumer_admitted = {});
scoped_refptr<SitePrefStore> GetSiteProfileSettings(
    const NativeProfileConfiguration& configuration);
scoped_refptr<PersistentPrefStore> SiteProfilePrefsForNamespace(
    const NativeProfileConfiguration& configuration, std::string_view prefix);
std::optional<RecordValueStoreBinding> SiteProfileManagedOrigin(
    const NativeProfileConfiguration& configuration, const url::Origin& origin);

}  // namespace alloy::site_records
#endif
