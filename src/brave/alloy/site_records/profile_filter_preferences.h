// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_PREFERENCES_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_PREFERENCES_H_

#include <atomic>
#include <set>
#include <vector>

#include "base/supports_user_data.h"
#include "base/callback_list.h"
#include "brave/components/brave_shields/core/browser/ad_block_custom_resource_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_subscription_service_manager.h"
#include "brave/alloy/site_records/profile_filter_storage.h"
#include "components/prefs/in_memory_pref_store.h"

namespace alloy::site_records {
class ProfileFilterResources;
// Native AdBlock prefs are a separate explicit profile input, never Local State.
// Initialization and writes use the same original profile FIFO as its other
// filter files. Close hides/wipes the UI cache, then drains accepted writes;
// consumers join that completion to their native teardown barrier.
class ProfileFilterPreferences final : public InMemoryPrefStore, public base::SupportsUserData {
 public:
  using Error = base::RepeatingCallback<void(FileStatus)>;
  using Loaded = base::OnceCallback<void(FileStatus, scoped_refptr<ProfileFilterPreferences>)>;
  using Closed = base::OnceCallback<void(FileStatus)>;
  static void Load(std::shared_ptr<ProfileFilterStorage> storage,
                   scoped_refptr<base::SequencedTaskRunner> runner, Error error, Loaded loaded,
                   base::RepeatingCallback<bool()> admitted = {});
  bool available() const;
  StorageMode mode() const { return storage_->mode(); }
  bool GetValue(std::string_view key, const base::Value** out) const override;
  base::DictValue GetValues() const override;
  bool GetMutableValue(std::string_view key, base::Value** out) override;
  void SetValue(std::string_view key, base::Value value, uint32_t flags) override;
  void SetValueSilently(std::string_view key, base::Value value, uint32_t flags) override;
  void ReportValueChanged(std::string_view key, uint32_t flags) override;
  void RemoveValue(std::string_view key, uint32_t flags) override;
  void RemoveValuesByPrefixSilently(std::string_view prefix) override;
  bool ReadOnly() const override;
  void CommitPendingWrite(base::OnceClosure reply = {}, base::OnceClosure done = {}) override;
  void Close(Closed done);
  static void Attach(base::SupportsUserData& context, scoped_refptr<ProfileFilterPreferences> prefs);
  static scoped_refptr<ProfileFilterPreferences> Get(const base::SupportsUserData& context);
  static scoped_refptr<ProfileFilterPreferences> EnsureTransient(base::SupportsUserData& context);
  brave_shields::AdBlockCustomResourceProvider::Backend ResourceBackend();
  using SubscriptionFetch = base::RepeatingCallback<base::OnceClosure(
      const GURL&, bool, brave_shields::AdBlockSubscriptionFiltersProvider::SourceReply)>;
  brave_shields::AdBlockSubscriptionServiceManager::Backend SubscriptionBackend(
      SubscriptionFetch fetch, base::RepeatingCallback<GURL(const GURL&)> source_url = {});
  void ReadSubscriptionSource(const std::string& path,
      brave_shields::AdBlockSubscriptionFiltersProvider::SourceReply reply);
  brave_shields::AdBlockDATCacheManager::Backend DATBackend();
  scoped_refptr<base::SequencedTaskRunner> task_runner() const { return runner_; }
  base::CallbackListSubscription ObserveClose(base::RepeatingClosure closed);
  void ConsumerFailed(FileStatus status);

 private:
  ProfileFilterPreferences(std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner, Error error, base::DictValue initial,
      base::RepeatingCallback<bool()> admitted = {});
  ~ProfileFilterPreferences() override;
  void Install(std::string_view key, base::Value value, uint32_t flags, bool notify);
  void Publish(std::string_view excluded = {});
  void Failed(FileStatus status);
  void FinishClose();
  void Wipe();
  const std::shared_ptr<ProfileFilterStorage> storage_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  const Error error_;
  const base::RepeatingCallback<bool()> admitted_;
  std::set<std::string, std::less<>> keys_;
  std::vector<Closed> closed_callbacks_;
  // Admission is also queried by subscription parsers on the engine sequence.
  // The preference cache and all lifecycle mutations remain UI-owned.
  std::atomic<FileStatus> status_{FileStatus::kOk};
  std::atomic<bool> closing_{false};
  bool closed_ = false;
  scoped_refptr<ProfileFilterResources> resources_;
  base::RepeatingClosureList consumers_closed_;
  base::RepeatingCallback<void(const std::string&,
      brave_shields::AdBlockSubscriptionFiltersProvider::SourceReply)> subscription_source_;
};
}  // namespace alloy::site_records
#endif
