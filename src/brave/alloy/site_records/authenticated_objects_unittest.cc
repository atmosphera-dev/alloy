// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/authenticated_objects.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <type_traits>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class AuthenticatedObjectsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                    true, &directory_));
    ASSERT_EQ(FileStatus::kOk, RandomKey(&key_));
    page_.fill(0x57);
  }

  size_t CountFiles() {
    base::FileEnumerator files(temp_.GetPath(), false,
                               base::FileEnumerator::FILES);
    size_t count = 0;
    while (!files.Next().empty()) {
      ++count;
    }
    return count;
  }

  const std::vector<uint8_t> binding_{1, 2, 3, 4};
  base::ScopedTempDir temp_;
  std::unique_ptr<ObjectDirectory> directory_;
  SecretKey key_;
  ObjectPage page_{};
};

TEST_F(AuthenticatedObjectsTest, OpaqueCiphertextRoundTripsAfterReopen) {
  ObjectRef ref;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 123, page_,
                              &ref));
  ASSERT_EQ(FileStatus::kOk, directory_->Sync());
  auto physical = base::ReadFileToBytes(
      temp_.GetPath().AppendASCII(ObjectName(ref.id)));
  ASSERT_TRUE(physical);
  ASSERT_EQ(4112u, physical->size());
  EXPECT_EQ(physical->end(),
            std::search(physical->begin(), physical->end(), page_.begin(),
                        page_.begin() + 32));
  directory_.reset();
  ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                  true, &directory_));
  ObjectPage output{};
  ASSERT_EQ(FileStatus::kOk,
            directory_->Read(ObjectType::kData, key_, binding_, 123, ref,
                              &output));
  EXPECT_EQ(page_, output);
}

TEST_F(AuthenticatedObjectsTest, WrongKeyPositionTypeAndIdentityFailClosed) {
  ObjectRef ref;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 123, page_,
                              &ref));
  SecretKey wrong;
  ASSERT_EQ(FileStatus::kOk, RandomKey(&wrong));
  ObjectPage output{};
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, wrong, binding_, 123, ref,
                              &output));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 124, ref,
                              &output));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kNamespace, key_, binding_, 123, ref,
                              &output));
  for (size_t i = 0; i < binding_.size(); ++i) {
    auto wrong_identity = binding_;
    base::span(wrong_identity)[i] ^= 1;
    EXPECT_EQ(FileStatus::kIntegrity,
              directory_->Read(ObjectType::kData, key_, wrong_identity, 123,
                                ref, &output));
  }
  EXPECT_EQ(ObjectPage{}, output);
}

TEST_F(AuthenticatedObjectsTest, PartialOldExtentReplayAndTruncationAreErrors) {
  ObjectRef old_ref;
  ObjectRef current;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 0, page_,
                              &old_ref));
  page_.fill(0x31);
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 0, page_,
                              &current));
  const auto old = base::ReadFileToBytes(
      temp_.GetPath().AppendASCII(ObjectName(old_ref.id)));
  ASSERT_TRUE(old);
  const auto path = temp_.GetPath().AppendASCII(ObjectName(current.id));
  ASSERT_TRUE(base::WriteFile(path, *old));
  ObjectPage output{};
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 0, current,
                              &output));
  // Replacing the expected digest too cannot defeat the ID-bound AEAD.
  current.digest = Digest(*old);
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 0, current,
                              &output));
  ASSERT_TRUE(base::WriteFile(path, base::span(*old).first(4000u)));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 0, current,
                              &output));
  ASSERT_TRUE(base::DeleteFile(path));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 0, current,
                              &output));
}

TEST_F(AuthenticatedObjectsTest, TornWriteRetryNeverReusesAnExtent) {
  base::FilePath torn;
  directory_->SetFaultForTesting(
      [&](PublishFence fence, const base::FilePath& path) {
        if (fence == PublishFence::kObjectWritten) {
          torn = path;
          base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
          EXPECT_TRUE(file.SetLength(17));
          return FileStatus::kIo;
        }
        return FileStatus::kOk;
      });
  ObjectRef ref;
  EXPECT_EQ(FileStatus::kIo,
            directory_->Seal(ObjectType::kData, key_, binding_, 0, page_,
                              &ref));
  EXPECT_TRUE(ref.empty());
  directory_->SetFaultForTesting({});
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 0, page_,
                              &ref));
  EXPECT_NE(torn, temp_.GetPath().AppendASCII(ObjectName(ref.id)));
  EXPECT_EQ(2u, CountFiles());
  ObjectPage output{};
  EXPECT_EQ(FileStatus::kOk,
            directory_->Read(ObjectType::kData, key_, binding_, 0, ref,
                              &output));
  EXPECT_EQ(page_, output);
}

TEST_F(AuthenticatedObjectsTest, AtomicRootFencesHaveExplicitRecoveryOutcome) {
  for (auto fence : {PublishFence::kRootWritten, PublishFence::kRootSynced,
                     PublishFence::kRootRenamed,
                     PublishFence::kRootDirectorySynced}) {
    SCOPED_TRACE(static_cast<int>(fence));
    directory_->SetFaultForTesting({});
    page_.fill(0x11);
    ASSERT_EQ(FileStatus::kOk, directory_->PublishRoot(key_, binding_, page_));
    directory_->SetFaultForTesting(
        [fence](PublishFence current, const base::FilePath&) {
          return current == fence ? FileStatus::kIo : FileStatus::kOk;
        });
    page_.fill(0x22);
    const bool renamed = fence == PublishFence::kRootRenamed ||
                         fence == PublishFence::kRootDirectorySynced;
    EXPECT_EQ(renamed ? FileStatus::kCommitUncertain : FileStatus::kIo,
              directory_->PublishRoot(key_, binding_, page_));
    directory_.reset();
    ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                    true, &directory_));
    ObjectPage output{};
    std::vector<uint8_t> bootstrap;
    ASSERT_EQ(FileStatus::kOk,
              directory_->ReadRoot(key_, &output, &bootstrap));
    ObjectPage expected;
    expected.fill(renamed ? 0x22 : 0x11);
    EXPECT_EQ(expected, output);
    EXPECT_EQ(binding_, bootstrap);
    EXPECT_EQ(1u, CountFiles());
  }
}

TEST_F(AuthenticatedObjectsTest, RootBootstrapIsAuthenticatedNoBackupFallback) {
  ASSERT_EQ(FileStatus::kOk, directory_->PublishRoot(key_, binding_, page_));
  std::vector<uint8_t> bootstrap;
  ASSERT_EQ(FileStatus::kOk, directory_->ReadBootstrap(&bootstrap));
  EXPECT_EQ(binding_, bootstrap);
  const auto path = temp_.GetPath().AppendASCII("root");
  auto bytes = base::ReadFileToBytes(path);
  ASSERT_TRUE(bytes);
  base::span(*bytes)[44] ^= 1;
  ASSERT_TRUE(base::WriteFile(path, *bytes));
  ObjectPage output{};
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->ReadRoot(key_, &output, &bootstrap));
  ASSERT_TRUE(base::DeleteFile(path));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->ReadRoot(key_, &output, &bootstrap));
}

TEST_F(AuthenticatedObjectsTest, ExclusiveOwnerAndSymlinkAdmission) {
  std::unique_ptr<ObjectDirectory> second;
  EXPECT_EQ(FileStatus::kBusy,
            ObjectDirectory::Open(temp_.GetPath(), false, true, &second));
  ObjectId child_id;
  ASSERT_EQ(FileStatus::kOk, RandomId(&child_id));
  const auto path = temp_.GetPath().AppendASCII(ObjectName(child_id));
  ASSERT_EQ(0, symlink(temp_.GetPath().value().c_str(), path.value().c_str()));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Child(child_id, false, &second));
}

TEST_F(AuthenticatedObjectsTest, PruneTwiceRemovesEverySupersededKeyPage) {
  ObjectRef live;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kCatalogue, key_, binding_, 0, page_,
                              &live));
  for (int iteration = 0; iteration < 2; ++iteration) {
    ObjectRef obsolete;
    ASSERT_EQ(FileStatus::kOk,
              directory_->Seal(ObjectType::kCatalogue, key_, binding_, 0,
                                page_, &obsolete));
    ASSERT_EQ(FileStatus::kOk, directory_->Prune({live.id}));
    EXPECT_EQ(1u, CountFiles());
  }
}

TEST_F(AuthenticatedObjectsTest, SubstitutedFifoCannotBlockObjectOrRootRead) {
  ObjectRef ref;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kData, key_, binding_, 0, page_,
                              &ref));
  const auto path = temp_.GetPath().AppendASCII(ObjectName(ref.id));
  ASSERT_TRUE(base::DeleteFile(path));
  ASSERT_EQ(0, mkfifo(path.value().c_str(), 0600));
  ObjectPage output{};
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->Read(ObjectType::kData, key_, binding_, 0, ref,
                              &output));
  const auto root = temp_.GetPath().AppendASCII("root");
  ASSERT_EQ(0, mkfifo(root.value().c_str(), 0600));
  std::vector<uint8_t> bootstrap;
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->ReadBootstrap(&bootstrap));
  EXPECT_EQ(FileStatus::kIntegrity,
            directory_->ReadRoot(key_, &output, &bootstrap));
}

TEST(SecretKeyTest, MoveReplacementAndExplicitLockWipe) {
  static_assert(!std::is_copy_constructible_v<SecretKey>);
  ObjectId material;
  material.fill(0x39);
  SecretKey first(material);
  SecretKey second(std::move(first));
  EXPECT_TRUE(std::ranges::all_of(first.bytes(), [](auto b) { return b == 0; }));
  EXPECT_TRUE(std::ranges::equal(material, second.bytes()));
  first = std::move(second);
  EXPECT_TRUE(std::ranges::all_of(second.bytes(), [](auto b) { return b == 0; }));
  first.Clear();
  EXPECT_TRUE(std::ranges::all_of(first.bytes(), [](auto b) { return b == 0; }));
}

TEST(SecretKeyTest, WrappingBindsIdentityAndDoesNotExposeTheRecordKey) {
  SecretKey wrapper;
  SecretKey record;
  ASSERT_EQ(FileStatus::kOk, RandomKey(&wrapper));
  ASSERT_EQ(FileStatus::kOk, RandomKey(&record));
  const std::vector<uint8_t> identity{1, 2, 3};
  std::vector<uint8_t> envelope;
  ASSERT_EQ(FileStatus::kOk, WrapKey(wrapper, record, identity, &envelope));
  ASSERT_EQ(80u, envelope.size());
  EXPECT_EQ(envelope.end(), std::search(envelope.begin(), envelope.end(),
                                        record.bytes().begin(), record.bytes().end()));
  SecretKey unwrapped;
  ASSERT_EQ(FileStatus::kOk,
            UnwrapKey(wrapper, envelope, identity, &unwrapped));
  EXPECT_TRUE(std::ranges::equal(record.bytes(), unwrapped.bytes()));
  const std::vector<uint8_t> wrong{1, 2, 4};
  EXPECT_EQ(FileStatus::kIntegrity,
            UnwrapKey(wrapper, envelope, wrong, &unwrapped));
  base::span(envelope)[79] ^= 1;
  EXPECT_EQ(FileStatus::kIntegrity,
            UnwrapKey(wrapper, envelope, identity, &unwrapped));
}

}  // namespace
}  // namespace alloy::site_records
