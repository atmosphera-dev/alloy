// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/device_api/managed_configuration_api.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "chrome/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace {
constexpr int kMaxConfigurationFileSize = 5 * 1024 * 1024;
constexpr auto kTrafficAnnotation = net::DefineNetworkTrafficAnnotation(
    "managed_configuration_loader", R"(
      semantics {
        sender: "Web App Managed Configuration Downloader"
        description: "Fetches origin configuration explicitly supplied by device policy."
        trigger: "Changes in ManagedConfigurationPerOrigin policy for an admitted origin."
        data: "No user data."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        cookies_allowed: NO
        setting: "Only used for origins explicitly configured by administrator policy."
        chrome_policy { ManagedConfigurationPerOrigin { ManagedConfigurationPerOrigin: "[]" } }
      })");
}  // namespace

const char ManagedConfigurationAPI::kOriginKey[] = "origin";
const char ManagedConfigurationAPI::kManagedConfigurationUrlKey[] = "managed_configuration_url";
const char ManagedConfigurationAPI::kManagedConfigurationHashKey[] = "managed_configuration_hash";

struct ManagedConfigurationAPI::Entry {
  Entry(const url::Origin& origin, ManagedConfigurationStore::Options options,
        scoped_refptr<base::SequencedTaskRunner> runner)
      : admitted(options.admitted), store(std::move(runner), origin, std::move(options)) {}
  base::RepeatingCallback<bool()> admitted;
  base::SequenceBound<ManagedConfigurationStore> store;
  base::UnguessableToken revision = base::UnguessableToken::Create();
  std::string url;
  std::string hash;
  bool refreshing = false;
  std::unique_ptr<network::SimpleURLLoader> downloader;
  const base::UnguessableToken binding_id = base::UnguessableToken::Create();
  base::CallbackListSubscription retirement;
};

ManagedConfigurationAPI::ManagedConfigurationAPI(PrefService* prefs, StoreFactory stores,
                                                 LoaderGetter loaders)
    : prefs_(prefs), stores_(std::move(stores)), loaders_(std::move(loaders)) {
  CHECK(prefs_ && stores_ && loaders_);
  pref_change_registrar_.Init(prefs_);
  pref_change_registrar_.Add(prefs::kManagedConfigurationPerOrigin, base::BindRepeating(
      &ManagedConfigurationAPI::OnConfigurationPolicyChanged, weak_ptr_factory_.GetWeakPtr()));
  OnConfigurationPolicyChanged();
}
ManagedConfigurationAPI::~ManagedConfigurationAPI() = default;

void ManagedConfigurationAPI::RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterListPref(prefs::kManagedConfigurationPerOrigin);
}

ManagedConfigurationAPI::Entry* ManagedConfigurationAPI::Active(
    const url::Origin& origin, base::UnguessableToken revision) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto found = entries_.find(origin);
  if (found == entries_.end() || found->second->revision != revision ||
      !found->second->admitted.Run()) return nullptr;
  return found->second.get();
}

ManagedConfigurationAPI::Entry* ManagedConfigurationAPI::Bind(const url::Origin& origin) {
  auto found = entries_.find(origin);
  if (found != entries_.end()) {
    if (found->second->admitted.Run()) return found->second.get();
    entries_.erase(found);
  }
  auto weak = weak_ptr_factory_.GetWeakPtr();
  auto factory = stores_;
  auto options = factory.Run(origin);
  if (!weak) return nullptr;
  if (!options || !options->admitted || !options->admitted.Run()) return nullptr;
  auto runner = options->runner ? options->runner : base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});
  auto observe_retirement = std::move(options->observe_retirement);
  auto entry = std::make_unique<Entry>(origin, std::move(*options), std::move(runner));
  auto* result = entry.get();
  entries_.emplace(origin, std::move(entry));
  if (observe_retirement) result->retirement = observe_retirement.Run(base::BindRepeating(
      [](base::WeakPtr<ManagedConfigurationAPI> owner, url::Origin origin,
         base::UnguessableToken binding) {
        if (!owner) return;
        auto found = owner->entries_.find(origin);
        if (found != owner->entries_.end() && found->second->binding_id == binding)
          owner->RevokeOrigin(origin);
      }, weak_ptr_factory_.GetWeakPtr(), origin, result->binding_id));
  return result;
}

void ManagedConfigurationAPI::RevokeOrigin(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Erase authority before destroying downloader/store; reentrant completions
  // cannot find the old binding and cannot acquire a replacement.
  auto removed = entries_.extract(origin);
}

void ManagedConfigurationAPI::GetOriginPolicyConfiguration(const url::Origin& origin,
    const std::vector<std::string>& keys,
    base::OnceCallback<void(std::optional<base::DictValue>)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = entries_.find(origin);
  if (!CanHaveManagedStore(origin) || found == entries_.end() || !found->second->admitted.Run()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  const auto revision = found->second->revision;
  found->second->store.AsyncCall(&ManagedConfigurationStore::Get).WithArgs(keys).Then(
      base::BindOnce([](base::WeakPtr<ManagedConfigurationAPI> owner, url::Origin origin,
                        base::UnguessableToken revision,
                        base::OnceCallback<void(std::optional<base::DictValue>)> reply,
                        std::optional<base::DictValue> result) {
        if (!owner || !owner->Active(origin, revision)) result.reset();
        std::move(reply).Run(std::move(result));
      }, weak_ptr_factory_.GetWeakPtr(), origin, revision, std::move(callback)));
}

void ManagedConfigurationAPI::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto origin = observer->GetOrigin();
  if (CanHaveManagedStore(origin)) {
    observers_[origin].AddObserver(observer);
    RefreshOrigin(origin);
  } else unmanaged_observers_.insert(observer);
}

void ManagedConfigurationAPI::GetOriginPolicyVersion(const url::Origin& origin,
    base::OnceCallback<void(ManagedConfigurationStore::Version)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto found = entries_.find(origin);
  if (found == entries_.end() || !found->second->admitted.Run()) {
    std::move(callback).Run({});
    return;
  }
  const auto revision = found->second->revision;
  found->second->store.AsyncCall(&ManagedConfigurationStore::GetVersion).Then(base::BindOnce(
      [](base::WeakPtr<ManagedConfigurationAPI> owner, url::Origin origin,
          base::UnguessableToken revision,
          base::OnceCallback<void(ManagedConfigurationStore::Version)> reply,
          ManagedConfigurationStore::Version result) {
        if (!owner || !owner->Active(origin, revision)) result = {};
        std::move(reply).Run(std::move(result));
      }, weak_ptr_factory_.GetWeakPtr(), origin, revision, std::move(callback)));
}
void ManagedConfigurationAPI::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (unmanaged_observers_.erase(observer)) return;
  observers_[observer->GetOrigin()].RemoveObserver(observer);
}
bool ManagedConfigurationAPI::CanHaveManagedStore(const url::Origin& origin) {
  return managed_origins_.contains(origin);
}
const std::set<url::Origin>& ManagedConfigurationAPI::GetManagedOrigins() const { return managed_origins_; }

ManagedConfigurationAPI::Policies ManagedConfigurationAPI::ReadPolicies() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Policies desired;
  for (const auto& entry : prefs_->GetList(prefs::kManagedConfigurationPerOrigin)) {
    if (!entry.is_dict()) continue;
    const auto* origin = entry.GetDict().FindString(kOriginKey);
    if (!origin) continue;
    const auto principal = url::Origin::Create(GURL(*origin));
    if (principal.opaque()) continue;
    const auto* url = entry.GetDict().FindString(kManagedConfigurationUrlKey);
    const auto* hash = entry.GetDict().FindString(kManagedConfigurationHashKey);
    desired[principal] = url && hash ? std::make_optional(Configuration(*url, *hash)) : std::nullopt;
  }
  return desired;
}

void ManagedConfigurationAPI::OnConfigurationPolicyChanged() {
  const auto desired = ReadPolicies();
  managed_origins_.clear();
  for (const auto& [origin, configuration] : desired) managed_origins_.insert(origin);
  std::vector<url::Origin> removed;
  for (const auto& [origin, entry] : entries_) if (!desired.contains(origin)) removed.push_back(origin);
  for (const auto& origin : removed) UpdateStoredDataForOrigin(origin, "", "");
  for (const auto& [origin, configuration] : desired) {
    if (configuration) UpdateStoredDataForOrigin(origin, configuration->first, configuration->second);
    else if (auto found = entries_.find(origin); found != entries_.end()) {
      // Missing fields are not an instruction to erase an existing cache.
      // Retire pending old-policy work while preserving the last valid data.
      found->second->revision = base::UnguessableToken::Create();
      found->second->downloader.reset();
      found->second->refreshing = false;
    }
  }
  PromoteObservers();
}

void ManagedConfigurationAPI::RefreshOrigin(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!CanHaveManagedStore(origin)) return;
  const auto desired = ReadPolicies();
  const auto found = desired.find(origin);
  if (found != desired.end()) {
    if (found->second) UpdateStoredDataForOrigin(origin, found->second->first, found->second->second);
    else Bind(origin);
  }
}

void ManagedConfigurationAPI::UpdateStoredDataForOrigin(const url::Origin& origin,
    const std::string& url, const std::string& hash) {
  Entry* entry = nullptr;
  if (url.empty()) {
    const auto found = entries_.find(origin);
    if (found == entries_.end()) return;
    if (!found->second->admitted.Run()) { RevokeOrigin(origin); return; }
    entry = found->second.get();
  } else entry = Bind(origin);
  if (!entry) return;
  if (entry->refreshing && entry->url == url && entry->hash == hash) return;
  entry->revision = base::UnguessableToken::Create();
  entry->url = url;
  entry->hash = hash;
  entry->refreshing = true;
  entry->downloader.reset();
  const auto revision = entry->revision;
  if (url.empty()) {
    entry->store.AsyncCall(&ManagedConfigurationStore::Clear).Then(base::BindOnce(
        &ManagedConfigurationAPI::OnCommitted, weak_ptr_factory_.GetWeakPtr(), origin, revision));
  } else {
    entry->store.AsyncCall(&ManagedConfigurationStore::GetVersion).Then(base::BindOnce(
        &ManagedConfigurationAPI::OnVersion, weak_ptr_factory_.GetWeakPtr(), origin, revision));
  }
}

void ManagedConfigurationAPI::OnVersion(const url::Origin& origin,
    base::UnguessableToken revision, ManagedConfigurationStore::Version version) {
  auto* entry = Active(origin, revision);
  if (!entry) return;
  if (!version.success || version.hash == entry->hash) {
    entry->refreshing = false;
    return;
  }
  auto weak = weak_ptr_factory_.GetWeakPtr();
  auto getter = loaders_;
  auto* factory = getter.Run();
  if (!weak) return;
  entry = Active(origin, revision);
  if (!entry) return;
  if (!factory) { entry->refreshing = false; return; }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(entry->url);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  entry->downloader = network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  entry->downloader->SetRetryOptions(3, network::SimpleURLLoader::RETRY_ON_5XX |
                                       network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  entry->downloader->DownloadToString(factory, base::BindOnce(&ManagedConfigurationAPI::DecodeData,
      weak_ptr_factory_.GetWeakPtr(), origin, revision), kMaxConfigurationFileSize);
}

void ManagedConfigurationAPI::DecodeData(const url::Origin& origin,
    base::UnguessableToken revision, std::optional<std::string> data) {
  auto* entry = Active(origin, revision);
  if (!entry) return;
  entry->downloader.reset();
  if (!data) { entry->refreshing = false; return; }
  auto decoded = base::JSONReader::ReadDict(*data, base::JSON_PARSE_RFC);
  if (!decoded) { entry->refreshing = false; return; }
  base::DictValue configuration;
  for (auto [key, value] : *decoded) {
    std::string serialized;
    if (!base::JSONWriter::Write(value, &serialized)) { entry->refreshing = false; return; }
    configuration.Set(key, std::move(serialized));  // Literal API keys, not paths.
  }
  entry->store.AsyncCall(&ManagedConfigurationStore::SetCurrentPolicy)
      .WithArgs(entry->hash, std::move(configuration)).Then(base::BindOnce(
          &ManagedConfigurationAPI::OnCommitted, weak_ptr_factory_.GetWeakPtr(), origin, revision));
}

void ManagedConfigurationAPI::OnCommitted(const url::Origin& origin,
    base::UnguessableToken revision, ManagedConfigurationStore::UpdateResult result) {
  auto* entry = Active(origin, revision);
  if (!entry) return;
  entry->refreshing = false;
  if (!result.success || !result.changed) return;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  for (auto& observer : observers_[origin]) {
    observer.OnManagedConfigurationChanged();
    if (!weak || !Active(origin, revision)) return;
  }
}

void ManagedConfigurationAPI::PromoteObservers() {
  std::vector<Observer*> promoted;
  for (auto observer : unmanaged_observers_)
    if (CanHaveManagedStore(observer->GetOrigin())) promoted.push_back(observer);
  for (auto* observer : promoted) {
    unmanaged_observers_.erase(observer);
    observers_[observer->GetOrigin()].AddObserver(observer);
  }
}
