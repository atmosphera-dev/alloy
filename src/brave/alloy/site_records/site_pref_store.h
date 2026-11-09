// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_SITE_PREF_STORE_H_
#define BRAVE_ALLOY_SITE_RECORDS_SITE_PREF_STORE_H_

#include <map>
#include <deque>
#include <optional>
#include <set>
#include <vector>

#include "brave/alloy/site_records/embed_list_settings.h"
#include "brave/alloy/site_records/record_pref_store.h"
#include "brave/alloy/site_records/preferred_app_records.h"
#include "brave/alloy/site_records/record_value_store_binding.h"
#include "brave/alloy/site_records/native_pref_view.h"
#include "base/threading/sequence_bound.h"
#include "base/memory/weak_ptr.h"

namespace alloy::site_records {
class NativePrefView;

// Native PrefService projection over owner-sharded preference files. Site
// identities remain inside their records; the profile shard holds only the
// remaining non-site preferences. The projection and dependency traversal are
// volatile and are rebuilt from authenticated live records on open.
class SitePrefStore final : public NativePrefSource {
 public:
  static constexpr auto& kEmbedList = embed_list::kPreference;
  static constexpr char kPreferredApps[] = "alloy.preferred_app_records";
  SitePrefStore(std::shared_ptr<RecordFileSystem> files,
                RecordPrefStore::ErrorCallback on_error,
                scoped_refptr<base::SequencedTaskRunner> io_runner = {},
                base::RepeatingCallback<bool()> admitted = {});

  bool GetValue(std::string_view key, const base::Value** result) const override;
  base::DictValue GetValues() const override;
  void AddObserver(PrefStore::Observer* observer) override;
  void RemoveObserver(PrefStore::Observer* observer) override;
  bool HasObservers() const override;
  bool IsInitializationComplete() const override;
  bool GetMutableValue(std::string_view key, base::Value** result) override;
  void ReportValueChanged(std::string_view key, uint32_t flags) override;
  void SetValue(std::string_view key, base::Value value, uint32_t flags) override;
  void SetValueSilently(std::string_view key, base::Value value, uint32_t flags) override;
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

  FileStatus Flush();
  using Completion = base::OnceCallback<void(FileStatus)>;
  void InitializeAsync(Completion completion);
  void FlushAsync(Completion completion);
  void SetFirstPartyEmbedsAsync(GURL outer, std::vector<GURL> embedded, Completion completion);
  void GetFirstPartyEmbedsAsync(GURL outer,
      base::OnceCallback<void(FileStatus, std::vector<std::string>)> completion);
  FileStatus last_status() const;
  std::optional<RecordValueStoreBinding> BindManagedOrigin(const url::Origin& origin);
  void Invalidate();
  // Original admission is checked when preparing AND beginning native use.
  // Called after key revocation. Completes after native owners have finished
  // teardown, the UI cache is destroyed, and worker plaintext is released.
  void CloseNativeCacheAsync(base::OnceClosure done);
  class NativeCache;
  base::WeakPtr<NativeCache> GetNativeCacheForTesting() const;
  // Owning-sequence operations on the same projection used by native prefs.
  // Enumeration copies canonical R16 domains and modifies output only on
  // success. Whole-list replacement validates all entries before publication.
  FileStatus GetFirstPartyEmbeds(const GURL& outer, std::vector<std::string>* domains);
  FileStatus SetFirstPartyEmbeds(const GURL& outer, const std::vector<GURL>& embedded);
  FileStatus SetConsideredFirstParty(const GURL& outer, const GURL& embedded, bool allowed);
  FileStatus GetPreferredApps(apps::PreferredApps* entries);
  FileStatus SetPreferredApps(const apps::PreferredApps& entries);
  struct PreferredVersion {
    base::UnguessableToken revision;
    base::UnguessableToken admission;
    bool operator==(const PreferredVersion&) const = default;
  };
  PreferredVersion preferred_version() const;
  // Pure original-generation check; a retained snapshot cannot become admitted
  // again after R13 installs a fresh projection, even if its value is unchanged.
  bool PreferredAdmission(const PreferredVersion& version) const;
  bool IsPreferredPublication(const PreferredVersion& expected) const;
  FileStatus PublishPreferredApps(const PreferredVersion& expected,
                                 const apps::PreferredApps& entries,
                                 PreferredVersion* published);
  void PublishPreferredAppsAsync(PreferredVersion expected, apps::PreferredApps entries,
      base::OnceCallback<void(FileStatus, PreferredVersion)> completion);
  bool IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const;
  // Shared admission for context-wide native working state. A context created
  // during R13 must observe the same closed record as existing contexts.
  bool RecordWorkingStateAdmitted(std::string_view domain) const;
  // Key + references publish first; revoke/drain every old native access before
  // preference observers or the final deletion acknowledgement (R13/R4).
  FileStatus DeleteSite(const GURL& site, std::function<void()> teardown,
                         RecordDeletion* result);
  void DeleteSiteAsync(GURL site, base::OnceClosure teardown,
      base::OnceCallback<void(FileStatus, RecordDeletion)> completion);
  // Key retirement precedes the asynchronous native barrier. The record's
  // admission remains closed, and deletion cannot finish/unlink or acknowledge
  // success until the native owner invokes the supplied continuation.
  void DeleteSiteWithTeardownAsync(GURL site,
      base::OnceCallback<void(Completion)> teardown,
      base::OnceCallback<void(FileStatus, RecordDeletion)> completion);
  // Native contexts return publication separately from completed revocation.
  // The shared owner releases all publications after every context has drained.
  void DeferNativeRetirementNotification(base::OnceClosure publication);

 private:
  ~SitePrefStore() override;
  class IO;
  struct State;
  struct Request;
  bool Admitted() const;
  FileStatus Fail(FileStatus status);
  base::DictValue Snapshot() const;
  void Enqueue(std::unique_ptr<Request> request);
  void Pump();
  void Complete(std::unique_ptr<Request> request, bool from_backend, State state);
  void Retired(std::unique_ptr<Request> request, State state);
  void FinishRetirement(std::unique_ptr<Request> request, base::UnguessableToken ticket,
                        FileStatus teardown_status);
  void NativeFence(base::UnguessableToken admission, uint64_t mutation,
                   base::OnceClosure reply, State state);
  FileStatus Wait(base::OnceCallback<void(Completion)> operation);
  FileStatus CompletionStatus(const Request& request, FileStatus status) const;
  PreferredVersion Version(std::string_view key) const;
  void AdvanceRevision(std::string_view key);
  void Changed(std::string_view key, bool notify, bool persist = true);
  bool CanStartNativeConstruction() const override;
  bool NativeCacheAlive() const override;
  bool NativeGetValue(std::string_view key, const base::Value** out) const override;
  bool NativeGetMutableValue(std::string_view key, base::Value** out) override;
  base::DictValue NativeGetValues() const override;
  void NativeSetValue(std::string_view key, base::Value value, bool notify) override;
  void NativeReportValueChanged(std::string_view key, bool notify) override;
  void NativeRemoveValue(std::string_view key) override;
  void NativeRemovePrefix(std::string_view prefix) override;
  bool NativeCanPersist() const override;
  void NativeViewsChanged() override;
  void MaybeCloseNativeCache();
  void BackendClosed();
  PreferredVersion Install(PrefValueMap* next, bool notify, bool admission_changed = false,
                           std::string_view publication_key = kPreferredApps);
  void RetireAdmission();
  void NotifyChange(std::string_view key);
  void PublishRetirementNotifications(bool success);

  const std::shared_ptr<RecordFileSystem> files_;
  const RecordCapability profile_;
  const RecordPrefStore::ErrorCallback on_error_;
  const base::RepeatingCallback<bool()> admitted_;
  const scoped_refptr<base::SequencedTaskRunner> io_runner_;
  base::SequenceBound<IO> io_;
  std::map<std::string, RecordCapability> capabilities_;
  std::unique_ptr<NativeCache> cache_;
  std::deque<std::unique_ptr<Request>> requests_;
  size_t pending_read_delegates_ = 0;
  size_t pending_backend_calls_ = 0;
  bool closing_cache_ = false;
  bool closing_backend_ = false;
  bool backend_closed_ = false;
  std::vector<base::OnceClosure> close_callbacks_;
  bool reading_ = false;
  bool busy_ = false;
  bool in_teardown_ = false;
  std::string retiring_record_;
  std::set<std::string> retirement_notifications_;
  std::vector<base::OnceClosure> native_retirement_publications_;
  uint64_t mutation_ = 0;
  base::ObserverList<PrefStore::Observer, true> observers_;
  FileStatus status_ = FileStatus::kOk;
  bool initialized_ = false;
  bool invalidated_ = false;
  bool dirty_ = false;
  std::set<std::string> dirty_keys_;
  // Explicit setting publishers share one admission lifetime, but changes to
  // an unrelated setting cannot change another publisher's value revision.
  std::map<std::string, base::UnguessableToken, std::less<>> revisions_{
      {kPreferredApps, base::UnguessableToken::Create()},
      {kEmbedList, base::UnguessableToken::Create()}};
  base::UnguessableToken admission_ = base::UnguessableToken::Create();
  std::optional<PreferredVersion> preferred_publication_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::RepeatingClosureList record_retired_;
  base::WeakPtrFactory<SitePrefStore> weak_factory_{this};
};

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_SITE_PREF_STORE_H_
