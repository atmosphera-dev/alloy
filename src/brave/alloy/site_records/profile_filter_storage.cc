// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_storage.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/unguessable_token.h"
#include "brave/alloy/site_records/record_value_store.h"
#include "components/value_store/lazy_leveldb.h"
#include "third_party/leveldatabase/leveldb_chrome.h"

namespace alloy::site_records {
namespace {
constexpr auto kPurpose = StorePurpose::kProfileCustomFilters;
constexpr char kStage[] = ".adblock-stage-";
std::string Name(std::string_view name) { return "adblock-"+std::string(name); }
struct CacheRead {
  CacheRead() = default;
  CacheRead(CacheRead&&) = default;
  ~CacheRead() {
    if (primary) Cleanse(*primary);
    if (additional) Cleanse(*additional);
  }
  FileStatus status = FileStatus::kOk;
  std::optional<DATFileDataBuffer> primary, additional;
};
struct CacheWrite {
  explicit CacheWrite(DATFileDataBuffer bytes) : bytes(std::move(bytes)) {}
  CacheWrite(CacheWrite&&) = default;
  ~CacheWrite() { Cleanse(bytes); }
  DATFileDataBuffer bytes;
};
constexpr std::string_view kSnapshotMagic="AlloyDATSnapshot1";
std::string SnapshotName(bool primary){return primary?"dat-snapshot-primary":"dat-snapshot-additional";}
}  // namespace

ProfileFilterStorage::ProfileFilterStorage(std::shared_ptr<RecordFileSystem> files,
                                         RecordCapability original)
    : files_(std::move(files)), original_(std::move(original)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}
FileStatus ProfileFilterStorage::Create(std::shared_ptr<RecordFileSystem> files,
    RecordCapability original, std::shared_ptr<ProfileFilterStorage>* out) {
  if (!out || !original.active()) return FileStatus::kDenied;
  if (original.mode() == StorageMode::kEncrypted) {
    if (!files || files->profile_id() != original.profile() ||
        !original.Allows(kPurpose, StorageAccess::kReadRecordFiles) ||
        !original.Allows(kPurpose, StorageAccess::kWriteRecordFiles)) return FileStatus::kDenied;
  } else {
    if (files) return FileStatus::kDenied;
    if (original.mode() == StorageMode::kMemory &&
        (!original.Allows(kPurpose, StorageAccess::kReadMemory) ||
         !original.Allows(kPurpose, StorageAccess::kWriteMemory))) return FileStatus::kDenied;
  }
  *out = std::shared_ptr<ProfileFilterStorage>(new ProfileFilterStorage(std::move(files), std::move(original)));
  return FileStatus::kOk;
}
ProfileFilterStorage::~ProfileFilterStorage() {
  for (auto& [name, bytes] : memory_) Cleanse(bytes);
}
bool ProfileFilterStorage::admitted() const {
  return active() &&
      original_.Allows(kPurpose, files_ ? StorageAccess::kReadRecordFiles : StorageAccess::kReadMemory);
}
bool ProfileFilterStorage::active() const {
  return active_.load(std::memory_order_acquire) && original_.active();
}
bool ProfileFilterStorage::Writable() const {
  return active_.load(std::memory_order_acquire) &&
      original_.Allows(kPurpose, files_ ? StorageAccess::kWriteRecordFiles : StorageAccess::kWriteMemory);
}
void ProfileFilterStorage::Invalidate() { active_.store(false, std::memory_order_release); }
bool ProfileFilterStorage::ValidName(std::string_view name) {
  return !name.empty() && name.front() != '.' &&
      name.find('/') == name.npos && name.find('\\') == name.npos && name.find('\0') == name.npos;
}
FileStatus ProfileFilterStorage::Prepare() {
  if (prepared_ || !files_) return FileStatus::kOk;
  std::vector<RecordDirectoryEntry> entries;
  auto status = files_->List(original_, kPurpose, "", &entries);
  if (status != FileStatus::kOk && status != FileStatus::kNotFound) return status;
  // An interrupted replacement never becomes authoritative at its final name.
  // Reclaim only this backend's reserved, encrypted staging files on reopen.
  for (const auto& entry : entries) if (entry.name.starts_with(kStage)) {
    if (entry.info.directory) return FileStatus::kIntegrity;
    status = files_->Remove(original_, kPurpose, entry.name);
    if (status != FileStatus::kOk) return status;
  }
  prepared_ = true;
  return FileStatus::kOk;
}
FileStatus ProfileFilterStorage::Read(std::string_view name, Bytes* out) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!admitted()) return FileStatus::kDenied;
  if (!out || !ValidName(name)) return FileStatus::kInvalid;
  auto status = Prepare(); if (status != FileStatus::kOk) return status;
  if (!files_) {
    auto found = memory_.find(name);
    if (found == memory_.end()) return FileStatus::kNotFound;
    *out = found->second; return FileStatus::kOk;
  }
  std::shared_ptr<RecordFile> file;
  status = files_->OpenFile(original_, kPurpose, Name(name), RecordOpen::kOpen, &file);
  if (status != FileStatus::kOk) return status;
  RecordFileInfo info;
  status = file->GetInfo(&info);
  Bytes bytes;
  if (status == FileStatus::kOk && info.length > bytes.max_size()) status = FileStatus::kInvalid;
  if (status == FileStatus::kOk) {
    bytes.resize(static_cast<size_t>(info.length));
    size_t read = 0;
    status = file->Read(0, bytes, &read);
    if (status == FileStatus::kOk && read != bytes.size()) status = FileStatus::kIntegrity;
  }
  const auto closed = file->Close();
  if (status == FileStatus::kOk) status = closed;
  if (status == FileStatus::kOk && !admitted()) status = FileStatus::kDenied;
  if (status == FileStatus::kOk) *out = std::move(bytes);
  Cleanse(bytes);
  return status;
}
FileStatus ProfileFilterStorage::Write(std::string_view name, base::span<const uint8_t> bytes) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!Writable()) return FileStatus::kDenied;
  if (!ValidName(name)) return FileStatus::kInvalid;
  auto status = Prepare(); if (status != FileStatus::kOk) return status;
  if (!files_) {
    auto& previous = memory_[std::string(name)]; Cleanse(previous);
    previous.assign(bytes.begin(), bytes.end()); return FileStatus::kOk;
  }
  const auto stage = std::string(kStage)+base::UnguessableToken::Create().ToString();
  std::shared_ptr<RecordFile> file;
  status = files_->OpenFile(original_, kPurpose, stage, RecordOpen::kCreate, &file);
  if (status != FileStatus::kOk) return status;
  status = file->Write(0, bytes);
  if (status == FileStatus::kOk) status = file->Sync();
  const auto closed = file->Close();
  if (status == FileStatus::kOk) status = closed;
  if (status != FileStatus::kOk) return status;
  return files_->Rename(original_, kPurpose, stage, Name(name));
}
FileStatus ProfileFilterStorage::Remove(std::string_view name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!Writable()) return FileStatus::kDenied;
  if (!ValidName(name)) return FileStatus::kInvalid;
  auto status = Prepare(); if (status != FileStatus::kOk) return status;
  if (!files_) {
    auto found = memory_.find(name);
    if (found == memory_.end()) return FileStatus::kNotFound;
    Cleanse(found->second); memory_.erase(found); return FileStatus::kOk;
  }
  return files_->Remove(original_, kPurpose, Name(name));
}
void ProfileFilterStorage::Close() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Invalidate();
  for (auto& [name, bytes] : memory_) Cleanse(bytes);
  memory_.clear();
}
FileStatus ProfileFilterStorage::OpenResourceValues(std::unique_ptr<value_store::ValueStore>* out) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!admitted() || !out) return FileStatus::kDenied;
  const base::FilePath logical("/adblock-custom-resources");
  if (files_) {
    std::unique_ptr<value_store::LeveldbValueStore> values;
    const auto status = CreateRecordValueStore(files_, original_, kPurpose, logical, &values);
    if (status == FileStatus::kOk) *out = std::move(values);
    return status;
  }
  auto backend = std::make_unique<value_store::LazyLevelDb::Backend>();
  backend->env = leveldb_chrome::NewMemEnv("AdBlock profile resources");
  backend->allow_recovery = false; backend->sync_writes = true;
  backend->admitted = base::BindRepeating([](RecordCapability cap) {
    return cap.Allows(kPurpose, StorageAccess::kReadMemory) && cap.Allows(kPurpose, StorageAccess::kWriteMemory);
  }, original_);
  *out = std::make_unique<value_store::LeveldbValueStore>("AdBlock profile resources", logical, std::move(backend), nullptr);
  return FileStatus::kOk;
}

brave_shields::AdBlockDATCacheManager::Backend MakeProfileFilterDATBackend(
    std::shared_ptr<ProfileFilterStorage> storage,
    scoped_refptr<base::SequencedTaskRunner> runner,
    base::RepeatingCallback<void(FileStatus)> on_error,
    base::RepeatingCallback<bool()> admitted) {
  CHECK(storage && runner && on_error);
  using Manager = brave_shields::AdBlockDATCacheManager;
  Manager::Backend backend;
  backend.read_snapshot=base::BindRepeating([](std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted,
      bool primary,std::string identity,Manager::SnapshotReply reply) {
    if(admitted&&!admitted.Run())return false;
    const bool posted=runner->PostTaskAndReplyWithResult(FROM_HERE,base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage,bool primary,std::string identity) {
          CacheRead result;
          if(storage->mode()==StorageMode::kStateless){
            if(!storage->active())result.status=FileStatus::kDenied;return result;
          }
          DATFileDataBuffer bytes;auto status=storage->Read(SnapshotName(primary),&bytes);
          if(status==FileStatus::kOk){
            const auto header=kSnapshotMagic.size()+64;
            if(bytes.size()<header || !std::equal(kSnapshotMagic.begin(),kSnapshotMagic.end(),bytes.begin()))
              result.status=FileStatus::kIntegrity;
            else if(std::equal(identity.begin(),identity.end(),reinterpret_cast<const char*>(bytes.data()+kSnapshotMagic.size())))
              result.primary=DATFileDataBuffer(bytes.begin()+header,bytes.end());
          }else if(status!=FileStatus::kNotFound)result.status=status;
          Cleanse(bytes);return result;
        },storage,primary,std::move(identity)),base::BindOnce(
            [](std::shared_ptr<ProfileFilterStorage> storage,base::RepeatingCallback<void(FileStatus)> error,
               base::RepeatingCallback<bool()> admitted,Manager::SnapshotReply reply,CacheRead result){
              if(result.status!=FileStatus::kOk){error.Run(result.status);return;}
              if(admitted&&!admitted.Run())return;
              if(result.status==FileStatus::kOk && !(storage->mode()==StorageMode::kStateless?storage->active():storage->admitted()))
                result.status=FileStatus::kDenied;
              if(result.status!=FileStatus::kOk){error.Run(result.status);return;}
              std::move(reply).Run(std::move(result.primary));
             },storage,error,admitted,std::move(reply)));
    if(!posted)error.Run(FileStatus::kCancelled);
    return posted;
  },storage,runner,on_error,admitted);
  backend.write_snapshot=base::BindRepeating([](std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner,base::RepeatingCallback<void(FileStatus)> error,
      base::RepeatingCallback<bool()> admitted,bool primary,std::string identity,
      DATFileDataBuffer bytes,std::shared_ptr<brave_shields::AdBlockLoadEpoch> epoch){
    if(admitted&&!admitted.Run()){Cleanse(bytes);return false;}
    DATFileDataBuffer envelope(kSnapshotMagic.begin(),kSnapshotMagic.end());
    envelope.insert(envelope.end(),identity.begin(),identity.end());envelope.insert(envelope.end(),bytes.begin(),bytes.end());Cleanse(bytes);
    const bool posted=runner->PostTaskAndReplyWithResult(FROM_HERE,base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage,bool primary,CacheWrite bytes,
            std::shared_ptr<brave_shields::AdBlockLoadEpoch> epoch){
          auto status=FileStatus::kOk;auto write=[&]{
            status=storage->mode()==StorageMode::kStateless?(storage->active()?FileStatus::kOk:FileStatus::kDenied):
                storage->Write(SnapshotName(primary),bytes.bytes);
          };
          const bool superseded=epoch&&!epoch->Publish(write);if(!epoch)write();return std::pair{superseded,status};
        },storage,primary,CacheWrite(std::move(envelope)),std::move(epoch)),base::BindOnce(
            [](base::RepeatingCallback<void(FileStatus)> error,std::pair<bool,FileStatus> result){
              if(!result.first&&result.second!=FileStatus::kOk)error.Run(result.second);
            },error));
    if(!posted)error.Run(FileStatus::kCancelled);
    return posted;
  },storage,runner,on_error,admitted);
  backend.read = base::BindRepeating([](std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted,
      Manager::ReadReply reply) {
    if(admitted&&!admitted.Run())return false;
    const bool posted = runner->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage) {
          CacheRead result;
          // Stateless has an explicit no-store cache, not a denied private
          // backend translated into a miss. No private file is consulted.
          if (storage->mode() == StorageMode::kStateless) {
            if (!storage->active()) result.status = FileStatus::kDenied;
            return result;
          }
          for (auto item : {std::pair{"dat-primary", &result.primary}, std::pair{"dat-additional", &result.additional}}) {
            DATFileDataBuffer bytes;
            const auto status = storage->Read(item.first, &bytes);
            if (status == FileStatus::kOk) *item.second = std::move(bytes);
            else if (status != FileStatus::kNotFound) { result.status = status; break; }
          }
          return result;
        }, storage), base::BindOnce([](std::shared_ptr<ProfileFilterStorage> storage,
            base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted,
            Manager::ReadReply reply, CacheRead result) {
          if(result.status!=FileStatus::kOk){error.Run(result.status);return;}
          if(admitted&&!admitted.Run())return;
          if (result.status == FileStatus::kOk &&
              !(storage->mode() == StorageMode::kStateless ? storage->active() : storage->admitted())) result.status = FileStatus::kDenied;
          if (result.status != FileStatus::kOk) {
            if (result.primary) Cleanse(*result.primary);
            if (result.additional) Cleanse(*result.additional);
            error.Run(result.status); return;
          }
          std::move(reply).Run(std::move(result.primary), std::move(result.additional));
        }, storage, error, admitted, std::move(reply)));
    if (!posted) error.Run(FileStatus::kCancelled);
    return posted;
  }, storage, runner, on_error, admitted);
  backend.write = base::BindRepeating([](std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted,
      bool primary, DATFileDataBuffer bytes,
      std::shared_ptr<brave_shields::AdBlockLoadEpoch> epoch) {
    if(admitted&&!admitted.Run()){Cleanse(bytes);return false;}
    const bool posted = runner->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage, bool primary, CacheWrite bytes,
            std::shared_ptr<brave_shields::AdBlockLoadEpoch> epoch) {
          auto status=FileStatus::kOk;
          auto write=[&] {
            status=storage->mode()==StorageMode::kStateless ?
                (storage->active()?FileStatus::kOk:FileStatus::kDenied) :
                storage->Write(primary ? "dat-primary" : "dat-additional",bytes.bytes);
          };
          const bool superseded=epoch&&!epoch->Publish(write);
          if(!epoch)write();
          return std::pair{superseded,status};
        }, storage, primary, CacheWrite(std::move(bytes)),std::move(epoch)), base::BindOnce(
            [](base::RepeatingCallback<void(FileStatus)> error, std::pair<bool,FileStatus> result) {
              if (!result.first && result.second != FileStatus::kOk) error.Run(result.second);
            }, error));
    if (!posted) error.Run(FileStatus::kCancelled);
    return posted;
  }, storage, runner, on_error, admitted);
  backend.drop = base::BindRepeating([](std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted) {
    if(admitted&&!admitted.Run())return false;
    const bool posted = runner->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage) {
          if (storage->mode() == StorageMode::kStateless)
            return storage->active() ? FileStatus::kOk : FileStatus::kDenied;
          for (const char* name : {"dat-primary", "dat-additional", "dat-snapshot-primary", "dat-snapshot-additional"}) {
            const auto status = storage->Remove(name);
            if (status != FileStatus::kOk && status != FileStatus::kNotFound) return status;
          }
          return FileStatus::kOk;
        }, storage), base::BindOnce([](base::RepeatingCallback<void(FileStatus)> error, FileStatus status) {
          if (status != FileStatus::kOk) error.Run(status);
        }, error));
    if (!posted) error.Run(FileStatus::kCancelled);
    return posted;
  }, storage, runner, on_error, admitted);
  return backend;
}
}  // namespace alloy::site_records
