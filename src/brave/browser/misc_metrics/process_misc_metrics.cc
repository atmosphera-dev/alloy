/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "brave/browser/misc_metrics/process_misc_metrics.h"
#include "brave/browser/misc_metrics/captcha_metrics.h"
#include "brave/browser/misc_metrics/doh_metrics.h"
#include "brave/browser/misc_metrics/media_session_metrics_impl.h"
#include "brave/browser/misc_metrics/uptime_monitor_impl.h"
#include "brave/components/misc_metrics/menu_metrics.h"
#include "brave/components/misc_metrics/new_tab_metrics.h"
#if !BUILDFLAG(IS_ANDROID)
#include "brave/browser/misc_metrics/vertical_tab_metrics.h"
#include "brave/components/misc_metrics/split_view_metrics.h"
#else
#include "brave/components/misc_metrics/privacy_hub_metrics.h"
#include "brave/components/misc_metrics/tab_metrics.h"
#endif

namespace misc_metrics {
ProcessMiscMetrics::ProcessMiscMetrics(PrefService* local_state) {
#if !BUILDFLAG(IS_ANDROID)
  menu_metrics_ = std::make_unique<MenuMetrics>(local_state);
  new_tab_metrics_ = std::make_unique<NewTabMetrics>(local_state);
#endif
}
ProcessMiscMetrics::~ProcessMiscMetrics() = default;
Web3Metrics& ProcessMiscMetrics::web3_metrics() { return web3_metrics_; }
#if !BUILDFLAG(IS_ANDROID)
MenuMetrics* ProcessMiscMetrics::menu_metrics() { return menu_metrics_.get(); }
NewTabMetrics* ProcessMiscMetrics::new_tab_metrics() { return new_tab_metrics_.get(); }
VerticalTabMetrics* ProcessMiscMetrics::vertical_tab_metrics() { return nullptr; }
SplitViewMetrics* ProcessMiscMetrics::split_view_metrics() { return nullptr; }
#else
PrivacyHubMetrics* ProcessMiscMetrics::privacy_hub_metrics() { return nullptr; }
TabMetrics* ProcessMiscMetrics::tab_metrics() { return nullptr; }
#endif
DefaultBrowserMonitor* ProcessMiscMetrics::default_browser_monitor() { return nullptr; }
UptimeMonitorImpl* ProcessMiscMetrics::uptime_monitor() { return nullptr; }
MediaSessionMetricsImpl* ProcessMiscMetrics::media_session_metrics() { return nullptr; }
CaptchaMetrics* ProcessMiscMetrics::captcha_metrics() { return nullptr; }
void ProcessMiscMetrics::RegisterPrefs(PrefRegistrySimple* registry) {
#if !BUILDFLAG(IS_ANDROID)
  MenuMetrics::RegisterPrefs(registry);
  NewTabMetrics::RegisterPrefs(registry);
  VerticalTabMetrics::RegisterPrefs(registry);
  SplitViewMetrics::RegisterPrefs(registry);
#else
  PrivacyHubMetrics::RegisterPrefs(registry);
  TabMetrics::RegisterPrefs(registry);
#endif
  CaptchaMetrics::RegisterPrefs(registry);
  DohMetrics::RegisterPrefs(registry);
  MediaSessionMetricsImpl::RegisterPrefs(registry);
  UptimeMonitorImpl::RegisterPrefs(registry);
}
}  // namespace misc_metrics
