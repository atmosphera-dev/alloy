// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_NATIVE_PREF_VIEW_H_
#define BRAVE_ALLOY_SITE_RECORDS_NATIVE_PREF_VIEW_H_

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "components/prefs/persistent_pref_store.h"
#include <map>

namespace alloy::site_records {
class NativePrefView;

// One native working-cache owner. Persistence/routing belongs to the concrete
// source; this common boundary owns only native views and their teardown fence.
class NativePrefSource : public PersistentPrefStore {
 public:
  scoped_refptr<NativePrefView> CreateNativeView();
 protected:
  NativePrefSource();
  ~NativePrefSource() override;
  void CloseNativeViews();
  bool NativeViewsEmpty() const { return native_views_.empty(); }
  virtual void NativeViewsChanged() = 0;
  virtual bool CanStartNativeConstruction() const = 0;
  virtual bool NativeCacheAlive() const = 0;
  virtual bool NativeGetValue(std::string_view key, const base::Value** out) const = 0;
  virtual base::DictValue NativeGetValues() const = 0;
  virtual bool NativeGetMutableValue(std::string_view key, base::Value** out) = 0;
  virtual void NativeSetValue(std::string_view key, base::Value value, bool notify) = 0;
  virtual void NativeReportValueChanged(std::string_view key, bool notify) = 0;
  virtual void NativeRemoveValue(std::string_view key) = 0;
  virtual void NativeRemovePrefix(std::string_view prefix) = 0;
  virtual bool NativeCanPersist() const = 0;
 private:
  friend class NativePrefView;
  void NativeViewClosed(base::UnguessableToken id);
  std::map<base::UnguessableToken, base::WeakPtr<NativePrefView>> native_views_;
};

// A native lifetime view of the same UI cache, not an I/O capability. Its owner
// must begin construction while admitted and finish teardown after destroying
// its native services/PrefService. Cache access may finish that already-started
// construction/teardown after revocation; persistence still uses original leases.
class NativePrefView final : public PersistentPrefStore {
 public:
  bool BeginConstruction(base::OnceClosure teardown);
  void ConstructionFinished();
  void FinishTeardown();
  bool CanStartServiceConstruction() const;
  base::WeakPtr<NativePrefView> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  const NativePrefSource* source_owner() const { return source_.get(); }

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
  friend class NativePrefSource;
  NativePrefView(scoped_refptr<NativePrefSource> source, base::UnguessableToken id);
  ~NativePrefView() override;
  void RequestTeardown();
  bool Live() const;
  enum class Stage { kPrepared, kConstructing, kActive, kTearingDown, kClosed };
  const scoped_refptr<NativePrefSource> source_;
  const base::UnguessableToken id_;
  Stage stage_ = Stage::kPrepared;
  bool teardown_requested_ = false;
  base::OnceClosure teardown_;
  base::WeakPtrFactory<NativePrefView> weak_factory_{this};
};
}  // namespace alloy::site_records
#endif
