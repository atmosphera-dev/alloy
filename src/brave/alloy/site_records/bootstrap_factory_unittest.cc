// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/background_fetch/background_fetch_delegate_factory.h"
#include "chrome/browser/background_sync/background_sync_controller_factory.h"
#include "chrome/browser/content_index/content_index_provider_factory.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/profiles/refcounted_profile_keyed_service_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/content/refcounted_browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/keyed_service/core/refcounted_keyed_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class BootstrapRefService final : public RefcountedKeyedService {
 public:
  BootstrapRefService() = default;
  void ShutdownOnUIThread() override { shutdown = true; }
  bool shutdown = false;
};

// Exercise the native original-only defaults, including cached service lookup.
// Explicit private selectors use item 2's existing graph/client contract; no
// second bootstrap-only service admission graph is involved.
class LegacyBootstrapFactory final : public BrowserContextKeyedServiceFactory {
 public:
  explicit LegacyBootstrapFactory(content::BrowserContext* context)
      : BrowserContextKeyedServiceFactory(
            "AlloyLegacyBootstrapProbe",
            BrowserContextDependencyManager::GetInstance()),
        context_(context) {}
  ~LegacyBootstrapFactory() override {
    if (builds) {
      BrowserContextShutdown(context_);
      BrowserContextDestroyed(context_);
    }
  }
  KeyedService* Get(bool create) {
    return GetServiceForBrowserContext(context_, create);
  }
  mutable int selections = 0;
  mutable int builds = 0;

 private:
  content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override {
    ++selections;
    return BrowserContextKeyedServiceFactory::GetBrowserContextToUse(context);
  }
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext*) const override {
    ++builds;
    return std::make_unique<KeyedService>();
  }
  raw_ptr<content::BrowserContext> context_;
};

class LegacyRefBootstrapFactory final
    : public RefcountedBrowserContextKeyedServiceFactory {
 public:
  explicit LegacyRefBootstrapFactory(content::BrowserContext* context)
      : RefcountedBrowserContextKeyedServiceFactory(
            "AlloyLegacyRefBootstrapProbe",
            BrowserContextDependencyManager::GetInstance()),
        context_(context) {}
  ~LegacyRefBootstrapFactory() override {
    if (builds) {
      BrowserContextShutdown(context_);
      BrowserContextDestroyed(context_);
    }
  }
  scoped_refptr<RefcountedKeyedService> Get(bool create) {
    return GetServiceForBrowserContext(context_, create);
  }
  mutable int selections = 0;
  mutable int builds = 0;

 private:
  content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override {
    ++selections;
    return RefcountedBrowserContextKeyedServiceFactory::GetBrowserContextToUse(
        context);
  }
  scoped_refptr<RefcountedKeyedService> BuildServiceInstanceFor(
      content::BrowserContext*) const override {
    ++builds;
    return base::MakeRefCounted<BootstrapRefService>();
  }
  raw_ptr<content::BrowserContext> context_;
};

class SelectedBootstrapFactory final : public ProfileKeyedServiceFactory {
 public:
  SelectedBootstrapFactory(content::BrowserContext* context,
                           bool admit,
                           bool explicit_mode = true)
      : ProfileKeyedServiceFactory(
            !admit          ? "AlloyDefaultBootstrapProbe"
            : explicit_mode ? "AlloyPrivateBootstrapProbe"
                            : "AlloyExistingPrivateScopeProbe",
            !admit ? ProfileSelections::BuildForRegularProfile()
            : !explicit_mode
                ? ProfileSelections::BuildForRegularAndIncognito()
                : ProfileSelections::Builder()
                      .WithRegular(ProfileSelection::kOwnInstance)
                      .WithAlloyStateless(ProfileSelection::kOwnInstance)
                      .Build()),
        context_(context) {}
  ~SelectedBootstrapFactory() override {
    if (builds) {
      BrowserContextShutdown(context_);
      BrowserContextDestroyed(context_);
    }
  }
  KeyedService* Get(bool create) {
    return GetServiceForBrowserContext(context_, create);
  }
  mutable int builds = 0;

 private:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext*) const override {
    ++builds;
    return std::make_unique<KeyedService>();
  }
  raw_ptr<content::BrowserContext> context_;
};

class SelectedRefBootstrapFactory final
    : public RefcountedProfileKeyedServiceFactory {
 public:
  explicit SelectedRefBootstrapFactory(content::BrowserContext* context)
      : RefcountedProfileKeyedServiceFactory(
            "AlloyPrivateRefBootstrapProbe",
            ProfileSelections::Builder()
                .WithRegular(ProfileSelection::kOwnInstance)
                .WithAlloyStateless(ProfileSelection::kOwnInstance)
                .Build()),
        context_(context) {}
  ~SelectedRefBootstrapFactory() override {
    if (builds) {
      BrowserContextShutdown(context_);
      BrowserContextDestroyed(context_);
    }
  }
  scoped_refptr<RefcountedKeyedService> Get(bool create) {
    return GetServiceForBrowserContext(context_, create);
  }
  mutable int builds = 0;

 private:
  scoped_refptr<RefcountedKeyedService> BuildServiceInstanceFor(
      content::BrowserContext*) const override {
    ++builds;
    return base::MakeRefCounted<BootstrapRefService>();
  }
  raw_ptr<content::BrowserContext> context_;
};

class BootstrapFactoryTest : public ChromeRenderViewHostTestHarness {};

TEST_F(BootstrapFactoryTest, NativeBackgroundOwnersRefuseStatelessOriginal) {
  // Native original identity is retained. The same mode admission must close
  // eager/lazy factory entry points before any offline catalogue,
  // engagement/metrics or background job owner can be consumed.
  RequestContextPolicy::AttachStateless(*profile(), NetworkState::kKeep);
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_FALSE(RequestContextPolicy::Get(*profile())
                   ->StorageCapability(StorePurpose::kServiceWorkers)
                   .Allows(StorePurpose::kServiceWorkers,
                           StorageAccess::kRunBackgroundJob));
  const auto expect_absent = [&] {
    EXPECT_EQ(ContentIndexProviderFactory::GetForProfile(profile()), nullptr);
    EXPECT_EQ(BackgroundFetchDelegateFactory::GetForProfile(profile()),
              nullptr);
    EXPECT_EQ(BackgroundSyncControllerFactory::GetForProfile(profile()),
              nullptr);
  };
  expect_absent();
  for (ProfileKeyedServiceFactory* factory :
       {static_cast<ProfileKeyedServiceFactory*>(
            ContentIndexProviderFactory::GetInstance()),
        static_cast<ProfileKeyedServiceFactory*>(
            BackgroundFetchDelegateFactory::GetInstance()),
        static_cast<ProfileKeyedServiceFactory*>(
            BackgroundSyncControllerFactory::GetInstance())}) {
    factory->CreateServiceNowForTesting(profile());
  }
  expect_absent();
}

TEST_F(BootstrapFactoryTest, LegacyDefaultsCannotReadCachedBootstrapServices) {
  LegacyBootstrapFactory normal(profile());
  LegacyRefBootstrapFactory refcounted(profile());
  ASSERT_NE(normal.Get(true), nullptr);
  auto retained = refcounted.Get(true);
  ASSERT_NE(retained, nullptr);
  ASSERT_EQ(normal.builds, 1);
  ASSERT_EQ(refcounted.builds, 1);
  const int normal_selections = normal.selections;
  const int ref_selections = refcounted.selections;

  // This boundary test deliberately seeds services before attaching policy to
  // an original native test profile. Real bootstrap attaches it earlier; the
  // real process fixture separately proves that construction path.
  RequestContextPolicy::AttachStateless(*profile(), NetworkState::kKeep);
  ASSERT_FALSE(profile()->IsOffTheRecord());
  EXPECT_EQ(normal.Get(false), nullptr);
  EXPECT_EQ(normal.Get(true), nullptr);
  EXPECT_EQ(refcounted.Get(false), nullptr);
  EXPECT_EQ(refcounted.Get(true), nullptr);
  normal.CreateServiceNowForTesting(profile());
  refcounted.CreateServiceNowForTesting(profile());
  EXPECT_GT(normal.selections, normal_selections);
  EXPECT_GT(refcounted.selections, ref_selections);
  EXPECT_EQ(normal.builds, 1);
  EXPECT_EQ(refcounted.builds, 1);
}

TEST_F(BootstrapFactoryTest, ProfileSelectionsPreserveExistingPrivateGraph) {
  SelectedBootstrapFactory ordinary(profile(), false);
  SelectedBootstrapFactory admitted(profile(), true);
  SelectedBootstrapFactory existing_private_scope(profile(), true, false);
  ASSERT_NE(ordinary.Get(true), nullptr);
  RequestContextPolicy::AttachStateless(*profile(), NetworkState::kKeep);
  EXPECT_EQ(ordinary.Get(false), nullptr);
  EXPECT_EQ(ordinary.Get(true), nullptr);
  ordinary.CreateServiceNowForTesting(profile());
  EXPECT_EQ(ordinary.builds, 1);
  auto* private_configuration = admitted.Get(true);
  ASSERT_NE(private_configuration, nullptr);
  EXPECT_EQ(admitted.Get(false), private_configuration);
  admitted.CreateServiceNowForTesting(profile());
  EXPECT_EQ(admitted.builds, 1);
  auto* existing = existing_private_scope.Get(true);
  ASSERT_NE(existing, nullptr);
  EXPECT_NE(existing, private_configuration);
  EXPECT_EQ(existing_private_scope.Get(false), existing);
  existing_private_scope.CreateServiceNowForTesting(profile());
  EXPECT_EQ(existing_private_scope.builds, 1);
}

TEST_F(BootstrapFactoryTest, ExplicitRefcountedAdmissionRetainsNativeShutdown) {
  RequestContextPolicy::AttachStateless(*profile(), NetworkState::kKeep);
  scoped_refptr<BootstrapRefService> retained;
  {
    SelectedRefBootstrapFactory admitted(profile());
    auto service = admitted.Get(true);
    ASSERT_NE(service, nullptr);
    retained = static_cast<BootstrapRefService*>(service.get());
    EXPECT_FALSE(retained->shutdown);
    EXPECT_EQ(admitted.Get(false), service);
    admitted.CreateServiceNowForTesting(profile());
    EXPECT_EQ(admitted.builds, 1);
  }
  EXPECT_TRUE(retained->shutdown);
}

}  // namespace
}  // namespace alloy::site_records
