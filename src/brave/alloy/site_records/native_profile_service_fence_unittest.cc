// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <memory>
#include <string>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "cef/libcef/browser/alloy/browser_platform_delegate_alloy.h"
#include "chrome/browser/autofill/autocomplete_history_manager_factory.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/favicon/favicon_utils.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/language/language_model_manager_factory.h"
#include "chrome/browser/media/webrtc/media_device_salt_service_factory.h"
#include "chrome/browser/metrics/profile_metrics_service_factory.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/notifications/scheduler/notification_schedule_service_factory.h"
#include "chrome/browser/offline_items_collection/offline_content_aggregator_factory.h"
#include "chrome/browser/optimization_guide/optimization_guide_web_contents_observer.h"
#include "chrome/browser/performance_manager/persistence/site_data/site_data_cache_facade_factory.h"
#include "chrome/browser/plugins/plugin_prefs.h"
#include "chrome/browser/plugins/plugin_prefs_factory.h"
#include "chrome/browser/profiles/off_the_record_profile_impl.h"
#include "chrome/browser/profiles/profile_impl.h"
#include "chrome/browser/profiles/profile_key.h"
#include "chrome/browser/supervised_user/family_link_settings_service_factory.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/translate/translate_ranker_factory.h"
#include "chrome/browser/ui/autofill/autofill_client_provider.h"
#include "chrome/browser/ui/autofill/autofill_client_provider_factory.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_browser_process.h"
#include "components/autofill/content/browser/content_autofill_client.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager_test_api.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager_test_api.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/single_field_fillers/autocomplete/autocomplete_history_manager.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/favicon/core/favicon_service.h"
#include "components/history/core/browser/history_service.h"
#include "components/keyed_service/core/keyed_service_base_factory.h"
#include "components/language/core/browser/language_model_manager.h"
#include "components/media_device_salt/media_device_salt_service.h"
#include "components/prefs/pref_service.h"
#include "components/supervised_user/core/browser/family_link_settings_service.h"
#include "components/supervised_user/core/common/pref_names.h"
#include "components/supervised_user/core/common/supervised_user_constants.h"
#include "components/translate/core/browser/ranker_model.h"
#include "components/translate/core/browser/translate_ranker_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/origin_trials_controller_delegate.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/web_contents_tester.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_builder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/origin_trials/scoped_test_origin_trial_policy.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/metrics_proto/translate_event.pb.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

// TestingProfile normally hardcodes the new-tab URL. Use the shared native
// preference/command-line implementation so a parent-homepage read is
// observable.
class NativeConfigurationParentProfile : public TestingProfile {
 public:
  GURL GetHomePage() override {
    return ProfileImpl::GetHomePageForPrefs(GetPrefs());
  }
};

// Exercise CEF's public creation notification and its complete native helper
// attachment. Only expose construction; no helper behavior is overridden.
class NativeAlloyTabHelperDelegate final
    : public CefBrowserPlatformDelegateAlloy {
 public:
  NativeAlloyTabHelperDelegate() = default;
  ~NativeAlloyTabHelperDelegate() override = default;
};

class StatelessNativeProfileCreationTest
    : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    previous_system_network_context_ =
        g_browser_process->system_network_context_manager();
    if (!SystemNetworkContextManager::HasInstance()) {
      ASSERT_NE(g_browser_process->local_state(), nullptr);
      SystemNetworkContextManager::CreateInstance(
          g_browser_process->local_state());
      owns_system_network_context_ = true;
    }
    TestingBrowserProcess::GetGlobal()->SetSystemNetworkContextManager(
        SystemNetworkContextManager::GetInstance());
    // Finish this fixture's original-profile startup, including its posted
    // extension-management tasks, before tests create private owners and install
    // their parent-access fence. Guest work is never drained outside that fence.
    task_environment()->RunUntilIdle();
  }

  void TearDown() override {
    ChromeRenderViewHostTestHarness::TearDown();
    TestingBrowserProcess::GetGlobal()->SetSystemNetworkContextManager(
        previous_system_network_context_.get());
    if (owns_system_network_context_) {
      SystemNetworkContextManager::DeleteInstance();
    }
  }

 protected:
  std::unique_ptr<TestingProfile> CreateTestingProfile() override {
    return std::make_unique<NativeConfigurationParentProfile>();
  }

 private:
  bool owns_system_network_context_ = false;
  raw_ptr<SystemNetworkContextManager> previous_system_network_context_ =
      nullptr;
};

TEST_F(StatelessNativeProfileCreationTest,
       CreationNavigationAndShutdownNeverAccessParentServices) {
  using Factory = KeyedServiceBaseFactory;
  const void* parent_profile = static_cast<content::BrowserContext*>(profile());
  const void* parent_key =
      static_cast<SimpleFactoryKey*>(profile()->GetProfileKey());
  ASSERT_NE(parent_key, nullptr);
  auto* parent_policy = profile()->GetProfilePolicyConnector();
  auto* parent_schema = profile()->GetPolicySchemaRegistryService();
  auto* parent_storage_policy = profile()->GetExtensionSpecialStoragePolicy();
  auto* parent_language =
      LanguageModelManagerFactory::GetForBrowserContext(profile());
  ASSERT_NE(parent_language, nullptr);
  auto* parent_theme = ThemeServiceFactory::GetForProfile(profile());
  ASSERT_NE(parent_theme, nullptr);
  parent_theme->SetBrowserColorScheme(ThemeService::BrowserColorScheme::kDark);
  const GURL parent_home("https://private-owner.test/parent-home");
  profile()->GetPrefs()->SetBoolean(prefs::kHomePageIsNewTabPage, false);
  profile()->GetPrefs()->SetString(prefs::kHomePage, parent_home.spec());
  profile()->GetPrefs()->SetString(prefs::kSupervisedUserId,
                                   supervised_user::kChildAccountSUID);
  ASSERT_EQ(profile()->GetHomePage(), parent_home);
  ASSERT_TRUE(profile()->IsChild());
  base::RunLoop().RunUntilIdle();

  int control_parent_reads = 0;
  {
    Factory::ScopedContextAccessObserverForTesting observe(base::BindRepeating(
        [](const void* parent, int* reads, const char*, const void* requested,
           const void* selected) {
          if (requested == parent || selected == parent) {
            ++*reads;
          }
        },
        parent_profile, &control_parent_reads));
    ASSERT_NE(ProfileMetricsServiceFactory::GetForProfile(profile()), nullptr);
    ASSERT_NE(ProfileMetricsServiceFactory::GetForProfile(profile()), nullptr);
  }
  // Repeated reads of an existing mapping must also be observed, not only
  // builds.
  EXPECT_GE(control_parent_reads, 2);

  int private_lookups = 0;
  Factory::ScopedContextAccessObserverForTesting fence(base::BindRepeating(
      [](const void* parent, const void* parent_key, int* lookups,
         const char* service, const void* requested, const void* selected) {
        CHECK_NE(requested, parent) << service;
        CHECK_NE(selected, parent) << service;
        CHECK_NE(requested, parent_key) << service;
        CHECK_NE(selected, parent_key) << service;
        ++*lookups;
      },
      parent_profile, parent_key, &private_lookups));

  // Unlike TestingProfile::BuildOffTheRecord, this uses production OTR Init and
  // its full native keyed-service graph, including eager creation and teardown.
  for (auto mode : {NetworkState::kKeep, NetworkState::kCut}) {
    auto child = std::make_unique<OffTheRecordProfileImpl>(
        profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(mode));
    EXPECT_TRUE(child->GetProfileKey()->IsAlloyStateless());
    // Direct Profile getters do not pass through the keyed-service observer.
    // They must expose this context's real native configuration as well.
    ASSERT_NE(child->GetProfilePolicyConnector(), nullptr);
    EXPECT_NE(child->GetProfilePolicyConnector(), parent_policy);
    const Profile& const_child = *child;
    EXPECT_EQ(const_child.GetProfilePolicyConnector(),
              child->GetProfilePolicyConnector());
    ASSERT_NE(child->GetPolicySchemaRegistryService(), nullptr);
    EXPECT_NE(child->GetPolicySchemaRegistryService(), parent_schema);
    EXPECT_EQ(child->GetUserCloudPolicyManager(), nullptr);
    EXPECT_EQ(child->GetProfileCloudPolicyManager(), nullptr);
    EXPECT_EQ(child->GetCloudPolicyManager(), nullptr);
    EXPECT_NE(child->GetHomePage(), parent_home);
    EXPECT_FALSE(child->IsChild());
    child->Init();
    (void)autofill::AutofillClientProviderFactory::GetForProfile(child.get());
    ASSERT_NE(child->GetExtensionSpecialStoragePolicy(), nullptr);
    EXPECT_NE(child->GetExtensionSpecialStoragePolicy(), parent_storage_policy);
    auto contents =
        content::WebContentsTester::CreateTestWebContents(child.get(), nullptr);
    NativeAlloyTabHelperDelegate native_helpers;
    native_helpers.WebContentsCreated(contents.get(), false);
    autofill::AutofillClientProviderFactory::GetForProfile(child.get())
        .CreateClientForWebContents(contents.get());
    ASSERT_NE(autofill::ContentAutofillClient::FromWebContents(contents.get()),
              nullptr);
    auto* language_manager =
        LanguageModelManagerFactory::GetForBrowserContext(child.get());
    ASSERT_NE(language_manager, nullptr);
    EXPECT_NE(language_manager, parent_language);
    EXPECT_EQ(language_manager->GetPrimaryModelType(),
              language::LanguageModelManager::ModelType::FLUENT);
    ASSERT_NE(language_manager->GetPrimaryModel(), nullptr);
    ChromeTranslateClient::CreateForWebContents(contents.get());
    ASSERT_NE(ChromeTranslateClient::FromWebContents(contents.get()), nullptr);
    auto* theme = ThemeServiceFactory::GetForProfile(child.get());
    ASSERT_NE(theme, nullptr);
    EXPECT_NE(theme, parent_theme);
    EXPECT_EQ(theme->GetBrowserColorScheme(),
              ThemeService::BrowserColorScheme::kSystem);
    EXPECT_TRUE(theme->UsingDefaultTheme());
    EXPECT_EQ(theme->GetThemeSyncableService(), nullptr);
    ASSERT_NE(theme->GetColorProvider(), nullptr);
    const auto before_theme =
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS);
    const auto extension_theme =
        extensions::ExtensionBuilder("uncaptured theme")
            .SetManifestKey("theme", base::DictValue())
            .SetPath(profile()->GetPath())
            .Build();
    ASSERT_TRUE(extension_theme->is_theme());
    theme->SetTheme(extension_theme.get());
    EXPECT_FALSE(theme->UsingExtensionTheme());
    EXPECT_EQ(
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS),
        before_theme);
    auto* tester = content::WebContentsTester::For(contents.get());
    tester->NavigateAndCommit(GURL("https://private-owner.test/first"));
    tester->NavigateAndCommit(GURL("https://private-owner.test/replacement"));
    ASSERT_NE(favicon::ContentFaviconDriver::FromWebContents(contents.get()),
              nullptr);
    favicon::SaveFaviconEvenIfInIncognito(contents.get());
    OptimizationGuideWebContentsObserver::CreateForWebContents(contents.get());
    auto* hints =
        OptimizationGuideWebContentsObserver::FromWebContents(contents.get());
    ASSERT_NE(hints, nullptr);
    hints->AddURLsToBatchFetchBasedOnPrediction(
        {GURL("https://denied-hints.test/prediction")}, contents.get());
    EXPECT_FALSE(hints->HasPendingHintsForTesting());
    ASSERT_TRUE(PluginPrefsFactory::GetPrefsForProfile(child.get()));
    const auto private_profiles = child->GetAllOffTheRecordProfiles();
    ASSERT_EQ(private_profiles.size(), 1u);
    EXPECT_EQ(private_profiles.front(), child.get());
    // The source-reviewed FamilyLink service stores account/host approvals;
    // the offline catalogue also retains cross-document data. Neither may be
    // selected, while the already-noop scheduler may have a private instance.
    EXPECT_EQ(supervised_user::FamilyLinkSettingsServiceFactory::GetForKey(
                  child->GetProfileKey()),
              nullptr);
    EXPECT_EQ(
        OfflineContentAggregatorFactory::GetForKey(child->GetProfileKey()),
        nullptr);
    EXPECT_NE(
        NotificationScheduleServiceFactory::GetForKey(child->GetProfileKey()),
        nullptr);
    native_helpers.WebContentsDestroyed(contents.get());
    contents.reset();
    child.reset();
  }
  base::RunLoop().RunUntilIdle();
  EXPECT_GT(private_lookups, 0);
}

TEST_F(StatelessNativeProfileCreationTest,
       TranslationRankerNeverCapturesModelsEventsOrLogging) {
  auto child = std::make_unique<OffTheRecordProfileImpl>(
      profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(
                     NetworkState::kKeep));
  child->Init();
  for (Profile* context : std::vector<Profile*>{profile(), child.get()}) {
    auto* ranker = static_cast<translate::TranslateRankerImpl*>(
        translate::TranslateRankerFactory::GetForBrowserContext(context));
    ASSERT_NE(ranker, nullptr);
    EXPECT_FALSE(ranker->HasCapturedDataForTesting());
    EXPECT_FALSE(ranker->CheckModelLoaderForTesting());
    ranker->EnableLogging(true);
    ranker->OnModelAvailable(std::make_unique<translate::RankerModel>());
    metrics::TranslateEventProto event;
    event.set_source_language("private-input-language");
    const auto before = event.SerializeAsString();
    ranker->RecordTranslateEvent(metrics::TranslateEventProto::UNKNOWN,
                                 ukm::kInvalidSourceId, &event);
    EXPECT_TRUE(ranker->ShouldOfferTranslation(&event, nullptr));
    EXPECT_TRUE(ranker->GetModelDecision(event));
    EXPECT_EQ(ranker->GetModelVersion(), 0u);
    EXPECT_EQ(event.SerializeAsString(), before);
    std::vector<metrics::TranslateEventProto> events;
    ranker->FlushTranslateEvents(&events);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(ranker->HasCapturedDataForTesting());
    EXPECT_FALSE(ranker->CheckModelLoaderForTesting());
  }
  child.reset();
}

TEST_F(StatelessNativeProfileCreationTest,
       MemoryOwnersRetainPrivateSettingsWithoutParentServiceAccess) {
  auto activity_factory_enabled =
      performance_manager::SiteDataCacheFacadeFactory::EnableForTesting();
  EXPECT_EQ(performance_manager::SiteDataCacheFacadeFactory::GetInstance()
                ->GetProfileFacadeForTesting(profile()),
            nullptr);
  const GURL site("https://memory-settings.test/");
  const GURL parent_home("https://persistent-parent.test/home");
  profile()->GetPrefs()->SetBoolean(prefs::kHomePageIsNewTabPage, false);
  profile()->GetPrefs()->SetString(prefs::kHomePage, parent_home.spec());
  auto* parent_map = HostContentSettingsMapFactory::GetForProfile(profile());
  ASSERT_NE(parent_map, nullptr);
  parent_map->SetContentSettingDefaultScope(
      site, site, ContentSettingsType::JAVASCRIPT, CONTENT_SETTING_BLOCK);
  base::RunLoop().RunUntilIdle();

  const void* parent = static_cast<content::BrowserContext*>(profile());
  const void* parent_key =
      static_cast<SimpleFactoryKey*>(profile()->GetProfileKey());
  int private_lookups = 0;
  KeyedServiceBaseFactory::ScopedContextAccessObserverForTesting fence(
      base::BindRepeating(
          [](const void* parent, const void* key, int* lookups,
             const char* name, const void* requested, const void* selected) {
            CHECK_NE(requested, parent) << name;
            CHECK_NE(selected, parent) << name;
            CHECK_NE(requested, key) << name;
            CHECK_NE(selected, key) << name;
            ++*lookups;
          },
          parent, parent_key, &private_lookups));
  auto first = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  auto second = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  ASSERT_TRUE(first->IsOffTheRecord());
  EXPECT_EQ(first->GetOriginalProfile(), profile());
  EXPECT_TRUE(first->GetProfileKey()->IsAlloyNoDisk());
  EXPECT_FALSE(first->GetProfileKey()->IsAlloyStateless());
  ASSERT_EQ(RequestContextPolicy::Get(*first)->storage_mode(),
            StorageMode::kMemory);
  EXPECT_NE(first->GetHomePage(), parent_home);
  first->Init();
  second->Init();
  auto* first_family =
      supervised_user::FamilyLinkSettingsServiceFactory::GetForKey(
          first->GetProfileKey());
  auto* second_family =
      supervised_user::FamilyLinkSettingsServiceFactory::GetForKey(
          second->GetProfileKey());
  ASSERT_NE(first_family, nullptr);
  ASSERT_NE(second_family, nullptr);
  EXPECT_NE(first_family, second_family);
  EXPECT_TRUE(first_family->IsReady());
  EXPECT_TRUE(second_family->IsReady());
  EXPECT_EQ(performance_manager::SiteDataCacheFacadeFactory::GetInstance()
                ->GetProfileFacadeForTesting(first.get()),
            nullptr);
  EXPECT_EQ(performance_manager::SiteDataCacheFacadeFactory::GetInstance()
                ->GetProfileFacadeForTesting(second.get()),
            nullptr);
  auto* first_map = HostContentSettingsMapFactory::GetForProfile(first.get());
  auto* second_map = HostContentSettingsMapFactory::GetForProfile(second.get());
  ASSERT_NE(first_map, nullptr);
  ASSERT_NE(second_map, nullptr);
  EXPECT_NE(first_map, second_map);
  EXPECT_EQ(
      first_map->GetContentSetting(site, site, ContentSettingsType::JAVASCRIPT),
      CONTENT_SETTING_ALLOW);
  first_map->SetContentSettingDefaultScope(
      site, site, ContentSettingsType::JAVASCRIPT, CONTENT_SETTING_BLOCK);
  EXPECT_EQ(
      HostContentSettingsMapFactory::GetForProfile(first.get())
          ->GetContentSetting(site, site, ContentSettingsType::JAVASCRIPT),
      CONTENT_SETTING_BLOCK);
  EXPECT_EQ(second_map->GetContentSetting(site, site,
                                          ContentSettingsType::JAVASCRIPT),
            CONTENT_SETTING_ALLOW);
  auto pending = RequestContextPolicy::Get(*first)->StorageCapability(
      StorePurpose::kDomStorage);
  EXPECT_TRUE(
      pending.Allows(StorePurpose::kDomStorage, StorageAccess::kWriteMemory));
  first.reset();
  EXPECT_FALSE(pending.active());
  second.reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_GT(private_lookups, 0);
  EXPECT_EQ(profile()->GetPrefs()->GetString(prefs::kHomePage),
            parent_home.spec());
}

TEST_F(StatelessNativeProfileCreationTest,
       SavedDataFacadesCompleteWithoutCapturingRecords) {
  auto child = std::make_unique<OffTheRecordProfileImpl>(
      profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(
                     NetworkState::kKeep));
  child->Init();
  for (Profile* context : std::vector<Profile*>{profile(), child.get()}) {
    context->GetPrefs()->SetBoolean(autofill::prefs::kAutofillHasSeenIban,
                                    true);
    context->GetPrefs()->SetBoolean(autofill::prefs::kAutofillHasSeenBnpl,
                                    false);
    const auto before =
        context->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS);
    auto* data =
        autofill::PersonalDataManagerFactory::GetForBrowserContext(context);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->IsStoreless());
    ASSERT_TRUE(data->IsDataLoaded());
    auto& addresses = data->address_data_manager();
    auto& payments = data->payments_data_manager();
    EXPECT_FALSE(payments.IsAutofillHasSeenIbanPrefEnabled());
    EXPECT_FALSE(autofill::test_api(addresses).HasBoundDataServices());
    EXPECT_FALSE(autofill::test_api(payments).HasBoundDataServices());
    autofill::AutofillProfile address(autofill::AddressCountryCode("US"));
    address.SetRawInfo(autofill::NAME_FULL, u"No Capture Person");
    address.SetRawInfo(autofill::ADDRESS_HOME_LINE1, u"123 No Capture Street");
    addresses.AddProfile(address);
    addresses.UpdateProfile(address);
    addresses.RecordUseOf(address);
    addresses.MigrateProfileToAccount(address);
    EXPECT_TRUE(addresses.GetProfiles().empty());
    EXPECT_EQ(addresses.GetProfileByGUID(address.guid()), nullptr);
    EXPECT_FALSE(addresses.IsAwaitingPendingAddressChanges());
    EXPECT_EQ(autofill::test_api(addresses).CapturedRecordCount(), 0u);
    autofill::CreditCard card;
    card.SetRawInfo(autofill::CREDIT_CARD_NUMBER, u"4111111111111111");
    payments.AddCreditCard(card);
    payments.UpdateCreditCard(card);
    payments.AddServerCreditCardForTest(
        std::make_unique<autofill::CreditCard>(card));
    EXPECT_TRUE(payments.GetCreditCards().empty());
    autofill::Iban iban;
    iban.set_value(u"DE89370400440532013000");
    EXPECT_TRUE(payments.AddAsLocalIban(iban).empty());
    EXPECT_TRUE(payments.OnAcceptedLocalIbanSave(iban).empty());
    payments.AddServerIbanForTest(std::make_unique<autofill::Iban>(iban));
    payments.SetAutofillHasSeenIban();
    payments.SetAutofillHasSeenBnpl();
    payments.SetAutofillAmountExtractionAiTermsSeen();
    payments.IncrementPaymentMethodsMandatoryReauthPromoShownCounter();
    EXPECT_TRUE(payments.GetIbans().empty());
    EXPECT_FALSE(payments.HasPendingPaymentQueries());
    EXPECT_EQ(autofill::test_api(payments).CapturedRecordCount(), 0u);
    base::test::TestFuture<void> completed;
    payments.AddCallbackAfterRefreshCompleted(completed.GetCallback());
    EXPECT_TRUE(completed.IsReady());
    data->Refresh();
    EXPECT_EQ(
        context->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS),
        before);
    EXPECT_EQ(autofill::test_api(addresses).CapturedRecordCount(), 0u);
    EXPECT_EQ(autofill::test_api(payments).CapturedRecordCount(), 0u);
    EXPECT_FALSE(autofill::test_api(addresses).HasBoundDataServices());
    EXPECT_FALSE(autofill::test_api(payments).HasBoundDataServices());
  }
  auto contents =
      content::WebContentsTester::CreateTestWebContents(child.get(), nullptr);
  autofill::AutofillClientProviderFactory::GetForProfile(child.get())
      .CreateClientForWebContents(contents.get());
  auto* client =
      autofill::ContentAutofillClient::FromWebContents(contents.get());
  ASSERT_NE(client, nullptr);
  auto* history =
      autofill::AutocompleteHistoryManagerFactory::GetForProfile(child.get());
  ASSERT_NE(history, nullptr);
  EXPECT_FALSE(history->GetProfileDatabase());
  autofill::FormFieldData field;
  field.set_name(u"no_capture_field");
  field.set_value(u"no_capture_value");
  autofill::FormData form;
  form.set_fields({field});
  history->OnWillSubmitFormWithFields({field}, nullptr);
  base::test::TestFuture<autofill::FieldGlobalId,
                         std::vector<autofill::Suggestion>>
      suggestions;
  history->OnGetSingleFieldSuggestions(form, nullptr, field, nullptr, *client,
                                       suggestions.GetCallback());
  ASSERT_TRUE(suggestions.Wait());
  EXPECT_EQ(suggestions.Get<0>(), field.global_id());
  EXPECT_TRUE(suggestions.Get<1>().empty());
  contents.reset();
  child.reset();
}

TEST_F(StatelessNativeProfileCreationTest,
       MemoryNativeHistoryFaviconsAndSaltsAreRetainedAndIsolated) {
  using Factory = KeyedServiceBaseFactory;
  const void* parent = static_cast<content::BrowserContext*>(profile());
  const void* parent_key =
      static_cast<SimpleFactoryKey*>(profile()->GetProfileKey());
  Factory::ScopedContextAccessObserverForTesting fence(base::BindRepeating(
      [](const void* parent, const void* parent_key, const char* service,
         const void* requested, const void* selected) {
        CHECK_NE(requested, parent) << service;
        CHECK_NE(selected, parent) << service;
        CHECK_NE(requested, parent_key) << service;
        CHECK_NE(selected, parent_key) << service;
      },
      parent, parent_key));
  auto first = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  auto second = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  for (Profile* child : std::vector<Profile*>{first.get(), second.get()}) {
    // These factories default to null for TestingProfile parents. Use their
    // production builders, not a replacement store or mocked readback.
    HistoryServiceFactory::GetInstance()->SetTestingFactory(
        child, HistoryServiceFactory::GetDefaultFactory());
    FaviconServiceFactory::GetInstance()->SetTestingFactory(
        child, FaviconServiceFactory::GetDefaultFactory());
  }
  first->Init();
  second->Init();
  ASSERT_TRUE(first->IsOffTheRecord());
  ASSERT_TRUE(second->IsOffTheRecord());
  auto* history_service = HistoryServiceFactory::GetForProfile(
      first.get(), ServiceAccessType::EXPLICIT_ACCESS);
  auto* other_history = HistoryServiceFactory::GetForProfile(
      second.get(), ServiceAccessType::EXPLICIT_ACCESS);
  ASSERT_NE(history_service, nullptr);
  ASSERT_NE(other_history, nullptr);
  ASSERT_NE(history_service, other_history);
  const GURL page("https://guest-native.test/retained");
  auto contents =
      content::WebContentsTester::CreateTestWebContents(first.get(), nullptr);
  NativeAlloyTabHelperDelegate helpers;
  helpers.WebContentsCreated(contents.get(), false);
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(page);
  base::CancelableTaskTracker tracker;
  base::test::TestFuture<history::QueryURLResult> seen, absent;
  history_service->QueryURL(page, seen.GetCallback(), &tracker);
  other_history->QueryURL(page, absent.GetCallback(), &tracker);
  ASSERT_TRUE(seen.Get().success);
  EXPECT_EQ(seen.Get().row.url(), page);
  EXPECT_FALSE(absent.Get().success);

  SkBitmap bitmap;
  bitmap.allocN32Pixels(16, 16);
  bitmap.eraseColor(SK_ColorBLUE);
  const GURL icon("https://guest-native.test/icon.png");
  auto* favicons = FaviconServiceFactory::GetForProfile(
      first.get(), ServiceAccessType::EXPLICIT_ACCESS);
  auto* other_favicons = FaviconServiceFactory::GetForProfile(
      second.get(), ServiceAccessType::EXPLICIT_ACCESS);
  ASSERT_NE(favicons, nullptr);
  ASSERT_NE(other_favicons, nullptr);
  favicons->SetFavicons({page}, icon, favicon_base::IconType::kFavicon,
                        gfx::Image::CreateFrom1xBitmap(bitmap));
  base::test::TestFuture<const favicon_base::FaviconRawBitmapResult&> image,
      no_image;
  favicons->GetRawFaviconForPageURL(page, {favicon_base::IconType::kFavicon},
                                    16, false, image.GetCallback(), &tracker);
  other_favicons->GetRawFaviconForPageURL(
      page, {favicon_base::IconType::kFavicon}, 16, false,
      no_image.GetCallback(), &tracker);
  ASSERT_TRUE(image.Get().is_valid());
  EXPECT_EQ(image.Get().icon_url, icon);
  EXPECT_EQ(image.Get().pixel_size, gfx::Size(16, 16));
  EXPECT_FALSE(no_image.Get().is_valid());

  const auto key =
      blink::StorageKey::CreateFirstParty(url::Origin::Create(page));
  auto* salts =
      MediaDeviceSaltServiceFactory::GetForBrowserContext(first.get());
  auto* other_salts =
      MediaDeviceSaltServiceFactory::GetForBrowserContext(second.get());
  ASSERT_NE(salts, nullptr);
  ASSERT_NE(other_salts, nullptr);
  base::test::TestFuture<const std::string&> salt, repeat, other;
  salts->GetSalt(key, salt.GetCallback());
  salts->GetSalt(key, repeat.GetCallback());
  other_salts->GetSalt(key, other.GetCallback());
  EXPECT_FALSE(salt.Get().empty());
  EXPECT_EQ(salt.Get(), repeat.Get());
  EXPECT_NE(salt.Get(), other.Get());
  EXPECT_NE(first->GetOriginTrialsControllerDelegate(), nullptr);
  EXPECT_NE(second->GetOriginTrialsControllerDelegate(), nullptr);
  EXPECT_NE(first->GetOriginTrialsControllerDelegate(),
            second->GetOriginTrialsControllerDelegate());
  helpers.WebContentsDestroyed(contents.get());
  contents.reset();
  first.reset();
  second.reset();
  base::RunLoop().RunUntilIdle();
}

TEST_F(StatelessNativeProfileCreationTest,
       MemoryOriginTrialsValidateRetainIsolateClearAndRetireSignedTokens) {
  // Chromium's signed FrobulatePersistent fixture, expiring at Unix2000000000;
  // the scoped policy supplies its test public key to the real validator.
  blink::ScopedTestOriginTrialPolicy trial_policy;
  const std::string token =
      "A5J4wscfmyiy3zsOMNASl25jJffHq/XGKNoFCSWoAsL0aXv+ugtTQoE/"
      "Cs3a6pDezyQpKfYy10kcbdVC4i1n8AQAAABleyJvcmlnaW4iOiAiaHR0cHM6Ly9lbmFibGVk"
      "Lm"
      "V4YW1wbGUuY29tOjQ0MyIsICJmZWF0dXJlIjogIkZyb2J1bGF0ZVBlcnNpc3RlbnQiLCAiZX"
      "hw"
      "aXJ5IjogMjAwMDAwMDAwMH0=";
  const auto origin = url::Origin::Create(GURL("https://enabled.example.com"));
  const auto now = base::Time::FromTimeT(1000000000);
  using Factory = KeyedServiceBaseFactory;
  const void* parent = static_cast<content::BrowserContext*>(profile());
  const void* parent_key =
      static_cast<SimpleFactoryKey*>(profile()->GetProfileKey());
  Factory::ScopedContextAccessObserverForTesting fence(base::BindRepeating(
      [](const void* parent, const void* parent_key, const char* service,
         const void* requested, const void* selected) {
        CHECK_NE(requested, parent) << service;
        CHECK_NE(selected, parent) << service;
        CHECK_NE(requested, parent_key) << service;
        CHECK_NE(selected, parent_key) << service;
      },
      parent, parent_key));
  auto first = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  auto second = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  first->Init();
  second->Init();
  auto* trials = first->GetOriginTrialsControllerDelegate();
  auto* isolated = second->GetOriginTrialsControllerDelegate();
  ASSERT_NE(trials, nullptr);
  ASSERT_NE(isolated, nullptr);
  task_environment()->RunUntilIdle();
  std::string bad_signature = token;
  bad_signature[16] = bad_signature[16] == 'A' ? 'B' : 'A';
  const std::vector<std::string> invalid_tokens{bad_signature};
  trials->PersistTrialsFromTokens(origin, origin, invalid_tokens, now,
                                  std::nullopt);
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(trials->GetPersistedTrialsForOrigin(origin, origin, now).empty());
  const std::vector<std::string> valid_tokens{token};
  trials->PersistTrialsFromTokens(origin, origin, valid_tokens, now,
                                  std::nullopt);
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(trials->GetPersistedTrialsForOrigin(origin, origin, now)
                  .contains("FrobulatePersistent"));
  EXPECT_TRUE(trials->IsFeaturePersistedForOrigin(
      origin, origin,
      blink::mojom::OriginTrialFeature::kOriginTrialsSampleAPIPersistentFeature,
      now));
  EXPECT_TRUE(
      isolated->GetPersistedTrialsForOrigin(origin, origin, now).empty());
  trials->ClearPersistedTokens();
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(trials->GetPersistedTrialsForOrigin(origin, origin, now).empty());
  trials->PersistTrialsFromTokens(origin, origin, valid_tokens, now,
                                  std::nullopt);
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(trials->GetPersistedTrialsForOrigin(origin, origin, now)
                  .contains("FrobulatePersistent"));
  first.reset();
  task_environment()->RunUntilIdle();
  auto recreated = std::make_unique<OffTheRecordProfileImpl>(
      profile(),
      Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
  recreated->Init();
  auto* fresh = recreated->GetOriginTrialsControllerDelegate();
  ASSERT_NE(fresh, nullptr);
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(fresh->GetPersistedTrialsForOrigin(origin, origin, now).empty());
  EXPECT_TRUE(
      isolated->GetPersistedTrialsForOrigin(origin, origin, now).empty());
  recreated.reset();
  second.reset();
  task_environment()->RunUntilIdle();
}

}  // namespace
}  // namespace alloy::site_records
