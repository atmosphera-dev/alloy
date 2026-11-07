// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::site_records {
namespace {
using Manager=brave_shields::AdBlockSubscriptionServiceManager;
using Provider=brave_shields::AdBlockSubscriptionFiltersProvider;
using Result=Provider::SourceResult;
SecretKey SubscriptionKey(){ObjectId key{};key.fill(0x76);return SecretKey(key);}
class ProfileFilterSubscriptionsTest:public ChromeRenderViewHostTestHarness {
 protected:
  scoped_refptr<ProfileFilterPreferences> Load(std::shared_ptr<ProfileFilterStorage> storage){
    base::test::TestFuture<FileStatus,scoped_refptr<ProfileFilterPreferences>> done;
    ProfileFilterPreferences::Load(storage,base::SequencedTaskRunner::GetCurrentDefault(),base::BindLambdaForTesting([&](FileStatus s){errors.push_back(s);}),done.GetCallback());
    task_environment()->RunUntilIdle();if(!done.IsReady()){ADD_FAILURE();return nullptr;}
    EXPECT_EQ(FileStatus::kOk,done.Get<0>());return done.Get<1>();
  }
  std::unique_ptr<PrefService> Prefs(scoped_refptr<ProfileFilterPreferences> owner){
    auto registry=base::MakeRefCounted<PrefRegistrySimple>();brave_shields::RegisterPrefsForAdBlockService(registry.get());
    PrefServiceFactory factory;factory.set_user_prefs(owner);return factory.Create(registry);
  }
  std::vector<FileStatus> errors;
};
TEST_F(ProfileFilterSubscriptionsTest, NativeSubscriptionPersistsPrivateBytesAndChoicesAcrossReopen){
  base::ScopedTempDir temp;ASSERT_TRUE(temp.CreateUniqueTempDir());auto path=temp.GetPath().AppendASCII("profile");
  std::shared_ptr<RecordFileSystem> files;ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(path,SubscriptionKey(),{},&files));
  std::shared_ptr<ProfileFilterStorage> storage;ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
  auto owner=Load(storage);ASSERT_TRUE(owner);auto prefs=Prefs(owner);
  const GURL url("https://private-subscription.invalid/list");const std::string text="||private-subscription-rule.invalid^$important\n";
  {
    auto backend=owner->SubscriptionBackend(base::BindLambdaForTesting([&](const GURL& input,bool ui,Provider::SourceReply reply)->base::OnceClosure{
      EXPECT_EQ(url,input);std::move(reply).Run(Result::kOk,DATFileDataBuffer(text.begin(),text.end()));return base::DoNothing();
    }),base::BindRepeating([](const GURL&){return GURL("chrome://adblock-source/fixture");}));
    brave_shields::AdBlockFiltersProviderManager providers;
    Manager manager(prefs.get(),&providers,backend,nullptr);ASSERT_TRUE(manager.CreateSubscription(url));task_environment()->RunUntilIdle();
    ASSERT_EQ(1u,manager.GetSubscriptions().size());EXPECT_FALSE(manager.GetSubscriptions()[0].last_successful_update_attempt.is_null());
    base::test::TestFuture<Result,DATFileDataBuffer> read;backend.source.Run(url).read.Run(read.GetCallback());task_environment()->RunUntilIdle();
    ASSERT_TRUE(read.IsReady());EXPECT_EQ(Result::kOk,read.Get<0>());EXPECT_EQ(text,std::string(read.Get<1>().begin(),read.Get<1>().end()));
  }
  bool closed=false;owner->Close(base::BindLambdaForTesting([&](FileStatus status){EXPECT_EQ(FileStatus::kOk,status);closed=true;}));task_environment()->RunUntilIdle();ASSERT_TRUE(closed);
  prefs.reset();owner.reset();storage.reset();ASSERT_EQ(FileStatus::kOk,files->Lock());files.reset();
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(path,SubscriptionKey(),&files));
  ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));owner=Load(storage);ASSERT_TRUE(owner);prefs=Prefs(owner);
  auto backend=owner->SubscriptionBackend(base::BindRepeating([](const GURL&,bool,Provider::SourceReply)->base::OnceClosure{ADD_FAILURE()<<"unexpected startup fetch";return base::DoNothing();}),base::BindRepeating([](const GURL&){return GURL("chrome://adblock-source/fixture");}));
  brave_shields::AdBlockFiltersProviderManager providers;Manager reopened(prefs.get(),&providers,backend,nullptr);
  ASSERT_EQ(1u,reopened.GetSubscriptions().size());EXPECT_EQ(url,reopened.GetSubscriptions()[0].subscription_url);
  base::test::TestFuture<Result,DATFileDataBuffer> read;backend.source.Run(url).read.Run(read.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(read.IsReady());EXPECT_EQ(Result::kOk,read.Get<0>());EXPECT_EQ(text,std::string(read.Get<1>().begin(),read.Get<1>().end()));EXPECT_TRUE(errors.empty());
}
TEST_F(ProfileFilterSubscriptionsTest, SupersededAndDeletedDownloadsCannotPublishOldBytes){
  RecordCapabilityIssuer issuer(base::UnguessableToken::Create(),base::UnguessableToken::Create(),base::UnguessableToken::Create(),StorageMode::kMemory);
  std::shared_ptr<ProfileFilterStorage> storage;ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(nullptr,issuer.Issue(StorePurpose::kProfileCustomFilters),&storage));
  auto owner=Load(storage);ASSERT_TRUE(owner);auto prefs=Prefs(owner);std::vector<Provider::SourceReply> pending;
  auto backend=owner->SubscriptionBackend(base::BindLambdaForTesting([&](const GURL&,bool,Provider::SourceReply done)->base::OnceClosure{pending.push_back(std::move(done));return base::DoNothing();}),base::BindRepeating([](const GURL&){return GURL("chrome://adblock-source/fixture");}));
  brave_shields::AdBlockFiltersProviderManager providers;Manager manager(prefs.get(),&providers,backend,nullptr);const GURL url("https://old-source.invalid/list");
  ASSERT_TRUE(manager.CreateSubscription(url));ASSERT_TRUE(manager.RefreshSubscription(url,true));ASSERT_EQ(2u,pending.size());
  std::move(pending[1]).Run(Result::kOk,DATFileDataBuffer({'n','e','w'}));task_environment()->RunUntilIdle();
  std::move(pending[0]).Run(Result::kOk,DATFileDataBuffer({'o','l','d'}));task_environment()->RunUntilIdle();
  base::test::TestFuture<Result,DATFileDataBuffer> read;backend.source.Run(url).read.Run(read.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(read.IsReady());EXPECT_EQ(Result::kOk,read.Get<0>());EXPECT_EQ(DATFileDataBuffer({'n','e','w'}),read.Get<1>());
  ASSERT_TRUE(manager.RefreshSubscription(url,true));ASSERT_TRUE(manager.DeleteSubscription(url));ASSERT_TRUE(manager.CreateSubscription(url));ASSERT_EQ(4u,pending.size());
  std::move(pending[2]).Run(Result::kOk,DATFileDataBuffer({'s','t','a','l','e'}));task_environment()->RunUntilIdle();
  base::test::TestFuture<Result,DATFileDataBuffer> missing;backend.source.Run(url).read.Run(missing.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(missing.IsReady());EXPECT_EQ(Result::kMissing,missing.Get<0>());EXPECT_TRUE(missing.Get<1>().empty());
  std::move(pending[3]).Run(Result::kOk,DATFileDataBuffer({'f','r','e','s','h'}));task_environment()->RunUntilIdle();EXPECT_TRUE(errors.empty());
}
}  // namespace
}  // namespace alloy::site_records
