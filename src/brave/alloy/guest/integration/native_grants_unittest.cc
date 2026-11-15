// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/alloy/guest/integration/native_requests.h"
#include "content/browser/download/alloy_guest_download.h"
#include "content/browser/file_system_access/alloy_guest_file_binding.h"
#include "base/unguessable_token.h"
#include "base/functional/bind.h"
#include "chrome/browser/extensions/chrome_extension_system.h"
#include "chrome/browser/extensions/chrome_extension_system_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/permissions/chrome_permissions_client.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/browser/alloy_embedded_state.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/web_contents_tester.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/render_frame_host_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "net/base/net_errors.h"

namespace alloy::guest::integration {
namespace {
class GuestNativeSettingsTest : public ChromeRenderViewHostTestHarness {};

TEST_F(GuestNativeSettingsTest, MemoryNativeGrantsUseExistingDocumentSettingsOwner) {
  auto* memory = TestingProfile::Builder().BuildOffTheRecord(profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(site_records::ContextMode::Memory()));
  extensions::ChromeExtensionSystemFactory::GetInstance()->SetTestingFactoryAndUse(
      memory, base::BindRepeating([](content::BrowserContext* context) -> std::unique_ptr<KeyedService> {
        return std::make_unique<extensions::ChromeExtensionSystem>(Profile::FromBrowserContext(context));
      }));
  auto first = content::WebContentsTester::CreateTestWebContents(memory, nullptr);
  auto sibling = content::WebContentsTester::CreateTestWebContents(memory, nullptr);
  const GURL url("https://native-grant-owner.test/page");
  for (auto* contents : {first.get(), sibling.get()})
    content::WebContentsTester::For(contents)->NavigateAndCommit(url);
  auto* original = first->GetPrimaryMainFrame();
  auto* peer = sibling->GetPrimaryMainFrame();
  scoped_refptr<HostContentSettingsMap> map =
      HostContentSettingsMapFactory::GetForNativeIntegration(original);
  auto* peer_map = HostContentSettingsMapFactory::GetForNativeIntegration(peer);
  auto* profile_map = HostContentSettingsMapFactory::GetForProfile(memory);
  ASSERT_TRUE(map); ASSERT_TRUE(peer_map); ASSERT_TRUE(profile_map);
  EXPECT_NE(map.get(), peer_map); EXPECT_NE(map.get(), profile_map);
  EXPECT_EQ(map.get(), HostContentSettingsMapFactory::GetForNativeIntegration(original));
  map->SetContentSettingDefaultScope(url, url,
      ContentSettingsType::CLIPBOARD_READ_WRITE, CONTENT_SETTING_ALLOW);
  EXPECT_EQ(map->GetContentSetting(url, url, ContentSettingsType::CLIPBOARD_READ_WRITE), CONTENT_SETTING_ALLOW);
  EXPECT_NE(peer_map->GetContentSetting(url, url, ContentSettingsType::CLIPBOARD_READ_WRITE), CONTENT_SETTING_ALLOW);
  EXPECT_NE(profile_map->GetContentSetting(url, url, ContentSettingsType::CLIPBOARD_READ_WRITE), CONTENT_SETTING_ALLOW);
  // Ordinary in-process memory permissions keep their original profile owner.
  EXPECT_EQ(ChromePermissionsClient::GetInstance()->GetSettingsMapForPermission(
      memory, original, ContentSettingsType::CLIPBOARD_READ_WRITE), profile_map);
  const auto old = original->GetWeakDocumentPtr();
  content::WebContentsTester::For(first.get())->NavigateAndCommit(url);
  EXPECT_FALSE(old.AsRenderFrameHostIfValid());
  auto* replacement = first->GetPrimaryMainFrame();
  auto* fresh = HostContentSettingsMapFactory::GetForNativeIntegration(replacement);
  ASSERT_TRUE(fresh); EXPECT_NE(fresh, map.get());
  EXPECT_NE(fresh->GetContentSetting(url, url, ContentSettingsType::CLIPBOARD_READ_WRITE), CONTENT_SETTING_ALLOW);
  content::RetireAlloyDocumentState(replacement);
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(replacement), nullptr);
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(peer), peer_map);
}

TEST_F(GuestNativeSettingsTest, SubframeNavigationRetiresPendingNativeReadsBeforeCommit) {
  site_records::RequestContextPolicy::AttachMemory(*profile());
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://outer.test/"));
  auto* root = web_contents()->GetPrimaryMainFrame();
  auto* child = content::RenderFrameHostTester::For(root)->AppendChild("child");
  child = content::NavigationSimulator::NavigateAndCommitFromDocument(GURL("https://child.test/"), child);
  auto* grandchild = content::RenderFrameHostTester::For(child)->AppendChild("grandchild");
  grandchild = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://nested.test/"), grandchild);
  const Owner owner{1, 1, 1, 1};
  NativeRequests requests(owner, [](Owner, std::string, std::string) {},
      [](Owner) { return true; }, [] { return 1u; }, [] { return 1u; }, {});
  requests.Bind(web_contents());
  requests.Call(owner, "start", "", [](ProviderResult result, std::string) {
    EXPECT_EQ(result, ProviderResult::kOk);
  });
  auto* bridge = content::AlloyGuestIntegration::Get(root);
  ASSERT_TRUE(bridge);
  auto parent = bridge->CaptureAdmission(root);
  auto original = bridge->CaptureAdmission(child);
  auto nested = bridge->CaptureAdmission(grandchild);
  auto download_source = content::CaptureAlloyGuestDownloadSource(child);
  ASSERT_TRUE(download_source.Run());
  int parent_retired = 0, child_retired = 0;
  auto parent_lifetime = bridge->CaptureLifetime(root, base::BindOnce(
      [](int* count) { ++*count; }, &parent_retired));
  auto child_lifetime = bridge->CaptureLifetime(child, base::BindOnce(
      [](int* count) { ++*count; }, &child_retired));
  ASSERT_TRUE(parent.Run()); ASSERT_TRUE(original.Run()); ASSERT_TRUE(nested.Run());
  int completed = 0;
  bridge->Request(child, content::AlloyGuestIntegration::Operation::kClipboardRead, {},
      base::BindOnce([](int* completed, std::optional<content::AlloyGuestIntegrationData> result) {
        ++*completed; EXPECT_FALSE(result);
      }, &completed));
  ASSERT_EQ(completed, 0);
  auto navigation = content::NavigationSimulator::CreateRendererInitiated(
      GURL("https://child.test/replacement"), child);
  navigation->Start();
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(parent_retired, 0); EXPECT_EQ(child_retired, 1);
  EXPECT_TRUE(parent.Run());
  EXPECT_FALSE(original.Run()); EXPECT_FALSE(nested.Run());
  EXPECT_FALSE(download_source.Run());
  auto during = bridge->CaptureAdmission(child);
  EXPECT_FALSE(during.Run());
  navigation->Fail(net::ERR_ABORTED);
  // Aborting may permit a new operation on the still-current document, but it
  // cannot resurrect a pending reply/capture from before the navigation.
  EXPECT_FALSE(original.Run()); EXPECT_FALSE(nested.Run());
  EXPECT_FALSE(download_source.Run());
  EXPECT_TRUE(bridge->CaptureAdmission(child).Run());
  EXPECT_EQ(completed, 1);
  EXPECT_TRUE(parent_lifetime.Run()); EXPECT_FALSE(child_lifetime.Run());
  requests.Close();
  EXPECT_EQ(parent_retired, 1); EXPECT_EQ(child_retired, 1);
}

TEST_F(GuestNativeSettingsTest, TransferredFileBindingCannotBorrowTheSelectedSourcesWiderLifetime) {
  site_records::RequestContextPolicy::AttachMemory(*profile());
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://outer.test/"));
  auto* root = web_contents()->GetPrimaryMainFrame();
  auto* child = content::RenderFrameHostTester::For(root)->AppendChild("child");
  child = content::NavigationSimulator::NavigateAndCommitFromDocument(GURL("https://child.test/"), child);
  const Owner owner{1, 1, 1, 1};
  NativeRequests requests(owner, [](Owner, std::string, std::string) {},
      [](Owner) { return true; }, [] { return 1u; }, [] { return 1u; }, {});
  requests.Bind(web_contents());
  requests.Call(owner, "start", "", [](ProviderResult, std::string) {});
  content::AlloyGuestFileMetadata metadata;
  metadata.capability = base::UnguessableToken::Create().ToString();
  metadata.identity = base::UnguessableToken::Create().ToString();
  metadata.name = "selected.txt"; metadata.readable = true;
  auto source = content::AlloyGuestFileCapability::Create(root, metadata); ASSERT_TRUE(source);
  auto issuer = content::AlloyGuestFileBinding::Create(child, source); ASSERT_TRUE(issuer);
  auto receiver = content::AlloyGuestFileBinding::Create(root, source, issuer); ASSERT_TRUE(receiver);
  int retired = 0;
  auto subscription = receiver->OnRetired(base::BindOnce([](int* count) { ++*count; }, &retired));
  auto navigation = content::NavigationSimulator::CreateRendererInitiated(GURL("https://child.test/replaced"), child);
  navigation->Start();
  EXPECT_TRUE(source->Admitted()); EXPECT_FALSE(issuer->Admitted()); EXPECT_FALSE(receiver->Admitted());
  EXPECT_EQ(retired, 1);
  navigation->Fail(net::ERR_ABORTED);
  EXPECT_TRUE(source->Admitted()); EXPECT_FALSE(issuer->Admitted()); EXPECT_FALSE(receiver->Admitted());
  EXPECT_FALSE(content::AlloyGuestFileBinding::Create(root, source, issuer));
  requests.Close(); EXPECT_EQ(retired, 1);
}

TEST_F(GuestNativeSettingsTest, NativeIntegrationPreservesExistingEmbeddedScopeSelection) {
  using Scope = site_records::EmbeddedStateScope;
  ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerOuterSite));
  auto first = content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  auto second = content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  for (auto* tab : {first.get(), second.get()})
    content::WebContentsTester::For(tab)->NavigateAndCommit(GURL("https://outer.test/"));
  auto append = [](content::WebContents* tab, const char* name, const char* url) {
    auto* frame = content::RenderFrameHostTester::For(tab->GetPrimaryMainFrame())->AppendChild(name);
    return content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url), frame);
  };
  auto* original = append(first.get(), "original", "https://shared.test/");
  auto* shared = append(second.get(), "shared", "https://shared.test/");
  auto* unrelated = append(second.get(), "unrelated", "https://unrelated.test/");
  ASSERT_TRUE(HostContentSettingsMapFactory::UsesScopedSettings(original));
  auto* canonical = HostContentSettingsMapFactory::GetForFrame(original);
  ASSERT_TRUE(canonical);
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(original), canonical);
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(shared), canonical);
  EXPECT_NE(HostContentSettingsMapFactory::GetForNativeIntegration(unrelated), canonical);
  EXPECT_NE(HostContentSettingsMapFactory::GetForProfile(profile()), canonical);
  auto owner = content::GetAlloyEmbeddedStateForDocument(original).temporary;
  ASSERT_TRUE(owner);
  owner->Retire();
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(original), nullptr);
  EXPECT_EQ(HostContentSettingsMapFactory::GetForNativeIntegration(shared), nullptr);
  EXPECT_NE(HostContentSettingsMapFactory::GetForNativeIntegration(unrelated), nullptr);
}
}  // namespace
}  // namespace alloy::guest::integration
