// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "brave/alloy/site_records/document_settings.h"
#include "components/permissions/features.h"
#include "components/permissions/origin_keyed_permission_action_service.h"
#include "components/permissions/permission_actions_history.h"
#include "components/permissions/permission_decision_auto_blocker.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "content/public/browser/permission_result.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"

namespace alloy::site_records {
namespace {

class NativeEmbargoObserver
    : public permissions::PermissionDecisionAutoBlocker::Observer {
 public:
  void OnEmbargoStarted(const GURL&, ContentSettingsType) override { ++events; }
  int events = 0;
};

TEST(DocumentPermissionServiceTest,
     NativeEmbargoStaysInDocumentAndForwardsEvents) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto configuration = factory.CreateSyncable(registry);
  DocumentSettings root_owner(configuration.get(), base::DoNothing());
  permissions::PermissionDecisionAutoBlocker root(&root_owner.settings(), true);
  NativeEmbargoObserver observer;
  root.AddObserver(&observer);
  auto first = std::make_unique<DocumentSettings>(configuration.get(),
                                                  base::DoNothing());
  DocumentSettings sibling(configuration.get(), base::DoNothing());
  const auto before = user->GetValues();
  static const int key = 0;
  auto create = [&root] {
    return base::BindOnce(
        [](permissions::PermissionDecisionAutoBlocker* root,
           HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
          return std::make_unique<permissions::PermissionDecisionAutoBlocker>(
              map, false, root->GetDocumentNotificationCallback());
        },
        &root);
  };
  auto* current = static_cast<permissions::PermissionDecisionAutoBlocker*>(
      first->GetOrCreateService(&key, create()));
  auto* peer = static_cast<permissions::PermissionDecisionAutoBlocker*>(
      sibling.GetOrCreateService(&key, create()));
  const GURL url("https://native-embargo.test/");
  constexpr auto type = ContentSettingsType::MEDIASTREAM_MIC;
  for (int i = 0; i != 3; ++i) {
    EXPECT_FALSE(root.RecordDismissAndEmbargo(url, type, false));
    current->RecordDismissAndEmbargo(url, type, false);
  }
  const auto embargo = current->GetEmbargoResult(url, type);
  ASSERT_TRUE(embargo);
  EXPECT_EQ(embargo->source,
            content::PermissionStatusSource::MULTIPLE_DISMISSALS);
  EXPECT_EQ(current->GetDismissCount(url, type), 3);
  EXPECT_EQ(peer->GetDismissCount(url, type), 0);
  EXPECT_FALSE(peer->GetEmbargoResult(url, type));
  EXPECT_EQ(root.GetDismissCount(url, type), 0);
  EXPECT_FALSE(root.GetEmbargoResult(url, type));
  EXPECT_EQ(observer.events, 1);
  auto retired_notification = current->GetDocumentNotificationCallback();
  first.reset();
  retired_notification.Run(url, type);
  EXPECT_EQ(observer.events, 1);
  DocumentSettings replacement(configuration.get(), base::DoNothing());
  auto* fresh = static_cast<permissions::PermissionDecisionAutoBlocker*>(
      replacement.GetOrCreateService(&key, create()));
  EXPECT_EQ(fresh->GetDismissCount(url, type), 0);
  EXPECT_FALSE(fresh->GetEmbargoResult(url, type));
  EXPECT_EQ(user->GetValues(), before);
  root.RemoveObserver(&observer);
}

TEST(DocumentPermissionServiceTest,
     PureMetricActionsAndOtpCountsNeverRetainFacts) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  permissions::PermissionActionsHistory::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto configuration = factory.CreateSyncable(registry);
  DocumentSettings owner(configuration.get(), base::DoNothing());
  permissions::PermissionActionsHistory history(configuration.get(),
                                                &owner.settings());
  permissions::OriginKeyedPermissionActionService metrics;
  const auto before = user->GetValues();
  const GURL url("https://discarded-metric.test/");
  constexpr auto type = ContentSettingsType::MEDIASTREAM_MIC;
  metrics.RecordAction(url, type, permissions::PermissionAction::GRANTED_ONCE);
  metrics.RecordActionWithTimeForTesting(
      url, type, permissions::PermissionAction::DENIED, base::TimeTicks::Now());
  EXPECT_FALSE(metrics.GetLastActionEntry(url, type));
  history.RecordOneTimeGrant(url, type);
  EXPECT_EQ(history.GetOneTimeGrantCount(url, type), 0);
  EXPECT_TRUE(owner.settings()
                  .GetWebsiteSetting(
                      url, url, ContentSettingsType::PERMISSION_ACTIONS_HISTORY)
                  .is_none());
  EXPECT_EQ(user->GetValues(), before);
}


TEST(DocumentPermissionServiceTest,
     NativeEmbargoWriteCannotContinueAfterSettingsObserverRetiresOwner) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto configuration = factory.CreateSyncable(registry);
  std::unique_ptr<DocumentSettings> owner;
  int retirement_callbacks = 0;
  owner = std::make_unique<DocumentSettings>(
      configuration.get(),
      base::BindRepeating(
          [](std::unique_ptr<DocumentSettings>* owner, int* callbacks,
             const ContentSettingsPattern&, const ContentSettingsPattern&,
             ContentSettingsTypeSet types) {
            if (types.Contains(
                    ContentSettingsType::PERMISSION_AUTOBLOCKER_DATA)) {
              ++*callbacks;
              owner->reset();
            }
          },
          &owner, &retirement_callbacks));
  static const int key = 0;
  int embargo_events = 0;
  auto* current = static_cast<
      permissions::PermissionDecisionAutoBlocker*>(owner->GetOrCreateService(
      &key,
      base::BindOnce(
          [](int* events,
             HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
            return std::make_unique<permissions::PermissionDecisionAutoBlocker>(
                map, false,
                base::BindRepeating([](int* events, const GURL&,
                                       ContentSettingsType) { ++*events; },
                                    events));
          },
          &embargo_events)));
  const auto before = user->GetValues();
  // A quiet dismissal normally writes both total and quiet counters. Its first
  // native map notification retires the owner, so it must not perform the
  // second write through a destroyed service or emit an embargo for a retired
  // document.
  EXPECT_FALSE(
      current->RecordDismissAndEmbargo(GURL("https://retiring-embargo.test/"),
                                       ContentSettingsType::GEOLOCATION, true));
  EXPECT_FALSE(owner);
  EXPECT_EQ(retirement_callbacks, 1);
  EXPECT_EQ(embargo_events, 0);
  EXPECT_EQ(user->GetValues(), before);
}

TEST(DocumentPermissionServiceTest,
     NativeHeuristicWriteCannotNotifyAfterSettingsObserverRetiresOwner) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {blink::features::kGeolocationElement,
       permissions::features::kPermissionHeuristicAutoGrant}, {});
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  permissions::PermissionActionsHistory::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto configuration = factory.CreateSyncable(registry);
  std::unique_ptr<DocumentSettings> owner;
  bool retire_on_change = false;
  owner = std::make_unique<DocumentSettings>(
      configuration.get(),
      base::BindRepeating(
          [](std::unique_ptr<DocumentSettings>* owner, bool* retire,
             const ContentSettingsPattern&, const ContentSettingsPattern&,
             ContentSettingsTypeSet types) {
            if (*retire &&
                types.Contains(
                    ContentSettingsType::PERMISSION_ACTIONS_HISTORY)) {
              owner->reset();
            }
          },
          &owner, &retire_on_change));
  static const int key = 0;
  int heuristic_events = 0;
  auto* current = static_cast<permissions::PermissionActionsHistory*>(
      owner->GetOrCreateService(
          &key,
          base::BindOnce(
              [](PrefService* configuration, int* events,
                 HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
                return std::make_unique<permissions::PermissionActionsHistory>(
                    configuration, map, false,
                    base::BindRepeating([](int* events, const GURL&,
                                           ContentSettingsType) { ++*events; },
                                        events));
              },
              configuration.get(), &heuristic_events)));
  const auto before = user->GetValues();
  const GURL url("https://retiring-heuristic.test/");
  constexpr auto type = ContentSettingsType::GEOLOCATION;
  for (int i = 0; i < 3; ++i) {
    current->RecordTemporaryGrant(url, type);
  }
  EXPECT_EQ(heuristic_events, 0);
  retire_on_change = true;
  EXPECT_FALSE(current->RecordTemporaryGrant(url, type));
  EXPECT_FALSE(owner);
  EXPECT_EQ(heuristic_events, 0);
  EXPECT_EQ(user->GetValues(), before);
}

}  // namespace
}  // namespace alloy::site_records
