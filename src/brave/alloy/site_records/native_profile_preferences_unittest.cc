// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/native_profile_preferences.h"
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "brave/alloy/site_records/record_file_system.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/content_settings_rule.h"
#include "components/content_settings/core/browser/permission_settings_registry.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/supervised_user/core/browser/family_link_settings_service.h"
#include "components/supervised_user/core/browser/supervised_user_content_settings_provider.h"
#include "components/supervised_user/core/common/supervised_user_constants.h"
#include "url/gurl.h"
#include "testing/gtest/include/gtest/gtest.h"
#include <atomic>
#include <thread>
namespace alloy::site_records {
namespace {
class NativeProfilePreferencesTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp.CreateUniqueTempDir());ObjectId bytes{};bytes.fill(0xa3);
    ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(temp.GetPath().AppendASCII("profile"),SecretKey(bytes),{},&files));
    runner=base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(),base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    input=base::MakeRefCounted<NativeProfilePreferences>(files->ProfileCapability(StorePurpose::kProfileSettings),runner,
        base::BindLambdaForTesting([&]{return !pending;}),base::BindLambdaForTesting([&]{++errors;}));
  }
  base::test::TaskEnvironment tasks;
  base::ScopedTempDir temp;
  std::shared_ptr<RecordFileSystem> files;
  scoped_refptr<base::SequencedTaskRunner> runner;
  scoped_refptr<NativeProfilePreferences> input;
  bool pending=false;
  int errors=0;
};
TEST_F(NativeProfilePreferencesTest, BorrowedCacheSurvivesRevokedConstructionUntilNativeTeardown) {
  input->SetValue("native.working",base::Value("private-working-value"),0);
  auto view=input->CreateNativeView();ASSERT_TRUE(view);bool retired=false,closed=false;
  ASSERT_TRUE(view->BeginConstruction(base::BindLambdaForTesting([&]{
    const base::Value* value=nullptr;ASSERT_TRUE(view->GetValue("native.working",&value));
    EXPECT_EQ("private-working-value",value->GetString());retired=true;view->FinishTeardown();
  })));
  ASSERT_EQ(FileStatus::kOk,files->Lock());input->Close(base::BindLambdaForTesting([&]{closed=true;}));
  EXPECT_FALSE(closed);EXPECT_FALSE(view->CanStartServiceConstruction());
  view->ConstructionFinished();EXPECT_TRUE(retired);EXPECT_FALSE(closed);tasks.RunUntilIdle();EXPECT_TRUE(closed);EXPECT_TRUE(view->GetValues().empty());EXPECT_EQ(0,errors);
}
TEST_F(NativeProfilePreferencesTest, LockPendingRefusesPreparedConstructionBeforeKeyRevocation) {
  auto view=input->CreateNativeView();ASSERT_TRUE(view);pending=true;
  EXPECT_TRUE(files->unlocked());EXPECT_TRUE(input->ReadOnly());EXPECT_FALSE(input->CreateNativeView());
  EXPECT_FALSE(view->BeginConstruction(base::BindOnce([]{})));bool closed=false;
  input->Close(base::BindLambdaForTesting([&]{closed=true;}));tasks.RunUntilIdle();EXPECT_TRUE(closed);EXPECT_TRUE(view->GetValues().empty());EXPECT_EQ(0,errors);
}
TEST_F(NativeProfilePreferencesTest, NativeShutdownDoneRunsOnOriginalWorkerWithoutUiPumping) {
  base::WaitableEvent completed;const auto ui=std::this_thread::get_id();std::atomic<bool> worker=false;
  input->CommitPendingWrite({},base::BindLambdaForTesting([&]{worker.store(std::this_thread::get_id()!=ui);completed.Signal();}));
  ASSERT_TRUE(completed.TimedWait(base::Seconds(5)));EXPECT_TRUE(worker.load());EXPECT_EQ(0,errors);
  tasks.RunUntilIdle();
}
TEST_F(NativeProfilePreferencesTest, NativeFamilyBackendIsReadyBeforeDependentContentSettingsSubscriber) {
  auto view=input->CreateNativeView();ASSERT_TRUE(view);
  NativeProfileConfiguration::Bindings bindings;bindings.prefs=view;
  bindings.admitted=base::BindRepeating(&NativePrefView::CanStartServiceConstruction,view);
  bindings.begin_construction=base::BindOnce(&NativePrefView::BeginConstruction,view);
  bindings.construction_finished=base::BindRepeating(&NativePrefView::ConstructionFinished,view);
  bindings.finish_teardown=base::BindOnce(&NativePrefView::FinishTeardown,view);
  NativeProfileConfiguration configuration(files->ProfileCapability(StorePurpose::kProfileSettings),std::move(bindings),base::DoNothing());
  ASSERT_TRUE(configuration.BeginConstruction());configuration.SetTeardownHandler(base::DoNothing());
  auto family_input=NativeProfilePrefsForNamespace(configuration,"alloy.family_link");
  auto other_input=NativeProfilePrefsForNamespace(configuration,"alloy.other_owner");
  family_input->SetValue("owner-control",base::Value("original-input"),0);
  EXPECT_TRUE(other_input->GetValues().empty());
  supervised_user::FamilyLinkSettingsService family;family.Init(family_input);
  ASSERT_TRUE(family.IsReady());bool initial=false;
  auto subscription=family.SubscribeForSettingsChange(base::BindLambdaForTesting([&](const base::DictValue&){initial=true;}));
  EXPECT_TRUE(initial);
  content_settings::ContentSettingsRegistry::GetInstance();
  content_settings::PermissionSettingsRegistry::GetInstance();
  {
    supervised_user::SupervisedUserContentSettingsProvider provider(&family);
    EXPECT_FALSE(family.IsActive());
    family.SetLocalSetting(supervised_user::kCameraMicDisabled,base::Value(true));
    EXPECT_FALSE(provider.GetRule(GURL("https://native-input.test"),GURL("https://native-input.test"),ContentSettingsType::MEDIASTREAM_CAMERA,false));
    family.SetActive(true);
    ASSERT_TRUE(family.IsActive());
    EXPECT_TRUE(provider.GetRule(GURL("https://native-input.test"),GURL("https://native-input.test"),ContentSettingsType::MEDIASTREAM_CAMERA,false));
    family.SetLocalSetting(supervised_user::kCameraMicDisabled,base::Value(false));
    EXPECT_FALSE(provider.GetRule(GURL("https://native-input.test"),GURL("https://native-input.test"),ContentSettingsType::MEDIASTREAM_CAMERA,false));
  }
  family.Shutdown();configuration.ConstructionFinished();EXPECT_EQ(0,errors);
}
}  // namespace
}  // namespace alloy::site_records
