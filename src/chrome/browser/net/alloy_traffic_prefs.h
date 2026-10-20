// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CHROME_BROWSER_NET_ALLOY_TRAFFIC_PREFS_H_
#define CHROME_BROWSER_NET_ALLOY_TRAFFIC_PREFS_H_

#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace alloy {
inline constexpr char kFaviconsEnabled[] = "alloy.favicons_enabled";
inline constexpr char kOpenSearchEnabled[] = "alloy.open_search_enabled";
inline constexpr char kDnsPrefetchEnabled[] = "alloy.dns_prefetch_enabled";
inline constexpr char kPreconnectEnabled[] = "alloy.preconnect_enabled";
inline constexpr const char* kTrafficPrefs[] = {
    kFaviconsEnabled, kOpenSearchEnabled, kDnsPrefetchEnabled,
    kPreconnectEnabled};

inline void RegisterTrafficPrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kFaviconsEnabled, true);
  registry->RegisterBooleanPref(kOpenSearchEnabled, false);
  registry->RegisterBooleanPref(kDnsPrefetchEnabled, false);
  registry->RegisterBooleanPref(kPreconnectEnabled, false);
}
}  // namespace alloy
#endif
