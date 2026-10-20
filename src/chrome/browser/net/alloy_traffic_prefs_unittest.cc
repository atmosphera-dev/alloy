// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/net/alloy_traffic_policy.h"
#include "components/policy/core/browser/configuration_policy_pref_store.h"
#include "components/policy/core/browser/configuration_policy_pref_store_test.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service_factory.h"
#include "components/prefs/testing_pref_store.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy {
namespace {
class AlloyTrafficPolicyTest : public policy::ConfigurationPolicyPrefStoreTest {
 protected:
  void SetUp() override {
    AddTrafficPolicyHandlers(&handler_list_);
    auto registry = base::MakeRefCounted<PrefRegistrySimple>();
    RegisterTrafficPrefs(registry.get());
    PrefServiceFactory factory;
    factory.set_managed_prefs(store_);
    factory.set_user_prefs(base::MakeRefCounted<TestingPrefStore>());
    prefs_ = factory.Create(registry);
  }
  void TearDown() override {
    prefs_.reset();
    ConfigurationPolicyPrefStoreTest::TearDown();
  }
  void Set(policy::PolicyMap& map, const char* key, base::Value value) {
    map.Set(key, policy::POLICY_LEVEL_MANDATORY, policy::POLICY_SCOPE_USER,
            policy::POLICY_SOURCE_PLATFORM, std::move(value), nullptr);
  }
  std::unique_ptr<PrefService> prefs_;
};

TEST_F(AlloyTrafficPolicyTest, DefaultsAndIndependentUserPreferences) {
  EXPECT_TRUE(prefs_->GetBoolean(kFaviconsEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kOpenSearchEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kDnsPrefetchEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kPreconnectEnabled));
  prefs_->SetBoolean(kDnsPrefetchEnabled, true);
  EXPECT_TRUE(prefs_->GetBoolean(kDnsPrefetchEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kPreconnectEnabled));
  for (auto* key : kTrafficPrefs) {
    EXPECT_TRUE(prefs_->FindPreference(key)->IsUserModifiable());
  }
}

TEST_F(AlloyTrafficPolicyTest,
       MandatoryPoliciesLockAndRefreshTheActualPrefConsumer) {
  int changed = 0;
  std::vector<std::pair<std::string, bool>> consumed;
  PrefChangeRegistrar observer;
  observer.Init(prefs_.get());
  for (auto* key : kTrafficPrefs) {
    observer.Add(
        key, base::BindRepeating(
                 [](int* count, std::vector<std::pair<std::string, bool>>* seen,
                    PrefService* prefs, const char* key) {
                   ++*count;
                   seen->emplace_back(key, prefs->GetBoolean(key));
                 },
                 &changed, &consumed, prefs_.get(), key));
  }
  policy::PolicyMap policies;
  Set(policies, "AlloyFaviconsEnabled", base::Value(false));
  Set(policies, "AlloyOpenSearchEnabled", base::Value(true));
  Set(policies, "NetworkPredictionOptions", base::Value(0));
  UpdateProviderPolicy(policies);
  EXPECT_FALSE(prefs_->GetBoolean(kFaviconsEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kOpenSearchEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kDnsPrefetchEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kPreconnectEnabled));
  EXPECT_EQ(changed, 4);
  for (auto* key : kTrafficPrefs) {
    EXPECT_TRUE(prefs_->FindPreference(key)->IsManaged());
    EXPECT_FALSE(prefs_->FindPreference(key)->IsUserModifiable());
    prefs_->SetBoolean(key, !prefs_->GetBoolean(key));
  }
  EXPECT_FALSE(prefs_->GetBoolean(kFaviconsEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kOpenSearchEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kDnsPrefetchEnabled));
  EXPECT_TRUE(prefs_->GetBoolean(kPreconnectEnabled));
  // PrefValueStore notifies on changes in any store, including a user value
  // hidden by mandatory policy. The consumer must still read managed values.
  ASSERT_EQ(changed, 8);
  ASSERT_EQ(consumed.size(), 8u);
  for (size_t i = 4; i < consumed.size(); ++i) {
    EXPECT_EQ(consumed[i].second, consumed[i].first != kFaviconsEnabled);
  }
  Set(policies, "NetworkPredictionOptions", base::Value(2));
  UpdateProviderPolicy(policies);
  EXPECT_FALSE(prefs_->GetBoolean(kDnsPrefetchEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kPreconnectEnabled));
  EXPECT_EQ(changed, 10);
  policies.Clear();
  UpdateProviderPolicy(policies);
  // Policy removal reveals the user's retained choices; it does not discard
  // or backfill user data to fake a policy state.
  for (auto* key : kTrafficPrefs) {
    EXPECT_FALSE(prefs_->FindPreference(key)->IsManaged());
    EXPECT_TRUE(prefs_->FindPreference(key)->IsUserModifiable());
  }
  EXPECT_TRUE(prefs_->GetBoolean(kFaviconsEnabled));
  EXPECT_FALSE(prefs_->GetBoolean(kOpenSearchEnabled));
}

TEST_F(AlloyTrafficPolicyTest, AllCurrentNetworkPredictionPolicyValues) {
  for (int value : {0, 1, 2, 3}) {
    policy::PolicyMap policies;
    Set(policies, "NetworkPredictionOptions", base::Value(value));
    UpdateProviderPolicy(policies);
    EXPECT_EQ(prefs_->GetBoolean(kDnsPrefetchEnabled), value != 2);
    EXPECT_EQ(prefs_->GetBoolean(kPreconnectEnabled), value != 2);
    EXPECT_TRUE(prefs_->FindPreference(kDnsPrefetchEnabled)->IsManaged());
    EXPECT_TRUE(prefs_->FindPreference(kPreconnectEnabled)->IsManaged());
  }
}
}  // namespace
}  // namespace alloy
