// Copyright (c) 2019 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/brave_shields_p3a.h"

#include "components/prefs/pref_registry_simple.h"

namespace brave_shields {

// Alloy keeps functional settings in their native owner, without a second
// usage/domain-count/manual-action projection. The native compatibility entry
// points do not inspect sites, mutate prefs, schedule reporting or keep metrics.
void MaybeRecordShieldsUsageP3A(ShieldsIconUsage, PrefService*) {}
void RecordShieldsAdsSetting(ControlType) {}
void RecordShieldsFingerprintSetting(ControlType) {}
void RecordShieldsDomainSettingCounts(PrefService*, bool, ControlType) {}
void RecordShieldsDomainSettingCountsWithChange(PrefService*, bool, ControlType,
                                                ControlType*, ControlType) {}
void RecordForgetFirstPartySetting(HostContentSettingsMap*) {}
void ReportAutoShredSettingsP3A(const HostContentSettingsMap&) {}
void RecordManualShredP3A(PrefService&) {}
void RecordHTTPSUpgradeSettingP3A(HostContentSettingsMap*) {}
void MaybeRecordInitialShieldsSettings(PrefService*, PrefService*,
    HostContentSettingsMap*, ControlType, ControlType) {}

// Preserve zero/default schema compatibility for native readers. Registration
// populates the default store, never retained user state; no producer above
// writes these paths and no old profile state is reopened for reporting.
void RegisterShieldsP3ALocalPrefs(PrefRegistrySimple* registry) {
  registry->RegisterIntegerPref(kUsagePrefName, -1);
  registry->RegisterBooleanPref(kManualShredTriggeredPrefName, false);
}
void RegisterShieldsP3AProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterIntegerPref(kFirstReportedRevisionPrefName, 0);
  registry->RegisterIntegerPref(kAdsStrictCountPrefName, 0);
  registry->RegisterIntegerPref(kAdsStandardCountPrefName, 0);
  registry->RegisterIntegerPref(kAdsAllowCountPrefName, 0);
  registry->RegisterIntegerPref(kFPStrictCountPrefName, 0);
  registry->RegisterIntegerPref(kFPStandardCountPrefName, 0);
  registry->RegisterIntegerPref(kFPAllowCountPrefName, 0);
}
void RegisterShieldsP3AProfilePrefsForMigration(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kFirstReportedPrefName, false);
}
void MigrateObsoleteProfilePrefs(PrefService*) {}

}  // namespace brave_shields
