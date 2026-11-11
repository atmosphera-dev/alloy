/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BRAVE_BROWSER_MISC_METRICS_PROCESS_MISC_METRICS_H_
#define BRAVE_BROWSER_MISC_METRICS_PROCESS_MISC_METRICS_H_

#include <memory>
#include "brave/components/misc_metrics/web3_metrics.h"
#include "build/build_config.h"

class PrefRegistrySimple;
class PrefService;
namespace misc_metrics {
inline constexpr char kWidevineEnabledHistogramName[] = "Brave.Core.WidevineEnabled";
class MenuMetrics;
class NewTabMetrics;
class VerticalTabMetrics;
class SplitViewMetrics;
class PrivacyHubMetrics;
class TabMetrics;
class CaptchaMetrics;
class DefaultBrowserMonitor;
class DohMetrics;
class MediaSessionMetricsImpl;
class UptimeMonitorImpl;

// Compatibility endpoints are stateless; no process activity owner is created.
class ProcessMiscMetrics {
 public:
  explicit ProcessMiscMetrics(PrefService* local_state);
  ~ProcessMiscMetrics();
  ProcessMiscMetrics(const ProcessMiscMetrics&) = delete;
  ProcessMiscMetrics& operator=(const ProcessMiscMetrics&) = delete;
  static void RegisterPrefs(PrefRegistrySimple* registry);
  Web3Metrics& web3_metrics();
#if !BUILDFLAG(IS_ANDROID)
  MenuMetrics* menu_metrics();
  NewTabMetrics* new_tab_metrics();
  VerticalTabMetrics* vertical_tab_metrics();
  SplitViewMetrics* split_view_metrics();
#else
  PrivacyHubMetrics* privacy_hub_metrics();
  TabMetrics* tab_metrics();
#endif
  DefaultBrowserMonitor* default_browser_monitor();
  UptimeMonitorImpl* uptime_monitor();
  MediaSessionMetricsImpl* media_session_metrics();
  CaptchaMetrics* captcha_metrics();

 private:
  Web3Metrics web3_metrics_;
#if !BUILDFLAG(IS_ANDROID)
  std::unique_ptr<MenuMetrics> menu_metrics_;
  std::unique_ptr<NewTabMetrics> new_tab_metrics_;
#endif
};
}  // namespace misc_metrics
#endif
