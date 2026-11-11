/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "brave/browser/misc_metrics/profile_misc_metrics_service.h"

namespace misc_metrics {
ProfileMiscMetricsService::ProfileMiscMetricsService(content::BrowserContext*) {}
ProfileMiscMetricsService::~ProfileMiscMetricsService() = default;
void ProfileMiscMetricsService::Shutdown() {}
PageMetrics* ProfileMiscMetricsService::GetPageMetrics() { return nullptr; }
#if BUILDFLAG(ENABLE_AI_CHAT)
ai_chat::AIChatMetrics* ProfileMiscMetricsService::GetAIChatMetrics() { return nullptr; }
#endif
#if BUILDFLAG(IS_ANDROID)
MiscAndroidMetrics* ProfileMiscMetricsService::GetMiscAndroidMetrics() { return nullptr; }
#endif
}  // namespace misc_metrics
