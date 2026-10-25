// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/callback_helpers.h"
#include "base/test/task_environment.h"
#include "brave/alloy/site_records/document_settings.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "net/cookies/site_for_cookies.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {

TEST(StatelessCookieModeTest, NativeCookieAndStorageConsumersRejectAdmission) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  content_settings::CookieSettings::RegisterProfilePrefs(registry.get());
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(base::MakeRefCounted<InMemoryPrefStore>());
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto preferences = factory.CreateSyncable(registry);
  DocumentSettings owner(preferences.get(), base::DoNothing());
  auto ordinary = base::MakeRefCounted<content_settings::CookieSettings>(
      &owner.settings(), preferences.get(), false,
      content_settings::CookieSettings::NoFedCmSharingPermissionsCallback());
  auto stateless = base::MakeRefCounted<content_settings::CookieSettings>(
      &owner.settings(), preferences.get(), false,
      content_settings::CookieSettings::NoFedCmSharingPermissionsCallback(),
      content_settings::kDummyExtensionScheme, true);
  const GURL first("https://cookie-owner.test/");
  const GURL embedded("https://embedded-cookie.test/");
  const auto origin = url::Origin::Create(first);
  const auto site = net::SiteForCookies::FromUrl(first);
  ordinary->SetCookieSetting(first, CONTENT_SETTING_ALLOW);
  ASSERT_TRUE(ordinary->IsFullCookieAccessAllowed(first, site, origin, {},
                                                std::nullopt));
  const auto baseline =
      owner.settings().GetSettingsForOneType(ContentSettingsType::COOKIES);

  EXPECT_FALSE(stateless->IsFullCookieAccessAllowed(first, site, origin, {},
                                                   std::nullopt));
  EXPECT_FALSE(stateless->IsFullCookieAccessAllowed(embedded, site, origin, {},
                                                   std::nullopt));
  EXPECT_FALSE(stateless->IsEphemeralCookieAccessAllowed(
      embedded, site, origin, {}, std::nullopt));
  content_settings::CookieSettingsBase::CookieSettingWithMetadata metadata;
  EXPECT_FALSE(stateless->IsAnyStorageAccessAllowed(
      embedded, site, origin, {}, std::nullopt, &metadata));
  EXPECT_FALSE(metadata.allow_partitioned_cookies());
  const content_settings::CookieSettingsBase& consumer = *stateless;
  EXPECT_FALSE(consumer.ShouldUseEphemeralStorage(embedded, site, origin));
  const GURL wordpress("https://wordpress.com/");
  EXPECT_FALSE(stateless->IsFullCookieAccessAllowed(
      GURL("https://wp.com/"), net::SiteForCookies::FromUrl(wordpress),
      url::Origin::Create(wordpress), {}, std::nullopt));
  const GURL internal("chrome://settings/");
  EXPECT_FALSE(stateless->IsFullCookieAccessAllowed(
      embedded, net::SiteForCookies::FromUrl(internal),
      url::Origin::Create(internal), {}, std::nullopt));
  EXPECT_FALSE(stateless->IsThirdPartyAccessAllowed(first));
  EXPECT_EQ(stateless->GetDefaultCookieSetting(), CONTENT_SETTING_BLOCK);
  EXPECT_TRUE(stateless->GetCookieSettings().empty());

  stateless->SetCookieSetting(embedded, CONTENT_SETTING_ALLOW);
  stateless->SetCookieSettingForUserBypass(first);
  stateless->SetThirdPartyCookieSetting(first, CONTENT_SETTING_ALLOW);
  stateless->ResetCookieSetting(first);
  stateless->ResetThirdPartyCookieSetting(first);
  stateless->SetDefaultCookieSetting(CONTENT_SETTING_ALLOW);
  EXPECT_EQ(owner.settings().GetSettingsForOneType(ContentSettingsType::COOKIES),
            baseline);
  EXPECT_TRUE(ordinary->IsFullCookieAccessAllowed(first, site, origin, {},
                                                 std::nullopt));
  EXPECT_FALSE(stateless->IsFullCookieAccessAllowed(first, site, origin, {},
                                                   std::nullopt));

  const url::Origin opaque;
  EXPECT_FALSE(stateless->IsAnyStorageAccessAllowed(
      embedded, net::SiteForCookies(), opaque, {}, std::nullopt));
  stateless->ShutdownOnUIThread();
  ordinary->ShutdownOnUIThread();
}

}  // namespace
}  // namespace alloy::site_records
