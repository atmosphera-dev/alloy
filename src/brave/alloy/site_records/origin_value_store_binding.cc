// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_value_store_binding.h"
#include "brave/alloy/site_records/record_value_store.h"
#include <atomic>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "url/origin.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
struct OriginBinding {
  std::shared_ptr<RecordFileSystem> files;
  RecordCapability profile;
  RecordCapability record;  // Immutable after ready's release publication.
  std::atomic<bool> ready{false};
  std::atomic<bool> enabled{true};
  bool Admitted() const {
    if (!profile.active() || !enabled.load(std::memory_order_acquire)) return false;
    return !ready.load(std::memory_order_acquire) || record.active();
  }
};
}  // namespace

RecordValueStoreBinding BindOriginValueStore(
    std::shared_ptr<RecordFileSystem> files, RecordCapability original_profile,
    url::Origin origin, scoped_refptr<base::SequencedTaskRunner> runner,
    base::RepeatingCallback<void(FileStatus)> on_error) {
  CHECK(files && runner && on_error);
  auto state = std::make_shared<OriginBinding>();
  state->files = std::move(files); state->profile = std::move(original_profile);
  runner->PostTask(FROM_HERE, base::BindOnce(
      [](std::shared_ptr<OriginBinding> state, url::Origin origin,
         base::RepeatingCallback<void(FileStatus)> error) {
        RecordInfo record;
        auto status = FileStatus::kCancelled;
        if (state->profile.Allows(StorePurpose::kProfileSettings, StorageAccess::kReadRecordFiles) &&
            state->profile.profile() == state->files->profile_id())
          status = state->files->ResolveRecord(origin.GetURL(), origin, true, &record);
        if (status == FileStatus::kOk)
          state->record = state->files->Capability(record.id, StorePurpose::kSiteSettings);
        else state->enabled.store(false, std::memory_order_release);
        state->ready.store(true, std::memory_order_release);
        if (status != FileStatus::kOk && status != FileStatus::kCancelled &&
            status != FileStatus::kDenied && status != FileStatus::kBusy) error.Run(status);
      }, state, origin, on_error));
  RecordValueStoreBinding result;
  result.runner = std::move(runner);
  result.admitted = base::BindRepeating([](std::shared_ptr<OriginBinding> state) {
    return state->Admitted();
  }, state);
  result.create = base::BindRepeating([](std::shared_ptr<OriginBinding> state,
      base::FilePath database) -> std::unique_ptr<value_store::ValueStore> {
    CHECK(state->ready.load(std::memory_order_acquire));  // Same FIFO as provisioning.
    if (!state->Admitted()) return nullptr;
    std::unique_ptr<value_store::LeveldbValueStore> store;
    if (CreateRecordValueStore(state->files, state->record, StorePurpose::kSiteSettings,
        database, &store) != FileStatus::kOk) return nullptr;
    return store;
  }, state, base::FilePath("/managed-"+base::HexEncode(origin.Serialize())));
  result.error = base::BindRepeating([](base::RepeatingCallback<void(FileStatus)> error,
      const value_store::ValueStore::Status& status) {
    if (status.code == value_store::ValueStore::READ_ONLY) return;
    if (!status.ok()) error.Run(status.IsCorrupted() ? FileStatus::kIntegrity : FileStatus::kIo);
  }, std::move(on_error));
  return result;
}
}  // namespace alloy::site_records
