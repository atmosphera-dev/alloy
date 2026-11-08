/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "brave/browser/misc_metrics/profile_misc_metrics_service_factory.h"
#include <memory>
#include "base/no_destructor.h"
#include "brave/browser/misc_metrics/profile_misc_metrics_service.h"
#include "chrome/browser/profiles/profile_selections.h"

namespace misc_metrics {
ProfileMiscMetricsServiceFactory* ProfileMiscMetricsServiceFactory::GetInstance() {
  static base::NoDestructor<ProfileMiscMetricsServiceFactory> instance;
  return instance.get();
}
ProfileMiscMetricsService* ProfileMiscMetricsServiceFactory::GetServiceForContext(content::BrowserContext* context) {
  return static_cast<ProfileMiscMetricsService*>(GetInstance()->GetServiceForBrowserContext(context, true));
}
ProfileMiscMetricsServiceFactory::ProfileMiscMetricsServiceFactory()
    : ProfileKeyedServiceFactory("ProfileMiscMetricsService", ProfileSelections::BuildForRegularProfile()) {}
ProfileMiscMetricsServiceFactory::~ProfileMiscMetricsServiceFactory() = default;
std::unique_ptr<KeyedService> ProfileMiscMetricsServiceFactory::BuildServiceInstanceForBrowserContext(content::BrowserContext* context) const {
  return std::make_unique<ProfileMiscMetricsService>(context);
}
}  // namespace misc_metrics
