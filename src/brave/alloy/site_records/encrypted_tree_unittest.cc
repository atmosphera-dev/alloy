// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_tree.h"

#include <map>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class EncryptedTreeTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                    true, &directory_));
    ASSERT_EQ(FileStatus::kOk, RandomKey(&key_));
    tree_ = std::make_unique<EncryptedTree>(*directory_, ObjectType::kCatalogue,
                                            key_, binding_);
  }

  base::ScopedTempDir temp_;
  SecretKey key_;
  const std::vector<uint8_t> binding_{9, 8, 7};
  std::unique_ptr<ObjectDirectory> directory_;
  std::unique_ptr<EncryptedTree> tree_;
};

TEST_F(EncryptedTreeTest, CowInsertUpdateDeleteKeepsOldRootConsistent) {
  ObjectRef root;
  std::map<ObjectId, std::vector<uint8_t>> expected;
  // Exercise prefixes, arbitrary insertion order, replacement and branch
  // collapse against an independent ordered reference map.
  for (int i = 0; i < 80; ++i) {
    ObjectId key;
    ASSERT_EQ(FileStatus::kOk, RandomId(&key));
    std::vector<uint8_t> value(31 + i, i);
    ObjectRef updated;
    ASSERT_EQ(FileStatus::kOk, tree_->Put(root, key, value, &updated));
    root = updated;
    expected[key] = value;
  }
  const auto original = root;
  const auto original_values = expected;
  size_t i = 0;
  for (auto entry = expected.begin(); entry != expected.end();) {
    ObjectRef updated;
    if (++i % 2 == 0) {
      ASSERT_EQ(FileStatus::kOk, tree_->Erase(root, entry->first, &updated));
      entry = expected.erase(entry);
    } else {
      entry->second = {0xaa, 0xbb};
      ASSERT_EQ(FileStatus::kOk,
                tree_->Put(root, entry->first, entry->second, &updated));
      ++entry;
    }
    root = updated;
  }
  for (const auto& [key, original_value] : original_values) {
    std::vector<uint8_t> value;
    ASSERT_EQ(FileStatus::kOk, tree_->Get(original, key, &value));
    EXPECT_EQ(original_value, value);
    if (expected.contains(key)) {
      ASSERT_EQ(FileStatus::kOk, tree_->Get(root, key, &value));
      EXPECT_EQ(expected.at(key), value);
    } else {
      EXPECT_EQ(FileStatus::kNotFound, tree_->Get(root, key, &value));
    }
  }
  std::map<ObjectId, std::vector<uint8_t>> enumerated;
  ASSERT_EQ(FileStatus::kOk,
            tree_->Visit(root, [&](const ObjectId& key,
                                    base::span<const uint8_t> value) {
              enumerated[key] = {value.begin(), value.end()};
              return FileStatus::kOk;
            }));
  EXPECT_EQ(expected, enumerated);
  std::set<ObjectId> reachable;
  ASSERT_EQ(FileStatus::kOk, tree_->Reachable(root, &reachable));
  EXPECT_EQ(expected.size() * 2 - 1, reachable.size());
  ASSERT_EQ(FileStatus::kOk, directory_->Sync());
  ASSERT_EQ(FileStatus::kOk, directory_->Prune(reachable));
  tree_.reset();
  directory_.reset();
  ASSERT_EQ(FileStatus::kOk, ObjectDirectory::Open(temp_.GetPath(), false,
                                                  true, &directory_));
  tree_ = std::make_unique<EncryptedTree>(*directory_, ObjectType::kCatalogue,
                                          key_, binding_);
  for (const auto& [key, expected_value] : expected) {
    std::vector<uint8_t> value;
    ASSERT_EQ(FileStatus::kOk, tree_->Get(root, key, &value));
    EXPECT_EQ(expected_value, value);
  }
  std::vector<uint8_t> value;
  EXPECT_EQ(FileStatus::kIntegrity,
            tree_->Get(original, original_values.begin()->first, &value));
}

TEST_F(EncryptedTreeTest, SparseNumericKeysDoNotMaterializeHoles) {
  ObjectRef root;
  ObjectId low{};
  ObjectId high{};
  base::span(high).back() = 255;
  const std::vector<uint8_t> value{1};
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, low, value, &root));
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, high, value, &root));
  std::set<ObjectId> reachable;
  ASSERT_EQ(FileStatus::kOk, tree_->Reachable(root, &reachable));
  EXPECT_EQ(3u, reachable.size());
  ObjectId hole{};
  base::span(hole).back() = 100;
  std::vector<uint8_t> result;
  EXPECT_EQ(FileStatus::kNotFound, tree_->Get(root, hole, &result));
  ASSERT_EQ(FileStatus::kOk, tree_->Erase(root, low, &root));
  ASSERT_EQ(FileStatus::kOk, tree_->Erase(root, high, &root));
  EXPECT_TRUE(root.empty());
}

TEST_F(EncryptedTreeTest, MissingOrReplayedChildIsNeverAnAbsentKey) {
  ObjectRef root;
  ObjectId a{};
  ObjectId b{};
  base::span(b).back() = 1;
  const std::vector<uint8_t> old{0x11};
  const std::vector<uint8_t> current{0x22};
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, a, old, &root));
  const auto old_leaf = root;
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, a, current, &root));
  const auto current_leaf = root;
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, b, current, &root));
  auto bytes = base::ReadFileToBytes(
      temp_.GetPath().AppendASCII(ObjectName(old_leaf.id)));
  ASSERT_TRUE(bytes);
  const auto path = temp_.GetPath().AppendASCII(ObjectName(current_leaf.id));
  ASSERT_TRUE(base::WriteFile(path, *bytes));
  std::vector<uint8_t> result;
  EXPECT_EQ(FileStatus::kIntegrity, tree_->Get(root, a, &result));
  ASSERT_TRUE(base::DeleteFile(path));
  EXPECT_EQ(FileStatus::kIntegrity, tree_->Get(root, a, &result));
  EXPECT_EQ(FileStatus::kOk, tree_->Get(root, b, &result));
  std::set<ObjectId> reachable;
  EXPECT_EQ(FileStatus::kIntegrity, tree_->Reachable(root, &reachable));
}

TEST_F(EncryptedTreeTest, AuthenticatedButInvalidNodeIsRejected) {
  ObjectPage malformed{};
  auto bytes = base::span(malformed);
  bytes[0] = 1;
  bytes[1] = 1;
  // Leaves must have bit 256 and no children, even with a valid AEAD tag.
  ObjectRef root;
  ASSERT_EQ(FileStatus::kOk,
            directory_->Seal(ObjectType::kCatalogue, key_, binding_, 0,
                              malformed, &root));
  std::vector<uint8_t> value;
  EXPECT_EQ(FileStatus::kIntegrity, tree_->Get(root, ObjectId{}, &value));
  std::set<ObjectId> reachable;
  EXPECT_EQ(FileStatus::kIntegrity, tree_->Reachable(root, &reachable));
}

TEST_F(EncryptedTreeTest, FailedPathCopyCannotChangeCommittedRoot) {
  ObjectRef root;
  const ObjectId key{};
  const std::vector<uint8_t> old{1, 2, 3};
  const std::vector<uint8_t> fresh{4, 5, 6};
  ASSERT_EQ(FileStatus::kOk, tree_->Put(root, key, old, &root));
  directory_->SetFaultForTesting(
      [](PublishFence fence, const base::FilePath&) {
        return fence == PublishFence::kObjectSynced ? FileStatus::kIo
                                                   : FileStatus::kOk;
      });
  ObjectRef updated;
  EXPECT_EQ(FileStatus::kIo, tree_->Put(root, key, fresh, &updated));
  EXPECT_TRUE(updated.empty());
  std::vector<uint8_t> value;
  ASSERT_EQ(FileStatus::kOk, tree_->Get(root, key, &value));
  EXPECT_EQ(old, value);
}

}  // namespace
}  // namespace alloy::site_records
