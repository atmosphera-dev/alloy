// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_leveldb_env.h"

#include <unistd.h>
#include <array>
#include <string>

#include "base/alloy/buildflags.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "base/test/task_environment.h"
#include "base/test/multiprocess_test.h"
#include "base/time/time.h"
#include "leveldb/db.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr auto kPurpose = StorePurpose::kIndexedDb;
constexpr std::string_view kMarker = "LEVELDB_CIPHER_MARKER_6f48a872182";
SecretKey EnvTestKey() {
  ObjectId bytes;
  bytes.fill(0x68);
  return SecretKey(bytes);
}
}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyLevelDbCrashChild) {
  base::test::TaskEnvironment tasks;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  std::shared_ptr<RecordFileSystem> files;
  if (RecordFileSystem::Open(path, EnvTestKey(), &files) != FileStatus::kOk) return 1;
  const GURL url("https://leveldb.example/");
  RecordInfo record;
  if (files->ResolveRecord(url, url::Origin::Create(url), false, &record) != FileStatus::kOk) return 2;
  std::unique_ptr<RecordLevelDbEnv> env;
  if (RecordLevelDbEnv::Create(files, files->Capability(record.id, kPurpose), kPurpose, &env) != FileStatus::kOk) return 3;
  auto options = env->Options();
  options.create_if_missing = true;
  std::unique_ptr<leveldb::DB> db;
  if (!leveldb_env::OpenDB(options, "/crash-db", &db).ok()) return 4;
  leveldb::WriteOptions write;
  write.sync = true;
  const auto status = db->Put(write, "key", leveldb::Slice(kMarker.data(), kMarker.size()));
  _exit(status.ok() ? 0 : 5);  // No Close/profile Lock; numeric WAL must recover.
}

namespace {
class RecordLevelDbEnvTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, EnvTestKey(), {}, &files_));
    Resolve();
    ASSERT_EQ(FileStatus::kOk, RecordLevelDbEnv::Create(files_, cap_, kPurpose, &env_));
  }
  void Resolve() {
    const GURL url("https://leveldb.example/");
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &record_));
    cap_ = files_->Capability(record_.id, kPurpose);
  }
  void Reopen() {
    env_.reset();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, EnvTestKey(), &files_));
    Resolve();
    ASSERT_EQ(FileStatus::kOk, RecordLevelDbEnv::Create(files_, cap_, kPurpose, &env_));
  }
  void NoPlaintext() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-i", "-l"}) command.AppendArg(arg);
    command.AppendArg(std::string(kMarker));
    command.AppendArgPath(path_);
    std::string output;
    int exit = -1;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  RecordInfo record_;
  RecordCapability cap_;
  std::shared_ptr<RecordFileSystem> files_;
  std::unique_ptr<RecordLevelDbEnv> env_;
};

TEST_F(RecordLevelDbEnvTest, RealDatabaseCompactionRewriteAndColdReopen) {
  auto options = env_->Options();
  options.create_if_missing = true;
  options.paranoid_checks = true;
  options.write_buffer_size = 65536;
  std::unique_ptr<leveldb::DB> db;
  ASSERT_TRUE(leveldb_env::OpenDB(options, "/database", &db).ok());
  leveldb::WriteOptions write;
  write.sync = true;
  for (int i = 0; i < 80; ++i) {
    const auto value = std::string(kMarker) + std::string(2000, 'a' + i % 26);
    ASSERT_TRUE(db->Put(write, "key-" + std::to_string(i), value).ok());
  }
  db->CompactRange(nullptr, nullptr);
  ASSERT_TRUE(leveldb_env::RewriteDB(options, "/database", &db).ok());
  db.reset();
  NoPlaintext();
  Reopen();
  options = env_->Options();
  options.paranoid_checks = true;
  ASSERT_TRUE(leveldb_env::OpenDB(options, "/database", &db).ok());
  for (int i = 0; i < 80; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get(leveldb::ReadOptions(), "key-" + std::to_string(i), &value).ok());
    EXPECT_EQ(std::string(kMarker) + std::string(2000, 'a' + i % 26), value);
  }
  db.reset();
}

TEST_F(RecordLevelDbEnvTest, NativeAppendRandomSequentialRenameAndLock) {
  ASSERT_TRUE(env_->CreateDir("/db").ok());
  leveldb::WritableFile* raw = nullptr;
  ASSERT_TRUE(env_->NewWritableFile("/db/CURRENT.tmp", &raw).ok());
  std::unique_ptr<leveldb::WritableFile> file(raw);
  ASSERT_TRUE(file->Append("MANIFEST-").ok());
  ASSERT_TRUE(file->Append("000001\n").ok());
  ASSERT_TRUE(file->Flush().ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  file.reset();
  ASSERT_TRUE(env_->RenameFile("/db/CURRENT.tmp", "/db/CURRENT").ok());
  ASSERT_TRUE(env_->NewAppendableFile("/db/CURRENT", &raw).ok());
  file.reset(raw);
  ASSERT_TRUE(file->Append("tail").ok());
  ASSERT_TRUE(file->Close().ok());
  file.reset();
  leveldb::SequentialFile* seq = nullptr;
  ASSERT_TRUE(env_->NewSequentialFile("/db/CURRENT", &seq).ok());
  std::unique_ptr<leveldb::SequentialFile> sequential(seq);
  ASSERT_TRUE(sequential->Skip(9).ok());
  std::array<char, 30> scratch{};
  leveldb::Slice result;
  ASSERT_TRUE(sequential->Read(scratch.size(), &result, scratch.data()).ok());
  EXPECT_EQ("000001\ntail", result.ToString());
  leveldb::RandomAccessFile* random = nullptr;
  ASSERT_TRUE(env_->NewRandomAccessFile("/db/CURRENT", &random).ok());
  std::unique_ptr<leveldb::RandomAccessFile> reader(random);
  ASSERT_TRUE(reader->Read(0, 9, &result, scratch.data()).ok());
  EXPECT_EQ("MANIFEST-", result.ToString());
  leveldb::FileLock* lock = nullptr;
  leveldb::FileLock* competing = nullptr;
  ASSERT_TRUE(env_->LockFile("/db/LOCK", &lock).ok());
  EXPECT_FALSE(env_->LockFile("/db/LOCK", &competing).ok());
  EXPECT_EQ(nullptr, competing);
  ASSERT_TRUE(env_->UnlockFile(lock).ok());
  ASSERT_TRUE(env_->LockFile("/db/LOCK", &competing).ok());
  ASSERT_TRUE(env_->UnlockFile(competing).ok());
}

TEST_F(RecordLevelDbEnvTest, PendingAppendIsVisibleAndWipedByCoordinator) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap_, kPurpose, "buffer", RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk, file->AppendBuffered(base::as_byte_span(kMarker)));
  RecordFileInfo info;
  ASSERT_EQ(FileStatus::kOk, file->GetInfo(&info));
  EXPECT_EQ(kMarker.size(), info.length);
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  EXPECT_TRUE(file->KeysClearedForTesting());
  EXPECT_EQ(FileStatus::kDenied, file->AppendBuffered(base::as_byte_span(kMarker)));
  Reopen();
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap_, kPurpose, "buffer", RecordOpen::kOpen, &file));
  std::vector<uint8_t> output(kMarker.size());
  size_t read;
  ASSERT_EQ(FileStatus::kOk, file->Read(0, output, &read));
  EXPECT_EQ(kMarker, std::string(output.begin(), output.end()));
}

TEST_F(RecordLevelDbEnvTest, DiagnosticPolicyAndFunctionalWalAreSeparate) {
  ASSERT_TRUE(env_->CreateDir("/db").ok());
  leveldb::Logger* logger = nullptr;
  ASSERT_TRUE(env_->NewLogger("/db/LOG", &logger).ok());
#if BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  ASSERT_NE(nullptr, logger);
  leveldb::Log(logger, "%s", kMarker.data());
#else
  EXPECT_EQ(nullptr, logger);
  EXPECT_FALSE(env_->FileExists("/db/LOG"));
#endif
  delete logger;
  leveldb::WritableFile* raw = nullptr;
  ASSERT_TRUE(env_->NewWritableFile("/db/000001.log", &raw).ok());
  std::unique_ptr<leveldb::WritableFile> wal(raw);
  ASSERT_TRUE(wal->Append(leveldb::Slice(kMarker.data(), kMarker.size())).ok());
  ASSERT_TRUE(wal->Close().ok());
  EXPECT_TRUE(env_->FileExists("/db/000001.log"));
  NoPlaintext();
}

TEST_F(RecordLevelDbEnvTest, TamperFailsIntegrityAndCannotCreateEmptyDatabase) {
  auto options = env_->Options();
  options.create_if_missing = true;
  std::unique_ptr<leveldb::DB> db;
  ASSERT_TRUE(leveldb_env::OpenDB(options, "/db", &db).ok());
  ASSERT_TRUE(db->Put(leveldb::WriteOptions(), "key", leveldb::Slice(kMarker.data(), kMarker.size())).ok());
  db.reset();
  base::FileEnumerator objects(path_.AppendASCII(ObjectName(record_.directory)), false, base::FileEnumerator::FILES);
  for (auto path = objects.Next(); !path.empty(); path = objects.Next()) {
    auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes && !bytes->empty());
    base::span(*bytes)[0] ^= 1;
    ASSERT_TRUE(base::WriteFile(path, *bytes));
  }
  const auto status = leveldb_env::OpenDB(options, "/db", &db);
  EXPECT_TRUE(status.IsIOError()) << status.ToString();
  EXPECT_EQ(FileStatus::kIntegrity, env_->last_status());
  EXPECT_FALSE(db);
  leveldb::WritableFile* rejected = nullptr;
  EXPECT_FALSE(env_->NewWritableFile("/db/CURRENT", &rejected).ok());
  EXPECT_EQ(nullptr, rejected);
}

TEST_F(RecordLevelDbEnvTest, SyncedNumericWalRecoversAfterProcessExit) {
  env_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyLevelDbCrashChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  ASSERT_EQ(0, exit);
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, EnvTestKey(), &files_));
  Resolve();
  ASSERT_EQ(FileStatus::kOk, RecordLevelDbEnv::Create(files_, cap_, kPurpose, &env_));
  std::unique_ptr<leveldb::DB> db;
  ASSERT_TRUE(leveldb_env::OpenDB(env_->Options(), "/crash-db", &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get(leveldb::ReadOptions(), "key", &value).ok());
  EXPECT_EQ(kMarker, value);
  db.reset();
  NoPlaintext();
}
}  // namespace
}  // namespace alloy::site_records
