// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "chrome/browser/apps/app_service/alloy_preferred_app_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "brave/alloy/site_records/site_profile_configuration.h"
#include "brave/alloy/site_records/preferred_app_storage.h"
namespace apps {
std::unique_ptr<PreferredAppsImpl> CreatePreferredAppsForProfile(
    PreferredAppsImpl::Host* host, Profile* profile,
    base::OnceClosure read_completed, base::OnceClosure write_completed) {
  if (profile) {
    if (auto* input = alloy::site_records::NativeProfileConfiguration::Get(*profile))
      return std::make_unique<PreferredAppsImpl>(host, alloy::site_records::MakePreferredAppStorage(alloy::site_records::GetSiteProfileSettings(*input)),
          std::move(read_completed), std::move(write_completed));
  }
  return std::make_unique<PreferredAppsImpl>(host,
      profile ? profile->GetPath() : base::FilePath(),
      std::move(read_completed), std::move(write_completed));
}
}
