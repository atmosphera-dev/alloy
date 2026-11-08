// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/components/misc_metrics/new_tab_metrics.h"
#include <utility>
#include "brave/components/misc_metrics/pref_names.h"
#include "components/prefs/pref_registry_simple.h"

namespace misc_metrics {
NewTabMetrics::NewTabMetrics(PrefService*) {}
NewTabMetrics::~NewTabMetrics() = default;
void NewTabMetrics::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(kMiscMetricsNTPWidgetUsageStorage);
}
void NewTabMetrics::Bind(mojo::PendingReceiver<brave_new_tab_page::mojom::NewTabMetrics> receiver) {
  receivers_.Add(this, std::move(receiver));
}
void NewTabMetrics::ReportNTPSearchDefaultEngine(std::optional<int64_t>) {}
void NewTabMetrics::ReportNTPSearchUsage(int64_t) {}
}  // namespace misc_metrics
