// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_adblock_service.h"

#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/metrics/histogram_functions.h"
#include "base/test/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_command_line.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/alloy/bundled_data.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "brave/components/brave_shields/core/common/brave_shield_constants.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {
using Native=brave_shields::AdBlockService;
using Wrapper=brave_shields::AdBlockEngineWrapper;
using ResourceProvider=brave_shields::AdBlockCustomResourceProvider;
using Result=brave_shields::AdBlockSubscriptionFiltersProvider::SourceResult;
struct MemoryInput {
  explicit MemoryInput(StorageMode mode):issuer(base::UnguessableToken::Create(),
      base::UnguessableToken::Create(),base::UnguessableToken::Create(),mode) {}
  RecordCapabilityIssuer issuer;
  std::shared_ptr<ProfileFilterStorage> storage;
  scoped_refptr<ProfileFilterPreferences> input;
};
class ProfileAdBlockServiceTest:public ChromeRenderViewHostTestHarness {
 protected:
  scoped_refptr<ProfileFilterPreferences> Load(std::shared_ptr<ProfileFilterStorage> storage) {
    base::test::TestFuture<FileStatus,scoped_refptr<ProfileFilterPreferences>> done;
    ProfileFilterPreferences::Load(storage,base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindLambdaForTesting([this](FileStatus status){errors.push_back(status);}),done.GetCallback());
    task_environment()->RunUntilIdle();
    if(!done.IsReady()){ADD_FAILURE();return nullptr;}
    EXPECT_EQ(FileStatus::kOk,done.Get<0>());return done.Get<1>();
  }
  std::unique_ptr<MemoryInput> Memory(StorageMode mode=StorageMode::kMemory) {
    auto input=std::make_unique<MemoryInput>(mode);
    EXPECT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(nullptr,
        input->issuer.Issue(StorePurpose::kProfileCustomFilters),&input->storage));
    input->input=Load(input->storage);return input;
  }
  std::unique_ptr<ProfileAdBlockService> Create(scoped_refptr<ProfileFilterPreferences> input) {
    auto owner=ProfileAdBlockService::Create(input,"en",nullptr,
        base::BindRepeating([](const GURL&,bool,brave_shields::AdBlockSubscriptionFiltersProvider::SourceReply done)->base::OnceClosure {
          std::move(done).Run(Result::kError,{});return base::DoNothing();
        }));
    if(!owner || !owner->Get()){ADD_FAILURE();return nullptr;}
    // Installed public inputs are deliberately empty in this native fixture.
    // Private configuration still flows through the real service and engines.
    owner->Get()->custom_resource_provider()->OverrideResourcesForTesting(adblock::new_empty_resource_storage());
    owner->Get()->component_service_manager()->SetFilterListCatalog({});
    task_environment()->RunUntilIdle();return owner;
  }
  bool Blocked(Native* service,const GURL& url) {
    base::test::TestFuture<bool> done;
    EXPECT_TRUE(service->AsyncCallAndReplyWithResult<bool>(base::BindOnce(
        [](GURL request,Wrapper* wrapper) {
          return wrapper->ShouldStartRequest(request,blink::mojom::ResourceType::kImage,
              url::Origin::Create(GURL("https://page.invalid")),"GET",true,false,false,false).matched;
        },url),done.GetCallback()));
    task_environment()->RunUntilIdle();
    if(!done.IsReady()){ADD_FAILURE();return false;}return done.Get();
  }
  void TearDown() override {
    task_environment()->RunUntilIdle();EXPECT_TRUE(errors.empty());
    ChromeRenderViewHostTestHarness::TearDown();
  }
  std::vector<FileStatus> errors;
};

TEST_F(ProfileAdBlockServiceTest, WholeNativeServiceUsesOnlyItsOwnConfigurationAndEnginePair) {
  base::HistogramTester histograms;
  auto a=Memory();auto b=Memory();ASSERT_TRUE(a->input&&b->input);
  auto alpha=Create(a->input);auto beta=Create(b->input);ASSERT_TRUE(alpha&&beta);
  ASSERT_NE(alpha->Get(),beta->Get());
  ASSERT_TRUE(alpha->Get()->custom_filters_provider()->UpdateCustomFilters("||alpha-service.invalid^$important"));
  ASSERT_TRUE(beta->Get()->custom_filters_provider()->UpdateCustomFilters("||beta-service.invalid^$important"));
  task_environment()->RunUntilIdle();
  EXPECT_EQ("||alpha-service.invalid^$important",alpha->Get()->custom_filters_provider()->GetCustomFilters());
  EXPECT_EQ("||beta-service.invalid^$important",beta->Get()->custom_filters_provider()->GetCustomFilters());
  EXPECT_TRUE(Blocked(alpha->Get(),GURL("https://alpha-service.invalid/image.png")));
  EXPECT_FALSE(Blocked(beta->Get(),GURL("https://alpha-service.invalid/image.png")));
  EXPECT_TRUE(Blocked(beta->Get(),GURL("https://beta-service.invalid/image.png")));
  EXPECT_FALSE(Blocked(alpha->Get(),GURL("https://unrelated.invalid/image.png")));
  base::UmaHistogramBoolean("AlloyTest.AdBlockHistogramControl",true);
  histograms.ExpectTotalCount("AlloyTest.AdBlockHistogramControl",1);
  histograms.ExpectTotalCount(brave_shields::kFilterListUsageHistogramName,0);
  histograms.ExpectTotalCount(brave_shields::kAdBlockOnlyModeEnabledHistogramName,0);
  histograms.ExpectTotalCount(brave_shields::kCookieListEnabledHistogram,0);
}

TEST_F(ProfileAdBlockServiceTest, ProfileCloseDrainsAcceptedNativeEngineTaskBeforeItsBackingFence) {
  auto input=Memory();auto owner=Create(input->input);ASSERT_TRUE(owner);auto* native=owner->Get();
  bool accepted_ran=false;
  ASSERT_TRUE(native->AsyncCall(base::BindLambdaForTesting([&](Wrapper* wrapper) {
    EXPECT_FALSE(wrapper->Serialize(false).empty());accepted_ran=true;
  })));
  EXPECT_FALSE(accepted_ran);
  base::test::TestFuture<FileStatus> closed;input->input->Close(closed.GetCallback());
  EXPECT_TRUE(native->IsClosed());EXPECT_EQ(nullptr,owner->Get());EXPECT_EQ(nullptr,owner->prefs());
  EXPECT_FALSE(native->AsyncCall(base::BindOnce([](Wrapper*){ADD_FAILURE();})));
  EXPECT_FALSE(closed.IsReady());
  task_environment()->RunUntilIdle();ASSERT_TRUE(closed.IsReady());
  EXPECT_EQ(FileStatus::kOk,closed.Get());EXPECT_TRUE(accepted_ran);
}

TEST_F(ProfileAdBlockServiceTest, EncryptedNativeServiceColdReopensItsOwnPrivateFilterConsumer) {
  base::ScopedTempDir temp;ASSERT_TRUE(temp.CreateUniqueTempDir());auto path=temp.GetPath().AppendASCII("profile");
  auto key=[] {ObjectId bytes{};bytes.fill(0x82);return SecretKey(bytes);};
  std::shared_ptr<RecordFileSystem> files;ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(path,key(),{},&files));
  std::shared_ptr<ProfileFilterStorage> storage;
  ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
  auto input=Load(storage);auto owner=Create(input);ASSERT_TRUE(owner);
  ASSERT_TRUE(owner->Get()->custom_filters_provider()->UpdateCustomFilters("||cold-service-private.invalid^$important"));
  task_environment()->RunUntilIdle();EXPECT_TRUE(Blocked(owner->Get(),GURL("https://cold-service-private.invalid/image.png")));
  base::test::TestFuture<FileStatus> closed;input->Close(closed.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(closed.IsReady());ASSERT_EQ(FileStatus::kOk,closed.Get());EXPECT_EQ(nullptr,owner->Get());
  owner.reset();input.reset();storage.reset();ASSERT_EQ(FileStatus::kOk,files->Lock());files.reset();
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,key(),&files));
  ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
  input=Load(storage);owner=Create(input);ASSERT_TRUE(owner);
  EXPECT_EQ("||cold-service-private.invalid^$important",owner->Get()->custom_filters_provider()->GetCustomFilters());
  EXPECT_TRUE(Blocked(owner->Get(),GURL("https://cold-service-private.invalid/image.png")));
  EXPECT_FALSE(Blocked(owner->Get(),GURL("https://unrelated.invalid/image.png")));
}

TEST_F(ProfileAdBlockServiceTest, StatelessWholeServiceRefusesPrivateProducerMutations) {
  auto input=Memory(StorageMode::kStateless);auto owner=Create(input->input);ASSERT_TRUE(owner);
  EXPECT_FALSE(owner->Get()->custom_filters_provider()->UpdateCustomFilters("||must-not-store.invalid^"));
  EXPECT_FALSE(owner->Get()->subscription_service_manager()->CreateSubscription(GURL("https://must-not-fetch.invalid/list")));
  EXPECT_TRUE(owner->Get()->custom_filters_provider()->GetCustomFilters().empty());
  TestingPrefServiceSimple dev;dev.registry()->RegisterBooleanPref(brave_shields::prefs::kAdBlockDeveloperMode,true);
  base::DictValue value;value.Set("name","user-forbidden.js");value.Set("content","YWxlcnQoMSk7");
  base::DictValue kind;kind.Set("mime","application/javascript");value.Set("kind",std::move(kind));
  base::test::TestFuture<ResourceProvider::ErrorCode> done;
  owner->Get()->custom_resource_provider()->AddResource(&dev,base::Value(std::move(value)),done.GetCallback());
  ASSERT_TRUE(done.IsReady());EXPECT_EQ(ResourceProvider::ErrorCode::kReadOnly,done.Get());
}
TEST_F(ProfileAdBlockServiceTest, ColdServiceUsesQualifiedCacheAndChangedInputsRebuildAtTheNativeConsumer) {
  base::test::ScopedFeatureList features;features.InitAndEnableFeature(brave_shields::features::kAdblockDATCache);
  base::ScopedTempDir temp;ASSERT_TRUE(temp.CreateUniqueTempDir());auto path=temp.GetPath().AppendASCII("profile");
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchPath(alloy::kDataDirectorySwitch,temp.GetPath());
  auto key=[] {ObjectId bytes{};bytes.fill(0x92);return SecretKey(bytes);};
  std::shared_ptr<RecordFileSystem> files;std::shared_ptr<ProfileFilterStorage> storage;
  scoped_refptr<ProfileFilterPreferences> input;std::unique_ptr<ProfileAdBlockService> owner;
  struct Observed:Native::Observer {
    int hits=0;void OnDATLoaded(bool primary,bool success) override {if(!primary&&success)++hits;}
  } observed;
  auto start=[&]{
    EXPECT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
    input=Load(storage);owner=ProfileAdBlockService::Create(input,"en",nullptr,
        base::BindRepeating([](const GURL&,bool,brave_shields::AdBlockSubscriptionFiltersProvider::SourceReply done)->base::OnceClosure {
          std::move(done).Run(Result::kError,{});return base::DoNothing();}));
    EXPECT_TRUE(owner&&owner->Get());if(!owner||!owner->Get())return;
    owner->Get()->AddObserver(&observed);
    owner->Get()->custom_resource_provider()->OverrideResourcesForTesting(adblock::new_empty_resource_storage());
    owner->Get()->component_service_manager()->SetFilterListCatalog({});task_environment()->RunUntilIdle();
  };
  auto stop=[&]{
    owner->Get()->RemoveObserver(&observed);base::test::TestFuture<FileStatus> closed;input->Close(closed.GetCallback());
    task_environment()->RunUntilIdle();EXPECT_TRUE(closed.IsReady());if(closed.IsReady())EXPECT_EQ(FileStatus::kOk,closed.Get());
    owner.reset();input.reset();storage.reset();EXPECT_EQ(FileStatus::kOk,files->Lock());files.reset();
  };
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(path,key(),{},&files));start();ASSERT_TRUE(owner&&owner->Get());
  ASSERT_TRUE(owner->Get()->custom_filters_provider()->UpdateCustomFilters("||first-snapshot.invalid^$important"));
  task_environment()->RunUntilIdle();EXPECT_TRUE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));stop();
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,key(),&files));observed.hits=0;start();ASSERT_TRUE(owner&&owner->Get());
  EXPECT_GT(observed.hits,0);EXPECT_TRUE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));
  stop();command_line.GetProcessCommandLine()->RemoveSwitch(alloy::kDataDirectorySwitch);
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,key(),&files));observed.hits=0;start();ASSERT_TRUE(owner&&owner->Get());
  EXPECT_EQ(observed.hits,0);EXPECT_TRUE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));
  stop();command_line.GetProcessCommandLine()->AppendSwitchPath(alloy::kDataDirectorySwitch,temp.GetPath());
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,key(),&files));observed.hits=0;start();ASSERT_TRUE(owner&&owner->Get());
  EXPECT_GT(observed.hits,0);EXPECT_TRUE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));
  ASSERT_TRUE(owner->Get()->custom_filters_provider()->UpdateCustomFilters("||second-snapshot.invalid^$important"));
  task_environment()->RunUntilIdle();EXPECT_TRUE(Blocked(owner->Get(),GURL("https://second-snapshot.invalid/image.png")));
  EXPECT_FALSE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));stop();
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,key(),&files));start();ASSERT_TRUE(owner&&owner->Get());
  EXPECT_TRUE(Blocked(owner->Get(),GURL("https://second-snapshot.invalid/image.png")));
  EXPECT_FALSE(Blocked(owner->Get(),GURL("https://first-snapshot.invalid/image.png")));stop();
}
TEST_F(ProfileAdBlockServiceTest, ReentrantNativeCloseJoinsThePostEngineDestructionFence) {
  auto input=Memory();auto owner=Create(input->input);ASSERT_TRUE(owner);auto* native=owner->Get();
  bool accepted_ran=false,outer=false,reentered=false;
  ASSERT_TRUE(native->AsyncCall(base::BindLambdaForTesting([&](Wrapper*){accepted_ran=true;})));
  auto notice=native->ObserveClose(base::BindLambdaForTesting([&]{
    EXPECT_FALSE(accepted_ran);EXPECT_TRUE(native->Close(base::BindLambdaForTesting([&]{EXPECT_TRUE(accepted_ran);reentered=true;})));
  }));
  ASSERT_TRUE(native->Close(base::BindLambdaForTesting([&]{EXPECT_TRUE(accepted_ran);outer=true;})));
  EXPECT_FALSE(outer||reentered);EXPECT_TRUE(native->IsClosed());task_environment()->RunUntilIdle();EXPECT_TRUE(accepted_ran&&outer&&reentered);
}
}  // namespace
}  // namespace alloy::site_records
