// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/check_op.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/browser/url_sanitizer/url_sanitizer_service_factory.h"
#include "brave/components/request_otr/browser/request_otr_navigation_throttle.h"
#include "brave/components/request_otr/browser/request_otr_service.h"
#include "brave/components/request_otr/common/features.h"
#include "brave/components/request_otr/common/pref_names.h"
#include "brave/components/url_sanitizer/core/browser/url_sanitizer_service.h"
#include "chrome/browser/accessibility/accessibility_labels_service_factory.h"
#include "chrome/browser/chrome_browser_interface_binders.h"
#include "chrome/browser/extensions/api/web_authentication_proxy/web_authentication_proxy_service.h"
#include "chrome/browser/feature_engagement/tracker_factory.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager_desktop.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager_factory.h"
#include "chrome/browser/ui/webui/family_link_user_internals/family_link_user_internals_ui.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/feature_engagement/public/tracker.h"
#include "components/keyed_service/core/keyed_service_base_factory.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "content/public/browser/browser_accessibility_state.h"
#include "content/public/browser/scoped_accessibility_mode.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/mock_navigation_handle.h"
#include "content/public/test/mock_navigation_throttle_registry.h"
#include "content/public/test/web_contents_tester.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/features.h"
#include "services/image_annotation/public/mojom/image_annotation.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/ax_mode.h"

namespace alloy::site_records {
namespace {

BASE_FEATURE(kAlloyUncapturedActivity, base::FEATURE_ENABLED_BY_DEFAULT);

TEST_F(ChromeRenderViewHostTestHarness,
       RequiredConfigurationIsPrivateAndDoesNotLearnAccountOrActivity) {
  auto* parent_cleaner =
      brave::URLSanitizerServiceFactory::GetForBrowserContext(profile());
  ASSERT_NE(parent_cleaner, nullptr);
  profile()->GetPrefs()->SetBoolean(prefs::kAdvancedProtectionAllowed, true);
  auto* parent_status =
      safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
          profile());
  ASSERT_NE(parent_status, nullptr);
  parent_status->SetAdvancedProtectionStatusForTesting(true);
  ASSERT_TRUE(parent_status->IsUnderAdvancedProtection());
  // Drain the normal parent's queued startup owners before observing effects
  // of creating the private context, as in the production-OTR fence fixture.
  base::RunLoop().RunUntilIdle();
  const void* parent = static_cast<content::BrowserContext*>(profile());
  KeyedServiceBaseFactory::ScopedContextAccessObserverForTesting fence(
      base::BindRepeating(
          [](const void* parent, const char* service, const void* requested,
             const void* selected) {
            CHECK_NE(requested, parent) << service;
            CHECK_NE(selected, parent) << service;
          },
          parent));
  for (auto mode : {NetworkState::kKeep, NetworkState::kCut}) {
    auto* child = TestingProfile::Builder().BuildOffTheRecord(
        profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(mode));
    const auto before =
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS);
    auto* cleaner =
        brave::URLSanitizerServiceFactory::GetForBrowserContext(child);
    ASSERT_NE(cleaner, nullptr);
    EXPECT_NE(cleaner, parent_cleaner);
    const GURL url("https://native-owner.test/path?keep=current-document");
    EXPECT_EQ(cleaner->SanitizeURL(url), url);
    EXPECT_FALSE(cleaner->CheckJsPermission(url));
    auto* tracker =
        feature_engagement::TrackerFactory::GetForBrowserContext(child);
    ASSERT_NE(tracker, nullptr);
    tracker->NotifyEvent("must-not-be-captured");
    tracker->NotifyUsedEvent(kAlloyUncapturedActivity);
    EXPECT_TRUE(tracker->ListEvents(kAlloyUncapturedActivity).empty());
    EXPECT_FALSE(
        tracker->ShouldTriggerHelpUIForTesting(kAlloyUncapturedActivity));
    base::test::TestFuture<bool> initialized;
    tracker->AddOnInitializedCallback(initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    EXPECT_FALSE(initialized.Get());
    auto* status =
        safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
            child);
    ASSERT_NE(status, nullptr);
    EXPECT_NE(status, parent_status);
    EXPECT_FALSE(status->IsUnderAdvancedProtection());
    status->SetAdvancedProtectionStatusForTesting(true);
    EXPECT_FALSE(status->IsUnderAdvancedProtection());
    EXPECT_FALSE(
        static_cast<safe_browsing::AdvancedProtectionStatusManagerDesktop*>(
            status)
            ->IsRefreshScheduled());
    EXPECT_EQ(
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS),
        before);
  }
}

TEST_F(ChromeRenderViewHostTestHarness,
       MissingNativeOwnersRejectInterfacesAndProfileDiagnostics) {
  auto* child = TestingProfile::Builder().BuildOffTheRecord(
      profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(
                     NetworkState::kKeep));
  EXPECT_EQ(
      extensions::WebAuthenticationProxyService::GetIfProxyAttached(child),
      nullptr);
  FamilyLinkUserInternalsUIConfig diagnostic;
  EXPECT_TRUE(diagnostic.IsWebUIEnabled(profile()));
  EXPECT_FALSE(diagnostic.IsWebUIEnabled(child));
  auto contents =
      content::WebContentsTester::CreateTestWebContents(child, nullptr);
  content::WebContentsTester::For(contents.get())
      ->NavigateAndCommit(GURL("https://native-owner.test/page"));
  const ui::AXMode label_mode(ui::AXMode::kWebContents |
                              ui::AXMode::kExtendedProperties |
                              ui::AXMode::kLabelImages);
  auto labels =
      content::BrowserAccessibilityState::GetInstance()
          ->CreateScopedModeForWebContents(contents.get(), label_mode);
  ASSERT_TRUE(
      contents->GetAccessibilityMode().has_mode(ui::AXMode::kLabelImages));
  mojo::BinderMapWithContext<content::RenderFrameHost*> binders;
  auto* frame = contents->GetPrimaryMainFrame();
  chrome::internal::PopulateChromeFrameBinders(&binders, frame);
  mojo::Remote<image_annotation::mojom::Annotator> remote;
  mojo::GenericPendingReceiver receiver(remote.BindNewPipeAndPassReceiver());
  base::test::TestFuture<void> disconnected;
  remote.set_disconnect_handler(disconnected.GetCallback());
  ASSERT_TRUE(binders.TryBind(frame, &receiver));
  ASSERT_TRUE(disconnected.Wait());
  EXPECT_EQ(AccessibilityLabelsServiceFactory::GetForProfileIfExists(child),
            nullptr);
}

TEST_F(ChromeRenderViewHostTestHarness,
       MissingNavigationOwnersCannotInstallThrottle) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures({request_otr::features::kBraveRequestOTRTab,
                             net::features::kBraveFirstPartyEphemeralStorage},
                            {});
  profile()->GetPrefs()->SetInteger(
      request_otr::kRequestOTRActionOption,
      static_cast<int>(
          request_otr::RequestOTRService::RequestOTRActionOption::kAsk));
  content::MockNavigationHandle handle(
      GURL("https://native-owner.test/request"),
      web_contents()->GetPrimaryMainFrame());
  content::MockNavigationThrottleRegistry registry(
      &handle,
      content::MockNavigationThrottleRegistry::RegistrationMode::kHold);
  request_otr::RequestOTRNavigationThrottle::MaybeCreateAndAdd(
      registry, nullptr, nullptr, profile()->GetPrefs(), "en-US");
  EXPECT_TRUE(registry.throttles().empty());
}

}  // namespace
}  // namespace alloy::site_records
