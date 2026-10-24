// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_file_system.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

constexpr auto kPurpose = StorePurpose::kIndexedDb;

SecretKey TestProfileKey() {
  ObjectId bytes;
  bytes.fill(0x37);
  return SecretKey(bytes);
}

class RecordFileSystemTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk,
              RecordFileSystem::Create(path_, TestProfileKey(), {}, &fs_));
    Resolve("https://en.wikipedia.org/", true, &record_);
    cap_ = fs_->Capability(record_.id, kPurpose);
  }

  void Resolve(std::string_view principal, bool create, RecordInfo* out) {
    const GURL url(principal);
    ASSERT_EQ(FileStatus::kOk,
              fs_->ResolveRecord(url, url::Origin::Create(url), create, out));
  }

  void Reopen() {
    ASSERT_EQ(FileStatus::kOk, fs_->Lock());
    ASSERT_TRUE(fs_->KeysClearedForTesting());
    fs_.reset();
    ASSERT_EQ(FileStatus::kOk,
              RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
    Resolve("http://ru.wikipedia.org/", false, &record_);
    cap_ = fs_->Capability(record_.id, kPurpose);
  }

  std::string Read(const std::shared_ptr<RecordFile>& file, size_t length,
                     uint64_t offset = 0) {
    std::vector<uint8_t> bytes(length);
    size_t read = 0;
    EXPECT_EQ(FileStatus::kOk, file->Read(offset, bytes, &read));
    return std::string(bytes.begin(), bytes.begin() + read);
  }

  std::map<base::FilePath, std::vector<uint8_t>> PhysicalFiles() {
    std::map<base::FilePath, std::vector<uint8_t>> files;
    base::FileEnumerator scan(path_, true, base::FileEnumerator::FILES);
    for (auto path = scan.Next(); !path.empty(); path = scan.Next()) {
      auto bytes = base::ReadFileToBytes(path);
      EXPECT_TRUE(bytes);
      if (bytes) {
        files.emplace(path, std::move(*bytes));
      }
    }
    return files;
  }

  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> fs_;
  RecordInfo record_;
  RecordCapability cap_;
};

TEST_F(RecordFileSystemTest, NativeNamespaceAndDurableCloseRoundTrip) {
  ASSERT_EQ(FileStatus::kOk,
            fs_->CreateDirectory(cap_, kPurpose, "db/journals", true));
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk,
            fs_->OpenFile(cap_, kPurpose, "db/native-secret.sqlite",
                            RecordOpen::kCreate, &file));
  const std::string marker = "encrypted-native-file-marker-321";
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
  RecordFileInfo before;
  ASSERT_EQ(FileStatus::kOk, file->GetInfo(&before));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  EXPECT_TRUE(file->KeysClearedForTesting());
  Reopen();
  ASSERT_EQ(FileStatus::kOk,
            fs_->OpenFile(cap_, kPurpose, "db/native-secret.sqlite",
                            RecordOpen::kOpen, &file));
  EXPECT_EQ(marker, Read(file, 100));
  RecordFileInfo after;
  ASSERT_EQ(FileStatus::kOk, file->GetInfo(&after));
  EXPECT_EQ(before.identity, after.identity);
  EXPECT_EQ(before.modified_us, after.modified_us);
  EXPECT_EQ(marker.size(), after.length);
  std::vector<RecordDirectoryEntry> entries;
  ASSERT_EQ(FileStatus::kOk, fs_->List(cap_, kPurpose, "db", &entries));
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(FileStatus::kNotEmpty, fs_->Remove(cap_, kPurpose, "db"));
  EXPECT_EQ(FileStatus::kInvalid,
            fs_->OpenFile(cap_, kPurpose, "../native.sqlite", RecordOpen::kOpen,
                            &file));
}

TEST_F(RecordFileSystemTest, RenameReplaceUnlinkPreserveOpenIdentity) {
  std::shared_ptr<RecordFile> old;
  std::shared_ptr<RecordFile> fresh;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "CURRENT",
                                           RecordOpen::kCreate, &old));
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "CURRENT.tmp",
                                           RecordOpen::kCreate, &fresh));
  ASSERT_EQ(FileStatus::kOk,
            old->Write(0, base::as_byte_span(std::string_view("old"))));
  ASSERT_EQ(FileStatus::kOk,
            fresh->Write(0, base::as_byte_span(std::string_view("new"))));
  RecordFileInfo identity;
  ASSERT_EQ(FileStatus::kOk, fresh->GetInfo(&identity));
  ASSERT_EQ(FileStatus::kOk,
            fs_->Rename(cap_, kPurpose, "CURRENT.tmp", "CURRENT"));
  EXPECT_EQ("old", Read(old, 10));
  EXPECT_EQ("new", Read(fresh, 10));
  std::shared_ptr<RecordFile> named;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "CURRENT",
                                           RecordOpen::kOpen, &named));
  RecordFileInfo info;
  ASSERT_EQ(FileStatus::kOk, named->GetInfo(&info));
  EXPECT_EQ(identity.identity, info.identity);
  ASSERT_EQ(FileStatus::kOk, fs_->Remove(cap_, kPurpose, "CURRENT"));
  ASSERT_EQ(FileStatus::kOk,
            fresh->Write(0, base::as_byte_span(std::string_view("detached"))));
  ASSERT_EQ(FileStatus::kOk, fresh->Close());
  EXPECT_EQ("detached", Read(named, 100));
  ASSERT_EQ(FileStatus::kOk, named->Close());
  EXPECT_TRUE(named->KeysClearedForTesting());
  Reopen();
  EXPECT_EQ(FileStatus::kNotFound,
            fs_->OpenFile(cap_, kPurpose, "CURRENT", RecordOpen::kOpen, &named));
  EXPECT_EQ(FileStatus::kNotFound,
            fs_->OpenFile(cap_, kPurpose, "CURRENT.tmp", RecordOpen::kOpen,
                            &named));
}

TEST_F(RecordFileSystemTest, DirectoryRenameKeepsChildrenAndRejectsCycles) {
  ASSERT_EQ(FileStatus::kOk,
            fs_->CreateDirectory(cap_, kPurpose, "a/b/c", true));
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "a/b/c/value",
                                           RecordOpen::kCreate, &file));
  EXPECT_EQ(FileStatus::kInvalid,
            fs_->Rename(cap_, kPurpose, "a", "a/b/c/nested"));
  ASSERT_EQ(FileStatus::kOk, fs_->Rename(cap_, kPurpose, "a/b", "renamed"));
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("inside"))));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  Reopen();
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "renamed/c/value",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ("inside", Read(file, 100));
  ASSERT_EQ(FileStatus::kOk, fs_->Remove(cap_, kPurpose, "renamed", true));
  EXPECT_EQ("inside", Read(file, 100));
  Reopen();
  RecordFileInfo info;
  EXPECT_EQ(FileStatus::kNotFound, fs_->Stat(cap_, kPurpose, "renamed", &info));
}

TEST_F(RecordFileSystemTest, ConcurrentHandlesShareOneLogicalRevision) {
  std::shared_ptr<RecordFile> first;
  std::shared_ptr<RecordFile> second;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "shared",
                                           RecordOpen::kCreate, &first));
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "shared",
                                           RecordOpen::kOpen, &second));
  std::atomic<int> failures{0};
  auto write = [&](const std::shared_ptr<RecordFile>& file, size_t parity) {
    for (size_t i = parity; i < 20; i += 2) {
      const std::vector<uint8_t> bytes(4096, i + 1);
      if (file->Write(i * 4096, bytes) != FileStatus::kOk) {
        ++failures;
      }
    }
  };
  std::thread a(write, first, 0);
  std::thread b(write, second, 1);
  a.join();
  b.join();
  EXPECT_EQ(0, failures.load());
  ASSERT_EQ(FileStatus::kOk, second->Sync());
  Reopen();
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "shared",
                                           RecordOpen::kOpen, &first));
  for (size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(std::string(4096, i + 1), Read(first, 4096, i * 4096));
  }
}

TEST_F(RecordFileSystemTest, SharedReservedPendingExclusiveLocks) {
  std::shared_ptr<RecordFile> first;
  std::shared_ptr<RecordFile> second;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "LOCK",
                                           RecordOpen::kCreate, &first));
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "LOCK",
                                           RecordOpen::kOpen, &second));
  ASSERT_EQ(FileStatus::kOk, first->Lock(RecordFileLock::kShared));
  ASSERT_EQ(FileStatus::kOk, second->Lock(RecordFileLock::kShared));
  ASSERT_EQ(FileStatus::kOk, first->Lock(RecordFileLock::kReserved));
  EXPECT_EQ(FileStatus::kBusy, second->Lock(RecordFileLock::kReserved));
  bool reserved = false;
  ASSERT_EQ(FileStatus::kOk, second->HasReservedLock(&reserved));
  EXPECT_TRUE(reserved);
  EXPECT_EQ(FileStatus::kBusy, first->Lock(RecordFileLock::kExclusive));
  ASSERT_EQ(FileStatus::kOk, second->Unlock());
  EXPECT_EQ(FileStatus::kBusy, second->Lock(RecordFileLock::kShared));
  ASSERT_EQ(FileStatus::kOk, first->Lock(RecordFileLock::kExclusive));
  ASSERT_EQ(FileStatus::kOk, first->Close());
  ASSERT_EQ(FileStatus::kOk, second->Lock(RecordFileLock::kExclusive));
}

TEST_F(RecordFileSystemTest, CataloguePrunedBeforeTeardownAndDataUnlink) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "secret",
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("private"))));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  const auto physical = PhysicalFiles();
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  std::vector<base::FilePath> key_pages;
  for (const auto& [path, bytes] : physical) {
    if (path.DirName() != record_path && path.BaseName().value() != "root") {
      key_pages.push_back(path);
    }
  }
  ASSERT_FALSE(key_pages.empty());
  bool called = false;
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, fs_->DeleteRecord(record_.id, [&] {
    called = true;
    EXPECT_FALSE(cap_.active());
    EXPECT_TRUE(file->KeysClearedForTesting());
    for (const auto& path : key_pages) {
      EXPECT_FALSE(base::PathExists(path));
    }
    EXPECT_TRUE(base::DirectoryExists(record_path));
    std::vector<RecordInfo> records;
    EXPECT_EQ(FileStatus::kOk, fs_->ListRecords(&records));
    EXPECT_TRUE(records.empty());
    size_t count;
    std::vector<uint8_t> bytes(10);
    EXPECT_EQ(FileStatus::kDenied, file->Read(0, bytes, &count));
    const GURL url("https://wikipedia.org/");
    RecordInfo replacement;
    EXPECT_EQ(FileStatus::kBusy,
              fs_->ResolveRecord(url, url::Origin::Create(url), true,
                                   &replacement));
  }, &deletion));
  EXPECT_TRUE(called);
  EXPECT_EQ(RecordDeletion::kDeleted, deletion);
  EXPECT_FALSE(base::DirectoryExists(record_path));
  const auto generation = record_.generation;
  Resolve("https://wikipedia.org/", true, &record_);
  EXPECT_NE(generation, record_.generation);
  const auto replacement_cap = fs_->Capability(record_.id, kPurpose);
  std::shared_ptr<RecordFile> replacement;
  EXPECT_EQ(FileStatus::kNotFound, fs_->OpenFile(replacement_cap, kPurpose,
      "secret", RecordOpen::kOpen, &replacement));
}

TEST_F(RecordFileSystemTest, DeleteDuringPendingWriteCannotResurrectGeneration) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "pending",
                                           RecordOpen::kCreate, &file));
  std::mutex mutex;
  std::condition_variable changed;
  bool pending = false;
  bool release = false;
  fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath&) {
    std::unique_lock lock(mutex);
    if (fence == PublishFence::kObjectWritten && !pending) {
      pending = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    return FileStatus::kOk;
  });
  std::atomic<int> write_result{-1};
  std::atomic<int> delete_result{-1};
  std::atomic<bool> deleting{false};
  std::thread writer([&] {
    write_result = static_cast<int>(file->Write(0,
        base::as_byte_span(std::string_view("pending-private-bytes"))));
  });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return pending; });
  }
  std::thread deleter([&] {
    deleting = true;
    RecordDeletion result;
    delete_result = static_cast<int>(fs_->DeleteRecord(record_.id, {}, &result));
  });
  while (!deleting.load()) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  writer.join();
  deleter.join();
  fs_->SetFaultForTesting({});
  EXPECT_EQ(static_cast<int>(FileStatus::kOk), write_result.load());
  EXPECT_EQ(static_cast<int>(FileStatus::kOk), delete_result.load());
  EXPECT_EQ(FileStatus::kDenied,
            file->Write(0, base::as_byte_span(std::string_view("too-late"))));
  EXPECT_TRUE(file->KeysClearedForTesting());
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  fs_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, fs_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
}

TEST_F(RecordFileSystemTest, LockWipesRetainedHandlesAndRefusesAllOperations) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "live",
                                           RecordOpen::kCreate, &file));
  ASSERT_FALSE(file->KeysClearedForTesting());
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  EXPECT_TRUE(file->KeysClearedForTesting());
  EXPECT_TRUE(fs_->KeysClearedForTesting());
  EXPECT_FALSE(cap_.active());
  EXPECT_FALSE(fs_->unlocked());
  EXPECT_EQ(FileStatus::kDenied, file->SetLength(17));
  RecordFileInfo info;
  EXPECT_EQ(FileStatus::kDenied, fs_->Stat(cap_, kPurpose, "live", &info));
  std::vector<RecordInfo> records;
  EXPECT_EQ(FileStatus::kDenied, fs_->ListRecords(&records));
}

TEST_F(RecordFileSystemTest, PurposeAndProfileCapabilitiesCannotBeSubstituted) {
  std::shared_ptr<RecordFile> file;
  EXPECT_EQ(FileStatus::kDenied,
            fs_->OpenFile(cap_, StorePurpose::kCookies, "db",
                            RecordOpen::kCreate, &file));
  RecordCapabilityIssuer foreign(base::UnguessableToken::Create(), record_.id,
                                  record_.generation, StorageMode::kEncrypted);
  EXPECT_EQ(FileStatus::kDenied,
            fs_->OpenFile(foreign.Issue(kPurpose), kPurpose, "db",
                            RecordOpen::kCreate, &file));
  const auto cookie_cap = fs_->Capability(record_.id, StorePurpose::kCookies);
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cookie_cap, StorePurpose::kCookies,
      "db", RecordOpen::kCreate, &file));
  EXPECT_EQ(FileStatus::kNotFound,
            fs_->OpenFile(cap_, kPurpose, "db", RecordOpen::kOpen, &file));
}

TEST_F(RecordFileSystemTest, AllNamesMetadataAndDataRemainCiphertext) {
  const std::string name = "private-native-name.sqlite";
  const std::string marker = "PRIVATE_RECORD_DATA_7a9876589252";
  const std::string metadata = "PRIVATE_RECORD_METADATA_21e28ccfe";
  ASSERT_EQ(FileStatus::kOk,
            fs_->SetRecordMetadata(record_.id, base::as_byte_span(metadata)));
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, name,
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  EXPECT_NE(ObjectName(Digest(base::as_byte_span(record_.domain))),
            ObjectName(record_.directory));
  for (const auto& [path, bytes] : PhysicalFiles()) {
    for (const auto& text : {name, marker, metadata, record_.domain}) {
      EXPECT_EQ(std::string::npos, path.value().find(text));
      const std::string body(bytes.begin(), bytes.end());
      EXPECT_EQ(std::string::npos, body.find(text));
    }
  }
  Reopen();
  EXPECT_EQ(metadata,
            std::string(record_.metadata.begin(), record_.metadata.end()));
}

TEST_F(RecordFileSystemTest, PartialRootReplayIsNotAnEmptyProfileFallback) {
  const auto old = base::ReadFileToBytes(path_.AppendASCII("root"));
  ASSERT_TRUE(old);
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "new-file",
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  fs_.reset();
  ASSERT_TRUE(base::WriteFile(path_.AppendASCII("root"), *old));
  EXPECT_EQ(FileStatus::kIntegrity,
            RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  EXPECT_FALSE(fs_);
}

TEST_F(RecordFileSystemTest, MissingCurrentRecordObjectsFailUnlock) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "missing",
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("current"))));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  fs_.reset();
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  base::FileEnumerator objects(record_path, false, base::FileEnumerator::FILES);
  for (auto path = objects.Next(); !path.empty(); path = objects.Next()) {
    ASSERT_TRUE(base::DeleteFile(path));
  }
  EXPECT_EQ(FileStatus::kIntegrity,
            RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  EXPECT_FALSE(fs_);
}

TEST_F(RecordFileSystemTest, AfterRenameFailureQuarantinesUntilReopen) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "uncertain",
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("committed"))));
  fs_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootRenamed ? FileStatus::kIo : FileStatus::kOk;
  });
  EXPECT_EQ(FileStatus::kCommitUncertain, file->Sync());
  EXPECT_TRUE(fs_->KeysClearedForTesting());
  EXPECT_TRUE(file->KeysClearedForTesting());
  EXPECT_FALSE(fs_->unlocked());
  EXPECT_EQ(FileStatus::kCommitUncertain, file->SetLength(0));
  fs_->SetFaultForTesting({});
  EXPECT_EQ(FileStatus::kCommitUncertain, fs_->Lock());
  fs_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  Resolve("https://wikipedia.org/", false, &record_);
  cap_ = fs_->Capability(record_.id, kPurpose);
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "uncertain",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ("committed", Read(file, 100));
}

TEST_F(RecordFileSystemTest, CowCollectionIsBoundedAndKeepsDetachedHandles) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "rewrite",
                                           RecordOpen::kCreate, &file));
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  const auto count_objects = [&] {
    size_t count = 0;
    base::FileEnumerator files(record_path, false, base::FileEnumerator::FILES);
    while (!files.Next().empty()) {
      ++count;
    }
    return count;
  };
  for (int iteration = 0; iteration < 20; ++iteration) {
    const std::string marker(100, 'a' + iteration);
    ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
    ASSERT_EQ(FileStatus::kOk, file->Sync());
    EXPECT_EQ(3u, count_objects());
  }
  std::shared_ptr<RecordFile> detached;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "detached",
                                           RecordOpen::kCreate, &detached));
  const std::string marker = "detached-live-object";
  ASSERT_EQ(FileStatus::kOk, detached->Write(0, base::as_byte_span(marker)));
  ASSERT_EQ(FileStatus::kOk, detached->Sync());
  ASSERT_EQ(FileStatus::kOk, fs_->Remove(cap_, kPurpose, "detached"));
  ASSERT_EQ(FileStatus::kOk, fs_->Sync());
  EXPECT_EQ(marker, Read(detached, 100));
  EXPECT_GT(count_objects(), 3u);
  ASSERT_EQ(FileStatus::kOk, detached->Close());
  EXPECT_TRUE(detached->KeysClearedForTesting());
  ASSERT_EQ(FileStatus::kOk, fs_->Sync());
  EXPECT_EQ(3u, count_objects());
}

TEST_F(RecordFileSystemTest, DeletedFilesPendingAreRetriedAtUnlock) {
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "pending-delete",
                                           RecordOpen::kCreate, &file));
  fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath& path) {
    return fence == PublishFence::kObsoleteObjectsUnlinked && path == record_path
               ? FileStatus::kIo : FileStatus::kOk;
  });
  RecordDeletion deletion = RecordDeletion::kDeleted;
  bool called = false;
  ASSERT_EQ(FileStatus::kOk,
            fs_->DeleteRecord(record_.id, [&] { called = true; }, &deletion));
  EXPECT_TRUE(called);
  EXPECT_EQ(RecordDeletion::kDeletedFilesPending, deletion);
  EXPECT_TRUE(base::DirectoryExists(record_path));
  EXPECT_FALSE(cap_.active());
  fs_->SetFaultForTesting({});
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  fs_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, fs_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
  EXPECT_FALSE(base::DirectoryExists(record_path));
}

TEST_F(RecordFileSystemTest, KeyRetirementFailureCannotAcknowledgeDeletion) {
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "key-retirement",
                                           RecordOpen::kCreate, &file));
  fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath& path) {
    return fence == PublishFence::kObsoleteObjectsUnlinked && path != record_path
               ? FileStatus::kIo : FileStatus::kOk;
  });
  RecordDeletion deletion;
  bool called = false;
  EXPECT_EQ(FileStatus::kCommitUncertain,
            fs_->DeleteRecord(record_.id, [&] { called = true; }, &deletion));
  EXPECT_FALSE(called);
  EXPECT_FALSE(fs_->unlocked());
  EXPECT_TRUE(fs_->KeysClearedForTesting());
  EXPECT_TRUE(file->KeysClearedForTesting());
  EXPECT_TRUE(base::DirectoryExists(record_path));
  fs_->SetFaultForTesting({});
  EXPECT_EQ(FileStatus::kCommitUncertain, fs_->Lock());
  fs_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, fs_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
  EXPECT_FALSE(base::DirectoryExists(record_path));
}

TEST_F(RecordFileSystemTest, CompleteSnapshotRestoreIsTheDocumentedBoundary) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "snapshot",
                                           RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("original"))));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  const auto snapshot = PhysicalFiles();
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("modified"))));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  ASSERT_EQ(FileStatus::kOk, fs_->Lock());
  fs_.reset();
  for (const auto& [path, bytes] : snapshot) {
    ASSERT_TRUE(base::WriteFile(path, bytes));
  }
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestProfileKey(), &fs_));
  Resolve("https://wikipedia.org/", false, &record_);
  cap_ = fs_->Capability(record_.id, kPurpose);
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "snapshot",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ("original", Read(file, 100));
}

TEST_F(RecordFileSystemTest, TornPhysicalWriteRetriesWithoutReusingItsObject) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "torn",
                                           RecordOpen::kCreate, &file));
  base::FilePath torn;
  fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath& path) {
    if (fence == PublishFence::kObjectWritten && torn.empty()) {
      torn = path;
      base::File physical(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
      EXPECT_TRUE(physical.SetLength(37));
      return FileStatus::kIo;
    }
    return FileStatus::kOk;
  });
  const std::string marker = "retried-authenticated-data";
  EXPECT_EQ(FileStatus::kIo, file->Write(0, base::as_byte_span(marker)));
  ASSERT_FALSE(torn.empty());
  fs_->SetFaultForTesting({});
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  EXPECT_FALSE(base::PathExists(torn));
  Reopen();
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "torn",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ(marker, Read(file, 100));
}

TEST_F(RecordFileSystemTest, ReplayedDataCannotBeReadOrSilentlyRazed) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "replayed",
                                           RecordOpen::kCreate, &file));
  base::FilePath extent;
  fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath& path) {
    if (fence == PublishFence::kObjectWritten && extent.empty()) {
      extent = path;
    }
    return FileStatus::kOk;
  });
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("old-value"))));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  const auto old = base::ReadFileToBytes(extent);
  ASSERT_TRUE(old);
  extent.clear();
  ASSERT_EQ(FileStatus::kOk,
            file->Write(0, base::as_byte_span(std::string_view("new-value"))));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  ASSERT_TRUE(base::WriteFile(extent, *old));
  std::vector<uint8_t> bytes(100, 0xff);
  size_t read = 99;
  EXPECT_EQ(FileStatus::kIntegrity, file->Read(0, bytes, &read));
  EXPECT_EQ(0u, read);
  EXPECT_TRUE(std::ranges::all_of(bytes, [](auto byte) { return byte == 0; }));
  EXPECT_EQ(FileStatus::kIntegrity, file->SetLength(0));
  EXPECT_EQ(FileStatus::kIntegrity,
            file->Write(0, base::as_byte_span(std::string_view("overwrite"))));
  EXPECT_TRUE(fs_->KeysClearedForTesting());
  EXPECT_TRUE(file->KeysClearedForTesting());
}

TEST_F(RecordFileSystemTest, TruncateExtendZeroTailSurvivesCatalogueReopen) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "length",
                                           RecordOpen::kCreate, &file));
  const std::string original(12000, 'S');
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(original)));
  ASSERT_EQ(FileStatus::kOk, file->SetLength(4017));
  ASSERT_EQ(FileStatus::kOk, file->SetLength(original.size()));
  ASSERT_EQ(FileStatus::kOk, file->Sync());
  Reopen();
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "length",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ(std::string(4017, 'S') + std::string(original.size() - 4017, '\0'),
            Read(file, original.size()));
}

TEST_F(RecordFileSystemTest, RecordsPublicationCostWithoutAnAcceptanceBudget) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "publication-cost",
                                           RecordOpen::kCreate, &file));
  const auto record_path = path_.AppendASCII(ObjectName(record_.directory));
  size_t records = 1;
  for (size_t population : {1u, 8u, 32u}) {
    while (records < population) {
      RecordInfo other;
      Resolve("https://r" + std::to_string(records++) + ".example/", true,
              &other);
    }
    int64_t elapsed_us = 0;
    size_t catalogue_writes = 0;
    fs_->SetFaultForTesting([&](PublishFence fence, const base::FilePath& path) {
      if (fence == PublishFence::kObjectWritten && path.DirName() != record_path) {
        ++catalogue_writes;
      }
      return FileStatus::kOk;
    });
    for (int sample = 0; sample < 5; ++sample) {
      ASSERT_EQ(FileStatus::kOk,
                file->Write(0, base::as_byte_span(std::string_view("x"))));
      const auto start = std::chrono::steady_clock::now();
      ASSERT_EQ(FileStatus::kOk, file->Sync());
      elapsed_us += std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
    }
    fs_->SetFaultForTesting({});
    // Search-path copies are bounded by the format's 256 branch bits plus
    // the replaced leaf, independent of profile population. No speed threshold.
    EXPECT_LE(catalogue_writes, 5u * 257u);
    RecordProperty("mean_sync_" + std::to_string(population) + "_records_us",
                   std::to_string(elapsed_us / 5));
    RecordProperty("catalogue_pages_" + std::to_string(population) + "_records",
                   std::to_string(catalogue_writes));
  }
}

TEST_F(RecordFileSystemTest, ReplacingOutputHandleClosesOutsideCommitBarrier) {
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "first",
                                           RecordOpen::kCreate, &file));
  const std::string marker = "first-handle-close-must-publish";
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
  // Assignment drops the sole old handle. Its destructor closes/commits through
  // the same coordinator, so it must execute after OpenFile releases the mutex.
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "second",
                                           RecordOpen::kCreate, &file));
  Reopen();
  ASSERT_EQ(FileStatus::kOk, fs_->OpenFile(cap_, kPurpose, "first",
                                           RecordOpen::kOpen, &file));
  EXPECT_EQ(marker, Read(file, 100));
}

}  // namespace
}  // namespace alloy::site_records
