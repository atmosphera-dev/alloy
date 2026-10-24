// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_file_system.h"

#include <unistd.h>

#include <charconv>
#include <set>
#include <string>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/multiprocess_test.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

constexpr int kInjectedCrash = 73;
constexpr auto kPurpose = StorePurpose::kIndexedDb;
constexpr size_t kPayloadSize = 9000;

SecretKey CrashTestKey() {
  ObjectId bytes;
  bytes.fill(0x69);
  return SecretKey(bytes);
}

int ParseNumber(std::string_view text) {
  int value = -1;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc() && result.ptr == text.data() + text.size()
             ? value : -1;
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyRecordCrashChild) {
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto path = command->GetSwitchValuePath("record-profile");
  const auto receipt = command->GetSwitchValuePath("record-fault-receipt");
  const int target = ParseNumber(command->GetSwitchValueASCII("record-fault"));
  const bool deleting = command->HasSwitch("record-delete");
  if (target < 1) {
    return 1;
  }
  std::shared_ptr<RecordFileSystem> fs;
  if (RecordFileSystem::Open(path, CrashTestKey(), &fs) != FileStatus::kOk) {
    return 2;
  }
  const GURL principal("https://crash.example/");
  RecordInfo record;
  if (fs->ResolveRecord(principal, url::Origin::Create(principal), false,
                          &record) != FileStatus::kOk) {
    return 3;
  }
  std::shared_ptr<RecordFile> file;
  if (fs->OpenFile(fs->Capability(record.id, kPurpose), kPurpose, "database",
                      RecordOpen::kOpen, &file) != FileStatus::kOk) {
    return 4;
  }
  int count = 0;
  fs->SetFaultForTesting([&](PublishFence fence, const base::FilePath&) {
    if (++count == target) {
      if (!base::WriteFile(receipt, std::to_string(static_cast<int>(fence)))) {
        _exit(5);
      }
      // Deliberately skip every destructor, close, Lock and graceful recovery.
      _exit(kInjectedCrash);
    }
    return FileStatus::kOk;
  });
  FileStatus status;
  if (deleting) {
    RecordDeletion result;
    status = fs->DeleteRecord(record.id, {}, &result);
  } else {
    const std::string payload(kPayloadSize, 'N');
    status = file->Write(0, base::as_byte_span(payload));
    if (status == FileStatus::kOk) {
      status = file->Sync();
    }
  }
  // The non-faulted final iteration crashes AFTER the operation acknowledged
  // success, proving its result survives without a later close/Lock flush.
  _exit(status == FileStatus::kOk ? 0 : 6);
}

namespace {

class RecordFileCrashTest : public testing::Test {
 protected:
  void RunEveryFence(bool deleting) {
    std::set<PublishFence> covered;
    bool reached_acknowledgement = false;
    int crashes = 0;
    for (int index = 1; index <= 128; ++index) {
      SCOPED_TRACE(index);
      base::ScopedTempDir temp;
      ASSERT_TRUE(temp.CreateUniqueTempDir());
      const auto path = temp.GetPath().AppendASCII("profile");
      const auto receipt = temp.GetPath().AppendASCII("fault-receipt");
      std::shared_ptr<RecordFileSystem> fs;
      ASSERT_EQ(FileStatus::kOk,
                RecordFileSystem::Create(path, CrashTestKey(), {}, &fs));
      const GURL principal("https://crash.example/");
      RecordInfo record;
      ASSERT_EQ(FileStatus::kOk, fs->ResolveRecord(principal,
          url::Origin::Create(principal), true, &record));
      std::shared_ptr<RecordFile> file;
      ASSERT_EQ(FileStatus::kOk, fs->OpenFile(fs->Capability(record.id, kPurpose),
          kPurpose, "database", RecordOpen::kCreate, &file));
      const std::string old(kPayloadSize, 'O');
      ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(old)));
      ASSERT_EQ(FileStatus::kOk, file->Close());
      ASSERT_EQ(FileStatus::kOk, fs->Lock());
      fs.reset();

      auto command = base::GetMultiProcessTestChildBaseCommandLine();
      command.AppendSwitchPath("record-profile", path);
      command.AppendSwitchPath("record-fault-receipt", receipt);
      command.AppendSwitchASCII("record-fault", std::to_string(index));
      if (deleting) {
        command.AppendSwitch("record-delete");
      }
      auto child = base::SpawnMultiProcessTestChild("AlloyRecordCrashChild",
                                                     command, {});
      ASSERT_TRUE(child.IsValid());
      int exit = -1;
      const bool stopped = base::WaitForMultiprocessTestChildExit(
          child, base::Seconds(30), &exit);
      if (!stopped) {
        base::TerminateMultiProcessTestChild(child, 99, true);
      }
      ASSERT_TRUE(stopped);
      ASSERT_TRUE(exit == 0 || exit == kInjectedCrash) << exit;
      if (exit == kInjectedCrash) {
        std::string fence;
        ASSERT_TRUE(base::ReadFileToString(receipt, &fence));
        const auto number = ParseNumber(fence);
        ASSERT_GE(number, 0);
        ASSERT_LE(number, static_cast<int>(PublishFence::kRecordDirectorySynced));
        covered.insert(static_cast<PublishFence>(number));
        ++crashes;
      }

      ASSERT_EQ(FileStatus::kOk,
                RecordFileSystem::Open(path, CrashTestKey(), &fs));
      const auto found = fs->ResolveRecord(principal,
          url::Origin::Create(principal), false, &record);
      if (deleting && found == FileStatus::kNotFound) {
        std::vector<RecordInfo> records;
        ASSERT_EQ(FileStatus::kOk, fs->ListRecords(&records));
        EXPECT_TRUE(records.empty());
      } else {
        ASSERT_EQ(FileStatus::kOk, found);
        ASSERT_EQ(FileStatus::kOk, fs->OpenFile(fs->Capability(record.id, kPurpose),
            kPurpose, "database", RecordOpen::kOpen, &file));
        std::vector<uint8_t> bytes(kPayloadSize);
        size_t read = 0;
        ASSERT_EQ(FileStatus::kOk, file->Read(0, bytes, &read));
        ASSERT_EQ(kPayloadSize, read);
        const std::string actual(bytes.begin(), bytes.end());
        if (deleting) {
          EXPECT_EQ(old, actual);
        } else {
          EXPECT_TRUE(actual == old || actual == std::string(kPayloadSize, 'N'));
          if (exit == 0) {
            EXPECT_EQ(std::string(kPayloadSize, 'N'), actual);
          }
        }
      }
      if (exit == 0) {
        reached_acknowledgement = true;
        if (deleting) {
          EXPECT_EQ(FileStatus::kNotFound, found);
        }
        break;
      }
    }
    EXPECT_TRUE(reached_acknowledgement);
    EXPECT_GT(crashes, 0);
    for (auto fence : {PublishFence::kObjectsDirectorySynced,
                       PublishFence::kRootWritten, PublishFence::kRootSynced,
                       PublishFence::kRootRenamed,
                       PublishFence::kRootDirectorySynced,
                       PublishFence::kObsoleteObjectsUnlinked,
                       PublishFence::kObsoleteDirectorySynced}) {
      EXPECT_TRUE(covered.contains(fence)) << static_cast<int>(fence);
    }
    if (!deleting) {
      EXPECT_TRUE(covered.contains(PublishFence::kObjectWritten));
      EXPECT_TRUE(covered.contains(PublishFence::kObjectSynced));
    } else {
      EXPECT_TRUE(covered.contains(PublishFence::kRecordDirectoryUnlinked));
      EXPECT_TRUE(covered.contains(PublishFence::kRecordDirectorySynced));
    }
    RecordProperty("crashed_fence_occurrences", crashes);
    RecordProperty("covered_fence_types", static_cast<int>(covered.size()));
  }
};

TEST_F(RecordFileCrashTest, ActualProcessCrashAtEveryWritePublishFence) {
  RunEveryFence(false);
}

TEST_F(RecordFileCrashTest, ActualProcessCrashAtEveryDeletionPublishFence) {
  RunEveryFence(true);
}

}  // namespace
}  // namespace alloy::site_records
