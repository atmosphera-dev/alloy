// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CHROME_BROWSER_APPS_APP_SERVICE_ALLOY_PREFERRED_APP_STORAGE_H_
#define CHROME_BROWSER_APPS_APP_SERVICE_ALLOY_PREFERRED_APP_STORAGE_H_
#include "components/services/app_service/public/cpp/preferred_apps_impl.h"
class Profile;
namespace apps {
std::unique_ptr<PreferredAppsImpl> CreatePreferredAppsForProfile(
    PreferredAppsImpl::Host* host, Profile* profile,
    base::OnceClosure read_completed = {}, base::OnceClosure write_completed = {});
}
#endif
