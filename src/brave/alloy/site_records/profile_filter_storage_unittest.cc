// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_storage.h"

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_engine.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"

namespace alloy::site_records {
namespace {
using Manager = brave_shields::AdBlockDATCacheManager;
constexpr auto kPurpose = StorePurpose::kProfileCustomFilters;
constexpr char kPrivateHost[] = "profile-cache-private.invalid";
constexpr char kConfiguration[] = "PRIVATE_FILTER_LIST_SELECTION_110";
SecretKey FilterKey() { ObjectId bytes{}; bytes.fill(0x71); return SecretKey(bytes); }

class ProfileFilterStorageTest : public ChromeRenderViewHostTestHarness {
 protected:
  ProfileFilterStorageTest() {
    features_.InitAndEnableFeature(brave_shields::features::kAdblockDATCache);
  }
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    adblock::set_domain_resolver();
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    root_ = temp_.GetPath().AppendASCII("private-filter-profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(root_, FilterKey(), {}, &files_));
    OpenStorage();
  }
  void TearDown() override {
    task_environment()->RunUntilIdle();
    store_.reset(); files_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }
  void OpenStorage() {
    ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files_,
        files_->ProfileCapability(kPurpose), &store_));
  }
  std::unique_ptr<Manager> Cache(std::shared_ptr<ProfileFilterStorage> store) {
    return std::make_unique<Manager>(MakeProfileFilterDATBackend(std::move(store),
        base::SequencedTaskRunner::GetCurrentDefault(), base::BindLambdaForTesting(
            [&](FileStatus status) { errors_.push_back(status); })));
  }
  std::pair<std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>> ReadCache(Manager& cache) {
    base::test::TestFuture<std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>> reply;
    cache.MaybeReadCachedDATFiles(reply.GetCallback());
    task_environment()->RunUntilIdle();
    if (!reply.IsReady()) { ADD_FAILURE() << "native cache did not return"; return {}; }
    auto [primary, additional] = reply.Take();
    return {std::move(primary), std::move(additional)};
  }
  DATFileDataBuffer PrivateDAT() {
    auto resources = adblock::new_empty_resource_storage();
    brave_shields::AdBlockEngine engine(false);
    const std::string text = std::string("||")+kPrivateHost+"^$important\n";
    DATFileDataBuffer rules(text.begin(), text.end());
    EXPECT_TRUE(engine.Load(false, rules, *resources));
    return engine.Serialize();
  }
  std::map<std::string, std::string> DiskImage() {
    std::map<std::string, std::string> result;
    base::FileEnumerator walk(temp_.GetPath(), true, base::FileEnumerator::FILES);
    for (auto path = walk.Next(); !path.empty(); path = walk.Next()) {
      std::string bytes;
      EXPECT_TRUE(base::ReadFileToString(path, &bytes));
      result.emplace(path.value(), std::move(bytes));
    }
    return result;
  }
  base::test::ScopedFeatureList features_;
  base::ScopedTempDir temp_;
  base::FilePath root_;
  std::shared_ptr<RecordFileSystem> files_;
  std::shared_ptr<ProfileFilterStorage> store_;
  std::vector<FileStatus> errors_;
};

TEST_F(ProfileFilterStorageTest, NativeCacheColdReadUsesOwnEncryptedProfile) {
  const auto dat = PrivateDAT(); ASSERT_FALSE(dat.empty());
  auto cache = Cache(store_);
  cache->WriteDATFile(false, dat);
  const std::string configuration(kConfiguration);
  ASSERT_EQ(FileStatus::kOk, store_->Write("configuration", base::as_byte_span(configuration)));
  task_environment()->RunUntilIdle(); ASSERT_TRUE(errors_.empty());
  auto initial = ReadCache(*cache);
  ASSERT_TRUE(initial.second); EXPECT_EQ(dat, *initial.second); EXPECT_FALSE(initial.first);
  const std::string encoded(reinterpret_cast<const char*>(dat.data()), dat.size());
  for (const auto& [path, bytes] : DiskImage()) {
    EXPECT_EQ(std::string::npos, bytes.find(kPrivateHost));
    EXPECT_EQ(std::string::npos, bytes.find(kConfiguration));
    EXPECT_EQ(std::string::npos, bytes.find(encoded));
    EXPECT_FALSE(path.ends_with(".dat"));
  }
  const auto original = files_->ProfileCapability(kPurpose);
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  ProfileFilterStorage::Bytes denied{9};
  EXPECT_EQ(FileStatus::kDenied, store_->Read("configuration", &denied));
  EXPECT_EQ(ProfileFilterStorage::Bytes({9}), denied);
  cache.reset(); store_.reset(); files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(root_, FilterKey(), &files_));
  std::shared_ptr<ProfileFilterStorage> stale;
  EXPECT_EQ(FileStatus::kDenied, ProfileFilterStorage::Create(files_, original, &stale));
  EXPECT_FALSE(stale); OpenStorage();
  cache = Cache(store_);
  auto restored = ReadCache(*cache);
  ASSERT_TRUE(restored.second); EXPECT_EQ(dat, *restored.second);
  auto resources = adblock::new_empty_resource_storage();
  brave_shields::AdBlockEngine native(false);
  ASSERT_TRUE(native.Load(true, *restored.second, *resources));
  EXPECT_TRUE(native.ShouldStartRequest(GURL(std::string("https://")+kPrivateHost+"/image.png"),
      blink::mojom::ResourceType::kImage, url::Origin::Create(GURL("https://owner.invalid")),
      "GET", false, false, false).matched);
  EXPECT_FALSE(native.ShouldStartRequest(GURL("https://unrelated.invalid/image.png"),
      blink::mojom::ResourceType::kImage, url::Origin::Create(GURL("https://owner.invalid")),
      "GET", false, false, false).matched);
  std::shared_ptr<RecordFileSystem> peer;
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(temp_.GetPath().AppendASCII("peer"), FilterKey(), {}, &peer));
  std::shared_ptr<ProfileFilterStorage> peer_store;
  EXPECT_EQ(FileStatus::kDenied, ProfileFilterStorage::Create(peer, files_->ProfileCapability(kPurpose), &peer_store));
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(peer, peer->ProfileCapability(kPurpose), &peer_store));
  auto peer_cache = Cache(peer_store);
  auto empty = ReadCache(*peer_cache);
  EXPECT_FALSE(empty.first); EXPECT_FALSE(empty.second); EXPECT_TRUE(errors_.empty());
}

TEST_F(ProfileFilterStorageTest, MemoryEditsAndStatelessRefusalDoNotTouchDisk) {
  const auto before = DiskImage();
  RecordCapabilityIssuer memory(base::UnguessableToken::Create(), base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kMemory);
  std::shared_ptr<ProfileFilterStorage> ram;
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(nullptr, memory.Issue(kPurpose), &ram));
  const ProfileFilterStorage::Bytes value{1, 2, 3};
  EXPECT_EQ(FileStatus::kOk, ram->Write("custom-resources", value));
  ProfileFilterStorage::Bytes read;
  EXPECT_EQ(FileStatus::kOk, ram->Read("custom-resources", &read)); EXPECT_EQ(value, read);
  ram->Close(); EXPECT_EQ(FileStatus::kDenied, ram->Read("custom-resources", &read));
  RecordCapabilityIssuer stateless(base::UnguessableToken::Create(), base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kStateless);
  std::shared_ptr<ProfileFilterStorage> strict;
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(nullptr, stateless.Issue(kPurpose), &strict));
  EXPECT_EQ(FileStatus::kDenied, strict->Write("configuration", value));
  EXPECT_EQ(FileStatus::kDenied, strict->Read("configuration", &read));
  std::shared_ptr<ProfileFilterStorage> refused;
  EXPECT_EQ(FileStatus::kDenied, ProfileFilterStorage::Create(nullptr, files_->ProfileCapability(kPurpose), &refused));
  EXPECT_FALSE(refused); EXPECT_EQ(before, DiskImage());
}

TEST_F(ProfileFilterStorageTest, DroppingNativeCachesPreservesPrivateConfiguration) {
  const ProfileFilterStorage::Bytes configuration{3, 4, 5};
  ASSERT_EQ(FileStatus::kOk, store_->Write("configuration", configuration));
  auto cache = Cache(store_);
  cache->WriteDATFile(true, PrivateDAT()); cache->WriteDATFile(false, PrivateDAT());
  task_environment()->RunUntilIdle();
  cache->DropDATCache(); task_environment()->RunUntilIdle();
  auto empty = ReadCache(*cache); EXPECT_FALSE(empty.first); EXPECT_FALSE(empty.second);
  ProfileFilterStorage::Bytes read;
  EXPECT_EQ(FileStatus::kOk, store_->Read("configuration", &read)); EXPECT_EQ(configuration, read);
  EXPECT_TRUE(errors_.empty());
}

TEST_F(ProfileFilterStorageTest, RevocationRefusesQueuedNativeCacheOperations) {
  auto cache = Cache(store_);
  cache->WriteDATFile(false, PrivateDAT());
  bool normal_reply = false;
  cache->MaybeReadCachedDATFiles(base::BindLambdaForTesting(
      [&](std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>) {
        normal_reply = true;
      }));
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(normal_reply); ASSERT_EQ(2u, errors_.size());
  EXPECT_EQ(FileStatus::kDenied, errors_[0]); EXPECT_EQ(FileStatus::kDenied, errors_[1]);
}

TEST_F(ProfileFilterStorageTest, AuthenticationFailureCannotBecomeNativeCacheMiss) {
  auto cache = Cache(store_); cache->WriteDATFile(false, PrivateDAT());
  task_environment()->RunUntilIdle(); ASSERT_TRUE(errors_.empty());
  size_t changed = 0;
  base::FileEnumerator walk(root_, true, base::FileEnumerator::FILES);
  for (auto path = walk.Next(); !path.empty(); path = walk.Next()) {
    if (path.BaseName().value() == "root") continue;
    std::string bytes; ASSERT_TRUE(base::ReadFileToString(path, &bytes)); ASSERT_FALSE(bytes.empty());
    bytes[0] ^= 1; ASSERT_TRUE(base::WriteFile(path, bytes)); ++changed;
  }
  ASSERT_GT(changed, 0u);
  bool normal_reply = false;
  cache->MaybeReadCachedDATFiles(base::BindLambdaForTesting(
      [&](std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>) {
        normal_reply = true;
      }));
  task_environment()->RunUntilIdle(); EXPECT_FALSE(normal_reply);
  ASSERT_EQ(1u, errors_.size()); EXPECT_EQ(FileStatus::kIntegrity, errors_[0]);
}

TEST_F(ProfileFilterStorageTest, FailedReplacementPreservesPreviousConfiguration) {
  const ProfileFilterStorage::Bytes old{1, 3, 5}, replacement{2, 4, 6};
  ASSERT_EQ(FileStatus::kOk, store_->Write("configuration", old));
  files_->SetFaultForTesting([](PublishFence, const base::FilePath&) { return FileStatus::kIo; });
  EXPECT_EQ(FileStatus::kIo, store_->Write("configuration", replacement));
  files_->SetFaultForTesting({});
  store_.reset(); files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(root_, FilterKey(), &files_)); OpenStorage();
  ProfileFilterStorage::Bytes read;
  EXPECT_EQ(FileStatus::kOk, store_->Read("configuration", &read)); EXPECT_EQ(old, read);
}
}  // namespace
}  // namespace alloy::site_records
