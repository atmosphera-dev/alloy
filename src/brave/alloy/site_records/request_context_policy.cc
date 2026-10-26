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
  CHECK(network_state == NetworkState::kKeep ||
        network_state == NetworkState::kCut);
  Attach(browser_context, ContextMode::Stateless(network_state));
}

// static
void RequestContextPolicy::AttachMemory(
    base::SupportsUserData& browser_context) {
  Attach(browser_context, ContextMode::Memory());
}

// static
void RequestContextPolicy::Attach(base::SupportsUserData& browser_context,
                                  ContextMode context_mode) {
  CHECK(!Get(browser_context));
  browser_context.SetUserData(&kRequestContextPolicyKey,
                              std::unique_ptr<RequestContextPolicy>(
                                  new RequestContextPolicy(context_mode)));
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

RequestContextPolicy::RequestContextPolicy(ContextMode context_mode)
    : context_mode_(context_mode),
      profile_(base::UnguessableToken::Create()),
      storage_issuer_(profile_,
                      base::UnguessableToken::Create(),
                      base::UnguessableToken::Create(),
                      context_mode.storage_mode()) {}

RequestContextPolicy::~RequestContextPolicy() = default;

// static
bool RequestContextPolicy::IsStateless(
    const base::SupportsUserData& browser_context) {
  const auto* policy = Get(browser_context);
  return policy && policy->storage_mode() == StorageMode::kStateless;
}

RecordCapability RequestContextPolicy::StorageCapability(
    StorePurpose purpose) const {
  if (purpose == StorePurpose::kDocumentWorkingState ||
      purpose == StorePurpose::kNetworkWorkingState) {
    return {};
  }
  return storage_issuer_.Issue(purpose);
}

// static
bool RequestContextPolicy::IsNoDisk(
    const base::SupportsUserData& browser_context) {
  const auto* policy = Get(browser_context);
  if (!policy) {
    return false;
  }
  const auto mode = policy->storage_mode();
  return mode == StorageMode::kStateless || mode == StorageMode::kMemory;
}

void RequestContextPolicy::Revoke() {
  storage_issuer_.Revoke();
}

}  // namespace alloy::site_records
