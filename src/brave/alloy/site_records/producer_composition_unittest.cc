// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "brave/alloy/site_records/site_profile_configuration.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include <tuple>
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "chrome/browser/apps/app_service/alloy_preferred_app_storage.h"
#include "chrome/browser/apps/app_service/app_service_proxy_factory.h"
#include "chrome/browser/device_api/managed_configuration_api.h"
#include "chrome/browser/device_api/managed_configuration_api_factory.h"
#include "chrome/browser/external_protocol/external_protocol_handler.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/permissions/permission_manager_factory.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/testing_pref_store.h"
#include "components/permissions/permission_manager.h"
#include "components/permissions/permission_context_base.h"
#include "components/permissions/permission_request_data.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/test/web_contents_tester.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/browser/alloy_embedded_state.h"
#include "cef/libcef/browser/alloy/profile_settings.h"
#include "services/network/test/test_url_loader_factory.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
SecretKey CompositionKey() { ObjectId key{}; key.fill(0x36); return SecretKey(key); }
class ExternalFakeWorker final : public shell_integration::DefaultSchemeClientWorker {
 public:
  explicit ExternalFakeWorker(const GURL& url) : DefaultSchemeClientWorker(url) {}
 private:
  ~ExternalFakeWorker() override = default;
  shell_integration::DefaultWebClientState CheckIsDefaultImpl() override {
    return shell_integration::DefaultWebClientState::NOT_DEFAULT;
  }
  std::u16string GetDefaultClientNameImpl() override { return u"Alloy native test transport"; }
  void SetAsDefaultImpl(base::OnceClosure done) override { std::move(done).Run(); }
};
class ExternalTransport final : public ExternalProtocolHandler::Delegate {
 public:
  ExternalTransport() { ExternalProtocolHandler::SetDelegateForTesting(this); }
  ~ExternalTransport() override { ExternalProtocolHandler::SetDelegateForTesting(nullptr); }
  scoped_refptr<shell_integration::DefaultSchemeClientWorker> CreateShellWorker(const GURL& url) override {
    ++workers; return base::MakeRefCounted<ExternalFakeWorker>(url);
  }
  ExternalProtocolHandler::BlockState GetBlockState(const std::string&, Profile*) override { return ExternalProtocolHandler::UNKNOWN; }
  void BlockRequest() override { ++blocked; }
  void RunExternalProtocolDialog(const GURL&, content::WebContents*, ui::PageTransition, bool,
      const std::optional<url::Origin>&, const std::u16string&) override { ++prompts; }
  void LaunchUrlWithoutSecurityCheck(const GURL&, content::WebContents*) override { ++launches; }
  void FinishedProcessingCheck() override { ++finished; }
  int workers=0,blocked=0,prompts=0,launches=0,finished=0;
};
class CompositionHost final : public apps::PreferredAppsImpl::Host {
 public:
  void OnSupportedLinksPreferenceChanged(const std::string&, bool) override {}
  bool QueryConflict(const std::string&, const apps::IntentFilterPtr&,
      const std::string&, const apps::IntentFilterPtr&) override { return true; }
  bool IsWebAppInExtendedScope(const GURL&, const std::string&) const override { return false; }
};
class ProducerCompositionTest : public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    ExternalProtocolHandler::PermitLaunchUrl();
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    encrypted_ = temp_.GetPath().AppendASCII("encrypted");
    external_policy_ = base::MakeRefCounted<TestingPrefStore>();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(encrypted_, CompositionKey(), {}, &files_));
    Open();
  }
  void TearDown() override {
    Close(); files_.reset(); task_environment()->RunUntilIdle();
    ExternalProtocolHandler::PermitLaunchUrl();
    ChromeRenderViewHostTestHarness::TearDown();
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus status) {
      ADD_FAILURE() << "native settings backing failure " << static_cast<int>(status);
    }));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
    auto view = store_->CreateNativeView(); ASSERT_TRUE(view);
    configuration_ = MakeSiteProfileConfiguration(
        files_->ProfileCapability(StorePurpose::kProfileSettings), view, store_, base::OnceClosure());
    ASSERT_TRUE(configuration_->BeginConstruction());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    RegisterUserProfilePrefs(registry.get());
    sync_preferences::PrefServiceSyncableFactory factory;
    factory.set_user_prefs(view);
    factory.set_managed_prefs(external_policy_);
    // Test transport is explicit. Eager native services are disabled only in
    // this fixture so the exact production constructors can be entered after
    // their original configuration, as real ProfileImpl construction does.
    native_ = TestingProfile::Builder()
        .SetPath(temp_.GetPath().AppendASCII("native-"+std::to_string(++round_)))
        .SetPrefService(factory.CreateSyncable(registry))
        .AddTestingFactory(ManagedConfigurationAPIFactory::GetInstance(), base::BindRepeating(
            [](content::BrowserContext*) -> std::unique_ptr<KeyedService> { return nullptr; }))
        .AddTestingFactory(apps::AppServiceProxyFactory::GetInstance(), base::BindRepeating(
            [](content::BrowserContext*) -> std::unique_ptr<KeyedService> { return nullptr; }))
        .Build();
    configuration_->Attach(*native_);
    alloy::BindNativeEmbedListSettings(*native_);
    configuration_->SetTeardownHandler(base::BindOnce([] {}));
    configuration_->ConstructionFinished();
  }
  void Close() {
    preferred_.reset(); managed_.reset(); task_environment()->RunUntilIdle();
    native_.reset(); configuration_.reset(); store_.reset(); task_environment()->RunUntilIdle();
  }
  void Reopen() {
    Close(); ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(encrypted_, CompositionKey(), &files_));
    Open();
  }
  void StartPreferred() {
    preferred_ = apps::CreatePreferredAppsForProfile(&host_, native_.get());
    task_environment()->RunUntilIdle();
    ASSERT_TRUE(preferred_->preferred_apps_list().IsInitialized());
  }
  void Choose(const std::string& id, const std::string& host) {
    apps::IntentFilters filters;
    filters.push_back(apps_util::MakeIntentFilterForUrlScope(GURL("https://"+host+"/")));
    preferred_->SetSupportedLinksPreference(id, std::move(filters));
    task_environment()->RunUntilIdle();
  }
  void StartManaged() {
    managed_ = std::make_unique<ManagedConfigurationAPI>(native_.get(), base::BindRepeating(
        [](network::TestURLLoaderFactory* loader) -> network::mojom::URLLoaderFactory* { return loader; }, &network_));
    task_environment()->RunUntilIdle();
  }
  std::optional<base::DictValue> Get(const url::Origin& origin) {
    base::test::TestFuture<std::optional<base::DictValue>> result;
    managed_->GetOriginPolicyConfiguration(origin, {"key"}, result.GetCallback());
    task_environment()->RunUntilIdle(); return result.Take();
  }
  void Expect(const url::Origin& origin, const std::string& expected) {
    auto result = Get(origin); ASSERT_TRUE(result);
    ASSERT_TRUE(result->FindString("key")); EXPECT_EQ(expected, *result->FindString("key"));
  }
  base::ScopedTempDir temp_;
  base::FilePath encrypted_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  scoped_refptr<TestingPrefStore> external_policy_;
  std::unique_ptr<NativeProfileConfiguration> configuration_;
  std::unique_ptr<TestingProfile> native_;
  CompositionHost host_;
  std::unique_ptr<apps::PreferredAppsImpl> preferred_;
  std::unique_ptr<ManagedConfigurationAPI> managed_;
  network::TestURLLoaderFactory network_;
  int round_ = 0;
  const url::Origin a_ = url::Origin::Create(GURL("https://alpha.test"));
  const url::Origin b_ = url::Origin::Create(GURL("https://beta.test"));
};

TEST_F(ChromeRenderViewHostTestHarness, GuestListUsesItsNativeMemoryProjectionForEmbeddedSelection) {
  RequestContextPolicy::AttachMemory(*profile());
  alloy::BindNativeEmbedListSettings(*profile());
  base::ListValue entries;
  entries.Append("beta.test");
  base::DictValue lists;
  lists.Set("alpha.test", std::move(entries));
  profile()->GetPrefs()->SetDict(embed_list::kPreference, std::move(lists));
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://alpha.test/"));
  auto* child = content::RenderFrameHostTester::For(web_contents()->GetPrimaryMainFrame())->AppendChild("listed");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(GURL("https://beta.test/"), child);
  ASSERT_TRUE(frame);
  const auto selection = content::GetAlloyEmbeddedStateForDocument(frame);
  EXPECT_TRUE(selection.considered_first_party);
  EXPECT_EQ(EmbeddedStateRegistry::Backing::kRealSite, selection.backing);
  EXPECT_FALSE(frame->GetStorageKey().nonce());
}

TEST_F(ProducerCompositionTest, ExternalProtocolNativeProfileRegistrationAndColdAuthority) {
  auto* prefs = native_->GetPrefs();
  ASSERT_NE(nullptr, prefs->FindPreference(prefs::kProtocolHandlerPerOriginAllowedProtocols));
  ASSERT_NE(nullptr, prefs->FindPreference(prefs::kAutoLaunchProtocolsFromOrigins));
  constexpr char scheme[] = "alloy-owned-protocol";
  ExternalProtocolHandler::SetBlockState(scheme, a_, ExternalProtocolHandler::DONT_BLOCK, native_.get());
  ExternalProtocolHandler::SetBlockState(scheme, b_, ExternalProtocolHandler::DONT_BLOCK, native_.get());
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &a_, native_.get()));
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &b_, native_.get()));
  Reopen();
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &a_, native_.get()));
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &b_, native_.get()));
}

TEST_F(ProducerCompositionTest, ColdRecordListSelectsNativeEmbeddedOwnerAndTracksRemoval) {
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_.GetURL(), {b_.GetURL()}));
  Reopen();
  auto outer = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
  content::WebContentsTester::For(outer.get())->NavigateAndCommit(a_.GetURL());
  auto embed = [&] {
    auto* child = content::RenderFrameHostTester::For(outer->GetPrimaryMainFrame())->AppendChild("embedded");
    return content::NavigationSimulator::NavigateAndCommitFromDocument(b_.GetURL(), child);
  };
  auto* listed = embed(); ASSERT_TRUE(listed);
  const auto real = content::GetAlloyEmbeddedStateForDocument(listed);
  EXPECT_TRUE(real.considered_first_party);
  EXPECT_EQ(EmbeddedStateRegistry::Backing::kRealSite, real.backing);
  EXPECT_FALSE(listed->GetStorageKey().nonce());
  auto listed_owner = content::AlloyDocumentStateOwner(listed);
  EXPECT_EQ(b_, content::AlloyConsideredFirstPartyCookieOrigin(
                    listed_owner, native_.get()));
  EXPECT_FALSE(content::AlloyConsideredFirstPartyCookieOrigin(
      listed_owner, profile()));
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_.GetURL(), {}));
  auto* unlisted = embed(); ASSERT_TRUE(unlisted);
  const auto temporary = content::GetAlloyEmbeddedStateForDocument(unlisted);
  EXPECT_FALSE(temporary.considered_first_party);
  EXPECT_EQ(EmbeddedStateRegistry::Backing::kTemporary, temporary.backing);
  ASSERT_TRUE(temporary.temporary);
  EXPECT_TRUE(unlisted->GetStorageKey().nonce());
  EXPECT_FALSE(content::AlloyConsideredFirstPartyCookieOrigin(
      content::AlloyDocumentStateOwner(unlisted), native_.get()));
  // Removing a list affects new admissions, not a captured live document.
  EXPECT_EQ(b_, content::AlloyConsideredFirstPartyCookieOrigin(
                    listed_owner, native_.get()));
  content::RetireAlloyDocumentState(listed);
  EXPECT_FALSE(content::AlloyConsideredFirstPartyCookieOrigin(
      listed_owner, native_.get()));
}

TEST_F(ProducerCompositionTest, ExternalProtocolOriginDictionaryRetiresOnlyItsRecord) {
  // Codec boundary: use the native dictionary shape without manually repairing
  // the production profile registry exercised by the preceding consumer.
  base::DictValue grants;
  grants.Set(a_.Serialize(), base::DictValue().Set("alloy-owned-protocol", true));
  grants.Set(b_.Serialize(), base::DictValue().Set("alloy-owned-protocol", true));
  store_->SetValue(prefs::kProtocolHandlerPerOriginAllowedProtocols, base::Value(std::move(grants)), 0);
  task_environment()->RunUntilIdle();
  Reopen();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue(prefs::kProtocolHandlerPerOriginAllowedProtocols, &value));
  ASSERT_TRUE(value->is_dict());
  ASSERT_TRUE(value->GetDict().contains(a_.Serialize()));
  ASSERT_TRUE(value->GetDict().contains(b_.Serialize()));
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_.GetURL(), {}, &deletion));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(store_->GetValue(prefs::kProtocolHandlerPerOriginAllowedProtocols, &value));
  EXPECT_FALSE(value->GetDict().contains(a_.Serialize()));
  EXPECT_TRUE(value->GetDict().contains(b_.Serialize()));
  Reopen();
  ASSERT_TRUE(store_->GetValue(prefs::kProtocolHandlerPerOriginAllowedProtocols, &value));
  EXPECT_FALSE(value->GetDict().contains(a_.Serialize()));
  EXPECT_TRUE(value->GetDict().contains(b_.Serialize()));
}

TEST_F(ProducerCompositionTest, ExternalProtocolNativeAuthorityAfterAcknowledgedRecordDeletion) {
  // Actual browser-owned visited-site records, independent of grant storage.
  RecordInfo alpha_record, beta_record;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_.GetURL(), a_, true, &alpha_record));
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(b_.GetURL(), b_, true, &beta_record));
  constexpr char scheme[] = "alloy-owned-protocol";
  ExternalProtocolHandler::SetBlockState(scheme, a_, ExternalProtocolHandler::DONT_BLOCK, native_.get());
  ExternalProtocolHandler::SetBlockState(scheme, b_, ExternalProtocolHandler::DONT_BLOCK, native_.get());
  Reopen();
  ASSERT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &a_, native_.get()));
  ASSERT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &b_, native_.get()));
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_.GetURL(), {}, &deletion));
  task_environment()->RunUntilIdle();
  EXPECT_EQ(ExternalProtocolHandler::UNKNOWN, ExternalProtocolHandler::GetBlockState(scheme, &a_, native_.get()));
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &b_, native_.get()));
  Reopen();
  EXPECT_EQ(ExternalProtocolHandler::UNKNOWN, ExternalProtocolHandler::GetBlockState(scheme, &a_, native_.get()));
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK, ExternalProtocolHandler::GetBlockState(scheme, &b_, native_.get()));
}

TEST_F(ProducerCompositionTest, ExternalProtocolRetiredOriginalDocumentCannotReachLaunchTransport) {
  auto contents=content::WebContentsTester::CreateTestWebContents(native_.get(),nullptr);
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(a_.GetURL());
  auto* frame=contents->GetPrimaryMainFrame();
  auto original=frame->GetWeakDocumentPtr();
  ASSERT_TRUE(content::AlloyDocumentStateAdmitted(frame));
  ExternalTransport transport;
  const GURL target("alloy-owned-protocol:payload");
  ExternalProtocolHandler::LaunchUrlWithoutSecurityCheck(target,contents.get(),original);
  ASSERT_EQ(1,transport.launches);
  content::RetireAlloyDocumentState(frame);
  ASSERT_EQ(frame,contents->GetPrimaryMainFrame());
  ASSERT_FALSE(content::AlloyDocumentStateAdmitted(frame));
  ExternalProtocolHandler::LaunchUrlWithoutSecurityCheck(target,contents.get(),original);
  EXPECT_EQ(1,transport.launches);
}

TEST_F(ProducerCompositionTest, ExternalProtocolNativeSchemeReplyKeepsOriginalDocument) {
  auto contents=content::WebContentsTester::CreateTestWebContents(native_.get(),nullptr);
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(a_.GetURL());
  web_modal::WebContentsModalDialogManager::CreateForWebContents(contents.get());
  auto* frame=contents->GetPrimaryMainFrame();auto original=frame->GetWeakDocumentPtr();
  ASSERT_TRUE(content::AlloyDocumentStateAdmitted(frame));
  ExternalTransport transport;
  auto getter=base::BindRepeating([](base::WeakPtr<content::WebContents> value) { return value.get(); },contents->GetWeakPtr());
  const GURL target("alloy-owned-protocol:payload");
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(target,getter,ui::PAGE_TRANSITION_LINK,true,false,a_,original);
  ASSERT_EQ(1,transport.workers);
  task_environment()->RunUntilIdle();
  ASSERT_EQ(1,transport.prompts);ASSERT_EQ(0,transport.launches);
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(target,getter,ui::PAGE_TRANSITION_LINK,true,false,a_,original);
  ASSERT_EQ(2,transport.workers);ASSERT_EQ(1,transport.prompts);
  content::RetireAlloyDocumentState(frame);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(1,transport.prompts);EXPECT_EQ(0,transport.launches);
}

TEST_F(ChromeRenderViewHostTestHarness, LegacyDocumentlessExternalRedirectReachesNativePrompt) {
  ASSERT_FALSE(RequestContextPolicy::Get(*profile()));
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://redirector.test/"));
  web_modal::WebContentsModalDialogManager::CreateForWebContents(web_contents());
  ExternalTransport transport;
  auto getter = base::BindRepeating([](base::WeakPtr<content::WebContents> value) {
    return value.get();
  }, web_contents()->GetWeakPtr());
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(GURL("alloy-owned-protocol:payload"), getter,
      ui::PAGE_TRANSITION_TYPED, true, false, url::Origin::Create(GURL("https://redirector.test/")), {});
  task_environment()->RunUntilIdle();
  EXPECT_EQ(1, transport.workers);
  EXPECT_EQ(1, transport.prompts);
  EXPECT_EQ(0, transport.blocked);
  EXPECT_EQ(0, transport.launches);
  ExternalProtocolHandler::PermitLaunchUrl();
}

TEST_F(ProducerCompositionTest, RedirectAttributionDoesNotReplaceOriginalExternalDocumentAuthority) {
  auto contents = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(a_.GetURL());
  web_modal::WebContentsModalDialogManager::CreateForWebContents(contents.get());
  auto* frame = contents->GetPrimaryMainFrame();
  const auto original = frame->GetWeakDocumentPtr();
  auto getter = base::BindRepeating([](base::WeakPtr<content::WebContents> value) {
    return value.get();
  }, contents->GetWeakPtr());
  ExternalTransport transport;
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(GURL("alloy-owned-protocol:payload"), getter,
      ui::PAGE_TRANSITION_LINK, true, false, b_, original);
  task_environment()->RunUntilIdle();
  ASSERT_EQ(1, transport.prompts);
  EXPECT_EQ(0, transport.blocked);
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(GURL("alloy-owned-protocol:payload"), getter,
      ui::PAGE_TRANSITION_LINK, true, false, b_, {});
  task_environment()->RunUntilIdle();
  EXPECT_EQ(1, transport.prompts);
  EXPECT_EQ(1, transport.blocked);  // Managed attribution requires an original document.
  content::RetireAlloyDocumentState(frame);
  ExternalProtocolHandler::PermitLaunchUrl();
  ExternalProtocolHandler::LaunchUrl(GURL("alloy-owned-protocol:payload"), getter,
      ui::PAGE_TRANSITION_LINK, true, false, b_, original);
  task_environment()->RunUntilIdle();
  EXPECT_EQ(1, transport.prompts);
  EXPECT_EQ(2, transport.blocked);
  EXPECT_EQ(0, transport.launches);
}

TEST_F(ChromeRenderViewHostTestHarness, LegacyInheritedSubframesKeepNativePermissionSettings) {
  ASSERT_FALSE(RequestContextPolicy::Get(*profile()));
  const GURL outer("https://inherited.test/");
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(outer);
  auto* settings = HostContentSettingsMapFactory::GetForProfile(profile());
  settings->SetContentSettingDefaultScope(outer, outer, ContentSettingsType::NOTIFICATIONS, CONTENT_SETTING_ALLOW);
  auto* manager = PermissionManagerFactory::GetForProfile(profile());
  ASSERT_TRUE(manager);
  auto* context = manager->GetPermissionContextForTesting(ContentSettingsType::NOTIFICATIONS);
  ASSERT_TRUE(context);
  for (const auto* url : {"about:blank", "about:srcdoc"}) {
    auto* parent = web_contents()->GetPrimaryMainFrame();
    auto* child = content::RenderFrameHostTester::For(parent)->AppendChild(url);
    auto navigation = content::NavigationSimulator::CreateRendererInitiated(GURL(url), child);
    navigation->SetInitiatorFrame(parent);
    navigation->Commit();
    auto* frame = navigation->GetFinalRenderFrameHost();
    ASSERT_TRUE(frame);
    ASSERT_EQ(parent->GetLastCommittedOrigin(), frame->GetLastCommittedOrigin());
    ASSERT_EQ(GURL(url), frame->GetLastCommittedURL());
    ASSERT_TRUE(RecordDomainForPrincipal(frame->GetLastCommittedURL(), frame->GetLastCommittedOrigin()).empty());
    ASSERT_EQ(settings, HostContentSettingsMapFactory::GetForFrame(frame));
    auto descriptor = content::PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(
        blink::PermissionType::NOTIFICATIONS);
    permissions::PermissionRequestData request(descriptor, permissions::PermissionRequestID(
        frame, permissions::PermissionRequestID::RequestLocalId()), true, outer, outer);
    EXPECT_EQ(blink::mojom::PermissionStatus::GRANTED, context->GetPermissionStatus(request, frame).status);
    content::RetireAlloyDocumentState(frame);
    EXPECT_EQ(blink::mojom::PermissionStatus::DENIED, context->GetPermissionStatus(request, frame).status);
  }
}

TEST_F(ProducerCompositionTest, ExternalProtocolRememberHonorsManagedPrincipalAndStatelessMode) {
  constexpr char scheme[]="alloy-owned-protocol";
  const auto file=url::Origin::Create(GURL("file:///owned-fixture.html"));
  ExternalProtocolHandler::SetBlockState(scheme,file,ExternalProtocolHandler::DONT_BLOCK,native_.get());
  EXPECT_EQ(ExternalProtocolHandler::UNKNOWN,ExternalProtocolHandler::GetBlockState(scheme,&file,native_.get()));
  ExternalProtocolHandler::SetBlockState(scheme,a_,ExternalProtocolHandler::DONT_BLOCK,native_.get());
  EXPECT_EQ(ExternalProtocolHandler::DONT_BLOCK,ExternalProtocolHandler::GetBlockState(scheme,&a_,native_.get()));
  auto stateless=TestingProfile::Builder().Build();
  RequestContextPolicy::AttachStateless(*stateless,NetworkState::kKeep);
  ExternalProtocolHandler::SetBlockState(scheme,a_,ExternalProtocolHandler::DONT_BLOCK,stateless.get());
  EXPECT_EQ(ExternalProtocolHandler::UNKNOWN,ExternalProtocolHandler::GetBlockState(scheme,&a_,stateless.get()));
}

TEST_F(ProducerCompositionTest, ProductionPreferredSelectionUsesEncryptedOwnerAcrossNativePaths) {
  StartPreferred(); Choose("alpha-app", "alpha.test"); Choose("beta-app", "beta.test");
  apps::PreferredApps persisted;
  ASSERT_EQ(FileStatus::kOk, store_->GetPreferredApps(&persisted));
  EXPECT_EQ(2u, persisted.size());
  EXPECT_FALSE(base::PathExists(native_->GetPath().AppendASCII("PreferredApps")));
  Reopen(); StartPreferred();
  EXPECT_EQ("alpha-app", preferred_->preferred_apps_list().FindPreferredAppForUrl(a_.GetURL()));
  EXPECT_EQ("beta-app", preferred_->preferred_apps_list().FindPreferredAppForUrl(b_.GetURL()));
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_.GetURL(), {}, &deletion));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(preferred_->preferred_apps_list().FindPreferredAppForUrl(a_.GetURL()));
  EXPECT_EQ("beta-app", preferred_->preferred_apps_list().FindPreferredAppForUrl(b_.GetURL()));
}

TEST_F(ProducerCompositionTest, ProductionManagedSelectionColdReadsWithoutLegacyDirectoryOrRefetch) {
  StartManaged();
  base::ListValue policies;
  for (const auto& [origin, url, hash] : {std::tuple(a_, "https://config.test/a", "hash-a"),
                                        std::tuple(b_, "https://config.test/b", "hash-b")}) {
    base::DictValue row; row.Set("origin", origin.Serialize());
    row.Set("managed_configuration_url", url); row.Set("managed_configuration_hash", hash);
    policies.Append(std::move(row));
  }
  native_->GetPrefs()->SetList(prefs::kManagedConfigurationPerOrigin, std::move(policies));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/a", R"({"key":"alpha-data"})"));
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/b", R"({"key":"beta-data"})"));
  task_environment()->RunUntilIdle(); Expect(a_, "\"alpha-data\""); Expect(b_, "\"beta-data\"");
  EXPECT_FALSE(base::PathExists(native_->GetPath().AppendASCII("Managed Configuration")));
  Reopen(); StartManaged();
  EXPECT_FALSE(network_.IsPending("https://config.test/a")); EXPECT_FALSE(network_.IsPending("https://config.test/b"));
  Expect(a_, "\"alpha-data\""); Expect(b_, "\"beta-data\"");
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_.GetURL(), {}, &deletion));
  task_environment()->RunUntilIdle(); EXPECT_FALSE(Get(a_)); Expect(b_, "\"beta-data\"");
}

TEST_F(ProducerCompositionTest, ExternalPolicyCannotKeepAnOldBindingOrDownloaderAfterDeletion) {
  auto policy = [&] (const std::string& alpha_url, const std::string& alpha_hash) {
    base::ListValue list;
    for (const auto& [origin, url, hash] : {std::tuple(a_, alpha_url, alpha_hash),
        std::tuple(b_, std::string("https://config.test/b"), std::string("hash-b"))}) {
      base::DictValue row; row.Set("origin", origin.Serialize());
      row.Set("managed_configuration_url", url); row.Set("managed_configuration_hash", hash);
      list.Append(std::move(row));
    }
    external_policy_->SetValue(prefs::kManagedConfigurationPerOrigin, base::Value(std::move(list)), 0);
    task_environment()->RunUntilIdle();
  };
  policy("https://config.test/a", "hash-a"); StartManaged();
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/a", R"({"key":"alpha-data"})"));
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/b", R"({"key":"beta-data"})"));
  task_environment()->RunUntilIdle(); Expect(a_, "\"alpha-data\""); Expect(b_, "\"beta-data\"");
  policy("https://config.test/late", "new-hash");
  ASSERT_TRUE(network_.IsPending("https://config.test/late"));
  const auto original_policy = external_policy_->GetValues();
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_.GetURL(), {}, &deletion));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(network_.IsPending("https://config.test/late"));
  EXPECT_FALSE(Get(a_)); Expect(b_, "\"beta-data\"");
  EXPECT_EQ(original_policy, external_policy_->GetValues());
}
}  // namespace
}  // namespace alloy::site_records
