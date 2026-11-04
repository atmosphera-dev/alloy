// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NET_BASE_ALLOY_NETWORK_RECORD_TARGET_H_
#define NET_BASE_ALLOY_NETWORK_RECORD_TARGET_H_

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/schemeful_site.h"
#include "net/base/session_usage.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace net {

// One R16 calculation for browser storage ownership and native network targets.
inline std::string AlloyRecordDomainForPrincipal(const GURL& principal,
                                                const url::Origin& origin) {
  if (!principal.is_valid() || principal.SchemeIs(url::kAboutScheme) ||
      principal.SchemeIs(url::kFileScheme) ||
      principal.SchemeIs(url::kDataScheme) || origin.opaque() ||
      origin.scheme() == url::kFileScheme ||
      origin.scheme() == url::kDataScheme ||
      origin.scheme() == url::kAboutScheme) {
    return {};
  }
  return SchemefulSite(origin).GetURL().GetHost();
}

// Created by the browser's record coordinator, not a renderer request. Wire
// decoding validates canonical identity again; no URL/domain heuristic at owners.
class AlloyNetworkRecordTarget {
 public:
  static std::optional<AlloyNetworkRecordTarget> FromPrincipal(
      const GURL& principal, const url::Origin& origin) {
    return FromCanonicalDomain(AlloyRecordDomainForPrincipal(principal, origin));
  }
  static std::optional<AlloyNetworkRecordTarget> FromCanonicalDomain(
      const std::string& domain) {
    const GURL principal("https://" + domain + "/");
    if (domain.empty() ||
        AlloyRecordDomainForPrincipal(principal, url::Origin::Create(principal)) !=
            domain) {
      return std::nullopt;
    }
    return AlloyNetworkRecordTarget(domain);
  }
  const std::string& domain() const { return domain_; }
  bool Owns(const url::Origin& origin) const {
    return domain_ == AlloyRecordDomainForPrincipal(origin.GetURL(), origin);
  }
  bool DependsOnTransport(const NetworkAnonymizationKey& key,
                          const GURL& destination,
                          SessionUsage usage = SessionUsage::kDestination) const {
    if (key.GetTopFrameSite() &&
        Owns(url::Origin::Create(key.GetTopFrameSite()->GetURL()))) {
      return true;
    }
    // A destination connection/TLS authority depends on its server's record
    // even when used from another top-level site. Proxy names are not that
    // destination principal; their native NAK still supplies context ownership.
    return usage == SessionUsage::kDestination &&
           (destination.SchemeIsHTTPOrHTTPS() ||
            destination.SchemeIs(url::kWsScheme) ||
            destination.SchemeIs(url::kWssScheme)) &&
           Owns(url::Origin::Create(destination));
  }

 private:
  explicit AlloyNetworkRecordTarget(std::string domain)
      : domain_(std::move(domain)) {}
  std::string domain_;
};

inline base::OnceClosure CommitNetworkRetirementOnDrop(
    base::OnceClosure commit) {
  return base::BindOnce(
      [](base::ScopedClosureRunner pending) { pending.RunAndReset(); },
      base::ScopedClosureRunner(std::move(commit)));
}

}  // namespace net
#endif
