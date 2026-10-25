// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/alloy/storage_policy.h"

#include <map>
#include <memory>
#include <string>

#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "brave/components/brave_shields/core/browser/ad_block_dat_cache_manager.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "components/safe_browsing/core/browser/db/hash_prefix_list.h"
#include "components/safe_browsing/core/browser/db/hash_prefix_map.h"
#include "components/safe_browsing/core/browser/db/v4_store.h"
#include "components/safe_browsing/core/browser/db/v4_store.pb.h"
#include "components/safe_browsing/core/browser/db/v5_store.h"
#include "components/safe_browsing/core/common/proto/v5_store.pb.h"
#include "components/value_store/value_store_factory_impl.h"
#include "crypto/hash.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/leveldatabase/env_chromium.h"
#include "third_party/leveldatabase/src/include/leveldb/db.h"

namespace alloy::site_records {
namespace {
namespace sb = safe_browsing;

std::string Checksum(std::string_view bytes) {
  const auto hash = crypto::hash::Sha256(base::as_byte_span(bytes));
  return std::string(hash.begin(), hash.end());
}

std::map<base::FilePath, std::string> Contents(const base::FilePath& root) {
  std::map<base::FilePath, std::string> contents;
  base::FileEnumerator files(root, true, base::FileEnumerator::FILES);
  for (auto path = files.Next(); !path.empty(); path = files.Next()) {
    std::string bytes;
    EXPECT_TRUE(base::ReadFileToString(path, &bytes));
    contents.emplace(path, std::move(bytes));
  }
  return contents;
}

class NoDiskNativeOwnerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(base::alloy::IsNoDiskProcess());
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
  }

  auto runner() { return base::SequencedTaskRunner::GetCurrentDefault(); }

  sb::SBStorePtr Apply(sb::SBStore& store,
                       std::unique_ptr<sb::SBUpdateResponse> response) {
    base::RunLoop loop;
    sb::SBStorePtr result(nullptr, sb::SBStoreDeleter(runner()));
    store.ApplyUpdate(std::move(response), runner(),
                      base::BindOnce(
                          [](sb::SBStorePtr* output, base::OnceClosure quit,
                             sb::SBStorePtr value) {
                            *output = std::move(value);
                            std::move(quit).Run();
                          },
                          base::Unretained(&result), loop.QuitClosure()));
    loop.Run();
    return result;
  }

  std::unique_ptr<sb::SBUpdateResponse> V4(std::string additions,
                                           std::string expected,
                                           bool partial = false) {
    auto response = std::make_unique<sb::SBUpdateResponse>();
    response->v4_response = std::make_unique<sb::ListUpdateResponse>();
    auto& value = *response->v4_response;
    value.set_response_type(partial ? sb::ListUpdateResponse::PARTIAL_UPDATE
                                    : sb::ListUpdateResponse::FULL_UPDATE);
    value.set_new_client_state("resident-v4");
    value.mutable_checksum()->set_sha256(Checksum(expected));
    if (!additions.empty()) {
      auto* hashes = value.add_additions();
      hashes->set_compression_type(sb::RAW);
      hashes->mutable_raw_hashes()->set_prefix_size(4);
      hashes->mutable_raw_hashes()->set_raw_hashes(additions);
    }
    return response;
  }

  std::unique_ptr<sb::SBUpdateResponse> V5(uint32_t addition,
                                           std::string expected,
                                           bool partial = false) {
    auto response = std::make_unique<sb::SBUpdateResponse>();
    response->v5_response = std::make_unique<sb::V5::HashList>();
    auto& value = *response->v5_response;
    value.set_partial_update(partial);
    value.set_version("resident-v5");
    value.set_sha256_checksum(Checksum(expected));
    if (addition) {
      auto* rice = value.mutable_additions_four_bytes();
      rice->set_entries_count(0);
      rice->set_first_value(addition);
      rice->set_rice_parameter(3);
    }
    return response;
  }

  void ExpectLookup(sb::SBStore& store, std::string prefix, bool matches) {
    EXPECT_EQ(store.GetMatchingHashPrefix(prefix + std::string(28, 'x')),
              matches ? prefix : std::string());
    EXPECT_TRUE(store.GetPathsInUse().empty());
    EXPECT_EQ(store.file_size(), 0);
  }

  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir directory_;
};

TEST_F(NoDiskNativeOwnerTest, NativeContainersFinalizeResidentPrefixes) {
  const auto anchor = directory_.GetPath().AppendASCII("absent/store");
  {
    sb::HashPrefixMap map(anchor);
    map.Append(4, "abcdefgh");
    sb::V4StoreFileFormat proto;
    sb::SBStoreFileFormat format(&proto);
    ASSERT_TRUE(map.WriteToDisk(format));
    EXPECT_EQ(map.GetMatchingHashPrefix("abcd" + std::string(28, 'x')), "abcd");
    EXPECT_EQ(map.GetMatchingHashPrefix("zzzz" + std::string(28, 'x')), "");
    EXPECT_TRUE(map.GetPaths().empty());
    map.Clear();
    EXPECT_TRUE(map.view().empty());
  }
  {
    sb::HashPrefixList list(anchor, 4);
    list.Append(4, "abcdefgh");
    ::V5StoreFileFormat proto;
    sb::SBStoreFileFormat format(&proto);
    ASSERT_TRUE(list.WriteToDisk(format));
    EXPECT_EQ(list.GetRawView(), "abcdefgh");
    EXPECT_EQ(list.GetMatchingHashPrefix("efgh" + std::string(28, 'x')),
              "efgh");
    EXPECT_TRUE(list.GetPaths().empty());
    list.Clear();
    EXPECT_TRUE(list.GetRawView().empty());
  }
  {
    sb::HashPrefixList invalid(anchor, 4);
    invalid.Append(4, "bad");
    ::V5StoreFileFormat proto;
    sb::SBStoreFileFormat format(&proto);
    EXPECT_FALSE(invalid.WriteToDisk(format));
  }
  EXPECT_TRUE(base::IsDirectoryEmpty(directory_.GetPath()));
}

TEST_F(NoDiskNativeOwnerTest, NativeV4UpdateChecksumAndLifetime) {
  const auto path = directory_.GetPath().AppendASCII("absent/v4.store");
  sb::V4Store initial(runner(), path, 4, true, false);
  initial.Initialize();
  EXPECT_FALSE(initial.HasValidData());
  auto first = Apply(initial, V4("abcd", "abcd"));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->VerifyChecksum());
  ExpectLookup(*first, "abcd", true);
  auto next = Apply(*first, V4("efgh", "abcdefgh", true));
  ASSERT_TRUE(next);
  EXPECT_TRUE(next->VerifyChecksum());
  ExpectLookup(*next, "efgh", true);
  ExpectLookup(*first, "efgh", false);
  auto invalid = V4("ijkl", "wrong-checksum", true);
  EXPECT_FALSE(Apply(*next, std::move(invalid)));
  ExpectLookup(*next, "abcd", true);
  auto cleared = Apply(*next, V4("", ""));
  ASSERT_TRUE(cleared);
  ExpectLookup(*cleared, "abcd", false);
  sb::V4Store fresh(runner(), path, 4, true, false);
  fresh.Initialize();
  EXPECT_FALSE(fresh.HasValidData());
  ExpectLookup(fresh, "abcd", false);
  EXPECT_TRUE(base::IsDirectoryEmpty(directory_.GetPath()));
}

TEST_F(NoDiskNativeOwnerTest, NativeV5UpdateChecksumAndLifetime) {
  const auto path = directory_.GetPath().AppendASCII("absent/v5.store");
  sb::V5Store initial(runner(), path, 4, path.AddExtensionASCII("v4"), true,
                      false);
  initial.Initialize();
  EXPECT_FALSE(initial.HasValidData());
  auto first = Apply(initial, V5(0x61626364, "abcd"));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->VerifyChecksum());
  ExpectLookup(*first, "abcd", true);
  auto next = Apply(*first, V5(0x65666768, "abcdefgh", true));
  ASSERT_TRUE(next);
  EXPECT_TRUE(next->VerifyChecksum());
  ExpectLookup(*next, "efgh", true);
  ExpectLookup(*first, "efgh", false);
  EXPECT_FALSE(Apply(*next, V5(0x696a6b6c, "wrong-checksum", true)));
  ExpectLookup(*next, "abcd", true);
  auto cleared = Apply(*next, V5(0, ""));
  ASSERT_TRUE(cleared);
  ExpectLookup(*cleared, "abcd", false);
  sb::V5Store fresh(runner(), path, 4, path.AddExtensionASCII("v4"), true,
                    false);
  fresh.Initialize();
  EXPECT_FALSE(fresh.HasValidData());
  ExpectLookup(fresh, "abcd", false);
  EXPECT_TRUE(base::IsDirectoryEmpty(directory_.GetPath()));
}

TEST_F(NoDiskNativeOwnerTest, ValidParentStoresCannotSeedNativeOwners) {
  const auto v4 = directory_.GetPath().AppendASCII("parent-v4.store");
  const auto v5 = directory_.GetPath().AppendASCII("parent-v5.store");
  sb::V4StoreFileFormat old_v4;
  old_v4.set_magic_number(0x600d71fe);
  old_v4.set_version_number(9);
  auto* response = old_v4.mutable_list_update_response();
  response->set_response_type(sb::ListUpdateResponse::FULL_UPDATE);
  response->set_new_client_state("parent-v4-state");
  response->mutable_checksum()->set_sha256(Checksum("leak"));
  auto* hash = old_v4.add_hash_files();
  hash->set_prefix_size(4);
  hash->set_file_size(4);
  hash->set_extension("seed");
  ::V5StoreFileFormat old_v5;
  old_v5.set_magic_number(0x600d71fe);
  old_v5.set_file_version(10);
  auto* details = old_v5.mutable_list_details();
  details->set_version("parent-v5-state");
  details->mutable_checksum()->set_sha256(Checksum("leak"));
  details->mutable_hash_file()->set_file_size(4);
  details->mutable_hash_file()->set_extension("seed");
  ASSERT_TRUE(base::WriteFile(v4, old_v4.SerializeAsString()));
  ASSERT_TRUE(base::WriteFile(v5, old_v5.SerializeAsString()));
  ASSERT_TRUE(base::WriteFile(v4.AddExtensionASCII("seed"), "leak"));
  ASSERT_TRUE(base::WriteFile(v5.AddExtensionASCII("seed"), "leak"));
  const auto before = Contents(directory_.GetPath());
  {
    sb::V4Store child_v4(runner(), v4, 4, true, false);
    sb::V5Store child_v5(runner(), v5, 4, v4, true, false);
    child_v4.Initialize();
    child_v5.Initialize();
    EXPECT_FALSE(child_v4.HasValidData());
    EXPECT_FALSE(child_v5.HasValidData());
    EXPECT_TRUE(child_v4.GetStoreState().empty());
    EXPECT_TRUE(child_v5.GetStoreState().empty());
    ExpectLookup(child_v4, "leak", false);
    ExpectLookup(child_v5, "leak", false);
    sb::SBStore::CleanupExtraFiles(v4, {});
  }
  EXPECT_EQ(Contents(directory_.GetPath()), before);
}

TEST_F(NoDiskNativeOwnerTest, NativeValueStoresShareOnlyTheirFactoryMemory) {
  const auto name = base::FilePath(FILE_PATH_LITERAL("Resources"));
  const auto root = directory_.GetPath();
  {
    leveldb_env::Options options;
    options.create_if_missing = true;
    std::unique_ptr<leveldb::DB> seed;
    ASSERT_TRUE(
        leveldb_env::OpenDB(options, root.Append(name).AsUTF8Unsafe(), &seed)
            .ok());
    ASSERT_TRUE(
        seed->Put(leveldb::WriteOptions(), "key", "\"parent-seed\"").ok());
  }
  const auto before = Contents(root);
  auto factory = base::MakeRefCounted<value_store::ValueStoreFactoryImpl>(root);
  auto store = factory->CreateValueStore(name, "NoDiskOwnerTest");
  auto initial = store->Get("key");
  ASSERT_TRUE(initial.status().ok());
  EXPECT_TRUE(initial.settings().empty());
  ASSERT_TRUE(store
                  ->Set(value_store::ValueStore::DEFAULTS, "key",
                        base::Value("resident"))
                  .status()
                  .ok());
  store.reset();
  EXPECT_TRUE(factory->HasValueStore(name));
  auto reopened = factory->CreateValueStore(name, "NoDiskOwnerTest");
  auto read = reopened->Get("key");
  ASSERT_TRUE(read.status().ok());
  ASSERT_NE(read.settings().FindString("key"), nullptr);
  EXPECT_EQ(*read.settings().FindString("key"), "resident");
  {
    auto independent =
        base::MakeRefCounted<value_store::ValueStoreFactoryImpl>(root);
    auto isolated = independent->CreateValueStore(name, "NoDiskOwnerTest");
    auto result = isolated->Get("key");
    ASSERT_TRUE(result.status().ok());
    EXPECT_TRUE(result.settings().empty());
  }
  reopened.reset();
  factory->DeleteValueStore(name);
  EXPECT_FALSE(factory->HasValueStore(name));
  auto survivor = factory->CreateValueStore(name, "NoDiskOwnerTest");
  ASSERT_TRUE(
      survivor
          ->Set(value_store::ValueStore::DEFAULTS, "key", base::Value("lease"))
          .status()
          .ok());
  factory = nullptr;
  auto final = survivor->Get("key");
  ASSERT_TRUE(final.status().ok());
  ASSERT_NE(final.settings().FindString("key"), nullptr);
  EXPECT_EQ(*final.settings().FindString("key"), "lease");
  survivor.reset();
  EXPECT_EQ(Contents(root), before);
}

TEST_F(NoDiskNativeOwnerTest,
       NativeDATCacheMissNeverReadsWritesOrDeletesSeeds) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(brave_shields::features::kAdblockDATCache);
  const auto cache = directory_.GetPath().AppendASCII("adblock_cache");
  ASSERT_TRUE(base::CreateDirectory(cache));
  ASSERT_TRUE(
      base::WriteFile(cache.AppendASCII("engine0.dat"), "parent-default"));
  ASSERT_TRUE(
      base::WriteFile(cache.AppendASCII("engine1.dat"), "parent-additional"));
  const auto before = Contents(directory_.GetPath());
  brave_shields::AdBlockDATCacheManager manager(directory_.GetPath());
  manager.WriteDATFile(true, DATFileDataBuffer{1, 2, 3, 4});
  manager.DropDATCache();
  base::RunLoop loop;
  bool called = false;
  manager.MaybeReadCachedDATFiles(base::BindOnce(
      [](bool* called, base::OnceClosure quit,
         std::optional<DATFileDataBuffer> first,
         std::optional<DATFileDataBuffer> second) {
        *called = true;
        EXPECT_FALSE(first.has_value());
        EXPECT_FALSE(second.has_value());
        std::move(quit).Run();
      },
      base::Unretained(&called), loop.QuitClosure()));
  EXPECT_FALSE(called);
  loop.Run();
  EXPECT_TRUE(called);
  EXPECT_EQ(Contents(directory_.GetPath()), before);
}

}  // namespace
}  // namespace alloy::site_records
