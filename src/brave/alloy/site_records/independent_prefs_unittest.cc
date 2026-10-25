// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "components/sync_preferences/pref_service_syncable.h"

#include "base/memory/ref_counted.h"
#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/signin/public/base/signin_switches.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

// Observe all value-read surfaces, not just whether a sentinel is returned.
class ObservedPrefStore final : public InMemoryPrefStore {
 public:
  mutable int reads = 0;

  bool GetValue(std::string_view key, const base::Value** value) const override {
    ++reads;
    return InMemoryPrefStore::GetValue(key, value);
  }
  base::DictValue GetValues() const override {
    ++reads;
    return InMemoryPrefStore::GetValues();
  }
  bool GetMutableValue(std::string_view key, base::Value** value) override {
    ++reads;
    return InMemoryPrefStore::GetMutableValue(key, value);
  }

 private:
  ~ObservedPrefStore() override = default;
};

void CheckIndependentPreferences(bool account_storage) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatureState(switches::kEnablePreferencesAccountStorage,
                               account_storage);
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  registry->RegisterStringPref("alloy.site", "registered-default");
  auto user = base::MakeRefCounted<ObservedPrefStore>();
  auto account = base::MakeRefCounted<ObservedPrefStore>();
  auto managed = base::MakeRefCounted<ObservedPrefStore>();
  auto extension = base::MakeRefCounted<ObservedPrefStore>();
  auto command_line = base::MakeRefCounted<ObservedPrefStore>();
  auto recommended = base::MakeRefCounted<ObservedPrefStore>();
  for (auto store : {user, account, managed, extension, command_line, recommended}) {
    store->SetValue("alloy.site", base::Value("parent-secret"), 0);
  }
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(user);
  factory.SetAccountPrefStore(account);
  factory.set_managed_prefs(managed);
  factory.set_extension_prefs(extension);
  factory.set_command_line_prefs(command_line);
  factory.set_recommended_prefs(recommended);
  auto parent = factory.CreateSyncable(registry);
  for (auto store : {user, account, managed, extension, command_line, recommended}) {
    store->reads = 0;
  }

  auto child = parent->CreateIndependentPrefService();
  EXPECT_EQ("registered-default", child->GetString("alloy.site"));
  child->SetString("alloy.site", "child-only");
  EXPECT_EQ("child-only", child->GetString("alloy.site"));
  for (auto store : {user, account, managed, extension, command_line, recommended}) {
    EXPECT_EQ(0, store->reads);
  }
  EXPECT_EQ("parent-secret", parent->GetString("alloy.site"));
  parent.reset();
  EXPECT_EQ("child-only", child->GetString("alloy.site"));
}

TEST(IndependentPrefsTest, DoesNotReadOrWriteParentStores) {
  CheckIndependentPreferences(false);
}

TEST(IndependentPrefsTest, AccountStorageHasAnIndependentMemoryBackend) {
  CheckIndependentPreferences(true);
}

TEST(IndependentPrefsTest, ExplicitApplicationInputsRemainEffectiveAndReadOnly) {
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  registry->RegisterStringPref("alloy.config", "registered-default");
  auto parent_store = base::MakeRefCounted<ObservedPrefStore>();
  parent_store->SetValue("alloy.config", base::Value("parent-secret"), 0);
  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_user_prefs(parent_store);
  factory.SetAccountPrefStore(base::MakeRefCounted<InMemoryPrefStore>());
  auto parent = factory.CreateSyncable(registry);
  parent_store->reads = 0;
  auto application_input = base::MakeRefCounted<InMemoryPrefStore>();
  application_input->SetValue("alloy.config", base::Value("explicit-input"), 0);
  auto child = parent->CreateIndependentPrefService(nullptr, nullptr, application_input);
  EXPECT_EQ("explicit-input", child->GetString("alloy.config"));
  ASSERT_TRUE(child->FindPreference("alloy.config"));
  EXPECT_FALSE(child->FindPreference("alloy.config")->IsUserModifiable());
  application_input->SetValue("alloy.config", base::Value("updated-input"), 0);
  EXPECT_EQ("updated-input", child->GetString("alloy.config"));
  EXPECT_EQ(0, parent_store->reads);
}

}  // namespace
}  // namespace alloy::site_records
