// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_PREF_STORE_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_PREF_STORE_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "brave/alloy/site_records/record_file_system.h"
#include "components/prefs/persistent_pref_store.h"
#include "components/prefs/pref_value_map.h"

namespace alloy::site_records {

// One native preference shard. Routing selects a trusted record BEFORE opening
// this adapter; it cannot change keys or spill into a profile/parent JSON store.
// All methods, including lifecycle invalidation, run on its owning sequence.
// The lifecycle owner invalidates decoded native projections during teardown;
// capability checks additionally deny every subsequent stale read/write.
class RecordPrefStore final : public PersistentPrefStore {
 public:
  using ErrorCallback = base::RepeatingCallback<void(FileStatus)>;
  RecordPrefStore(std::shared_ptr<RecordFileSystem> files,
                  RecordCapability capability,
                  StorePurpose purpose,
                  std::string name,
                  ErrorCallback on_error);

  bool GetValue(std::string_view key, const base::Value** result) const override;
  base::DictValue GetValues() const override;
  void AddObserver(PrefStore::Observer* observer) override;
  void RemoveObserver(PrefStore::Observer* observer) override;
  bool HasObservers() const override;
  bool IsInitializationComplete() const override;
  bool GetMutableValue(std::string_view key, base::Value** result) override;
  void ReportValueChanged(std::string_view key, uint32_t flags) override;
  void SetValue(std::string_view key, base::Value value, uint32_t flags) override;
  void SetValueSilently(std::string_view key, base::Value value,
                        uint32_t flags) override;
  void RemoveValue(std::string_view key, uint32_t flags) override;
  void RemoveValuesByPrefixSilently(std::string_view prefix) override;
  bool ReadOnly() const override;
  PrefReadError GetReadError() const override;
  PrefReadError ReadPrefs() override;
  void ReadPrefsAsync(ReadErrorDelegate* error_delegate) override;
  void CommitPendingWrite(base::OnceClosure reply_callback = {},
                          base::OnceClosure synchronous_done_callback = {}) override;
  void SchedulePendingLossyWrites() override;
  void OnStoreDeletionFromDisk() override;
  bool HasReadErrorDelegate() const override;

  // Chromium's completion closures have no status argument. Engine owners use
  // this result and last_status() rather than equating callback with success.
  FileStatus Flush();
  FileStatus last_status() const;
  void Invalidate();

 private:
  ~RecordPrefStore() override;
  bool Admitted() const;
  bool Writable() const;
  FileStatus Load();
  FileStatus FlushImpl();
  FileStatus Fail(FileStatus status);
  void Notify(std::string_view key);
  void ScheduleWrite();

  const std::shared_ptr<RecordFileSystem> files_;
  const RecordCapability capability_;
  const StorePurpose purpose_;
  const std::string name_;
  const ErrorCallback on_error_;
  PrefValueMap prefs_;
  PrefValueMap refused_;
  base::ObserverList<PrefStore::Observer, true> observers_;
  bool initialized_ = false;
  bool dirty_ = false;
  bool invalidated_ = false;
  FileStatus status_ = FileStatus::kOk;
  base::OneShotTimer write_timer_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_PREF_STORE_H_
