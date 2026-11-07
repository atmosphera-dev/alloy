// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_LIST_P3A_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_LIST_P3A_H_
#include <string>
#include <vector>
class PrefService;
namespace brave_shields {
class FilterListCatalogEntry;
inline constexpr char kFilterListUsageHistogramName[] = "Brave.Shields.FilterLists.2";
inline constexpr char kAdBlockOnlyModeEnabledHistogramName[] = "Brave.Shields.AdBlockOnlyModeEnabled";
// Native compatibility surface only. No settings-usage observer, identifier
// collection, preference owner or reporting timer is retained.
class AdBlockListP3A {
 public:
  explicit AdBlockListP3A(PrefService*);
  ~AdBlockListP3A();
  AdBlockListP3A(const AdBlockListP3A&) = delete;
  AdBlockListP3A& operator=(const AdBlockListP3A&) = delete;
  void ReportFilterListUsage();
  void OnFilterListCatalogLoaded(const std::vector<FilterListCatalogEntry>&,
      const std::string&);
};
}  // namespace brave_shields
#endif
