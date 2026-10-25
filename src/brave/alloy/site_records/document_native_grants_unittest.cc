// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/scoped_observation.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "base/uuid.h"
#include "chrome/browser/bluetooth/bluetooth_chooser_context_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"
#include "chrome/browser/file_system_access/file_system_access_permission_context_factory.h"
#include "chrome/browser/hid/hid_chooser_context.h"
#include "chrome/browser/hid/hid_chooser_context_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/metrics/profile_metrics_service_factory.h"
#include "chrome/browser/origin_trials/origin_trials_factory.h"
#include "chrome/browser/extensions/chrome_extension_system.h"
#include "chrome/browser/extensions/chrome_extension_system_factory.h"
#include "chrome/browser/permissions/permission_actions_history_factory.h"
#include "chrome/browser/permissions/permission_decision_auto_blocker_factory.h"
#include "chrome/browser/permissions/permission_manager_factory.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/profiles/refcounted_profile_keyed_service_factory.h"
#include "chrome/browser/profiles/profile_key.h"
#include "chrome/browser/supervised_user/family_link_settings_service_factory.h"
#include "chrome/browser/serial/serial_chooser_context.h"
#include "chrome/browser/serial/serial_chooser_context_factory.h"
#include "chrome/browser/ssl/https_only_mode_tab_helper.h"
#include "chrome/browser/ssl/stateful_ssl_host_state_delegate_factory.h"
#include "chrome/browser/usb/usb_chooser_context.h"
#include "chrome/browser/usb/usb_chooser_context_factory.h"
#include "chrome/browser/usb/usb_chooser_controller.h"
#include "chrome/browser/ui/page_info/chrome_page_info_delegate.h"
#include "chrome/browser/ui/page_info/chrome_page_info_ui_delegate.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/content_settings/browser/ui/cookie_controls_controller.h"
#include "components/content_settings/browser/ui/cookie_controls_view.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/permissions/contexts/bluetooth_chooser_context.h"
#include "components/permissions/features.h"
#include "components/permissions/permission_actions_history.h"
#include "components/permissions/permission_context_base.h"
#include "components/permissions/permission_decision_auto_blocker.h"
#include "components/permissions/permission_manager.h"
#include "components/permissions/permission_request_data.h"
#include "components/permissions/permission_request_id.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "components/value_store/value_store_factory.h"
#include "components/security_interstitials/content/stateful_ssl_host_state_delegate.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/browser/alloy_network_generation.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/public/browser/document_user_data.h"
#include "content/public/browser/file_system_access_entry_factory.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/web_contents_tester.h"
#include "content/public/test/navigation_simulator.h"
#include "device/bluetooth/test/mock_bluetooth_device.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/api/storage/settings_namespace.h"
#include "extensions/browser/api/storage/storage_frontend.h"
#include "extensions/browser/extension_host_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/load_error_reporter.h"
#include "extensions/browser/state_store.h"
#include "extensions/browser/unpacked_installer.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "services/device/public/cpp/test/fake_hid_manager.h"
#include "services/device/public/cpp/test/fake_serial_port_manager.h"
#include "services/device/public/cpp/test/fake_usb_device_manager.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"
#include "third_party/blink/public/mojom/usb/web_usb_service.mojom.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {

class DocumentNativeGrantsTest : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    child_ = TestingProfile::Builder().BuildOffTheRecord(
         profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(
                       network_state()));
    ASSERT_NE(RequestContextPolicy::Get(*child_), nullptr);
    extensions::ChromeExtensionSystemFactory::GetInstance()->SetTestingFactoryAndUse(
        child_, base::BindRepeating([](content::BrowserContext* context)
                                      -> std::unique_ptr<KeyedService> {
          return std::make_unique<extensions::ChromeExtensionSystem>(
              Profile::FromBrowserContext(context));
        }));
    first_ = content::WebContentsTester::CreateTestWebContents(child_, nullptr);
    sibling_ =
        content::WebContentsTester::CreateTestWebContents(child_, nullptr);
    Navigate(first_.get());
    Navigate(sibling_.get());
  }

  void TearDown() override {
    sibling_.reset();
    first_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

 protected:
  virtual NetworkState network_state() const { return NetworkState::kKeep; }
  void Navigate(content::WebContents* contents) {
    content::WebContentsTester::For(contents)->NavigateAndCommit(
        GURL("https://grant-owner.test/page"));
  }
  content::RenderFrameHost* first() { return first_->GetPrimaryMainFrame(); }
  content::RenderFrameHost* sibling() {
    return sibling_->GetPrimaryMainFrame();
  }
  url::Origin origin() { return first()->GetLastCommittedOrigin(); }
  HostContentSettingsMap* settings() {
    return HostContentSettingsMapFactory::GetForFrame(first());
  }

  raw_ptr<TestingProfile> child_ = nullptr;
  std::unique_ptr<content::WebContents> first_;
  std::unique_ptr<content::WebContents> sibling_;
};

// A real native keyed factory with a construction side effect. Rejection must
// happen before constructing/retrieving the parent's service, not after a read.
class CountingFactory : public ProfileKeyedServiceFactory {
 public:
  using ProfileKeyedServiceFactory::GetServiceForBrowserContext;
  using ProfileKeyedServiceFactory::BrowserContextShutdown;
  using ProfileKeyedServiceFactory::BrowserContextDestroyed;
  CountingFactory()
      : ProfileKeyedServiceFactory(
            "AlloyParentReadProbe",
            ProfileSelections::BuildRedirectedInIncognito()) {}
  mutable int constructions = 0;

 private:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext*) const override {
    ++constructions;
    return std::make_unique<KeyedService>();
  }
};

class RefcountedSelectionProbe : public RefcountedProfileKeyedServiceFactory {
 public:
  RefcountedSelectionProbe()
      : RefcountedProfileKeyedServiceFactory(
            "AlloyRefcountedParentReadProbe",
            ProfileSelections::BuildRedirectedInIncognito()) {}
  content::BrowserContext* Select(content::BrowserContext* context) const {
    return GetBrowserContextToUse(context);
  }

 private:
  scoped_refptr<RefcountedKeyedService> BuildServiceInstanceFor(
      content::BrowserContext*) const override {
    ADD_FAILURE() << "Selection-only probe unexpectedly constructed a store";
    return nullptr;
  }
};

TEST_F(DocumentNativeGrantsTest, NativeFactoriesRejectParentBeforeOpenOrRead) {
  EXPECT_TRUE(child_->GetProfileKey()->IsAlloyStateless());
  EXPECT_EQ(supervised_user::FamilyLinkSettingsServiceFactory::GetForKey(
                child_->GetProfileKey()),
            nullptr);
  auto* profile_configuration = ProfileMetricsServiceFactory::GetForProfile(child_);
  ASSERT_NE(profile_configuration, nullptr);
  EXPECT_NE(profile_configuration,
            ProfileMetricsServiceFactory::GetForProfile(profile()));
  auto* configuration = extensions::ExtensionRegistry::Get(child_);
  ASSERT_NE(configuration, nullptr);
  EXPECT_NE(configuration, extensions::ExtensionRegistry::Get(profile()));
  EXPECT_EQ(configuration->GenerateInstalledExtensionsSet().size(), 0u);
  auto* extension_client = extensions::ExtensionsBrowserClient::Get();
  EXPECT_EQ(extension_client->GetOriginalContext(child_), child_);
  EXPECT_EQ(extension_client->GetContextRedirectedToOriginal(child_), child_);
  EXPECT_FALSE(extension_client->IsSameContext(child_, profile()));
  auto* system = extensions::ExtensionSystem::Get(child_);
  ASSERT_NE(system, nullptr);
  ASSERT_NE(system->state_store(), nullptr);
  auto storage = extensions::StorageFrontend::CreateForTesting(
      system->store_factory(), child_);
  for (auto area : {extensions::settings_namespace::LOCAL,
                    extensions::settings_namespace::SYNC,
                    extensions::settings_namespace::MANAGED}) {
    EXPECT_FALSE(storage->IsStorageEnabled(area));
  }
  EXPECT_NE(system->state_store(), extensions::ExtensionSystem::Get(profile())->state_store());
  EXPECT_NE(extensions::ExtensionHostRegistry::Get(child_), extensions::ExtensionHostRegistry::Get(profile()));
  EXPECT_NE(extensions::ExtensionPrefs::Get(child_), extensions::ExtensionPrefs::Get(profile()));
  system->state_store()->SetExtensionValue(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "namespace-probe", base::Value("not-retained"));
  base::test::TestFuture<std::optional<base::Value>> stored;
  system->state_store()->GetExtensionValue(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "namespace-probe", stored.GetCallback());
  ASSERT_TRUE(stored.Wait());
  EXPECT_FALSE(stored.Get().has_value());
  auto installer = extensions::UnpackedInstaller::Create(child_);
  extensions::LoadErrorReporter::Init(false);
  base::test::TestFuture<const extensions::Extension*, base::FilePath,
                         std::u16string> installed;
  installer->set_completion_callback(installed.GetCallback<
      const extensions::Extension*, const base::FilePath&, const std::u16string&>());
  installer->Load(base::FilePath("/nonexistent-stateless-extension"));
  ASSERT_TRUE(installed.Wait());
  EXPECT_EQ(installed.Get<0>(), nullptr);
  EXPECT_EQ(installed.Get<2>(),
            u"Extension installation is unavailable in stateless contexts");
  const auto private_configuration = ProfileSelections::Builder()
      .WithRegular(ProfileSelection::kRedirectedToOriginal)
      .WithAlloyStateless(ProfileSelection::kOwnInstance).Build();
  EXPECT_EQ(private_configuration.ApplyProfileSelection(child_), child_);
  EXPECT_EQ(private_configuration.ApplyProfileSelection(profile()->GetPrimaryOTRProfile(true)), profile());
  CountingFactory factory;
  auto* parent = factory.GetServiceForBrowserContext(profile(), true);
  ASSERT_NE(parent, nullptr);
  ASSERT_EQ(factory.constructions, 1);
  EXPECT_EQ(factory.GetServiceForBrowserContext(child_, true), nullptr);
  EXPECT_EQ(factory.GetServiceForBrowserContext(child_, false), nullptr);
  EXPECT_EQ(factory.constructions, 1);
  auto* ordinary = profile()->GetPrimaryOTRProfile(true);
  EXPECT_EQ(factory.GetServiceForBrowserContext(ordinary, true), parent);
  RefcountedSelectionProbe refcounted;
  EXPECT_EQ(refcounted.Select(child_), nullptr);
  EXPECT_EQ(refcounted.Select(ordinary), profile());
  EXPECT_EQ(HistoryServiceFactory::GetForProfile(
                child_, ServiceAccessType::EXPLICIT_ACCESS),
            nullptr);
  EXPECT_EQ(HistoryServiceFactory::GetForProfileWithoutCreating(child_),
            nullptr);
  EXPECT_EQ(FaviconServiceFactory::GetForProfile(
                 child_, ServiceAccessType::EXPLICIT_ACCESS),
            nullptr);
  EXPECT_EQ(OriginTrialsFactory::GetForBrowserContext(child_), nullptr);
  EXPECT_EQ(child_->GetOriginTrialsControllerDelegate(), nullptr);
  factory.BrowserContextShutdown(profile());
  factory.BrowserContextDestroyed(profile());
}

TEST_F(DocumentNativeGrantsTest,
       UsbFakeManagerGrantChooserPolicyAndRetirement) {
  device::FakeUsbDeviceManager manager;
  auto device =
      manager.CreateAndAddDevice(1000, 1, "Alloy", "Fixture", "serial");
  auto* current = UsbChooserContextFactory::GetForFrame(first());
  auto* peer = UsbChooserContextFactory::GetForFrame(sibling());
  ASSERT_NE(current, peer);
  for (auto* context : {current, peer}) {
    mojo::PendingRemote<device::mojom::UsbDeviceManager> remote;
    manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
    context->SetDeviceManagerForTesting(std::move(remote));
    base::test::TestFuture<std::vector<device::mojom::UsbDeviceInfoPtr>> list;
    context->GetDevices(list.GetCallback());
    ASSERT_TRUE(list.Wait());
    ASSERT_EQ(list.Get().size(), 1u);
  }
  const auto site = origin();
  current->GrantDevicePermission(site, *device);
  EXPECT_TRUE(current->HasDevicePermission(site, *device));
  EXPECT_FALSE(peer->HasDevicePermission(site, *device));
  child_->GetTestingPrefService()->SetManagedPref(
      prefs::kManagedDefaultWebUsbGuardSetting,
      base::Value(CONTENT_SETTING_BLOCK));
  EXPECT_FALSE(current->HasDevicePermission(site, *device));
  child_->GetTestingPrefService()->RemoveManagedPref(
      prefs::kManagedDefaultWebUsbGuardSetting);
  current->RevokeDevicePermissionWebInitiated(site, *device);
  EXPECT_FALSE(current->HasDevicePermission(site, *device));
  base::test::TestFuture<device::mojom::UsbDeviceInfoPtr> selected;
  auto controller = std::make_unique<UsbChooserController>(
      first(), blink::mojom::WebUsbRequestDeviceOptions::New(),
      selected.GetCallback());
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(controller->NumOptions(), 1u);
  auto weak = current->AsWeakPtr();
  Navigate(first_.get());
  EXPECT_FALSE(weak);
  controller->Select({0});
  controller.reset();
  ASSERT_TRUE(selected.Wait());
  EXPECT_FALSE(selected.Get());
  EXPECT_FALSE(
      UsbChooserContextFactory::GetForFrame(first())->HasDevicePermission(
          site, *device));
  EXPECT_FALSE(peer->HasDevicePermission(site, *device));
}

TEST_F(DocumentNativeGrantsTest, HidFakeManagerGrantRevocationAndRetirement) {
  device::FakeHidManager manager;
  auto device = device::mojom::HidDeviceInfo::New();
  device->guid = base::Uuid::GenerateRandomV4().AsLowercaseString();
  device->physical_device_id = "alloy-physical-fixture";
  device->vendor_id = 0x1234;
  device->product_id = 0xabcd;
  device->product_name = "Alloy fixture";
  device->serial_number = "fixture-serial";
  auto collection = device::mojom::HidCollectionInfo::New();
  collection->usage =
      device::mojom::HidUsageAndPage::New(device::mojom::kGenericDesktopGamePad,
                                          device::mojom::kPageGenericDesktop);
  collection->collection_type = device::mojom::kHIDCollectionTypeApplication;
  collection->input_reports.push_back(
      device::mojom::HidReportDescription::New());
  device->collections.push_back(std::move(collection));
  auto* current = HidChooserContextFactory::GetForFrame(first());
  auto* peer = HidChooserContextFactory::GetForFrame(sibling());
  for (auto* context : {current, peer}) {
    mojo::PendingRemote<device::mojom::HidManager> remote;
    manager.Bind(remote.InitWithNewPipeAndPassReceiver());
    base::test::TestFuture<std::vector<device::mojom::HidDeviceInfoPtr>> initialized;
    context->SetHidManagerForTesting(std::move(remote), initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    ASSERT_TRUE(initialized.Get().empty());
  }
  // The native test connection helper requires an empty initial enumeration;
  // exercise real DeviceAdded delivery and query the resulting chooser inventory.
  manager.AddDevice(device->Clone());
  base::RunLoop().RunUntilIdle();
  for (auto* context : {current, peer}) {
    base::test::TestFuture<std::vector<device::mojom::HidDeviceInfoPtr>> list;
    context->GetDevices(list.GetCallback());
    ASSERT_TRUE(list.Wait());
    ASSERT_EQ(list.Get().size(), 1u);
    EXPECT_EQ(list.Get().front()->guid, device->guid);
  }
  const auto site = origin();
  current->GrantDevicePermission(site, *device);
  EXPECT_TRUE(current->HasDevicePermission(site, *device));
  EXPECT_FALSE(peer->HasDevicePermission(site, *device));
  current->RevokeDevicePermission(site, *device);
  EXPECT_FALSE(current->HasDevicePermission(site, *device));
  current->GrantDevicePermission(site, *device);
  Navigate(first_.get());
  EXPECT_FALSE(
      HidChooserContextFactory::GetForFrame(first())->HasDevicePermission(
          site, *device));
  EXPECT_FALSE(peer->HasDevicePermission(site, *device));
}

TEST_F(DocumentNativeGrantsTest, SerialFakeManagerGrantPolicyAndRetirement) {
  device::FakeSerialPortManager manager;
  auto port = device::mojom::SerialPortInfo::New();
  port->token = base::UnguessableToken::Create();
  port->path = base::FilePath("/dev/alloy-not-a-real-port");
  port->display_name = "Alloy fake serial port";
  port->has_vendor_id = port->has_product_id = true;
  port->vendor_id = 1000;
  port->product_id = 1;
  port->serial_number = "alloy-fixture";
  auto* current = SerialChooserContextFactory::GetForFrame(first());
  auto* peer = SerialChooserContextFactory::GetForFrame(sibling());
  for (auto* context : {current, peer}) {
    mojo::PendingRemote<device::mojom::SerialPortManager> remote;
    manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
    context->SetPortManagerForTesting(std::move(remote));
    context->FlushPortManagerConnectionForTesting();
  }
  current->OnPortAdded(port->Clone());
  peer->OnPortAdded(port->Clone());
  const auto site = origin();
  current->GrantPortPermission(site, *port);
  EXPECT_TRUE(current->HasPortPermission(site, *port));
  EXPECT_FALSE(peer->HasPortPermission(site, *port));
  child_->GetTestingPrefService()->SetManagedPref(
      prefs::kManagedDefaultSerialGuardSetting,
      base::Value(CONTENT_SETTING_BLOCK));
  EXPECT_FALSE(current->HasPortPermission(site, *port));
  child_->GetTestingPrefService()->RemoveManagedPref(
      prefs::kManagedDefaultSerialGuardSetting);
  current->RevokePortPermissionWebInitiated(site, port->token);
  EXPECT_FALSE(current->HasPortPermission(site, *port));
  current->GrantPortPermission(site, *port);
  auto weak = current->AsWeakPtr();
  Navigate(first_.get());
  EXPECT_FALSE(weak);
  EXPECT_FALSE(
      SerialChooserContextFactory::GetForFrame(first())->HasPortPermission(
          site, *port));
}

TEST_F(DocumentNativeGrantsTest, BluetoothNativeIdsServicesAndRetirement) {
  device::MockBluetoothDevice device(nullptr, 0, "Alloy fixture",
                                     "00:11:22:33:44:55", false, false);
  auto* current = BluetoothChooserContextFactory::GetForFrame(first());
  auto* peer = BluetoothChooserContextFactory::GetForFrame(sibling());
  const auto site = origin();
  const device::BluetoothUUID service("180d");
  auto options = blink::mojom::WebBluetoothRequestDeviceOptions::New();
  options->accept_all_devices = true;
  options->optional_services.push_back(service);
  const auto id =
      current->GrantServiceAccessPermission(site, &device, options.get());
  EXPECT_TRUE(current->HasDevicePermission(site, id));
  EXPECT_TRUE(current->IsAllowedToAccessService(site, id, service));
  EXPECT_FALSE(peer->HasDevicePermission(site, id));
  current->RevokeDevicePermissionWebInitiated(site, id);
  EXPECT_FALSE(current->HasDevicePermission(site, id));
  const auto fresh =
      current->GrantServiceAccessPermission(site, &device, options.get());
  EXPECT_TRUE(current->HasDevicePermission(site, fresh));
  Navigate(first_.get());
  auto* replacement = BluetoothChooserContextFactory::GetForFrame(first());
  EXPECT_FALSE(replacement->HasDevicePermission(site, fresh));
  const auto next =
      replacement->GrantServiceAccessPermission(site, &device, options.get());
  EXPECT_NE(fresh, next);
  EXPECT_FALSE(peer->HasDevicePermission(site, next));
}

class GrantObserver
    : public content::FileSystemAccessPermissionGrant::Observer {
 public:
  void OnPermissionStatusChanged() override { ++changes; }
  int changes = 0;
};

TEST_F(DocumentNativeGrantsTest, NativeFileGrantsDenySurvivingReadAndWrite) {
  using Context = content::FileSystemAccessPermissionContext;
  using Status = content::FileSystemAccessPermissionGrant::PermissionStatus;
  auto* root = FileSystemAccessPermissionContextFactory::GetForProfile(child_);
  auto* current = root->ForDocument(first()->GetWeakDocumentPtr());
  auto* peer = root->ForDocument(sibling()->GetWeakDocumentPtr());
  ASSERT_NE(current, peer);
  const auto site = origin();
  const content::PathInfo path(
      base::FilePath("/nonexistent-alloy-fixture.txt"));
  auto read = current->GetReadPermissionGrant(
      site, path, Context::HandleType::kFile, Context::AccessTrigger::kOpen);
  auto write = current->GetWritePermissionGrant(
      site, path, Context::HandleType::kFile, Context::AccessTrigger::kSave);
  EXPECT_EQ(read->GetStatus(), Status::GRANTED);
  EXPECT_EQ(write->GetStatus(), Status::GRANTED);
  auto sibling_read =
      peer->GetReadPermissionGrant(site, path, Context::HandleType::kFile,
                                   Context::AccessTrigger::kProgrammaticRead);
  EXPECT_EQ(sibling_read->GetStatus(), Status::ASK);
  GrantObserver observer;
  base::ScopedObservation<content::FileSystemAccessPermissionGrant,
                          GrantObserver>
      observation(&observer);
  observation.Observe(read.get());
  Navigate(first_.get());
  EXPECT_EQ(read->GetStatus(), Status::DENIED);
  EXPECT_EQ(write->GetStatus(), Status::DENIED);
  EXPECT_GT(observer.changes, 0);
  observation.Reset();
  base::test::TestFuture<
      content::FileSystemAccessPermissionGrant::PermissionRequestOutcome>
      late;
  read->RequestPermission(first()->GetGlobalId(),
                          content::FileSystemAccessPermissionGrant::
                              UserActivationState::kNotRequired,
                          late.GetCallback());
  ASSERT_TRUE(late.Wait());
  EXPECT_EQ(read->GetStatus(), Status::DENIED);
  auto* replacement = root->ForDocument(first()->GetWeakDocumentPtr());
  EXPECT_EQ(
      replacement
          ->GetReadPermissionGrant(site, path, Context::HandleType::kFile,
                                   Context::AccessTrigger::kProgrammaticRead)
          ->GetStatus(),
      Status::ASK);
  EXPECT_EQ(sibling_read->GetStatus(), Status::ASK);
}

TEST_F(DocumentNativeGrantsTest, CopiedFileBindingCannotRetargetReplacement) {
  content::FileSystemAccessEntryFactory::BindingContext binding(
      first()->GetStorageKey(), first()->GetLastCommittedURL(),
      first()->GetGlobalId());
  const auto saved = binding;
  auto* root = FileSystemAccessPermissionContextFactory::GetForProfile(child_);
  ASSERT_NE(root->ForDocument(saved.document), nullptr);
  EXPECT_EQ(FileSystemAccessPermissionContextFactory::GetForDocument(saved.document),
            root->ForDocument(saved.document));
  Navigate(first_.get());
  EXPECT_EQ(saved.document.AsRenderFrameHostIfValid(), nullptr);
  EXPECT_EQ(root->ForDocument(saved.document), nullptr);
  EXPECT_EQ(FileSystemAccessPermissionContextFactory::GetForDocument(saved.document),
            nullptr);
  EXPECT_NE(root->ForDocument(first()->GetWeakDocumentPtr()), nullptr);
}

TEST_F(DocumentNativeGrantsTest, ChooserQueryConsumerKeepsOriginalDocument) {
  ChromePageInfoDelegate delegate(first_.get());
  EXPECT_EQ(delegate.GetChooserContext(ContentSettingsType::USB_CHOOSER_DATA),
            UsbChooserContextFactory::GetForFrame(first()));
  EXPECT_EQ(delegate.GetChooserContext(ContentSettingsType::HID_CHOOSER_DATA),
            HidChooserContextFactory::GetForFrame(first()));
  EXPECT_EQ(delegate.GetChooserContext(ContentSettingsType::SERIAL_CHOOSER_DATA),
            SerialChooserContextFactory::GetForFrame(first()));
  Navigate(first_.get());
  EXPECT_EQ(delegate.GetChooserContext(ContentSettingsType::USB_CHOOSER_DATA), nullptr);
  EXPECT_EQ(delegate.GetChooserContext(ContentSettingsType::HID_CHOOSER_DATA), nullptr);
  ChromePageInfoDelegate replacement(first_.get());
  EXPECT_EQ(replacement.GetChooserContext(ContentSettingsType::USB_CHOOSER_DATA),
            UsbChooserContextFactory::GetForFrame(first()));
}

TEST_F(DocumentNativeGrantsTest,
       PageInfoSettingsAndEffectivePermissionKeepOriginalDocument) {
  const auto site = origin();
  const auto url = site.GetURL();
  constexpr auto type = ContentSettingsType::MEDIASTREAM_MIC;
  constexpr auto permission = blink::PermissionType::AUDIO_CAPTURE;
  ChromePageInfoDelegate delegate(first_.get());
  ChromePageInfoDelegate peer(sibling_.get());
  ChromePageInfoUiDelegate labels(first_.get(), url);
  ChromePageInfoUiDelegate peer_labels(sibling_.get(), url);
  auto* current = delegate.GetContentSettings();
  ASSERT_EQ(current, settings());
  ASSERT_NE(current, peer.GetContentSettings());
  current->SetContentSettingDefaultScope(url, url, type, CONTENT_SETTING_ALLOW);
  peer.GetContentSettings()->SetContentSettingDefaultScope(
      url, url, type, CONTENT_SETTING_BLOCK);
  EXPECT_EQ(delegate.GetPermissionResult(permission, site, std::nullopt).status,
            blink::mojom::PermissionStatus::GRANTED);
  EXPECT_EQ(labels.GetPermissionResult(permission).status,
            blink::mojom::PermissionStatus::GRANTED);
  EXPECT_EQ(peer.GetPermissionResult(permission, site, std::nullopt).status,
            blink::mojom::PermissionStatus::DENIED);
  EXPECT_EQ(peer_labels.GetPermissionResult(permission).status,
            blink::mojom::PermissionStatus::DENIED);

  Navigate(first_.get());
  EXPECT_EQ(delegate.GetContentSettings(), nullptr);
  EXPECT_EQ(delegate.GetPermissionResult(permission, site, std::nullopt).status,
            blink::mojom::PermissionStatus::DENIED);
  EXPECT_EQ(labels.GetPermissionResult(permission).status,
            blink::mojom::PermissionStatus::DENIED);
  ChromePageInfoDelegate replacement(first_.get());
  replacement.GetContentSettings()->SetContentSettingDefaultScope(
      url, url, type, CONTENT_SETTING_ALLOW);
  EXPECT_EQ(replacement.GetPermissionResult(permission, site, std::nullopt).status,
            blink::mojom::PermissionStatus::GRANTED);
  EXPECT_EQ(labels.GetPermissionResult(permission).status,
            blink::mojom::PermissionStatus::DENIED);
  EXPECT_EQ(peer.GetContentSettings()->GetContentSetting(url, url, type),
            CONTENT_SETTING_BLOCK);
  first_.reset();
  EXPECT_EQ(delegate.GetContentSettings(), nullptr);
  EXPECT_EQ(replacement.GetContentSettings(), nullptr);
  EXPECT_EQ(labels.GetPermissionResult(permission).status,
            blink::mojom::PermissionStatus::DENIED);
}

class DocumentCookieControlsObserver
    : public content_settings::CookieControlsObserver {
 public:
  void OnStatusChanged(CookieControlsState state,
                       CookieControlsEnforcement enforcement,
                       base::Time) override {
    ++updates;
    last_state = state;
    last_enforcement = enforcement;
  }
  int updates = 0;
  CookieControlsState last_state = CookieControlsState::kHidden;
  CookieControlsEnforcement last_enforcement =
      CookieControlsEnforcement::kNoEnforcement;
};

TEST_F(DocumentNativeGrantsTest,
       CookieControlsBlockBypassAndCannotObserveReplacement) {
  ChromePageInfoDelegate delegate(first_.get());
  auto controller = delegate.CreateCookieControlsController();
  ASSERT_TRUE(controller);
  DocumentCookieControlsObserver observer;
  controller->AddObserver(&observer);
  const auto url = origin().GetURL();
  const auto baseline =
      settings()->GetSettingsForOneType(ContentSettingsType::COOKIES);
  controller->Update(first_.get());
  ASSERT_GT(observer.updates, 0);
  EXPECT_EQ(observer.last_state, CookieControlsState::kBlocked3pc);
  EXPECT_EQ(observer.last_enforcement,
            CookieControlsEnforcement::kEnforcedByCookieSetting);
  controller->OnCookieBlockingEnabledForSite(false);
  controller->OnCookieBlockingEnabledForSite(true);
  EXPECT_EQ(settings()->GetSettingsForOneType(ContentSettingsType::COOKIES),
            baseline);
  EXPECT_TRUE(settings()->GetWebsiteSetting(
      url, url, ContentSettingsType::COOKIE_CONTROLS_METADATA).is_none());
  EXPECT_FALSE(controller->StateChangedViaBypass());

  Navigate(first_.get());
  const auto updates = observer.updates;
  controller->Update(first_.get());
  controller->UpdateCookieControlsIcon();
  controller->OnCookieBlockingEnabledForSite(false);
  controller->SetStateChangedViaBypass(true);
  controller->OnUiClosing();
  EXPECT_EQ(observer.updates, updates);
  EXPECT_FALSE(controller->StateChangedViaBypass());
  EXPECT_EQ(delegate.CreateCookieControlsController(), nullptr);
  EXPECT_TRUE(settings()->GetWebsiteSetting(
      url, url, ContentSettingsType::COOKIE_CONTROLS_METADATA).is_none());
  ChromePageInfoDelegate replacement(first_.get());
  auto fresh = replacement.CreateCookieControlsController();
  ASSERT_TRUE(fresh);
  fresh->AddObserver(&observer);
  fresh->Update(first_.get());
  EXPECT_GT(observer.updates, updates);
  fresh->RemoveObserver(&observer);
  controller->RemoveObserver(&observer);
  first_.reset();
  EXPECT_EQ(delegate.CreateCookieControlsController(), nullptr);
  controller->OnUiClosing();
  fresh->OnUiClosing();
}

TEST_F(DocumentNativeGrantsTest,
       NativeHeuristicCannotSupersedeManagedPolicyOrRetargetRetiredRequest) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {blink::features::kGeolocationElement,
       permissions::features::kPermissionHeuristicAutoGrant}, {});
  constexpr auto type = ContentSettingsType::GEOLOCATION;
  const auto url = origin().GetURL();
  auto* manager = PermissionManagerFactory::GetForProfile(child_);
  auto* context = manager->GetPermissionContextForTesting(type);
  ASSERT_NE(context, nullptr);
  auto descriptor = content::PermissionDescriptorUtil::
      CreatePermissionDescriptorForPermissionType(blink::PermissionType::GEOLOCATION);
  auto request_for = [&](content::RenderFrameHost* frame) {
    permissions::PermissionRequestData request(
        descriptor, permissions::PermissionRequestID(
                        frame, permissions::PermissionRequestID::RequestLocalId()),
        true, url, url);
    request.embedded_permission_request_descriptor =
        blink::mojom::EmbeddedPermissionRequestDescriptor::New();
    auto geolocation =
        blink::mojom::GeolocationEmbeddedPermissionRequestDescriptor::New();
    geolocation->autolocate = false;
    request.embedded_permission_request_descriptor->detail =
        blink::mojom::EmbeddedPermissionControlDescriptorExtension::NewGeolocation(
            std::move(geolocation));
    return request;
  };
  auto request = request_for(first());
  ASSERT_TRUE(request.IsEligibleForHeuristicAutoGrant());
  auto* history = PermissionActionsHistoryFactory::GetForFrame(first());
  ASSERT_NE(history, nullptr);
  for (int i = 0; i != 4; ++i) {
    history->RecordTemporaryGrant(url, type);
  }
  ASSERT_TRUE(history->CheckHeuristicallyAutoGranted(url, type, false));
  EXPECT_EQ(context->GetPermissionStatus(request, first()).source,
            content::PermissionStatusSource::HEURISTIC_GRANT);
  auto peer_request = request_for(sibling());
  EXPECT_EQ(context->GetPermissionStatus(peer_request, sibling()).status,
            blink::mojom::PermissionStatus::ASK);

  child_->GetTestingPrefService()->SetManagedPref(
      prefs::kManagedDefaultGeolocationSetting, base::Value(CONTENT_SETTING_BLOCK));
  EXPECT_EQ(context->GetPermissionStatus(request, first()).status,
            blink::mojom::PermissionStatus::DENIED);
  EXPECT_TRUE(history->CheckHeuristicallyAutoGranted(url, type, false));
  child_->GetTestingPrefService()->RemoveManagedPref(
      prefs::kManagedDefaultGeolocationSetting);
  EXPECT_EQ(context->GetPermissionStatus(request, first()).source,
            content::PermissionStatusSource::HEURISTIC_GRANT);

  auto saved = request.Clone();
  Navigate(first_.get());
  EXPECT_EQ(context->GetPermissionStatus(saved, first()).status,
            blink::mojom::PermissionStatus::DENIED);
  auto replacement_request = request_for(first());
  EXPECT_EQ(context->GetPermissionStatus(replacement_request, first()).status,
            blink::mojom::PermissionStatus::ASK);
}

class RetiringDocumentAdmissionProbe
    : public content::DocumentUserData<RetiringDocumentAdmissionProbe> {
 public:
  ~RetiringDocumentAdmissionProbe() override {
    auto* frame = document_.AsRenderFrameHostIfValid();
    EXPECT_NE(frame, nullptr);
    if (frame) {
      // Native weak references intentionally survive into user-data teardown.
      // Alloy must nevertheless refuse every new working-state admission.
      EXPECT_FALSE(content::AlloyDocumentStateAdmitted(frame));
      EXPECT_EQ(content::GetAlloyNetworkGenerationForDocument(
                    content::RenderFrameHostImpl::From(frame)), nullptr);
      EXPECT_EQ(HostContentSettingsMapFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(PermissionDecisionAutoBlockerFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(PermissionActionsHistoryFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(UsbChooserContextFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(HidChooserContextFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(SerialChooserContextFactory::GetForFrame(frame), nullptr);
      EXPECT_EQ(FileSystemAccessPermissionContextFactory::GetForDocument(document_),
                nullptr);
    }
    if (retired_) {
      std::move(retired_).Run();
    }
  }

  void ObserveRetirement(base::OnceClosure retired) {
    retired_ = std::move(retired);
  }

 private:
  explicit RetiringDocumentAdmissionProbe(content::RenderFrameHost* frame)
      : DocumentUserData(frame), document_(frame->GetWeakDocumentPtr()) {}
  friend class content::DocumentUserData<RetiringDocumentAdmissionProbe>;
  DOCUMENT_USER_DATA_KEY_DECL();
  const content::WeakDocumentPtr document_;
  base::OnceClosure retired_;
};
DOCUMENT_USER_DATA_KEY_IMPL(RetiringDocumentAdmissionProbe);

TEST_F(DocumentNativeGrantsTest, CanonicalAdmissionClosesBeforeUserDataTeardown) {
  ASSERT_TRUE(content::AlloyDocumentStateAdmitted(first()));
  ASSERT_NE(settings(), nullptr);
  base::test::TestFuture<void> retired;
  RetiringDocumentAdmissionProbe::GetOrCreateForCurrentDocument(first())
      ->ObserveRetirement(retired.GetCallback());
  const auto original = first()->GetWeakDocumentPtr();
  Navigate(first_.get());
  ASSERT_TRUE(retired.Wait());
  EXPECT_EQ(original.AsRenderFrameHostIfValid(), nullptr);
  EXPECT_TRUE(content::AlloyDocumentStateAdmitted(first()));
  EXPECT_NE(settings(), nullptr);
  EXPECT_TRUE(content::AlloyDocumentStateAdmitted(sibling()));
}

class DocumentNetworkRetirementTest : public DocumentNativeGrantsTest {
 protected:
  NetworkState network_state() const override { return NetworkState::kCut; }
};

TEST_F(DocumentNetworkRetirementTest,
       BorrowedLeaseCannotKeepNetworkAuthorityAfterDocumentRetires) {
  auto generation = content::GetAlloyNetworkGenerationForDocument(
      content::RenderFrameHostImpl::From(first()));
  auto peer = content::GetAlloyNetworkGenerationForDocument(
      content::RenderFrameHostImpl::From(sibling()));
  ASSERT_TRUE(generation);
  ASSERT_TRUE(peer);
  ASSERT_NE(generation, peer);
  auto original_authority = generation->GetWeakPtr();
  auto peer_authority = peer->GetWeakPtr();
  ASSERT_TRUE(original_authority);
  Navigate(first_.get());
  ASSERT_FALSE(original_authority);
  EXPECT_FALSE(generation->GetWeakPtr());
  EXPECT_TRUE(peer_authority);
  EXPECT_NE(generation, content::GetAlloyNetworkGenerationForDocument(
                            content::RenderFrameHostImpl::From(first())));
  // A late native callback keeps only a disconnected Mojo capability. It must
  // neither reopen this generation nor borrow the partition's shared context.
  mojo::Remote<network::mojom::URLLoaderFactory> late_factory;
  base::RunLoop disconnected;
  auto receiver = late_factory.BindNewPipeAndPassReceiver();
  late_factory.set_disconnect_handler(disconnected.QuitClosure());
  generation->GetNetworkContext()->CreateURLLoaderFactory(
      std::move(receiver), network::mojom::URLLoaderFactoryParams::New());
  disconnected.Run();
  EXPECT_FALSE(late_factory.is_connected());
  EXPECT_TRUE(peer_authority);
}

TEST_F(DocumentNativeGrantsTest, NativeSslDecisionsAreOwnedAndExplicitClearStillWorks) {
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  const auto host = origin().host();
  auto owner = content::AlloyDocumentStateOwner(first());
  auto* current = StatefulSSLHostStateDelegateFactory::GetForOwner(child_, owner);
  auto* peer = StatefulSSLHostStateDelegateFactory::GetForOwner(
      child_, content::AlloyDocumentStateOwner(sibling()));
  auto* root = StatefulSSLHostStateDelegateFactory::GetForProfile(child_);
  auto* partition = first()->GetStoragePartition();
  ASSERT_NE(current, nullptr);
  ASSERT_NE(peer, nullptr);
  ASSERT_NE(current, peer);
  current->AllowCert(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition);
  EXPECT_EQ(current->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition),
            content::SSLHostStateDelegate::ALLOWED);
  EXPECT_EQ(peer->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition),
            content::SSLHostStateDelegate::DENIED);
  EXPECT_EQ(root->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition),
            content::SSLHostStateDelegate::DENIED);
  current->HostRanInsecureContent(host, content::SSLHostStateDelegate::MIXED_CONTENT);
  EXPECT_TRUE(current->DidHostRunInsecureContent(host, content::SSLHostStateDelegate::MIXED_CONTENT));
  EXPECT_FALSE(peer->DidHostRunInsecureContent(host, content::SSLHostStateDelegate::MIXED_CONTENT));
  current->AllowHttpForHost(host, partition);
  EXPECT_TRUE(current->IsHttpAllowedForHost(host, partition));
  EXPECT_FALSE(peer->IsHttpAllowedForHost(host, partition));
  root->Clear(base::NullCallback());
  EXPECT_EQ(current->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition),
            content::SSLHostStateDelegate::DENIED);
  EXPECT_FALSE(current->IsHttpAllowedForHost(host, partition));
  Navigate(first_.get());
  EXPECT_FALSE(owner);
  EXPECT_EQ(StatefulSSLHostStateDelegateFactory::GetForOwner(child_, owner), nullptr);
  auto* fresh = StatefulSSLHostStateDelegateFactory::GetForOwner(
      child_, content::AlloyDocumentStateOwner(first()));
  EXPECT_FALSE(fresh->DidHostRunInsecureContent(host, content::SSLHostStateDelegate::MIXED_CONTENT));
  EXPECT_EQ(content::AlloyStateOwnerForContext(content::AlloyDocumentStateOwner(first()), profile()), nullptr);
}

TEST_F(DocumentNativeGrantsTest, NativeHttpsMetadataAndApprovedReloadDoNotSeedLaterDocuments) {
  HttpsOnlyModeTabHelper::CreateForWebContents(first_.get());
  auto* helper = HttpsOnlyModeTabHelper::FromWebContents(first_.get());
  ASSERT_NE(helper, nullptr);
  const auto url = first()->GetLastCommittedURL();
  const auto host = url.GetHost();
  helper->set_fallback_url(url);
  helper->set_is_navigation_upgraded(true);
  helper->add_failed_upgrade(url, security_interstitials::https_only_mode::FallbackReason::kCertError);
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  auto* original = StatefulSSLHostStateDelegateFactory::GetForOwner(
      child_, content::AlloyDocumentStateOwner(first()));
  original->AllowCert(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, first()->GetStoragePartition());
  StatefulSSLHostStateDelegateFactory::PrepareHttpReload(child_, first_.get(), url);
  first_->GetController().Reload(content::ReloadType::NORMAL, false);
  auto reload = content::NavigationSimulator::CreateFromPending(first_->GetController());
  reload->ReadyToCommit();
  auto pending = content::AlloyNavigationStateOwner(reload->GetNavigationHandle());
  auto* approved = StatefulSSLHostStateDelegateFactory::GetForOwner(child_, pending);
  ASSERT_NE(approved, nullptr);
  EXPECT_TRUE(approved->IsHttpAllowedForHost(host, first()->GetStoragePartition()));
  EXPECT_EQ(approved->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID,
                                 first()->GetStoragePartition()), content::SSLHostStateDelegate::DENIED);
  reload->Commit();
  EXPECT_FALSE(helper->has_failed_upgrade(url));
  EXPECT_TRUE(StatefulSSLHostStateDelegateFactory::GetForOwner(
      child_, content::AlloyDocumentStateOwner(first()))->IsHttpAllowedForHost(host, first()->GetStoragePartition()));
  Navigate(first_.get());
  EXPECT_FALSE(pending);
  EXPECT_FALSE(helper->is_navigation_upgraded());
  EXPECT_TRUE(helper->fallback_url().is_empty());
  EXPECT_FALSE(helper->has_failed_upgrade(url));
  EXPECT_FALSE(StatefulSSLHostStateDelegateFactory::GetForOwner(
      child_, content::AlloyDocumentStateOwner(first()))->IsHttpAllowedForHost(host, first()->GetStoragePartition()));
}

}  // namespace
}  // namespace alloy::site_records
