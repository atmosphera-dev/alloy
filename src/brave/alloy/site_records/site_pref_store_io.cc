// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store_io.h"
#include "brave/alloy/site_records/embed_list_settings.h"
#include "brave/alloy/site_records/pref_cache_clear.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "components/content_settings/core/browser/content_settings_utils.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/permission_settings_registry.h"
#include "components/content_settings/core/browser/website_settings_registry.h"
#include "url/gurl.h"

namespace alloy::site_records::internal {
namespace {
constexpr std::string_view kExceptions = "profile.content_settings.exceptions.";
constexpr std::string_view kPartitionedExceptions = "profile.content_settings.partitioned_exceptions.";
constexpr std::string_view kZoom = "partition.per_host_zoom_levels";
constexpr std::string_view kManagedList = "profile.managed_configuration.list";
constexpr std::string_view kManagedHash = "profile.managed_configuration.last_hash";
constexpr std::string_view kProtocolHandlers = "custom_handlers.registered_protocol_handlers";
constexpr std::string_view kIgnoredHandlers = "custom_handlers.ignored_protocol_handlers";

// Native origin-indexed dictionaries share record ownership, independently of
// their payload and native key encoding. Unknown non-site prefs still use the
// profile tree; looking like a URL is never an ownership heuristic.
enum class OriginKeyEncoding { kSerialized, kHex };
struct OriginDictionarySpec {
  std::string_view pref;
  OriginKeyEncoding encoding;
  bool require_record;
};
constexpr OriginDictionarySpec kOriginDictionaries[] = {
    {kManagedHash, OriginKeyEncoding::kHex, true},
    {"protocol_handler.allowed_origin_protocol_pairs", OriginKeyEncoding::kSerialized, false},
};
const OriginDictionarySpec* OriginDictionaryFor(std::string_view key) {
  for (const auto& spec : kOriginDictionaries) if (spec.pref == key) return &spec;
  return nullptr;
}

std::string Domain(const GURL& url) {
  return RecordDomainForPrincipal(url, url::Origin::Create(url));
}

GURL HostUrl(std::string_view host) {
  return ContentSettingsPattern::FromString(host).ToRepresentativeUrl();
}

const content_settings::WebsiteSettingsRegistry* Registry(
    const content_settings::WebsiteSettingsRegistry* selected) {
  if (selected) return selected;
  content_settings::ContentSettingsRegistry::GetInstance();
  content_settings::PermissionSettingsRegistry::GetInstance();
  return content_settings::WebsiteSettingsRegistry::GetInstance();
}

// Preserve native pref keys (including dots) and nested dictionaries. A scalar
// cannot legitimately occur in two owner shards; accepting it would conceal a
// wrong-owner write or a corrupt shard instead of reporting it.
bool Merge(base::DictValue* into, const base::DictValue& from) {
  for (auto [key, value] : from) {
    auto* old = into->Find(key);
    if (!old) {
      into->Set(key, value.Clone());
    } else if (old->is_dict() && value.is_dict()) {
      if (!Merge(&old->GetDict(), value.GetDict())) return false;
    } else if (old->is_list() && value.is_list()) {
      for (const auto& entry : value.GetList()) old->GetList().Append(entry.Clone());
    } else {
      return false;
    }
  }
  return true;
}

void WipeBuffers(std::map<std::string, std::string>* buffers) {
  for (auto& [domain, bytes] : *buffers) Cleanse(base::as_writable_byte_span(bytes));
}
}  // namespace

SitePrefStore::SitePrefStore(std::shared_ptr<RecordFileSystem> files,
                             RecordPrefStore::ErrorCallback on_error,
                             const content_settings::WebsiteSettingsRegistry* registry)
    : files_(std::move(files)),
      profile_(files_ ? files_->ProfileCapability(StorePurpose::kProfileSettings)
                      : RecordCapability()),
      on_error_(std::move(on_error)), registry_(registry) {
  CHECK(on_error_);
}

SitePrefStore::~SitePrefStore() {
  if (dirty_ && Admitted()) FlushImpl();
}

bool SitePrefStore::Admitted() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (invalidated_ || status_ != FileStatus::kOk || !files_ ||
      !profile_.Allows(StorePurpose::kProfileSettings, StorageAccess::kReadRecordFiles)) return false;
  for (const auto& [domain, cap] : capabilities_) if (!cap.active()) return false;
  return true;
}

bool SitePrefStore::GetValue(std::string_view key, const base::Value** result) const {
  return initialized_ && Admitted() && values_.GetValue(key, result);
}
base::DictValue SitePrefStore::GetValues() const {
  return initialized_ && Admitted() ? values_.AsDict() : base::DictValue();
}
bool SitePrefStore::GetMutableValue(std::string_view key, base::Value** result) {
  return initialized_ && Admitted() && values_.GetValue(key, result);
}
void SitePrefStore::AddObserver(PrefStore::Observer* observer) { observers_.AddObserver(observer); }
void SitePrefStore::RemoveObserver(PrefStore::Observer* observer) { observers_.RemoveObserver(observer); }
bool SitePrefStore::HasObservers() const { return !observers_.empty(); }
bool SitePrefStore::IsInitializationComplete() const { return initialized_; }
bool SitePrefStore::ReadOnly() const { return !Admitted(); }
bool SitePrefStore::HasReadErrorDelegate() const { return false; }

FileStatus SitePrefStore::last_status() const {
  return status_ != FileStatus::kOk ? status_ :
      Admitted() ? FileStatus::kOk : FileStatus::kDenied;
}
PersistentPrefStore::PrefReadError SitePrefStore::GetReadError() const {
  const auto status = last_status();
  return status == FileStatus::kOk ? PREF_READ_ERROR_NONE :
      status == FileStatus::kDenied ? PREF_READ_ERROR_ACCESS_DENIED : PREF_READ_ERROR_FILE_OTHER;
}

FileStatus SitePrefStore::Fail(FileStatus status) {
  if (status_ != FileStatus::kOk) return status_;
  if (status != FileStatus::kOk) {
    status_ = status;
    timer_.Stop();
    dirty_ = false;
    persisted_.clear();
    RetireAdmission();
    values_.Clear();
    // General native pref consumers can perform mutable migrations while
    // rebuilding; the lifecycle owner tears those services down on failure.
    // This explicit preference has a read-only snapshot/invalidation contract.
    observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnPrefValueChanged, kPreferredApps);
    on_error_.Run(status);
  }
  return status;
}

void SitePrefStore::Changed(std::string_view key, bool notify) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  AdvanceRevision(key);
  dirty_ = true;
  if (!timer_.IsRunning()) timer_.Start(FROM_HERE, base::Seconds(1),
      base::BindOnce([](SitePrefStore* store) { store->Flush(); }, base::Unretained(this)));
  if (notify) observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnPrefValueChanged, key);
}
void SitePrefStore::ReportValueChanged(std::string_view key, uint32_t flags) {
  if (initialized_ && Admitted()) Changed(key, true);
}
void SitePrefStore::SetValue(std::string_view key, base::Value value, uint32_t flags) {
  if (initialized_ && Admitted() && values_.SetValue(key, std::move(value))) Changed(key, true);
}
void SitePrefStore::SetValueSilently(std::string_view key, base::Value value, uint32_t flags) {
  if (initialized_ && Admitted() && values_.SetValue(key, std::move(value))) Changed(key, false);
}
void SitePrefStore::RemoveValue(std::string_view key, uint32_t flags) {
  if (initialized_ && Admitted() && values_.RemoveValue(key)) Changed(key, true);
}
void SitePrefStore::RemoveValuesByPrefixSilently(std::string_view prefix) {
  if (!initialized_ || !Admitted()) return;
  std::vector<std::string> removed;
  for (const auto& [key, value] : values_)
    if (base::StartsWith(key, prefix)) removed.push_back(key);
  values_.ClearWithPrefix(prefix);
  for (const auto& key : removed) Changed(key, false);
}

FileStatus SitePrefStore::Split(const PrefValueMap& values, std::string_view key, const base::Value& value,
                                std::string_view excluding, Shards* shards,
                                base::Value* retained,
                                const content_settings::WebsiteSettingsRegistry* registry) {
  *retained = value.Clone();
  if (key == kPreferredApps) {
    if (!value.is_list()) return FileStatus::kInvalid;
    apps::PreferredApps validated;
    if (DecodePreferredAppRecords(value.GetList(), &validated) != FileStatus::kOk) return FileStatus::kInvalid;
    base::ListValue kept;
    for (const auto& fragment : value.GetList()) {
      std::string owner;
      if (PreferredAppRecordOwner(fragment, &owner) != FileStatus::kOk) return FileStatus::kInvalid;
      if (!excluding.empty() && owner == excluding) continue;
      auto* list = (*shards)[owner].FindList(key);
      if (!list) list = &(*shards)[owner].Set(key, base::ListValue())->GetList();
      list->Append(fragment.Clone());
      kept.Append(fragment.Clone());
    }
    *retained = base::Value(std::move(kept));
    return FileStatus::kOk;
  }
  registry = Registry(registry);
  const bool exception = IsRegisteredExceptionPref(key, registry);
  const bool protocol = key == kProtocolHandlers || key == kIgnoredHandlers;
  const auto* origin_dictionary = OriginDictionaryFor(key);
  const bool migration = key == "profile.content_settings.exceptions.migrate_geolocation" ||
      key == "profile.content_settings.exceptions.has_migrated_local_network_access";
  if (migration && !value.is_bool()) return FileStatus::kInvalid;
  if (!exception && (base::StartsWith(key, kExceptions) || base::StartsWith(key, kPartitionedExceptions)) &&
      !migration) {
    return FileStatus::kInvalid;
  }
  // Ordinary explicit non-site configuration is encrypted in the profile
  // namespace. URL-like strings are NOT a heuristic for classifying settings.
  if (!exception && key != kZoom && key != kEmbedList &&
      key != kManagedList && !origin_dictionary && !protocol) {
    (*shards)[""].Set(key, value.Clone());
    return FileStatus::kOk;
  }
  const bool is_list = key == kManagedList || protocol;
  if ((!is_list && !value.is_dict()) || (is_list && !value.is_list())) return FileStatus::kInvalid;
  base::DictValue kept;
  base::ListValue kept_list;
  if (is_list) {
    for (const auto& entry : value.GetList()) {
      if (!entry.is_dict()) return FileStatus::kInvalid;
      const auto* origin = entry.GetDict().FindString(protocol ? "url" : "origin");
      if (!origin) return FileStatus::kInvalid;
      const auto owner = Domain(GURL(*origin));
      if (owner.empty() && protocol && GURL(*origin).is_valid()) {
        kept_list.Append(entry.Clone()); continue;  // Nonpersistent principal: volatile only.
      }
      if (owner.empty()) return FileStatus::kInvalid;
      if (owner == excluding) continue;
      auto* list = (*shards)[owner].FindList(key);
      if (!list) list = &(*shards)[owner].Set(key, base::ListValue())->GetList();
      list->Append(entry.Clone());
      kept_list.Append(entry.Clone());
    }
    *retained = base::Value(std::move(kept_list));
    return FileStatus::kOk;
  }
  for (auto [entry_key, entry] : value.GetDict()) {
    if (key == kZoom) {
      if (!entry.is_dict()) return FileStatus::kInvalid;
      base::DictValue kept_hosts;
      for (auto [host, zoom] : entry.GetDict()) {
        const auto owner = Domain(HostUrl(host));
        if (owner == excluding && !excluding.empty()) continue;
        kept_hosts.Set(host, zoom.Clone());
        if (owner.empty()) continue;  // Nonpersistent principal: volatile only.
        auto* zooms = (*shards)[owner].FindDict(key);
        if (!zooms) zooms = &(*shards)[owner].Set(key, base::DictValue())->GetDict();
        auto* hosts = zooms->FindDict(entry_key);
        if (!hosts) hosts = &zooms->Set(entry_key, base::DictValue())->GetDict();
        hosts->Set(host, zoom.Clone());
      }
      if (!kept_hosts.empty()) kept.Set(entry_key, std::move(kept_hosts));
      continue;
    }
    std::string owner;
    bool persistent = true;
    base::Value copy = entry.Clone();
    if (key == kEmbedList) {
      owner = Domain(HostUrl(entry_key));
      if (owner.empty() || !entry.is_list()) return FileStatus::kInvalid;
      base::ListValue embeds;
      for (const auto& embedded : entry.GetList()) {
        if (!embedded.is_string()) return FileStatus::kInvalid;
        const auto domain = Domain(HostUrl(embedded.GetString()));
        if (domain.empty()) return FileStatus::kInvalid;
        if (domain != excluding) embeds.Append(embedded.Clone());
      }
      copy = base::Value(std::move(embeds));
    } else if (origin_dictionary) {
      std::string origin = entry_key;
      if (origin_dictionary->encoding == OriginKeyEncoding::kHex &&
          !base::HexStringToString(entry_key, &origin)) return FileStatus::kInvalid;
      const GURL principal(origin);
      if (!principal.is_valid()) return FileStatus::kInvalid;
      owner = Domain(principal);
      if (owner.empty() && origin_dictionary->require_record) return FileStatus::kInvalid;
      // R16 nonpersistent principals remain volatile; never put their grants in
      // the profile-private shard just because their record domain is empty.
      persistent = !owner.empty();
    } else {
      const auto pair = content_settings::ParsePatternString(entry_key);
      if (!pair.first.IsValid() || !pair.second.IsValid()) return FileStatus::kInvalid;
      const auto primary = Domain(pair.first.ToRepresentativeUrl());
      const auto secondary = Domain(pair.second.ToRepresentativeUrl());
      if (!excluding.empty() && (primary == excluding || secondary == excluding)) continue;
      owner = primary.empty() ? secondary : primary;
      // A genuine global default has no site owner. A nonpersistent principal
      // must not become profile-private simply because its record key is empty.
      if (owner.empty()) persistent = pair.first.MatchesAllHosts() && pair.second.MatchesAllHosts();
      if (!primary.empty() && !secondary.empty() && primary != secondary) {
        persistent = ConsideredFirstParty(values, pair.second.ToRepresentativeUrl(),
                                           pair.first.ToRepresentativeUrl());
      }
      const auto* sharing = registry->Get(
          ContentSettingsType::FEDERATED_IDENTITY_SHARING);
      if (sharing && (key == sharing->pref_name() || key == sharing->partitioned_pref_name())) {
        if (!copy.is_dict()) return FileStatus::kInvalid;
        auto* setting = copy.GetDict().FindDict("setting");
        auto* objects = setting ? setting->FindList("chosen-objects") : nullptr;
        if (!objects) return FileStatus::kInvalid;
        base::ListValue retained_objects;
        for (const auto& object : *objects) {
          if (!object.is_dict()) return FileStatus::kInvalid;
          bool removed = false;
          for (const auto* role : {"rp-requester", "rp-embedder", "idp-origin"}) {
            const auto* participant = object.GetDict().FindString(role);
            if (participant && !excluding.empty() && Domain(GURL(*participant)) == excluding)
              removed = true;
          }
          if (!removed) retained_objects.Append(object.Clone());
        }
        if (retained_objects.empty()) continue;
        setting->Set("chosen-objects", std::move(retained_objects));
      }
    }
    if (!excluding.empty() && owner == excluding) continue;
    kept.Set(entry_key, copy.Clone());
    if (!persistent) continue;
    auto* entries = (*shards)[owner].FindDict(key);
    if (!entries) entries = &(*shards)[owner].Set(key, base::DictValue())->GetDict();
    entries->Set(entry_key, std::move(copy));
  }
  *retained = base::Value(std::move(kept));
  return FileStatus::kOk;
}

FileStatus SitePrefStore::Build(std::string_view excluding, Shards* shards,
                                PrefValueMap* retained) const {
  return BuildValues(values_, excluding, shards, retained, registry_);
}
FileStatus SitePrefStore::BuildValues(const PrefValueMap& values, std::string_view excluding,
                                      Shards* shards, PrefValueMap* retained,
                                      const content_settings::WebsiteSettingsRegistry* registry) {
  for (const auto& [key, value] : values) {
    base::Value next;
    const auto status = Split(values, key, value, excluding, shards, &next, registry);
    if (status != FileStatus::kOk) return status;
    retained->SetValue(key, std::move(next));
  }
  return FileStatus::kOk;
}

FileStatus SitePrefStore::Prepare(const Shards& shards, std::string_view excluding,
                                  std::map<std::string, std::string>* buffers,
                                  std::vector<SettingsReplacement>* replacements) {
  std::set<std::string> owners;
  for (const auto& [owner, value] : shards) owners.insert(owner);
  for (const auto& [owner, value] : persisted_) owners.insert(owner);
  for (const auto& owner : owners) {
    if (!excluding.empty() && owner == excluding) continue;
    const auto fresh = shards.find(owner);
    const auto old = persisted_.find(owner);
    base::DictValue empty;
    const auto& value = fresh == shards.end() ? empty : fresh->second;
    if (old != persisted_.end() && old->second == value) continue;
    RecordCapability cap = profile_;
    const auto purpose = owner.empty() ? StorePurpose::kProfileSettings : StorePurpose::kSiteSettings;
    if (!owner.empty()) {
      const auto found = capabilities_.find(owner);
      if (found != capabilities_.end()) {
        cap = found->second;
        if (!cap.active()) return FileStatus::kDenied;
      } else {
        RecordInfo record;
        const auto url = HostUrl(owner);
        const auto status = files_->ResolveRecord(url, url::Origin::Create(url), true, &record);
        if (status != FileStatus::kOk) return status;
        cap = files_->Capability(record.id, purpose);
        capabilities_.emplace(owner, cap);
      }
    }
    base::DictValue envelope;
    envelope.Set("values", value.Clone());
    auto& bytes = (*buffers)[owner];
    if (!base::JSONWriter::Write(envelope, &bytes)) return FileStatus::kInvalid;
    replacements->push_back({cap, purpose, "preferences", base::as_byte_span(bytes)});
  }
  return FileStatus::kOk;
}

FileStatus SitePrefStore::Load() {
  if (!Admitted()) return FileStatus::kDenied;
  std::vector<RecordInfo> records;
  auto status = files_->ListRecords(&records);
  if (status != FileStatus::kOk) return status;
  capabilities_[""] = profile_;
  for (const auto& record : records) capabilities_[record.domain] = files_->Capability(record.id, StorePurpose::kSiteSettings);
  base::DictValue merged;
  for (const auto& [owner, cap] : capabilities_) {
    const auto purpose = owner.empty() ? StorePurpose::kProfileSettings : StorePurpose::kSiteSettings;
    auto shard = base::MakeRefCounted<RecordPrefStore>(files_, cap, purpose, "preferences",
        base::BindRepeating([](FileStatus) {}));
    if (shard->ReadPrefs() != PREF_READ_ERROR_NONE) return shard->last_status();
    const base::Value* value = nullptr;
    if (!shard->GetValue("values", &value)) continue;
    if (!value->is_dict()) return FileStatus::kIntegrity;
    if (!Merge(&merged, value->GetDict())) return FileStatus::kIntegrity;
    persisted_[owner] = value->GetDict().Clone();
  }
  for (auto [key, value] : merged) values_.SetValue(key, std::move(value));
  // Authenticate the semantic owner as well as the ciphertext. Misplaced site
  // facts in a profile shard must fail, not silently become an extra copy.
  Shards expected;
  PrefValueMap retained;
  status = Build({}, &expected, &retained);
  if (status != FileStatus::kOk) return FileStatus::kIntegrity;
  for (const auto& [owner, value] : persisted_) {
    const auto found = expected.find(owner);
    if (found == expected.end() ? !value.empty() : value != found->second) return FileStatus::kIntegrity;
  }
  return FileStatus::kOk;
}

PersistentPrefStore::PrefReadError SitePrefStore::ReadPrefs() {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!initialized_) {
    const auto status = Load();
    initialized_ = true;
    Fail(status);
    observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnInitializationCompleted, status_ == FileStatus::kOk);
  }
  return GetReadError();
}
void SitePrefStore::ReadPrefsAsync(ReadErrorDelegate* error_delegate) {
  std::unique_ptr<ReadErrorDelegate> delegate(error_delegate);
  const auto error = ReadPrefs();
  if (delegate && error != PREF_READ_ERROR_NONE) delegate->OnError(error);
}

FileStatus SitePrefStore::FlushImpl() {
  timer_.Stop();
  if (!initialized_ || !Admitted()) return FileStatus::kDenied;
  if (!dirty_) return FileStatus::kOk;
  Shards shards;
  PrefValueMap retained;
  auto status = Build({}, &shards, &retained);
  std::map<std::string, std::string> buffers;
  std::vector<SettingsReplacement> replacements;
  if (status == FileStatus::kOk) status = Prepare(shards, {}, &buffers, &replacements);
  if (status == FileStatus::kOk) status = files_->ReplaceSettings(replacements);
  WipeBuffers(&buffers);
  if (status != FileStatus::kOk) return Fail(status);
  persisted_ = std::move(shards);
  dirty_ = false;
  return FileStatus::kOk;
}
FileStatus SitePrefStore::Flush() {
  scoped_refptr<SitePrefStore> keep_alive(this);
  return status_ == FileStatus::kOk ? FlushImpl() : status_;
}
void SitePrefStore::CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) {
  Flush();
  if (done) std::move(done).Run();
  if (reply) base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, std::move(reply));
}
void SitePrefStore::SchedulePendingLossyWrites() { Flush(); }
void SitePrefStore::OnStoreDeletionFromDisk() { Invalidate(); }

SitePrefStore::PreferredVersion SitePrefStore::Install(PrefValueMap* next, bool notify,
                                                      bool admission_changed,
                                                      std::string_view publication_key) {
  std::vector<std::string> changed;
  values_.GetDifferingKeys(next, &changed);
  const bool preferred_changed = std::ranges::find(changed, kPreferredApps) != changed.end();
  for (const auto& key : changed) AdvanceRevision(key);
  // One publication owns its value and lifetime event. A forced event is
  // necessary for unchanged values, but must not retire a newly queued action
  // a second time after observers have already handled this invalidation.
  if (admission_changed && !preferred_changed) changed.emplace_back(kPreferredApps);
  values_.Swap(next);
  const auto installed = Version(publication_key);
  if (notify) for (const auto& key : changed)
    observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnPrefValueChanged, key);
  return installed;
}
void SitePrefStore::Invalidate() {
  scoped_refptr<SitePrefStore> keep_alive(this);
  invalidated_ = true;
  timer_.Stop();
  dirty_ = false;
  for (auto& [domain, values] : persisted_) ClearPrefCache(values);
  persisted_.clear();
  capabilities_.clear();
  RetireAdmission();
  PrefValueMap empty;
  Install(&empty, true, true);
  ClearPrefCache(empty);
}

bool SitePrefStore::IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const {
  return Admitted() && ConsideredFirstParty(values_, outer, embedded);
}
bool SitePrefStore::ConsideredFirstParty(const PrefValueMap& values, const GURL& outer,
                                        const GURL& embedded) {
  const base::Value* value = nullptr;
  values.GetValue(kEmbedList, &value);
  return embed_list::Contains(value, outer, embedded);
}
FileStatus SitePrefStore::SetConsideredFirstParty(const GURL& outer, const GURL& embedded, bool allowed) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!initialized_ || !Admitted()) return FileStatus::kDenied;
  const auto outer_domain = Domain(outer), embedded_domain = Domain(embedded);
  if (outer_domain.empty() || embedded_domain.empty()) return FileStatus::kInvalid;
  std::vector<std::string> previous;
  const auto status = GetFirstPartyEmbeds(outer, &previous);
  if (status != FileStatus::kOk) return status;
  if ((std::ranges::find(previous, embedded_domain) != previous.end()) == allowed) return Flush();
  base::ListValue list;
  for (const auto& domain : previous) if (domain != embedded_domain) list.Append(domain);
  if (allowed) list.Append(embedded_domain);
  return ReplaceFirstPartyEmbeds(outer_domain, std::move(list));
}

FileStatus SitePrefStore::GetFirstPartyEmbeds(const GURL& outer, std::vector<std::string>* domains) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!domains) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  const auto owner = Domain(outer);
  if (owner.empty()) return FileStatus::kInvalid;
  const base::Value* value = nullptr;
  values_.GetValue(kEmbedList, &value);
  return embed_list::Read(value, outer, domains) ? FileStatus::kOk : Fail(FileStatus::kIntegrity);
}

FileStatus SitePrefStore::SetFirstPartyEmbeds(const GURL& outer, const std::vector<GURL>& embedded) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!initialized_ || !Admitted()) return FileStatus::kDenied;
  auto replacement = embed_list::Canonicalize(outer, embedded);
  if (!replacement) return FileStatus::kInvalid;
  return ReplaceFirstPartyEmbeds(replacement->outer, std::move(replacement->embedded));
}

FileStatus SitePrefStore::ReplaceFirstPartyEmbeds(const std::string& outer, base::ListValue embedded) {
  const base::Value* old = nullptr;
  base::DictValue value;
  if (values_.GetValue(kEmbedList, &old)) {
    if (!old->is_dict()) return Fail(FileStatus::kInvalid);
    value = old->GetDict().Clone();
  }
  const auto* previous_value = value.Find(outer);
  if (previous_value && !previous_value->is_list()) return Fail(FileStatus::kIntegrity);
  const auto* previous = previous_value ? &previous_value->GetList() : nullptr;
  if (previous ? *previous == embedded : embedded.empty()) return Flush();
  if (embedded.empty()) value.Remove(outer);
  else value.Set(outer, std::move(embedded));
  PreferredVersion published;
  return PublishValue(kEmbedList, base::Value(std::move(value)), &published);
}

FileStatus SitePrefStore::GetPreferredApps(apps::PreferredApps* entries) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!entries) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  const base::Value* value = nullptr;
  if (!values_.GetValue(kPreferredApps, &value)) { entries->clear(); return FileStatus::kOk; }
  if (!value->is_list()) return Fail(FileStatus::kIntegrity);
  const auto status = DecodePreferredAppRecords(value->GetList(), entries);
  return status == FileStatus::kOk ? status : Fail(FileStatus::kIntegrity);
}

FileStatus SitePrefStore::SetPreferredApps(const apps::PreferredApps& entries) {
  PreferredVersion published;
  return PublishPreferredApps(preferred_version(), entries, &published);
}

SitePrefStore::PreferredVersion SitePrefStore::preferred_version() const {
  return Version(kPreferredApps);
}
bool SitePrefStore::PreferredAdmission(const PreferredVersion& version) const {
  return initialized_ && Admitted() && version.admission == admission_;
}
SitePrefStore::PreferredVersion SitePrefStore::Version(std::string_view key) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto found = revisions_.find(key);
  CHECK(found != revisions_.end());
  return {found->second, admission_};
}
void SitePrefStore::AdvanceRevision(std::string_view key) {
  const auto found = revisions_.find(key);
  if (found != revisions_.end()) found->second = base::UnguessableToken::Create();
}
void SitePrefStore::RetireAdmission() {
  admission_ = base::UnguessableToken::Create();
}
FileStatus SitePrefStore::PublishPreferredApps(const PreferredVersion& expected,
    const apps::PreferredApps& entries, PreferredVersion* published) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  // expected may be a reference into this owner; capture it before publication.
  const auto original = expected;
  if (!published) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return FileStatus::kDenied;
  if (original != preferred_version()) return FileStatus::kBusy;
  apps::PreferredApps previous;
  auto status = GetPreferredApps(&previous);
  if (status != FileStatus::kOk) return status;
  bool equal = previous.size() == entries.size();
  if (equal) for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i] || !entries[i]->intent_filter || previous[i]->app_id != entries[i]->app_id ||
        *previous[i]->intent_filter != *entries[i]->intent_filter) { equal = false; break; }
  }
  if (equal) {
    status = Flush();
    if (status != FileStatus::kOk) return status;
    if (original != preferred_version() || !PreferredAdmission(original)) return FileStatus::kBusy;
    *published = original;
    return FileStatus::kOk;
  }
  base::ListValue fragments;
  status = EncodePreferredAppRecords(entries, &fragments);
  if (status != FileStatus::kOk) return status;
  return PublishValue(kPreferredApps, base::Value(std::move(fragments)), published);
}

FileStatus SitePrefStore::PublishValue(std::string_view key, base::Value value,
                                      PreferredVersion* published) {
  PrefValueMap candidate;
  for (const auto& [name, current] : values_) candidate.SetValue(name, current.Clone());
  candidate.SetValue(key, std::move(value));
  Shards shards;
  PrefValueMap retained;
  auto status = BuildValues(candidate, {}, &shards, &retained, registry_);
  std::map<std::string, std::string> buffers;
  std::vector<SettingsReplacement> replacements;
  if (status == FileStatus::kOk) status = Prepare(shards, {}, &buffers, &replacements);
  if (status == FileStatus::kOk) status = files_->ReplaceSettings(replacements);
  WipeBuffers(&buffers);
  if (status != FileStatus::kOk) return Fail(status);
  timer_.Stop();
  dirty_ = false;
  persisted_ = std::move(shards);
  // The same installation owner captures the published identity before
  // notifications, even if a callback replaces or deletes it synchronously.
  const auto version = Install(&retained, true, false, key);
  if (!PreferredAdmission(version) || version != Version(key)) return FileStatus::kBusy;
  *published = version;
  return FileStatus::kOk;
}

FileStatus SitePrefStore::DeleteSite(const GURL& site, std::function<void()> teardown,
                                     RecordDeletion* result) {
  base::UnguessableToken ticket;
  const auto status = RetireSite(site, &ticket);
  if (status != FileStatus::kOk) return status;
  if (teardown) teardown();
  return files_->FinishRecordDeletion(ticket, result);
}

FileStatus SitePrefStore::RetireSite(const GURL& site, base::UnguessableToken* ticket,
                                     const RecordInfo* expected) {
  scoped_refptr<SitePrefStore> keep_alive(this);
  if (!initialized_ || !Admitted()) return FileStatus::kDenied;
  const auto owner = Domain(site);
  if (owner.empty()) return FileStatus::kInvalid;
  RecordInfo record;
  auto status = files_->ResolveRecord(site, url::Origin::Create(site), false, &record);
  if (status != FileStatus::kOk) return status;
  if (expected && record.id != expected->id) return FileStatus::kBusy;
  Shards shards;
  PrefValueMap retained;
  status = Build(owner, &shards, &retained);
  std::map<std::string, std::string> buffers;
  std::vector<SettingsReplacement> replacements;
  if (status == FileStatus::kOk) status = Prepare(shards, owner, &buffers, &replacements);
  if (status == FileStatus::kOk) status = files_->RetireRecordWithSettings(record.id, replacements, ticket,
      expected ? std::optional(expected->last_visit_us) : std::nullopt);
  if (status == FileStatus::kOk) {
    timer_.Stop();
    dirty_ = false;
    capabilities_.erase(owner);
    persisted_ = std::move(shards);
    RetireAdmission();
    Install(&retained, true, true);
  }
  WipeBuffers(&buffers);
  if (expected && (status == FileStatus::kBusy || status == FileStatus::kNotFound)) return status;
  return status == FileStatus::kOk ? status : Fail(status);
}

bool SitePrefStore::IsRegisteredExceptionPref(std::string_view key,
    const content_settings::WebsiteSettingsRegistry* registry) {
  registry = Registry(registry);
  for (const auto* info : *registry)
    if (key == info->pref_name() || key == info->partitioned_pref_name()) return true;
  return false;
}
void SitePrefStore::InitializeRegistries() { IsRegisteredExceptionPref({}); }

FileStatus SitePrefStore::PruneSnapshot(base::DictValue values, std::string_view excluding,
                                       base::DictValue* retained) {
  PrefValueMap input, output;
  for (auto [key, value] : values) input.SetValue(key, std::move(value));
  Shards shards;
  const auto status = BuildValues(input, excluding, &shards, &output);
  if (status != FileStatus::kOk) return status;
  for (const auto& [key, value] : output) retained->Set(key, value.Clone());
  return FileStatus::kOk;
}

base::DictValue SitePrefStore::Snapshot() const {
  base::DictValue result;
  for (const auto& [key, value] : values_) result.Set(key, value.Clone());
  return result;
}

void SitePrefStore::ApplySnapshot(base::DictValue values) {
  PrefValueMap next;
  for (auto [key, value] : values) next.SetValue(key, std::move(value));
  std::vector<std::string> changed;
  values_.GetDifferingKeys(&next, &changed);
  if (!changed.empty()) {
    values_.Swap(&next);
    dirty_ = true;
  }
}
}  // namespace alloy::site_records::internal
