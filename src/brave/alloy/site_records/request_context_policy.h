// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_REQUEST_CONTEXT_POLICY_H_
#define BRAVE_ALLOY_SITE_RECORDS_REQUEST_CONTEXT_POLICY_H_

#include "base/supports_user_data.h"
#include "brave/alloy/site_records/record_capability.h"

namespace alloy::site_records {

// Immutable engine policy attached to the browser-owned BrowserContext before
// any of its owners initialize. CEF sharing aliases that BrowserContext, so it
// cannot accidentally lose or override the storage mode. No renderer-provided
// origin, process flag or preference selects this policy.
//
// SupportsUserData keeps this layer independent of content/CEF. Only their
// browser-side adapters may attach or resolve it. Access to the attachment is
// on the owning sequence; issued capabilities support cross-sequence checks.
class RequestContextPolicy final : public base::SupportsUserData::Data {
 public:
  static void AttachStateless(base::SupportsUserData& browser_context,
                              NetworkState network_state);
  static RequestContextPolicy* Get(base::SupportsUserData& browser_context);
  static const RequestContextPolicy* Get(
      const base::SupportsUserData& browser_context);
  static bool IsStateless(const base::SupportsUserData& browser_context);
  static bool IsNoDisk(const base::SupportsUserData& browser_context);

  ~RequestContextPolicy() override;

  NetworkState network_state() const { return network_state_; }
  base::UnguessableToken profile() const { return profile_; }

  // Context-level storage authority never authorizes document/network working
  // state. Those need their own document/network-generation lifetime owners.
  RecordCapability StorageCapability(StorePurpose purpose) const;

  // Called by the browser's shutdown barrier before tearing down its owners.
  // Destruction also revokes as a final safeguard; neither clears a store.
  void Revoke();

 private:
  explicit RequestContextPolicy(NetworkState network_state);

  const NetworkState network_state_;
  const base::UnguessableToken profile_;
  RecordCapabilityIssuer storage_issuer_;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_REQUEST_CONTEXT_POLICY_H_
