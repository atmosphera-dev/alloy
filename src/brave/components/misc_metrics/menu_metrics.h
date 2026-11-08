/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BRAVE_COMPONENTS_MISC_METRICS_MENU_METRICS_H_
#define BRAVE_COMPONENTS_MISC_METRICS_MENU_METRICS_H_

class PrefRegistrySimple;
class PrefService;

namespace misc_metrics {
enum class MenuGroup { kTabWindow, kBraveFeatures, kBrowserViews };
inline constexpr char kFrequentMenuGroupHistogramName[] =
    "Brave.Toolbar.FrequentMenuGroup";
inline constexpr char kMenuDismissRateHistogramName[] = "Brave.Toolbar.MenuDismissRate";
inline constexpr char kMenuOpensHistogramName[] = "Brave.Toolbar.MenuOpens";

// Native menu callers retain an interface, never an activity collector.
class MenuMetrics {
 public:
  explicit MenuMetrics(PrefService* local_state);
  ~MenuMetrics();
  MenuMetrics(const MenuMetrics&) = delete;
  MenuMetrics& operator=(const MenuMetrics&) = delete;
  static void RegisterPrefs(PrefRegistrySimple* registry);
  void RecordMenuGroupAction(MenuGroup group);
  void RecordMenuShown();
  void RecordMenuDismiss();
};
}  // namespace misc_metrics
#endif
