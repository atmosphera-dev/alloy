// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef SERVICES_NETWORK_NATIVE_NETWORK_RETIREMENT_H_
#define SERVICES_NETWORK_NATIVE_NETWORK_RETIREMENT_H_

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "net/base/alloy_network_record_target.h"
#include "net/base/isolation_info.h"
#include "net/base/network_anonymization_key.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace network {
// Native ownership facts, borrowed only while the engine's matcher executes.
// Selection policy is supplied by the canonical engine owner, not this view.
struct NativeNetworkOwner {
  const GURL& destination;
  const net::NetworkAnonymizationKey& network_key;
  const url::Origin& initiator;
  const net::IsolationInfo* isolation_info;
};
using NativeNetworkOwnerMatcher =
    base::RepeatingCallback<bool(const NativeNetworkOwner&)>;

// Prepared admission is monotone. Abandoning a prepared operation still has to
// revoke its original native resources rather than leave inert endpoints alive.
using net::CommitNetworkRetirementOnDrop;

// Per-native-object cancellation/drain lifetime. No record or generation
// registry. Declare before a native object's resources so destruction drains
// those first.
class NativeNetworkRetirement final {
 public:
  using Lifetime = std::shared_ptr<std::vector<base::ScopedClosureRunner>>;
  NativeNetworkRetirement() = default;
  NativeNetworkRetirement(const NativeNetworkRetirement&) = delete;
  NativeNetworkRetirement& operator=(const NativeNetworkRetirement&) = delete;
  bool Prepare(const base::RepeatingClosure& done) {
    lifetime_->emplace_back(done);
    return !std::exchange(retired_, true);
  }
  bool BeginRevocation() {
    CHECK(retired_);
    return !std::exchange(revoking_, true);
  }
  bool retired() const { return retired_; }
  Lifetime HoldCallback() const { return lifetime_; }

 private:
  Lifetime lifetime_ =
      std::make_shared<std::vector<base::ScopedClosureRunner>>();
  bool retired_ = false;
  bool revoking_ = false;
};
}  // namespace network
#endif  // SERVICES_NETWORK_NATIVE_NETWORK_RETIREMENT_H_
