// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_pref_store.h"

#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"

namespace alloy::site_records {
namespace {
constexpr uint64_t kMaxPrefBytes = 64 * 1024 * 1024;
}  // namespace

RecordPrefStore::RecordPrefStore(std::shared_ptr<RecordFileSystem> files,
                                 RecordCapability capability,
                                 StorePurpose purpose,
                                 std::string name,
                                 ErrorCallback on_error)
    : files_(std::move(files)), capability_(std::move(capability)),
      purpose_(purpose), name_(std::move(name)), on_error_(std::move(on_error)) {
  CHECK(on_error_);
  if (purpose_ != StorePurpose::kSiteSettings &&
      purpose_ != StorePurpose::kProfileSettings) {
    status_ = FileStatus::kDenied;
  }
}

RecordPrefStore::~RecordPrefStore() {
  // Normal owners explicitly flush and inspect the result before destruction.
  // Like JsonPrefStore, destruction also lands a still-pending write; it cannot
  // turn an unobserved I/O failure into a successful close acknowledgement.
  if (dirty_ && Admitted()) FlushImpl();
}

bool RecordPrefStore::Admitted() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return !invalidated_ && status_ == FileStatus::kOk && files_ &&
      capability_.Allows(purpose_, StorageAccess::kReadRecordFiles);
}

bool RecordPrefStore::GetValue(std::string_view key,
                               const base::Value** result) const {
  return initialized_ && Admitted() && prefs_.GetValue(key, result);
}

base::DictValue RecordPrefStore::GetValues() const {
  return initialized_ && Admitted() ? prefs_.AsDict() : base::DictValue();
}

bool RecordPrefStore::GetMutableValue(std::string_view key, base::Value** result) {
  return initialized_ && Admitted() && prefs_.GetValue(key, result);
}

void RecordPrefStore::AddObserver(PrefStore::Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void RecordPrefStore::RemoveObserver(PrefStore::Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

bool RecordPrefStore::HasObservers() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return !observers_.empty();
}

bool RecordPrefStore::IsInitializationComplete() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return initialized_;
}

void RecordPrefStore::Notify(std::string_view key) {
  scoped_refptr<RecordPrefStore> keep_alive(this);
  observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnPrefValueChanged, key);
}

void RecordPrefStore::ScheduleWrite() {
  if (!write_timer_.IsRunning()) write_timer_.Start(FROM_HERE, base::Seconds(1),
      base::BindOnce([](RecordPrefStore* store) { store->Flush(); }, base::Unretained(this)));
}

void RecordPrefStore::ReportValueChanged(std::string_view key, uint32_t flags) {
  if (!initialized_ || !Admitted()) return;
  dirty_ = true;
  ScheduleWrite();
  Notify(key);
}

void RecordPrefStore::SetValue(std::string_view key, base::Value value,
                               uint32_t flags) {
  if (!initialized_ || !Admitted()) return;
  if (prefs_.SetValue(key, std::move(value))) ReportValueChanged(key, flags);
}

void RecordPrefStore::SetValueSilently(std::string_view key, base::Value value,
                                       uint32_t flags) {
  if (!initialized_ || !Admitted()) return;
  dirty_ |= prefs_.SetValue(key, std::move(value));
  if (dirty_) ScheduleWrite();
}

void RecordPrefStore::RemoveValue(std::string_view key, uint32_t flags) {
  if (!initialized_ || !Admitted()) return;
  if (prefs_.RemoveValue(key)) ReportValueChanged(key, flags);
}

void RecordPrefStore::RemoveValuesByPrefixSilently(std::string_view prefix) {
  if (!initialized_ || !Admitted()) return;
  prefs_.ClearWithPrefix(prefix);
  dirty_ = true;
  ScheduleWrite();
}

bool RecordPrefStore::ReadOnly() const { return !Admitted(); }

FileStatus RecordPrefStore::last_status() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return status_ != FileStatus::kOk ? status_ :
      Admitted() ? FileStatus::kOk : FileStatus::kDenied;
}

PersistentPrefStore::PrefReadError RecordPrefStore::GetReadError() const {
  return last_status() == FileStatus::kOk ? PREF_READ_ERROR_NONE :
      last_status() == FileStatus::kDenied ? PREF_READ_ERROR_ACCESS_DENIED :
      PREF_READ_ERROR_FILE_OTHER;
}

FileStatus RecordPrefStore::Fail(FileStatus status) {
  if (status_ != FileStatus::kOk) return status_;
  if (status != FileStatus::kOk) {
    status_ = status;
    prefs_.Clear();
    dirty_ = false;
    on_error_.Run(status);
  }
  return status;
}

FileStatus RecordPrefStore::Load() {
  if (!Admitted()) return FileStatus::kDenied;
  std::shared_ptr<RecordFile> file;
  auto status = files_->OpenFile(capability_, purpose_, name_, RecordOpen::kOpen, &file);
  // Only an authenticated absent logical name means an initial empty shard.
  if (status == FileStatus::kNotFound) return FileStatus::kOk;
  if (status != FileStatus::kOk) return status;
  RecordFileInfo info;
  status = file->GetInfo(&info);
  if (status != FileStatus::kOk) return status;
  if (info.length > kMaxPrefBytes) return FileStatus::kInvalid;
  std::vector<uint8_t> bytes(info.length);
  size_t read = 0;
  status = file->Read(0, bytes, &read);
  if (status == FileStatus::kOk && read != bytes.size()) status = FileStatus::kIntegrity;
  auto value = status == FileStatus::kOk ?
      base::JSONReader::ReadDict(base::as_string_view(bytes), base::JSON_PARSE_RFC) :
      std::nullopt;
  Cleanse(bytes);
  if (status != FileStatus::kOk) return status;
  if (!value) return FileStatus::kIntegrity;
  // Flat keys preserve native pref names containing dots. GetValues() retains
  // PrefStore's normal expanded representation through PrefValueMap::AsDict.
  for (auto [key, entry] : *value) prefs_.SetValue(key, std::move(entry));
  return file->Close();
}

PersistentPrefStore::PrefReadError RecordPrefStore::ReadPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  scoped_refptr<RecordPrefStore> keep_alive(this);
  if (!initialized_) {
    const auto status = Load();
    // Publish initialization before any external callback can reenter. A
    // failed authenticated load must never be retried as an empty/denied load.
    initialized_ = true;
    Fail(status);
    observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnInitializationCompleted,
                                     status_ == FileStatus::kOk);
  }
  return GetReadError();
}

void RecordPrefStore::ReadPrefsAsync(ReadErrorDelegate* error_delegate) {
  std::unique_ptr<ReadErrorDelegate> delegate(error_delegate);
  const auto error = ReadPrefs();
  if (delegate && error != PREF_READ_ERROR_NONE) delegate->OnError(error);
}

FileStatus RecordPrefStore::Flush() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  scoped_refptr<RecordPrefStore> keep_alive(this);
  return FlushImpl();
}

FileStatus RecordPrefStore::FlushImpl() {
  write_timer_.Stop();
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ?
      FileStatus::kDenied : last_status();
  if (!dirty_) return FileStatus::kOk;
  base::DictValue flat;
  for (const auto& [key, entry] : prefs_) flat.Set(key, entry.Clone());
  std::string bytes;
  if (!base::JSONWriter::Write(flat, &bytes)) return Fail(FileStatus::kInvalid);
  if (bytes.size() > kMaxPrefBytes) {
    Cleanse(base::as_writable_byte_span(bytes));
    return Fail(FileStatus::kInvalid);
  }
  const SettingsReplacement replacement{capability_, purpose_, name_, base::as_byte_span(bytes)};
  const auto status = files_->ReplaceSettings(base::span_from_ref(replacement));
  Cleanse(base::as_writable_byte_span(bytes));
  if (status != FileStatus::kOk) return Fail(status);
  dirty_ = false;
  return FileStatus::kOk;
}

void RecordPrefStore::CommitPendingWrite(base::OnceClosure reply_callback,
                                         base::OnceClosure synchronous_done_callback) {
  Flush();
  if (synchronous_done_callback) std::move(synchronous_done_callback).Run();
  if (reply_callback) base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, std::move(reply_callback));
}

void RecordPrefStore::SchedulePendingLossyWrites() { Flush(); }
void RecordPrefStore::OnStoreDeletionFromDisk() { Invalidate(); }
bool RecordPrefStore::HasReadErrorDelegate() const { return false; }

void RecordPrefStore::Invalidate() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  scoped_refptr<RecordPrefStore> keep_alive(this);
  if (invalidated_) return;
  invalidated_ = true;
  write_timer_.Stop();
  std::vector<std::string> changed;
  for (const auto& [key, value] : prefs_) changed.push_back(key);
  prefs_.Clear();
  dirty_ = false;
  for (const auto& key : changed) Notify(key);
}

}  // namespace alloy::site_records
