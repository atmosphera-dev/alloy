// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_leveldb_env.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <utility>

#include "base/alloy/buildflags.h"
#include "base/compiler_specific.h"

namespace alloy::site_records {
namespace {
std::string VirtualName(const std::string& name) {
  return name.starts_with('/') ? name.substr(1) : name;
}
}  // namespace

struct RecordLevelDbEnv::State {
  std::shared_ptr<RecordFileSystem> files;
  RecordCapability cap;
  StorePurpose purpose;
  std::atomic<FileStatus> last{FileStatus::kOk};

  leveldb::Status Result(FileStatus status) {
    if (status == FileStatus::kOk) return leveldb::Status::OK();
    auto prior = last.load();
    while (prior != FileStatus::kIntegrity && prior != FileStatus::kCommitUncertain &&
           !last.compare_exchange_weak(prior, status)) {}
    const auto fatal = last.load();
    if (fatal == FileStatus::kIntegrity || fatal == FileStatus::kCommitUncertain)
      return leveldb::Status::IOError("record integrity");
    if (status == FileStatus::kNotFound) return leveldb::Status::NotFound("record file");
    if (status == FileStatus::kInvalid) return leveldb::Status::InvalidArgument("record path");
    return leveldb::Status::IOError("record file unavailable");
  }
  FileStatus Open(const std::string& name, RecordOpen mode, std::shared_ptr<RecordFile>* out) {
    return files->OpenFile(cap, purpose, VirtualName(name), mode, out);
  }

  class Sequential final : public leveldb::SequentialFile {
   public:
    Sequential(std::shared_ptr<State> state, std::shared_ptr<RecordFile> file)
        : state_(std::move(state)), file_(std::move(file)) {}
    leveldb::Status Read(size_t size, leveldb::Slice* result, char* scratch) override {
      *result = leveldb::Slice();
      // Env's native Read contract gives size writable bytes in scratch.
      auto bytes = UNSAFE_BUFFERS(base::span(reinterpret_cast<uint8_t*>(scratch), size));
      size_t read = 0;
      const auto status = file_->Read(offset_, bytes, &read);
      if (status == FileStatus::kOk) {
        offset_ += read;
        *result = leveldb::Slice(scratch, read);
      }
      return state_->Result(status);
    }
    leveldb::Status Skip(uint64_t amount) override {
      RecordFileInfo info;
      const auto status = file_->GetInfo(&info);
      if (status == FileStatus::kOk) offset_ += std::min(amount, info.length > offset_ ? info.length - offset_ : 0);
      return state_->Result(status);
    }
   private:
    std::shared_ptr<State> state_;
    std::shared_ptr<RecordFile> file_;
    uint64_t offset_ = 0;
  };
  class Random final : public leveldb::RandomAccessFile {
   public:
    Random(std::shared_ptr<State> state, std::shared_ptr<RecordFile> file)
        : state_(std::move(state)), file_(std::move(file)) {}
    leveldb::Status Read(uint64_t offset, size_t size, leveldb::Slice* result, char* scratch) const override {
      *result = leveldb::Slice();
      // Env's native Read contract gives size writable bytes in scratch.
      auto bytes = UNSAFE_BUFFERS(base::span(reinterpret_cast<uint8_t*>(scratch), size));
      size_t read = 0;
      const auto status = file_->Read(offset, bytes, &read);
      if (status == FileStatus::kOk) *result = leveldb::Slice(scratch, read);
      return state_->Result(status);
    }
   private:
    std::shared_ptr<State> state_;
    std::shared_ptr<RecordFile> file_;
  };
  class Writable final : public leveldb::WritableFile {
   public:
    Writable(std::shared_ptr<State> state, std::shared_ptr<RecordFile> file)
        : state_(std::move(state)), file_(std::move(file)) {}
    leveldb::Status Append(const leveldb::Slice& data) override {
      return state_->Result(file_->AppendBuffered(base::as_byte_span(
          std::string_view(data.data() ? data.data() : "", data.size()))));
    }
    leveldb::Status Close() override { return state_->Result(file_->Close()); }
    leveldb::Status Flush() override { return state_->Result(file_->FlushBuffered()); }
    leveldb::Status Sync() override { return state_->Result(file_->Sync()); }
   private:
    std::shared_ptr<State> state_;
    std::shared_ptr<RecordFile> file_;
  };
  struct Lock final : leveldb::FileLock {
    std::shared_ptr<State> owner;
    std::shared_ptr<RecordFile> file;
  };
#if BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  class Logger final : public leveldb::Logger {
   public:
    Logger(std::shared_ptr<State> state, std::shared_ptr<RecordFile> file)
        : state_(std::move(state)), file_(std::move(file)) {}
    void Logv(const char* format, std::va_list args) override {
      std::va_list copy;
      va_copy(copy, args);
      const int length = std::vsnprintf(nullptr, 0, format, copy);
      va_end(copy);
      if (length < 0) return;
      std::vector<char> bytes(static_cast<size_t>(length) + 1);
      std::vsnprintf(bytes.data(), bytes.size(), format, args);
      bytes.back() = '\n';
      state_->Result(file_->AppendBuffered(base::as_byte_span(bytes)));
      state_->Result(file_->FlushBuffered());
      Cleanse(base::as_writable_byte_span(bytes));
    }
   private:
    std::shared_ptr<State> state_;
    std::shared_ptr<RecordFile> file_;
  };
#endif
};

RecordLevelDbEnv::RecordLevelDbEnv(std::shared_ptr<State> state) : state_(std::move(state)) {}
RecordLevelDbEnv::~RecordLevelDbEnv() = default;
FileStatus RecordLevelDbEnv::Create(std::shared_ptr<RecordFileSystem> files,
                                     RecordCapability cap, StorePurpose purpose,
                                     std::unique_ptr<RecordLevelDbEnv>* out) {
  if (!files || !cap.Allows(purpose, StorageAccess::kWriteRecordFiles)) return FileStatus::kDenied;
  RecordFileInfo root;
  const auto status = files->Stat(cap, purpose, "", &root);
  if (status != FileStatus::kOk) return status;
  auto state = std::make_shared<State>();
  state->files = std::move(files);
  state->cap = std::move(cap);
  state->purpose = purpose;
  out->reset(new RecordLevelDbEnv(std::move(state)));
  return FileStatus::kOk;
}
FileStatus RecordLevelDbEnv::last_status() const { return state_->last.load(); }
leveldb_env::Options RecordLevelDbEnv::Options(leveldb_env::Options options) {
  options.env = this;
  return options;
}
leveldb::Status RecordLevelDbEnv::NewSequentialFile(const std::string& name, leveldb::SequentialFile** out) {
  *out = nullptr;
  std::shared_ptr<RecordFile> file;
  const auto status = state_->Open(name, RecordOpen::kOpen, &file);
  if (status == FileStatus::kOk) *out = new State::Sequential(state_, std::move(file));
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::NewRandomAccessFile(const std::string& name, leveldb::RandomAccessFile** out) {
  *out = nullptr;
  std::shared_ptr<RecordFile> file;
  const auto status = state_->Open(name, RecordOpen::kOpen, &file);
  if (status == FileStatus::kOk) *out = new State::Random(state_, std::move(file));
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::NewWritableFile(const std::string& name, leveldb::WritableFile** out) {
  *out = nullptr;
  std::shared_ptr<RecordFile> file;
  const auto status = state_->Open(name, RecordOpen::kCreateOrTruncate, &file);
  if (status == FileStatus::kOk) *out = new State::Writable(state_, std::move(file));
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::NewAppendableFile(const std::string& name, leveldb::WritableFile** out) {
  *out = nullptr;
  std::shared_ptr<RecordFile> file;
  const auto status = state_->Open(name, RecordOpen::kOpenOrCreate, &file);
  if (status == FileStatus::kOk) *out = new State::Writable(state_, std::move(file));
  return state_->Result(status);
}
bool RecordLevelDbEnv::FileExists(const std::string& name) {
  RecordFileInfo info;
  const auto status = state_->files->Stat(state_->cap, state_->purpose, VirtualName(name), &info);
  if (status != FileStatus::kNotFound) state_->Result(status);
  return status == FileStatus::kOk;
}
leveldb::Status RecordLevelDbEnv::GetChildren(const std::string& name, std::vector<std::string>* out) {
  out->clear();
  std::vector<RecordDirectoryEntry> entries;
  const auto status = state_->files->List(state_->cap, state_->purpose, VirtualName(name), &entries);
  if (status == FileStatus::kOk) for (const auto& entry : entries) out->push_back(entry.name);
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::RemoveFile(const std::string& name) {
  return state_->Result(state_->files->Remove(state_->cap, state_->purpose, VirtualName(name)));
}
leveldb::Status RecordLevelDbEnv::CreateDir(const std::string& name) {
  return state_->Result(state_->files->CreateDirectory(state_->cap, state_->purpose, VirtualName(name)));
}
leveldb::Status RecordLevelDbEnv::RemoveDir(const std::string& name) {
  return RemoveFile(name);  // The namespace enforces empty-directory removal.
}
leveldb::Status RecordLevelDbEnv::GetFileSize(const std::string& name, uint64_t* out) {
  RecordFileInfo info;
  const auto status = state_->files->Stat(state_->cap, state_->purpose, VirtualName(name), &info);
  if (status == FileStatus::kOk) *out = info.length;
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::RenameFile(const std::string& from, const std::string& to) {
  return state_->Result(state_->files->Rename(state_->cap, state_->purpose, VirtualName(from), VirtualName(to)));
}
leveldb::Status RecordLevelDbEnv::LockFile(const std::string& name, leveldb::FileLock** out) {
  *out = nullptr;
  std::shared_ptr<RecordFile> file;
  auto status = state_->Open(name, RecordOpen::kOpenOrCreate, &file);
  if (status == FileStatus::kOk) status = file->Lock(RecordFileLock::kExclusive);
  if (status == FileStatus::kOk) {
    auto* lock = new State::Lock();
    lock->owner = state_;
    lock->file = std::move(file);
    *out = lock;
  }
  return state_->Result(status);
}
leveldb::Status RecordLevelDbEnv::UnlockFile(leveldb::FileLock* opaque) {
  auto* lock = static_cast<State::Lock*>(opaque);
  if (lock->owner != state_) return leveldb::Status::InvalidArgument("foreign record lock");
  const auto status = lock->file->Unlock();
  lock->file->Close();
  delete lock;
  return state_->Result(status);
}
void RecordLevelDbEnv::Schedule(void (*fn)(void*), void* arg) { leveldb::Env::Default()->Schedule(fn, arg); }
void RecordLevelDbEnv::StartThread(void (*fn)(void*), void* arg) { leveldb::Env::Default()->StartThread(fn, arg); }
uint64_t RecordLevelDbEnv::NowMicros() { return leveldb::Env::Default()->NowMicros(); }
void RecordLevelDbEnv::SleepForMicroseconds(int value) { leveldb::Env::Default()->SleepForMicroseconds(value); }
leveldb::Status RecordLevelDbEnv::GetTestDirectory(std::string* out) {
  *out = "/leveldb-test";
  return CreateDir(*out);
}
leveldb::Status RecordLevelDbEnv::NewLogger(const std::string& name, leveldb::Logger** out) {
  *out = nullptr;
  RecordFileInfo root;
  const auto admission = state_->files->Stat(state_->cap, state_->purpose, "", &root);
  if (admission != FileStatus::kOk) return state_->Result(admission);
#if BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  std::shared_ptr<RecordFile> file;
  const auto status = state_->Open(name, RecordOpen::kCreateOrTruncate, &file);
  if (status == FileStatus::kOk) *out = new State::Logger(state_, std::move(file));
  return state_->Result(status);
#else
  return leveldb::Status::OK();
#endif
}
}  // namespace alloy::site_records
