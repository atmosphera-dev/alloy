// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_resources.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/alloy/site_records/pref_cache_clear.h"
#include "base/functional/bind.h"
#include "base/location.h"
namespace alloy::site_records {
namespace {
FileStatus Status(const value_store::ValueStore::Status& status) {
  if (status.ok()) return FileStatus::kOk;
  if (status.code == value_store::ValueStore::READ_ONLY) return FileStatus::kDenied;
  return status.IsCorrupted() ? FileStatus::kIntegrity : FileStatus::kIo;
}
void WipeResourceValue(base::Value& value) {
  base::DictValue moved; moved.Set("value", std::move(value)); ClearPrefCache(moved);
}
using Provider = brave_shields::AdBlockCustomResourceProvider;
Provider::ErrorCode ResultCode(FileStatus status) {
  return status == FileStatus::kOk ? Provider::ErrorCode::kOk :
      status == FileStatus::kDenied || status == FileStatus::kCancelled ? Provider::ErrorCode::kCancelled : Provider::ErrorCode::kStorage;
}
}  // namespace
ProfileFilterResources::Result::~Result() { WipeResourceValue(values); }
ProfileFilterResources::ProfileFilterResources(std::shared_ptr<ProfileFilterStorage> storage,
    scoped_refptr<base::SequencedTaskRunner> runner)
    : base::RefCountedDeleteOnSequence<ProfileFilterResources>(runner), storage_(std::move(storage)) {}
ProfileFilterResources::~ProfileFilterResources() = default;
FileStatus ProfileFilterResources::Open() {
  if (closed_ || !storage_->admitted()) return FileStatus::kDenied;
  return values_ ? FileStatus::kOk : storage_->OpenResourceValues(&values_);
}
ProfileFilterResources::Result ProfileFilterResources::Read() {
  Result result;
  if (storage_->mode() == StorageMode::kStateless && !closed_) return result;
  result.status = Open(); if (result.status != FileStatus::kOk) return result;
  auto value = values_->Get("SCRIPTLETS"); result.status = Status(value.status());
  if (result.status != FileStatus::kOk) return result;
  auto dict = value.PassSettings(); const auto* resources = dict.Find("SCRIPTLETS");
  if (resources && !resources->is_list()) result.status = FileStatus::kIntegrity;
  else if (resources) result.values = resources->Clone();
  ClearPrefCache(dict); return result;
}
FileStatus ProfileFilterResources::Write(base::Value values) {
  auto status = Open();
  if (status == FileStatus::kOk) {
    auto result = values_->Set(value_store::ValueStore::IGNORE_QUOTA | value_store::ValueStore::NO_GENERATE_CHANGES, "SCRIPTLETS", values);
    status = Status(result.status());
  }
  WipeResourceValue(values); return status;
}
void ProfileFilterResources::Close() { closed_ = true; values_.reset(); }
Provider::Backend ProfileFilterPreferences::ResourceBackend() {
  if (!resources_) resources_ = base::MakeRefCounted<ProfileFilterResources>(storage_, runner_);
  Provider::Backend backend;
  auto self = base::WrapRefCounted(this); auto resources = resources_;
  backend.available = base::BindRepeating(&ProfileFilterPreferences::available, self);
  backend.can_edit = base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self) { return !self->ReadOnly(); }, self);
  backend.observe_close = base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self, base::RepeatingClosure notify) {
    return self->consumers_closed_.Add(std::move(notify));
  }, self);
  backend.read = base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self,
      scoped_refptr<ProfileFilterResources> resources, Provider::ReadCallback callback) {
    if (!self->available()) { std::move(callback).Run(Provider::ErrorCode::kCancelled, base::Value(base::Value::Type::LIST)); return; }
    auto reply = std::make_shared<Provider::ReadCallback>(std::move(callback));
    const bool posted = self->runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&ProfileFilterResources::Read, resources),
        base::BindOnce([](scoped_refptr<ProfileFilterPreferences> self, std::shared_ptr<Provider::ReadCallback> reply, ProfileFilterResources::Result result) {
          if (result.status == FileStatus::kOk && !self->available()) result.status = FileStatus::kDenied;
          if (result.status != FileStatus::kOk && result.status != FileStatus::kDenied && result.status != FileStatus::kCancelled) self->Failed(result.status);
          if (result.status != FileStatus::kOk) {
            WipeResourceValue(result.values); result.values = base::Value(base::Value::Type::LIST);
          }
          std::move(*reply).Run(ResultCode(result.status), std::move(result.values));
        }, self, reply));
    if (!posted) std::move(*reply).Run(Provider::ErrorCode::kCancelled, base::Value(base::Value::Type::LIST));
  }, self, resources);
  backend.write = base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self,
      scoped_refptr<ProfileFilterResources> resources, base::Value values, Provider::StatusCallback callback) {
    if (self->ReadOnly()) { std::move(callback).Run(self->available() ? Provider::ErrorCode::kReadOnly : Provider::ErrorCode::kCancelled); return; }
    auto reply = std::make_shared<Provider::StatusCallback>(std::move(callback));
    const bool posted = self->runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&ProfileFilterResources::Write, resources, std::move(values)),
        base::BindOnce([](scoped_refptr<ProfileFilterPreferences> self, std::shared_ptr<Provider::StatusCallback> reply, FileStatus status) {
          if (status == FileStatus::kOk && !self->available()) status = FileStatus::kDenied;
          if (status != FileStatus::kOk && status != FileStatus::kDenied && status != FileStatus::kCancelled) self->Failed(status);
          std::move(*reply).Run(ResultCode(status));
        }, self, reply));
    if (!posted) std::move(*reply).Run(Provider::ErrorCode::kCancelled);
  }, self, resources);
  return backend;
}
}  // namespace alloy::site_records
