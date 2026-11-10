// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_SITE_PREF_STORE_IO_H_
#define BRAVE_ALLOY_SITE_RECORDS_SITE_PREF_STORE_IO_H_

#include <map>
#include <set>
#include <vector>
#include "base/memory/raw_ptr.h"

#include "brave/alloy/site_records/embed_list_settings.h"
#include "brave/alloy/site_records/record_pref_store.h"
#include "brave/alloy/site_records/preferred_app_records.h"

namespace content_settings { class WebsiteSettingsRegistry; }

namespace alloy::site_records::internal {

// Private, blocking implementation. Constructed, called and destroyed only by
// SitePrefStore's SequenceBound I/O owner, never installed in a UI PrefService.
class SitePrefStore final : public PersistentPrefStore {
 public:
  static constexpr auto& kEmbedList = embed_list::kPreference;
  static constexpr char kPreferredApps[] = "alloy.preferred_app_records";
  SitePrefStore(std::shared_ptr<RecordFileSystem> files,
                RecordPrefStore::ErrorCallback on_error,
                const content_settings::WebsiteSettingsRegistry* registry = nullptr);
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
  void CommitPendingWrite(base::OnceClosure reply = {}, base::OnceClosure done = {}) override;
  void SchedulePendingLossyWrites() override;
  void OnStoreDeletionFromDisk() override;
  bool HasReadErrorDelegate() const override;
  FileStatus Flush();
  FileStatus last_status() const;
  void Invalidate();
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
  bool PreferredAdmission(const PreferredVersion& version) const;
  FileStatus PublishPreferredApps(const PreferredVersion& expected,
                                 const apps::PreferredApps& entries, PreferredVersion* published);
  bool IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const;
  FileStatus DeleteSite(const GURL& site, std::function<void()> teardown, RecordDeletion* result);
  FileStatus RetireSite(const GURL& site, base::UnguessableToken* ticket,
                        const RecordInfo* expected = nullptr);
  // Decay selections retain their stored identity; never resolve them through
  // a content-setting pattern or borrow a replacement with the same domain.
  FileStatus RetireRecord(const RecordInfo& expected, base::UnguessableToken* ticket);
  // Flat native keys, unlike PrefStore::GetValues' expanded dictionary.
  base::DictValue Snapshot() const;
  void ApplySnapshot(base::DictValue values);
  const std::map<std::string, RecordCapability>& capabilities() const { return capabilities_; }
  static void InitializeRegistries();
  static bool IsRegisteredExceptionPref(std::string_view key,
      const content_settings::WebsiteSettingsRegistry* registry = nullptr);
  static FileStatus PruneSnapshot(base::DictValue values, std::string_view excluding,
                                  base::DictValue* retained);

 private:
  ~SitePrefStore() override;
  using Shards = std::map<std::string, base::DictValue>;
  bool Admitted() const;
  FileStatus Load();
  FileStatus Fail(FileStatus status);
  static FileStatus Split(const PrefValueMap& values, std::string_view key, const base::Value& value,
                   std::string_view excluding, Shards* shards, base::Value* retained,
                   const content_settings::WebsiteSettingsRegistry* registry);
  FileStatus Build(std::string_view excluding, Shards* shards, PrefValueMap* retained) const;
  static FileStatus BuildValues(const PrefValueMap& values, std::string_view excluding,
                        Shards* shards, PrefValueMap* retained,
                        const content_settings::WebsiteSettingsRegistry* registry = nullptr);
  FileStatus Prepare(const Shards& shards, std::string_view excluding,
                     std::map<std::string, std::string>* buffers,
                     std::vector<SettingsReplacement>* replacements);
  FileStatus FlushImpl();
  FileStatus RetireSelectedRecord(const RecordInfo& record, base::UnguessableToken* ticket,
                                 std::optional<int64_t> unchanged_visit);
  FileStatus PublishValue(std::string_view key, base::Value value, PreferredVersion* published);
  FileStatus ReplaceFirstPartyEmbeds(const std::string& outer, base::ListValue embedded);
  static bool ConsideredFirstParty(const PrefValueMap& values, const GURL& outer, const GURL& embedded);
  PreferredVersion Version(std::string_view key) const;
  void AdvanceRevision(std::string_view key);
  void Changed(std::string_view key, bool notify);
  PreferredVersion Install(PrefValueMap* next, bool notify, bool admission_changed = false,
                           std::string_view publication_key = kPreferredApps);
  void RetireAdmission();
  const std::shared_ptr<RecordFileSystem> files_;
  const RecordCapability profile_;
  const RecordPrefStore::ErrorCallback on_error_;
  // Optional borrowed schema for a synchronous pre-CEF operation. Its owned
  // graph must outlive this store; normal native owners use the global schema.
  const raw_ptr<const content_settings::WebsiteSettingsRegistry> registry_;
  std::map<std::string, RecordCapability> capabilities_;
  PrefValueMap values_;
  Shards persisted_;
  base::ObserverList<PrefStore::Observer, true> observers_;
  base::OneShotTimer timer_;
  FileStatus status_ = FileStatus::kOk;
  bool initialized_ = false;
  bool invalidated_ = false;
  bool dirty_ = false;
  std::map<std::string, base::UnguessableToken, std::less<>> revisions_{
      {kPreferredApps, base::UnguessableToken::Create()},
      {kEmbedList, base::UnguessableToken::Create()}};
  base::UnguessableToken admission_ = base::UnguessableToken::Create();
  SEQUENCE_CHECKER(sequence_checker_);
};
}  // namespace alloy::site_records::internal
#endif
