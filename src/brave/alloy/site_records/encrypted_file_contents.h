// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_FILE_CONTENTS_H_
#define BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_FILE_CONTENTS_H_

#include <memory>
#include <vector>

#include "brave/alloy/site_records/authenticated_objects.h"
#include "brave/alloy/site_records/record_capability.h"

namespace alloy::site_records {

// Stable format-v1 tags. Document/network working-state capabilities have no
// persistent tag. These are independent of StorePurpose's in-memory ordinals.
uint32_t PersistentStoreTag(StorePurpose purpose);

struct FileContentsRoot {
  ObjectId file_id{};
  uint64_t length = 0;
  uint64_t revision = 0;
  ObjectRef chunks;
  friend bool operator==(const FileContentsRoot&, const FileContentsRoot&) =
      default;
};

// Internal random-access implementation, NOT a second persistence entrypoint.
// Its owner is the serialized profile commit coordinator: that owner supplies
// a root selected from the authenticated namespace, shares one instance across
// handles, and publishes Root() through that namespace/catalogue on Sync/Close.
// Passing an arbitrary older FileContentsRoot is not a supported recovery path.
// All calls, including Revoke(), are serialized by the coordinator's barrier.
class EncryptedFileContents {
 public:
  // Format-v1 limits: <=1 TiB logical length, <2^32 mutations per file ID.
  // Hitting a limit fails explicitly. Each chunk seal derives a one-use key
  // from a new random 256-bit extent ID; retries never reuse an old extent.
  static constexpr uint64_t kMaxLength = uint64_t{1} << 40;
  static constexpr uint64_t kMaxRevision = (uint64_t{1} << 32) - 1;

  static FileStatus Create(ObjectDirectory& directory,
                           const SecretKey& record_key,
                           RecordCapability capability,
                           StorePurpose purpose,
                           std::unique_ptr<EncryptedFileContents>* out);
  static FileStatus Open(ObjectDirectory& directory,
                         const SecretKey& record_key,
                         RecordCapability capability,
                         StorePurpose purpose,
                         const FileContentsRoot& authenticated_root,
                         std::unique_ptr<EncryptedFileContents>* out);
  ~EncryptedFileContents();
  EncryptedFileContents(const EncryptedFileContents&) = delete;
  EncryptedFileContents& operator=(const EncryptedFileContents&) = delete;

  FileStatus Read(uint64_t offset, base::span<uint8_t> out, size_t* read);
  FileStatus Write(uint64_t offset, base::span<const uint8_t> bytes);
  FileStatus SetLength(uint64_t length);
  const FileContentsRoot& root() const { return root_; }
  void Revoke();
  bool KeysClearedForTesting() const;
  // Authenticates map pages/bounds and returns live map/data object identities
  // for post-publication GC. Data ciphertext is authenticated on actual read.
  FileStatus Reachable(std::set<ObjectId>* objects);

 private:
  EncryptedFileContents(ObjectDirectory& directory,
                         const SecretKey& record_key,
                         RecordCapability capability,
                         StorePurpose purpose,
                         const FileContentsRoot& root);
  FileStatus Admission(StorageAccess access) const;
  FileStatus Remember(FileStatus status);
  FileStatus ReadChunk(uint64_t index, ObjectPage* page);
  FileStatus SealChunk(uint64_t index,
                       const ObjectPage& page,
                       ObjectRef* chunks);
  FileStatus ValidateMap();

  ObjectDirectory& directory_;
  RecordCapability capability_;
  const StorePurpose purpose_;
  const std::vector<uint8_t> binding_;
  SecretKey file_key_;
  SecretKey map_key_;
  FileContentsRoot root_;
  FileStatus failure_ = FileStatus::kOk;
  bool revoked_ = false;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_FILE_CONTENTS_H_
