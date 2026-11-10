// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"
#include "brave/alloy/site_records/site_profile_configuration.h"
#include "brave/alloy/site_records/site_record_decay.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"
#include "chrome/browser/file_system_access/file_system_access_permission_context_factory.h"
#include "chrome/browser/hid/hid_chooser_context.h"
#include "chrome/browser/hid/hid_chooser_context_factory.h"
#include "chrome/browser/serial/serial_chooser_context.h"
#include "chrome/browser/serial/serial_chooser_context_factory.h"
#include "chrome/browser/bluetooth/bluetooth_chooser_context_factory.h"
#include "components/permissions/contexts/bluetooth_chooser_context.h"
#include "chrome/browser/permissions/permission_manager_factory.h"
#include "chrome/browser/webid/federated_identity_permission_context.h"
#include "chrome/browser/webid/federated_identity_account_keyed_permission_context.h"
#include "chrome/browser/webid/federated_identity_permission_context_factory.h"
#include "chrome/browser/custom_handlers/protocol_handler_registry_factory.h"
#include "chrome/browser/ssl/stateful_ssl_host_state_delegate_factory.h"
#include "components/custom_handlers/protocol_handler.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "components/custom_handlers/pref_names.h"
#include "base/json/json_writer.h"
#include "components/permissions/permission_manager.h"
#include "components/permissions/permission_context_base.h"
#include "components/permissions/permission_request_data.h"
#include "components/security_interstitials/content/stateful_ssl_host_state_delegate.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/browser/usb/usb_chooser_context.h"
#include "chrome/browser/usb/usb_chooser_context_factory.h"
#include "chrome/browser/usb/usb_chooser_controller.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "content/public/browser/bluetooth_delegate.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "content/public/browser/alloy_site_retirement.h"
#include "content/public/browser/file_system_access_permission_context.h"
#include "content/public/browser/file_system_access_permission_grant.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/web_contents_tester.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "services/device/public/cpp/test/fake_usb_device_manager.h"
#include "services/device/public/cpp/test/fake_hid_manager.h"
#include "services/device/public/cpp/test/fake_serial_port_manager.h"
#include "content/browser/storage_partition_impl.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/navigation_or_document_handle.h"
#include "content/browser/ssl/ssl_error_handler.h"
#include "services/network/test/test_cookie_manager.h"
#include "services/network/test/test_network_context.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/schemeful_site.h"
#include "net/cert/cert_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "device/bluetooth/test/mock_bluetooth_device.h"
#include "base/uuid.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {
class RetirementPrefObserver final : public PrefStore::Observer {
 public:
  explicit RetirementPrefObserver(base::RepeatingClosure changed) : changed_(std::move(changed)) {}
  void OnPrefValueChanged(std::string_view) override { changed_.Run(); }
  void OnInitializationCompleted(bool) override {}
 private:
  base::RepeatingClosure changed_;
};

class SiteRecordVisitTest : public ChromeRenderViewHostTestHarness {};
TEST_F(SiteRecordVisitTest, OnlyCommittedTopLevelDocumentsRefreshTheRecordClock) {
  base::ScopedTempDir temp; ASSERT_TRUE(temp.CreateUniqueTempDir());
  ObjectId key{}; key.fill(0x31);
  std::shared_ptr<RecordFileSystem> files;
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(temp.GetPath().AppendASCII("profile"), SecretKey(key), {}, &files));
  auto prefs = base::MakeRefCounted<SitePrefStore>(files, base::BindRepeating([](FileStatus) {}));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, prefs->ReadPrefs());
  auto configuration = MakeSiteProfileConfiguration(files->ProfileCapability(StorePurpose::kProfileSettings),
      prefs->CreateNativeView(), prefs, {});
  ASSERT_TRUE(configuration->BeginConstruction());
  configuration->SetTeardownHandler(base::DoNothing());
  configuration->Attach(*profile()); configuration->ConstructionFinished();
  const GURL alpha("https://alpha.test/page"), beta("https://beta.test/page"), hop("https://hop.test/");
  NavigateAndCommit(alpha); task_environment()->RunUntilIdle();
  auto last = [&](const GURL& site) {
    RecordInfo info;
    EXPECT_EQ(FileStatus::kOk, files->ResolveRecord(site, url::Origin::Create(site), false, &info));
    return info;
  };
  auto original = last(alpha); EXPECT_GT(original.last_visit_us, 0);
  ASSERT_EQ(FileStatus::kOk, files->RecordVisit(original.id, 1));
  auto same = content::NavigationSimulator::CreateRendererInitiated(GURL(alpha.spec()+"#fragment"), main_rfh());
  same->CommitSameDocument(); task_environment()->RunUntilIdle();
  EXPECT_EQ(1, last(alpha).last_visit_us);
  auto* child = content::RenderFrameHostTester::For(main_rfh())->AppendChild("embedded");
  content::NavigationSimulator::CreateRendererInitiated(beta, child)->Commit();
  task_environment()->RunUntilIdle();
  RecordInfo absent; EXPECT_EQ(FileStatus::kNotFound, files->ResolveRecord(beta, url::Origin::Create(beta), false, &absent));
  EXPECT_EQ(1, last(alpha).last_visit_us);
  content::NavigationSimulator::Reload(web_contents()); task_environment()->RunUntilIdle();
  EXPECT_GT(last(alpha).last_visit_us, 1);
  auto redirect = content::NavigationSimulator::CreateBrowserInitiated(hop, web_contents());
  redirect->Start(); redirect->Redirect(beta); redirect->Commit(); task_environment()->RunUntilIdle();
  EXPECT_GT(last(beta).last_visit_us, 0);
  EXPECT_EQ(FileStatus::kNotFound, files->ResolveRecord(hop, url::Origin::Create(hop), false, &absent));
  ASSERT_EQ(FileStatus::kOk, files->RecordVisit(original.id, 1));
  content::NavigationSimulator::GoBack(web_contents()); task_environment()->RunUntilIdle();
  EXPECT_GT(last(alpha).last_visit_us, 1);
  ASSERT_EQ(FileStatus::kOk, files->Lock());
  configuration.reset(); task_environment()->RunUntilIdle();
}

class SiteRetirementTest : public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ObjectId bytes{}; bytes.fill(0x34);
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(
        temp_.GetPath().AppendASCII("encrypted"), SecretKey(bytes), {}, &files_));
    OpenNative(true);
    ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  }
  void NavigateNative(content::WebContents* contents, const GURL& url, const net::SSLInfo* ssl_info) {
    if (!ssl_info) {
      content::WebContentsTester::For(contents)->NavigateAndCommit(url);
      return;
    }
    auto navigation = content::NavigationSimulator::CreateBrowserInitiated(url, contents);
    navigation->SetSSLInfo(*ssl_info);
    navigation->Commit();
  }
  void OpenNative(bool with_alpha, const net::SSLInfo* ssl_info = nullptr) {
    store_ = base::MakeRefCounted<SitePrefStore>(files_,
        base::BindRepeating([](FileStatus status) { ADD_FAILURE() << static_cast<int>(status); }));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    RegisterUserProfilePrefs(registry.get());
    sync_preferences::PrefServiceSyncableFactory factory;
    factory.set_user_prefs(store_);
    native_ = TestingProfile::Builder().SetPrefService(factory.CreateSyncable(registry)).Build();
    RequestContextPolicy::AttachEncrypted(*native_, files_->ProfileCapability(StorePurpose::kProfileSettings));
    if (with_alpha) {
      alpha_ = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
      NavigateNative(alpha_.get(), a_, ssl_info);
    }
    beta_ = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
    NavigateNative(beta_.get(), b_, ssl_info);
  }
  void CloseNative() {
    before_close_.Reset(); during_native_drain_.Reset();
    beta_.reset(); alpha_.reset(); native_.reset(); store_.reset();
    task_environment()->RunUntilIdle();
  }
  void ReopenNative(bool with_alpha, const net::SSLInfo* ssl_info = nullptr) {
    CloseNative();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ObjectId bytes{}; bytes.fill(0x34);
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(
        temp_.GetPath().AppendASCII("encrypted"), SecretKey(bytes), &files_));
    OpenNative(with_alpha, ssl_info);
  }
  void TearDown() override {
    CloseNative(); files_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }
  void DeleteAlpha() {
    RecordInfo original;
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &original));
    const auto old = files_->Capability(original.id, StorePurpose::kSiteSettings);
    ASSERT_TRUE(old.active());
    base::test::TestFuture<FileStatus, RecordDeletion> result;
    store_->DeleteSiteWithTeardownAsync(a_, base::BindLambdaForTesting(
        [&](SitePrefStore::Completion done) {
          EXPECT_FALSE(old.active());
          RecordInfo refused;
          EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, url::Origin::Create(a_), true, &refused));
          content::RetireAlloySiteState(native_.get(),
              base::BindRepeating([](const url::Origin& origin) { return origin.host() == "alpha.test"; }),
              base::BindLambdaForTesting([&](content::WebContents* contents) {
                EXPECT_EQ(alpha_.get(), contents);
                if (before_close_) before_close_.Run();
                alpha_.reset();
              }),
              base::BindOnce([](scoped_refptr<SitePrefStore> store,
                               SitePrefStore::Completion done, bool success,
                               base::OnceClosure publication) {
                store->DeferNativeRetirementNotification(std::move(publication));
                std::move(done).Run(success ? FileStatus::kOk : FileStatus::kIo);
              }, store_, std::move(done)));
        }), base::BindOnce([](bool* acknowledged,
                             base::OnceCallback<void(FileStatus, RecordDeletion)> reply,
                             FileStatus status, RecordDeletion deletion) {
          *acknowledged = true; std::move(reply).Run(status, deletion);
        }, &deletion_acknowledged_, result.GetCallback()));
    if (during_native_drain_) during_native_drain_.Run();
    ASSERT_TRUE(result.Wait());
    EXPECT_EQ(FileStatus::kOk, result.Get<0>());
    EXPECT_FALSE(alpha_); EXPECT_TRUE(beta_);
  }
  base::ScopedTempDir temp_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  std::unique_ptr<TestingProfile> native_;
  std::unique_ptr<content::WebContents> alpha_, beta_;
  base::RepeatingClosure before_close_;
  base::RepeatingClosure during_native_drain_;
  bool deletion_acknowledged_ = false;
  const GURL a_{"https://alpha.test/page"}, b_{"https://beta.test/page"};
};

TEST_F(SiteRetirementTest, AcknowledgementRevokesRetainedFileGrantsWithoutTouchingUserFile) {
  using Context = content::FileSystemAccessPermissionContext;
  using Status = content::FileSystemAccessPermissionGrant::PermissionStatus;
  const auto path = temp_.GetPath().AppendASCII("user-selected.txt");
  ASSERT_TRUE(base::WriteFile(path, "user-owned bytes"));
  auto* root = FileSystemAccessPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(root);
  auto* alpha = root->ForDocument(alpha_->GetPrimaryMainFrame()->GetWeakDocumentPtr());
  auto* beta = root->ForDocument(beta_->GetPrimaryMainFrame()->GetWeakDocumentPtr());
  ASSERT_TRUE(alpha); ASSERT_TRUE(beta);
  auto read = alpha->GetReadPermissionGrant(url::Origin::Create(a_), content::PathInfo(path),
      Context::HandleType::kFile, Context::AccessTrigger::kOpen);
  auto write = alpha->GetWritePermissionGrant(url::Origin::Create(a_), content::PathInfo(path),
      Context::HandleType::kFile, Context::AccessTrigger::kSave);
  auto retained = beta->GetReadPermissionGrant(url::Origin::Create(b_), content::PathInfo(path),
      Context::HandleType::kFile, Context::AccessTrigger::kOpen);
  ASSERT_EQ(Status::GRANTED, read->GetStatus());
  ASSERT_EQ(Status::GRANTED, write->GetStatus());
  ASSERT_EQ(Status::GRANTED, retained->GetStatus());
  DeleteAlpha();
  EXPECT_EQ(Status::DENIED, read->GetStatus());
  EXPECT_EQ(Status::DENIED, write->GetStatus());
  EXPECT_EQ(Status::GRANTED, retained->GetStatus());
  std::string bytes; ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  EXPECT_EQ("user-owned bytes", bytes);
}

TEST_F(SiteRetirementTest, AcknowledgementRevokesEphemeralDeviceGrantAndPreservesOtherSite) {
  device::FakeUsbDeviceManager manager;
  auto device = manager.CreateAndAddDevice(1000, 1, "Alloy", "Ephemeral fixture", "");
  auto* current = UsbChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(current);
  mojo::PendingRemote<device::mojom::UsbDeviceManager> remote;
  manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
  current->SetDeviceManagerForTesting(std::move(remote));
  base::test::TestFuture<std::vector<device::mojom::UsbDeviceInfoPtr>> devices;
  current->GetDevices(devices.GetCallback());
  ASSERT_TRUE(devices.Wait()); ASSERT_EQ(1u, devices.Get().size());
  current->GrantDevicePermission(url::Origin::Create(a_), *device);
  current->GrantDevicePermission(url::Origin::Create(b_), *device);
  ASSERT_TRUE(current->HasDevicePermission(url::Origin::Create(a_), *device));
  ASSERT_TRUE(current->HasDevicePermission(url::Origin::Create(b_), *device));
  DeleteAlpha();
  EXPECT_FALSE(current->HasDevicePermission(url::Origin::Create(a_), *device));
  EXPECT_TRUE(current->HasDevicePermission(url::Origin::Create(b_), *device));
}

TEST_F(SiteRetirementTest, SharedAdmissionRetiresOnlyTheOriginalSiteEpoch) {
  auto* root = UsbChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(root);
  auto old = root->CapturePermissionAdmission(url::Origin::Create(a_));
  auto beta = root->CapturePermissionAdmission(url::Origin::Create(b_));
  ASSERT_TRUE(old.Run()); ASSERT_TRUE(beta.Run());
  DeleteAlpha();
  EXPECT_FALSE(old.Run()); EXPECT_TRUE(beta.Run());
  auto fresh = root->CapturePermissionAdmission(url::Origin::Create(a_));
  EXPECT_TRUE(fresh.Run()); EXPECT_FALSE(old.Run()); EXPECT_TRUE(beta.Run());
}

TEST_F(SiteRetirementTest, AcknowledgementRetiresHidSerialAndBluetoothTogether) {
  const auto alpha = url::Origin::Create(a_), beta = url::Origin::Create(b_);
  device::FakeHidManager hid_manager;
  auto hid = device::mojom::HidDeviceInfo::New();
  hid->guid = base::Uuid::GenerateRandomV4().AsLowercaseString();
  hid->physical_device_id = "alloy-ephemeral-hid";
  hid->vendor_id = 0x1234; hid->product_id = 0xabcd; hid->product_name = "fixture";
  auto collection = device::mojom::HidCollectionInfo::New();
  collection->usage = device::mojom::HidUsageAndPage::New(
      device::mojom::kGenericDesktopGamePad, device::mojom::kPageGenericDesktop);
  collection->collection_type = device::mojom::kHIDCollectionTypeApplication;
  collection->input_reports.push_back(device::mojom::HidReportDescription::New());
  hid->collections.push_back(std::move(collection));
  auto* hid_context = HidChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(hid_context);
  mojo::PendingRemote<device::mojom::HidManager> hid_remote;
  hid_manager.Bind(hid_remote.InitWithNewPipeAndPassReceiver());
  base::test::TestFuture<std::vector<device::mojom::HidDeviceInfoPtr>> initialized;
  hid_context->SetHidManagerForTesting(std::move(hid_remote), initialized.GetCallback());
  ASSERT_TRUE(initialized.Wait()); ASSERT_TRUE(initialized.Get().empty());
  hid_manager.AddDevice(hid->Clone()); base::RunLoop().RunUntilIdle();
  hid_context->GrantDevicePermission(alpha, *hid); hid_context->GrantDevicePermission(beta, *hid);
  ASSERT_TRUE(hid_context->HasDevicePermission(alpha, *hid));
  ASSERT_TRUE(hid_context->HasDevicePermission(beta, *hid));

  device::FakeSerialPortManager serial_manager;
  auto serial = device::mojom::SerialPortInfo::New();
  serial->token = base::UnguessableToken::Create();
  serial->path = base::FilePath("/dev/alloy-not-a-real-port");
  serial->display_name = "ephemeral fixture";
  auto* serial_context = SerialChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(serial_context);
  mojo::PendingRemote<device::mojom::SerialPortManager> serial_remote;
  serial_manager.AddReceiver(serial_remote.InitWithNewPipeAndPassReceiver());
  serial_context->SetPortManagerForTesting(std::move(serial_remote));
  serial_context->FlushPortManagerConnectionForTesting();
  serial_context->OnPortAdded(serial->Clone());
  serial_context->GrantPortPermission(alpha, *serial); serial_context->GrantPortPermission(beta, *serial);
  ASSERT_TRUE(serial_context->HasPortPermission(alpha, *serial));
  ASSERT_TRUE(serial_context->HasPortPermission(beta, *serial));

  device::MockBluetoothDevice bluetooth(nullptr, 0, "fixture", "00:11:22:33:44:55", false, false);
  auto* bluetooth_context = BluetoothChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(bluetooth_context);
  auto options = blink::mojom::WebBluetoothRequestDeviceOptions::New();
  options->accept_all_devices = true;
  options->optional_services.push_back(device::BluetoothUUID("180d"));
  auto alpha_id = bluetooth_context->GrantServiceAccessPermission(alpha, &bluetooth, options.get());
  auto beta_id = bluetooth_context->GrantServiceAccessPermission(beta, &bluetooth, options.get());
  ASSERT_TRUE(bluetooth_context->HasDevicePermission(alpha, alpha_id));
  ASSERT_TRUE(bluetooth_context->HasDevicePermission(beta, beta_id));
  DeleteAlpha();
  EXPECT_FALSE(hid_context->HasDevicePermission(alpha, *hid));
  EXPECT_FALSE(serial_context->HasPortPermission(alpha, *serial));
  EXPECT_FALSE(bluetooth_context->HasDevicePermission(alpha, alpha_id));
  EXPECT_TRUE(hid_context->HasDevicePermission(beta, *hid));
  EXPECT_TRUE(serial_context->HasPortPermission(beta, *serial));
  EXPECT_TRUE(bluetooth_context->HasDevicePermission(beta, beta_id));
}

TEST_F(SiteRetirementTest, PersistentAlphaRemovalPreservesUnrelatedEphemeralDeviceAuthority) {
  device::FakeUsbDeviceManager manager;
  auto persistent = manager.CreateAndAddDevice(1000, 1, "Alloy", "persistent", "alpha-serial");
  auto ephemeral = manager.CreateAndAddDevice(1000, 2, "Alloy", "ephemeral", "");
  auto* root = UsbChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(root);
  mojo::PendingRemote<device::mojom::UsbDeviceManager> remote;
  manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
  root->SetDeviceManagerForTesting(std::move(remote));
  base::test::TestFuture<std::vector<device::mojom::UsbDeviceInfoPtr>> devices;
  root->GetDevices(devices.GetCallback()); ASSERT_TRUE(devices.Wait()); ASSERT_EQ(2u, devices.Get().size());
  root->GrantDevicePermission(url::Origin::Create(a_), *persistent);
  root->GrantDevicePermission(url::Origin::Create(b_), *ephemeral);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(root->HasDevicePermission(url::Origin::Create(a_), *persistent));
  ASSERT_TRUE(root->HasDevicePermission(url::Origin::Create(b_), *ephemeral));
  DeleteAlpha();
  EXPECT_FALSE(root->HasDevicePermission(url::Origin::Create(a_), *persistent));
  EXPECT_TRUE(root->HasDevicePermission(url::Origin::Create(b_), *ephemeral));
}

TEST_F(SiteRetirementTest, ScanOnlyDeviceIdentityRetiresWithoutAChosenDeviceObject) {
  auto* root = BluetoothChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(root);
  const auto alpha = url::Origin::Create(a_), beta = url::Origin::Create(b_);
  const std::string address = "00:11:22:33:44:55";
  const auto old = root->AddScannedDevice(alpha, address);
  const auto retained = root->AddScannedDevice(beta, address);
  ASSERT_EQ(address, root->GetDeviceAddress(alpha, old));
  ASSERT_EQ(address, root->GetDeviceAddress(beta, retained));
  DeleteAlpha();
  EXPECT_TRUE(root->GetDeviceAddress(alpha, old).empty());
  EXPECT_EQ(address, root->GetDeviceAddress(beta, retained));
  auto fresh = root->AddScannedDevice(alpha, address);
  EXPECT_NE(old, fresh);
  EXPECT_EQ(retained, root->GetWebBluetoothDeviceId(beta, address));
}

TEST_F(SiteRetirementTest, NativeFactoriesRejectRetiredDocumentBeforeItsDestruction) {
  auto* frame = alpha_->GetPrimaryMainFrame();
  const auto document = frame->GetWeakDocumentPtr();
  auto* files = FileSystemAccessPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(files);
  before_close_ = base::BindLambdaForTesting([&] {
    EXPECT_TRUE(document.AsRenderFrameHostIfValid());
    EXPECT_FALSE(content::AlloyDocumentStateAdmitted(frame));
    EXPECT_EQ(nullptr, HostContentSettingsMapFactory::GetForFrame(frame));
    EXPECT_EQ(nullptr, UsbChooserContextFactory::GetForFrame(frame));
    EXPECT_EQ(nullptr, HidChooserContextFactory::GetForFrame(frame));
    EXPECT_EQ(nullptr, SerialChooserContextFactory::GetForFrame(frame));
    EXPECT_EQ(nullptr, BluetoothChooserContextFactory::GetForFrame(frame));
    EXPECT_EQ(nullptr, files->ForDocument(document));
  });
  DeleteAlpha();
  EXPECT_FALSE(document.AsRenderFrameHostIfValid());
}

TEST_F(SiteRetirementTest, HeldNativeChooserCannotReacquirePermissionAfterRecordRetirement) {
  device::FakeUsbDeviceManager manager;
  auto device = manager.CreateAndAddDevice(1000, 1, "Alloy", "held chooser", "");
  auto* root = UsbChooserContextFactory::GetForFrame(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(root);
  mojo::PendingRemote<device::mojom::UsbDeviceManager> remote;
  manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
  root->SetDeviceManagerForTesting(std::move(remote));
  base::test::TestFuture<std::vector<device::mojom::UsbDeviceInfoPtr>> devices;
  root->GetDevices(devices.GetCallback()); ASSERT_TRUE(devices.Wait());
  base::test::TestFuture<device::mojom::UsbDeviceInfoPtr> selected;
  auto chooser = std::make_unique<UsbChooserController>(alpha_->GetPrimaryMainFrame(),
      blink::mojom::WebUsbRequestDeviceOptions::New(), selected.GetCallback());
  base::RunLoop().RunUntilIdle(); ASSERT_EQ(1u, chooser->NumOptions());
  DeleteAlpha();
  ASSERT_FALSE(root->HasDevicePermission(url::Origin::Create(a_), *device));
  chooser->Select({0}); chooser.reset();
  ASSERT_TRUE(selected.Wait()); EXPECT_FALSE(selected.Get());
  EXPECT_FALSE(root->HasDevicePermission(url::Origin::Create(a_), *device));
}

TEST_F(SiteRetirementTest, NativeBluetoothConsumerFailsClosedForAStillLiveRetiredFrame) {
  auto* bluetooth = content::GetContentClientForTesting()->browser()->GetBluetoothDelegate();
  ASSERT_TRUE(bluetooth);
  auto* frame = alpha_->GetPrimaryMainFrame();
  before_close_ = base::BindLambdaForTesting([&] {
    ASSERT_TRUE(frame->GetWeakDocumentPtr().AsRenderFrameHostIfValid());
    ASSERT_FALSE(content::AlloyDocumentStateAdmitted(frame));
    EXPECT_TRUE(bluetooth->GetPermittedDevices(frame).empty());
    EXPECT_FALSE(bluetooth->HasDevicePermission(frame, blink::WebBluetoothDeviceId()));
    EXPECT_TRUE(bluetooth->GetDeviceAddress(frame, blink::WebBluetoothDeviceId()).empty());
  });
  DeleteAlpha();
}

TEST_F(SiteRetirementTest, WebIdRetiresEveryTypedParticipantAndPreservesUnrelatedSharing) {
  auto* identity = FederatedIdentityPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(identity);
  const auto alpha = url::Origin::Create(a_), beta = url::Origin::Create(b_);
  const auto gamma = url::Origin::Create(GURL("https://gamma.test"));
  const auto delta = url::Origin::Create(GURL("https://delta.test"));
  identity->GrantSharingPermission(alpha, beta, gamma, "requester");
  identity->GrantSharingPermission(beta, alpha, gamma, "embedder");
  identity->GrantSharingPermission(beta, beta, alpha, "provider");
  identity->GrantSharingPermission(beta, beta, delta, "retained");
  identity->FlushScheduledSaveSettingsCalls();
  ASSERT_TRUE(identity->HasSharingPermission(alpha, beta, gamma));
  ASSERT_TRUE(identity->HasSharingPermission(beta, alpha, gamma));
  ASSERT_TRUE(identity->HasSharingPermission(beta, beta, alpha));
  ASSERT_TRUE(identity->HasSharingPermission(beta, beta, delta));
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  DeleteAlpha();
  EXPECT_FALSE(identity->HasSharingPermission(alpha, beta, gamma));
  EXPECT_FALSE(identity->HasSharingPermission(beta, alpha, gamma));
  EXPECT_FALSE(identity->HasSharingPermission(beta, beta, alpha));
  EXPECT_TRUE(identity->HasSharingPermission(beta, beta, delta));
  std::string snapshot;
  ASSERT_TRUE(base::JSONWriter::Write(store_->GetValues(), &snapshot));
  EXPECT_EQ(std::string::npos, snapshot.find("alpha.test"));
  EXPECT_NE(std::string::npos, snapshot.find("delta.test"));
  identity->FlushScheduledSaveSettingsCalls();
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  auto cold = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus status) {
    ADD_FAILURE() << static_cast<int>(status);
  }));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, cold->ReadPrefs());
  ASSERT_TRUE(base::JSONWriter::Write(cold->GetValues(), &snapshot));
  EXPECT_EQ(std::string::npos, snapshot.find("alpha.test"));
  EXPECT_NE(std::string::npos, snapshot.find("delta.test"));
}

TEST_F(SiteRetirementTest, PublicationCannotNotifyWhileOriginalDocumentAdmissionRemainsOpen) {
  RecordInfo original;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &original));
  auto old = files_->Capability(original.id, StorePurpose::kSiteSettings);
  auto* frame = alpha_->GetPrimaryMainFrame();
  auto* settings = HostContentSettingsMapFactory::GetForProfile(native_.get());
  const auto document = frame->GetWeakDocumentPtr();
  const auto permission = settings->CapturePermissionAdmission(url::Origin::Create(a_));
  ASSERT_TRUE(permission.Run()); ASSERT_TRUE(content::AlloyDocumentStateAdmitted(frame));
  bool observed = false;
  RetirementPrefObserver observer(base::BindLambdaForTesting([&] {
    if (observed || old.active()) return;
    observed = true;
    // This is an ordinary native preference notification, after durable key
    // retirement but before the deletion API's final acknowledgement.
    // Approved full native drain may already have destroyed the original frame.
    auto* original_frame = document.AsRenderFrameHostIfValid();
    EXPECT_FALSE(content::AlloyDocumentStateAdmitted(original_frame));
    EXPECT_FALSE(permission.Run());
    EXPECT_EQ(nullptr, UsbChooserContextFactory::GetForFrame(original_frame));
  }));
  store_->AddObserver(&observer);
  DeleteAlpha();
  store_->RemoveObserver(&observer);
  EXPECT_TRUE(observed);
}

namespace {
class HeldCookieProjection final : public network::TestCookieManager {
 public:
  void SetContentSettings(ContentSettingsType type,
      const std::vector<ContentSettingPatternSource>& settings,
      SetContentSettingsCallback done) override {
    if (type != ContentSettingsType::FEDERATED_IDENTITY_SHARING) {
      std::move(done).Run(); return;
    }
    std::vector<std::string> next;
    for (const auto& setting : settings)
      next.push_back(setting.primary_pattern.ToString() + "," + setting.secondary_pattern.ToString());
    if (!hold) {
      projection = std::move(next); std::move(done).Run(); return;
    }
    pending.push_back(base::BindOnce(
        [](HeldCookieProjection* transport, std::vector<std::string> next,
           SetContentSettingsCallback done) {
          transport->projection = std::move(next); std::move(done).Run();
        }, base::Unretained(this), std::move(next), std::move(done)));
    if (received) std::move(received).Run();
  }
  bool hold = false;
  base::OnceClosure received;
  std::vector<base::OnceClosure> pending;
  std::vector<std::string> projection;
  mojo::Receiver<network::mojom::CookieManager> receiver{this};
};
class ProjectionNetworkContext final : public network::TestNetworkContext {
 public:
  void GetCookieManager(mojo::PendingReceiver<network::mojom::CookieManager> incoming) override {
    ++cookie_bindings;
    cookie.receiver.Bind(std::move(incoming));
  }
  size_t cookie_bindings = 0;
  HeldCookieProjection cookie;
  mojo::Receiver<network::mojom::NetworkContext> receiver{this};
};

class NoOsProtocolDelegate final : public custom_handlers::ProtocolHandlerRegistry::Delegate {
 public:
  void RegisterExternalHandler(const std::string&) override {}
  bool IsExternalHandlerRegistered(const std::string&) override { return false; }
  void RegisterWithOSAsDefaultClient(const std::string&, DefaultClientCallback callback) override {
    std::move(callback).Run(true);
  }
  void CheckDefaultClientWithOS(const std::string&, DefaultClientCallback callback) override {
    std::move(callback).Run(true);
  }
  bool ShouldRemoveHandlersNotInOS() override { return false; }
};
}  // namespace

TEST_F(SiteRetirementTest, NativeWebIdProjectionMustAcknowledgeBeforePreferencePublicationOrDeletion) {
  ProjectionNetworkContext transport;
  mojo::PendingRemote<network::mojom::NetworkContext> network;
  transport.receiver.Bind(network.InitWithNewPipeAndPassReceiver());
  auto* partition = static_cast<content::StoragePartitionImpl*>(native_->GetDefaultStoragePartition());
  partition->GetNetworkContext();  // Initialize the native owner before replacing transport.
  partition->SetNetworkContextForTesting(std::move(network));
  // Replacement and the old cached sub-interface have distinct pipe lifetimes.
  // Drain their native disconnect/flush fence before issuing any operation;
  // interrupted permission/projection calls must never be replayed.
  partition->FlushNetworkInterfaceForTesting();
  partition->GetCookieManagerForBrowserProcess();
  partition->FlushNetworkInterfaceForTesting();
  ASSERT_EQ(1u, transport.cookie_bindings);
  auto* identity = FederatedIdentityPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(identity);
  const auto alpha = url::Origin::Create(a_), beta = url::Origin::Create(b_);
  const auto delta = url::Origin::Create(GURL("https://delta.test"));
  identity->GrantSharingPermission(beta, beta, alpha, "retired");
  identity->GrantSharingPermission(beta, beta, delta, "retained");
  base::test::TestFuture<void> alpha_ready, beta_ready;
  identity->MarkStorageAccessEligible(net::SchemefulSite(beta), net::SchemefulSite(alpha),
      alpha_ready.GetCallback());
  identity->MarkStorageAccessEligible(net::SchemefulSite(beta), net::SchemefulSite(delta),
      beta_ready.GetCallback());
  ASSERT_TRUE(alpha_ready.Wait()); ASSERT_TRUE(beta_ready.Wait());
  ASSERT_EQ(2u, transport.cookie.projection.size());
  identity->FlushScheduledSaveSettingsCalls();
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  bool notified = false;
  RetirementPrefObserver observer(base::BindLambdaForTesting([&] {
    notified = true;
    ASSERT_TRUE(transport.cookie.pending.empty());
    EXPECT_EQ(1u, transport.cookie.projection.size());
    EXPECT_FALSE(identity->HasSharingPermission(beta, beta, alpha));
    EXPECT_TRUE(identity->HasSharingPermission(beta, beta, delta));
  }));
  base::test::TestFuture<void> held;
  transport.cookie.hold = true;
  transport.cookie.received = held.GetCallback();
  during_native_drain_ = base::BindLambdaForTesting([&] {
    ASSERT_TRUE(held.Wait());
    EXPECT_FALSE(deletion_acknowledged_); EXPECT_FALSE(notified);
    EXPECT_FALSE(alpha_); EXPECT_TRUE(beta_);
    EXPECT_FALSE(identity->HasSharingPermission(beta, beta, alpha));
    EXPECT_TRUE(identity->HasSharingPermission(beta, beta, delta));
    {
      // A newly constructed native owner must not load old relationships from
      // a provider cache while the original network projection is still held.
      FederatedIdentityAccountKeyedPermissionContext late(native_.get());
      EXPECT_FALSE(late.HasPermission(beta, beta, alpha));
      EXPECT_TRUE(late.HasPermission(beta, beta, delta));
    }
    RecordInfo refused;
    EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, alpha, true, &refused));
    transport.cookie.hold = false;
    auto completions = std::move(transport.cookie.pending);
    transport.cookie.pending.clear();
    for (auto& complete : completions) std::move(complete).Run();
  });
  store_->AddObserver(&observer);
  DeleteAlpha();
  store_->RemoveObserver(&observer);
  EXPECT_TRUE(notified); EXPECT_TRUE(deletion_acknowledged_);
  ASSERT_EQ(1u, transport.cookie.projection.size());
  EXPECT_EQ(std::string::npos, transport.cookie.projection[0].find("alpha.test"));
  EXPECT_NE(std::string::npos, transport.cookie.projection[0].find("delta.test"));
}

TEST_F(SiteRetirementTest, NativeProtocolHandlersRetireLiveAndColdWithoutTouchingOtherSite) {
  // Keep the production profile selector and registry; replace only OS transport.
  ProtocolHandlerRegistryFactory::GetInstance()->SetTestingFactory(
      native_.get(), ProtocolHandlerRegistryFactory::GetDefaultFactory());
  auto* handlers = ProtocolHandlerRegistryFactory::GetForBrowserContext(native_.get());
  ASSERT_TRUE(handlers);
  handlers->SetDelegateForTesting(std::make_unique<NoOsProtocolDelegate>());
  const auto alpha = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloyalpha", GURL("https://alpha.test/handler?q=%s"));
  const auto beta = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloybeta", GURL("https://beta.test/handler?q=%s"));
  const auto ignored = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloyignored", GURL("https://alpha.test/refused?q=%s"));
  handlers->OnAcceptRegisterProtocolHandler(alpha);
  handlers->OnAcceptRegisterProtocolHandler(beta);
  handlers->OnIgnoreRegisterProtocolHandler(ignored);
  ASSERT_TRUE(handlers->IsRegistered(alpha)); ASSERT_TRUE(handlers->IsRegistered(beta));
  ASSERT_TRUE(handlers->IsIgnored(ignored));
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  DeleteAlpha();
  EXPECT_FALSE(handlers->IsRegistered(alpha));
  EXPECT_FALSE(handlers->HasDefaultHandler(alpha.protocol()));
  EXPECT_FALSE(handlers->IsIgnored(ignored));
  EXPECT_TRUE(handlers->IsRegistered(beta));
  EXPECT_TRUE(handlers->HasDefaultHandler(beta.protocol()));
  std::string snapshot;
  ASSERT_TRUE(base::JSONWriter::Write(store_->GetValues(), &snapshot));
  EXPECT_EQ(std::string::npos, snapshot.find("alpha.test"));
  EXPECT_NE(std::string::npos, snapshot.find("beta.test/handler"));
  auto cold = custom_handlers::ProtocolHandlerRegistry::Create(native_->GetPrefs(),
      std::make_unique<NoOsProtocolDelegate>());
  EXPECT_FALSE(cold->IsRegistered(alpha)); EXPECT_FALSE(cold->IsIgnored(ignored));
  EXPECT_TRUE(cold->IsRegistered(beta));
  cold->Shutdown();
}

TEST_F(SiteRetirementTest, WebIdNativeOwnersReopenEncryptedRelationshipsBeforeAndAfterDeletion) {
  const auto alpha = url::Origin::Create(a_), beta = url::Origin::Create(b_);
  const auto gamma = url::Origin::Create(GURL("https://gamma.test"));
  const auto delta = url::Origin::Create(GURL("https://delta.test"));
  auto* identity = FederatedIdentityPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(identity);
  identity->GrantSharingPermission(alpha, beta, gamma, "requester");
  identity->GrantSharingPermission(beta, alpha, gamma, "embedder");
  identity->GrantSharingPermission(beta, beta, alpha, "provider");
  identity->GrantSharingPermission(beta, beta, delta, "retained");
  identity->FlushScheduledSaveSettingsCalls();
  ASSERT_EQ(FileStatus::kOk, store_->Flush());

  // Destroy every native service/pref projection and release the old encrypted
  // filesystem owner, then open the authoritative ciphertext with its key.
  ReopenNative(true);
  ASSERT_TRUE(native_); ASSERT_TRUE(alpha_); ASSERT_TRUE(beta_);
  identity = FederatedIdentityPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(identity);
  ASSERT_TRUE(identity->HasSharingPermission(alpha, beta, gamma));
  ASSERT_TRUE(identity->HasSharingPermission(beta, alpha, gamma));
  ASSERT_TRUE(identity->HasSharingPermission(beta, beta, alpha));
  ASSERT_TRUE(identity->HasSharingPermission(beta, beta, delta));
  DeleteAlpha();
  ReopenNative(false);
  ASSERT_TRUE(native_); EXPECT_FALSE(alpha_); ASSERT_TRUE(beta_);
  identity = FederatedIdentityPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(identity);
  EXPECT_FALSE(identity->HasSharingPermission(alpha, beta, gamma));
  EXPECT_FALSE(identity->HasSharingPermission(beta, alpha, gamma));
  EXPECT_FALSE(identity->HasSharingPermission(beta, beta, alpha));
  EXPECT_TRUE(identity->HasSharingPermission(beta, beta, delta));
  std::string snapshot;
  ASSERT_TRUE(base::JSONWriter::Write(store_->GetValues(), &snapshot));
  EXPECT_EQ(std::string::npos, snapshot.find("alpha.test"));
  EXPECT_NE(std::string::npos, snapshot.find("delta.test"));
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_, alpha, false, &absent));
}

TEST_F(SiteRetirementTest, ProtocolFactoryReopensEncryptedRegistryBeforeAndAfterDeletion) {
  const auto alpha = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloycoldalpha", GURL("https://alpha.test/handler?q=%s"));
  const auto beta = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloycoldbeta", GURL("https://beta.test/handler?q=%s"));
  const auto alpha_ignored = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloycoldignoredalpha", GURL("https://alpha.test/refused?q=%s"));
  const auto beta_ignored = custom_handlers::ProtocolHandler::CreateProtocolHandler(
      "web+alloycoldignoredbeta", GURL("https://beta.test/refused?q=%s"));
  auto selected_registry = [&] {
    ProtocolHandlerRegistryFactory::GetInstance()->SetTestingFactory(
        native_.get(), ProtocolHandlerRegistryFactory::GetDefaultFactory());
    auto* registry = ProtocolHandlerRegistryFactory::GetForBrowserContext(native_.get());
    if (registry) registry->SetDelegateForTesting(std::make_unique<NoOsProtocolDelegate>());
    return registry;
  };
  auto* handlers = selected_registry();
  ASSERT_TRUE(handlers);
  handlers->OnAcceptRegisterProtocolHandler(alpha);
  handlers->OnAcceptRegisterProtocolHandler(beta);
  handlers->OnIgnoreRegisterProtocolHandler(alpha_ignored);
  handlers->OnIgnoreRegisterProtocolHandler(beta_ignored);
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  ReopenNative(true);
  ASSERT_TRUE(native_); ASSERT_TRUE(alpha_); ASSERT_TRUE(beta_);
  handlers = selected_registry();
  ASSERT_TRUE(handlers);
  ASSERT_TRUE(handlers->IsRegistered(alpha)); ASSERT_TRUE(handlers->IsRegistered(beta));
  ASSERT_TRUE(handlers->HasDefaultHandler(alpha.protocol()));
  ASSERT_TRUE(handlers->HasDefaultHandler(beta.protocol()));
  ASSERT_TRUE(handlers->IsIgnored(alpha_ignored)); ASSERT_TRUE(handlers->IsIgnored(beta_ignored));
  EXPECT_EQ(alpha.url(), handlers->GetHandlerFor(alpha.protocol()).url());
  EXPECT_EQ(beta.url(), handlers->GetHandlerFor(beta.protocol()).url());
  DeleteAlpha();
  ReopenNative(false);
  ASSERT_TRUE(native_); EXPECT_FALSE(alpha_); ASSERT_TRUE(beta_);
  handlers = selected_registry();
  ASSERT_TRUE(handlers);
  EXPECT_FALSE(handlers->IsRegistered(alpha)); EXPECT_FALSE(handlers->IsIgnored(alpha_ignored));
  EXPECT_FALSE(handlers->HasDefaultHandler(alpha.protocol()));
  EXPECT_TRUE(handlers->IsRegistered(beta)); EXPECT_TRUE(handlers->IsIgnored(beta_ignored));
  ASSERT_TRUE(handlers->HasDefaultHandler(beta.protocol()));
  EXPECT_EQ(beta.url(), handlers->GetHandlerFor(beta.protocol()).url());
  std::string snapshot;
  ASSERT_TRUE(base::JSONWriter::Write(store_->GetValues(), &snapshot));
  EXPECT_EQ(std::string::npos, snapshot.find("alpha.test"));
  EXPECT_NE(std::string::npos, snapshot.find("beta.test/handler"));
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &absent));
}

TEST_F(SiteRetirementTest, SavedPermissionRequestCannotReadAFreshDocumentsGrant) {
  auto* context = PermissionManagerFactory::GetForProfile(native_.get())
      ->GetPermissionContextForTesting(ContentSettingsType::MEDIASTREAM_MIC);
  ASSERT_TRUE(context);
  auto descriptor = content::PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(
      blink::PermissionType::AUDIO_CAPTURE);
  auto request_for = [&](content::RenderFrameHost* frame) {
    return permissions::PermissionRequestData(descriptor,
        permissions::PermissionRequestID(frame, permissions::PermissionRequestID::RequestLocalId()),
        true, a_, a_);
  };
  auto old = request_for(alpha_->GetPrimaryMainFrame());
  auto* settings = HostContentSettingsMapFactory::GetForProfile(native_.get());
  settings->SetContentSettingDefaultScope(a_, a_, ContentSettingsType::MEDIASTREAM_MIC, CONTENT_SETTING_ALLOW);
  ASSERT_EQ(blink::mojom::PermissionStatus::GRANTED,
      context->GetPermissionStatus(old, alpha_->GetPrimaryMainFrame()).status);
  DeleteAlpha();
  alpha_ = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
  content::WebContentsTester::For(alpha_.get())->NavigateAndCommit(a_);
  settings->SetContentSettingDefaultScope(a_, a_, ContentSettingsType::MEDIASTREAM_MIC, CONTENT_SETTING_ALLOW);
  auto fresh = request_for(alpha_->GetPrimaryMainFrame());
  EXPECT_EQ(blink::mojom::PermissionStatus::GRANTED,
      context->GetPermissionStatus(fresh, alpha_->GetPrimaryMainFrame()).status);
  EXPECT_EQ(blink::mojom::PermissionStatus::DENIED,
      context->GetPermissionStatus(old, alpha_->GetPrimaryMainFrame()).status);
}

TEST_F(SiteRetirementTest, EncryptedNativeSslDecisionsColdReopenAndSelectiveDeletion) {
  using SSL = content::SSLHostStateDelegate;
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  net::SSLInfo ssl_error;
  ssl_error.cert = certificate;
  ssl_error.cert_status = net::CERT_STATUS_AUTHORITY_INVALID;
  auto* ssl = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(ssl);
  auto* partition = native_->GetDefaultStoragePartition();
  for (const auto& host : {a_.GetHost(), b_.GetHost()}) {
    ssl->AllowCert(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition);
    ssl->AllowHttpForHost(host, partition);
    ssl->SetHttpsEnforcementForHost(host, true, partition);
    EXPECT_EQ(SSL::ALLOWED, ssl->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  }
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  // A clean synthetic TLS response intentionally revokes learned exceptions in
  // SSLManager. This fixture represents the SAME accepted certificate error.
  ReopenNative(true, &ssl_error);
  ASSERT_TRUE(native_); ASSERT_TRUE(alpha_); ASSERT_TRUE(beta_);
  ssl = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(ssl);
  partition = native_->GetDefaultStoragePartition();
  for (const auto& host : {a_.GetHost(), b_.GetHost()}) {
    ASSERT_EQ(SSL::ALLOWED, ssl->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
    ASSERT_TRUE(ssl->IsHttpAllowedForHost(host, partition));
    ASSERT_TRUE(ssl->IsHttpsEnforcedForUrl(GURL("https://"+host), partition));
  }
  DeleteAlpha();
  EXPECT_EQ(SSL::DENIED, ssl->QueryPolicy(a_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_FALSE(ssl->IsHttpAllowedForHost(a_.GetHost(), partition));
  EXPECT_FALSE(ssl->IsHttpsEnforcedForUrl(a_, partition));
  EXPECT_EQ(SSL::ALLOWED, ssl->QueryPolicy(b_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_TRUE(ssl->IsHttpAllowedForHost(b_.GetHost(), partition));
  EXPECT_TRUE(ssl->IsHttpsEnforcedForUrl(b_, partition));
  ReopenNative(false, &ssl_error);
  ASSERT_TRUE(native_); EXPECT_FALSE(alpha_); ASSERT_TRUE(beta_);
  ssl = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(ssl);
  partition = native_->GetDefaultStoragePartition();
  EXPECT_EQ(SSL::DENIED, ssl->QueryPolicy(a_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_FALSE(ssl->IsHttpAllowedForHost(a_.GetHost(), partition));
  EXPECT_FALSE(ssl->IsHttpsEnforcedForUrl(a_, partition));
  EXPECT_EQ(SSL::ALLOWED, ssl->QueryPolicy(b_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_TRUE(ssl->IsHttpAllowedForHost(b_.GetHost(), partition));
  EXPECT_TRUE(ssl->IsHttpsEnforcedForUrl(b_, partition));
}

TEST_F(SiteRetirementTest, NativeSslCleanResponseRevokesLearnedExceptionsSelectively) {
  using SSL = content::SSLHostStateDelegate;
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  auto* ssl = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(ssl);
  auto* partition = native_->GetDefaultStoragePartition();
  for (const auto& host : {a_.GetHost(), b_.GetHost()}) {
    ssl->AllowCert(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition);
    ssl->AllowHttpForHost(host, partition);
    ASSERT_EQ(SSL::ALLOWED, ssl->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  }
  net::SSLInfo clean;
  clean.cert = certificate;
  clean.cert_status = 0;
  NavigateNative(alpha_.get(), a_, &clean);
  EXPECT_EQ(SSL::DENIED, ssl->QueryPolicy(a_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_FALSE(ssl->IsHttpAllowedForHost(a_.GetHost(), partition));
  EXPECT_EQ(SSL::ALLOWED, ssl->QueryPolicy(b_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
  EXPECT_TRUE(ssl->IsHttpAllowedForHost(b_.GetHost(), partition));
}

TEST_F(SiteRetirementTest, NativeSslRetirementClearsNonDefaultGrantsAndInsecureHostMetadata) {
  using SSL = content::SSLHostStateDelegate;
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  auto* ssl = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(ssl);
  auto* partition = native_->GetStoragePartition(
      content::StoragePartitionConfig::Create(native_.get(), "alloy", "ssl-fixture", true));
  ASSERT_TRUE(partition); ASSERT_NE(native_->GetDefaultStoragePartition(), partition);
  for (const auto& host : {a_.GetHost(), b_.GetHost()}) {
    ssl->AllowCert(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition);
    ssl->AllowHttpForHost(host, partition);
    ssl->SetHttpsEnforcementForHost(host, true, partition);
    ssl->HostRanInsecureContent(host, SSL::MIXED_CONTENT);
    ssl->HostRanInsecureContent(host, SSL::CERT_ERRORS_CONTENT);
    ASSERT_EQ(SSL::ALLOWED, ssl->QueryPolicy(host, *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
    ASSERT_TRUE(ssl->IsHttpAllowedForHost(host, partition));
    ASSERT_TRUE(ssl->DidHostRunInsecureContent(host, SSL::MIXED_CONTENT));
    ASSERT_TRUE(ssl->DidHostRunInsecureContent(host, SSL::CERT_ERRORS_CONTENT));
    ASSERT_TRUE(ssl->IsHttpsEnforcedForUrl(GURL("https://"+host), partition));
  }
  auto verify_retired = base::BindLambdaForTesting([&] {
    EXPECT_EQ(SSL::DENIED, ssl->QueryPolicy(a_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
    EXPECT_FALSE(ssl->IsHttpAllowedForHost(a_.GetHost(), partition));
    EXPECT_FALSE(ssl->DidHostRunInsecureContent(a_.GetHost(), SSL::MIXED_CONTENT));
    EXPECT_FALSE(ssl->DidHostRunInsecureContent(a_.GetHost(), SSL::CERT_ERRORS_CONTENT));
    EXPECT_FALSE(ssl->IsHttpsEnforcedForUrl(a_, partition));
    EXPECT_EQ(SSL::ALLOWED, ssl->QueryPolicy(b_.GetHost(), *certificate, net::ERR_CERT_AUTHORITY_INVALID, partition));
    EXPECT_TRUE(ssl->IsHttpAllowedForHost(b_.GetHost(), partition));
    EXPECT_TRUE(ssl->DidHostRunInsecureContent(b_.GetHost(), SSL::MIXED_CONTENT));
    EXPECT_TRUE(ssl->DidHostRunInsecureContent(b_.GetHost(), SSL::CERT_ERRORS_CONTENT));
    EXPECT_TRUE(ssl->IsHttpsEnforcedForUrl(b_, partition));
  });
  before_close_ = verify_retired;
  DeleteAlpha();
  verify_retired.Run();
}

TEST_F(SiteRetirementTest, NativeSslOwnerResolutionRejectsRetiredOriginalAdmission) {
  auto alpha_owner = content::AlloyDocumentStateOwner(alpha_->GetPrimaryMainFrame());
  auto beta_owner = content::AlloyDocumentStateOwner(beta_->GetPrimaryMainFrame());
  ASSERT_TRUE(alpha_owner); ASSERT_TRUE(beta_owner);
  auto* root = StatefulSSLHostStateDelegateFactory::GetForProfile(native_.get());
  ASSERT_TRUE(root);
  ASSERT_TRUE(root->ForAlloyStateOwner(alpha_owner));
  ASSERT_TRUE(StatefulSSLHostStateDelegateFactory::GetForOwner(native_.get(), alpha_owner));
  auto verify_retired = base::BindLambdaForTesting([&] {
    EXPECT_EQ(nullptr, root->ForAlloyStateOwner(alpha_owner));
    EXPECT_EQ(nullptr, StatefulSSLHostStateDelegateFactory::GetForOwner(native_.get(), alpha_owner));
    EXPECT_NE(nullptr, root->ForAlloyStateOwner(beta_owner));
    EXPECT_NE(nullptr, StatefulSSLHostStateDelegateFactory::GetForOwner(native_.get(), beta_owner));
  });
  before_close_ = verify_retired;
  DeleteAlpha();
  verify_retired.Run();
  alpha_ = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
  content::WebContentsTester::For(alpha_.get())->NavigateAndCommit(a_);
  auto fresh = content::AlloyDocumentStateOwner(alpha_->GetPrimaryMainFrame());
  ASSERT_TRUE(fresh);
  EXPECT_NE(nullptr, root->ForAlloyStateOwner(fresh));
  EXPECT_NE(nullptr, StatefulSSLHostStateDelegateFactory::GetForOwner(native_.get(), fresh));
  verify_retired.Run();
}

TEST_F(SiteRetirementTest, NativeSslPendingReplyDeniesRetiredOwnerAndKeepsBetaRequest) {
  class Endpoint final : public content::SSLErrorHandler::Delegate {
   public:
    void CancelSSLRequest(int error, const net::SSLInfo*) override { ++cancelled; last_error = error; }
    void ContinueSSLRequest() override { ++continued; }
    int cancelled = 0, continued = 0, last_error = net::OK;
    base::WeakPtrFactory<Endpoint> weak{this};
  } alpha_reply, beta_reply;
  auto certificate = net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(certificate);
  net::SSLInfo error;
  error.cert = certificate;
  error.cert_status = net::CERT_STATUS_AUTHORITY_INVALID;
  auto alpha_handle = content::RenderFrameHostImpl::From(alpha_->GetPrimaryMainFrame())->GetNavigationOrDocumentHandle();
  auto beta_handle = content::RenderFrameHostImpl::From(beta_->GetPrimaryMainFrame())->GetNavigationOrDocumentHandle();
  auto pending_alpha = std::make_unique<content::SSLErrorHandler>(alpha_.get(),
      alpha_reply.weak.GetWeakPtr(), true, a_, net::ERR_CERT_AUTHORITY_INVALID, error, false, alpha_handle.get());
  content::SSLErrorHandler pending_beta(beta_.get(), beta_reply.weak.GetWeakPtr(), true,
      b_, net::ERR_CERT_AUTHORITY_INVALID, error, false, beta_handle.get());
  EXPECT_TRUE(pending_alpha->alloy_scoped()); EXPECT_TRUE(pending_beta.alloy_scoped());
  ASSERT_TRUE(pending_alpha->GetScopedStateDelegate());
  ASSERT_TRUE(pending_beta.GetScopedStateDelegate());
  before_close_ = base::BindLambdaForTesting([&] {
    // The native request reply is released after key/original admission revoke,
    // before the WebContents is destroyed. No unsupported raw-pointer lifetime.
    EXPECT_EQ(nullptr, pending_alpha->GetScopedStateDelegate());
    pending_alpha->ContinueRequest();
    EXPECT_EQ(0, alpha_reply.continued); EXPECT_EQ(1, alpha_reply.cancelled);
    EXPECT_EQ(net::ERR_CERT_AUTHORITY_INVALID, alpha_reply.last_error);
    pending_alpha.reset();
  });
  DeleteAlpha();
  EXPECT_FALSE(pending_alpha);
  pending_beta.ContinueRequest();
  EXPECT_EQ(1, beta_reply.continued); EXPECT_EQ(0, beta_reply.cancelled);
}

TEST_F(SiteRetirementTest, RetainedFileGrantCannotRetargetOrEraseAFreshGenerationSlot) {
  using Context = content::FileSystemAccessPermissionContext;
  using Status = content::FileSystemAccessPermissionGrant::PermissionStatus;
  auto* root = FileSystemAccessPermissionContextFactory::GetForProfile(native_.get());
  ASSERT_TRUE(root);
  const content::PathInfo path(temp_.GetPath().AppendASCII("selected-again.txt"));
  auto old = root->GetReadPermissionGrant(url::Origin::Create(a_), path,
      Context::HandleType::kFile, Context::AccessTrigger::kOpen);
  ASSERT_EQ(Status::GRANTED, old->GetStatus());
  DeleteAlpha();
  EXPECT_EQ(Status::DENIED, old->GetStatus());
  alpha_ = content::WebContentsTester::CreateTestWebContents(native_.get(), nullptr);
  content::WebContentsTester::For(alpha_.get())->NavigateAndCommit(a_);
  auto fresh = root->ForDocument(alpha_->GetPrimaryMainFrame()->GetWeakDocumentPtr())
      ->GetReadPermissionGrant(url::Origin::Create(a_), path,
          Context::HandleType::kFile, Context::AccessTrigger::kProgrammaticRead);
  ASSERT_NE(old.get(), fresh.get());
  EXPECT_EQ(Status::ASK, fresh->GetStatus());
  base::test::TestFuture<content::FileSystemAccessPermissionGrant::PermissionRequestOutcome> late;
  old->RequestPermission(alpha_->GetPrimaryMainFrame()->GetGlobalId(),
      content::FileSystemAccessPermissionGrant::UserActivationState::kNotRequired, late.GetCallback());
  ASSERT_TRUE(late.Wait());
  EXPECT_EQ(Status::DENIED, old->GetStatus()); EXPECT_EQ(Status::ASK, fresh->GetStatus());
  old.reset();
  auto again = root->GetReadPermissionGrant(url::Origin::Create(a_), path,
      Context::HandleType::kFile, Context::AccessTrigger::kProgrammaticRead);
  EXPECT_EQ(fresh.get(), again.get()); EXPECT_EQ(Status::ASK, again->GetStatus());
}
}  // namespace
}  // namespace alloy::site_records
