// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::site_records {
namespace {
class ProfileAdBlockBrokerTest:public ChromeRenderViewHostTestHarness {
 protected:
  void TearDown() override {task_environment()->RunUntilIdle();ChromeRenderViewHostTestHarness::TearDown();}
};
TEST_F(ProfileAdBlockBrokerTest, OriginalMemoryContextOwnsOneServiceAndRevocationCannotRecreateIt) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*profile());RequestContextPolicy::AttachMemory(*peer);
  auto* alpha=brave_shields::AdBlockServiceForContext(*profile());auto* beta=brave_shields::AdBlockServiceForContext(*peer);
  ASSERT_TRUE(alpha&&beta);EXPECT_NE(alpha,beta);EXPECT_EQ(alpha,brave_shields::AdBlockServiceForContext(*profile()));
  ASSERT_TRUE(alpha->custom_filters_provider()->UpdateCustomFilters("||broker-alpha.invalid^$important"));
  EXPECT_TRUE(beta->custom_filters_provider()->GetCustomFilters().empty());
  RequestContextPolicy::Get(*profile())->Revoke();EXPECT_EQ(nullptr,brave_shields::AdBlockServiceForContext(*profile()));
  EXPECT_EQ(nullptr,brave_shields::AdBlockPrefsForContext(*profile()));
  EXPECT_EQ(beta,brave_shields::AdBlockServiceForContext(*peer));
}
TEST_F(ProfileAdBlockBrokerTest, StatelessBrokerRefusesEditsAndMissingEncryptedInputHasNoLegacyFallback) {
  RequestContextPolicy::AttachStateless(*profile(),NetworkState::kKeep);
  auto* service=brave_shields::AdBlockServiceForContext(*profile());ASSERT_TRUE(service);
  EXPECT_FALSE(service->custom_filters_provider()->UpdateCustomFilters("||must-not-store.invalid^"));
  auto peer=TestingProfile::Builder().Build();base::ScopedTempDir temp;ASSERT_TRUE(temp.CreateUniqueTempDir());
  ObjectId key{};key.fill(0x95);std::shared_ptr<RecordFileSystem> files;
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(temp.GetPath().AppendASCII("profile"),SecretKey(key),{},&files));
  RequestContextPolicy::AttachEncrypted(*peer,files->ProfileCapability(StorePurpose::kProfileSettings));
  EXPECT_EQ(nullptr,brave_shields::AdBlockServiceForContext(*peer));
  EXPECT_EQ(nullptr,brave_shields::AdBlockPrefsForContext(*peer));
}
TEST_F(ProfileAdBlockBrokerTest, SharedEncryptedInputKeepsOneServiceUntilLastContextAndLockDeniesIt) {
  auto a=TestingProfile::Builder().Build();auto b=TestingProfile::Builder().Build();
  base::ScopedTempDir temp;ASSERT_TRUE(temp.CreateUniqueTempDir());ObjectId key{};key.fill(0x96);
  std::shared_ptr<RecordFileSystem> files;ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(temp.GetPath().AppendASCII("profile"),SecretKey(key),{},&files));
  std::shared_ptr<ProfileFilterStorage> storage;ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
  base::test::TestFuture<FileStatus,scoped_refptr<ProfileFilterPreferences>> loaded;
  ProfileFilterPreferences::Load(storage,base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating([](FileStatus){ADD_FAILURE();}),loaded.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(loaded.IsReady());ASSERT_EQ(FileStatus::kOk,loaded.Get<0>());auto input=loaded.Get<1>();
  RequestContextPolicy::AttachEncrypted(*a,files->ProfileCapability(StorePurpose::kProfileSettings));
  RequestContextPolicy::AttachEncrypted(*b,files->ProfileCapability(StorePurpose::kProfileSettings));
  ProfileFilterPreferences::Attach(*a,input);ProfileFilterPreferences::Attach(*b,input);
  auto* service=brave_shields::AdBlockServiceForContext(*a);ASSERT_TRUE(service);
  EXPECT_EQ(service,brave_shields::AdBlockServiceForContext(*b));
  ASSERT_TRUE(service->custom_filters_provider()->UpdateCustomFilters("||shared-broker.invalid^"));
  a.reset();task_environment()->RunUntilIdle();EXPECT_EQ(service,brave_shields::AdBlockServiceForContext(*b));
  EXPECT_EQ("||shared-broker.invalid^",service->custom_filters_provider()->GetCustomFilters());
  base::test::TestFuture<FileStatus> closed;input->Close(closed.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(closed.IsReady());EXPECT_EQ(FileStatus::kOk,closed.Get());EXPECT_EQ(nullptr,brave_shields::AdBlockServiceForContext(*b));
  EXPECT_EQ(nullptr,brave_shields::AdBlockPrefsForContext(*b));b.reset();task_environment()->RunUntilIdle();
}
}  // namespace
}  // namespace alloy::site_records
