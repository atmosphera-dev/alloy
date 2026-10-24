// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_cache_files.h"

#include <cstring>
#include <string>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/base/io_buffer.h"
#include "net/base/test_completion_callback.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr auto kPurpose = StorePurpose::kHttpCache;
constexpr std::string_view kMarker = "CACHE_CIPHER_MARKER_9736a2fe9410";
SecretKey CacheTestKey() {
  ObjectId bytes;
  bytes.fill(0x72);
  return SecretKey(bytes);
}

class RecordCacheFilesTest : public testing::Test {
 protected:
  const base::FilePath kCachePath{"/record-cache"};
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, CacheTestKey(), {}, &files_));
    Resolve();
    Factory();
  }
  void TearDown() override { CloseBackend(); }
  void Resolve() {
    const GURL url("https://cache.example/");
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &record_));
    cap_ = files_->Capability(record_.id, kPurpose);
  }
  void Factory() {
    ASSERT_EQ(FileStatus::kOk, RecordCacheFileOperationsFactory::Create(files_, cap_, kPurpose, &factory_));
  }
  disk_cache::BackendResult OpenBackend(const base::FilePath& path,
      disk_cache::ResetHandling reset = disk_cache::ResetHandling::kNeverReset) {
    base::test::TestFuture<disk_cache::BackendResult> future;
    auto result = disk_cache::CreateCacheBackend(net::DISK_CACHE, net::CACHE_BACKEND_SIMPLE,
        factory_, path, 16 * 1024 * 1024, reset,
        nullptr, nullptr, future.GetCallback());
    return result.net_error == net::ERR_IO_PENDING ? future.Take() : std::move(result);
  }
  disk_cache::BackendResult OpenBackend() { return OpenBackend(kCachePath); }
  void CreateBackend() {
    auto result = OpenBackend();
    ASSERT_EQ(net::OK, result.net_error);
    ASSERT_TRUE(result.backend);
    backend_ = std::move(result.backend);
  }
  void CloseBackend() {
    if (!backend_) return;
    backend_.reset();
    base::test::TestFuture<void> cleaned;
    disk_cache::WaitForBackendCleanupForTesting(kCachePath, cleaned.GetCallback());
    ASSERT_TRUE(cleaned.Wait());
    tasks_.RunUntilIdle();
  }
  disk_cache::EntryResult Entry(const std::string& key, bool create) {
    base::test::TestFuture<disk_cache::EntryResult> future;
    auto result = create ? backend_->CreateEntry(key, net::HIGHEST, future.GetCallback())
                         : backend_->OpenEntry(key, net::HIGHEST, future.GetCallback());
    return result.net_error() == net::ERR_IO_PENDING ? future.Take() : std::move(result);
  }
  void NoPlaintext() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-i", "-l"}) command.AppendArg(arg);
    command.AppendArg(std::string(kMarker));
    command.AppendArgPath(path_);
    int exit = -1;
    std::string output;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }
  void Reopen() {
    CloseBackend();
    factory_.reset();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, CacheTestKey(), &files_));
    Resolve();
    Factory();
  }
  base::test::TaskEnvironment tasks_{base::test::TaskEnvironment::MainThreadType::IO};
  base::ScopedTempDir temp_;
  base::FilePath path_;
  RecordInfo record_;
  RecordCapability cap_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<RecordCacheFileOperationsFactory> factory_;
  std::unique_ptr<disk_cache::Backend> backend_;
};

TEST_F(RecordCacheFilesTest, RealSimpleBackendStreamsSparseAndColdReopen) {
  CreateBackend();
  const std::string key = "https://cache.example/CACHE_CIPHER_MARKER_9736a2fe9410";
  auto result = Entry(key, true);
  ASSERT_EQ(net::OK, result.net_error());
  disk_cache::ScopedEntryPtr entry(result.ReleaseEntry());
  auto input = base::MakeRefCounted<net::StringIOBuffer>(std::string(kMarker));
  for (int index : {0, 1, 2}) {
    net::TestCompletionCallback callback;
    EXPECT_EQ(static_cast<int>(kMarker.size()), callback.GetResult(
        entry->WriteData(index, 0, input.get(), kMarker.size(), callback.callback(), true)));
  }
  entry.reset();
  auto sparse_result = Entry("sparse-key", true);
  ASSERT_EQ(net::OK, sparse_result.net_error());
  disk_cache::ScopedEntryPtr sparse(sparse_result.ReleaseEntry());
  auto sparse_input = base::MakeRefCounted<net::StringIOBuffer>(std::string(1024, 'S'));
  net::TestCompletionCallback written;
  ASSERT_EQ(1024, written.GetResult(sparse->WriteSparseData(8192, sparse_input.get(), 1024, written.callback())));
  sparse.reset();
  CloseBackend();
  NoPlaintext();
  Reopen();
  CreateBackend();
  result = Entry(key, false);
  ASSERT_EQ(net::OK, result.net_error());
  entry.reset(result.ReleaseEntry());
  for (int index : {0, 1, 2}) {
    auto output = base::MakeRefCounted<net::IOBufferWithSize>(100);
    net::TestCompletionCallback callback;
    ASSERT_EQ(static_cast<int>(kMarker.size()), callback.GetResult(
        entry->ReadData(index, 0, output.get(), 100, callback.callback())));
    EXPECT_EQ(kMarker, std::string_view(output->data(), kMarker.size()));
  }
  entry.reset();
  sparse_result = Entry("sparse-key", false);
  ASSERT_EQ(net::OK, sparse_result.net_error());
  sparse.reset(sparse_result.ReleaseEntry());
  auto output = base::MakeRefCounted<net::IOBufferWithSize>(1024);
  net::TestCompletionCallback hole, present;
  EXPECT_EQ(0, hole.GetResult(sparse->ReadSparseData(0, output.get(), 1024, hole.callback())));
  ASSERT_EQ(1024, present.GetResult(sparse->ReadSparseData(8192, output.get(), 1024, present.callback())));
  EXPECT_EQ(std::string(1024, 'S'), std::string(output->data(), 1024));
  sparse.reset();
  net::TestCompletionCallback doomed;
  EXPECT_EQ(net::OK, doomed.GetResult(backend_->DoomEntry(key, net::HIGHEST, doomed.callback())));
  EXPECT_NE(net::OK, Entry(key, false).net_error());
  result = Entry(key, true);
  ASSERT_EQ(net::OK, result.net_error());
  entry.reset(result.ReleaseEntry());
  EXPECT_EQ(0, entry->GetDataSize(1));
  entry.reset();
  CloseBackend();
  NoPlaintext();
}

TEST_F(RecordCacheFilesTest, VirtualFileInfoReplaceEnumerationAndUnbind) {
  auto operations = factory_->Create(base::SequencedTaskRunner::GetCurrentDefault());
  ASSERT_TRUE(operations->CreateDirectory(kCachePath.AppendASCII("index-dir")));
  auto first = operations->OpenFile(kCachePath.AppendASCII("index-dir/temp-index"),
      base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
  ASSERT_TRUE(first && first->IsValid());
  ASSERT_TRUE(first->WriteAndCheck(0, base::as_byte_span(kMarker)));
  ASSERT_EQ(static_cast<int64_t>(kMarker.size()), first->GetLength());
  base::File::Error error;
  ASSERT_TRUE(operations->ReplaceFile(kCachePath.AppendASCII("index-dir/temp-index"),
      kCachePath.AppendASCII("index-dir/the-real-index"), &error));
  EXPECT_EQ(base::File::FILE_OK, error);
  auto entries = operations->EnumerateFiles(kCachePath.AppendASCII("index-dir"));
  const auto only = entries->Next();
  ASSERT_TRUE(only);
  EXPECT_EQ(kCachePath.AppendASCII("index-dir/the-real-index"), only->path);
  EXPECT_EQ(static_cast<int64_t>(kMarker.size()), only->size);
  EXPECT_FALSE(entries->Next());
  EXPECT_FALSE(entries->HasError());
  auto unbound = operations->Unbind();
  operations = unbound->Bind(base::SequencedTaskRunner::GetCurrentDefault());
  ASSERT_TRUE(operations->DeleteFile(kCachePath.AppendASCII("index-dir/the-real-index"),
      disk_cache::BackendFileOperations::DeleteFileMode::kEnsureImmediateAvailability));
  std::vector<uint8_t> bytes(kMarker.size());
  EXPECT_TRUE(first->ReadAndCheck(0, bytes));  // Old handle survives unlink.
  EXPECT_EQ(kMarker, std::string(bytes.begin(), bytes.end()));
  auto replacement = operations->OpenFile(kCachePath.AppendASCII("index-dir/the-real-index"),
      base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  ASSERT_TRUE(replacement->IsValid());
  EXPECT_EQ(0, replacement->GetLength());
  NoPlaintext();
}

TEST_F(RecordCacheFilesTest, TamperedBackendFailsWithIntegrityNotPlaintextReset) {
  CreateBackend();
  auto result = Entry("tamper-key", true);
  ASSERT_EQ(net::OK, result.net_error());
  disk_cache::ScopedEntryPtr entry(result.ReleaseEntry());
  auto input = base::MakeRefCounted<net::StringIOBuffer>(std::string(kMarker));
  net::TestCompletionCallback callback;
  ASSERT_EQ(static_cast<int>(kMarker.size()), callback.GetResult(
      entry->WriteData(1, 0, input.get(), kMarker.size(), callback.callback(), true)));
  entry.reset();
  CloseBackend();
  base::FileEnumerator objects(path_.AppendASCII(ObjectName(record_.directory)), false, base::FileEnumerator::FILES);
  size_t changed = 0;
  for (auto path = objects.Next(); !path.empty(); path = objects.Next()) {
    auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes && !bytes->empty());
    base::span(*bytes)[0] ^= 1;
    ASSERT_TRUE(base::WriteFile(path, *bytes));
    ++changed;
  }
  ASSERT_GT(changed, 0u);
  auto failed = OpenBackend(kCachePath, disk_cache::ResetHandling::kResetOnError);
  EXPECT_NE(net::OK, failed.net_error);
  EXPECT_EQ(FileStatus::kIntegrity, factory_->last_status());
  EXPECT_FALSE(files_->unlocked());
  auto operations = factory_->Create(base::SequencedTaskRunner::GetCurrentDefault());
  EXPECT_FALSE(operations->CreateDirectory(kCachePath));
}

TEST_F(RecordCacheFilesTest, FailedConsistencyCannotReachPhysicalRecoveryHelpers) {
  // A virtual absolute path must never authorize I/O at its physical spelling.
  const auto cache_path = temp_.GetPath().AppendASCII("unrelated-cache");
  const auto index_path = cache_path.AppendASCII("index");
  ASSERT_TRUE(base::CreateDirectory(cache_path));
  ASSERT_TRUE(base::WriteFile(index_path, kMarker));
  auto operations = factory_->Create(base::SequencedTaskRunner::GetCurrentDefault());
  ASSERT_TRUE(operations->CreateDirectory(cache_path));
  auto index = operations->OpenFile(index_path,
      base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
  ASSERT_TRUE(index->IsValid());
  // Authenticated virtual bytes, but an invalid Simple Cache index structure.
  const std::string bad_index(64, 'X');
  ASSERT_TRUE(index->WriteAndCheck(0, base::as_byte_span(bad_index)));
  index.reset();
  auto failed = OpenBackend(cache_path);
  EXPECT_NE(net::OK, failed.net_error);
  std::string physical;
  EXPECT_TRUE(base::ReadFileToString(index_path, &physical));
  EXPECT_EQ(kMarker, physical);
  EXPECT_TRUE(operations->PathExists(index_path));
  EXPECT_TRUE(files_->unlocked());
}

TEST_F(RecordCacheFilesTest, CleanupWaitsForDurableRemovalAndCannotResurrectDeletedRecord) {
  auto operations = factory_->Create(base::SequencedTaskRunner::GetCurrentDefault());
  ASSERT_TRUE(operations->CreateDirectory(kCachePath));
  auto file = operations->OpenFile(kCachePath.AppendASCII("body"),
      base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
  ASSERT_TRUE(file->WriteAndCheck(0, base::as_byte_span(kMarker)));
  base::test::TestFuture<bool> cleanup;
  operations->CleanupDirectory(kCachePath, cleanup.GetCallback());
  EXPECT_FALSE(cleanup.IsReady());
  EXPECT_TRUE(cleanup.Get());
  EXPECT_FALSE(operations->PathExists(kCachePath));
  ASSERT_TRUE(operations->CreateDirectory(kCachePath));
  base::test::TestFuture<bool> pending;
  operations->CleanupDirectory(kCachePath, pending.GetCallback());
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, files_->DeleteRecord(record_.id, {}, &deletion));
  EXPECT_FALSE(pending.Get());
  EXPECT_FALSE(file->IsValid());
  EXPECT_FALSE(file->WriteAndCheck(0, base::as_byte_span(kMarker)));
}
}  // namespace
}  // namespace alloy::site_records
