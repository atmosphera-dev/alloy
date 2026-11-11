// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/preferred_app_storage.h"

#include "base/functional/bind.h"

namespace alloy::site_records {
namespace {
using Native = apps::PreferredAppsImpl;
Native::StorageResult Result(FileStatus status) {
  switch (status) {
    case FileStatus::kOk: return Native::StorageResult::kOk;
    case FileStatus::kBusy: return Native::StorageResult::kConflict;
    case FileStatus::kDenied:
    case FileStatus::kCancelled: return Native::StorageResult::kDenied;
    case FileStatus::kInvalid:
    case FileStatus::kIntegrity: return Native::StorageResult::kInvalid;
    default: return Native::StorageResult::kIo;
  }
}
class Storage final : public Native::Storage, private PrefStore::Observer,
                      public std::enable_shared_from_this<Storage> {
 public:
  explicit Storage(scoped_refptr<SitePrefStore> store) : store_(std::move(store)) {
    CHECK(store_);
    store_->AddObserver(this);
  }
  ~Storage() override { store_->RemoveObserver(this); }
  void SetChangedCallback(Changed changed) override { changed_ = std::move(changed); }
  void Read(Reply reply) override {
    store_->InitializeAsync(base::BindOnce([](std::shared_ptr<Storage> self, Reply reply,
        FileStatus status) {
      Native::Snapshot snapshot;
      if (status == FileStatus::kOk) status = self->store_->GetPreferredApps(&snapshot.entries);
      if (status == FileStatus::kOk) self->BindSnapshot(self->store_->preferred_version(), &snapshot);
      std::move(reply).Run(Result(status), std::move(snapshot));
    }, shared_from_this(), std::move(reply)));
  }
  void Write(Native::Version expected, apps::PreferredApps entries, Reply reply) override {
    // The native caller holds this backend through callback-time owner loss.
    // Suppress only this transaction's revision echo; R13 admission changes
    // still invalidate the live native model synchronously inside publication.
    publishing_ = true;
    expected_ = {expected.revision, expected.admission};
    auto retained = apps::ClonePreferredApps(entries);
    store_->PublishPreferredAppsAsync(expected_, std::move(entries), base::BindOnce(
        [](std::shared_ptr<Storage> self, apps::PreferredApps entries, Reply reply,
           FileStatus status, SitePrefStore::PreferredVersion published) {
          self->publishing_ = false;
          Native::Snapshot snapshot;
          if (status == FileStatus::kOk) {
            snapshot.entries = std::move(entries);
            self->BindSnapshot(published, &snapshot);
          }
          std::move(reply).Run(Result(status), std::move(snapshot));
        }, shared_from_this(), std::move(retained), std::move(reply)));
  }
 private:
  void BindSnapshot(SitePrefStore::PreferredVersion version, Native::Snapshot* snapshot) {
    snapshot->version = {version.revision, version.admission};
    snapshot->admitted = base::BindRepeating(
        [](scoped_refptr<SitePrefStore> store, SitePrefStore::PreferredVersion version) {
          return store->PreferredAdmission(version);
        }, store_, version);
  }
  void OnPrefValueChanged(std::string_view key) override {
    if (key != SitePrefStore::kPreferredApps) return;
    if (publishing_ && store_->IsPreferredPublication(expected_)) return;
    auto changed = changed_;
    if (changed) changed.Run(Result(store_->last_status()));
  }
  void OnInitializationCompleted(bool) override {
    auto changed = changed_;
    if (changed) changed.Run(Result(store_->last_status()));
  }
  const scoped_refptr<SitePrefStore> store_;
  Changed changed_;
  SitePrefStore::PreferredVersion expected_;
  bool publishing_ = false;
};
}  // namespace
std::shared_ptr<Native::Storage> MakePreferredAppStorage(scoped_refptr<SitePrefStore> store) {
  return std::make_shared<Storage>(std::move(store));
}
}  // namespace alloy::site_records
