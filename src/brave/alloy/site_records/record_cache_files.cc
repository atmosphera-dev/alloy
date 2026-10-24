// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_cache_files.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace alloy::site_records {
struct RecordCacheFileOperationsFactory::State {
  std::shared_ptr<RecordFileSystem> files;
  RecordCapability cap;
  StorePurpose purpose;
  std::atomic<FileStatus> last{FileStatus::kOk};

  base::File::Error Result(FileStatus status) {
    if (status == FileStatus::kOk) return base::File::FILE_OK;
    auto prior = last.load();
    while (prior != FileStatus::kIntegrity && prior != FileStatus::kCommitUncertain &&
           !last.compare_exchange_weak(prior, status)) {}
    if (last.load() == FileStatus::kIntegrity || last.load() == FileStatus::kCommitUncertain)
      return base::File::FILE_ERROR_IO;
    switch (status) {
      case FileStatus::kNotFound: return base::File::FILE_ERROR_NOT_FOUND;
      case FileStatus::kExists: return base::File::FILE_ERROR_EXISTS;
      case FileStatus::kDenied: return base::File::FILE_ERROR_ACCESS_DENIED;
      case FileStatus::kBusy: return base::File::FILE_ERROR_IN_USE;
      case FileStatus::kNotEmpty: return base::File::FILE_ERROR_NOT_EMPTY;
      case FileStatus::kNotDirectory: return base::File::FILE_ERROR_NOT_A_DIRECTORY;
      case FileStatus::kIsDirectory: return base::File::FILE_ERROR_NOT_A_FILE;
      default: return base::File::FILE_ERROR_IO;
    }
  }
  bool Active() const { return cap.Allows(purpose, StorageAccess::kReadRecordFiles); }
  FileStatus Path(const base::FilePath& path, std::string* name) {
    if (!path.IsAbsolute() || path.ReferencesParent()) return FileStatus::kInvalid;
    *name = path.value().substr(1);
    return FileStatus::kOk;
  }
  static base::File::Info Info(const RecordFileInfo& value) {
    base::File::Info info;
    info.size = value.length;
    info.is_directory = value.directory;
    info.last_modified = base::Time::UnixEpoch() + base::Microseconds(value.modified_us);
    info.last_accessed = info.last_modified;
    info.creation_time = info.last_modified;
    return info;
  }

  class File final : public disk_cache::CacheFile {
   public:
    File(std::shared_ptr<State> state, std::shared_ptr<RecordFile> file,
         base::File::Error error, bool readonly)
        : state_(std::move(state)), file_(std::move(file)), error_(error), readonly_(readonly) {}
    bool IsValid() const override { return file_ && state_->Active() && error_.load() == base::File::FILE_OK; }
    base::File::Error error_details() const override {
      return !state_->Active() && error_.load() == base::File::FILE_OK
          ? base::File::FILE_ERROR_ACCESS_DENIED : error_.load();
    }
    std::optional<size_t> Read(int64_t offset, base::span<uint8_t> bytes) override {
      std::ranges::fill(bytes, 0);
      if (!IsValid() || offset < 0) return std::nullopt;
      size_t read = 0;
      error_ = state_->Result(file_->Read(offset, bytes, &read));
      return error_.load() == base::File::FILE_OK ? std::optional<size_t>(read) : std::nullopt;
    }
    std::optional<size_t> Write(int64_t offset, base::span<const uint8_t> bytes) override {
      if (!IsValid() || readonly_ || offset < 0) return std::nullopt;
      auto status = file_->Write(offset, bytes);
      // CacheFile has no error-returning Close/Sync API. Publish before returning
      // success, so a failed root commit is visible to the native Write caller.
      if (status == FileStatus::kOk) status = file_->Sync();
      error_ = state_->Result(status);
      return error_.load() == base::File::FILE_OK ? std::optional<size_t>(bytes.size()) : std::nullopt;
    }
    bool GetInfo(base::File::Info* out) override {
      if (!IsValid()) return false;
      RecordFileInfo info;
      error_ = state_->Result(file_->GetInfo(&info));
      if (error_.load() != base::File::FILE_OK) return false;
      *out = State::Info(info);
      return true;
    }
    int64_t GetLength() override {
      base::File::Info info;
      return GetInfo(&info) ? info.size : -1;
    }
    bool SetLength(int64_t length) override {
      if (!IsValid() || readonly_ || length < 0) return false;
      auto status = file_->SetLength(length);
      if (status == FileStatus::kOk) status = file_->Sync();
      error_ = state_->Result(status);
      return error_.load() == base::File::FILE_OK;
    }
    bool ReadAndCheck(int64_t offset, base::span<uint8_t> bytes) override {
      return Read(offset, bytes) == bytes.size();
    }
    bool WriteAndCheck(int64_t offset, base::span<const uint8_t> bytes) override {
      return Write(offset, bytes) == bytes.size();
    }
   private:
    std::shared_ptr<State> state_;
    std::shared_ptr<RecordFile> file_;
    std::atomic<base::File::Error> error_;
    const bool readonly_;
  };

  class Enumerator final : public disk_cache::BackendFileOperations::FileEnumerator {
   public:
    Enumerator(std::shared_ptr<State> state, const base::FilePath& parent)
        : state_(std::move(state)) {
      std::string path;
      auto status = state_->Path(parent, &path);
      std::vector<RecordDirectoryEntry> children;
      if (status == FileStatus::kOk) status = state_->files->List(state_->cap, state_->purpose, path, &children);
      error_ = state_->Result(status) != base::File::FILE_OK;
      if (!error_) for (const auto& child : children) {
        if (child.info.directory) continue;
        const auto info = State::Info(child.info);
        entries_.emplace_back(parent.Append(child.name), info.size, info.last_accessed, info.last_modified);
      }
    }
    std::optional<disk_cache::BackendFileOperations::FileEnumerationEntry> Next() override {
      if (!state_->Active()) { error_ = true; state_->Result(FileStatus::kDenied); return std::nullopt; }
      if (position_ == entries_.size()) return std::nullopt;
      return entries_[position_++];
    }
    bool HasError() const override { return error_; }
   private:
    std::shared_ptr<State> state_;
    std::vector<disk_cache::BackendFileOperations::FileEnumerationEntry> entries_;
    size_t position_ = 0;
    bool error_ = false;
  };

  class Unbound;
  class Operations final : public disk_cache::BackendFileOperations {
   public:
    Operations(std::shared_ptr<State> state, scoped_refptr<base::SequencedTaskRunner> runner)
        : state_(std::move(state)), runner_(std::move(runner)) {
      CHECK(runner_);
      DETACH_FROM_SEQUENCE(sequence_checker_);
    }
    bool CreateDirectory(const base::FilePath& path) override {
      std::string name;
      auto status = Name(path, &name);
      if (status == FileStatus::kOk) status = state_->files->CreateDirectory(state_->cap, state_->purpose, name, true);
      return state_->Result(status) == base::File::FILE_OK;
    }
    bool PathExists(const base::FilePath& path) override { return GetFileInfo(path).has_value(); }
    bool DirectoryExists(const base::FilePath& path) override {
      auto info = GetFileInfo(path);
      return info && info->is_directory;
    }
    std::unique_ptr<disk_cache::CacheFile> OpenFile(const base::FilePath& path, uint32_t flags) override {
      std::string name;
      auto status = Name(path, &name);
      std::shared_ptr<RecordFile> file;
      RecordOpen mode = RecordOpen::kOpen;
      if (flags & base::File::FLAG_CREATE) mode = RecordOpen::kCreate;
      else if (flags & base::File::FLAG_CREATE_ALWAYS) mode = RecordOpen::kCreateOrTruncate;
      else if (flags & base::File::FLAG_OPEN_ALWAYS) mode = RecordOpen::kOpenOrCreate;
      if (status == FileStatus::kOk) status = state_->files->OpenFile(state_->cap, state_->purpose, name, mode, &file);
      if (status == FileStatus::kOk && (flags & base::File::FLAG_OPEN_TRUNCATED)) {
        status = file->SetLength(0);
        if (status == FileStatus::kOk) status = file->Sync();
      }
      const bool readonly = !(flags & (base::File::FLAG_WRITE | base::File::FLAG_APPEND));
      return std::make_unique<File>(state_, std::move(file), state_->Result(status), readonly);
    }
    bool DeleteFile(const base::FilePath& path, DeleteFileMode) override {
      std::string name;
      auto status = Name(path, &name);
      if (status == FileStatus::kOk) status = state_->files->Remove(state_->cap, state_->purpose, name);
      return state_->Result(status) == base::File::FILE_OK;
    }
    bool ReplaceFile(const base::FilePath& from, const base::FilePath& to, base::File::Error* error) override {
      std::string a, b;
      auto status = Name(from, &a);
      if (status == FileStatus::kOk) status = Name(to, &b);
      if (status == FileStatus::kOk) status = state_->files->Rename(state_->cap, state_->purpose, a, b);
      const auto result = state_->Result(status);
      if (error) *error = result;
      return result == base::File::FILE_OK;
    }
    std::optional<base::File::Info> GetFileInfo(const base::FilePath& path) override {
      std::string name;
      auto status = Name(path, &name);
      RecordFileInfo info;
      if (status == FileStatus::kOk) status = state_->files->Stat(state_->cap, state_->purpose, name, &info);
      return state_->Result(status) == base::File::FILE_OK ? std::optional<base::File::Info>(State::Info(info)) : std::nullopt;
    }
    std::unique_ptr<FileEnumerator> EnumerateFiles(const base::FilePath& path) override {
      return std::make_unique<Enumerator>(state_, path);
    }
    void CleanupDirectory(const base::FilePath& path, base::OnceCallback<void(bool)> callback) override {
      std::string name;
      const auto admission = Name(path, &name);
      runner_->PostTask(FROM_HERE, base::BindOnce(
          [](std::shared_ptr<State> state, std::string name, FileStatus admission,
             base::OnceCallback<void(bool)> done) {
            auto status = admission;
            if (status == FileStatus::kOk) status = state->files->Remove(state->cap, state->purpose, name, true);
            if (status == FileStatus::kNotFound) status = FileStatus::kOk;
            const bool success = state->Result(status) == base::File::FILE_OK;
            // Namespace removal/data publication completed before callback.
            // Record-wide deletion itself remains the coordinator's R13 API.
            std::move(done).Run(success);
          }, state_, std::move(name), admission, std::move(callback)));
    }
    std::unique_ptr<disk_cache::UnboundBackendFileOperations> Unbind() override;
    bool IsEncrypted() const override { return true; }
   private:
    FileStatus Name(const base::FilePath& path, std::string* name) {
      DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
      return bound_ ? state_->Path(path, name) : FileStatus::kDenied;
    }
    std::shared_ptr<State> state_;
    scoped_refptr<base::SequencedTaskRunner> runner_;
    bool bound_ = true;
    SEQUENCE_CHECKER(sequence_checker_);
  };
  class Unbound final : public disk_cache::UnboundBackendFileOperations {
   public:
    explicit Unbound(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::unique_ptr<disk_cache::BackendFileOperations> Bind(scoped_refptr<base::SequencedTaskRunner> runner) override {
      CHECK(state_);
      return std::make_unique<Operations>(std::exchange(state_, nullptr), std::move(runner));
    }
   private:
    std::shared_ptr<State> state_;
  };
};

std::unique_ptr<disk_cache::UnboundBackendFileOperations>
RecordCacheFileOperationsFactory::State::Operations::Unbind() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(bound_);
  bound_ = false;
  return std::make_unique<Unbound>(state_);
}

RecordCacheFileOperationsFactory::RecordCacheFileOperationsFactory(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
RecordCacheFileOperationsFactory::~RecordCacheFileOperationsFactory() = default;
FileStatus RecordCacheFileOperationsFactory::Create(std::shared_ptr<RecordFileSystem> files,
    RecordCapability cap, StorePurpose purpose, scoped_refptr<RecordCacheFileOperationsFactory>* out) {
  if (!files || !cap.Allows(purpose, StorageAccess::kWriteRecordFiles)) return FileStatus::kDenied;
  RecordFileInfo root;
  const auto status = files->Stat(cap, purpose, "", &root);
  if (status != FileStatus::kOk) return status;
  auto state = std::make_shared<State>();
  state->files = std::move(files); state->cap = std::move(cap); state->purpose = purpose;
  *out = scoped_refptr<RecordCacheFileOperationsFactory>(new RecordCacheFileOperationsFactory(std::move(state)));
  return FileStatus::kOk;
}
std::unique_ptr<disk_cache::BackendFileOperations> RecordCacheFileOperationsFactory::Create(
    scoped_refptr<base::SequencedTaskRunner> runner) {
  return std::make_unique<State::Operations>(state_, std::move(runner));
}
std::unique_ptr<disk_cache::UnboundBackendFileOperations> RecordCacheFileOperationsFactory::CreateUnbound() {
  return std::make_unique<State::Unbound>(state_);
}
FileStatus RecordCacheFileOperationsFactory::last_status() const { return state_->last.load(); }
}  // namespace alloy::site_records
