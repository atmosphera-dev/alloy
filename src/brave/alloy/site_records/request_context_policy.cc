// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"

#include <memory>

#include "base/check.h"

namespace alloy::site_records {
namespace {
const char kRequestContextPolicyKey = 0;
}  // namespace

// static
void RequestContextPolicy::AttachStateless(
    base::SupportsUserData& browser_context,
    NetworkState network_state) {
  CHECK(!Get(browser_context));
  CHECK(network_state == NetworkState::kKeep ||
        network_state == NetworkState::kCut);
  browser_context.SetUserData(
      &kRequestContextPolicyKey,
      std::unique_ptr<RequestContextPolicy>(
          new RequestContextPolicy(network_state)));
}

// static
RequestContextPolicy* RequestContextPolicy::Get(
    base::SupportsUserData& browser_context) {
  return static_cast<RequestContextPolicy*>(
      browser_context.GetUserData(&kRequestContextPolicyKey));
}

// static
const RequestContextPolicy* RequestContextPolicy::Get(
    const base::SupportsUserData& browser_context) {
  return static_cast<const RequestContextPolicy*>(
      browser_context.GetUserData(&kRequestContextPolicyKey));
}

RequestContextPolicy::RequestContextPolicy(NetworkState network_state)
    : network_state_(network_state),
      profile_(base::UnguessableToken::Create()),
      storage_issuer_(profile_, base::UnguessableToken::Create(),
                      base::UnguessableToken::Create(), StorageMode::kStateless) {}

RequestContextPolicy::~RequestContextPolicy() = default;

// static
bool RequestContextPolicy::IsStateless(
    const base::SupportsUserData& browser_context) {
  const auto* policy = Get(browser_context);
  return policy && policy->StorageCapability(StorePurpose::kDomStorage).mode() ==
                       StorageMode::kStateless;
}

RecordCapability RequestContextPolicy::StorageCapability(
    StorePurpose purpose) const {
  if (purpose == StorePurpose::kDocumentWorkingState ||
      purpose == StorePurpose::kNetworkWorkingState) {
    return {};
  }
  return storage_issuer_.Issue(purpose);
}

void RequestContextPolicy::Revoke() {
  storage_issuer_.Revoke();
}

}  // namespace alloy::site_records
