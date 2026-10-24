// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_LEVELDB_ENV_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_LEVELDB_ENV_H_

#include "brave/alloy/site_records/record_file_system.h"
#include "third_party/leveldatabase/env_chromium.h"

namespace alloy::site_records {

// Capability-bound native Env. No file operation delegates to the OS/default
// Env. The owner keeps this alive until its DB/compaction workers have closed.
class RecordLevelDbEnv final : public leveldb::Env {
 public:
  static FileStatus Create(std::shared_ptr<RecordFileSystem> files,
                           RecordCapability cap, StorePurpose purpose,
                           std::unique_ptr<RecordLevelDbEnv>* out);
  ~RecordLevelDbEnv() override;
  FileStatus last_status() const;
  leveldb_env::Options Options(leveldb_env::Options options = {});

  leveldb::Status NewSequentialFile(const std::string&, leveldb::SequentialFile**) override;
  leveldb::Status NewRandomAccessFile(const std::string&, leveldb::RandomAccessFile**) override;
  leveldb::Status NewWritableFile(const std::string&, leveldb::WritableFile**) override;
  leveldb::Status NewAppendableFile(const std::string&, leveldb::WritableFile**) override;
  bool FileExists(const std::string&) override;
  leveldb::Status GetChildren(const std::string&, std::vector<std::string>*) override;
  leveldb::Status RemoveFile(const std::string&) override;
  leveldb::Status CreateDir(const std::string&) override;
  leveldb::Status RemoveDir(const std::string&) override;
  leveldb::Status GetFileSize(const std::string&, uint64_t*) override;
  leveldb::Status RenameFile(const std::string&, const std::string&) override;
  leveldb::Status LockFile(const std::string&, leveldb::FileLock**) override;
  leveldb::Status UnlockFile(leveldb::FileLock*) override;
  void Schedule(void (*)(void*), void*) override;
  void StartThread(void (*)(void*), void*) override;
  leveldb::Status GetTestDirectory(std::string*) override;
  leveldb::Status NewLogger(const std::string&, leveldb::Logger**) override;
  uint64_t NowMicros() override;
  void SleepForMicroseconds(int) override;

 private:
  struct State;
  explicit RecordLevelDbEnv(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_LEVELDB_ENV_H_
