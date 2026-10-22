// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_capability.h"

#include <array>
#include <thread>
#include <type_traits>

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

constexpr auto kStoredPurposes = std::to_array<StorePurpose>({
    StorePurpose::kCookies,
    StorePurpose::kDomStorage,
    StorePurpose::kIndexedDb,
    StorePurpose::kCacheStorage,
    StorePurpose::kFileSystem,
    StorePurpose::kServiceWorkers,
    StorePurpose::kBackgroundJobs,
    StorePurpose::kHttpCache,
    StorePurpose::kCodeCache,
    StorePurpose::kHistory,
    StorePurpose::kVisitedLinks,
    StorePurpose::kFavicons,
    StorePurpose::kSiteSettings,
    StorePurpose::kQuota,
    StorePurpose::kTransportSecurity,
    StorePurpose::kServerProperties,
    StorePurpose::kReportingConfiguration,
    StorePurpose::kSharedDictionaries,
    StorePurpose::kMediaDeviceSalts,
    StorePurpose::kOriginTrials,
    StorePurpose::kNotifications,
    StorePurpose::kBlobSpill,
});

constexpr auto kAccesses = std::to_array<StorageAccess>({
    StorageAccess::kReadMemory,
    StorageAccess::kWriteMemory,
    StorageAccess::kReadRecordFiles,
    StorageAccess::kWriteRecordFiles,
    StorageAccess::kReadParentProfile,
    StorageAccess::kRunBackgroundJob,
});

TEST(RecordCapabilityTest, StatelessRejectsEveryStorageAdmission) {
  RecordCapabilityIssuer owner(
      base::UnguessableToken::Create(), base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kStateless);
  for (auto purpose : kStoredPurposes) {
    const auto capability = owner.Issue(purpose);
    for (auto access : kAccesses) {
      EXPECT_FALSE(capability.Allows(purpose, access));
    }
  }
}

TEST(RecordCapabilityTest, WorkingStateIsNotAStorageOrBackgroundCapability) {
  for (auto mode : {StorageMode::kStateless, StorageMode::kMemory,
                    StorageMode::kEncrypted}) {
    RecordCapabilityIssuer owner(base::UnguessableToken::Create(),
                                 base::UnguessableToken::Create(),
                                 base::UnguessableToken::Create(), mode);
    for (auto purpose : {StorePurpose::kDocumentWorkingState,
                         StorePurpose::kNetworkWorkingState}) {
      const auto capability = owner.Issue(purpose);
      EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kReadMemory));
      EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kWriteMemory));
      EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadRecordFiles));
      EXPECT_FALSE(
          capability.Allows(purpose, StorageAccess::kWriteRecordFiles));
      EXPECT_FALSE(
          capability.Allows(purpose, StorageAccess::kRunBackgroundJob));
      EXPECT_FALSE(capability.Allows(StorePurpose::kCookies,
                                     StorageAccess::kWriteMemory));
    }
  }
}

TEST(RecordCapabilityTest, MemoryNeverAdmitsDiskOrParentProfile) {
  RecordCapabilityIssuer owner(
      base::UnguessableToken::Create(), base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kMemory);
  for (auto purpose : kStoredPurposes) {
    const auto capability = owner.Issue(purpose);
    EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kReadMemory));
    EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kWriteMemory));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadRecordFiles));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kWriteRecordFiles));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadParentProfile));
  }
}

TEST(RecordCapabilityTest, EncryptedAdmissionRemainsBoundToPurposeAndOwner) {
  const auto profile = base::UnguessableToken::Create();
  const auto record = base::UnguessableToken::Create();
  const auto generation = base::UnguessableToken::Create();
  RecordCapabilityIssuer owner(profile, record, generation,
                               StorageMode::kEncrypted);
  const auto capability = owner.Issue(StorePurpose::kIndexedDb);
  EXPECT_EQ(profile, capability.profile());
  EXPECT_EQ(record, capability.record());
  EXPECT_EQ(generation, capability.generation());
  EXPECT_TRUE(capability.Allows(StorePurpose::kIndexedDb,
                                StorageAccess::kWriteRecordFiles));
  EXPECT_FALSE(capability.Allows(StorePurpose::kCookies,
                                 StorageAccess::kWriteRecordFiles));
  EXPECT_FALSE(capability.Allows(StorePurpose::kIndexedDb,
                                 StorageAccess::kReadParentProfile));
  EXPECT_FALSE(capability.Allows(StorePurpose::kIndexedDb,
                                 StorageAccess::kRunBackgroundJob));
}

TEST(RecordCapabilityTest, RevocationReachesCopiesAndPendingOperations) {
  const auto profile = base::UnguessableToken::Create();
  const auto record = base::UnguessableToken::Create();
  RecordCapabilityIssuer old_owner(profile, record,
                                   base::UnguessableToken::Create(),
                                   StorageMode::kEncrypted);
  const auto original = old_owner.Issue(StorePurpose::kIndexedDb);
  const auto pending = original;
  EXPECT_TRUE(pending.Allows(StorePurpose::kIndexedDb,
                             StorageAccess::kWriteRecordFiles));
  std::thread revoker([&] { old_owner.Revoke(); });
  revoker.join();
  old_owner.Revoke();
  RecordCapabilityIssuer recreated(profile, record,
                                   base::UnguessableToken::Create(),
                                   StorageMode::kEncrypted);
  EXPECT_NE(original.generation(),
            recreated.Issue(StorePurpose::kIndexedDb).generation());
  for (const auto& capability :
       {original, pending, old_owner.Issue(StorePurpose::kIndexedDb)}) {
    EXPECT_FALSE(capability.active());
    EXPECT_FALSE(capability.Allows(StorePurpose::kIndexedDb,
                                   StorageAccess::kWriteRecordFiles));
  }
  EXPECT_TRUE(
      recreated.Issue(StorePurpose::kIndexedDb)
          .Allows(StorePurpose::kIndexedDb, StorageAccess::kWriteRecordFiles));
}

TEST(RecordCapabilityTest, DestroyedOwnerAndMissingCapabilityFailClosed) {
  RecordCapability retained;
  EXPECT_FALSE(retained.active());
  {
    RecordCapabilityIssuer owner(
        base::UnguessableToken::Create(), base::UnguessableToken::Create(),
        base::UnguessableToken::Create(), StorageMode::kMemory);
    retained = owner.Issue(StorePurpose::kCookies);
    EXPECT_TRUE(retained.active());
  }
  EXPECT_FALSE(
      retained.Allows(StorePurpose::kCookies, StorageAccess::kReadMemory));
  EXPECT_FALSE(RecordCapability().Allows(StorePurpose::kDocumentWorkingState,
                                         StorageAccess::kWriteMemory));
  static_assert(!std::is_copy_constructible_v<RecordCapabilityIssuer>);
}

TEST(RecordCapabilityTest, UnknownInputsNeverGrantAdmission) {
  RecordCapabilityIssuer owner(
      base::UnguessableToken::Create(), base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kEncrypted);
  const auto unknown = static_cast<StorePurpose>(999);
  EXPECT_FALSE(
      owner.Issue(unknown).Allows(unknown, StorageAccess::kReadMemory));
  EXPECT_FALSE(
      owner.Issue(StorePurpose::kCookies)
          .Allows(StorePurpose::kCookies, static_cast<StorageAccess>(999)));
}

TEST(RecordDomainTest, SchemeIndependentRegistrableDomainAndHostFallback) {
  for (const auto* url :
       {"https://en.wikipedia.org/a", "http://ru.wikipedia.org/b",
        "https://wikipedia.org:8443/"}) {
    EXPECT_EQ("wikipedia.org", RecordDomainForPrincipal(
                                   GURL(url), url::Origin::Create(GURL(url))));
  }
  for (const auto* host : {"127.0.0.1", "[::1]", "localhost", "printer",
                           "nomad.home", "a.github.io", "b.github.io"}) {
    const GURL url(std::string("https://") + host + "/");
    EXPECT_EQ(url.host(),
              RecordDomainForPrincipal(url, url::Origin::Create(url)));
  }
}

TEST(RecordDomainTest, NonpersistentPrincipalsNeverShareAnEmptyDomainRecord) {
  EXPECT_TRUE(
      RecordDomainForPrincipal(GURL("https://example.test/"), url::Origin())
          .empty());
  for (const auto* url : {"file:///a", "file://localhost/b",
                          "data:text/plain,a", "about:blank", "about:srcdoc"}) {
    EXPECT_TRUE(
        RecordDomainForPrincipal(GURL(url), url::Origin::Create(GURL(url)))
            .empty());
  }
}

TEST(RecordDomainTest, InheritedOriginDoesNotMakeAboutOrDataPersistent) {
  const auto inherited = url::Origin::Create(GURL("https://wikipedia.org/"));
  for (const auto* url :
       {"about:blank", "about:srcdoc", "data:text/plain,a", "file:///a", ""}) {
    EXPECT_TRUE(RecordDomainForPrincipal(GURL(url), inherited).empty());
  }
}

}  // namespace
}  // namespace alloy::site_records
