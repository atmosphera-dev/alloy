// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_SQLITE_VFS_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_SQLITE_VFS_H_

#include <memory>
#include <string>

#include "brave/alloy/site_records/record_file_system.h"
#include "sql/database.h"
#include "third_party/sqlite/sqlite3.h"

namespace alloy::site_records {

// Explicit, non-default VFS registration for one trusted record/purpose. All
// paths are virtual namespace names, never OS paths used to select a key.
// Keep this registration alive until its SQLite connections are closed. The
// destructor checks that contract instead of leaving dangling VFS callbacks.
class RecordSqliteVfs {
 public:
  static FileStatus Create(std::shared_ptr<RecordFileSystem> files,
                           RecordCapability capability, StorePurpose purpose,
                           std::unique_ptr<RecordSqliteVfs>* out);
  ~RecordSqliteVfs();
  RecordSqliteVfs(const RecordSqliteVfs&) = delete;
  RecordSqliteVfs& operator=(const RecordSqliteVfs&) = delete;

  const char* name() const;
  // Preserve the caller's rollback/WAL/locking options. Both the option and
  // xFetch/xFileControl enforce no mmap. sql's default VFS wrapper is initialized
  // first and remains unchanged; only this explicitly named VFS is selected.
  sql::DatabaseOptions Options(sql::DatabaseOptions options = {}) const;
  FileStatus last_status() const;
  sqlite3_vfs* vfs_for_testing();

 private:
  struct Impl;
  explicit RecordSqliteVfs(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_SQLITE_VFS_H_
