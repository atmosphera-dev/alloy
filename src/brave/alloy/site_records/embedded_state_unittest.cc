// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/embedded_state.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

using Scope = EmbeddedStateScope;
using Backing = EmbeddedStateRegistry::Backing;

class ListPolicy final : public EmbeddedFirstPartyPolicy {
 public:
  bool IsConsideredFirstParty(const GURL& outer,
                              const GURL& embedded) const override {
    return listed && outer.host() == "outer.test" &&
           embedded.host() == "embedded.test";
  }
  bool listed = false;
};

class EmbeddedStateTest : public testing::Test {
 protected:
  void Open(EmbeddedStateRegistry& registry,
            base::UnguessableToken tab,
            const char* outer = "https://outer.test/") {
    registry.CommitOuterSite(tab, GURL(outer),
                             url::Origin::Create(GURL(outer)));
  }
  EmbeddedStateRegistry::Resolution Resolve(
      EmbeddedStateRegistry& registry,
      base::UnguessableToken tab,
      const char* embedded = "https://embedded.test/") {
    return registry.Resolve(tab, GURL(embedded),
                            url::Origin::Create(GURL(embedded)));
  }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  const base::UnguessableToken profile_ = base::UnguessableToken::Create();
  const base::UnguessableToken tab_a_ = base::UnguessableToken::Create();
  const base::UnguessableToken tab_b_ = base::UnguessableToken::Create();
};

TEST_F(EmbeddedStateTest, FirstPartyRetirementClosesOldAndPendingAdmissionUntilPublication) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerOuterSite, nullptr);
  const GURL alpha("https://sub.alpha.test/a"), beta("https://beta.test/");
  auto resolve = [&](const GURL& url) {
    return registry.ResolveFirstParty(url, url::Origin::Create(url));
  };
  const auto old = resolve(alpha).first_party;
  const auto other = resolve(beta).first_party;
  ASSERT_TRUE(old.active()); ASSERT_TRUE(other.active());
  EXPECT_EQ(old.generation(), resolve(GURL("http://alpha.test:8080/b")).first_party.generation());
  auto publication = registry.BeginFirstPartyRetirement(alpha, url::Origin::Create(alpha));
  EXPECT_FALSE(old.active());
  EXPECT_TRUE(other.active());
  const auto pending = resolve(alpha);
  EXPECT_EQ(Backing::kNone, pending.backing);
  EXPECT_TRUE(pending.temporary_admission_closed);
  EXPECT_FALSE(pending.first_party.active());
  std::move(publication).Run();
  const auto fresh = resolve(alpha).first_party;
  EXPECT_TRUE(fresh.active());
  EXPECT_NE(old.generation(), fresh.generation());
  EXPECT_FALSE(old.active()); EXPECT_FALSE(pending.first_party.active());
  EXPECT_EQ(other.generation(), resolve(beta).first_party.generation());
  registry.Shutdown();
  EXPECT_FALSE(fresh.active()); EXPECT_FALSE(other.active());
}

TEST_F(EmbeddedStateTest, NewContextCannotBypassSharedRecordRetirement) {
  bool retiring = true;
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
      Scope::kPerOuterSite, nullptr, {}, base::BindRepeating(
          [](bool* retiring, std::string_view site) {
            return !*retiring || site != "alpha.test";
          }, &retiring));
  const GURL a("https://alpha.test/"), b("https://beta.test/");
  const auto blocked = registry.ResolveFirstParty(a, url::Origin::Create(a));
  EXPECT_TRUE(blocked.temporary_admission_closed);
  EXPECT_TRUE(registry.ResolveFirstParty(b, url::Origin::Create(b)).first_party.active());
  retiring = false;
  EXPECT_TRUE(registry.ResolveFirstParty(a, url::Origin::Create(a)).first_party.active());
  EXPECT_FALSE(blocked.first_party.active());
}

TEST_F(EmbeddedStateTest, FirstPartyLeaseIsChildOfOriginalProfileGeneration) {
  RecordCapabilityIssuer parent(profile_, base::UnguessableToken::Create(),
      base::UnguessableToken::Create(), StorageMode::kEncrypted);
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
      Scope::kPerOuterSite, nullptr, parent.Issue(StorePurpose::kProfileSettings));
  const GURL site("https://alpha.test/");
  auto old = registry.ResolveFirstParty(site, url::Origin::Create(site)).first_party;
  ASSERT_TRUE(old.active());
  parent.Revoke();
  EXPECT_FALSE(old.active());
  EXPECT_TRUE(registry.ResolveFirstParty(site, url::Origin::Create(site)).temporary_admission_closed);
}

TEST_F(EmbeddedStateTest, PerTabIsolatesSameOuterAndPreservesSameSiteReloads) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, nullptr);
  Open(registry, tab_a_);
  Open(registry, tab_b_);
  auto a = Resolve(registry, tab_a_);
  auto b = Resolve(registry, tab_b_);
  ASSERT_EQ(a.backing, Backing::kTemporary);
  ASSERT_EQ(b.backing, Backing::kTemporary);
  ASSERT_TRUE(a.temporary);
  EXPECT_NE(a.temporary, b.temporary);
  Open(registry, tab_a_, "http://sub.outer.test/another-document");
  EXPECT_EQ(a.temporary, Resolve(registry, tab_a_).temporary);
  registry.CloseTab(tab_b_);
  EXPECT_TRUE(a.temporary->Capability(StorePurpose::kCookies).active());
  EXPECT_FALSE(b.temporary->Capability(StorePurpose::kCookies).active());
}

TEST_F(EmbeddedStateTest, PerTabMoveAndBackRetireEvenRetainedBorrowers) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto retained = Resolve(registry, tab_a_).temporary;
  auto capability = retained->Capability(StorePurpose::kIndexedDb);
  Open(registry, tab_a_, "https://different.test/");
  EXPECT_FALSE(capability.active());
  auto different = Resolve(registry, tab_a_).temporary;
  Open(registry, tab_a_);
  EXPECT_FALSE(different->Capability(StorePurpose::kCookies).active());
  auto fresh = Resolve(registry, tab_a_).temporary;
  EXPECT_NE(capability.generation(),
            fresh->Capability(StorePurpose::kIndexedDb).generation());
  registry.CloseTab(tab_a_);
  EXPECT_FALSE(fresh->Capability(StorePurpose::kIndexedDb).active());
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
}

TEST_F(EmbeddedStateTest, OuterSiteSharesUntilLastTabAndThirtySecondKeepalive) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerOuterSite, nullptr);
  Open(registry, tab_a_);
  Open(registry, tab_b_);
  auto scope = Resolve(registry, tab_a_).temporary;
  EXPECT_EQ(scope, Resolve(registry, tab_b_).temporary);
  auto capability = scope->Capability(StorePurpose::kDomStorage);
  registry.CloseTab(tab_a_);
  tasks_.FastForwardBy(base::Minutes(1));
  EXPECT_TRUE(capability.active());
  registry.CloseTab(tab_b_);
  tasks_.FastForwardBy(base::Seconds(29));
  EXPECT_TRUE(capability.active());
  tasks_.FastForwardBy(base::Seconds(1));
  EXPECT_FALSE(capability.active());
  Open(registry, tab_a_);
  EXPECT_NE(scope, Resolve(registry, tab_a_).temporary);
}

TEST_F(EmbeddedStateTest, ReopeningOuterCancelsKeepaliveBeforeEmbeddedUse) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory,
                                 Scope::kPerOuterSite, nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  auto capability = scope->Capability(StorePurpose::kCookies);
  registry.CloseTab(tab_a_);
  tasks_.FastForwardBy(base::Seconds(29));
  Open(registry, tab_b_);
  tasks_.FastForwardBy(base::Minutes(1));
  EXPECT_TRUE(capability.active());
  EXPECT_EQ(scope, Resolve(registry, tab_b_).temporary);
  registry.CloseTab(tab_b_);
  tasks_.FastForwardBy(base::Seconds(30));
  EXPECT_FALSE(capability.active());
}

TEST_F(EmbeddedStateTest, OuterSitesAndNativeContextsDoNotShareScopes) {
  EmbeddedStateRegistry first(profile_, StorageMode::kMemory,
                              Scope::kPerOuterSite, nullptr);
  EmbeddedStateRegistry second(base::UnguessableToken::Create(),
                               StorageMode::kMemory, Scope::kPerOuterSite,
                               nullptr);
  Open(first, tab_a_);
  Open(first, tab_b_, "https://different.test/");
  Open(second, tab_a_);
  auto a = Resolve(first, tab_a_).temporary;
  EXPECT_NE(a, Resolve(first, tab_b_).temporary);
  EXPECT_NE(a, Resolve(second, tab_a_).temporary);
}

TEST_F(EmbeddedStateTest, ExplicitListIsTheOnlyCrossSiteRealDataDecision) {
  auto policy = std::make_unique<ListPolicy>();
  auto* list = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, std::move(policy));
  Open(registry, tab_a_);
  auto temporary = Resolve(registry, tab_a_).temporary;
  list->listed = true;
  auto listed = Resolve(registry, tab_a_);
  EXPECT_EQ(listed.backing, Backing::kRealSite);
  EXPECT_FALSE(listed.temporary);
  EXPECT_TRUE(listed.considered_first_party);
  Open(registry, tab_b_, "https://different.test/");
  EXPECT_EQ(Resolve(registry, tab_b_).backing, Backing::kTemporary);
  list->listed = false;
  EXPECT_EQ(temporary, Resolve(registry, tab_a_).temporary);
  EXPECT_EQ(Resolve(registry, tab_a_, "https://other.outer.test/").backing,
            Backing::kRealSite);
  Open(registry, tab_b_, "https://wordpress.com/");
  EXPECT_EQ(Resolve(registry, tab_b_, "https://wp.com/").backing,
            Backing::kTemporary);
}

TEST_F(EmbeddedStateTest, StatelessNeverCreatesAJarEvenWithExplicitList) {
  auto policy = std::make_unique<ListPolicy>();
  policy->listed = true;
  EmbeddedStateRegistry registry(profile_, StorageMode::kStateless,
                                 Scope::kPerTab, std::move(policy));
  Open(registry, tab_a_);
  auto embedded = Resolve(registry, tab_a_);
  EXPECT_EQ(embedded.backing, Backing::kNone);
  EXPECT_FALSE(embedded.temporary);
  EXPECT_EQ(Resolve(registry, tab_a_, "https://outer.test/").backing,
            Backing::kNone);
}

TEST_F(EmbeddedStateTest, R16OpaqueAndNonpersistentPrincipalsGetNoBacking) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  for (const auto* url : {"file:///a", "about:blank", "data:text/plain,a"}) {
    EXPECT_EQ(Resolve(registry, tab_a_, url).backing, Backing::kNone);
  }
  EXPECT_EQ(
      registry.Resolve(tab_a_, GURL("https://embedded.test/"), url::Origin())
          .backing,
      Backing::kNone);
  auto old = Resolve(registry, tab_a_).temporary;
  Open(registry, tab_a_, "about:blank");
  EXPECT_FALSE(old->Capability(StorePurpose::kCookies).active());
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
  Open(registry, tab_a_, "https://127.0.0.1/");
  EXPECT_EQ(Resolve(registry, tab_a_, "https://localhost/").backing,
            Backing::kTemporary);
}

TEST_F(EmbeddedStateTest, NoEmbeddedPurposeCanOpenRecordFilesOrParentState) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  for (const auto purpose : std::to_array<StorePurpose>(
           {StorePurpose::kCookies, StorePurpose::kDomStorage,
            StorePurpose::kIndexedDb, StorePurpose::kCacheStorage,
            StorePurpose::kFileSystem, StorePurpose::kServiceWorkers,
            StorePurpose::kHttpCache, StorePurpose::kCodeCache,
            StorePurpose::kTransportSecurity, StorePurpose::kServerProperties,
            StorePurpose::kReportingConfiguration,
            StorePurpose::kSharedDictionaries, StorePurpose::kSiteSettings,
            StorePurpose::kOriginTrials, StorePurpose::kMediaDeviceSalts,
            StorePurpose::kFavicons, StorePurpose::kQuota,
            StorePurpose::kNotifications, StorePurpose::kBlobSpill})) {
    auto capability = scope->Capability(purpose);
    EXPECT_TRUE(capability.Allows(purpose, StorageAccess::kWriteMemory));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kWriteRecordFiles));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadRecordFiles));
    EXPECT_FALSE(capability.Allows(purpose, StorageAccess::kReadParentProfile));
  }
  EXPECT_FALSE(scope->Capability(StorePurpose::kProfileSettings).active());
  EXPECT_FALSE(scope->Capability(StorePurpose::kHistory).active());
  EXPECT_FALSE(scope->Capability(StorePurpose::kVisitedLinks).active());
}

class NativeOwner final : public KeyedService {
 public:
  explicit NativeOwner(base::OnceClosure shutdown)
      : shutdown_(std::move(shutdown)) {}
  void Shutdown() override { std::move(shutdown_).Run(); }

 private:
  base::OnceClosure shutdown_;
};

TEST_F(EmbeddedStateTest, NativeBackendRetiresBeforeLateCallbackOrReentry) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  const auto capability = scope->Capability(StorePurpose::kSiteSettings);
  const char type = 0;
  int shutdown = 0;
  auto factory = [&] {
    return base::BindLambdaForTesting(
        [&](RecordCapability received) -> std::unique_ptr<KeyedService> {
          EXPECT_EQ(received.generation(), capability.generation());
          return std::make_unique<NativeOwner>(base::BindLambdaForTesting([&] {
            ++shutdown;
            EXPECT_FALSE(capability.active());
            EXPECT_FALSE(scope->GetOrCreateService(
                &type, StorePurpose::kSiteSettings,
                base::BindOnce(
                    [](RecordCapability) -> std::unique_ptr<KeyedService> {
                      ADD_FAILURE() << "retired native constructor was invoked";
                      return nullptr;
                    })));
            registry.CloseTab(tab_a_);
          }));
        });
  };
  auto* backend =
      scope->GetOrCreateService(&type, StorePurpose::kSiteSettings, factory());
  ASSERT_TRUE(backend);
  EXPECT_EQ(backend, scope->GetOrCreateService(
                         &type, StorePurpose::kSiteSettings, factory()));
  registry.CloseTab(tab_a_);
  EXPECT_EQ(shutdown, 1);
  scope->Retire();
  EXPECT_EQ(shutdown, 1);
}

TEST_F(EmbeddedStateTest, CallbackRetirementCannotInstallConstructingBackend) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  int shutdown = 0;
  const char type = 0;
  EXPECT_FALSE(scope->GetOrCreateService(
      &type, StorePurpose::kSiteSettings,
      base::BindLambdaForTesting(
          [&](RecordCapability capability) -> std::unique_ptr<KeyedService> {
            registry.CloseTab(tab_a_);
            EXPECT_FALSE(capability.active());
            return std::make_unique<NativeOwner>(
                base::BindLambdaForTesting([&] { ++shutdown; }));
          })));
  EXPECT_EQ(shutdown, 1);
}

TEST_F(EmbeddedStateTest, NativeInstanceSlotsStayDistinctUntilScopeRetirement) {
  auto scope = base::MakeRefCounted<EmbeddedState>(profile_);
  const char type = 0, partition_a = 0, partition_b = 0;
  int constructed = 0, shutdown = 0;
  auto factory = [&] {
    return base::BindLambdaForTesting(
        [&](RecordCapability capability) -> std::unique_ptr<KeyedService> {
          ++constructed;
          EXPECT_TRUE(capability.active());
          return std::make_unique<NativeOwner>(
              base::BindLambdaForTesting([&] { ++shutdown; }));
        });
  };
  auto* a = scope->GetOrCreateService(&type, StorePurpose::kQuota, factory(),
                                      &partition_a);
  auto* b = scope->GetOrCreateService(&type, StorePurpose::kQuota, factory(),
                                      &partition_b);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NE(a, b);
  EXPECT_EQ(a, scope->GetOrCreateService(&type, StorePurpose::kQuota, factory(),
                                         &partition_a));
  EXPECT_EQ(b, scope->GetOrCreateService(&type, StorePurpose::kQuota, factory(),
                                         &partition_b));
  EXPECT_EQ(constructed, 2);
  scope->Retire();
  EXPECT_EQ(shutdown, 2);
}

TEST_F(EmbeddedStateTest,
       NestedInstanceFactoryRetirementCannotPublishEitherSlot) {
  auto scope = base::MakeRefCounted<EmbeddedState>(profile_);
  const char type = 0, partition_a = 0, partition_b = 0;
  int shutdown = 0;
  auto native = [&] {
    return std::make_unique<NativeOwner>(
        base::BindLambdaForTesting([&] { ++shutdown; }));
  };
  EXPECT_FALSE(scope->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindLambdaForTesting(
          [&](RecordCapability capability) -> std::unique_ptr<KeyedService> {
            EXPECT_TRUE(scope->GetOrCreateService(
                &type, StorePurpose::kQuota,
                base::BindLambdaForTesting(
                    [&](RecordCapability) -> std::unique_ptr<KeyedService> {
                      return native();
                    }),
                &partition_b));
            scope->Retire();
            EXPECT_FALSE(capability.active());
            EXPECT_EQ(shutdown, 1);
            return native();
          }),
      &partition_a));
  EXPECT_EQ(shutdown, 2);
  EXPECT_FALSE(scope->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindOnce([](RecordCapability) -> std::unique_ptr<KeyedService> {
        ADD_FAILURE() << "retired native instance was reconstructed";
        return nullptr;
      }),
      &partition_b));
}

TEST_F(EmbeddedStateTest, NestedNavigationCannotRecreateSupersededOuterOwner) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  const char type = 0;
  ASSERT_TRUE(scope->GetOrCreateService(
      &type, StorePurpose::kSiteSettings,
      base::BindLambdaForTesting(
          [&](RecordCapability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<NativeOwner>(base::BindLambdaForTesting(
                [&] { Open(registry, tab_a_, "https://newest.test/"); }));
          })));
  Open(registry, tab_a_, "https://superseded.test/");
  EXPECT_EQ(Resolve(registry, tab_a_, "https://newest.test/").backing,
            Backing::kRealSite);
  EXPECT_EQ(Resolve(registry, tab_a_, "https://superseded.test/").backing,
            Backing::kTemporary);
}

TEST_F(EmbeddedStateTest, ContextShutdownRevokesKeepaliveAndOutstandingLeases) {
  scoped_refptr<EmbeddedState> retained;
  {
    EmbeddedStateRegistry registry(profile_, StorageMode::kMemory,
                                   Scope::kPerOuterSite, nullptr);
    Open(registry, tab_a_);
    retained = Resolve(registry, tab_a_).temporary;
    registry.CloseTab(tab_a_);
    registry.Shutdown();
    EXPECT_FALSE(retained->Capability(StorePurpose::kCookies).active());
    Open(registry, tab_a_);
    EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
  }
  tasks_.FastForwardBy(base::Minutes(1));
  EXPECT_FALSE(retained->Capability(StorePurpose::kCookies).active());
}

TEST_F(EmbeddedStateTest,
       UnrelatedTabCallbackDoesNotCancelCommittedNavigation) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto scope = Resolve(registry, tab_a_).temporary;
  const char type = 0;
  ASSERT_TRUE(scope->GetOrCreateService(
      &type, StorePurpose::kSiteSettings,
      base::BindLambdaForTesting(
          [&](RecordCapability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<NativeOwner>(base::BindLambdaForTesting(
                [&] { Open(registry, tab_b_, "https://peer.test/"); }));
          })));
  Open(registry, tab_a_, "https://destination.test/");
  EXPECT_EQ(Resolve(registry, tab_a_, "https://destination.test/").backing,
            Backing::kRealSite);
  EXPECT_EQ(Resolve(registry, tab_b_, "https://peer.test/").backing,
            Backing::kRealSite);
}

class CallbackPolicy final : public EmbeddedFirstPartyPolicy {
 public:
  bool IsConsideredFirstParty(const GURL&, const GURL&) const override {
    std::move(callback).Run();
    return true;
  }
  mutable base::OnceClosure callback;
};

TEST_F(EmbeddedStateTest, NativeListReaderCannotReturnAuthorityAfterShutdown) {
  auto policy = std::make_unique<CallbackPolicy>();
  auto* reader = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, std::move(policy));
  Open(registry, tab_a_);
  reader->callback = base::BindLambdaForTesting([&] { registry.Shutdown(); });
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
}

TEST_F(EmbeddedStateTest, NativeListReaderKeepsUnrelatedTabOwnershipSeparate) {
  auto policy = std::make_unique<CallbackPolicy>();
  auto* reader = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kEncrypted,
                                 Scope::kPerTab, std::move(policy));
  Open(registry, tab_a_);
  reader->callback = base::BindLambdaForTesting(
      [&] { Open(registry, tab_b_, "https://different.test/"); });
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kRealSite);
}

class RecordingPolicy final : public EmbeddedFirstPartyPolicy {
 public:
  bool IsConsideredFirstParty(const GURL& outer,
                              const GURL& embedded) const override {
    queries.emplace_back(outer.spec(), embedded.spec());
    return false;
  }
  mutable std::vector<std::pair<std::string, std::string>> queries;
};

TEST_F(EmbeddedStateTest, NativeProjectionReceivesTrustedDirectionalUrls) {
  auto policy = std::make_unique<RecordingPolicy>();
  auto* projection = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 std::move(policy));
  Open(registry, tab_a_, "https://sub.outer.test:8443/start");
  const auto initial =
      Resolve(registry, tab_a_, "https://sub.embedded.test/frame");
  EXPECT_EQ(initial.backing, Backing::kTemporary);
  Open(registry, tab_a_, "http://other.outer.test/latest");
  EXPECT_EQ(Resolve(registry, tab_a_, "http://embedded.test/next").backing,
            Backing::kTemporary);
  ASSERT_EQ(projection->queries.size(), 2u);
  EXPECT_EQ(projection->queries[0].first, "https://sub.outer.test:8443/start");
  EXPECT_EQ(projection->queries[0].second, "https://sub.embedded.test/frame");
  EXPECT_EQ(projection->queries[1].first, "http://other.outer.test/latest");
  EXPECT_EQ(projection->queries[1].second, "http://embedded.test/next");
}

class NativeProjection final : public base::RefCounted<NativeProjection> {
 public:
  explicit NativeProjection(int* destroyed) : destroyed_(destroyed) {}
  bool IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const {
    return admitted && outer == GURL("https://outer.test/") &&
           embedded == GURL("https://embedded.test/");
  }
  bool admitted = true;

 private:
  friend class base::RefCounted<NativeProjection>;
  ~NativeProjection() { ++*destroyed_; }
  int* const destroyed_;
};

TEST_F(EmbeddedStateTest, NativeProjectionBindingKeepsOnlyItsExplicitOwner) {
  int destroyed = 0;
  auto projection = base::MakeRefCounted<NativeProjection>(&destroyed);
  auto* native = projection.get();
  {
    EmbeddedStateRegistry registry(
        profile_, StorageMode::kMemory, Scope::kPerTab,
        EmbeddedFirstPartyPolicy::Bind(base::BindRepeating(
            &NativeProjection::IsConsideredFirstParty, projection)));
    projection.reset();
    Open(registry, tab_a_);
    EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kRealSite);
    EXPECT_EQ(destroyed, 0);
    native->admitted = false;
    EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kTemporary);
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);
}

TEST_F(EmbeddedStateTest, EmbeddedSiteGenerationsAreScopedAndR16Grouped) {
  for (auto scope : {Scope::kPerTab, Scope::kPerOuterSite}) {
    EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, scope,
                                   nullptr);
    Open(registry, tab_a_);
    Open(registry, tab_b_);
    auto alpha = Resolve(registry, tab_a_, "https://a.alpha.test/").temporary;
    auto same_site =
        Resolve(registry, tab_a_, "http://b.alpha.test/").temporary;
    auto beta = Resolve(registry, tab_a_, "https://beta.test/").temporary;
    auto peer = Resolve(registry, tab_b_, "https://alpha.test/").temporary;
    ASSERT_TRUE(alpha);
    ASSERT_TRUE(beta);
    EXPECT_EQ(alpha, same_site);
    EXPECT_NE(alpha, beta);
    EXPECT_EQ(alpha == peer, scope == Scope::kPerOuterSite);
    registry.RevokeSite(GURL("file:///not-a-record"),
                        url::Origin::Create(GURL("file:///not-a-record")));
    EXPECT_TRUE(alpha->Capability(StorePurpose::kDomStorage).active());
    EXPECT_TRUE(beta->Capability(StorePurpose::kDomStorage).active());
  }
}

TEST_F(EmbeddedStateTest, RevokeSitePreservesOtherSitesAcrossScopes) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  Open(registry, tab_b_, "https://other-outer.test/");
  auto a = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  auto b = Resolve(registry, tab_b_, "https://sub.alpha.test/").temporary;
  auto sibling_a = Resolve(registry, tab_a_, "https://beta.test/").temporary;
  auto sibling_b = Resolve(registry, tab_b_, "https://beta.test/").temporary;
  registry.RevokeSite(
      GURL("http://different.alpha.test/path"),
      url::Origin::Create(GURL("http://different.alpha.test/path")));
  EXPECT_FALSE(a->Capability(StorePurpose::kDomStorage).active());
  EXPECT_FALSE(b->Capability(StorePurpose::kQuota).active());
  EXPECT_TRUE(sibling_a->Capability(StorePurpose::kIndexedDb).active());
  EXPECT_TRUE(sibling_b->Capability(StorePurpose::kCacheStorage).active());
  EXPECT_EQ(sibling_a,
            Resolve(registry, tab_a_, "https://beta.test/").temporary);
  EXPECT_EQ(sibling_b,
            Resolve(registry, tab_b_, "https://beta.test/").temporary);
  auto fresh = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  ASSERT_TRUE(fresh);
  EXPECT_NE(fresh->Capability(StorePurpose::kDomStorage).generation(),
            a->Capability(StorePurpose::kDomStorage).generation());
}

TEST_F(EmbeddedStateTest, RevokeOuterSiteRetiresItsJarAndAdmissionOnly) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory,
                                 Scope::kPerOuterSite, nullptr);
  Open(registry, tab_a_);
  Open(registry, tab_b_, "https://other-outer.test/");
  auto alpha = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  auto beta = Resolve(registry, tab_a_, "https://beta.test/").temporary;
  auto other = Resolve(registry, tab_b_).temporary;
  registry.RevokeSite(GURL("http://sub.outer.test/"),
                      url::Origin::Create(GURL("http://sub.outer.test/")));
  EXPECT_FALSE(alpha->Capability(StorePurpose::kDomStorage).active());
  EXPECT_FALSE(beta->Capability(StorePurpose::kQuota).active());
  EXPECT_TRUE(other->Capability(StorePurpose::kDomStorage).active());
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
  EXPECT_EQ(other, Resolve(registry, tab_b_).temporary);
  Open(registry, tab_a_);
  auto fresh = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  ASSERT_TRUE(fresh);
  EXPECT_NE(alpha, fresh);
}

TEST_F(EmbeddedStateTest, RevokeSiteCancelsPendingPolicyAdmission) {
  auto policy = std::make_unique<CallbackPolicy>();
  auto* reader = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 std::move(policy));
  Open(registry, tab_a_);
  reader->callback = base::BindLambdaForTesting([&] {
    registry.RevokeSite(GURL("https://embedded.test/"),
                        url::Origin::Create(GURL("https://embedded.test/")));
  });
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kNone);
  reader->callback = base::BindOnce([] {});
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kRealSite);
}

TEST_F(EmbeddedStateTest, RevokeUnrelatedSiteKeepsPendingPolicyAdmission) {
  auto policy = std::make_unique<CallbackPolicy>();
  auto* reader = policy.get();
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 std::move(policy));
  Open(registry, tab_a_);
  reader->callback = base::BindLambdaForTesting([&] {
    registry.RevokeSite(GURL("https://unrelated.test/"),
                        url::Origin::Create(GURL("https://unrelated.test/")));
  });
  EXPECT_EQ(Resolve(registry, tab_a_).backing, Backing::kRealSite);
}

TEST_F(EmbeddedStateTest, AllSiteIssuersRevokeBeforeAnyShutdownCallback) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  Open(registry, tab_b_, "https://other-outer.test/");
  auto alpha = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  auto peer = Resolve(registry, tab_b_, "https://alpha.test/").temporary;
  auto beta = Resolve(registry, tab_a_, "https://beta.test/").temporary;
  const char type = 0;
  int shutdown = 0;
  ASSERT_TRUE(alpha->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindLambdaForTesting([&](RecordCapability)
                                     -> std::unique_ptr<KeyedService> {
        return std::make_unique<NativeOwner>(base::BindLambdaForTesting([&] {
          ++shutdown;
          EXPECT_FALSE(alpha->Capability(StorePurpose::kQuota).active());
          EXPECT_FALSE(peer->Capability(StorePurpose::kQuota).active());
          EXPECT_TRUE(beta->Capability(StorePurpose::kQuota).active());
          EXPECT_EQ(Resolve(registry, tab_a_, "https://alpha.test/").backing,
                    Backing::kNone);
          registry.RevokeSite(GURL("https://alpha.test/"),
                              url::Origin::Create(GURL("https://alpha.test/")));
        }));
      })));
  registry.RevokeSite(GURL("https://alpha.test/"),
                      url::Origin::Create(GURL("https://alpha.test/")));
  EXPECT_EQ(shutdown, 1);
  EXPECT_EQ(beta, Resolve(registry, tab_a_, "https://beta.test/").temporary);
}

TEST_F(EmbeddedStateTest, SiteRevocationDuringFactoryCannotPublish) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto alpha = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  auto beta = Resolve(registry, tab_a_, "https://beta.test/").temporary;
  const char type = 0;
  int shutdown = 0;
  EXPECT_FALSE(alpha->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindLambdaForTesting(
          [&](RecordCapability capability) -> std::unique_ptr<KeyedService> {
            registry.RevokeSite(
                GURL("https://alpha.test/"),
                url::Origin::Create(GURL("https://alpha.test/")));
            EXPECT_FALSE(capability.active());
            return std::make_unique<NativeOwner>(
                base::BindLambdaForTesting([&] { ++shutdown; }));
          })));
  EXPECT_EQ(shutdown, 1);
  EXPECT_TRUE(beta->Capability(StorePurpose::kQuota).active());
}

TEST_F(EmbeddedStateTest, RevokingOuterCancelsPendingCommit) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto state = Resolve(registry, tab_a_).temporary;
  const char type = 0;
  ASSERT_TRUE(state->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindLambdaForTesting(
          [&](RecordCapability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<NativeOwner>(
                base::BindLambdaForTesting([&] {
                  registry.RevokeSite(
                      GURL("https://destination.test/"),
                      url::Origin::Create(GURL("https://destination.test/")));
                }));
          })));
  Open(registry, tab_a_, "https://destination.test/");
  EXPECT_EQ(Resolve(registry, tab_a_, "https://destination.test/").backing,
            Backing::kNone);
}

TEST_F(EmbeddedStateTest, OuterRetirementRevokesAllSitesBeforeCallbacks) {
  EmbeddedStateRegistry registry(profile_, StorageMode::kMemory, Scope::kPerTab,
                                 nullptr);
  Open(registry, tab_a_);
  auto alpha = Resolve(registry, tab_a_, "https://alpha.test/").temporary;
  auto beta = Resolve(registry, tab_a_, "https://beta.test/").temporary;
  const char type = 0;
  ASSERT_TRUE(alpha->GetOrCreateService(
      &type, StorePurpose::kQuota,
      base::BindLambdaForTesting(
          [&](RecordCapability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<NativeOwner>(
                base::BindLambdaForTesting([&] {
                  EXPECT_FALSE(beta->Capability(StorePurpose::kQuota).active());
                }));
          })));
  registry.CloseTab(tab_a_);
  EXPECT_FALSE(alpha->Capability(StorePurpose::kQuota).active());
  EXPECT_FALSE(beta->Capability(StorePurpose::kQuota).active());
}

}  // namespace
}  // namespace alloy::site_records
