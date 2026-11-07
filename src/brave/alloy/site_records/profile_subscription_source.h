// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_SUBSCRIPTION_SOURCE_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_SUBSCRIPTION_SOURCE_H_

#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "content/public/browser/web_ui_data_source.h"

namespace alloy::site_records {
bool IsProfileSubscriptionSourcePath(const std::string& path);
content::WebUIDataSource::HandleRequestCallback ProfileSubscriptionSourceHandler(
    scoped_refptr<ProfileFilterPreferences> original);
void InstallProfileSubscriptionSource(content::WebUIDataSource& source,
    scoped_refptr<ProfileFilterPreferences> original);
}  // namespace alloy::site_records
#endif
