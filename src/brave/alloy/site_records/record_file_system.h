// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_FILE_SYSTEM_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_FILE_SYSTEM_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "brave/alloy/site_records/encrypted_file_contents.h"

class GURL;

namespace alloy::site_records {

struct RecordInfo {
  base::UnguessableToken id;
  base::UnguessableToken generation;
  ObjectId directory{};
  std::string domain;
  int64_t created_us = 0;
  std::vector<uint8_t> metadata;
};

struct RecordFileInfo {
  ObjectId identity{};
  uint64_t length = 0;
  int64_t modified_us = 0;
  bool directory = false;
};

struct RecordDirectoryEntry {
  std::string name;
  RecordFileInfo info;
};

enum class RecordOpen { kOpen, kOpenOrCreate, kCreate, kCreateOrTruncate };
enum class RecordDeletion { kDeleted, kDeletedFilesPending };
enum class RecordFileLock { kNone, kShared, kReserved, kPending, kExclusive };

class RecordFile;
struct RecordFileState;

// The only persistence owner. All native adapters share this coordinator and
// use capability-bound virtual names; none receives a native ciphertext FD or
// a raw key. Its mutex covers admission, file revisions, namespace/catalogue
// publication and generation revocation. Different processes cannot share an
// unlocked profile: its directory inode is locked for this owner's lifetime.
class RecordFileSystem : public std::enable_shared_from_this<RecordFileSystem> {
 public:
  static FileStatus Create(const base::FilePath& path,
                           SecretKey profile_key,
                           std::vector<uint8_t> public_bootstrap,
                           std::shared_ptr<RecordFileSystem>* out);
  static FileStatus Open(const base::FilePath& path,
                         SecretKey profile_key,
                         std::shared_ptr<RecordFileSystem>* out);
  static FileStatus ReadBootstrap(const base::FilePath& path,
                                  std::vector<uint8_t>* out);
  ~RecordFileSystem();
  RecordFileSystem(const RecordFileSystem&) = delete;
  RecordFileSystem& operator=(const RecordFileSystem&) = delete;

  // Explicit lock first flushes completed logical writes, then revokes and
  // wipes even if flushing fails. The error is returned to the lifecycle owner;
  // failure is never reported as a successful durable close. Destructor is an
  // emergency wipe, not a substitute for handling native Close/Sync errors.
  FileStatus Lock();
  bool unlocked() const;
  FileStatus Sync();
  base::UnguessableToken profile_id() const;
  std::vector<uint8_t> bootstrap() const;

  // Browser-owned principal only; R16 grouping never changes origin/StorageKey
  // security checks inside the selected shard. Domains and private metadata
  // are encrypted, and record names are full profile-keyed HMAC-SHA-256 values.
  FileStatus ResolveRecord(const GURL& browser_owned_url,
                            const url::Origin& browser_owned_origin,
                            bool create,
                            RecordInfo* out);
  FileStatus ListRecords(std::vector<RecordInfo>* out);
  FileStatus SetRecordMetadata(base::UnguessableToken record,
                               base::span<const uint8_t> metadata);
  RecordCapability Capability(base::UnguessableToken record,
                               StorePurpose purpose);
  // A separate profile-private tree uses profile-derived keys. It is not a
  // site record, never appears in ListRecords and cannot be site-deleted.
  RecordCapability ProfileCapability(StorePurpose purpose);
  FileStatus RecordKeyEnvelopePathForTesting(base::UnguessableToken record,
                                             base::FilePath* out);

  // R13: publish key/root removal and unlink superseded key envelopes first,
  // revoke/wipe every handle, then invoke engine teardown outside the mutex,
  // then remove data objects. No teardown callback precedes key destruction.
  // Admission for the same record directory stays closed through teardown.
  FileStatus DeleteRecord(base::UnguessableToken record,
                           std::function<void()> teardown,
                           RecordDeletion* result);

  FileStatus OpenFile(const RecordCapability& capability,
                      StorePurpose purpose,
                      std::string_view relative_name,
                      RecordOpen mode,
                      std::shared_ptr<RecordFile>* out);
  FileStatus Stat(const RecordCapability& capability,
                  StorePurpose purpose,
                  std::string_view relative_name,
                  RecordFileInfo* out);
  FileStatus List(const RecordCapability& capability,
                  StorePurpose purpose,
                  std::string_view relative_directory,
                  std::vector<RecordDirectoryEntry>* out);
  FileStatus CreateDirectory(const RecordCapability& capability,
                             StorePurpose purpose,
                             std::string_view relative_name,
                             bool recursive = false);
  FileStatus Remove(const RecordCapability& capability,
                    StorePurpose purpose,
                    std::string_view relative_name,
                    bool recursive = false);
  FileStatus Rename(const RecordCapability& capability,
                    StorePurpose purpose,
                    std::string_view from,
                    std::string_view to);

  void SetFaultForTesting(FileFault fault);
  // Observes that coordinator-owned key buffers and live file helpers were
  // actually cleared after Lock, rather than merely checking a lock flag.
  bool KeysClearedForTesting() const;

 private:
  friend class RecordFile;
  struct Impl;
  explicit RecordFileSystem(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// A handle shares file identity and current logical revision with other handles
// in the coordinator. Rename preserves identity; unlink/replace detaches the
// name while keeping old handles usable until close or generation revocation.
// The weak owner cannot keep profile keys alive after lifecycle-owner teardown.
class RecordFile {
 public:
  ~RecordFile();
  RecordFile(const RecordFile&) = delete;
  RecordFile& operator=(const RecordFile&) = delete;
  FileStatus Read(uint64_t offset, base::span<uint8_t> bytes, size_t* read);
  FileStatus Write(uint64_t offset, base::span<const uint8_t> bytes);
  // Bounded coordinator-owned append buffering; all file operations observe
  // pending bytes and lock/revoke wipes them together with the file keys.
  FileStatus AppendBuffered(base::span<const uint8_t> bytes);
  FileStatus FlushBuffered();
  FileStatus SetLength(uint64_t length);
  FileStatus GetInfo(RecordFileInfo* out);
  FileStatus Sync();
  FileStatus Close();
  FileStatus Lock(RecordFileLock lock);
  FileStatus Unlock(RecordFileLock lock = RecordFileLock::kNone);
  FileStatus HasReservedLock(bool* out);
  FileStatus GetLock(RecordFileLock* out);
  bool KeysClearedForTesting() const;

 private:
  friend class RecordFileSystem;
  friend struct RecordFileSystem::Impl;
  RecordFile(std::weak_ptr<RecordFileSystem> owner,
              RecordCapability capability,
              StorePurpose purpose,
              std::shared_ptr<RecordFileState> state);
  std::weak_ptr<RecordFileSystem> owner_;
  RecordCapability capability_;
  const StorePurpose purpose_;
  std::shared_ptr<RecordFileState> state_;
  bool closed_ = false;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_FILE_SYSTEM_H_
