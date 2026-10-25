// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/document_settings.h"

#include <vector>

#include "base/memory/ref_counted.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
class ScopedSettings {
 public:
  explicit ScopedSettings(PrefService* prefs)
      : map(base::MakeRefCounted<HostContentSettingsMap>(prefs,
                                                         true,
                                                         false,
                                                         false,
                                                         false)) {}
  ~ScopedSettings() { map->ShutdownOnUIThread(); }
  scoped_refptr<HostContentSettingsMap> map;
};

TEST(DocumentSettingsTest, NativeShardsIsolateDecisionsAndHonorLivePolicy) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  auto policy = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  factory.set_managed_prefs(policy);
  auto service = factory.CreateSyncable(registry);
  const GURL site("https://document.test/");
  ScopedSettings first(service.get());
  ScopedSettings second(service.get());
  const auto before = user->GetValues();
  first.map->SetContentSettingDefaultScope(
      site, site, ContentSettingsType::JAVASCRIPT, CONTENT_SETTING_BLOCK);
  EXPECT_EQ(CONTENT_SETTING_BLOCK,
            first.map->GetContentSetting(site, site,
                                         ContentSettingsType::JAVASCRIPT));
  EXPECT_EQ(CONTENT_SETTING_ALLOW,
            second.map->GetContentSetting(site, site,
                                          ContentSettingsType::JAVASCRIPT));
  EXPECT_EQ(before, user->GetValues());
  // Configuration is an input, not a per-document grant. It remains live and
  // takes precedence over both shards' user decisions without copying them.
  policy->SetValue(prefs::kManagedDefaultJavaScriptSetting,
                   base::Value(static_cast<int>(CONTENT_SETTING_BLOCK)), 0);
  EXPECT_EQ(CONTENT_SETTING_BLOCK,
            second.map->GetContentSetting(site, site,
                                          ContentSettingsType::JAVASCRIPT));
  policy->RemoveValue(prefs::kManagedDefaultJavaScriptSetting, 0);
  EXPECT_EQ(CONTENT_SETTING_ALLOW,
            second.map->GetContentSetting(site, site,
                                          ContentSettingsType::JAVASCRIPT));
  ScopedSettings replacement(service.get());
  EXPECT_EQ(CONTENT_SETTING_ALLOW,
            replacement.map->GetContentSetting(
                site, site, ContentSettingsType::JAVASCRIPT));
}

TEST(DocumentSettingsTest,
     SubscribersSeeUpdatedShardOnDecisionAndPolicyChange) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto policy = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(base::MakeRefCounted<InMemoryPrefStore>());
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  factory.set_managed_prefs(policy);
  auto service = factory.CreateSyncable(registry);
  const GURL site("https://document.test/");
  std::vector<ContentSetting> observations;
  DocumentSettings* owner = nullptr;
  DocumentSettings document(
      service.get(),
      base::BindLambdaForTesting([&owner, &site, &observations](
                                     const ContentSettingsPattern& primary,
                                     const ContentSettingsPattern& secondary,
                                     ContentSettingsTypeSet types) {
        if (types.Contains(ContentSettingsType::JAVASCRIPT)) {
          observations.push_back(owner->settings().GetContentSetting(
              site, site, ContentSettingsType::JAVASCRIPT));
        }
      }));
  owner = &document;
  document.settings().SetContentSettingDefaultScope(
      site, site, ContentSettingsType::JAVASCRIPT, CONTENT_SETTING_BLOCK);
  ASSERT_FALSE(observations.empty());
  EXPECT_EQ(CONTENT_SETTING_BLOCK, observations.back());
  observations.clear();
  policy->SetValue(prefs::kManagedDefaultJavaScriptSetting,
                   base::Value(static_cast<int>(CONTENT_SETTING_ALLOW)), 0);
  ASSERT_FALSE(observations.empty());
  EXPECT_EQ(CONTENT_SETTING_ALLOW, observations.back());
  observations.clear();
  policy->RemoveValue(prefs::kManagedDefaultJavaScriptSetting, 0);
  ASSERT_FALSE(observations.empty());
  EXPECT_EQ(CONTENT_SETTING_BLOCK, observations.back());
}

class NativeSettingsClient final : public KeyedService {
 public:
  NativeSettingsClient(HostContentSettingsMap* map,
                       bool* shutdown,
                       bool* destroyed)
      : map_(map), shutdown_(shutdown), destroyed_(destroyed) {}
  ~NativeSettingsClient() override {
    EXPECT_TRUE(*shutdown_);
    *destroyed_ = true;
  }
  void Shutdown() override {
    // ObjectPermissionContextBase's pending saves need this same native map
    // contract: the map must still accept its final write during shutdown.
    const GURL site("https://ending-document.test/");
    map_->SetContentSettingDefaultScope(
        site, site, ContentSettingsType::JAVASCRIPT, CONTENT_SETTING_BLOCK);
    EXPECT_EQ(
        CONTENT_SETTING_BLOCK,
        map_->GetContentSetting(site, site, ContentSettingsType::JAVASCRIPT));
    *shutdown_ = true;
  }

 private:
  HostContentSettingsMap* const map_;
  bool* const shutdown_;
  bool* const destroyed_;
};

TEST(DocumentSettingsTest,
     NativeClientsFinishBeforeMapWithoutBackingStoreWrites) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto service = factory.CreateSyncable(registry);
  static const int key = 0;
  bool shutdown = false, destroyed = false;
  int notifications = 0, constructed = 0;
  auto before = user->GetValues();
  {
    DocumentSettings owner(
        service.get(),
        base::BindLambdaForTesting(
            [&](const ContentSettingsPattern&, const ContentSettingsPattern&,
                ContentSettingsTypeSet) { ++notifications; }));
    // PrefProvider initializes its configuration schema version in its ctor.
    // Client admission/flushes must not change the resulting backing store.
    before = user->GetValues();
    auto create = [&]() {
      return base::BindLambdaForTesting(
          [&](HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
            ++constructed;
            return std::make_unique<NativeSettingsClient>(map, &shutdown,
                                                          &destroyed);
          });
    };
    auto* first = owner.GetOrCreateService(&key, create());
    EXPECT_EQ(first, owner.GetOrCreateService(&key, create()));
    EXPECT_EQ(1, constructed);
    EXPECT_FALSE(shutdown);
  }
  EXPECT_TRUE(shutdown);
  EXPECT_TRUE(destroyed);
  EXPECT_EQ(before, user->GetValues());
  // The ending document's final flush cannot reenter consumers/new documents.
  EXPECT_EQ(0, notifications);
}
}  // namespace
}  // namespace alloy::site_records
