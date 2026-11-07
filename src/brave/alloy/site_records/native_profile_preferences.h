// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_NATIVE_PROFILE_PREFERENCES_H_
#define BRAVE_ALLOY_SITE_RECORDS_NATIVE_PROFILE_PREFERENCES_H_
#include "brave/alloy/site_records/native_pref_view.h"
#include "brave/alloy/site_records/record_capability.h"
#include "base/task/sequenced_task_runner.h"
#include "components/prefs/in_memory_pref_store.h"
#include <vector>
namespace alloy::site_records {
// Volatile native construction/working preferences, not a site-setting store.
// Explicit feature inputs (such as private AdBlock preferences) own persistence
// separately. Original key revocation stops new construction immediately, while
// borrowed values survive until all already-started native owners retire.
class NativeProfilePreferences final : public NativePrefSource {
 public:
  NativeProfilePreferences(RecordCapability original,
      scoped_refptr<base::SequencedTaskRunner> runner,
      base::RepeatingCallback<bool()> admitted,
      base::RepeatingClosure error);
  void Close(base::OnceClosure done);
  bool GetValue(std::string_view key, const base::Value** out) const override;
  base::DictValue GetValues() const override;
  bool GetMutableValue(std::string_view key, base::Value** out) override;
  void SetValue(std::string_view key, base::Value value, uint32_t flags) override;
  void SetValueSilently(std::string_view key, base::Value value, uint32_t flags) override;
  void ReportValueChanged(std::string_view key, uint32_t flags) override;
  void RemoveValue(std::string_view key, uint32_t flags) override;
  void RemoveValuesByPrefixSilently(std::string_view prefix) override;
  void AddObserver(PrefStore::Observer* observer) override;
  void RemoveObserver(PrefStore::Observer* observer) override;
  bool HasObservers() const override;
  bool IsInitializationComplete() const override;
  bool ReadOnly() const override;
  PrefReadError GetReadError() const override;
  PrefReadError ReadPrefs() override;
  void ReadPrefsAsync(ReadErrorDelegate* delegate) override;
  void CommitPendingWrite(base::OnceClosure reply = {}, base::OnceClosure done = {}) override;
  void SchedulePendingLossyWrites() override;
  void OnStoreDeletionFromDisk() override;
  bool HasReadErrorDelegate() const override;
 private:
  ~NativeProfilePreferences() override;
  bool CanStartNativeConstruction() const override;
  bool NativeCacheAlive() const override;
  bool NativeGetValue(std::string_view key, const base::Value** out) const override;
  base::DictValue NativeGetValues() const override;
  bool NativeGetMutableValue(std::string_view key, base::Value** out) override;
  void NativeSetValue(std::string_view key, base::Value value, bool notify) override;
  void NativeReportValueChanged(std::string_view key, bool notify) override;
  void NativeRemoveValue(std::string_view key) override;
  void NativeRemovePrefix(std::string_view prefix) override;
  bool NativeCanPersist() const override;
  void NativeViewsChanged() override;
  void MaybeClose();
  void FinishClose();
  void Wipe();
  const RecordCapability original_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  const base::RepeatingCallback<bool()> admitted_;
  const base::RepeatingClosure error_;
  const scoped_refptr<InMemoryPrefStore> cache_;
  bool closing_ = false;
  bool cache_closed_ = false;
  bool closed_ = false;
  std::vector<base::OnceClosure> closed_callbacks_;
};
}  // namespace alloy::site_records
#endif
