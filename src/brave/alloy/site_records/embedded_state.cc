// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/embedded_state.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/time/time.h"

namespace alloy::site_records {
namespace {

void RetireStates(std::vector<scoped_refptr<EmbeddedState>> states) {
  for (const auto& state : states) {
    state->RevokeAdmission();
  }
  for (const auto& state : states) {
    state->Retire();
  }
}

class ProjectedPolicy final : public EmbeddedFirstPartyPolicy {
 public:
  explicit ProjectedPolicy(Predicate predicate)
      : predicate_(std::move(predicate)) {
    CHECK(predicate_);
  }
  bool IsConsideredFirstParty(const GURL& outer,
                              const GURL& embedded) const override {
    return predicate_.Run(outer, embedded);
  }

 private:
  const Predicate predicate_;
};

}  // namespace

std::unique_ptr<EmbeddedFirstPartyPolicy> EmbeddedFirstPartyPolicy::Bind(
    Predicate predicate) {
  return std::make_unique<ProjectedPolicy>(std::move(predicate));
}

EmbeddedState::EmbeddedState(base::UnguessableToken profile)
    : issuer_(profile,
              base::UnguessableToken::Create(),
              base::UnguessableToken::Create(),
              StorageMode::kMemory) {}

EmbeddedState::~EmbeddedState() {
  Retire();
}

RecordCapability EmbeddedState::Capability(StorePurpose purpose) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A jar never owns profile settings, top-level history or visited links.
  if (IsProfileStorePurpose(purpose) || purpose == StorePurpose::kHistory ||
      purpose == StorePurpose::kVisitedLinks) {
    return {};
  }
  return issuer_.Issue(purpose);
}

KeyedService* EmbeddedState::GetOrCreateService(const void* type,
                                                StorePurpose purpose,
                                                ServiceFactory factory,
                                                const void* native_instance) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto capability = Capability(purpose);
  if (!capability.Allows(purpose, StorageAccess::kWriteMemory)) {
    return nullptr;
  }
  CHECK(type);
  const ServiceKey key{type, native_instance};
  auto [slot, created] = services_.try_emplace(key, ServiceEntry{purpose, {}});
  if (!created) {
    CHECK(slot->second.purpose == purpose);
    return slot->second.service.get();
  }
  // The native constructor may invoke callbacks. Hold this scope through them,
  // recheck the original authority, and never install into a retired owner.
  scoped_refptr<EmbeddedState> lease(this);
  auto service = std::move(factory).Run(capability);
  if (!capability.active()) {
    if (service) {
      service->Shutdown();
    }
    return nullptr;
  }
  if (!service) {
    services_.erase(key);
    return nullptr;
  }
  // Retire() is the only operation that can remove this reserved slot, and
  // the original generation check above excludes that transition.
  slot = services_.find(key);
  CHECK(slot != services_.end());
  slot->second.service = std::move(service);
  return slot->second.service.get();
}

void EmbeddedState::RevokeAdmission() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  issuer_.Revoke();
}

void EmbeddedState::Retire() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RevokeAdmission();
  // Detach before native callbacks, so reentry cannot rediscover a
  // shutting-down instance. Already-issued capabilities are revoked before any
  // callback runs.
  auto services = std::move(services_);
  services_.clear();
  for (auto& [type, entry] : services) {
    if (entry.service) {
      entry.service->Shutdown();
    }
  }
}

EmbeddedStateRegistry::EmbeddedStateRegistry(
    base::UnguessableToken profile,
    StorageMode mode,
    EmbeddedStateScope scope,
    std::unique_ptr<EmbeddedFirstPartyPolicy> policy)
    : profile_(profile),
      mode_(mode),
      scope_(scope),
      policy_(std::move(policy)) {
  CHECK(!profile_.is_empty());
  CHECK(scope_ == EmbeddedStateScope::kPerOuterSite ||
        scope_ == EmbeddedStateScope::kPerTab);
}

EmbeddedStateRegistry::~EmbeddedStateRegistry() {
  Shutdown();
}

EmbeddedStateRegistry::ScopeKey EmbeddedStateRegistry::Key(
    base::UnguessableToken tab,
    const std::string& outer_site) const {
  return {outer_site, scope_ == EmbeddedStateScope::kPerTab
                          ? tab
                          : base::UnguessableToken()};
}

void EmbeddedStateRegistry::CommitOuterSite(
    base::UnguessableToken tab,
    const GURL& document_url,
    const url::Origin& document_origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!tab.is_empty());
  if (closed_) {
    return;
  }
  const auto site = RecordDomainForPrincipal(document_url, document_origin);
  if (revoking_sites_.contains(site)) {
    return;
  }
  auto existing = tabs_.find(tab);
  if (existing != tabs_.end() && existing->second.site == site) {
    existing->second.url = document_url;
    return;
  }
  const auto operation = base::UnguessableToken::Create();
  pending_commits_[tab] = {operation, site};
  auto alive = weak_factory_.GetWeakPtr();
  RemoveTab(tab);
  if (!alive || closed_) {
    return;
  }
  const auto pending = pending_commits_.find(tab);
  if (pending == pending_commits_.end() ||
      pending->second.operation != operation) {
    return;
  }
  pending_commits_.erase(pending);
  if (site.empty()) {
    return;
  }
  tabs_[tab] = {site, base::UnguessableToken::Create(), document_url};
  if (auto entry = scopes_.find(Key(tab, site)); entry != scopes_.end()) {
    entry->second->keepalive.Stop();
  }
}

void EmbeddedStateRegistry::CloseTab(base::UnguessableToken tab) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_commits_.erase(tab);
  RemoveTab(tab);
}

void EmbeddedStateRegistry::RemoveTab(base::UnguessableToken tab) {
  auto found = tabs_.find(tab);
  if (found == tabs_.end()) {
    return;
  }
  const auto key = Key(tab, found->second.site);
  tabs_.erase(found);
  auto scope = scopes_.find(key);
  if (scope == scopes_.end()) {
    return;
  }
  if (scope_ == EmbeddedStateScope::kPerTab) {
    Retire(key);
    return;
  }
  for (const auto& [other, owner] : tabs_) {
    if (owner.site == key.outer_site) {
      return;
    }
  }
  scope->second->keepalive.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(&EmbeddedStateRegistry::Retire, weak_factory_.GetWeakPtr(),
                     key));
}

EmbeddedStateRegistry::Resolution EmbeddedStateRegistry::Resolve(
    base::UnguessableToken tab,
    const GURL& frame_url,
    const url::Origin& frame_origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_ || mode_ == StorageMode::kStateless) {
    return {};
  }
  auto outer = tabs_.find(tab);
  const auto frame_site = RecordDomainForPrincipal(frame_url, frame_origin);
  if (outer == tabs_.end() || frame_site.empty()) {
    return {};
  }
  const auto outer_site = outer->second.site;
  if (revoking_sites_.contains(frame_site) ||
      revoking_sites_.contains(outer_site)) {
    return {};
  }
  if (frame_site == outer_site) {
    return {Backing::kRealSite, nullptr};
  }
  auto alive = weak_factory_.GetWeakPtr();
  const auto generation = outer->second.generation;
  const auto outer_url = outer->second.url;
  const auto operation = base::UnguessableToken::Create();
  pending_resolutions_.emplace(operation, std::pair{outer_site, frame_site});
  const bool listed =
      policy_ && policy_->IsConsideredFirstParty(outer_url, frame_url);
  // A native policy reader must not carry an old frame decision across a
  // synchronous navigation, tab close or context shutdown.
  if (!alive || closed_) {
    return {};
  }
  if (!pending_resolutions_.erase(operation)) {
    return {};
  }
  const auto current = tabs_.find(tab);
  if (current == tabs_.end() || current->second.generation != generation) {
    return {};
  }
  if (listed) {
    return {Backing::kRealSite, nullptr, true};
  }
  const auto key = Key(tab, outer_site);
  auto [entry, created] = scopes_.try_emplace(key);
  if (created) {
    entry->second = std::make_unique<Entry>();
  }
  auto& state = entry->second->sites[frame_site];
  if (!state || !state->Capability(StorePurpose::kDomStorage).active()) {
    state = base::MakeRefCounted<EmbeddedState>(profile_);
  }
  return {Backing::kTemporary, state};
}

void EmbeddedStateRegistry::RevokeSite(const GURL& document_url,
                                       const url::Origin& document_origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  const auto site = RecordDomainForPrincipal(document_url, document_origin);
  if (site.empty() || !revoking_sites_.insert(site).second) {
    return;
  }
  for (auto pending = pending_resolutions_.begin();
       pending != pending_resolutions_.end();) {
    if (pending->second.first == site || pending->second.second == site) {
      pending = pending_resolutions_.erase(pending);
    } else {
      ++pending;
    }
  }
  for (auto pending = pending_commits_.begin();
       pending != pending_commits_.end();) {
    if (pending->second.site == site) {
      pending = pending_commits_.erase(pending);
    } else {
      ++pending;
    }
  }
  for (auto tab = tabs_.begin(); tab != tabs_.end();) {
    if (tab->second.site == site) {
      tab = tabs_.erase(tab);
    } else {
      ++tab;
    }
  }
  std::vector<scoped_refptr<EmbeddedState>> retired;
  for (auto scope = scopes_.begin(); scope != scopes_.end();) {
    if (scope->first.outer_site == site) {
      scope->second->keepalive.Stop();
      for (auto& [embedded_site, state] : scope->second->sites) {
        retired.push_back(std::move(state));
      }
      scope = scopes_.erase(scope);
    } else {
      auto found = scope->second->sites.find(site);
      if (found != scope->second->sites.end()) {
        retired.push_back(std::move(found->second));
        scope->second->sites.erase(found);
      }
      ++scope;
    }
  }
  // Every matching native graph has been detached, and every issuer is revoked
  // before callbacks. No callback can re-admit the same site during this
  // barrier.
  auto alive = weak_factory_.GetWeakPtr();
  RetireStates(std::move(retired));
  if (alive) {
    revoking_sites_.erase(site);
  }
}

void EmbeddedStateRegistry::Retire(const ScopeKey& key) {
  auto found = scopes_.find(key);
  if (found == scopes_.end()) {
    return;
  }
  auto entry = std::move(found->second);
  scopes_.erase(found);
  entry->keepalive.Stop();
  std::vector<scoped_refptr<EmbeddedState>> retired;
  for (auto& [site, state] : entry->sites) {
    retired.push_back(std::move(state));
  }
  RetireStates(std::move(retired));
}

void EmbeddedStateRegistry::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (closed_) {
    return;
  }
  closed_ = true;
  weak_factory_.InvalidateWeakPtrs();
  tabs_.clear();
  pending_commits_.clear();
  pending_resolutions_.clear();
  revoking_sites_.clear();
  auto scopes = std::move(scopes_);
  scopes_.clear();
  std::vector<scoped_refptr<EmbeddedState>> retired;
  for (auto& [key, entry] : scopes) {
    entry->keepalive.Stop();
    for (auto& [site, state] : entry->sites) {
      retired.push_back(std::move(state));
    }
  }
  RetireStates(std::move(retired));
}

}  // namespace alloy::site_records
