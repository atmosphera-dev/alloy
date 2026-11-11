/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BRAVE_BROWSER_MISC_METRICS_PROFILE_MISC_METRICS_SERVICE_H_
#define BRAVE_BROWSER_MISC_METRICS_PROFILE_MISC_METRICS_SERVICE_H_

#include "brave/components/ai_chat/core/common/buildflags/buildflags.h"
#include "build/build_config.h"
#include "components/keyed_service/core/keyed_service.h"

namespace content { class BrowserContext; }
namespace ai_chat { class AIChatMetrics; }
namespace misc_metrics {
class PageMetrics;
class MiscAndroidMetrics;
inline constexpr char kSearchSuggestEnabledHistogramName[] = "Brave.Search.SearchSuggest";
inline constexpr char kShieldsDevModeEnabledHistogramName[] = "Brave.Shields.DevModeEnabled";
inline constexpr char kSponsoredAdsEnabledHistogramName[] = "Brave.Ads.SponsoredEnabled";

class ProfileMiscMetricsService : public KeyedService {
 public:
  explicit ProfileMiscMetricsService(content::BrowserContext* context);
  ~ProfileMiscMetricsService() override;
  ProfileMiscMetricsService(const ProfileMiscMetricsService&) = delete;
  ProfileMiscMetricsService& operator=(const ProfileMiscMetricsService&) = delete;
  void Shutdown() override;
  PageMetrics* GetPageMetrics();
#if BUILDFLAG(ENABLE_AI_CHAT)
  ai_chat::AIChatMetrics* GetAIChatMetrics();
#endif
#if BUILDFLAG(IS_ANDROID)
  MiscAndroidMetrics* GetMiscAndroidMetrics();
#endif
};
}  // namespace misc_metrics
#endif
