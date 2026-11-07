// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_STORAGE_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_STORAGE_H_

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/sequence_checker.h"
#include "brave/alloy/site_records/record_file_system.h"
#include "brave/components/brave_shields/core/browser/ad_block_dat_cache_manager.h"
#include "components/value_store/value_store.h"

namespace alloy::site_records {

// One original profile generation, no native path/key and no parent fallback.
// All Read/Write/Remove/Close calls use its single owning I/O sequence. Admission
// and Invalidate are cross-sequence; the lifecycle owner must drain that sequence
// and native consumers before acknowledging lock. Decrypted results belong to
// the consumer and must join that same teardown barrier.
class ProfileFilterStorage final {
 public:
  using Bytes = std::vector<uint8_t>;
  static FileStatus Create(std::shared_ptr<RecordFileSystem> files,
                           RecordCapability original,
                           std::shared_ptr<ProfileFilterStorage>* out);
  ~ProfileFilterStorage();
  bool active() const;
  StorageMode mode() const { return original_.mode(); }
  const RecordCapability& original_generation() const { return original_; }
  bool admitted() const;
  void Invalidate();
  FileStatus Read(std::string_view name, Bytes* out);
  FileStatus Write(std::string_view name, base::span<const uint8_t> bytes);
  FileStatus Remove(std::string_view name);
  void Close();
  FileStatus OpenResourceValues(std::unique_ptr<value_store::ValueStore>* out);

 private:
  ProfileFilterStorage(std::shared_ptr<RecordFileSystem> files,
                       RecordCapability original);
  FileStatus Prepare();
  bool Writable() const;
  static bool ValidName(std::string_view name);
  const std::shared_ptr<RecordFileSystem> files_;
  const RecordCapability original_;
  std::atomic<bool> active_{true};
  bool prepared_ = false;
  std::map<std::string, Bytes, std::less<>> memory_;
  SEQUENCE_CHECKER(sequence_checker_);
};

// Called on the native consumer sequence. Errors return there explicitly,
// including unexpected revocation while an I/O reply was queued. New operations
// check the lifecycle owner's admission cut on the native sequence; accepted IO
// retains its original capability. Successful retired reads are cancelled and
// cleansed, but actual non-OK IO results always reach the error owner. In backed modes only
// authenticated absence becomes a native cache miss. Stateless selects an
// explicit no-store cache: no DAT read/write is admitted and compiled buffers
// are discarded/cleansed, without consulting another owner. Uses the original FIFO.
brave_shields::AdBlockDATCacheManager::Backend MakeProfileFilterDATBackend(
    std::shared_ptr<ProfileFilterStorage> storage,
    scoped_refptr<base::SequencedTaskRunner> runner,
    base::RepeatingCallback<void(FileStatus)> on_error,
    base::RepeatingCallback<bool()> admitted = {});

}  // namespace alloy::site_records
#endif
