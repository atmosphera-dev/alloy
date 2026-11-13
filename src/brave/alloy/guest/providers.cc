// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/providers.h"

#include <utility>

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
  if (closed_ || !admitted_ || !owner.request || !owns_(owner)) {
    reply(ProviderResult::kCancelled, {});
    return;
  }
  auto found = providers_.find(feature);
  if (found == providers_.end() || !found->second.available()) {
    reply(ProviderResult::kUnavailable, {});
    return;
  }
  if (pending_.contains(owner.request)) {
    reply(ProviderResult::kFailed, {});
    return;
  }
  auto pending = std::make_shared<Pending>(Pending{owner, std::move(reply)});
  pending_.emplace(owner.request, pending);
  std::weak_ptr<Lifetime> lifetime = lifetime_;
  auto call = found->second.call;
  auto post = post_;
  call(owner, operation, payload,
        [lifetime, pending, post](ProviderResult result, std::string value) {
    post([lifetime, pending, result, value = std::move(value)]() mutable {
    auto live = lifetime.lock();
    if (!live || !live->owner || !pending->reply)
      return;
    auto* registry = live->owner;
    auto reply = std::exchange(pending->reply, {});
    registry->pending_.erase(pending->owner.request);
    if (registry->closed_ || !registry->owns_(pending->owner)) {
      result = ProviderResult::kCancelled;
      value.clear();
    }
    // Erase before dispatch. A callback can close/destroy this registry.
    reply(result, std::move(value));
    });
  });
}
void Providers::Cancel(const Owner& owner) {
  auto found = pending_.find(owner.request);
  if (found == pending_.end() || found->second->owner != owner)
    return;
  auto pending = std::move(found->second);
  pending_.erase(found);
  auto reply = std::exchange(pending->reply, {});
  reply(ProviderResult::kCancelled, {});
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
