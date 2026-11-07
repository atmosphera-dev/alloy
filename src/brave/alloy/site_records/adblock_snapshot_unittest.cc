// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_storage.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_engine.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"
namespace alloy::site_records {
namespace {
using Cache=brave_shields::AdBlockDATCacheManager;
using Epoch=brave_shields::AdBlockLoadEpoch;
class AdBlockSnapshotTest:public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();adblock::set_domain_resolver();
    ASSERT_TRUE(temp.CreateUniqueTempDir());ObjectId key{};key.fill(0x91);
    ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Create(temp.GetPath().AppendASCII("profile"),SecretKey(key),{},&files));
    Open();
  }
  void Open(){
    ASSERT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(files,files->ProfileCapability(StorePurpose::kProfileCustomFilters),&storage));
    cache=std::make_unique<Cache>(MakeProfileFilterDATBackend(storage,base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindLambdaForTesting([&](FileStatus s){errors.push_back(s);})));}
  std::string Identity(char configuration='a',char sources='b'){return std::string(32,configuration)+std::string(32,sources);}
  DATFileDataBuffer DAT(){
    brave_shields::AdBlockEngine engine(false);auto resources=adblock::new_empty_resource_storage();
    std::string text="||snapshot-private.invalid^$important";DATFileDataBuffer bytes(text.begin(),text.end());
    EXPECT_TRUE(engine.Load(false,bytes,*resources));return engine.Serialize();}
  std::optional<DATFileDataBuffer> Read(bool primary,const std::string& identity){
    base::test::TestFuture<std::optional<DATFileDataBuffer>> done;cache->ReadSnapshot(primary,identity,done.GetCallback());
    task_environment()->RunUntilIdle();if(!done.IsReady()){ADD_FAILURE();return {}; }return done.Take();}
  void TearDown() override {task_environment()->RunUntilIdle();cache.reset();storage.reset();files.reset();ChromeRenderViewHostTestHarness::TearDown();}
  base::ScopedTempDir temp;std::shared_ptr<RecordFileSystem> files;std::shared_ptr<ProfileFilterStorage> storage;
  std::unique_ptr<Cache> cache;std::vector<FileStatus> errors;
};
TEST_F(AdBlockSnapshotTest, ColdQualifiedCacheReusesOnlyMatchingConfigurationSourceAndEngineRole){
  auto dat=DAT();cache->WriteSnapshot(false,Identity(),dat,std::make_shared<Epoch>());task_environment()->RunUntilIdle();
  cache.reset();storage.reset();ASSERT_EQ(FileStatus::kOk,files->Lock());files.reset();ObjectId key{};key.fill(0x91);
  ASSERT_EQ(FileStatus::kOk,RecordFileSystem::Open(temp.GetPath().AppendASCII("profile"),SecretKey(key),&files));Open();
  EXPECT_FALSE(Read(false,Identity('x','b')));EXPECT_FALSE(Read(false,Identity('a','x')));EXPECT_FALSE(Read(true,Identity()));
  auto restored=Read(false,Identity());ASSERT_TRUE(restored);EXPECT_EQ(dat,*restored);
  brave_shields::AdBlockEngine native(false);auto resources=adblock::new_empty_resource_storage();ASSERT_TRUE(native.Load(true,*restored,*resources));
  EXPECT_TRUE(native.ShouldStartRequest(GURL("https://snapshot-private.invalid/image.png"),blink::mojom::ResourceType::kImage,
      url::Origin::Create(GURL("https://page.invalid")),"GET",false,false,false).matched);
  EXPECT_FALSE(native.ShouldStartRequest(GURL("https://unrelated.invalid/image.png"),blink::mojom::ResourceType::kImage,
      url::Origin::Create(GURL("https://page.invalid")),"GET",false,false,false).matched);EXPECT_TRUE(errors.empty());
}
TEST_F(AdBlockSnapshotTest, UnqualifiedBytesCannotEnterQualifiedReadAndDropPreservesConfiguration){
  base::test::ScopedFeatureList features;features.InitAndEnableFeature(brave_shields::features::kAdblockDATCache);
  cache->WriteDATFile(false,DAT());task_environment()->RunUntilIdle();EXPECT_FALSE(Read(false,Identity()));
  const DATFileDataBuffer configuration{1,2,3};ASSERT_EQ(FileStatus::kOk,storage->Write("configuration",configuration));
  cache->WriteSnapshot(false,Identity(),DAT(),std::make_shared<Epoch>());task_environment()->RunUntilIdle();ASSERT_TRUE(Read(false,Identity()));
  cache->DropDATCache();task_environment()->RunUntilIdle();EXPECT_FALSE(Read(false,Identity()));
  DATFileDataBuffer bytes;ASSERT_EQ(FileStatus::kOk,storage->Read("configuration",&bytes));EXPECT_EQ(DATFileDataBuffer({1,2,3}),bytes);EXPECT_TRUE(errors.empty());
}
TEST_F(AdBlockSnapshotTest, RevokedQueuedSnapshotNeverRepliesWithCacheMissOrPublishes){
  bool replied=false;cache->WriteSnapshot(false,Identity(),DAT(),std::make_shared<Epoch>());
  cache->ReadSnapshot(false,Identity(),base::BindLambdaForTesting([&](std::optional<DATFileDataBuffer>){replied=true;}));
  ASSERT_EQ(FileStatus::kOk,files->Lock());task_environment()->RunUntilIdle();EXPECT_FALSE(replied);
  ASSERT_EQ(2u,errors.size());EXPECT_EQ(FileStatus::kDenied,errors[0]);EXPECT_EQ(FileStatus::kDenied,errors[1]);
}
TEST_F(AdBlockSnapshotTest, MalformedAuthenticatedEnvelopeIsAnErrorNotIdentityMismatch){
  const DATFileDataBuffer malformed{1,2,3};ASSERT_EQ(FileStatus::kOk,storage->Write("dat-snapshot-additional",malformed));bool replied=false;
  cache->ReadSnapshot(false,Identity(),base::BindLambdaForTesting([&](std::optional<DATFileDataBuffer>){replied=true;}));
  task_environment()->RunUntilIdle();EXPECT_FALSE(replied);ASSERT_EQ(1u,errors.size());EXPECT_EQ(FileStatus::kIntegrity,errors[0]);
}
TEST_F(AdBlockSnapshotTest, NativeParserFingerprintIncludesBytesPermissionsDebugAndFraming){
  auto fingerprint=[](bool debug,const std::vector<DATFileDataBuffer>& streams,uint8_t permissions){
    auto set=adblock::new_filter_set(debug);for(auto& bytes:streams)set->add_filter_list_with_permissions(bytes,permissions);
    auto bytes=set->source_identity();return std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size());};
  DATFileDataBuffer a{'a'},b{'b'},ab{'a','b'};
  EXPECT_EQ(fingerprint(false,{a,b},0),fingerprint(false,{a,b},0));
  EXPECT_NE(fingerprint(false,{a,b},0),fingerprint(false,{ab},0));
  EXPECT_NE(fingerprint(false,{a},0),fingerprint(false,{b},0));
  EXPECT_NE(fingerprint(false,{a},0),fingerprint(false,{a},255));
  EXPECT_NE(fingerprint(false,{a},0),fingerprint(true,{a},0));
}
}
}
