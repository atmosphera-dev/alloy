// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/scoped_observation.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "brave/alloy/site_records/document_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/permissions/object_permission_context_base.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {

class NativeObjectGrants final
    : public permissions::ObjectPermissionContextBase {
 public:
  explicit NativeObjectGrants(HostContentSettingsMap* map)
      : ObjectPermissionContextBase(ContentSettingsType::USB_GUARD,
                                    ContentSettingsType::USB_CHOOSER_DATA,
                                    map) {}
  std::string GetKeyForObject(const base::DictValue& object) override {
    return *object.FindString("id");
  }
  bool IsValidObject(const base::DictValue& object) override {
    return object.FindString("id") != nullptr;
  }
  std::u16string GetObjectDisplayName(const base::DictValue&) override {
    return u"Native fake grant";
  }
};

class GrantObserver final
    : public permissions::ObjectPermissionContextBase::PermissionObserver {
 public:
  void OnPermissionRevoked(const url::Origin& origin) override {
    revoked.push_back(origin);
  }
  std::vector<url::Origin> revoked;
};

TEST(DocumentObjectGrantsTest,
     NativeCachesPolicyRevocationAndReplacementAreOwned) {
  base::test::TaskEnvironment tasks;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  auto user = base::MakeRefCounted<InMemoryPrefStore>();
  auto policy = base::MakeRefCounted<InMemoryPrefStore>();
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  factory.set_managed_prefs(policy);
  auto configuration = factory.CreateSyncable(registry);
  DocumentSettings root_owner(configuration.get(), base::DoNothing());
  static const int key = 0;
  auto* root_grants =
      static_cast<NativeObjectGrants*>(root_owner.GetOrCreateService(
          &key,
          base::BindOnce(
              [](HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
                return std::make_unique<NativeObjectGrants>(map);
              })));
  GrantObserver observer;
  base::ScopedObservation<NativeObjectGrants, GrantObserver> observation(&observer);
  observation.Observe(root_grants);
  auto create = [root_grants]() {
    return base::BindOnce(
        [](NativeObjectGrants* target,
           HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
          auto grants = std::make_unique<NativeObjectGrants>(map);
          grants->SetDocumentScope(*target);
          return grants;
        },
        root_grants);
  };
  auto first = std::make_unique<DocumentSettings>(configuration.get(),
                                                  base::DoNothing());
  DocumentSettings sibling(configuration.get(), base::DoNothing());
  auto* a = static_cast<NativeObjectGrants*>(
      first->GetOrCreateService(&key, create()));
  auto* b = static_cast<NativeObjectGrants*>(
      sibling.GetOrCreateService(&key, create()));
  const auto before = user->GetValues();
  const auto site = url::Origin::Create(GURL("https://device.test/"));
  EXPECT_TRUE(
      b->GetGrantedObjects(site).empty());  // Prime native sibling cache.
  auto grant = [] {
    base::DictValue value;
    value.Set("id", "device");
    return value;
  };
  a->GrantObjectPermission(site, grant());
  tasks.RunUntilIdle();
  ASSERT_NE(nullptr, a->GetGrantedObject(site, "device"));
  EXPECT_TRUE(b->GetGrantedObjects(site).empty());
  EXPECT_TRUE(root_grants->GetGrantedObjects(site).empty());
  policy->SetValue(prefs::kManagedDefaultWebUsbGuardSetting,
                   base::Value(static_cast<int>(CONTENT_SETTING_BLOCK)), 0);
  EXPECT_FALSE(a->CanRequestObjectPermission(site));
  EXPECT_EQ(nullptr, a->GetGrantedObject(site, "device"));
  // Native policy invalidation is itself a revocation notification, relayed to
  // existing consumers even though the grant data remains in the document.
  ASSERT_FALSE(observer.revoked.empty());
  EXPECT_EQ(site, observer.revoked.back());
  policy->RemoveValue(prefs::kManagedDefaultWebUsbGuardSetting, 0);
  ASSERT_NE(nullptr, a->GetGrantedObject(site, "device"));
  const auto before_explicit_revoke = observer.revoked.size();
  a->RevokeObjectPermission(site, std::string_view("device"));
  ASSERT_EQ(before_explicit_revoke + 1, observer.revoked.size());
  EXPECT_EQ(site, observer.revoked.back());
  EXPECT_TRUE(b->GetGrantedObjects(site).empty());
  a->GrantObjectPermission(site, grant());
  // End with a pending native save. Its destructor must flush into the dying
  // document's map, never the root/sibling or the configuration's user store.
  first.reset();
  tasks.RunUntilIdle();
  DocumentSettings replacement(configuration.get(), base::DoNothing());
  auto* fresh = static_cast<NativeObjectGrants*>(
      replacement.GetOrCreateService(&key, create()));
  EXPECT_TRUE(fresh->GetGrantedObjects(site).empty());
  EXPECT_TRUE(b->GetGrantedObjects(site).empty());
  EXPECT_TRUE(root_grants->GetGrantedObjects(site).empty());
  EXPECT_EQ(before, user->GetValues());
  observation.Reset();
}

}  // namespace
}  // namespace alloy::site_records
