// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/components/brave_shields/core/browser/ad_block_custom_resource_provider.h"
#include <limits>
#include <algorithm>
#include "base/check.h"
#include "base/check_is_test.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/brave_shields/core/browser/brave_shields_utils.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "components/value_store/value_store_factory_impl.h"
#include "components/value_store/value_store_frontend.h"
#include "components/value_store/value_store_task_runner.h"
namespace brave_shields {
namespace {
constexpr char kKey[] = "SCRIPTLETS";
const std::string& Name(const base::Value& resource) {
  static const base::NoDestructor<std::string> empty;
  const auto* name = resource.is_dict() ? resource.GetDict().FindString("name") : nullptr;
  return name ? *name : *empty;
}
bool Valid(const base::Value& value) {
  if (!value.is_dict()) return false;
  const auto& name = Name(value);
  const auto* mime = value.GetDict().FindStringByDottedPath("kind.mime");
  return !name.empty() && base::IsStringASCII(name) && name.starts_with("user-") &&
      name.ends_with(".js") && mime && *mime == "application/javascript" && value.GetDict().FindString("content");
}
auto Find(base::ListValue& values, const std::string& name) {
  return std::ranges::find_if(values, [&](const base::Value& value) { return Name(value) == name; });
}
}  // namespace
AdBlockCustomResourceProvider::AdBlockCustomResourceProvider(const base::FilePath& root,
    std::unique_ptr<AdBlockResourceProvider> defaults) : default_resource_provider_(std::move(defaults)) {
  CHECK(default_resource_provider_);
  if (root.IsAbsolute()) storage_ = std::make_unique<value_store::ValueStoreFrontend>(
      base::MakeRefCounted<value_store::ValueStoreFactoryImpl>(root),
      base::FilePath(FILE_PATH_LITERAL("AdBlock Custom Resources")), "AdBlock Custom Resources",
      base::SequencedTaskRunner::GetCurrentDefault(), value_store::GetValueStoreTaskRunner());
  else CHECK_IS_TEST();
  default_resource_provider_->AddObserver(this);
}
AdBlockCustomResourceProvider::AdBlockCustomResourceProvider(Backend backend,
    std::unique_ptr<AdBlockResourceProvider> defaults)
    : default_resource_provider_(std::move(defaults)), backend_(std::move(backend)) {
  CHECK(default_resource_provider_ && backend_->available && backend_->can_edit &&
        backend_->read && backend_->write && backend_->observe_close);
  retirement_ = backend_->observe_close.Run(base::BindRepeating(
      &AdBlockCustomResourceProvider::Close, weak_ptr_factory_.GetWeakPtr()));
  default_resource_provider_->AddObserver(this);
}
AdBlockCustomResourceProvider::~AdBlockCustomResourceProvider() {
  Close(); default_resource_provider_->RemoveObserver(this);
}
bool AdBlockCustomResourceProvider::Available() const { return !closed_ && (!backend_ || backend_->available.Run()); }
void AdBlockCustomResourceProvider::GetCustomResources(GetCallback callback) {
  GetCustomResourcesWithStatus(base::BindOnce([](GetCallback callback, ErrorCode status, base::Value values) {
    if (status == ErrorCode::kOk) std::move(callback).Run(std::move(values));
  }, std::move(callback)));
}
void AdBlockCustomResourceProvider::GetCustomResourcesWithStatus(ReadCallback callback) {
  if (!Available()) { std::move(callback).Run(ErrorCode::kCancelled, base::Value(base::Value::Type::LIST)); return; }
  if (backend_) { backend_->read.Run(std::move(callback)); return; }
  if (!storage_) { CHECK_IS_TEST(); std::move(callback).Run(ErrorCode::kOk, base::Value(base::Value::Type::LIST)); return; }
  storage_->Get(kKey, base::BindOnce([](ReadCallback callback, std::optional<base::Value> value) {
    if (value && !value->is_list()) { std::move(callback).Run(ErrorCode::kStorage, base::Value(base::Value::Type::LIST)); return; }
    std::move(callback).Run(ErrorCode::kOk, value ? std::move(*value) : base::Value(base::Value::Type::LIST));
  }, std::move(callback)));
}
bool AdBlockCustomResourceProvider::ValidateEdit(StatusCallback* done) {
  if (!Available()) { std::move(*done).Run(ErrorCode::kCancelled); return false; }
  if (backend_ && !backend_->can_edit.Run()) { std::move(*done).Run(ErrorCode::kReadOnly); return false; }
  return true;
}
void AdBlockCustomResourceProvider::AddResource(PrefService* prefs, const base::Value& value, StatusCallback done) {
  if (!ValidateEdit(&done)) return;
  if (!IsDeveloperModeEnabled(prefs) || !Valid(value)) { std::move(done).Run(ErrorCode::kInvalid); return; }
  Enqueue({Kind::kAdd, Name(value), value.Clone(), std::move(done)});
}
void AdBlockCustomResourceProvider::UpdateResource(PrefService* prefs, const std::string& old,
    const base::Value& value, StatusCallback done) {
  if (!ValidateEdit(&done)) return;
  if (!IsDeveloperModeEnabled(prefs) || !Valid(value)) { std::move(done).Run(ErrorCode::kInvalid); return; }
  Enqueue({Kind::kUpdate, old, value.Clone(), std::move(done)});
}
void AdBlockCustomResourceProvider::RemoveResource(PrefService* prefs, const std::string& name, StatusCallback done) {
  if (!ValidateEdit(&done)) return;
  if (!IsDeveloperModeEnabled(prefs)) { std::move(done).Run(ErrorCode::kInvalid); return; }
  Enqueue({Kind::kRemove, name, base::Value(), std::move(done)});
}
void AdBlockCustomResourceProvider::Enqueue(Mutation mutation) { pending_.push_back(std::move(mutation)); Pump(); }
void AdBlockCustomResourceProvider::Pump() {
  if (closed_ || current_ || pending_.empty()) return;
  current_ = std::move(pending_.front()); pending_.pop_front();
  GetCustomResourcesWithStatus(base::BindOnce(&AdBlockCustomResourceProvider::Mutate, weak_ptr_factory_.GetWeakPtr()));
}
void AdBlockCustomResourceProvider::Mutate(ErrorCode status, base::Value resources) {
  if (status != ErrorCode::kOk) { Completed(status); return; }
  if (!resources.is_list()) { Completed(ErrorCode::kStorage); return; }
  auto& values = resources.GetList(); auto found = Find(values, current_->name);
  if (current_->kind == Kind::kAdd) {
    if (found != values.end()) { Completed(ErrorCode::kAlreadyExists); return; }
    values.Append(std::move(current_->resource));
  } else {
    if (found == values.end()) { Completed(ErrorCode::kNotFound); return; }
    if (current_->kind == Kind::kRemove) values.erase(found);
    else {
      const auto name = Name(current_->resource);
      if (name != current_->name && Find(values, name) != values.end()) { Completed(ErrorCode::kAlreadyExists); return; }
      *found = std::move(current_->resource);
    }
  }
  if (backend_) backend_->write.Run(std::move(resources), base::BindOnce(
      &AdBlockCustomResourceProvider::Saved, weak_ptr_factory_.GetWeakPtr()));
  else { if (storage_) storage_->Set(kKey, std::move(resources)); Saved(ErrorCode::kOk); }
}
void AdBlockCustomResourceProvider::Saved(ErrorCode status) {
  if (status == ErrorCode::kOk) {
    auto weak = weak_ptr_factory_.GetWeakPtr(); ReloadResourcesAndNotify(); if (!weak) return;
    for (auto& observer : observers_) { observer.OnCustomResourcesChanged(); if (!weak) return; }
  }
  Completed(status);
}
void AdBlockCustomResourceProvider::Completed(ErrorCode status) {
  auto done = std::move(current_->done); current_.reset();
  auto weak = weak_ptr_factory_.GetWeakPtr(); std::move(done).Run(status); if (weak) Pump();
}
void AdBlockCustomResourceProvider::Close() {
  if (closed_) return;
  closed_ = true; weak_ptr_factory_.InvalidateWeakPtrs(); cached_storage_.reset();
  std::vector<StatusCallback> cancelled;
  if (current_) { cancelled.push_back(std::move(current_->done)); current_.reset(); }
  for (auto& mutation : pending_) cancelled.push_back(std::move(mutation.done));
  pending_.clear();
  for (auto& callback : cancelled) std::move(callback).Run(ErrorCode::kCancelled);
}
void AdBlockCustomResourceProvider::AddObserver(Observer* observer) { observers_.AddObserver(observer); }
void AdBlockCustomResourceProvider::RemoveObserver(Observer* observer) { observers_.RemoveObserver(observer); }
void AdBlockCustomResourceProvider::LoadResources(base::OnceCallback<void(AdblockResourceStorageBox)> callback) {
  if (!Available()) { cached_storage_.reset(); return; }
  if (cached_storage_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<AdBlockCustomResourceProvider> self,
            uint64_t revision,base::OnceCallback<void(AdblockResourceStorageBox)> callback, AdblockResourceStorageBox storage) {
          if (!self || !self->Available())return;
          if(revision!=self->revision_)self->LoadResources(std::move(callback));
          else std::move(callback).Run(std::move(storage));
        }, weak_ptr_factory_.GetWeakPtr(), revision_,std::move(callback), adblock::clone_resource_storage(*cached_storage_.value())));
    return;
  }
  default_resource_provider_->LoadResources(base::BindOnce(
      &AdBlockCustomResourceProvider::OnDefaultResourcesLoaded, weak_ptr_factory_.GetWeakPtr(),revision_, std::move(callback)));
}
void AdBlockCustomResourceProvider::OverrideResourcesForTesting(AdblockResourceStorageBox storage) { CHECK(revision_!=std::numeric_limits<uint64_t>::max());++revision_;cached_storage_ = std::move(storage); }
void AdBlockCustomResourceProvider::OnResourcesLoaded(AdblockResourceStorageBox storage) {
  CHECK(revision_!=std::numeric_limits<uint64_t>::max());++revision_;cached_storage_.reset(); OnDefaultResourcesLoaded(revision_,base::BindOnce(
      &AdBlockCustomResourceProvider::NotifyResourcesLoaded, weak_ptr_factory_.GetWeakPtr()), std::move(storage));
}
void AdBlockCustomResourceProvider::OnDefaultResourcesLoaded(
    uint64_t revision,base::OnceCallback<void(AdblockResourceStorageBox)> callback, AdblockResourceStorageBox storage) {
  if(!Available())return;
  if(revision!=revision_){LoadResources(std::move(callback));return;}
  GetCustomResourcesWithStatus(base::BindOnce(&AdBlockCustomResourceProvider::OnCustomResourcesLoaded,
      weak_ptr_factory_.GetWeakPtr(),revision, std::move(callback), std::move(storage)));
}
void AdBlockCustomResourceProvider::OnCustomResourcesLoaded(
    uint64_t revision,base::OnceCallback<void(AdblockResourceStorageBox)> callback, AdblockResourceStorageBox storage,
    ErrorCode status, base::Value resources) {
  if (!Available() || status != ErrorCode::kOk || !resources.is_list()) return;
  if(revision!=revision_){LoadResources(std::move(callback));return;}
  if (!resources.GetList().empty()) {
    auto json = base::WriteJson(resources); if (!json) return;
    storage = adblock::extend_resource_storage(*storage, *json);
  }
  cached_storage_ = std::move(storage);
  std::move(callback).Run(adblock::clone_resource_storage(*cached_storage_.value()));
}
void AdBlockCustomResourceProvider::ReloadResourcesAndNotify() {
  CHECK(revision_!=std::numeric_limits<uint64_t>::max());++revision_;cached_storage_.reset(); LoadResources(base::BindOnce(
      &AdBlockCustomResourceProvider::NotifyResourcesLoaded, weak_ptr_factory_.GetWeakPtr()));
}
}  // namespace brave_shields
