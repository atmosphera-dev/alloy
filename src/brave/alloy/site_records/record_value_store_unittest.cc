// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_value_store.h"

#include <unistd.h>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/process/launch.h"
#include "base/strings/string_view_util.h"
#include "base/test/task_environment.h"
#include "base/test/multiprocess_test.h"
#include "brave/alloy/site_records/record_leveldb_env.h"
#include "leveldb/write_batch.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr auto kPurpose = StorePurpose::kSiteSettings;
constexpr std::string_view kMarker = "MANAGED_VALUE_CIPHER_4fe9c306875b";
SecretKey ValueStoreKey() {
  ObjectId bytes;
  bytes.fill(0x92);
  return SecretKey(bytes);
}

class RecordValueStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, ValueStoreKey(), {}, &files_));
    Resolve();
    ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  }
  void Resolve() {
    const GURL site("https://managed.alpha.test/");
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(site, url::Origin::Create(site), true, &record_));
    cap_ = files_->Capability(record_.id, kPurpose);
  }
  void Reopen() {
    store_.reset();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ValueStoreKey(), &files_));
    Resolve();
    ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  }
  void Expect(const std::string& key, const std::string& expected) {
    auto result = store_->Get(key);
    ASSERT_TRUE(result.status().ok()) << result.status().message;
    ASSERT_TRUE(result.settings().FindString(key));
    EXPECT_EQ(expected, *result.settings().FindString(key));
  }
  void Set(const std::string& key, std::string_view value) {
    auto result = store_->Set(value_store::ValueStore::DEFAULTS, key, base::Value(value));
    ASSERT_TRUE(result.status().ok()) << result.status().message;
  }
  void NoPlaintext() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-F", "-i", "-l",
                            "-e", "MANAGED_VALUE_CIPHER_4fe9c306875b", "-e", "alpha.test"}) {
      command.AppendArg(arg);
    }
    command.AppendArgPath(path_);
    int exit = -1;
    std::string output;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  base::FilePath database_{"/managed"};
  std::shared_ptr<RecordFileSystem> files_;
  RecordInfo record_;
  RecordCapability cap_;
  std::unique_ptr<value_store::LeveldbValueStore> store_;
};

TEST_F(RecordValueStoreTest, NativeChangesGetKeysRemoveClearAndColdReopen) {
  Set("policy", kMarker);
  base::DictValue values;
  values.Set("number", 42);
  base::DictValue nested;
  nested.Set("enabled", true);
  values.Set("nested", std::move(nested));
  auto changed = store_->Set(value_store::ValueStore::IGNORE_QUOTA, values);
  ASSERT_TRUE(changed.status().ok());
  EXPECT_EQ(2u, changed.changes().size());
  auto same = store_->Set(value_store::ValueStore::DEFAULTS, "policy", base::Value(kMarker));
  ASSERT_TRUE(same.status().ok());
  EXPECT_TRUE(same.changes().empty());
  auto keys = store_->GetKeys();
  ASSERT_TRUE(keys.status().ok());
  EXPECT_EQ(3u, keys.settings().size());
  Reopen();
  Expect("policy", std::string(kMarker));
  auto selected = store_->Get(std::vector<std::string>{"number", "nested", "absent"});
  ASSERT_TRUE(selected.status().ok());
  EXPECT_EQ(42, selected.settings().FindInt("number"));
  ASSERT_TRUE(selected.settings().FindDict("nested"));
  EXPECT_EQ(true, selected.settings().FindDict("nested")->FindBool("enabled"));
  auto removed = store_->Remove(std::vector<std::string>{"number", "nested"});
  ASSERT_TRUE(removed.status().ok());
  EXPECT_EQ(2u, removed.changes().size());
  NoPlaintext();
  auto cleared = store_->Clear();
  ASSERT_TRUE(cleared.status().ok()) << cleared.status().message;
  EXPECT_EQ(1u, cleared.changes().size());
  Reopen();
  auto empty = store_->Get();
  ASSERT_TRUE(empty.status().ok());
  EXPECT_TRUE(empty.settings().empty());
}

TEST_F(RecordValueStoreTest, LockDeniesCachedNativeReadsAndEveryWrite) {
  Set("policy", kMarker);
  Expect("policy", std::string(kMarker));
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  EXPECT_EQ(value_store::ValueStore::READ_ONLY, store_->Get("policy").status().code);
  EXPECT_EQ(value_store::ValueStore::READ_ONLY, store_->GetKeys().status().code);
  EXPECT_FALSE(store_->Set(value_store::ValueStore::DEFAULTS, "late", base::Value(1)).status().ok());
  EXPECT_FALSE(store_->Remove("policy").status().ok());
  EXPECT_FALSE(store_->Clear().status().ok());
}

TEST_F(RecordValueStoreTest, RecordDeletionDeniesWarmCacheAndOtherRecordIsIntact) {
  Set("policy", kMarker);
  Expect("policy", std::string(kMarker));
  const GURL second("https://managed.beta.test/");
  RecordInfo other;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(second, url::Origin::Create(second), true, &other));
  std::unique_ptr<value_store::LeveldbValueStore> live;
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, files_->Capability(other.id, kPurpose),
      kPurpose, database_, &live));
  ASSERT_TRUE(live->Set(value_store::ValueStore::DEFAULTS, "policy", base::Value("other")).status().ok());
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, files_->DeleteRecord(record_.id, [&] {
    EXPECT_EQ(value_store::ValueStore::READ_ONLY, store_->Get("policy").status().code);
    EXPECT_FALSE(store_->Set(value_store::ValueStore::DEFAULTS, "late", base::Value(1)).status().ok());
  }, &deletion));
  auto result = live->Get("policy");
  ASSERT_TRUE(result.status().ok());
  ASSERT_TRUE(result.settings().FindString("policy"));
  EXPECT_EQ("other", *result.settings().FindString("policy"));
  Resolve();
  std::unique_ptr<value_store::LeveldbValueStore> fresh;
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &fresh));
  auto empty = fresh->Get();
  ASSERT_TRUE(empty.status().ok());
  EXPECT_TRUE(empty.settings().empty());
  EXPECT_EQ(value_store::ValueStore::READ_ONLY, store_->Get("policy").status().code);
}

TEST_F(RecordValueStoreTest, MalformedJsonIsNotDeletedBySingleOrWholeStoreRead) {
  Set("healthy", kMarker);
  leveldb::WriteBatch poison;
  poison.Put("bad", "{invalid json");
  ASSERT_TRUE(store_->WriteToDbForTest(&poison));
  auto bad = store_->Get("bad");
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, bad.status().code);
  EXPECT_EQ(value_store::ValueStore::RESTORE_NONE, bad.status().restore_status);
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, store_->Get("healthy").status().code);
  Reopen();
  auto all = store_->Get();
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, all.status().code);
  EXPECT_EQ(value_store::ValueStore::RESTORE_NONE, all.status().restore_status);
  store_.reset();
  std::unique_ptr<RecordLevelDbEnv> env;
  ASSERT_EQ(FileStatus::kOk, RecordLevelDbEnv::Create(files_, cap_, kPurpose, &env));
  std::unique_ptr<leveldb::DB> raw;
  ASSERT_TRUE(leveldb_env::OpenDB(env->Options(), database_.value(), &raw).ok());
  std::string value;
  ASSERT_TRUE(raw->Get(leveldb::ReadOptions(), "bad", &value).ok());
  EXPECT_EQ("{invalid json", value);
  ASSERT_TRUE(raw->Get(leveldb::ReadOptions(), "healthy", &value).ok());
  const auto parsed = base::JSONReader::Read(value, base::JSON_PARSE_RFC);
  ASSERT_TRUE(parsed && parsed->is_string());
  EXPECT_EQ(kMarker, parsed->GetString());
}

TEST_F(RecordValueStoreTest, NativeOpenCorruptionCannotRepairOrRazeAuthenticatedDatabase) {
  Set("healthy", kMarker);
  store_.reset();
  constexpr std::string_view invalid = "not a native CURRENT file";
  std::shared_ptr<RecordFile> current;
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap_, kPurpose, "managed/CURRENT",
      RecordOpen::kCreateOrTruncate, &current));
  ASSERT_EQ(FileStatus::kOk, current->Write(0, base::as_byte_span(invalid)));
  ASSERT_EQ(FileStatus::kOk, current->Close());
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  auto result = store_->Get();
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, result.status().code);
  EXPECT_EQ(value_store::ValueStore::RESTORE_NONE, result.status().restore_status);
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, store_->GetKeys().status().code);
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap_, kPurpose, "managed/CURRENT", RecordOpen::kOpen, &current));
  std::vector<uint8_t> bytes(invalid.size());
  size_t read = 0;
  ASSERT_EQ(FileStatus::kOk, current->Read(0, bytes, &read));
  EXPECT_EQ(invalid, base::as_string_view(bytes));
}

TEST_F(RecordValueStoreTest, CiphertextTamperCannotReachPhysicalBackendOrEmptyFallback) {
  store_.reset();
  const auto physical = temp_.GetPath().AppendASCII("physical-canary");
  ASSERT_TRUE(base::CreateDirectory(physical));
  const auto canary = physical.AppendASCII("canary");
  ASSERT_TRUE(base::WriteFile(canary, "must remain untouched"));
  const auto parent = physical.DirName().value().substr(1);
  ASSERT_EQ(FileStatus::kOk, files_->CreateDirectory(cap_, kPurpose, parent, true));
  database_ = physical;
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  Set("policy", kMarker);
  store_.reset();
  base::FileEnumerator objects(path_.AppendASCII(ObjectName(record_.directory)), false,
      base::FileEnumerator::FILES);
  for (auto path = objects.Next(); !path.empty(); path = objects.Next()) {
    auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes && !bytes->empty());
    base::span(*bytes)[0] ^= 1;
    ASSERT_TRUE(base::WriteFile(path, *bytes));
  }
  // Construction may itself hit the corrupted namespace; neither outcome is
  // allowed to select a real filesystem path or return a successful empty DB.
  const auto status = CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_);
  if (status == FileStatus::kOk) EXPECT_FALSE(store_->Get().status().ok());
  else EXPECT_EQ(FileStatus::kIntegrity, status);
  EXPECT_FALSE(files_->unlocked());
  std::string saved;
  ASSERT_TRUE(base::ReadFileToString(canary, &saved));
  EXPECT_EQ("must remain untouched", saved);
  base::FileEnumerator children(physical, false, base::FileEnumerator::FILES);
  size_t count = 0;
  while (!children.Next().empty()) ++count;
  EXPECT_EQ(1u, count);
}

TEST_F(RecordValueStoreTest, FailedNativeClearReturnsErrorAndKeepsCommittedValues) {
  Set("policy", kMarker);
  // Enter publication after staging, rather than sometimes rejecting a COW
  // object before any live roots changed. The authoritative old root survives.
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  auto result = store_->Clear();
  EXPECT_FALSE(result.status().ok());
  EXPECT_FALSE(files_->unlocked());
  EXPECT_TRUE(files_->KeysClearedForTesting());
  files_->SetFaultForTesting({});
  store_.reset();
  EXPECT_EQ(FileStatus::kIo, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ValueStoreKey(), &files_));
  Resolve();
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  Expect("policy", std::string(kMarker));
}

TEST_F(RecordValueStoreTest, NoDiskAndWrongPurposeCapabilitiesCannotConstructBackend) {
  RecordCapabilityIssuer memory(files_->profile_id(), record_.id,
      base::UnguessableToken::Create(), StorageMode::kMemory);
  std::unique_ptr<value_store::LeveldbValueStore> rejected;
  EXPECT_EQ(FileStatus::kDenied, CreateRecordValueStore(files_, memory.Issue(kPurpose), kPurpose, database_, &rejected));
  EXPECT_FALSE(rejected);
  EXPECT_EQ(FileStatus::kDenied, CreateRecordValueStore(files_, cap_, StorePurpose::kCookies, database_, &rejected));
  EXPECT_FALSE(rejected);
  EXPECT_EQ(FileStatus::kInvalid, CreateRecordValueStore(files_, cap_, kPurpose, base::FilePath("../escape"), &rejected));
  EXPECT_FALSE(rejected);
}

TEST_F(RecordValueStoreTest, AcknowledgedNativeWritesAndDeletesSurviveActualProcessExit) {
  Set("remove", "before");
  Set("direct", "before");
  store_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyValueStoreWriteChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  ASSERT_EQ(0, exit);
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ValueStoreKey(), &files_));
  Resolve();
  ASSERT_EQ(FileStatus::kOk, CreateRecordValueStore(files_, cap_, kPurpose, database_, &store_));
  Expect("kept", std::string(kMarker));
  auto removed = store_->Get(std::vector<std::string>{"remove", "direct"});
  ASSERT_TRUE(removed.status().ok());
  EXPECT_TRUE(removed.settings().empty());
  NoPlaintext();
}

TEST_F(RecordValueStoreTest, OrdinaryNativeConstructorRetainsItsExistingRecoveryContract) {
  // Positive counterpart: strict recovery policy belongs to the injected
  // authenticated backend, not a global change to native ValueStore semantics.
  value_store::LeveldbValueStore ordinary("AlloyNativeControl", temp_.GetPath().AppendASCII("ordinary"));
  ASSERT_TRUE(ordinary.Set(value_store::ValueStore::DEFAULTS, "healthy", base::Value(kMarker)).status().ok());
  leveldb::WriteBatch poison;
  poison.Put("bad", "{invalid json");
  ASSERT_TRUE(ordinary.WriteToDbForTest(&poison));
  auto repaired = ordinary.Get("bad");
  EXPECT_EQ(value_store::ValueStore::CORRUPTION, repaired.status().code);
  EXPECT_EQ(value_store::ValueStore::VALUE_RESTORE_DELETE_SUCCESS, repaired.status().restore_status);
  auto absent = ordinary.Get("bad");
  ASSERT_TRUE(absent.status().ok());
  EXPECT_TRUE(absent.settings().empty());
  auto healthy = ordinary.Get("healthy");
  ASSERT_TRUE(healthy.status().ok());
  ASSERT_TRUE(healthy.settings().FindString("healthy"));
  EXPECT_EQ(kMarker, *healthy.settings().FindString("healthy"));
  EXPECT_TRUE(ordinary.Clear().status().ok());
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyValueStoreWriteChild) {
  base::test::TaskEnvironment tasks;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  std::shared_ptr<RecordFileSystem> files;
  if (RecordFileSystem::Open(path, ValueStoreKey(), &files) != FileStatus::kOk) return 1;
  const GURL site("https://managed.alpha.test/");
  RecordInfo record;
  if (files->ResolveRecord(site, url::Origin::Create(site), false, &record) != FileStatus::kOk) return 2;
  std::unique_ptr<value_store::LeveldbValueStore> store;
  if (CreateRecordValueStore(files, files->Capability(record.id, kPurpose), kPurpose,
      base::FilePath("/managed"), &store) != FileStatus::kOk) return 3;
  if (!store->Set(value_store::ValueStore::DEFAULTS, "kept", base::Value(kMarker)).status().ok()) return 4;
  if (!store->Remove("remove").status().ok()) return 5;
  if (!store->Delete("direct").ok()) return 6;
  _exit(0);  // No ValueStore/DB/Env destructors or profile Lock may flush it.
}

}  // namespace alloy::site_records
