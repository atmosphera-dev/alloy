// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class BrowserOwner : public base::SupportsUserData {
 public:
  ~BrowserOwner() override = default;
};

TEST(RequestContextPolicyTest, IsolatedOwnersAndSharingKeepTheirIdentity) {
  BrowserOwner first;
  BrowserOwner second;
  EXPECT_EQ(nullptr, RequestContextPolicy::Get(first));
  RequestContextPolicy::AttachStateless(first, NetworkState::kKeep);
  RequestContextPolicy::AttachStateless(second, NetworkState::kCut);
  const auto* shared = RequestContextPolicy::Get(first);
  EXPECT_EQ(shared, RequestContextPolicy::Get(first));
  EXPECT_EQ(NetworkState::kKeep, shared->network_state());
  EXPECT_EQ(NetworkState::kCut,
            RequestContextPolicy::Get(second)->network_state());
  EXPECT_NE(shared->profile(), RequestContextPolicy::Get(second)->profile());
  EXPECT_FALSE(shared->StorageCapability(StorePurpose::kCookies)
                   .Allows(StorePurpose::kCookies, StorageAccess::kWriteMemory));
}

TEST(RequestContextPolicyTest, ContextLifetimeCannotGrantDocumentWorkingState) {
  BrowserOwner owner;
  RequestContextPolicy::AttachStateless(owner, NetworkState::kKeep);
  const auto* policy = RequestContextPolicy::Get(owner);
  for (auto purpose : {StorePurpose::kDocumentWorkingState,
                       StorePurpose::kNetworkWorkingState}) {
    EXPECT_FALSE(policy->StorageCapability(purpose).active());
    EXPECT_FALSE(policy->StorageCapability(purpose)
                     .Allows(purpose, StorageAccess::kWriteMemory));
  }
}

TEST(RequestContextPolicyTest, ShutdownRevokesEveryOutstandingStorageHandle) {
  RecordCapability pending;
  {
    BrowserOwner owner;
    RequestContextPolicy::AttachStateless(owner, NetworkState::kCut);
    pending = RequestContextPolicy::Get(owner)->StorageCapability(
        StorePurpose::kIndexedDb);
    EXPECT_TRUE(pending.active());
    RequestContextPolicy::Get(owner)->Revoke();
    EXPECT_FALSE(pending.active());
    EXPECT_FALSE(RequestContextPolicy::Get(owner)
                     ->StorageCapability(StorePurpose::kIndexedDb)
                     .active());
  }
  EXPECT_FALSE(pending.active());
}

TEST(RequestContextPolicyTest, MemoryOwnerAdmitsOnlyItsOwnResidentStorage) {
  BrowserOwner first;
  BrowserOwner second;
  RequestContextPolicy::AttachMemory(first);
  RequestContextPolicy::AttachMemory(second);
  const auto* policy = RequestContextPolicy::Get(first);
  EXPECT_TRUE(RequestContextPolicy::IsNoDisk(first));
  EXPECT_FALSE(RequestContextPolicy::IsStateless(first));
  EXPECT_EQ(policy->network_state(), NetworkState::kKeep);
  EXPECT_NE(policy->profile(), RequestContextPolicy::Get(second)->profile());
  for (auto purpose : {StorePurpose::kCookies, StorePurpose::kDomStorage,
                       StorePurpose::kIndexedDb, StorePurpose::kCacheStorage,
                       StorePurpose::kHttpCache, StorePurpose::kSiteSettings}) {
    auto capability = policy->StorageCapability(purpose);
    EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kReadMemory));
    EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kWriteMemory));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadParentProfile));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadRecordFiles));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kWriteRecordFiles));
  }
  auto pending = policy->StorageCapability(StorePurpose::kCookies);
  RequestContextPolicy::Get(first)->Revoke();
  EXPECT_FALSE(pending.active());
  EXPECT_TRUE(RequestContextPolicy::Get(second)
                  ->StorageCapability(StorePurpose::kCookies)
                  .Allows(StorePurpose::kCookies, StorageAccess::kWriteMemory));
}

}  // namespace
}  // namespace alloy::site_records
