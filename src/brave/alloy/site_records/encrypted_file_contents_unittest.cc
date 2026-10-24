// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_file_contents.h"

#include <algorithm>
#include <limits>
#include <map>
#include <string>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

constexpr auto kPurpose = StorePurpose::kIndexedDb;

class EncryptedFileContentsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                    true, &directory_));
    ASSERT_EQ(FileStatus::kOk, RandomKey(&key_));
    owner_ = std::make_unique<RecordCapabilityIssuer>(
        base::UnguessableToken::Create(), base::UnguessableToken::Create(),
        base::UnguessableToken::Create(), StorageMode::kEncrypted);
    ASSERT_EQ(FileStatus::kOk,
              EncryptedFileContents::Create(*directory_, key_,
                                              owner_->Issue(kPurpose), kPurpose,
                                              &file_));
  }

  void Reopen() {
    const auto root = file_->root();
    file_.reset();
    ASSERT_EQ(FileStatus::kOk,
              EncryptedFileContents::Open(*directory_, key_,
                                            owner_->Issue(kPurpose), kPurpose,
                                            root, &file_));
  }

  void ExpectBytes(uint64_t offset, base::span<const uint8_t> expected) {
    std::vector<uint8_t> output(expected.size(), 0xab);
    size_t read = 999;
    ASSERT_EQ(FileStatus::kOk, file_->Read(offset, output, &read));
    EXPECT_EQ(expected.size(), read);
    EXPECT_TRUE(std::ranges::equal(expected, output));
  }

  base::ScopedTempDir temp_;
  SecretKey key_;
  std::unique_ptr<RecordCapabilityIssuer> owner_;
  std::unique_ptr<ObjectDirectory> directory_;
  std::unique_ptr<EncryptedFileContents> file_;
};

TEST_F(EncryptedFileContentsTest, RandomAccessCrossChunkWritesAndShortReads) {
  std::vector<uint8_t> expected(11000, 0);
  for (int round = 0; round < 30; ++round) {
    const size_t offset = (round * 379) % 9500;
    std::vector<uint8_t> bytes(1500, round + 1);
    ASSERT_EQ(FileStatus::kOk, file_->Write(offset, bytes));
    base::span(expected).subspan(offset, bytes.size()).copy_from(bytes);
  }
  expected.resize(file_->root().length);
  ExpectBytes(0, expected);
  ASSERT_EQ(FileStatus::kOk, directory_->Sync());
  Reopen();
  ExpectBytes(0, expected);
  std::vector<uint8_t> output(100, 0xff);
  size_t read = 999;
  ASSERT_EQ(FileStatus::kOk, file_->Read(expected.size() - 13, output, &read));
  EXPECT_EQ(13u, read);
  EXPECT_TRUE(std::ranges::equal(base::span(expected).last(13u),
                                 base::span(output).first(13u)));
  EXPECT_TRUE(std::ranges::all_of(base::span(output).subspan(13u),
                                  [](uint8_t b) { return b == 0; }));
  ASSERT_EQ(FileStatus::kOk, file_->Read(expected.size() + 1, output, &read));
  EXPECT_EQ(0u, read);
  EXPECT_TRUE(std::ranges::all_of(output, [](uint8_t b) { return b == 0; }));
}

TEST_F(EncryptedFileContentsTest, ShrinkThenExtendCannotRecoverOldTail) {
  const std::vector<uint8_t> secret(3 * kRecordChunkSize, 0x5a);
  ASSERT_EQ(FileStatus::kOk, file_->Write(0, secret));
  ASSERT_EQ(FileStatus::kOk, file_->SetLength(kRecordChunkSize + 17));
  ASSERT_EQ(FileStatus::kOk, file_->SetLength(secret.size()));
  Reopen();
  std::vector<uint8_t> expected(secret.size(), 0);
  std::ranges::fill(base::span(expected).first(kRecordChunkSize + 17), 0x5a);
  ExpectBytes(0, expected);
  ASSERT_EQ(FileStatus::kOk, file_->SetLength(0));
  ASSERT_EQ(FileStatus::kOk, file_->SetLength(secret.size()));
  Reopen();
  std::ranges::fill(expected, 0);
  ExpectBytes(0, expected);
}

TEST_F(EncryptedFileContentsTest, TerabyteSparseExtensionDoesNotAllocateHoles) {
  ASSERT_EQ(FileStatus::kOk,
            file_->SetLength(EncryptedFileContents::kMaxLength));
  base::FileEnumerator initial(temp_.GetPath(), false,
                               base::FileEnumerator::FILES);
  EXPECT_TRUE(initial.Next().empty());
  const std::vector<uint8_t> marker{0x61, 0x62, 0x63};
  ASSERT_EQ(FileStatus::kOk,
            file_->Write(EncryptedFileContents::kMaxLength - marker.size(),
                           marker));
  Reopen();
  ExpectBytes(EncryptedFileContents::kMaxLength - marker.size(), marker);
  const std::vector<uint8_t> zeroes(100, 0);
  ExpectBytes(EncryptedFileContents::kMaxLength / 2, zeroes);
  base::FileEnumerator files(temp_.GetPath(), false,
                             base::FileEnumerator::FILES);
  size_t count = 0;
  while (!files.Next().empty()) {
    ++count;
  }
  // One data extent and one authenticated map leaf, regardless of hole size.
  EXPECT_EQ(2u, count);
}

TEST_F(EncryptedFileContentsTest, FaultedMultiChunkWritePreservesLogicalRevision) {
  const std::vector<uint8_t> old(9000, 0x11);
  const std::vector<uint8_t> fresh(9000, 0x22);
  ASSERT_EQ(FileStatus::kOk, file_->Write(0, old));
  const auto root = file_->root();
  int written = 0;
  directory_->SetFaultForTesting(
      [&](PublishFence fence, const base::FilePath&) {
        return fence == PublishFence::kObjectWritten && ++written == 4
                   ? FileStatus::kIo
                   : FileStatus::kOk;
      });
  EXPECT_EQ(FileStatus::kIo, file_->Write(0, fresh));
  EXPECT_EQ(root, file_->root());
  ExpectBytes(0, old);
  directory_->SetFaultForTesting({});
  ASSERT_EQ(FileStatus::kOk, file_->Write(0, fresh));
  EXPECT_EQ(root.revision + 1, file_->root().revision);
  Reopen();
  ExpectBytes(0, fresh);
}

TEST_F(EncryptedFileContentsTest, CorruptionIsLatchedAndNeverRepairedByWrite) {
  base::FilePath data;
  directory_->SetFaultForTesting(
      [&](PublishFence fence, const base::FilePath& path) {
        if (fence == PublishFence::kObjectWritten && data.empty()) {
          data = path;
        }
        return FileStatus::kOk;
      });
  const std::vector<uint8_t> bytes(8000, 0x41);
  ASSERT_EQ(FileStatus::kOk, file_->Write(0, bytes));
  ASSERT_FALSE(data.empty());
  auto ciphertext = base::ReadFileToBytes(data);
  ASSERT_TRUE(ciphertext);
  base::span(*ciphertext)[7] ^= 1;
  ASSERT_TRUE(base::WriteFile(data, *ciphertext));
  std::vector<uint8_t> output(8000, 0xff);
  size_t read = 999;
  EXPECT_EQ(FileStatus::kIntegrity, file_->Read(0, output, &read));
  EXPECT_EQ(0u, read);
  EXPECT_TRUE(std::ranges::all_of(output, [](uint8_t b) { return b == 0; }));
  EXPECT_EQ(FileStatus::kIntegrity, file_->Write(0, bytes));
  EXPECT_EQ(FileStatus::kIntegrity, file_->SetLength(0));
}

TEST_F(EncryptedFileContentsTest, OwnerGenerationPurposeAndKeyAreBound) {
  const std::vector<uint8_t> marker{1, 2, 3};
  ASSERT_EQ(FileStatus::kOk, file_->Write(0, marker));
  const auto root = file_->root();
  std::unique_ptr<EncryptedFileContents> wrong;
  EXPECT_EQ(FileStatus::kDenied,
            EncryptedFileContents::Open(*directory_, key_,
                                          owner_->Issue(kPurpose),
                                          StorePurpose::kCookies, root, &wrong));
  RecordCapabilityIssuer other(base::UnguessableToken::Create(),
                                base::UnguessableToken::Create(),
                                base::UnguessableToken::Create(),
                                StorageMode::kEncrypted);
  EXPECT_EQ(FileStatus::kIntegrity,
            EncryptedFileContents::Open(*directory_, key_,
                                          other.Issue(kPurpose), kPurpose, root,
                                          &wrong));
  SecretKey wrong_key;
  ASSERT_EQ(FileStatus::kOk, RandomKey(&wrong_key));
  EXPECT_EQ(FileStatus::kIntegrity,
            EncryptedFileContents::Open(*directory_, wrong_key,
                                          owner_->Issue(kPurpose), kPurpose,
                                          root, &wrong));
  owner_->Revoke();
  size_t read;
  std::vector<uint8_t> out(20, 0xff);
  EXPECT_EQ(FileStatus::kDenied, file_->Read(0, out, &read));
  EXPECT_EQ(FileStatus::kDenied, file_->Write(0, marker));
  EXPECT_EQ(FileStatus::kDenied, file_->SetLength(0));
  EXPECT_TRUE(std::ranges::all_of(out, [](uint8_t b) { return b == 0; }));
}

TEST_F(EncryptedFileContentsTest, LimitsCannotWrapOffsetsOrRevision) {
  const std::vector<uint8_t> marker{1, 2, 3};
  EXPECT_EQ(FileStatus::kInvalid,
            file_->Write(std::numeric_limits<uint64_t>::max(), marker));
  EXPECT_EQ(FileStatus::kInvalid,
            file_->Write(EncryptedFileContents::kMaxLength - 1, marker));
  EXPECT_EQ(FileStatus::kInvalid,
            file_->SetLength(EncryptedFileContents::kMaxLength + 1));
  auto root = file_->root();
  root.revision = EncryptedFileContents::kMaxRevision;
  std::unique_ptr<EncryptedFileContents> exhausted;
  ASSERT_EQ(FileStatus::kOk,
            EncryptedFileContents::Open(*directory_, key_,
                                          owner_->Issue(kPurpose), kPurpose,
                                          root, &exhausted));
  EXPECT_EQ(FileStatus::kInvalid, exhausted->Write(0, marker));
  EXPECT_EQ(FileStatus::kInvalid, exhausted->SetLength(1));
}

TEST_F(EncryptedFileContentsTest, NoDiskModesCannotConstructEncryptedFiles) {
  for (auto mode : {StorageMode::kStateless, StorageMode::kMemory}) {
    RecordCapabilityIssuer owner(base::UnguessableToken::Create(),
                                  base::UnguessableToken::Create(),
                                  base::UnguessableToken::Create(), mode);
    std::unique_ptr<EncryptedFileContents> refused;
    EXPECT_EQ(FileStatus::kDenied,
              EncryptedFileContents::Create(*directory_, key_,
                                              owner.Issue(kPurpose), kPurpose,
                                              &refused));
    EXPECT_FALSE(refused);
  }
}

}  // namespace
}  // namespace alloy::site_records
