// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_CACHE_FILES_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_CACHE_FILES_H_

#include "brave/alloy/site_records/record_file_system.h"
#include "net/disk_cache/disk_cache.h"

namespace alloy::site_records {
// Inject into a record-owned Simple backend before entry lookup. Absolute
// paths are virtual namespace names; the factory's capability selects the key.
class RecordCacheFileOperationsFactory final : public disk_cache::BackendFileOperationsFactory {
 public:
  static FileStatus Create(std::shared_ptr<RecordFileSystem> files,
                           RecordCapability cap, StorePurpose purpose,
                           scoped_refptr<RecordCacheFileOperationsFactory>* out);
  std::unique_ptr<disk_cache::BackendFileOperations> Create(
      scoped_refptr<base::SequencedTaskRunner> task_runner) override;
  std::unique_ptr<disk_cache::UnboundBackendFileOperations> CreateUnbound() override;
  FileStatus last_status() const;

 private:
  struct State;
  explicit RecordCacheFileOperationsFactory(std::shared_ptr<State> state);
  ~RecordCacheFileOperationsFactory() override;
  std::shared_ptr<State> state_;
};
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_CACHE_FILES_H_
