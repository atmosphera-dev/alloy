// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
SecretKey PreferencesKey() { ObjectId bytes{}; bytes.fill(0x73); return SecretKey(bytes); }
class ProfileFilterPreferencesTest : public ChromeRenderViewHostTestHarness {
 protected:
  scoped_refptr<ProfileFilterPreferences> Load(std::shared_ptr<ProfileFilterStorage> storage) {
    base::test::TestFuture<FileStatus, scoped_refptr<ProfileFilterPreferences>> loaded;
    ProfileFilterPreferences::Load(storage, base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindLambdaForTesting([&](FileStatus status) { errors.push_back(status); }), loaded.GetCallback());
    task_environment()->RunUntilIdle();
    if (!loaded.IsReady()) { ADD_FAILURE(); return nullptr; }
    EXPECT_EQ(FileStatus::kOk, loaded.Get<0>()); return loaded.Get<1>();
  }
  std::unique_ptr<PrefService> Prefs(scoped_refptr<ProfileFilterPreferences> store) {
    auto registry = base::MakeRefCounted<PrefRegistrySimple>();
    brave_shields::RegisterPrefsForAdBlockService(registry.get());
    PrefServiceFactory factory; factory.set_user_prefs(store); return factory.Create(registry);
  }
  std::vector<FileStatus> errors;
};
TEST_F(ProfileFilterPreferencesTest, NativeProducerUsesOnlyItsOriginalContextAndRefusesStatelessEdits) {
  auto peer = TestingProfile::Builder().Build();
  RequestContextPolicy::AttachMemory(*profile()); RequestContextPolicy::AttachStateless(*peer, NetworkState::kKeep);
  std::shared_ptr<ProfileFilterStorage> memory, strict;
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(nullptr, RequestContextPolicy::Get(*profile())->StorageCapability(StorePurpose::kProfileCustomFilters), &memory));
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(nullptr, RequestContextPolicy::Get(*peer)->StorageCapability(StorePurpose::kProfileCustomFilters), &strict));
  auto own = Load(memory), other = Load(strict); ASSERT_TRUE(own && other);
  ProfileFilterPreferences::Attach(*profile(), own); ProfileFilterPreferences::Attach(*peer, other);
  EXPECT_EQ(own, ProfileFilterPreferences::Get(*profile())); EXPECT_EQ(other, ProfileFilterPreferences::Get(*peer));
  auto first = Prefs(own), second = Prefs(other);
  brave_shields::AdBlockFiltersProviderManager manager1, manager2;
  brave_shields::AdBlockCustomFiltersProvider producer1(first.get(), &manager1), producer2(second.get(), &manager2);
  EXPECT_TRUE(producer1.UpdateCustomFilters("||native-config-private.invalid^"));
  EXPECT_EQ("||native-config-private.invalid^", producer1.GetCustomFilters());
  EXPECT_TRUE(producer2.GetCustomFilters().empty()); EXPECT_FALSE(producer2.UpdateCustomFilters("must-not-store"));
  EXPECT_TRUE(producer2.GetCustomFilters().empty()); task_environment()->RunUntilIdle();
  RequestContextPolicy::Get(*profile())->Revoke();
  EXPECT_FALSE(ProfileFilterPreferences::Get(*profile())); EXPECT_FALSE(producer1.UpdateCustomFilters("revoked"));
  EXPECT_TRUE(producer1.GetCustomFilters().empty()); EXPECT_TRUE(errors.empty());
}
TEST_F(ProfileFilterPreferencesTest, CloseDrainsAcceptedNativeEditBeforeColdEncryptedReload) {
  base::ScopedTempDir temp; ASSERT_TRUE(temp.CreateUniqueTempDir()); auto path = temp.GetPath().AppendASCII("profile");
  std::shared_ptr<RecordFileSystem> files; ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path, PreferencesKey(), {}, &files));
  std::shared_ptr<ProfileFilterStorage> storage; ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files, files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  auto store = Load(storage); ASSERT_TRUE(store); auto prefs = Prefs(store);
  brave_shields::AdBlockFiltersProviderManager manager;
  {
    brave_shields::AdBlockCustomFiltersProvider producer(prefs.get(), &manager);
    ASSERT_TRUE(producer.UpdateCustomFilters("||cold-native-configuration.invalid^"));
    bool closed = false;
    store->Close(base::BindLambdaForTesting([&](FileStatus status) { EXPECT_EQ(FileStatus::kOk, status); closed = true; }));
    EXPECT_FALSE(closed); EXPECT_TRUE(producer.GetCustomFilters().empty());
    EXPECT_FALSE(producer.UpdateCustomFilters("late-edit")); task_environment()->RunUntilIdle(); EXPECT_TRUE(closed);
  }
  prefs.reset(); store.reset(); storage.reset(); ASSERT_EQ(FileStatus::kOk, files->Lock()); files.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path, PreferencesKey(), &files));
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files, files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  store = Load(storage); ASSERT_TRUE(store); prefs = Prefs(store);
  brave_shields::AdBlockCustomFiltersProvider reopened(prefs.get(), &manager);
  EXPECT_EQ("||cold-native-configuration.invalid^", reopened.GetCustomFilters()); EXPECT_TRUE(errors.empty());
}
}  // namespace
}  // namespace alloy::site_records
