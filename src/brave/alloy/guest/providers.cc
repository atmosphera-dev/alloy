// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/providers.h"

#include <utility>
#include <limits>

namespace alloy::guest {
struct Providers::Pending {
  Owner owner;
  ProviderReply reply;
};
struct Providers::Lifetime {
  Providers* owner;
};

Providers::Providers(Owns owns, Post post)
    : owns_(std::move(owns)),
      post_(std::move(post)),
      lifetime_(std::make_shared<Lifetime>(Lifetime{this})) {
  providers_.emplace(Feature::kLinuxLock, LinuxLockProvider());
  providers_.emplace(Feature::kStream, StreamProvider());
  providers_.emplace(Feature::kMedia, MediaProvider());
  providers_.emplace(Feature::kIntegration, IntegrationProvider());
  providers_.emplace(Feature::kAccessibility, AccessibilityProvider());
  providers_.emplace(Feature::kNamespaceLock, NamespaceLockProvider());
  providers_.emplace(Feature::kCapture, CaptureProvider());
}
Providers::~Providers() {
  Close();
  lifetime_->owner = nullptr;
}
bool Providers::Install(Feature feature, Provider provider) {
  if (admitted_ || closed_ || !providers_.contains(feature))
    return false;
  providers_[feature] = std::move(provider);
  return true;
}
void Providers::Admit() { admitted_ = !closed_; }
bool Providers::Available(Feature feature) const {
  auto found = providers_.find(feature);
  return !closed_ && found != providers_.end() && found->second.available();
}
void Providers::Invoke(Feature feature, Owner owner, const std::string& operation,
                         const std::string& payload, ProviderReply reply) {
  if (!reply)
    return;
  const bool release = operation == "@release";
  if (closed_ || !admitted_ || !owner.request || (!release && !owns_(owner))) {
    reply(ProviderResult::kCancelled, {});
    return;
  }
  auto found = providers_.find(feature);
  if (found == providers_.end() ||
      !(release ? !!found->second.release : found->second.available())) {
    reply(ProviderResult::kUnavailable, {});
    return;
  }
  if (next_pending_ == std::numeric_limits<uint64_t>::max()) {
    reply(ProviderResult::kFailed, {});
    return;
  }
  auto pending = std::make_shared<Pending>(Pending{owner, std::move(reply)});
  const uint64_t id = next_pending_++;
  pending_.emplace(id, pending);
  std::weak_ptr<Lifetime> lifetime = lifetime_;
  auto call = release ? found->second.release : found->second.call;
  auto post = post_;
  call(owner, operation, payload,
        [lifetime, pending, post, id, release](ProviderResult result, std::string value) {
    post([lifetime, pending, result, id, release, value = std::move(value)]() mutable {
    auto live = lifetime.lock();
    if (!live || !live->owner || !pending->reply)
      return;
    auto* registry = live->owner;
    auto reply = std::exchange(pending->reply, {});
     registry->pending_.erase(id);
     if (registry->closed_ || (!release && !registry->owns_(pending->owner))) {
      result = ProviderResult::kCancelled;
      value.clear();
    }
    // Erase before dispatch. A callback can close/destroy this registry.
    reply(result, std::move(value));
    });
  });
}
void Providers::Cancel(const Owner& owner) {
  std::vector<std::shared_ptr<Pending>> cancelled;
  for (auto found = pending_.begin(); found != pending_.end();) {
    if (found->second->owner == owner) {
      cancelled.push_back(std::move(found->second));
      found = pending_.erase(found);
    } else {
      ++found;
    }
  }
  for (auto& pending : cancelled) {
    auto reply = std::exchange(pending->reply, {});
    if (reply) reply(ProviderResult::kCancelled, {});
  }
}
void Providers::Close() {
  if (closed_)
    return;
  closed_ = true;
  // Move all owners out before callbacks. Reentrancy cannot install, admit or
  // complete an old request against a newly admitted document.
  auto pending = std::move(pending_);
  for (auto& [id, entry] : pending) {
    auto reply = std::exchange(entry->reply, {});
    if (reply)
      reply(ProviderResult::kCancelled, {});
  }
}
}  // namespace alloy::guest
