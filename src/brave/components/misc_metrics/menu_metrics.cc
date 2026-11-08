/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "brave/components/misc_metrics/menu_metrics.h"
#include "brave/components/misc_metrics/pref_names.h"
#include "components/prefs/pref_registry_simple.h"

namespace misc_metrics {
MenuMetrics::MenuMetrics(PrefService*) {}
MenuMetrics::~MenuMetrics() = default;
void MenuMetrics::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kMiscMetricsMenuGroupActionCounts);
  registry->RegisterListPref(kMiscMetricsMenuShownStorage);
  registry->RegisterListPref(kMiscMetricsMenuDismissStorage);
}
void MenuMetrics::RecordMenuGroupAction(MenuGroup) {}
void MenuMetrics::RecordMenuShown() {}
void MenuMetrics::RecordMenuDismiss() {}
}  // namespace misc_metrics
