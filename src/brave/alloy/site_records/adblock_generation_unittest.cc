// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_storage.h"
#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_engine_wrapper.h"
#include "brave/components/brave_shields/core/browser/ad_block_custom_resource_provider.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"
namespace alloy::site_records {
namespace {
using Manager=brave_shields::AdBlockFiltersProviderManager;
using Epoch=brave_shields::AdBlockLoadEpoch;
using Wrapper=brave_shields::AdBlockEngineWrapper;
using Resources=brave_shields::AdBlockCustomResourceProvider;
class HeldSource:public brave_shields::AdBlockFiltersProvider {
 public:
  explicit HeldSource(Manager* manager):AdBlockFiltersProvider(false,manager) {}
  void LoadFilterSet(Manager::Loaded done) override {pending.push_back(std::move(done));}
  std::string GetNameForDebugging() override {return "HeldSource";}
  std::vector<Manager::Loaded> pending;
};
class PublicResources:public brave_shields::AdBlockResourceProvider {
 public:
  void LoadResources(base::OnceCallback<void(brave_shields::AdblockResourceStorageBox)> done) override {
    std::move(done).Run(adblock::new_empty_resource_storage());
  }
  void Changed(){NotifyResourcesLoaded(adblock::new_empty_resource_storage());}
};
base::Value ResourceList(const char* name) {
  base::DictValue value;value.Set("name",name);value.Set("content","YWxlcnQoMSk7");
  base::DictValue kind;kind.Set("mime","application/javascript");value.Set("kind",std::move(kind));
  base::ListValue list;list.Append(std::move(value));return base::Value(std::move(list));
}
std::unique_ptr<rust::Box<adblock::FilterSet>> Filters(const std::string& text) {
  auto filters=std::make_unique<rust::Box<adblock::FilterSet>>(adblock::new_filter_set(false));
  DATFileDataBuffer bytes(text.begin(),text.end());(*filters)->add_filter_list_with_permissions(bytes,UINT8_MAX);return filters;
}
bool Blocked(Wrapper& wrapper,const char* host) {
  return wrapper.ShouldStartRequest(GURL(std::string("https://")+host+"/image.png"),
      blink::mojom::ResourceType::kImage,url::Origin::Create(GURL("https://page.invalid")),
      "GET",true,false,false,false).matched;
}
class AdBlockGenerationTest:public ChromeRenderViewHostTestHarness {};
TEST_F(AdBlockGenerationTest, PendingAggregateReleasesPrivateContributionBeforeHeldPublicSourceReturns) {
  Manager manager;HeldSource fast(&manager),slow(&manager);
  base::test::TestFuture<Manager::Parse> result;manager.LoadFilterSetForEngine(false,result.GetCallback());
  task_environment()->RunUntilIdle();ASSERT_EQ(1u,fast.pending.size());ASSERT_EQ(1u,slow.pending.size());
  auto private_payload=std::make_shared<std::string>("private compile payload");std::weak_ptr<std::string> released=private_payload;
  std::move(fast.pending[0]).Run(base::BindOnce([](std::shared_ptr<std::string>,rust::Box<adblock::FilterSet>*){},private_payload));
  private_payload.reset();EXPECT_FALSE(released.expired());
  manager.OnChanged(false);EXPECT_TRUE(released.expired());
  std::move(slow.pending[0]).Run(base::DoNothing());EXPECT_FALSE(result.IsReady());
}
TEST_F(AdBlockGenerationTest, PreparedStaleEngineCannotReplaceLiveEngineButFreshCandidateCan) {
  adblock::set_domain_resolver();auto wrapper=Wrapper::Create();auto original=std::make_shared<Epoch>();
  auto initial=wrapper->PrepareLoad(false,Filters("||initial.invalid^$important"),adblock::new_empty_resource_storage(),true);
  auto published=wrapper->PublishLoad(false,std::move(initial),original);ASSERT_TRUE(published.current&&published.loaded);
  EXPECT_TRUE(Blocked(*wrapper,"initial.invalid"));
  auto stale=std::make_shared<Epoch>();auto candidate=wrapper->PrepareLoad(false,
      Filters("||stale.invalid^$important"),adblock::new_empty_resource_storage(),true);
  stale->Revoke();auto refused=wrapper->PublishLoad(false,std::move(candidate),stale);
  EXPECT_FALSE(refused.current);EXPECT_FALSE(refused.dat);EXPECT_TRUE(Blocked(*wrapper,"initial.invalid"));
  EXPECT_FALSE(Blocked(*wrapper,"stale.invalid"));
  auto fresh=std::make_shared<Epoch>();auto next=wrapper->PrepareLoad(false,
      Filters("||fresh.invalid^$important"),adblock::new_empty_resource_storage(),true);
  ASSERT_TRUE(wrapper->PublishLoad(false,std::move(next),fresh).current);
  EXPECT_TRUE(Blocked(*wrapper,"fresh.invalid"));EXPECT_FALSE(Blocked(*wrapper,"initial.invalid"));
}
TEST_F(AdBlockGenerationTest, LateResourceReadCannotOverwriteFreshCacheOrReplyWithOldResource) {
  base::RepeatingClosureList close;std::vector<Resources::ReadCallback> reads;
  Resources::Backend backend;backend.available=base::BindRepeating([]{return true;});backend.can_edit=backend.available;
  backend.read=base::BindLambdaForTesting([&](Resources::ReadCallback done){reads.push_back(std::move(done));});
  backend.write=base::BindRepeating([](base::Value,Resources::StatusCallback done){std::move(done).Run(Resources::ErrorCode::kOk);});
  backend.observe_close=base::BindLambdaForTesting([&](base::RepeatingClosure done){return close.Add(std::move(done));});
  auto base_resources=std::make_unique<PublicResources>();auto* public_source=base_resources.get();
  Resources provider(std::move(backend),std::move(base_resources));
  base::test::TestFuture<brave_shields::AdblockResourceStorageBox> first;
  provider.LoadResources(first.GetCallback());ASSERT_EQ(1u,reads.size());
  public_source->Changed();ASSERT_EQ(2u,reads.size());
  std::move(reads[1]).Run(Resources::ErrorCode::kOk,ResourceList("user-fresh.js"));
  std::move(reads[0]).Run(Resources::ErrorCode::kOk,ResourceList("user-stale.js"));
  task_environment()->RunUntilIdle();ASSERT_TRUE(first.IsReady());
  EXPECT_TRUE(adblock::has_resource_for_testing(*first.Get(),std::string("user-fresh.js")));
  EXPECT_FALSE(adblock::has_resource_for_testing(*first.Get(),std::string("user-stale.js")));
  base::test::TestFuture<brave_shields::AdblockResourceStorageBox> cached;provider.LoadResources(cached.GetCallback());
  task_environment()->RunUntilIdle();ASSERT_TRUE(cached.IsReady());EXPECT_EQ(2u,reads.size());
  EXPECT_TRUE(adblock::has_resource_for_testing(*cached.Get(),std::string("user-fresh.js")));
}
TEST_F(AdBlockGenerationTest, QueuedStaleDATWriteCannotPublishAndRevocationStillReportsError) {
  base::test::ScopedFeatureList features;features.InitAndEnableFeature(brave_shields::features::kAdblockDATCache);
  RecordCapabilityIssuer issuer(base::UnguessableToken::Create(),base::UnguessableToken::Create(),
      base::UnguessableToken::Create(),StorageMode::kMemory);
  std::shared_ptr<ProfileFilterStorage> storage;ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(
      nullptr,issuer.Issue(StorePurpose::kProfileCustomFilters),&storage));std::vector<FileStatus> errors;
  brave_shields::AdBlockDATCacheManager cache(MakeProfileFilterDATBackend(storage,
      base::SequencedTaskRunner::GetCurrentDefault(),base::BindLambdaForTesting([&](FileStatus s){errors.push_back(s);})));
  auto stale=std::make_shared<Epoch>();cache.WriteDATFile(false,{1,2,3},stale);stale->Revoke();
  task_environment()->RunUntilIdle();DATFileDataBuffer bytes;EXPECT_EQ(FileStatus::kNotFound,storage->Read("dat-additional",&bytes));
  EXPECT_TRUE(errors.empty());auto fresh=std::make_shared<Epoch>();cache.WriteDATFile(false,{4,5,6},fresh);
  task_environment()->RunUntilIdle();EXPECT_EQ(FileStatus::kOk,storage->Read("dat-additional",&bytes));
  EXPECT_EQ(DATFileDataBuffer({4,5,6}),bytes);issuer.Revoke();cache.WriteDATFile(false,{7,8,9},fresh);
  task_environment()->RunUntilIdle();ASSERT_EQ(1u,errors.size());EXPECT_EQ(FileStatus::kDenied,errors[0]);
}
}  // namespace
}  // namespace alloy::site_records
