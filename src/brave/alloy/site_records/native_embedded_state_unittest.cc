// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/request_context_policy.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/run_loop.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_command_line.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/uuid.h"
#include "brave/components/brave_shields/core/browser/brave_shields_utils.h"
#include "chrome/browser/bluetooth/bluetooth_chooser_context_factory.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/hid/hid_chooser_context.h"
#include "chrome/browser/hid/hid_chooser_context_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/media/webrtc/media_device_salt_service_factory.h"
#include "chrome/browser/permissions/chrome_permissions_client.h"
#include "chrome/browser/serial/serial_chooser_context.h"
#include "chrome/browser/serial/serial_chooser_context_factory.h"
#include "chrome/browser/usb/usb_chooser_context.h"
#include "chrome/browser/usb/usb_chooser_context_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/favicon/core/favicon_service.h"
#include "components/media_device_salt/media_device_salt_service.h"
#include "components/permissions/contexts/bluetooth_chooser_context.h"
#include "components/permissions/features.h"
#include "components/permissions/permission_actions_history.h"
#include "components/permissions/permission_decision_auto_blocker.h"
#include "components/persistent_cache/backend.h"
#include "components/persistent_cache/backend_storage.h"
#include "components/persistent_cache/client.h"
#include "components/persistent_cache/pending_backend.h"
#include "components/persistent_cache/persistent_cache.h"
#include "components/persistent_cache/persistent_cache_collection.h"
#include "components/persistent_cache/transaction_error.h"
#include "components/prefs/pref_service.h"
#include "components/services/storage/public/cpp/buckets/bucket_info.h"
#include "components/services/storage/public/mojom/blob_storage_context.mojom.h"
#include "components/services/storage/public/mojom/service_worker_storage_control.mojom.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "content/browser/alloy_embedded_state.h"
#include "content/browser/alloy_network_generation.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/browser/buckets/alloy_embedded_bucket_storage.h"
#include "content/browser/buckets/bucket_manager.h"
#include "content/browser/code_cache/generated_code_cache.h"
#include "content/browser/dom_storage/alloy_embedded_local_storage.h"
#include "content/browser/dom_storage/dom_storage_context_wrapper.h"
#include "content/browser/file_system_access/file_system_access_manager_impl.h"
#include "content/browser/media/media_devices_util.h"
#include "content/browser/origin_trials/critical_origin_trials_throttle.h"
#include "content/browser/process_lock.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/origin_trial_state_host_impl.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/security/cpsp/child_process_security_policy_impl.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/service_worker_client.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_main_resource_handle.h"
#include "content/browser/service_worker/service_worker_metrics.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_registry.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/browser/storage_partition_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/origin_trials_controller_delegate.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/runtime_feature_state/runtime_feature_state_document_data.h"
#include "content/public/browser/site_isolation_policy.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "crypto/hash.h"
#include "device/bluetooth/test/mock_bluetooth_device.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/features.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/base/network_isolation_key.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_setting_override.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/storage_access_api/status.h"
#include "services/device/public/cpp/test/fake_usb_device_manager.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/cache_storage_side_data_writer.mojom.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "storage/browser/blob/blob_data_builder.h"
#include "storage/browser/blob/blob_data_handle.h"
#include "storage/browser/blob/blob_reader.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_key.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_key_range.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_metadata.h"
#include "third_party/blink/public/common/loader/code_cache_util.h"
#include "third_party/blink/public/common/origin_trials/scoped_test_origin_trial_policy.h"
#include "third_party/blink/public/common/origin_trials/trial_token_result.h"
#include "third_party/blink/public/common/origin_trials/trial_token_validator.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/serialized_blob.mojom.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom.h"
#include "third_party/blink/public/mojom/buckets/bucket_manager_host.mojom.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_response.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_access_handle_host.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_delegate_host.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_handle.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_writer.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_transfer_token.mojom.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom.h"
#include "third_party/blink/public/mojom/loader/code_cache.mojom.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "third_party/blink/public/mojom/origin_trials/origin_trial_feature.mojom-shared.h"
#include "third_party/blink/public/mojom/origin_trials/origin_trial_state_host.mojom.h"
#include "third_party/blink/public/mojom/quota/quota_manager_host.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

using Backing = EmbeddedStateRegistry::Backing;
using Scope = EmbeddedStateScope;

class NativeIDBOpen final : public blink::mojom::IDBFactoryClient,
                            public blink::mojom::IDBDatabaseCallbacks {
 public:
  void Open(mojo::Remote<blink::mojom::IDBFactory>& factory,
            bool create_store = false) {
    create_store_ = create_store;
    factory->Open(client_.BindNewEndpointAndPassRemote(),
                  callbacks_.BindNewEndpointAndPassRemote(),
                  u"scoped-native-database", 1,
                  transaction_.BindNewEndpointAndPassReceiver(), 1, 0, false);
    ASSERT_TRUE(opened_.Wait());
  }
  mojo::AssociatedRemote<blink::mojom::IDBTransaction> BeginTransaction(
      int64_t id) {
    mojo::AssociatedRemote<blink::mojom::IDBTransaction> transaction;
    database_->CreateTransaction(
        transaction.BindNewEndpointAndPassReceiver(), id, {1},
        blink::mojom::IDBTransactionMode::ReadWrite,
        blink::mojom::IDBTransactionDurability::Relaxed);
    return transaction;
  }
  void Count(int64_t id, blink::mojom::IDBDatabase::CountCallback callback) {
    database_->Count(id, 1, blink::IndexedDBIndexMetadata::kInvalidId,
                     blink::IndexedDBKeyRange(), std::move(callback));
  }
  void OpenCursor(int64_t id,
                  blink::mojom::IDBDatabase::OpenCursorCallback callback) {
    database_->OpenCursor(
        id, 1, blink::IndexedDBIndexMetadata::kInvalidId,
        blink::IndexedDBKeyRange(), blink::mojom::IDBCursorDirection::Next,
        false, blink::mojom::IDBTaskType::Normal, std::move(callback));
  }
  void CommitAndWait(
      int64_t id,
      mojo::AssociatedRemote<blink::mojom::IDBTransaction>& transaction) {
    base::test::TestFuture<void> complete;
    expected_complete_ = id;
    on_complete_ = complete.GetCallback();
    transaction->Commit(0);
    ASSERT_TRUE(complete.Wait());
    expected_complete_.reset();
  }
  bool DatabaseConnected() {
    database_.FlushForTesting();
    return database_.is_connected();
  }
  bool AwaitDatabaseDisconnected() {
    if (!database_.is_connected()) {
      return true;
    }
    base::test::TestFuture<void> disconnected;
    database_.set_disconnect_handler(disconnected.GetCallback());
    return disconnected.Wait();
  }
  void ExpectRetirementAbort(int64_t transaction_id) {
    expected_abort_ = transaction_id;
  }
  void Error(blink::mojom::IDBException code,
             const std::u16string& message) override {
    ADD_FAILURE() << "IDB open error " << static_cast<int>(code);
    opened_.SetValue();
  }
  void Blocked(int64_t) override {
    ADD_FAILURE() << "IDB open blocked";
    opened_.SetValue();
  }
  void UpgradeNeeded(
      mojo::PendingAssociatedRemote<blink::mojom::IDBDatabase> database,
      int64_t,
      blink::mojom::IDBDataLoss,
      const std::string&,
      const blink::IndexedDBDatabaseMetadata&) override {
    database_.Bind(std::move(database));
    if (create_store_) {
      transaction_->CreateObjectStore(1, u"held_values",
                                      blink::IndexedDBKeyPath(), false);
    }
    transaction_->Commit(0);
  }
  void OpenSuccess(
      mojo::PendingAssociatedRemote<blink::mojom::IDBDatabase> database,
      const blink::IndexedDBDatabaseMetadata&) override {
    if (database) {
      database_.Bind(std::move(database));
    }
    opened_.SetValue();
  }
  void DeleteSuccess(int64_t) override { ADD_FAILURE() << "unexpected delete"; }
  void ForcedClose() override {}
  void VersionChange(int64_t, int64_t) override {}
  void Abort(int64_t transaction_id,
             blink::mojom::IDBException code,
             const std::u16string&) override {
    if (expected_abort_ == transaction_id) {
      EXPECT_NE(code, blink::mojom::IDBException::kNoError);
      return;
    }
    ADD_FAILURE() << "IDB transaction aborted";
  }
  void Complete(int64_t transaction_id) override {
    if (expected_complete_ == transaction_id && on_complete_) {
      std::move(on_complete_).Run();
    }
  }

 private:
  bool create_store_ = false;
  std::optional<int64_t> expected_abort_;
  std::optional<int64_t> expected_complete_;
  base::OnceClosure on_complete_;
  base::test::TestFuture<void> opened_;
  mojo::AssociatedReceiver<blink::mojom::IDBFactoryClient> client_{this};
  mojo::AssociatedReceiver<blink::mojom::IDBDatabaseCallbacks> callbacks_{this};
  mojo::AssociatedRemote<blink::mojom::IDBDatabase> database_;
  mojo::AssociatedRemote<blink::mojom::IDBTransaction> transaction_;
};

class NativeServiceWorkerBodyRead final : public mojo::DataPipeDrainer::Client {
 public:
  std::string Read(storage::mojom::ServiceWorkerResourceReader& reader,
                   int64_t size) {
    base::test::TestFuture<mojo::ScopedDataPipeConsumerHandle> prepared;
    reader.PrepareReadData(size, prepared.GetCallback());
    auto pipe = prepared.Take();
    EXPECT_TRUE(pipe.is_valid());
    if (!pipe.is_valid()) {
      return {};
    }
    mojo::DataPipeDrainer drainer(this, std::move(pipe));
    base::test::TestFuture<int32_t> finished;
    reader.ReadData(finished.GetCallback());
    EXPECT_TRUE(completed_.Wait());
    EXPECT_EQ(finished.Get(), size);
    return bytes_;
  }
  void OnDataAvailable(base::span<const uint8_t> bytes) override {
    bytes_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  void OnDataComplete() override { completed_.SetValue(); }

 private:
  std::string bytes_;
  base::test::TestFuture<void> completed_;
};

class NativeMemoryBlobSource final : public storage::mojom::BlobDataItemReader {
 public:
  NativeMemoryBlobSource(std::string bytes, std::string side_data)
      : bytes_(std::move(bytes)), side_data_(std::move(side_data)) {}
  void Read(uint64_t offset,
            uint64_t length,
            mojo::ScopedDataPipeProducerHandle pipe,
            ReadCallback callback) override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      std::move(callback).Run(net::ERR_FAILED);
      return;
    }
    const bool copied = mojo::BlockingCopyFromString(
        bytes_.substr(static_cast<size_t>(offset), static_cast<size_t>(length)),
        pipe);
    std::move(callback).Run(copied ? net::OK : net::ERR_FAILED);
  }
  void ReadSideData(ReadSideDataCallback callback) override {
    // The native DataHandle consumer expects a byte count, as CacheStorage's
    // reader supplies, despite the old Mojom comment describing a net status.
    std::move(callback).Run(
        static_cast<int>(side_data_.size()),
        mojo_base::BigBuffer(base::as_byte_span(side_data_)));
  }

 private:
  const std::string bytes_;
  const std::string side_data_;
};

class NativeDeferredBlobSource final
    : public storage::mojom::BlobDataItemReader {
 public:
  explicit NativeDeferredBlobSource(std::string bytes)
      : bytes_(std::move(bytes)) {}
  mojo::PendingRemote<storage::mojom::BlobDataItemReader> BindReader() {
    mojo::PendingRemote<storage::mojom::BlobDataItemReader> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void DeferNextRead(base::OnceClosure arrived) {
    CHECK(!arrived_ && !pending_);
    arrived_ = std::move(arrived);
  }
  void CompletePendingRead() {
    CHECK(pending_);
    auto read = std::move(*pending_);
    pending_.reset();
    Complete(std::move(read));
  }
  void PublishPendingPrefix(size_t count) {
    CHECK(pending_ && count > 0 && count < pending_->length);
    CHECK(mojo::BlockingCopyFromString(
        bytes_.substr(static_cast<size_t>(pending_->offset), count),
        pending_->pipe));
    pending_->offset += count;
    pending_->length -= count;
  }
  void Read(uint64_t offset,
            uint64_t length,
            mojo::ScopedDataPipeProducerHandle pipe,
            ReadCallback callback) override {
    Pending read{offset, length, std::move(pipe), std::move(callback)};
    if (arrived_) {
      CHECK(!pending_);
      pending_.emplace(std::move(read));
      std::move(arrived_).Run();
      return;
    }
    Complete(std::move(read));
  }
  void ReadSideData(ReadSideDataCallback callback) override {
    std::move(callback).Run(0, mojo_base::BigBuffer());
  }

 private:
  struct Pending {
    uint64_t offset;
    uint64_t length;
    mojo::ScopedDataPipeProducerHandle pipe;
    ReadCallback callback;
  };
  void Complete(Pending read) {
    if (read.offset > bytes_.size() ||
        read.length > bytes_.size() - read.offset) {
      std::move(read.callback).Run(net::ERR_FAILED);
      return;
    }
    const bool copied = mojo::BlockingCopyFromString(
        bytes_.substr(static_cast<size_t>(read.offset),
                      static_cast<size_t>(read.length)),
        read.pipe);
    read.pipe.reset();
    std::move(read.callback).Run(copied ? net::OK : net::ERR_ABORTED);
  }
  const std::string bytes_;
  base::OnceClosure arrived_;
  std::optional<Pending> pending_;
  mojo::ReceiverSet<storage::mojom::BlobDataItemReader> receivers_;
};

class NativeBlobRead final : public mojo::DataPipeDrainer::Client {
 public:
  enum class Delivery { kAll, kRange, kGetter };
  std::pair<int32_t, std::string> ReadGetter(
      network::mojom::DataPipeGetter& getter) {
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    base::test::TestFuture<int32_t, uint64_t> status;
    getter.Read(std::move(producer), status.GetCallback());
    mojo::DataPipeDrainer drainer(this, std::move(consumer));
    EXPECT_TRUE(completed_.Wait());
    const auto result = status.Take();
    if (std::get<0>(result) == net::OK)
      EXPECT_EQ(std::get<1>(result), bytes_.size());
    return {std::get<0>(result), bytes_};
  }
  std::string Read(blink::mojom::Blob& blob) {
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    blob.ReadAll(std::move(producer), mojo::NullRemote());
    mojo::DataPipeDrainer drainer(this, std::move(consumer));
    EXPECT_TRUE(completed_.Wait());
    return bytes_;
  }
  std::string ReadAtBoundary(blink::mojom::Blob& blob,
                             base::OnceClosure boundary,
                             Delivery delivery = Delivery::kAll,
                             uint64_t range_length = 0) {
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    mojo::Remote<network::mojom::DataPipeGetter> getter;
    base::test::TestFuture<int32_t, uint64_t> getter_reply;
    if (delivery == Delivery::kGetter) {
      blob.AsDataPipeGetter(getter.BindNewPipeAndPassReceiver());
      getter->Read(std::move(producer), getter_reply.GetCallback());
    } else if (delivery == Delivery::kRange) {
      blob.ReadRange(1, range_length, std::move(producer), mojo::NullRemote());
    } else {
      blob.ReadAll(std::move(producer), mojo::NullRemote());
    }
    mojo::DataPipeDrainer drainer(this, std::move(consumer));
    std::move(boundary).Run();
    EXPECT_TRUE(completed_.Wait());
    if (delivery == Delivery::kGetter) {
      EXPECT_EQ(getter_reply.Get<0>(), net::OK);
    }
    return bytes_;
  }
  std::string Drain(mojo::ScopedDataPipeConsumerHandle consumer) {
    mojo::DataPipeDrainer drainer(this, std::move(consumer));
    EXPECT_TRUE(completed_.Wait());
    return bytes_;
  }
  void OnDataAvailable(base::span<const uint8_t> bytes) override {
    bytes_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  void OnDataComplete() override { completed_.SetValue(); }

 private:
  std::string bytes_;
  base::test::TestFuture<void> completed_;
};

struct NativeStreamEligibilityResult {
  bool scoped_shape;
  bool ordinary_shape;
  bool scoped_direct;
  bool ordinary_direct;
  int scoped_result;
};

class NativeStreamEligibility {
 public:
  NativeStreamEligibility(
      RecordCapability capability,
      mojo::PendingRemote<storage::mojom::BlobDataItemReader> scoped_source,
      mojo::PendingRemote<storage::mojom::BlobDataItemReader> ordinary_source,
      uint64_t size)
      : scoped_context_(base::FilePath(),
                        base::FilePath(),
                        nullptr,
                        base::BindRepeating(
                            [](RecordCapability capability) {
                              return capability.Allows(
                                  StorePurpose::kBlobSpill,
                                  StorageAccess::kReadMemory);
                            },
                            std::move(capability))) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    auto create =
        [size](storage::BlobStorageContext& context,
               mojo::PendingRemote<storage::mojom::BlobDataItemReader> source,
               const char* uuid) {
          auto item = storage::mojom::BlobDataItem::New();
          item->type = storage::mojom::BlobDataItemType::kIndexedDB;
          item->size = size;
          item->reader = std::move(source);
          auto builder = std::make_unique<storage::BlobDataBuilder>(uuid);
          builder->AppendMojoDataItem(std::move(item));
          return context.AddFinishedBlob(std::move(builder));
        };
    scoped_ =
        create(scoped_context_, std::move(scoped_source), "scoped-direct-test");
    ordinary_ = create(ordinary_context_, std::move(ordinary_source),
                       "ordinary-direct-test");
  }
  NativeStreamEligibilityResult Probe() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    auto scoped = scoped_->CreateReader(), ordinary = ordinary_->CreateReader();
    CHECK(scoped->CalculateSize(base::DoNothing()) ==
          storage::BlobReader::Status::DONE);
    CHECK(ordinary->CalculateSize(base::DoNothing()) ==
          storage::BlobReader::Status::DONE);
    auto result =
        base::MakeRefCounted<base::RefCountedData<int>>(net::ERR_IO_PENDING);
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    scoped->ReadSingleMojoDataItem(
        std::move(producer),
        base::BindOnce([](scoped_refptr<base::RefCountedData<int>> result,
                          int status) { result->data = status; },
                       result));
    return {scoped->IsSingleMojoDataItem(), ordinary->IsSingleMojoDataItem(),
            scoped->CanReadSingleMojoDataItemDirectly(),
            ordinary->CanReadSingleMojoDataItemDirectly(), result->data};
  }

 private:
  storage::BlobStorageContext scoped_context_;
  storage::BlobStorageContext ordinary_context_;
  std::unique_ptr<storage::BlobDataHandle> scoped_, ordinary_;
};

struct NativeBlobReadSample {
  storage::BlobReader::Status status;
  int error;
  int bytes;
  std::string value;
};

struct NativePrimedBlobResults {
  NativeBlobReadSample scoped;
  NativeBlobReadSample ordinary;
  storage::BlobReader::Status late_size;
  int late_size_error;
};

class NativePrimedBlobReaders {
 public:
  explicit NativePrimedBlobReaders(RecordCapability capability) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    scoped_context_ = std::make_unique<storage::BlobStorageContext>(
        base::FilePath(), base::FilePath(), nullptr,
        base::BindRepeating(
            [](RecordCapability captured) {
              return captured.Allows(StorePurpose::kBlobSpill,
                                     StorageAccess::kReadMemory);
            },
            std::move(capability)));
    ordinary_context_ = std::make_unique<storage::BlobStorageContext>();
  }
  NativePrimedBlobResults Prime() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    auto scoped =
        std::make_unique<storage::BlobDataBuilder>("scoped-primed-reader");
    scoped->AppendData("alpha-primed-reader");
    scoped_handle_ = scoped_context_->AddFinishedBlob(std::move(scoped));
    auto ordinary =
        std::make_unique<storage::BlobDataBuilder>("ordinary-primed-reader");
    ordinary->AppendData("plain-primed-reader");
    ordinary_handle_ = ordinary_context_->AddFinishedBlob(std::move(ordinary));
    scoped_reader_ = scoped_handle_->CreateReader();
    late_reader_ = scoped_handle_->CreateReader();
    ordinary_reader_ = ordinary_handle_->CreateReader();
    CHECK(scoped_reader_->CalculateSize(base::DoNothing()) ==
          storage::BlobReader::Status::DONE);
    CHECK(ordinary_reader_->CalculateSize(base::DoNothing()) ==
          storage::BlobReader::Status::DONE);
    NativePrimedBlobResults result{Read(*scoped_reader_, 5),
                                   Read(*ordinary_reader_, 5),
                                   storage::BlobReader::Status::DONE, net::OK};
    // Ordinary snapshots intentionally keep their backing resources after the
    // native registry/context dies. Scope admission must not break that case.
    ordinary_context_.reset();
    return result;
  }
  NativePrimedBlobResults AfterRetirement() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    auto scoped = Read(*scoped_reader_, 64);
    auto ordinary = Read(*ordinary_reader_, 64);
    const auto late = late_reader_->CalculateSize(base::DoNothing());
    return {std::move(scoped), std::move(ordinary), late,
            late_reader_->net_error()};
  }

 private:
  static NativeBlobReadSample Read(storage::BlobReader& reader, int capacity) {
    auto buffer = base::MakeRefCounted<net::IOBufferWithSize>(capacity);
    int bytes = 0;
    const auto status =
        reader.Read(buffer.get(), capacity, &bytes, base::DoNothing());
    CHECK(status != storage::BlobReader::Status::IO_PENDING);
    CHECK_GE(bytes, 0);
    CHECK_LE(bytes, capacity);
    return {status, reader.net_error(), bytes,
            std::string(buffer->data(), bytes)};
  }
  std::unique_ptr<storage::BlobStorageContext> scoped_context_,
      ordinary_context_;
  std::unique_ptr<storage::BlobDataHandle> scoped_handle_, ordinary_handle_;
  std::unique_ptr<storage::BlobReader> scoped_reader_, ordinary_reader_,
      late_reader_;
};

class NativeEmbeddedStateTest : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    child_ = TestingProfile::Builder().BuildOffTheRecord(
        profile(),
        Profile::OTRProfileID::CreateUniqueForAlloy(ContextMode::Memory()));
    ASSERT_TRUE(RequestContextPolicy::IsNoDisk(*child_));
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*child_, scope()));
    InstallPolicy();
    first_ = content::WebContentsTester::CreateTestWebContents(child_, nullptr);
    second_ =
        content::WebContentsTester::CreateTestWebContents(child_, nullptr);
    Navigate(first_.get(), "https://outer.test/page");
    Navigate(second_.get(), "https://outer.test/second");
  }
  void TearDown() override {
    second_.reset();
    first_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

 protected:
  virtual Scope scope() const { return Scope::kPerTab; }
  virtual void InstallPolicy() {}
  mojo::Remote<blink::mojom::BucketManagerHost> Buckets(
      content::RenderFrameHost* target) {
    auto* frame = static_cast<content::RenderFrameHostImpl*>(target);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        frame->GetStoragePartition());
    mojo::Remote<blink::mojom::BucketManagerHost> manager;
    partition->GetBucketManager()->BindReceiver(
        frame->GetWeakPtr(), manager.BindNewPipeAndPassReceiver(),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return manager;
  }
  mojo::Remote<blink::mojom::BucketHost> CreateBucket(
      mojo::Remote<blink::mojom::BucketManagerHost>& manager,
      const char* name) {
    base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                           blink::mojom::BucketError>
        opened;
    manager->OpenBucket(name, blink::mojom::BucketPolicies::New(),
                        opened.GetCallback());
    auto [pending, error] = opened.Take();
    EXPECT_TRUE(pending.is_valid());
    return mojo::Remote<blink::mojom::BucketHost>(std::move(pending));
  }
  content::DOMStorageContextWrapper* Dom(content::RenderFrameHost* frame) {
    return static_cast<content::DOMStorageContextWrapper*>(
        frame->GetStoragePartition()->GetDOMStorageContext());
  }
  mojo::Remote<blink::mojom::StorageArea> OpenLocalArea(
      content::RenderFrameHost* frame) {
    mojo::Remote<blink::mojom::StorageArea> area;
    Dom(frame)->OpenLocalStorage(
        frame->GetStorageKey(), frame->GetFrameToken(),
        area.BindNewPipeAndPassReceiver(),
        content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
            frame->GetProcess()->GetID()),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return area;
  }
  mojo::Remote<blink::mojom::StorageArea> OpenSession(
      content::RenderFrameHost* frame,
      const std::string& ns) {
    mojo::Remote<blink::mojom::StorageArea> area;
    Dom(frame)->BindStorageArea(
        frame->GetStorageKey(), frame->GetFrameToken(), ns,
        area.BindNewPipeAndPassReceiver(),
        content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
            frame->GetProcess()->GetID()),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return area;
  }
  std::vector<uint8_t> SessionValue(
      mojo::Remote<blink::mojom::StorageArea>& area) {
    base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> read;
    area->GetAll(mojo::NullRemote(), read.GetCallback());
    auto values = read.Take();
    if (values.empty()) {
      return {};
    }
    EXPECT_EQ(values.size(), 1u);
    return values[0]->value;
  }
  void CheckCrossTabSessionClone(bool shared) {
    auto* a = Embed(first_.get());
    auto* b = Embed(second_.get());
    auto* control = Dom(a)->GetSessionStorageControl();
    const auto source = base::Uuid::GenerateRandomV4().AsLowercaseString();
    const auto target = base::Uuid::GenerateRandomV4().AsLowercaseString();
    control->CreateNamespace(source);
    auto area = OpenSession(a, source);
    base::test::TestFuture<bool> written;
    area->Put({1}, {8}, std::nullopt, nullptr, written.GetCallback());
    ASSERT_TRUE(written.Get());
    control->CloneNamespace(
        source, target, storage::mojom::SessionStorageCloneType::kImmediate);
    auto clone = OpenSession(b, target);
    EXPECT_EQ(SessionValue(clone),
              shared ? std::vector<uint8_t>{8} : std::vector<uint8_t>{});
    auto same_scope = OpenSession(a, target);
    EXPECT_EQ(SessionValue(same_scope), (std::vector<uint8_t>{8}));
    control->DeleteNamespace(source, false);
    control->DeleteNamespace(target, false);
    same_scope.FlushForTesting();
    EXPECT_FALSE(same_scope.is_connected());
  }
  void Navigate(content::WebContents* contents, const char* url) {
    content::WebContentsTester::For(contents)->NavigateAndCommit(GURL(url));
  }
  content::RenderFrameHost* Embed(content::WebContents* contents,
                                  const char* url = "https://embedded.test/") {
    auto* initial =
        content::RenderFrameHostTester::For(contents->GetPrimaryMainFrame())
            ->AppendChild("embedded");
    auto* committed =
        content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                    initial);
    // A throttle can cancel Commit() and leave the initial inherited document.
    // Never use that document as a successful native storage principal.
    CHECK(committed);
    CHECK(committed->GetLastCommittedURL() == GURL(url))
        << "test iframe navigation did not commit " << url;
    CHECK(committed->GetLastCommittedOrigin() == url::Origin::Create(GURL(url)))
        << "test iframe origin did not commit " << url;
    return committed;
  }
  raw_ptr<TestingProfile> child_ = nullptr;
  std::unique_ptr<content::WebContents> first_;
  std::unique_ptr<content::WebContents> second_;
};

TEST_F(NativeEmbeddedStateTest, NativeStorageKeysAndScopesSeparateTabs) {
  const auto origin = url::Origin::Create(GURL("https://embedded.test/"));
  EXPECT_EQ(content::GetAlloyEmbeddedStateForCandidate(nullptr, origin).backing,
            Backing::kNone);
  EXPECT_FALSE(content::GetAlloyEmbeddedNonceForCandidate(nullptr, origin));
  auto* first = Embed(first_.get());
  auto* second = Embed(second_.get());
  const auto a = content::GetAlloyEmbeddedStateForDocument(first);
  const auto b = content::GetAlloyEmbeddedStateForDocument(second);
  ASSERT_EQ(a.backing, Backing::kTemporary);
  ASSERT_EQ(b.backing, Backing::kTemporary);
  ASSERT_TRUE(a.temporary);
  ASSERT_TRUE(b.temporary);
  EXPECT_NE(a.temporary, b.temporary);
  const auto& a_key = first->GetStorageKey();
  const auto& b_key = second->GetStorageKey();
  ASSERT_TRUE(a_key.nonce());
  ASSERT_TRUE(b_key.nonce());
  EXPECT_NE(*a_key.nonce(), *b_key.nonce());
  EXPECT_EQ(*a_key.nonce(),
            a.temporary->Capability(StorePurpose::kDomStorage).generation());
  EXPECT_FALSE(first_->GetPrimaryMainFrame()->GetStorageKey().nonce());
  EXPECT_EQ(
      content::GetAlloyEmbeddedStateForDocument(first_->GetPrimaryMainFrame())
          .backing,
      Backing::kRealSite);
}

TEST_F(NativeEmbeddedStateTest, SameSiteReloadKeepsScopeMoveAndBackDoNot) {
  auto* frame = Embed(first_.get());
  auto state = content::GetAlloyEmbeddedStateForDocument(frame).temporary;
  ASSERT_TRUE(state);
  auto old = state->Capability(StorePurpose::kDomStorage);
  Navigate(first_.get(), "https://sub.outer.test/reload");
  EXPECT_TRUE(old.active());
  EXPECT_EQ(
      state,
      content::GetAlloyEmbeddedStateForDocument(Embed(first_.get())).temporary);
  Navigate(first_.get(), "https://different.test/");
  EXPECT_FALSE(old.active());
  Navigate(first_.get(), "https://outer.test/back");
  auto fresh = content::GetAlloyEmbeddedStateForDocument(Embed(first_.get()));
  ASSERT_TRUE(fresh.temporary);
  EXPECT_NE(
      old.generation(),
      fresh.temporary->Capability(StorePurpose::kDomStorage).generation());
  first_.reset();
  EXPECT_FALSE(fresh.temporary->Capability(StorePurpose::kDomStorage).active());
}

TEST_F(NativeEmbeddedStateTest, SharedContextChoiceIsFrozenAfterAdmission) {
  EXPECT_FALSE(
      content::ConfigureAlloyEmbeddedStateScope(*child_, Scope::kPerOuterSite));
  auto* first = Embed(first_.get());
  auto* second = Embed(second_.get());
  EXPECT_NE(content::GetAlloyEmbeddedStateForDocument(first).temporary,
            content::GetAlloyEmbeddedStateForDocument(second).temporary);
}

class NativeOuterEmbeddedStateTest : public NativeEmbeddedStateTest {
 protected:
  Scope scope() const override { return Scope::kPerOuterSite; }
};

TEST_F(NativeOuterEmbeddedStateTest, NativeTabsOfOuterSiteShareMemoryOwner) {
  auto* first = Embed(first_.get());
  auto* second = Embed(second_.get());
  const auto a = content::GetAlloyEmbeddedStateForDocument(first);
  const auto b = content::GetAlloyEmbeddedStateForDocument(second);
  ASSERT_TRUE(a.temporary);
  EXPECT_EQ(a.temporary, b.temporary);
  EXPECT_EQ(first->GetStorageKey(), second->GetStorageKey());
  first_.reset();
  EXPECT_TRUE(b.temporary->Capability(StorePurpose::kDomStorage).active());
  content::ShutdownAlloyEmbeddedState(*child_);
  EXPECT_FALSE(b.temporary->Capability(StorePurpose::kDomStorage).active());
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(second).backing,
            Backing::kNone);
}

TEST_F(NativeOuterEmbeddedStateTest,
       EmbeddedSiteRetirementPreservesSiblingStores) {
  auto* removed = Embed(first_.get(), "https://alpha.test/");
  auto* sibling = Embed(first_.get(), "https://beta.test/");
  auto* peer = Embed(second_.get(), "https://beta.test/");
  auto removed_owner = content::GetAlloyEmbeddedStateForDocument(removed);
  auto sibling_owner = content::GetAlloyEmbeddedStateForDocument(sibling);
  ASSERT_TRUE(removed_owner.temporary);
  ASSERT_TRUE(sibling_owner.temporary);
  auto open = [&](content::RenderFrameHost* target) {
    mojo::Remote<blink::mojom::StorageArea> area;
    Dom(target)->OpenLocalStorage(
        target->GetStorageKey(), target->GetFrameToken(),
        area.BindNewPipeAndPassReceiver(),
        content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
            target->GetProcess()->GetID()),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return area;
  };
  auto removed_area = open(removed);
  auto sibling_area = open(sibling);
  auto peer_area = open(peer);
  base::test::TestFuture<bool> removed_write, sibling_write;
  removed_area->Put({1}, {41}, std::nullopt, nullptr,
                    removed_write.GetCallback());
  sibling_area->Put({1}, {42}, std::nullopt, nullptr,
                    sibling_write.GetCallback());
  ASSERT_TRUE(removed_write.Get());
  ASSERT_TRUE(sibling_write.Get());
  ASSERT_EQ(SessionValue(removed_area), (std::vector<uint8_t>{41}));
  ASSERT_EQ(SessionValue(sibling_area), (std::vector<uint8_t>{42}));
  ASSERT_EQ(SessionValue(peer_area), (std::vector<uint8_t>{42}));
  auto removed_manager = Buckets(removed);
  auto sibling_manager = Buckets(sibling);
  auto removed_bucket = CreateBucket(removed_manager, "owned_bucket");
  auto sibling_bucket = CreateBucket(sibling_manager, "owned_bucket");

  // This is the selected site's backend owner, not a tab/context clear. Every
  // unrelated site in the same logical outer jar must retain its authority.
  removed_owner.temporary->Retire();
  removed_area.FlushForTesting();
  removed_bucket.FlushForTesting();
  EXPECT_FALSE(removed_area.is_connected());
  EXPECT_FALSE(removed_bucket.is_connected());
  EXPECT_FALSE(
      removed_owner.temporary->Capability(StorePurpose::kDomStorage).active());
  EXPECT_TRUE(
      sibling_owner.temporary->Capability(StorePurpose::kDomStorage).active());
  sibling_area.FlushForTesting();
  peer_area.FlushForTesting();
  sibling_bucket.FlushForTesting();
  ASSERT_TRUE(sibling_area.is_connected())
      << "unrelated site lost its native area";
  ASSERT_TRUE(peer_area.is_connected())
      << "unrelated outer-site peer lost its native area";
  ASSERT_TRUE(sibling_bucket.is_connected())
      << "unrelated site lost its native bucket";
  EXPECT_EQ(SessionValue(sibling_area), (std::vector<uint8_t>{42}));
  EXPECT_EQ(SessionValue(peer_area), (std::vector<uint8_t>{42}));
  EXPECT_TRUE(first_);
  EXPECT_TRUE(second_);
}

TEST_F(NativeEmbeddedStateTest,
       SiteRevocationClosesMatchingAreasAndKeepsSiblings) {
  // Keep HTTP coverage without asking an HTTPS outer to run blocked mixed
  // content. The two native scopes still exercise R16 across schemes/origins.
  Navigate(second_.get(), "http://outer.test/second");
  ASSERT_EQ(second_->GetPrimaryMainFrame()->GetLastCommittedURL(),
            GURL("http://outer.test/second"));
  auto* alpha_a = Embed(first_.get(), "https://alpha.test/");
  auto* alpha_b = Embed(second_.get(), "http://sub.alpha.test/");
  ASSERT_TRUE(alpha_a->GetStorageKey().nonce());
  ASSERT_TRUE(alpha_b->GetStorageKey().nonce());
  auto* beta_a = Embed(first_.get(), "https://beta.test/");
  auto* beta_b = Embed(second_.get(), "https://beta.test/");
  auto a = OpenLocalArea(alpha_a), b = OpenLocalArea(alpha_b);
  auto sibling_a = OpenLocalArea(beta_a), sibling_b = OpenLocalArea(beta_b);
  base::test::TestFuture<bool> write_a, write_b, write_sibling_a,
      write_sibling_b;
  a->Put({1}, {51}, std::nullopt, nullptr, write_a.GetCallback());
  b->Put({1}, {52}, std::nullopt, nullptr, write_b.GetCallback());
  sibling_a->Put({1}, {53}, std::nullopt, nullptr,
                 write_sibling_a.GetCallback());
  sibling_b->Put({1}, {54}, std::nullopt, nullptr,
                 write_sibling_b.GetCallback());
  ASSERT_TRUE(write_a.Get());
  ASSERT_TRUE(write_b.Get());
  ASSERT_TRUE(write_sibling_a.Get());
  ASSERT_TRUE(write_sibling_b.Get());
  ASSERT_EQ(SessionValue(a), (std::vector<uint8_t>{51}));
  ASSERT_EQ(SessionValue(b), (std::vector<uint8_t>{52}));
  ASSERT_EQ(SessionValue(sibling_a), (std::vector<uint8_t>{53}));
  ASSERT_EQ(SessionValue(sibling_b), (std::vector<uint8_t>{54}));
  const auto old_generation =
      content::GetAlloyEmbeddedStateForDocument(alpha_a)
          .temporary->Capability(StorePurpose::kDomStorage)
          .generation();
  content::RevokeAlloyEmbeddedSite(
      *child_, GURL("https://different.alpha.test/"),
      url::Origin::Create(GURL("https://different.alpha.test/")));
  a.FlushForTesting();
  b.FlushForTesting();
  EXPECT_FALSE(a.is_connected());
  EXPECT_FALSE(b.is_connected());
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(alpha_a).backing,
            Backing::kNone);
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(alpha_b).backing,
            Backing::kNone);
  sibling_a.FlushForTesting();
  sibling_b.FlushForTesting();
  ASSERT_TRUE(sibling_a.is_connected());
  ASSERT_TRUE(sibling_b.is_connected());
  EXPECT_EQ(SessionValue(sibling_a), (std::vector<uint8_t>{53}));
  EXPECT_EQ(SessionValue(sibling_b), (std::vector<uint8_t>{54}));
  auto* fresh_frame = Embed(first_.get(), "https://alpha.test/fresh");
  auto fresh_owner = content::GetAlloyEmbeddedStateForDocument(fresh_frame);
  ASSERT_TRUE(fresh_owner.temporary);
  EXPECT_NE(
      fresh_owner.temporary->Capability(StorePurpose::kDomStorage).generation(),
      old_generation);
  auto fresh = OpenLocalArea(fresh_frame);
  EXPECT_TRUE(SessionValue(fresh).empty());
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(alpha_a).backing,
            Backing::kNone);
}

TEST_F(NativeOuterEmbeddedStateTest,
       RetiredDocumentCannotAdoptReplacementSiteGeneration) {
  auto* old = Embed(first_.get(), "https://alpha.test/");
  ASSERT_TRUE(old->GetStorageKey().nonce());
  const auto old_nonce = *old->GetStorageKey().nonce();
  // No data binding to this document is needed before deletion. Late first use
  // must not borrow the new site's generation from the still-live outer jar.
  content::RevokeAlloyEmbeddedSite(
      *child_, GURL("https://alpha.test/"),
      url::Origin::Create(GURL("https://alpha.test/")));
  auto* fresh = Embed(second_.get(), "https://alpha.test/fresh");
  auto owner = content::GetAlloyEmbeddedStateForDocument(fresh);
  ASSERT_TRUE(owner.temporary);
  EXPECT_NE(
      old_nonce,
      owner.temporary->Capability(StorePurpose::kDomStorage).generation());
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(old).backing,
            Backing::kNone);
  auto area = OpenLocalArea(fresh);
  base::test::TestFuture<bool> written;
  area->Put({1}, {55}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  EXPECT_EQ(SessionValue(area), (std::vector<uint8_t>{55}));
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(old).backing,
            Backing::kNone);
}

class NativeListedEmbeddedStateTest : public NativeEmbeddedStateTest {
 protected:
  scoped_refptr<base::RefCountedData<bool>> listed_ =
      base::MakeRefCounted<base::RefCountedData<bool>>(true);
  void InstallPolicy() override {
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedFirstPartyPolicy(
        *child_, EmbeddedFirstPartyPolicy::Bind(base::BindRepeating(
                     [](scoped_refptr<base::RefCountedData<bool>> listed,
                        const GURL& outer, const GURL& embedded) {
                       return listed->data && outer.host() == "outer.test" &&
                              embedded.host() == "embedded.test";
                     },
                     listed_))));
  }
};

TEST_F(NativeListedEmbeddedStateTest, ListedFrameSelectsItsOwnFirstPartyKey) {
  auto* listed = Embed(first_.get());
  const auto selection = content::GetAlloyEmbeddedStateForDocument(listed);
  EXPECT_EQ(selection.backing, Backing::kRealSite);
  EXPECT_TRUE(selection.considered_first_party);
  EXPECT_FALSE(listed->GetStorageKey().nonce());
  EXPECT_EQ(listed->GetStorageKey(), blink::StorageKey::CreateFirstParty(
                                         listed->GetLastCommittedOrigin()));
  EXPECT_NE(listed->GetStorageKey(),
            first_->GetPrimaryMainFrame()->GetStorageKey());
  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(
                Embed(second_.get(), "https://unlisted.test/"))
                .backing,
            Backing::kTemporary);
}

TEST_F(NativeListedEmbeddedStateTest,
       RetiredTemporaryDocumentCannotAcquireNewlyListedRealStore) {
  listed_->data = false;
  auto* old = Embed(first_.get());
  ASSERT_TRUE(old->GetStorageKey().nonce());
  const auto old_key = old->GetStorageKey();
  // The navigation selected a temporary principal. Before its first data bind,
  // revoke that site and change the explicit native policy projection.
  listed_->data = true;
  content::RevokeAlloyEmbeddedSite(
      *child_, GURL("https://embedded.test/"),
      url::Origin::Create(GURL("https://embedded.test/")));
  // A legitimate new top-level visit creates the replacement real values. The
  // retired embedded document must not acquire that new generation's store.
  auto real_contents =
      content::WebContentsTester::CreateTestWebContents(child_, nullptr);
  Navigate(real_contents.get(), "https://embedded.test/");
  auto* real_frame = real_contents->GetPrimaryMainFrame();
  ASSERT_FALSE(real_frame->GetStorageKey().nonce());
  auto real = OpenLocalArea(real_frame);
  base::test::TestFuture<bool> real_written;
  real->Put({1}, {71}, std::nullopt, nullptr, real_written.GetCallback());
  ASSERT_TRUE(real_written.Get());
  ASSERT_EQ(SessionValue(real), (std::vector<uint8_t>{71}));

  EXPECT_EQ(content::GetAlloyEmbeddedStateForDocument(old).backing,
            Backing::kNone)
      << "retired temporary document reclassified as real";
  auto forbidden = OpenLocalArea(old);
  base::test::TestFuture<bool> stale_write;
  forbidden->Put({1}, {72}, std::nullopt, nullptr,
                 mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                     stale_write.GetCallback(), false));
  EXPECT_FALSE(stale_write.Get())
      << "stale nonced document wrote real controller";
  forbidden.FlushForTesting();
  EXPECT_FALSE(forbidden.is_connected());

  // The new explicit list still admits a fresh document to its own real data.
  auto* fresh = Embed(second_.get());
  ASSERT_FALSE(fresh->GetStorageKey().nonce());
  const auto selected = content::GetAlloyEmbeddedStateForDocument(fresh);
  ASSERT_EQ(selected.backing, Backing::kRealSite);
  ASSERT_TRUE(selected.considered_first_party);
  auto allowed = OpenLocalArea(fresh);
  ASSERT_EQ(SessionValue(allowed), (std::vector<uint8_t>{71}));
  base::test::TestFuture<bool> updated;
  allowed->Put({1}, {73}, std::nullopt, nullptr, updated.GetCallback());
  ASSERT_TRUE(updated.Get());
  EXPECT_EQ(SessionValue(real), (std::vector<uint8_t>{73}));
  auto* dom = Dom(real_frame);
  dom->GetLocalStorageControl()->Flush();
  base::test::TestFuture<std::vector<storage::mojom::StorageUsageInfoPtr>>
      usage;
  dom->GetLocalStorageControl()->GetUsage(usage.GetCallback());
  auto entries = usage.Take();
  EXPECT_TRUE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == real_frame->GetStorageKey();
  }));
  EXPECT_FALSE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == old_key;
  })) << "retired temporary principal entered the real native catalogue";
}

TEST_F(NativeEmbeddedStateTest, LocalStorageEndpointEndsWithItsTabScope) {
  auto* frame = Embed(first_.get());
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto* dom = static_cast<content::DOMStorageContextWrapper*>(
      partition->GetDOMStorageContext());
  mojo::Remote<blink::mojom::StorageArea> area;
  dom->OpenLocalStorage(
      frame->GetStorageKey(), frame->GetFrameToken(),
      area.BindNewPipeAndPassReceiver(),
      content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
          frame->GetProcess()->GetID()),
      base::BindOnce([](std::string_view reason) { ADD_FAILURE() << reason; }));
  base::test::TestFuture<bool> written;
  area->Put({1}, {2, 3, 4}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> contents;
  area->GetAll(mojo::NullRemote(), contents.GetCallback());
  auto values = contents.Take();
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0]->value, (std::vector<uint8_t>{2, 3, 4}));
  first_.reset();
  area.FlushForTesting();
  EXPECT_FALSE(area.is_connected());
  // The context and sibling tab still live: retirement cannot be implemented
  // by relying on whole-context destruction or merely choosing a fresh key.
  EXPECT_TRUE(second_);
}

class NativeDiskEmbeddedLocalStorageTest
    : public ChromeRenderViewHostTestHarness {
 protected:
  void CheckEmbeddedNetworkOwnership(Scope scope);
};

void NativeDiskEmbeddedLocalStorageTest::CheckEmbeddedNetworkOwnership(
    Scope scope) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto second_tab =
      content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
  content::WebContentsTester::For(second_tab.get())
      ->NavigateAndCommit(GURL("https://outer.test/second"));
  auto append = [](content::WebContents* tab, const char* name,
                   const char* url) {
    auto* initial =
        content::RenderFrameHostTester::For(tab->GetPrimaryMainFrame())
            ->AppendChild(name);
    return static_cast<content::RenderFrameHostImpl*>(
        content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                    initial));
  };
  auto* alpha = append(web_contents(), "network-alpha", "https://alpha.test/");
  auto* beta = append(web_contents(), "network-beta", "https://beta.test/");
  auto* second_alpha =
      append(second_tab.get(), "network-second-alpha", "https://alpha.test/");
  auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
  auto peer_owner = content::GetAlloyEmbeddedStateForDocument(beta).temporary;
  auto second_owner =
      content::GetAlloyEmbeddedStateForDocument(second_alpha).temporary;
  ASSERT_TRUE(owner && peer_owner && second_owner);
  ASSERT_NE(owner, peer_owner);
  const bool shared = scope == Scope::kPerOuterSite;
  ASSERT_EQ(owner == second_owner, shared);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(alpha->GetStoragePartition());
  ASSERT_TRUE(partition->GetStoragePartitionPath());
  ASSERT_EQ(alpha->GetStoragePartition(), beta->GetStoragePartition());
  ASSERT_EQ(alpha->GetStoragePartition(), second_alpha->GetStoragePartition());
  auto first = content::GetAlloyNetworkGenerationForDocument(alpha);
  auto peer = content::GetAlloyNetworkGenerationForDocument(beta);
  auto second = content::GetAlloyNetworkGenerationForDocument(second_alpha);
  ASSERT_TRUE(first && peer && second)
      << "temporary embedded owners must select native network leases in a "
         "disk Profile";
  ASSERT_NE(first, peer);
  ASSERT_EQ(first == second, shared);
  auto* top = static_cast<content::RenderFrameHostImpl*>(
      web_contents()->GetPrimaryMainFrame());
  auto* top_context = content::GetAlloyNetworkContextForDocument(top);
  ASSERT_EQ(top_context, partition->GetNetworkContext());
  ASSERT_NE(first->GetNetworkContext(), top_context);
  ASSERT_EQ(content::GetAlloyNetworkContextForDocument(alpha),
            first->GetNetworkContext());
  ASSERT_EQ(content::GetAlloyNetworkContextForDocument(beta),
            peer->GetNetworkContext());
  ASSERT_EQ(content::GetAlloyNetworkContextForDocument(second_alpha),
            second->GetNetworkContext());
  ASSERT_TRUE(first->memory_only() && peer->memory_only() &&
              second->memory_only());
  auto cookies = [](network::mojom::NetworkContext* context) {
    mojo::Remote<network::mojom::CookieManager> remote;
    context->GetCookieManager(remote.BindNewPipeAndPassReceiver());
    return remote;
  };
  auto first_cookies = cookies(first->GetNetworkContext());
  auto peer_cookies = cookies(peer->GetNetworkContext());
  auto second_cookies = cookies(second->GetNetworkContext());
  auto real_cookies = cookies(top_context);
  auto cookie = net::CanonicalCookie::CreateForTesting(
      GURL("https://alpha.test/"), "scope_marker=alpha; Secure; SameSite=None",
      base::Time::Now(), net::CookieSourceType::kHTTP);
  ASSERT_TRUE(cookie);
  base::test::TestFuture<net::CookieAccessResult> written;
  first_cookies->SetCanonicalCookie(*cookie, GURL("https://alpha.test/"),
                                    net::CookieOptions::MakeAllInclusive(),
                                    written.GetCallback());
  ASSERT_TRUE(written.Get().status.IsInclude());
  auto has_marker = [](mojo::Remote<network::mojom::CookieManager>& remote,
                       std::string_view name = "scope_marker",
                       std::string_view contents = "alpha") {
    base::test::TestFuture<net::CookieList> read;
    remote->GetAllCookies(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        read.GetCallback<const net::CookieList&>(), net::CookieList()));
    return std::ranges::any_of(read.Get(), [&](const auto& value) {
      return value.Name() == name && value.Value() == contents;
    });
  };
  auto put_control = [](mojo::Remote<network::mojom::CookieManager>& remote,
                        const GURL& url, std::string_view line) {
    auto value = net::CanonicalCookie::CreateForTesting(
        url, line, base::Time::Now(), net::CookieSourceType::kHTTP);
    if (!value)
      return false;
    base::test::TestFuture<net::CookieAccessResult> reply;
    remote->SetCanonicalCookie(*value, url,
                               net::CookieOptions::MakeAllInclusive(),
                               reply.GetCallback());
    return reply.Get().status.IsInclude();
  };
  ASSERT_TRUE(put_control(peer_cookies, GURL("https://beta.test/"),
                          "peer_marker=beta; Secure; SameSite=None"));
  ASSERT_TRUE(put_control(real_cookies, GURL("https://outer.test/"),
                          "real_marker=top; Secure; SameSite=None"));
  ASSERT_TRUE(has_marker(first_cookies));
  ASSERT_EQ(has_marker(second_cookies), shared);
  ASSERT_FALSE(has_marker(peer_cookies));
  ASSERT_FALSE(has_marker(real_cookies));
  auto first_weak = first->GetWeakPtr();
  auto peer_weak = peer->GetWeakPtr();
  auto second_weak = second->GetWeakPtr();
  ASSERT_TRUE(first_weak && peer_weak && second_weak);
  RecordProperty("positive_embedded_network_owner",
                 shared ? "native-outer-site-shared-alpha-separate-beta-top"
                        : "native-tab-separated-alpha-beta-top");
  owner->Retire();
  ASSERT_TRUE(alpha->IsActive() && beta->IsActive() &&
              second_alpha->IsActive());
  EXPECT_FALSE(first_weak);
  EXPECT_FALSE(first->GetWeakPtr());
  first_cookies.FlushForTesting();
  EXPECT_FALSE(has_marker(first_cookies))
      << "a retained native CookieManager cannot expose retired scope bytes";
  EXPECT_TRUE(has_marker(peer_cookies, "peer_marker", "beta"));
  EXPECT_TRUE(has_marker(real_cookies, "real_marker", "top"));
  EXPECT_TRUE(peer_weak);
  EXPECT_EQ(static_cast<bool>(second_weak), !shared);
  EXPECT_FALSE(content::GetAlloyNetworkGenerationForDocument(alpha));
  EXPECT_TRUE(content::GetAlloyEmbeddedStateForDocument(alpha)
                  .temporary_admission_closed);
  EXPECT_NE(content::GetAlloyNetworkContextForDocument(alpha), top_context)
      << "revoked embedded authority must not fall through to the real jar";
  EXPECT_EQ(content::GetAlloyNetworkContextForDocument(beta),
            peer->GetNetworkContext());
  EXPECT_EQ(content::GetAlloyNetworkContextForDocument(top), top_context);
  if (!shared) {
    EXPECT_EQ(content::GetAlloyNetworkGenerationForDocument(second_alpha),
              second);
  }
  if (!HasFailure()) {
    RecordProperty("retired_embedded_network_owner",
                   shared ? "shared-alpha-lease-retired-beta-top-live"
                          : "alpha-retired-second-tab-beta-top-live");
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       PerTabEmbeddedNetworkOwnsItsNativeLease) {
  CheckEmbeddedNetworkOwnership(Scope::kPerTab);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       OuterSiteEmbeddedNetworkSharesItsNativeLease) {
  CheckEmbeddedNetworkOwnership(Scope::kPerOuterSite);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       UnusedSiteRevocationDoesNotFreezeScopeOption) {
  content::RevokeAlloyEmbeddedSite(
      *profile(), GURL("https://alpha.test/"),
      url::Origin::Create(GURL("https://alpha.test/")));
  EXPECT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       MemoryServiceWorkerContextEndsWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto append = [&](const char* name, const char* url) {
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild(name);
    return content::NavigationSimulator::NavigateAndCommitFromDocument(
        GURL(url), initial);
  };
  auto* alpha = append("alpha", "https://alpha.test/");
  auto* beta = append("beta", "https://beta.test/");
  ASSERT_EQ(alpha->GetLastCommittedURL(), GURL("https://alpha.test/"));
  ASSERT_EQ(beta->GetLastCommittedURL(), GURL("https://beta.test/"));
  auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
  auto peer = content::GetAlloyEmbeddedStateForDocument(beta).temporary;
  ASSERT_TRUE(owner);
  ASSERT_TRUE(peer);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(alpha->GetStoragePartition());
  ASSERT_TRUE(partition->GetStoragePartitionPath());
  auto* graph = content::GetAlloyEmbeddedBucketStorage(*partition, owner);
  auto* peer_graph = content::GetAlloyEmbeddedBucketStorage(*partition, peer);
  ASSERT_TRUE(graph);
  ASSERT_TRUE(peer_graph);
  scoped_refptr<content::ServiceWorkerContextWrapper> context =
      graph->service_worker();
  scoped_refptr<content::ServiceWorkerContextWrapper> sibling =
      peer_graph->service_worker();
  ASSERT_TRUE(context);
  ASSERT_TRUE(sibling);
  EXPECT_NE(context, sibling);
  EXPECT_NE(context.get(), partition->GetServiceWorkerContext());
  EXPECT_TRUE(context->is_incognito());
  EXPECT_TRUE(context->user_data_directory().empty());
  EXPECT_EQ(context->bucket_storage_owner().temporary, owner);
  EXPECT_EQ(context->storage_partition(), partition);
  using Found =
      base::test::TestFuture<blink::ServiceWorkerStatusCode,
                             scoped_refptr<content::ServiceWorkerRegistration>>;
  const GURL scope("https://alpha.test/unused/");
  Found before;
  context->FindRegistrationForScope(scope, alpha->GetStorageKey(),
                                    before.GetCallback());
  EXPECT_EQ(before.Get<0>(), blink::ServiceWorkerStatusCode::kErrorNotFound);
  EXPECT_FALSE(before.Get<1>());
  owner->Retire();
  EXPECT_FALSE(context->context());
  EXPECT_FALSE(context->storage_partition());
  Found after;
  context->FindRegistrationForScope(scope, alpha->GetStorageKey(),
                                    after.GetCallback());
  EXPECT_EQ(after.Get<0>(), blink::ServiceWorkerStatusCode::kErrorAbort);
  EXPECT_FALSE(after.Get<1>());
  Found peer_query;
  sibling->FindRegistrationForScope(GURL("https://beta.test/unused/"),
                                    beta->GetStorageKey(),
                                    peer_query.GetCallback());
  EXPECT_EQ(peer_query.Get<0>(),
            blink::ServiceWorkerStatusCode::kErrorNotFound);
  EXPECT_TRUE(sibling->context());
  EXPECT_FALSE(profile()->IsOffTheRecord());
}

TEST_F(NativeEmbeddedStateTest,
       ReservedServiceWorkerClientMovesWithoutChangingUUID) {
  auto* first = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* second =
      static_cast<content::RenderFrameHostImpl*>(Embed(second_.get()));
  auto alpha = first->GetBucketStorageOwner().temporary;
  auto beta = second->GetBucketStorageOwner().temporary;
  ASSERT_TRUE(alpha);
  ASSERT_TRUE(beta);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(first->GetStoragePartition());
  auto from = content::GetAlloyServiceWorkerContextForOwner(
      *partition, first->GetBucketStorageOwner());
  auto into = content::GetAlloyServiceWorkerContextForOwner(
      *partition, second->GetBucketStorageOwner());
  ASSERT_TRUE(from);
  ASSERT_TRUE(into);
  ASSERT_NE(from, into);
  auto reserved = from->context()
                      ->service_worker_client_owner()
                      .CreateServiceWorkerClientForWindow(
                          true, first->GetFrameTreeNodeId());
  auto client = reserved.AsWeakPtr();
  ASSERT_TRUE(client);
  const auto uuid = client->client_uuid();
  client->UpdateUrls(GURL("https://embedded.test/"),
                     url::Origin::Create(GURL("https://outer.test/")),
                     first->GetStorageKey());
  ASSERT_TRUE(
      from->context()->service_worker_client_owner().TransferReservedClient(
          *client, *into->context()));
  ASSERT_TRUE(client);
  EXPECT_EQ(client->client_uuid(), uuid);
  EXPECT_EQ(client->context().get(), into->context());
  EXPECT_FALSE(from->context()
                   ->service_worker_client_owner()
                   .GetServiceWorkerClientByClientID(uuid));
  EXPECT_EQ(into->context()
                ->service_worker_client_owner()
                .GetServiceWorkerClientByClientID(uuid),
            client.get());
  alpha->Retire();
  ASSERT_TRUE(client);
  EXPECT_EQ(client->context().get(), into->context());
  // Native cross-origin reserved-environment discard still changes UUID and
  // updates the new owner's own index, without any virtual identifier scheme.
  const GURL redirected("https://other.test/");
  client->UpdateUrls(
      redirected, url::Origin::Create(GURL("https://outer.test/")),
      second->GetStorageKey().WithOrigin(url::Origin::Create(redirected)));
  EXPECT_NE(client->client_uuid(), uuid);
  EXPECT_FALSE(into->context()
                   ->service_worker_client_owner()
                   .GetServiceWorkerClientByClientID(uuid));
  EXPECT_EQ(into->context()
                ->service_worker_client_owner()
                .GetServiceWorkerClientByClientID(client->client_uuid()),
            client.get());
  beta->Retire();
  EXPECT_FALSE(client);
}

TEST_F(NativeEmbeddedStateTest,
       HeldServiceWorkerStopsWhenItsNativeContextEnds) {
  content::EmbeddedWorkerTestHelper first(base::FilePath(),
                                          TestingProfile::Builder().Build());
  content::EmbeddedWorkerTestHelper sibling(base::FilePath(),
                                            TestingProfile::Builder().Build());
  auto [registration, version] = first.PrepareRegistrationAndVersion(
      GURL("https://worker.test/"), GURL("https://worker.test/sw.js"));
  auto [peer_registration, peer] = sibling.PrepareRegistrationAndVersion(
      GURL("https://peer.test/"), GURL("https://peer.test/sw.js"));
  first.StartWorker(version->embedded_worker(),
                    first.CreateStartParams(version));
  sibling.StartWorker(peer->embedded_worker(), sibling.CreateStartParams(peer));
  ASSERT_EQ(version->running_status(), blink::EmbeddedWorkerStatus::kRunning);
  ASSERT_EQ(peer->running_status(), blink::EmbeddedWorkerStatus::kRunning);
  auto context = first.context()->AsWeakPtr();
  first.ShutdownContext();
  EXPECT_FALSE(context);
  // The registration and version stay retained, and the sibling context stays
  // alive. Native Stop IPC must end the worker rather than just dropping refs.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(version->running_status(), blink::EmbeddedWorkerStatus::kStopped);
  EXPECT_EQ(peer->running_status(), blink::EmbeddedWorkerStatus::kRunning);
  base::test::TestFuture<blink::ServiceWorkerStatusCode> restarted;
  version->StartWorker(content::ServiceWorkerMetrics::EventType::MESSAGE,
                       restarted.GetCallback());
  EXPECT_EQ(restarted.Get(), blink::ServiceWorkerStatusCode::kErrorAbort);
  sibling.ShutdownContext();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(peer->running_status(), blink::EmbeddedWorkerStatus::kStopped);
}

// Exercise the production request boundary, not just the native client-map
// transfer primitive. NavigationSimulator supplies a real pending request so
// owner resolution follows the same browser-owned navigation as the loader.
void CheckServiceWorkerNavigationEntry(content::RenderFrameHost* frame,
                                       bool via_real_site = false) {
  auto* native_frame = static_cast<content::RenderFrameHostImpl*>(frame);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  scoped_refptr<content::ServiceWorkerContextWrapper> real =
      static_cast<content::ServiceWorkerContextWrapper*>(
          partition->GetServiceWorkerContext());
  ASSERT_TRUE(real->context());
  const GURL alpha_url("https://alpha.test/entry");
  const GURL beta_url("https://beta.test/redirected");
  auto navigation =
      content::NavigationSimulator::CreateRendererInitiated(alpha_url, frame);
  navigation->Start();
  ASSERT_FALSE(navigation->HasFailed());
  const auto isolation_info =
      native_frame->ComputeIsolationInfoForNavigation(alpha_url);
  const auto outer = isolation_info.top_frame_origin();
  content::ServiceWorkerMainResourceHandle handle(real, base::DoNothing(), "");
  handle.set_service_worker_client(
      real->context()
          ->service_worker_client_owner()
          .CreateServiceWorkerClientForWindow(
              true, native_frame->GetFrameTreeNodeId()),
      isolation_info);
  ASSERT_TRUE(handle.InitializeForRequest(alpha_url, outer, nullptr));
  scoped_refptr<content::ServiceWorkerContextWrapper> alpha =
      handle.context_wrapper();
  ASSERT_NE(alpha, real);
  auto alpha_owner = alpha->bucket_storage_owner().temporary;
  ASSERT_TRUE(alpha_owner);
  ASSERT_TRUE(handle.service_worker_client());
  ASSERT_EQ(handle.service_worker_client()->context().get(), alpha->context());
  ASSERT_EQ(
      handle.service_worker_client()->key().nonce(),
      alpha_owner->Capability(StorePurpose::kServiceWorkers).generation());
  const auto old_uuid = handle.service_worker_client()->client_uuid();
  if (via_real_site) {
    const GURL real_url("https://outer.test/first-party-hop");
    navigation->Redirect(real_url);
    ASSERT_FALSE(navigation->HasFailed());
    ASSERT_TRUE(handle.InitializeForRequest(real_url, outer, nullptr));
    ASSERT_EQ(handle.context_wrapper(), real.get());
    ASSERT_TRUE(handle.service_worker_client());
    ASSERT_FALSE(handle.service_worker_client()->key().nonce());
    ASSERT_EQ(
        handle.service_worker_client()->key(),
        blink::StorageKey::CreateFirstParty(url::Origin::Create(real_url)));
    ::testing::Test::RecordProperty(
        "real_hop_navigation_entry",
        "ordinary-context-first-party-key-no-temporary-nonce");
  }
  navigation->Redirect(beta_url);
  ASSERT_FALSE(navigation->HasFailed());
  ASSERT_TRUE(handle.InitializeForRequest(beta_url, outer, nullptr));
  scoped_refptr<content::ServiceWorkerContextWrapper> beta =
      handle.context_wrapper();
  ASSERT_NE(beta, alpha);
  ASSERT_NE(beta, real);
  auto beta_owner = beta->bucket_storage_owner().temporary;
  ASSERT_TRUE(beta_owner);
  ASSERT_NE(alpha_owner, beta_owner);
  auto client = handle.service_worker_client();
  ASSERT_TRUE(client);
  ASSERT_EQ(client->context().get(), beta->context());
  ASSERT_EQ(client->key().origin(), url::Origin::Create(beta_url));
  ASSERT_EQ(client->key().nonce(),
            beta_owner->Capability(StorePurpose::kServiceWorkers).generation());
  ASSERT_NE(client->client_uuid(), old_uuid);
  ::testing::Test::RecordProperty("positive_navigation_entry",
                                  "real-alpha-beta-native-key-and-uuid");
  alpha_owner->Retire();
  ASSERT_FALSE(alpha->context());
  ASSERT_TRUE(client);
  ASSERT_EQ(client->context().get(), beta->context());
  beta_owner->Retire();
  EXPECT_FALSE(client);
  EXPECT_FALSE(handle.PrepareContextForRequest(beta_url));
  EXPECT_FALSE(handle.service_worker_client());
  EXPECT_TRUE(real->context());
  navigation->AbortFromRenderer();
  ::testing::Test::RecordProperty("retired_navigation_entry",
                                  "old-owner-independent-new-owner-refused");
}

TEST_F(NativeEmbeddedStateTest,
       ServiceWorkerNavigationEntryTracksRedirectOwner) {
  CheckServiceWorkerNavigationEntry(Embed(first_.get()));
}

TEST_F(NativeOuterEmbeddedStateTest,
       ServiceWorkerNavigationEntryTracksRedirectOwner) {
  CheckServiceWorkerNavigationEntry(Embed(first_.get()));
}

TEST_F(NativeEmbeddedStateTest,
       ServiceWorkerNavigationEntryClearsTemporaryNonceOnRealHop) {
  CheckServiceWorkerNavigationEntry(Embed(first_.get()), true);
}

TEST_F(NativeOuterEmbeddedStateTest,
       ServiceWorkerNavigationEntryClearsTemporaryNonceOnRealHop) {
  CheckServiceWorkerNavigationEntry(Embed(first_.get()), true);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldServiceWorkerRegistrationAndUserDataEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto append = [&](const char* name, const char* url) {
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild(name);
    return content::NavigationSimulator::NavigateAndCommitFromDocument(
        GURL(url), initial);
  };
  auto* alpha = append("registration-alpha", "https://alpha.test/");
  auto* beta = append("registration-beta", "https://beta.test/");
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(alpha->GetStoragePartition());
  const auto owner = content::GetAlloyEmbeddedStateForDocument(alpha);
  const auto peer_owner = content::GetAlloyEmbeddedStateForDocument(beta);
  ASSERT_TRUE(owner.temporary);
  ASSERT_TRUE(peer_owner.temporary);
  auto context =
      content::GetAlloyServiceWorkerContextForOwner(*partition, owner);
  auto peer =
      content::GetAlloyServiceWorkerContextForOwner(*partition, peer_owner);
  ASSERT_TRUE(context);
  ASSERT_TRUE(peer);
  auto create = [](auto& wrapper, content::RenderFrameHost* frame) {
    auto& registry = wrapper->context()->registry();
    const auto scope = frame->GetLastCommittedURL();
    const auto script = scope.Resolve("sw.js");
    auto resource = content::WriteToDiskCacheSync(
        registry.GetRemoteStorageControl(), script,
        {{"Content-Type", "application/javascript"}}, "self.value='native';",
        "");
    CHECK(resource);
    auto registration = content::CreateServiceWorkerRegistrationAndVersion(
        wrapper->context(), scope, script, frame->GetStorageKey(),
        resource->resource_id);
    CHECK(registration);
    auto* version = registration->waiting_version();
    version->SetStatus(content::ServiceWorkerVersion::ACTIVATED);
    registration->SetActiveVersion(version);
    base::test::TestFuture<blink::ServiceWorkerStatusCode> stored;
    registry.StoreRegistration(registration.get(), version,
                               stored.GetCallback());
    EXPECT_EQ(stored.Get(), blink::ServiceWorkerStatusCode::kOk);
    return registration;
  };
  auto registration = create(context, alpha);
  auto peer_registration = create(peer, beta);
  scoped_refptr<content::ServiceWorkerVersion> held_version =
      registration->active_version();
  ASSERT_TRUE(held_version);
  // Independent native ID allocators deliberately collide: IDs carry no
  // authority to redirect a retained request into another owner's registry.
  ASSERT_EQ(registration->id(), peer_registration->id());
  const int64_t id = registration->id();
  const std::string key = "alloy-held-user-data";
  auto store = [&](auto& wrapper, const auto& storage_key,
                   const std::string& value) {
    base::test::TestFuture<blink::ServiceWorkerStatusCode> result;
    wrapper->StoreRegistrationUserData(id, storage_key, {{key, value}},
                                       result.GetCallback());
    return result.Get();
  };
  using Data = base::test::TestFuture<const std::vector<std::string>&,
                                      blink::ServiceWorkerStatusCode>;
  using Found =
      base::test::TestFuture<blink::ServiceWorkerStatusCode,
                             scoped_refptr<content::ServiceWorkerRegistration>>;
  using AllData = base::test::TestFuture<
      const std::vector<std::pair<int64_t, std::string>>&,
      blink::ServiceWorkerStatusCode>;
  ASSERT_EQ(store(context, alpha->GetStorageKey(), "alpha-user-data"),
            blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(store(peer, beta->GetStorageKey(), "beta-user-data"),
            blink::ServiceWorkerStatusCode::kOk);
  Data before, prefix;
  context->GetRegistrationUserData(id, {key}, before.GetCallback());
  context->GetRegistrationUserDataByKeyPrefix(id, "alloy-held-",
                                              prefix.GetCallback());
  ASSERT_EQ(before.Get<1>(), blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(before.Get<0>(), (std::vector<std::string>{"alpha-user-data"}));
  ASSERT_EQ(prefix.Get<1>(), blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(prefix.Get<0>(), before.Get<0>());
  AllData all;
  context->GetUserDataForAllRegistrations(key, all.GetCallback());
  ASSERT_EQ(all.Get<1>(), blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(
      all.Get<0>(),
      (std::vector<std::pair<int64_t, std::string>>{{id, "alpha-user-data"}}));
  Found found, peer_found, real_missing;
  context->FindReadyRegistrationForIdOnly(id, found.GetCallback());
  peer->FindReadyRegistrationForIdOnly(id, peer_found.GetCallback());
  ASSERT_EQ(found.Get<0>(), blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(found.Get<1>(), registration);
  ASSERT_EQ(peer_found.Get<0>(), blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(peer_found.Get<1>(), peer_registration);
  auto* real = static_cast<content::ServiceWorkerContextWrapper*>(
      partition->GetServiceWorkerContext());
  real->FindRegistrationForIdWithoutActivation(id, alpha->GetStorageKey(),
                                               real_missing.GetCallback());
  ASSERT_EQ(real_missing.Get<0>(),
            blink::ServiceWorkerStatusCode::kErrorNotFound);
  ASSERT_FALSE(real_missing.Get<1>());
  RecordProperty(
      "positive_registration_user_data",
      "colliding-ids-live-lookup-bytes-prefix-enumeration-real-absent");
  owner.temporary->Retire();
  ASSERT_TRUE(alpha->IsActive());
  ASSERT_FALSE(context->context());
  Data denied;
  Found ended;
  AllData denied_all;
  base::test::TestFuture<blink::ServiceWorkerStatusCode> cleared, restarted;
  context->GetRegistrationUserData(id, {key}, denied.GetCallback());
  context->FindReadyRegistrationForIdOnly(id, ended.GetCallback());
  context->GetUserDataForAllRegistrations(key, denied_all.GetCallback());
  context->ClearRegistrationUserData(id, {key}, cleared.GetCallback());
  EXPECT_EQ(store(context, alpha->GetStorageKey(), "late-forbidden-value"),
            blink::ServiceWorkerStatusCode::kErrorAbort);
  EXPECT_EQ(denied.Get<1>(), blink::ServiceWorkerStatusCode::kErrorAbort);
  EXPECT_TRUE(denied.Get<0>().empty());
  EXPECT_EQ(ended.Get<0>(), blink::ServiceWorkerStatusCode::kErrorAbort);
  EXPECT_FALSE(ended.Get<1>());
  EXPECT_EQ(denied_all.Get<1>(), blink::ServiceWorkerStatusCode::kErrorAbort);
  EXPECT_TRUE(denied_all.Get<0>().empty());
  EXPECT_EQ(cleared.Get(), blink::ServiceWorkerStatusCode::kErrorAbort);
  held_version->StartWorker(content::ServiceWorkerMetrics::EventType::MESSAGE,
                            restarted.GetCallback());
  EXPECT_EQ(restarted.Get(), blink::ServiceWorkerStatusCode::kErrorAbort);
  Data peer_after;
  Found peer_alive;
  peer->FindReadyRegistrationForIdOnly(id, peer_alive.GetCallback());
  peer->GetRegistrationUserData(id, {key}, peer_after.GetCallback());
  EXPECT_EQ(peer_alive.Get<0>(), blink::ServiceWorkerStatusCode::kOk);
  EXPECT_EQ(peer_alive.Get<1>(), peer_registration);
  EXPECT_EQ(peer_after.Get<1>(), blink::ServiceWorkerStatusCode::kOk);
  EXPECT_EQ(peer_after.Get<0>(), (std::vector<std::string>{"beta-user-data"}));
  EXPECT_TRUE(peer->context());
  EXPECT_FALSE(profile()->IsOffTheRecord());
  RecordProperty(
      "retired_registration_user_data",
      "read-write-clear-enumeration-id-restart-abort-peer-bytes-live");
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldServiceWorkerScriptHandlesEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto append = [&](const char* name, const char* url) {
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild(name);
    return content::NavigationSimulator::NavigateAndCommitFromDocument(
        GURL(url), initial);
  };
  auto* alpha = append("script-alpha", "https://alpha.test/");
  auto* beta = append("script-beta", "https://beta.test/");
  ASSERT_EQ(alpha->GetLastCommittedURL(), GURL("https://alpha.test/"));
  ASSERT_EQ(beta->GetLastCommittedURL(), GURL("https://beta.test/"));
  auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
  auto peer_owner = content::GetAlloyEmbeddedStateForDocument(beta).temporary;
  ASSERT_TRUE(owner);
  ASSERT_TRUE(peer_owner);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(alpha->GetStoragePartition());
  auto* graph = content::GetAlloyEmbeddedBucketStorage(*partition, owner);
  auto* peer_graph =
      content::GetAlloyEmbeddedBucketStorage(*partition, peer_owner);
  ASSERT_TRUE(graph);
  ASSERT_TRUE(peer_graph);
  scoped_refptr<content::ServiceWorkerContextWrapper> context =
      graph->service_worker();
  scoped_refptr<content::ServiceWorkerContextWrapper> peer =
      peer_graph->service_worker();
  ASSERT_TRUE(context);
  ASSERT_TRUE(peer);
  ASSERT_NE(context, peer);
  ASSERT_TRUE(context->is_incognito());
  ASSERT_TRUE(context->user_data_directory().empty());
  auto& control = context->context()->registry().GetRemoteStorageControl();
  auto& peer_control = peer->context()->registry().GetRemoteStorageControl();
  const std::string script = "self.scriptValue='alpha-native-script';";
  const std::string peer_script = "self.scriptValue='beta-native-script';";
  const std::string metadata = "alpha-native-script-metadata";
  const std::string peer_metadata = "beta-native-script-metadata";
  // This upstream helper operates the selected native cache: the captured
  // temporary context above uses its real memory backend in a disk profile.
  auto stored = content::WriteToDiskCacheSync(
      control, GURL("https://alpha.test/sw.js"),
      {{"Content-Type", "application/javascript"}}, script, metadata);
  auto sibling = content::WriteToDiskCacheSync(
      peer_control, GURL("https://beta.test/sw.js"),
      {{"Content-Type", "application/javascript"}}, peer_script, peer_metadata);
  ASSERT_TRUE(stored);
  ASSERT_TRUE(sibling);
  auto reader_for = [](auto& control_pipe, int64_t id) {
    mojo::Remote<storage::mojom::ServiceWorkerResourceReader> reader;
    control_pipe->CreateResourceReader(id, std::nullopt,
                                       reader.BindNewPipeAndPassReceiver());
    return reader;
  };
  auto head_matches = [](auto& reader, const std::string& expected) {
    base::test::TestFuture<int32_t, network::mojom::URLResponseHeadPtr,
                           std::optional<mojo_base::BigBuffer>>
        head;
    reader->ReadResponseHead(head.GetCallback());
    if (head.Get<0>() <= 0 || !head.Get<1>() || !head.Get<2>()) {
      return false;
    }
    const auto& data = *head.Get<2>();
    return std::string(reinterpret_cast<const char*>(data.data()),
                       data.size()) == expected;
  };
  auto first_read = reader_for(control, stored->resource_id);
  ASSERT_TRUE(head_matches(first_read, metadata));
  NativeServiceWorkerBodyRead first_body;
  ASSERT_EQ(first_body.Read(*first_read.get(), script.size()), script);
  auto reader = reader_for(control, stored->resource_id);
  auto peer_reader = reader_for(peer_control, sibling->resource_id);
  ASSERT_TRUE(head_matches(reader, metadata));
  ASSERT_TRUE(head_matches(peer_reader, peer_metadata));
  const int64_t pending_id = content::GetNewResourceIdSync(control);
  mojo::Remote<storage::mojom::ServiceWorkerResourceWriter> pending_writer;
  control->CreateResourceWriter(pending_id,
                                pending_writer.BindNewPipeAndPassReceiver());
  auto header = network::mojom::URLResponseHead::New();
  header->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders("HTTP/1.1 200 OK\n"));
  header->content_length = script.size();
  base::test::TestFuture<int32_t> header_written;
  pending_writer->WriteResponseHead(std::move(header),
                                    header_written.GetCallback());
  ASSERT_GT(header_written.Get(), 0);
  mojo::Remote<storage::mojom::ServiceWorkerResourceMetadataWriter>
      pending_metadata;
  control->CreateResourceMetadataWriter(
      stored->resource_id, pending_metadata.BindNewPipeAndPassReceiver());
  pending_metadata.FlushForTesting();
  ASSERT_TRUE(pending_metadata.is_connected());
  RecordProperty("positive_script_handles",
                 "body-metadata-head-partial-writer-peer");
  owner->Retire();
  ASSERT_FALSE(context->context());
  ASSERT_TRUE(peer->context());
  // No idle wait: this is a new admission after retirement, with every
  // wrapper/document/client reference still held by this test.
  base::test::TestFuture<mojo::ScopedDataPipeConsumerHandle> late_read;
  reader->PrepareReadData(
      script.size(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          late_read.GetCallback(), mojo::ScopedDataPipeConsumerHandle()));
  ASSERT_FALSE(late_read.Take().is_valid());
  base::test::TestFuture<int32_t> late_body;
  pending_writer->WriteData(
      mojo_base::BigBuffer(base::as_byte_span(script)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(late_body.GetCallback(),
                                                  int32_t{net::ERR_ABORTED}));
  ASSERT_LT(late_body.Get(), 0);
  base::test::TestFuture<int32_t> late_metadata;
  pending_metadata->WriteMetadata(
      mojo_base::BigBuffer(base::as_byte_span(metadata)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(late_metadata.GetCallback(),
                                                  int32_t{net::ERR_ABORTED}));
  ASSERT_LT(late_metadata.Get(), 0);
  reader.FlushForTesting();
  pending_writer.FlushForTesting();
  pending_metadata.FlushForTesting();
  ASSERT_FALSE(reader.is_connected());
  ASSERT_FALSE(pending_writer.is_connected());
  ASSERT_FALSE(pending_metadata.is_connected());
  NativeServiceWorkerBodyRead sibling_body;
  ASSERT_EQ(sibling_body.Read(*peer_reader.get(), peer_script.size()),
            peer_script);
  EXPECT_FALSE(profile()->IsOffTheRecord());
  RecordProperty("retired_script_handles", "closed-peer-body-live");
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       TemporaryAreaNeverEntersDiskController) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto* main = web_contents()->GetPrimaryMainFrame();
  auto* initial =
      content::RenderFrameHostTester::For(main)->AppendChild("child");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://embedded.test/"), initial);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  ASSERT_TRUE(partition->GetStoragePartitionPath());
  auto* dom = static_cast<content::DOMStorageContextWrapper*>(
      partition->GetDOMStorageContext());
  auto open = [&](content::RenderFrameHost* target) {
    mojo::Remote<blink::mojom::StorageArea> area;
    dom->OpenLocalStorage(
        target->GetStorageKey(), target->GetFrameToken(),
        area.BindNewPipeAndPassReceiver(),
        content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
            target->GetProcess()->GetID()),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return area;
  };
  auto first_party = open(main);
  base::test::TestFuture<bool> first_write;
  first_party->Put({1}, {9}, std::nullopt, nullptr, first_write.GetCallback());
  ASSERT_TRUE(first_write.Get());
  auto temporary = open(frame);
  base::test::TestFuture<bool> temporary_write;
  temporary->Put({1}, {2}, std::nullopt, nullptr,
                 temporary_write.GetCallback());
  ASSERT_TRUE(temporary_write.Get());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> read;
  temporary->GetAll(mojo::NullRemote(), read.GetCallback());
  auto values = read.Take();
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0]->value, (std::vector<uint8_t>{2}));
  dom->GetLocalStorageControl()->Flush();
  base::test::TestFuture<std::vector<storage::mojom::StorageUsageInfoPtr>>
      usage;
  dom->GetLocalStorageControl()->GetUsage(usage.GetCallback());
  auto entries = usage.Take();
  EXPECT_TRUE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == main->GetStorageKey();
  }));
  EXPECT_FALSE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == frame->GetStorageKey();
  }));
}

TEST_F(NativeEmbeddedStateTest, SessionStorageEndpointEndsWithItsTabScope) {
  auto* frame = Embed(first_.get());
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto* dom = static_cast<content::DOMStorageContextWrapper*>(
      partition->GetDOMStorageContext());
  const std::string namespace_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  dom->GetSessionStorageControl()->CreateNamespace(namespace_id);
  mojo::Remote<blink::mojom::StorageArea> area;
  dom->BindStorageArea(
      frame->GetStorageKey(), frame->GetFrameToken(), namespace_id,
      area.BindNewPipeAndPassReceiver(),
      content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
          frame->GetProcess()->GetID()),
      base::BindOnce([](std::string_view reason) { ADD_FAILURE() << reason; }));
  base::test::TestFuture<bool> written;
  area->Put({1}, {5, 6, 7}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> contents;
  area->GetAll(mojo::NullRemote(), contents.GetCallback());
  auto values = contents.Take();
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0]->value, (std::vector<uint8_t>{5, 6, 7}));
  first_.reset();
  area.FlushForTesting();
  EXPECT_FALSE(area.is_connected());
  EXPECT_TRUE(second_);
  dom->GetSessionStorageControl()->DeleteNamespace(namespace_id, false);
}

TEST_F(NativeEmbeddedStateTest, BucketEndpointsEndWithTheirTabScope) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  mojo::Remote<blink::mojom::BucketManagerHost> manager;
  partition->GetBucketManager()->BindReceiver(
      frame->GetWeakPtr(), manager.BindNewPipeAndPassReceiver(),
      base::BindOnce([](std::string_view reason) { ADD_FAILURE() << reason; }));
  const auto expires = base::Time::Now() + base::Days(1);
  auto policies = blink::mojom::BucketPolicies::New();
  policies->expires = expires;
  base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                         blink::mojom::BucketError>
      opened;
  manager->OpenBucket("scoped_bucket", std::move(policies),
                      opened.GetCallback());
  auto [pending, error] = opened.Take();
  ASSERT_TRUE(pending.is_valid());
  mojo::Remote<blink::mojom::BucketHost> bucket(std::move(pending));
  base::test::TestFuture<std::optional<base::Time>, bool> read;
  bucket->Expires(read.GetCallback());
  ASSERT_TRUE(read.Get<1>());
  ASSERT_TRUE(read.Get<0>().has_value());
  EXPECT_EQ(*read.Get<0>(), expires);
  first_.reset();
  manager.FlushForTesting();
  bucket.FlushForTesting();
  EXPECT_FALSE(manager.is_connected());
  EXPECT_FALSE(bucket.is_connected());
  EXPECT_TRUE(second_);
}

TEST_F(NativeEmbeddedStateTest, SessionClonePreservesMutationOrdering) {
  auto* a = Embed(first_.get());
  auto* b = Embed(first_.get(), "https://other-embedded.test/");
  auto* dom = Dom(a);
  auto* control = dom->GetSessionStorageControl();
  const auto source = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const auto target = base::Uuid::GenerateRandomV4().AsLowercaseString();
  control->CreateNamespace(source);
  auto real = OpenSession(first_->GetPrimaryMainFrame(), source);
  auto first = OpenSession(a, source);
  auto second = OpenSession(b, source);
  mojo::Remote<blink::mojom::SessionStorageNamespace> ns;
  dom->BindNamespace(source, base::BindOnce([](std::string_view reason) {
                       ADD_FAILURE() << reason;
                     }),
                     ns.BindNewPipeAndPassReceiver());
  control->CloneNamespace(
      source, target,
      storage::mojom::SessionStorageCloneType::kWaitForCloneOnNamespace);
  // Exactly the renderer's cross-pipe ordering, with no wait for individual
  // writes. The browser hop must preserve both sides at the utility endpoints.
  base::test::TestFuture<bool> before_real, before_first, before_second;
  real->Put({1}, {10}, std::nullopt, nullptr, before_real.GetCallback());
  first->Put({1}, {11}, std::nullopt, nullptr, before_first.GetCallback());
  second->Put({1}, {12}, std::nullopt, nullptr, before_second.GetCallback());
  for (auto* area : {&real, &first, &second}) {
    ns.PauseReceiverUntilFlushCompletes(area->FlushAsync());
  }
  ns->Clone(target);
  for (auto* area : {&real, &first, &second}) {
    area->PauseReceiverUntilFlushCompletes(ns.FlushAsync());
  }
  base::test::TestFuture<bool> after_real, after_first, after_second;
  real->Put({1}, {20}, std::nullopt, nullptr, after_real.GetCallback());
  first->Put({1}, {21}, std::nullopt, nullptr, after_first.GetCallback());
  second->Put({1}, {22}, std::nullopt, nullptr, after_second.GetCallback());
  auto cloned_real = OpenSession(first_->GetPrimaryMainFrame(), target);
  auto cloned_first = OpenSession(a, target);
  auto cloned_second = OpenSession(b, target);
  EXPECT_EQ(SessionValue(cloned_real), (std::vector<uint8_t>{10}));
  EXPECT_EQ(SessionValue(cloned_first), (std::vector<uint8_t>{11}));
  EXPECT_EQ(SessionValue(cloned_second), (std::vector<uint8_t>{12}));
  EXPECT_TRUE(before_real.Get());
  EXPECT_TRUE(before_first.Get());
  EXPECT_TRUE(before_second.Get());
  EXPECT_TRUE(after_real.Get());
  EXPECT_TRUE(after_first.Get());
  EXPECT_TRUE(after_second.Get());
  EXPECT_EQ(SessionValue(real), (std::vector<uint8_t>{20}));
  EXPECT_EQ(SessionValue(first), (std::vector<uint8_t>{21}));
  EXPECT_EQ(SessionValue(second), (std::vector<uint8_t>{22}));
  control->DeleteNamespace(source, false);
  control->DeleteNamespace(target, false);
}

TEST_F(NativeEmbeddedStateTest, SessionCloneDoesNotCrossTabScope) {
  CheckCrossTabSessionClone(false);
}

TEST_F(NativeOuterEmbeddedStateTest, SessionCloneUsesLiveOuterScope) {
  CheckCrossTabSessionClone(true);
}

TEST_F(NativeEmbeddedStateTest, NamedBucketsRemainTabLocal) {
  auto first = Buckets(Embed(first_.get()));
  auto second = Buckets(Embed(second_.get()));
  auto bucket = CreateBucket(first, "local_bucket");
  base::test::TestFuture<std::vector<std::string>, bool> first_keys,
      second_keys;
  first->Keys(first_keys.GetCallback<const std::vector<std::string>&, bool>());
  second->Keys(
      second_keys.GetCallback<const std::vector<std::string>&, bool>());
  EXPECT_TRUE(first_keys.Get<1>());
  EXPECT_EQ(first_keys.Get<0>(), (std::vector<std::string>{"local_bucket"}));
  EXPECT_TRUE(second_keys.Get<1>());
  EXPECT_TRUE(second_keys.Get<0>().empty());
}

TEST_F(NativeEmbeddedStateTest,
       IndexedDBUsesItsBucketNamespaceAndEndsWithScope) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  Embed(second_.get());
  auto* main =
      static_cast<content::RenderFrameHostImpl*>(first_->GetPrimaryMainFrame());
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  mojo::Remote<blink::mojom::IDBFactory> real;
  // The harness uses MockRenderProcessHost: its public BindIndexedDB is a
  // no-op. Bind the real first-party native partition controller directly;
  // temporary access below goes through the real BucketHost protocol instead.
  partition->BindIndexedDB(
      storage::BucketLocator::ForDefaultBucket(main->GetStorageKey()),
      main->GetBucketClientInfo(), real.BindNewPipeAndPassReceiver());
  NativeIDBOpen real_opened;
  real_opened.Open(real);
  using Reply =
      base::test::TestFuture<std::vector<blink::mojom::IDBNameAndVersionPtr>,
                             blink::mojom::IDBErrorPtr>;
  Reply real_info;
  real->GetDatabaseInfo(real_info.GetCallback());
  ASSERT_TRUE(real_info.Wait());
  ASSERT_EQ(real_info.Get<0>().size(), 1u);
  auto manager = Buckets(frame);
  auto bucket = CreateBucket(manager, "idb_bucket");
  mojo::Remote<blink::mojom::IDBFactory> factory;
  bucket->GetIdbFactory(factory.BindNewPipeAndPassReceiver());
  Reply isolated;
  factory->GetDatabaseInfo(isolated.GetCallback());
  ASSERT_TRUE(isolated.Wait());
  ASSERT_EQ(isolated.Get<1>()->error_code,
            blink::mojom::IDBException::kNoError);
  ASSERT_TRUE(isolated.Get<0>().empty())
      << "foreign quota namespace exposed real database";
  NativeIDBOpen opened;
  opened.Open(factory);
  Reply before;
  factory->GetDatabaseInfo(before.GetCallback());
  ASSERT_TRUE(before.Wait()) << "database metadata before scope retirement";
  ASSERT_EQ(before.Get<1>()->error_code, blink::mojom::IDBException::kNoError);
  ASSERT_EQ(before.Get<0>().size(), 1u);
  EXPECT_EQ(before.Get<0>()[0]->name, u"scoped-native-database");
  first_.reset();
  factory.FlushForTesting();
  ASSERT_FALSE(factory.is_connected())
      << "held native factory survived scope retirement";
  Reply after;
  factory->GetDatabaseInfo(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      after.GetCallback(), std::vector<blink::mojom::IDBNameAndVersionPtr>{},
      blink::mojom::IDBError::New(blink::mojom::IDBException::kAbortError,
                                  u"scope closed")));
  ASSERT_TRUE(after.Wait()) << "retired factory reply/cancellation";
  EXPECT_TRUE(after.Get<0>().empty());
  factory.FlushForTesting();
  EXPECT_FALSE(factory.is_connected());
  EXPECT_TRUE(second_);
}

TEST_F(NativeEmbeddedStateTest,
       HeldIndexedDatabaseAndTransactionEndWithSiteOwner) {
  for (bool named : {true, false}) {
    SCOPED_TRACE(named ? "NAMED_IDB" : "DEFAULT_IDB");
    auto* frame =
        static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
    auto* peer =
        static_cast<content::RenderFrameHostImpl*>(Embed(second_.get()));
    const auto owner = frame->GetBucketStorageOwner().temporary;
    ASSERT_TRUE(owner);
    mojo::Remote<blink::mojom::BucketManagerHost> first_manager, second_manager;
    mojo::Remote<blink::mojom::BucketHost> first_bucket, second_bucket;
    mojo::Remote<blink::mojom::IDBFactory> first_factory, second_factory;
    if (named) {
      first_manager = Buckets(frame);
      second_manager = Buckets(peer);
      first_bucket = CreateBucket(first_manager, "held_idb");
      second_bucket = CreateBucket(second_manager, "held_idb");
      first_bucket->GetIdbFactory(first_factory.BindNewPipeAndPassReceiver());
      second_bucket->GetIdbFactory(second_factory.BindNewPipeAndPassReceiver());
    } else {
      // MockRenderProcessHost's default binder holds the receiver without
      // binding. Invoke its real production owner-selection boundary with
      // native inputs.
      auto* partition = static_cast<content::StoragePartitionImpl*>(
          frame->GetStoragePartition());
      content::BindAlloyBucketIndexedDB(
          *partition, *frame,
          storage::BucketLocator::ForDefaultBucket(frame->GetStorageKey()),
          first_factory.BindNewPipeAndPassReceiver());
      content::BindAlloyBucketIndexedDB(
          *partition, *peer,
          storage::BucketLocator::ForDefaultBucket(peer->GetStorageKey()),
          second_factory.BindNewPipeAndPassReceiver());
    }
    NativeIDBOpen first_database, second_database;
    first_database.Open(first_factory, true);
    second_database.Open(second_factory, true);
    ASSERT_TRUE(first_database.DatabaseConnected());
    ASSERT_TRUE(second_database.DatabaseConnected());
    auto transaction = first_database.BeginTransaction(2);
    auto peer_transaction = second_database.BeginTransaction(2);
    base::test::TestFuture<bool, uint32_t> positive, peer_positive;
    first_database.Count(2, positive.GetCallback());
    second_database.Count(2, peer_positive.GetCallback());
    ASSERT_TRUE(positive.Get<0>());
    ASSERT_EQ(positive.Get<1>(), 0u);
    ASSERT_TRUE(peer_positive.Get<0>());
    ASSERT_EQ(peer_positive.Get<1>(), 0u);
    transaction.FlushForTesting();
    ASSERT_TRUE(transaction.is_connected());
    SCOPED_TRACE("HELD_IDB_AFTER_POSITIVE_COUNT");
    first_database.ExpectRetirementAbort(2);
    owner->Retire();
    EXPECT_TRUE(frame->IsActive());
    EXPECT_FALSE(owner->Capability(StorePurpose::kIndexedDb).active());
    // Send a new request after revocation through the actual held database and
    // transaction, while both documents/context remain live.
    base::test::TestFuture<bool, uint32_t> denied;
    first_database.Count(2, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                                denied.GetCallback(), false, uint32_t{0}));
    EXPECT_FALSE(denied.Get<0>())
        << "held IDB database accepted a retired-site read";
    EXPECT_EQ(denied.Get<1>(), 0u);
    ASSERT_TRUE(first_database.AwaitDatabaseDisconnected());
    EXPECT_FALSE(first_database.DatabaseConnected());
    if (transaction.is_connected()) {
      base::test::TestFuture<void> disconnected;
      transaction.set_disconnect_handler(disconnected.GetCallback());
      ASSERT_TRUE(disconnected.Wait());
    }
    EXPECT_FALSE(transaction.is_connected());
    base::test::TestFuture<bool, uint32_t> peer_after;
    second_database.Count(2, peer_after.GetCallback());
    EXPECT_TRUE(peer_after.Get<0>());
    EXPECT_EQ(peer_after.Get<1>(), 0u);
    EXPECT_TRUE(second_database.DatabaseConnected());
    peer_transaction->Commit(0);
    EXPECT_TRUE(first_);
    EXPECT_TRUE(second_);
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldIndexedDBCursorsAndValueBlobsEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  mojo::Remote<storage::mojom::BlobStorageContext> input_context(
      content::ChromeBlobStorageContext::GetRemoteFor(profile()));
  std::vector<mojo::Remote<blink::mojom::Blob>> inputs;
  auto external_blob = [&](const std::string& bytes) {
    mojo::Remote<blink::mojom::Blob> original;
    input_context->RegisterFromMemory(
        original.BindNewPipeAndPassReceiver(),
        base::Uuid::GenerateRandomV4().AsLowercaseString(),
        base::as_byte_span(bytes));
    mojo::PendingRemote<blink::mojom::Blob> input;
    original->Clone(input.InitWithNewPipeAndPassReceiver());
    inputs.push_back(std::move(original));
    return blink::mojom::IDBExternalObject::NewBlobOrFile(
        blink::mojom::IDBBlobInfo::New(std::move(input), u"text/plain",
                                       static_cast<int64_t>(bytes.size()),
                                       nullptr));
  };
  auto value_blob = [](blink::mojom::IDBValue& value,
                       const std::string& expected_bits) {
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(value.bits.data()),
                          value.bits.size()),
              expected_bits);
    CHECK_EQ(value.external_objects.size(), 1u);
    CHECK(value.external_objects[0]->is_blob_or_file());
    return mojo::Remote<blink::mojom::Blob>(
        std::move(value.external_objects[0]->get_blob_or_file()->blob));
  };
  for (bool named : {false, true}) {
    const std::string namespace_name = named ? "named" : "default";
    SCOPED_TRACE(namespace_name);
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                      initial));
    };
    auto* alpha = append("idb-cursor-alpha", "https://alpha.test/");
    auto* beta = append("idb-cursor-beta", "https://beta.test/");
    auto owner = alpha->GetBucketStorageOwner().temporary;
    ASSERT_TRUE(owner);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        alpha->GetStoragePartition());
    ASSERT_TRUE(partition->GetStoragePartitionPath());
    std::vector<mojo::Remote<blink::mojom::BucketManagerHost>> managers;
    std::vector<mojo::Remote<blink::mojom::BucketHost>> buckets;
    auto factory = [&](content::RenderFrameHostImpl* frame) {
      mojo::Remote<blink::mojom::IDBFactory> result;
      if (named) {
        mojo::Remote<blink::mojom::BucketManagerHost> manager;
        partition->GetBucketManager()->BindReceiver(
            frame->GetWeakPtr(), manager.BindNewPipeAndPassReceiver(),
            base::BindOnce(
                [](std::string_view reason) { ADD_FAILURE() << reason; }));
        base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                               blink::mojom::BucketError>
            opened;
        manager->OpenBucket("held_cursor", blink::mojom::BucketPolicies::New(),
                            opened.GetCallback());
        auto [pending, error] = opened.Take();
        CHECK(pending.is_valid());
        mojo::Remote<blink::mojom::BucketHost> bucket(std::move(pending));
        bucket->GetIdbFactory(result.BindNewPipeAndPassReceiver());
        managers.push_back(std::move(manager));
        buckets.push_back(std::move(bucket));
      } else {
        // The harness has a MockRenderProcessHost. Invoke the production
        // default-bucket binder with the real native document's inputs.
        content::BindAlloyBucketIndexedDB(
            *partition, *frame,
            storage::BucketLocator::ForDefaultBucket(frame->GetStorageKey()),
            result.BindNewPipeAndPassReceiver());
      }
      return result;
    };
    auto first_factory = factory(alpha), peer_factory = factory(beta);
    NativeIDBOpen first_database, peer_database;
    first_database.Open(first_factory, true);
    peer_database.Open(peer_factory, true);
    const size_t original_input = inputs.size();
    auto put = [&](NativeIDBOpen& database, const std::string& marker) {
      auto transaction = database.BeginTransaction(2);
      for (int row = 1; row <= 3; ++row) {
        auto value = blink::mojom::IDBValue::New();
        const std::string bits = marker + "-" + std::to_string(row);
        value->bits = mojo_base::BigBuffer(base::as_byte_span(bits));
        value->external_objects.push_back(external_blob(marker + "-blob"));
        base::test::TestFuture<blink::mojom::IDBTransactionPutResultPtr> stored;
        transaction->Put(1, std::move(value),
                         blink::IndexedDBKey(static_cast<double>(row),
                                             blink::mojom::IDBKeyType::Number),
                         blink::mojom::IDBPutMode::AddOnly, {},
                         stored.GetCallback());
        auto result = stored.Take();
        ASSERT_TRUE(result->is_key());
        ASSERT_EQ(result->get_key().number(), row);
      }
      database.CommitAndWait(2, transaction);
    };
    put(first_database, "alpha-idb");
    put(peer_database, "beta-idb");
    ASSERT_FALSE(HasFatalFailure());
    auto transaction = first_database.BeginTransaction(3);
    auto peer_transaction = peer_database.BeginTransaction(3);
    base::test::TestFuture<blink::mojom::IDBDatabaseOpenCursorResultPtr> opened,
        peer_opened;
    first_database.OpenCursor(3, opened.GetCallback());
    peer_database.OpenCursor(3, peer_opened.GetCallback());
    auto first = opened.Take(), peer = peer_opened.Take();
    ASSERT_TRUE(first->is_value() && peer->is_value());
    ASSERT_TRUE(first->get_value()->value && peer->get_value()->value);
    ASSERT_EQ(first->get_value()->key.number(), 1);
    ASSERT_EQ(peer->get_value()->key.number(), 1);
    mojo::AssociatedRemote<blink::mojom::IDBCursor> cursor(
        std::move(first->get_value()->cursor));
    mojo::AssociatedRemote<blink::mojom::IDBCursor> peer_cursor(
        std::move(peer->get_value()->cursor));
    auto blob = value_blob(*first->get_value()->value, "alpha-idb-1");
    auto peer_blob = value_blob(*peer->get_value()->value, "beta-idb-1");
    mojo::Remote<blink::mojom::Blob> clone;
    blob->Clone(clone.BindNewPipeAndPassReceiver());
    mojo::Remote<network::mojom::DataPipeGetter> getter, peer_getter;
    blob->AsDataPipeGetter(getter.BindNewPipeAndPassReceiver());
    peer_blob->AsDataPipeGetter(peer_getter.BindNewPipeAndPassReceiver());
    NativeBlobRead positive, clone_positive, peer_positive;
    ASSERT_EQ(positive.Read(*blob.get()), "alpha-idb-blob");
    ASSERT_EQ(clone_positive.Read(*clone.get()), "alpha-idb-blob");
    ASSERT_EQ(peer_positive.Read(*peer_blob.get()), "beta-idb-blob");
    base::test::TestFuture<blink::mojom::IDBCursorResultPtr> advanced;
    cursor->Advance(1, advanced.GetCallback());
    auto next = advanced.Take();
    ASSERT_TRUE(next->is_values());
    ASSERT_EQ(next->get_values()->keys.size(), 1u);
    ASSERT_EQ(next->get_values()->values.size(), 1u);
    ASSERT_EQ(next->get_values()->keys[0].number(), 2);
    auto second_blob =
        value_blob(*next->get_values()->values[0], "alpha-idb-2");
    NativeBlobRead second_positive;
    ASSERT_EQ(second_positive.Read(*second_blob.get()), "alpha-idb-blob");
    NativeBlobRead positive_getter, positive_peer_getter;
    auto getter_value = positive_getter.ReadGetter(*getter.get());
    auto peer_getter_value =
        positive_peer_getter.ReadGetter(*peer_getter.get());
    ASSERT_EQ(getter_value.first, net::OK);
    ASSERT_EQ(getter_value.second, "alpha-idb-blob");
    ASSERT_EQ(peer_getter_value.first, net::OK);
    ASSERT_EQ(peer_getter_value.second, "beta-idb-blob");
    RecordProperty("positive_idb_cursor_blob_" + namespace_name,
                   "committed-native-rows-cursor-advance-blobs-clone-peer");
    RecordProperty("positive_idb_blob_getter_" + namespace_name,
                   "native-idb-blob-data-pipe-getter-and-peer-bytes");

    first_database.ExpectRetirementAbort(3);
    owner->Retire();
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    EXPECT_FALSE(owner->Capability(StorePurpose::kIndexedDb).active());
    // This is a new request on an already-advanced cursor, not a fresh bind.
    // A dropped callback is cancellation only together with actual disconnect.
    auto denied_callback = [](auto& future) {
      return mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          future.GetCallback(),
          blink::mojom::IDBCursorResult::NewErrorResult(
              blink::mojom::IDBError::New(
                  blink::mojom::IDBException::kAbortError, u"scope closed")));
    };
    base::test::TestFuture<blink::mojom::IDBCursorResultPtr> late_advance,
        late_continue, late_prefetch;
    cursor->Advance(1, denied_callback(late_advance));
    cursor->Continue(blink::IndexedDBKey(), blink::IndexedDBKey(),
                     denied_callback(late_continue));
    cursor->Prefetch(1, denied_callback(late_prefetch));
    EXPECT_TRUE(late_advance.Get()->is_error_result());
    EXPECT_TRUE(late_continue.Get()->is_error_result());
    EXPECT_TRUE(late_prefetch.Get()->is_error_result());
    ASSERT_TRUE(first_database.AwaitDatabaseDisconnected());
    cursor.FlushForTesting();
    EXPECT_FALSE(cursor.is_connected());
    NativeBlobRead denied, denied_clone, denied_second, peer_after, ordinary;
    EXPECT_EQ(denied.Read(*blob.get()), std::string());
    EXPECT_EQ(denied_clone.Read(*clone.get()), std::string());
    EXPECT_EQ(denied_second.Read(*second_blob.get()), std::string());
    EXPECT_EQ(peer_after.Read(*peer_blob.get()), "beta-idb-blob");
    EXPECT_EQ(ordinary.Read(*inputs[original_input].get()), "alpha-idb-blob");
    NativeBlobRead denied_getter, live_peer_getter;
    auto denied_getter_value = denied_getter.ReadGetter(*getter.get());
    auto live_peer_getter_value =
        live_peer_getter.ReadGetter(*peer_getter.get());
    EXPECT_NE(denied_getter_value.first, net::OK);
    EXPECT_TRUE(denied_getter_value.second.empty());
    EXPECT_EQ(live_peer_getter_value.first, net::OK);
    EXPECT_EQ(live_peer_getter_value.second, "beta-idb-blob");
    base::test::TestFuture<blink::mojom::IDBCursorResultPtr> peer_advanced;
    peer_cursor->Advance(1, peer_advanced.GetCallback());
    auto peer_next = peer_advanced.Take();
    ASSERT_TRUE(peer_next->is_values());
    ASSERT_EQ(peer_next->get_values()->keys.size(), 1u);
    ASSERT_EQ(peer_next->get_values()->values.size(), 1u);
    EXPECT_EQ(peer_next->get_values()->keys[0].number(), 2);
    auto next_peer_blob =
        value_blob(*peer_next->get_values()->values[0], "beta-idb-2");
    NativeBlobRead peer_next_read;
    EXPECT_EQ(peer_next_read.Read(*next_peer_blob.get()), "beta-idb-blob");
    peer_database.CommitAndWait(3, peer_transaction);
    EXPECT_TRUE(peer_database.DatabaseConnected());
    EXPECT_FALSE(profile()->IsOffTheRecord());
    if (!HasFailure()) {
      RecordProperty(
          "retired_idb_cursor_blob_" + namespace_name,
          "cursor-methods-and-old-blob-aliases-denied-peer-input-live");
      RecordProperty("retired_idb_blob_getter_" + namespace_name,
                     "old-getter-denied-no-bytes-peer-getter-live");
    }
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       ImportedBlobPreservesSideDataAndScopeAdmission) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto* initial =
      content::RenderFrameHostTester::For(web_contents()->GetPrimaryMainFrame())
          ->AppendChild("blob-import");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://alpha.test/"), initial);
  auto owner = content::GetAlloyEmbeddedStateForDocument(frame).temporary;
  ASSERT_TRUE(owner);
  auto scoped = base::MakeRefCounted<content::ChromeBlobStorageContext>(
      base::BindRepeating(
          [](RecordCapability capability) {
            return capability.Allows(StorePurpose::kBlobSpill,
                                     StorageAccess::kReadMemory);
          },
          owner->Capability(StorePurpose::kBlobSpill)));
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&content::ChromeBlobStorageContext::InitializeOnIOThread,
                     scoped, base::FilePath(), base::FilePath(), nullptr));
  mojo::Remote<storage::mojom::BlobStorageContext> destination;
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&content::ChromeBlobStorageContext::BindMojoContext,
                     scoped, destination.BindNewPipeAndPassReceiver()));
  mojo::Remote<storage::mojom::BlobStorageContext> input_context(
      content::ChromeBlobStorageContext::GetRemoteFor(profile()));
  const std::string bytes = "native-import-body";
  const std::string metadata = "native-import-side-data";
  auto item = storage::mojom::BlobDataItem::New();
  item->type = storage::mojom::BlobDataItemType::kIndexedDB;
  item->size = bytes.size();
  item->side_data_size = metadata.size();
  item->content_type = "text/plain";
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<NativeMemoryBlobSource>(bytes, metadata),
      item->reader.InitWithNewPipeAndPassReceiver());
  mojo::Remote<blink::mojom::Blob> original, imported;
  input_context->RegisterFromDataItem(
      original.BindNewPipeAndPassReceiver(),
      base::Uuid::GenerateRandomV4().AsLowercaseString(), std::move(item));
  mojo::PendingRemote<blink::mojom::Blob> input;
  original->Clone(input.InitWithNewPipeAndPassReceiver());
  destination->RegisterFromBlob(imported.BindNewPipeAndPassReceiver(),
                                std::move(input), "text/plain", bytes.size());
  auto read_metadata = [](auto& blob) {
    base::test::TestFuture<std::optional<mojo_base::BigBuffer>> value;
    blob->ReadSideData(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        value.GetCallback(), std::optional<mojo_base::BigBuffer>()));
    auto result = value.Take();
    return result ? std::string(reinterpret_cast<const char*>(result->data()),
                                result->size())
                  : std::string();
  };
  NativeBlobRead positive;
  ASSERT_EQ(positive.Read(*imported.get()), bytes);
  ASSERT_EQ(read_metadata(imported), metadata);
  ASSERT_EQ(read_metadata(original), metadata);
  RecordProperty("positive_imported_blob_metadata",
                 "native-import-body-and-side-data-byte-count-readback");
  owner->Retire();
  NativeBlobRead denied, original_after;
  EXPECT_TRUE(denied.Read(*imported.get()).empty());
  EXPECT_TRUE(read_metadata(imported).empty());
  EXPECT_EQ(original_after.Read(*original.get()), bytes);
  EXPECT_EQ(read_metadata(original), metadata);
  if (!HasFailure()) {
    RecordProperty("retired_imported_blob_metadata",
                   "import-denied-original-body-and-side-data-live");
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldIndexedDBFileTransferTokensEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  using Error = blink::mojom::FileSystemAccessErrorPtr;
  using Identity = base::test::TestFuture<Error, const std::string&>;
  using Snapshot = base::test::TestFuture<Error, const base::File::Info&,
                                          blink::mojom::SerializedBlobPtr>;
  struct Held {
    NativeIDBOpen database;
    scoped_refptr<content::FileSystemAccessManagerImpl> manager;
    mojo::Remote<blink::mojom::FileSystemAccessManager> client;
    mojo::Remote<blink::mojom::BucketManagerHost> buckets;
    mojo::Remote<blink::mojom::BucketHost> bucket;
    mojo::Remote<blink::mojom::IDBFactory> factory;
    mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> root;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> file;
    mojo::Remote<blink::mojom::FileSystemAccessTransferToken> returned_token;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> redeemed;
    mojo::Remote<blink::mojom::Blob> blob;
    std::string identity;
  };
  auto aborted = [] {
    auto error = blink::mojom::FileSystemAccessError::New();
    error->status = blink::mojom::FileSystemAccessStatus::kOperationAborted;
    return error;
  };
  auto clone_token = [](auto& token) {
    mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> clone;
    token->Clone(clone.InitWithNewPipeAndPassReceiver());
    return clone;
  };
  auto redeem = [](Held& held, auto token) {
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> file;
    held.client->GetFileHandleFromToken(std::move(token),
                                        file.BindNewPipeAndPassReceiver());
    return file;
  };
  auto snapshot_blob = [](auto& file) {
    Snapshot snapshot;
    file->AsBlob(snapshot.GetCallback());
    auto [error, info, blob] = snapshot.Take();
    EXPECT_EQ(error->status, blink::mojom::FileSystemAccessStatus::kOk);
    CHECK(blob);
    return mojo::Remote<blink::mojom::Blob>(std::move(blob->blob));
  };
  for (bool named : {false, true}) {
    const std::string namespace_name = named ? "named" : "default";
    SCOPED_TRACE(namespace_name);
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                      initial));
    };
    auto* alpha = append("idb-token-alpha", "https://alpha.test/");
    auto* beta = append("idb-token-beta", "https://beta.test/");
    auto owner = alpha->GetBucketStorageOwner().temporary;
    ASSERT_TRUE(owner);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        alpha->GetStoragePartition());
    ASSERT_TRUE(partition->GetStoragePartitionPath());
    auto open = [&](content::RenderFrameHostImpl* frame,
                    const std::string& marker) {
      auto held = std::make_unique<Held>();
      auto* graph = content::GetAlloyEmbeddedBucketStorage(
          *partition, frame->GetBucketStorageOwner().temporary);
      CHECK(graph && graph->file_system_access());
      held->manager = base::WrapRefCounted(graph->file_system_access());
      content::FileSystemAccessManagerImpl::BindingContext binding(
          frame->GetStorageKey(), frame->GetLastCommittedURL(),
          frame->GetGlobalId());
      held->manager->BindReceiver(binding,
                                  held->client.BindNewPipeAndPassReceiver());
      base::test::TestFuture<
          Error,
          mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>
          root;
      if (named) {
        partition->GetBucketManager()->BindReceiver(
            frame->GetWeakPtr(), held->buckets.BindNewPipeAndPassReceiver(),
            base::BindOnce(
                [](std::string_view reason) { ADD_FAILURE() << reason; }));
        base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                               blink::mojom::BucketError>
            bucket;
        held->buckets->OpenBucket("idb_transfers",
                                  blink::mojom::BucketPolicies::New(),
                                  bucket.GetCallback());
        auto [remote, error] = bucket.Take();
        CHECK(remote.is_valid());
        held->bucket.Bind(std::move(remote));
        held->bucket->GetDirectory(root.GetCallback());
        held->bucket->GetIdbFactory(held->factory.BindNewPipeAndPassReceiver());
      } else {
        held->manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                              root.GetCallback());
        content::BindAlloyBucketIndexedDB(
            *partition, *frame,
            storage::BucketLocator::ForDefaultBucket(frame->GetStorageKey()),
            held->factory.BindNewPipeAndPassReceiver());
      }
      auto [root_error, directory] = root.Take();
      CHECK(root_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            directory.is_valid());
      held->root.Bind(std::move(directory));
      base::test::TestFuture<
          Error, mojo::PendingRemote<blink::mojom::FileSystemAccessFileHandle>>
          created;
      held->root->GetFile("idb-transfer.bin", true, created.GetCallback());
      auto [file_error, file] = created.Take();
      CHECK(file_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            file.is_valid());
      held->file.Bind(std::move(file));
      base::test::TestFuture<
          Error, mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter>>
          writer_opened;
      held->file->CreateFileWriter(
          false, false,
          blink::mojom::FileSystemAccessWritableFileStreamLockMode::kExclusive,
          writer_opened.GetCallback());
      auto [writer_error, pending_writer] = writer_opened.Take();
      CHECK(writer_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            pending_writer.is_valid());
      mojo::Remote<blink::mojom::FileSystemAccessFileWriter> writer(
          std::move(pending_writer));
      mojo::ScopedDataPipeProducerHandle producer;
      mojo::ScopedDataPipeConsumerHandle consumer;
      CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer),
               MOJO_RESULT_OK);
      CHECK(mojo::BlockingCopyFromString(marker, producer));
      producer.reset();
      base::test::TestFuture<Error, uint64_t> written;
      writer->Write(0, std::move(consumer), written.GetCallback());
      EXPECT_EQ(written.Get<0>()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      EXPECT_EQ(written.Get<1>(), marker.size());
      base::test::TestFuture<Error> closed;
      writer->Close(closed.GetCallback());
      EXPECT_EQ(closed.Get()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      Identity original_id;
      held->file->GetUniqueId(original_id.GetCallback());
      EXPECT_EQ(original_id.Get<0>()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      held->identity = original_id.Get<1>();
      EXPECT_FALSE(held->identity.empty());

      mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> input;
      held->file->Transfer(input.InitWithNewPipeAndPassReceiver());
      held->database.Open(held->factory, true);
      auto transaction = held->database.BeginTransaction(2);
      auto value = blink::mojom::IDBValue::New();
      value->bits = mojo_base::BigBuffer(base::as_byte_span(marker));
      value->external_objects.push_back(
          blink::mojom::IDBExternalObject::NewFileSystemAccessToken(
              std::move(input)));
      base::test::TestFuture<blink::mojom::IDBTransactionPutResultPtr> stored;
      transaction->Put(1, std::move(value),
                       blink::IndexedDBKey(1, blink::mojom::IDBKeyType::Number),
                       blink::mojom::IDBPutMode::AddOnly, {},
                       stored.GetCallback());
      CHECK(stored.Get()->is_key());
      held->database.CommitAndWait(2, transaction);
      auto read_transaction = held->database.BeginTransaction(3);
      base::test::TestFuture<blink::mojom::IDBDatabaseOpenCursorResultPtr> read;
      held->database.OpenCursor(3, read.GetCallback());
      auto result = read.Take();
      CHECK(result->is_value() && result->get_value()->value);
      auto& returned = *result->get_value()->value;
      EXPECT_EQ(std::string(reinterpret_cast<const char*>(returned.bits.data()),
                            returned.bits.size()),
                marker);
      CHECK_EQ(returned.external_objects.size(), 1u);
      CHECK(returned.external_objects[0]->is_file_system_access_token());
      held->returned_token.Bind(std::move(
          returned.external_objects[0]->get_file_system_access_token()));
      held->database.CommitAndWait(3, read_transaction);
      held->redeemed = redeem(*held, clone_token(held->returned_token));
      Identity redeemed_id;
      held->redeemed->GetUniqueId(redeemed_id.GetCallback());
      EXPECT_EQ(redeemed_id.Get<0>()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      EXPECT_EQ(redeemed_id.Get<1>(), held->identity);
      held->blob = snapshot_blob(held->redeemed);
      return held;
    };
    auto first = open(alpha, "alpha-idb-file"),
         peer = open(beta, "beta-idb-file");
    NativeBlobRead positive, peer_positive;
    ASSERT_EQ(positive.Read(*first->blob.get()), "alpha-idb-file");
    ASSERT_EQ(peer_positive.Read(*peer->blob.get()), "beta-idb-file");
    ASSERT_NE(first->identity, peer->identity);
    mojo::Remote<blink::mojom::FileSystemAccessTransferToken> late_token(
        clone_token(first->returned_token));
    late_token.FlushForTesting();
    ASSERT_TRUE(late_token.is_connected());
    RecordProperty(
        "positive_idb_file_transfer_" + namespace_name,
        "native-committed-token-return-redemption-file-id-and-bytes-peer");
    owner->Retire();
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    Identity denied_id, denied_late_id;
    first->redeemed->GetUniqueId(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_id.GetCallback(), aborted(), std::string()));
    auto late_file = redeem(*first, late_token.Unbind());
    late_file->GetUniqueId(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_late_id.GetCallback(), aborted(), std::string()));
    EXPECT_NE(denied_id.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_NE(denied_late_id.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    Snapshot denied_snapshot;
    first->redeemed->AsBlob(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_snapshot.GetCallback(), aborted(), base::File::Info(),
        blink::mojom::SerializedBlobPtr()));
    EXPECT_NE(denied_snapshot.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_FALSE(denied_snapshot.Get<2>());
    NativeBlobRead denied, peer_after;
    EXPECT_TRUE(denied.Read(*first->blob.get()).empty());
    EXPECT_EQ(peer_after.Read(*peer->blob.get()), "beta-idb-file");
    first->returned_token.FlushForTesting();
    EXPECT_FALSE(first->returned_token.is_connected());
    auto peer_file = redeem(*peer, clone_token(peer->returned_token));
    Identity peer_id;
    peer_file->GetUniqueId(peer_id.GetCallback());
    ASSERT_EQ(peer_id.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    ASSERT_EQ(peer_id.Get<1>(), peer->identity);
    auto peer_blob = snapshot_blob(peer_file);
    NativeBlobRead peer_fresh;
    ASSERT_EQ(peer_fresh.Read(*peer_blob.get()), "beta-idb-file");
    EXPECT_FALSE(profile()->IsOffTheRecord());
    if (!HasFailure()) {
      RecordProperty(
          "retired_idb_file_transfer_" + namespace_name,
          "returned-token-and-file-denied-peer-redemption-and-bytes-live");
    }
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       PendingBlobSourceCannotPublishBytesAfterScopeRetirement) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  for (auto delivery :
       {NativeBlobRead::Delivery::kAll, NativeBlobRead::Delivery::kRange,
        NativeBlobRead::Delivery::kGetter}) {
    for (bool imported : {false, true}) {
      const std::string storage_variant = imported ? "imported" : "direct";
      const std::string variant =
          delivery == NativeBlobRead::Delivery::kAll
              ? storage_variant
              : (delivery == NativeBlobRead::Delivery::kRange ? "range_"
                                                              : "getter_") +
                    storage_variant;
      SCOPED_TRACE(variant);
      auto append = [&](const char* name, const char* url) {
        auto* initial = content::RenderFrameHostTester::For(
                            web_contents()->GetPrimaryMainFrame())
                            ->AppendChild(name);
        return content::NavigationSimulator::NavigateAndCommitFromDocument(
            GURL(url), initial);
      };
      auto* alpha = append("pending-blob-alpha", "https://alpha.test/");
      auto* beta = append("pending-blob-beta", "https://beta.test/");
      auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
      auto peer_owner =
          content::GetAlloyEmbeddedStateForDocument(beta).temporary;
      ASSERT_TRUE(owner && peer_owner);
      const std::string bytes = "pending-native-blob-bytes";
      NativeDeferredBlobSource source(bytes);
      base::SequenceBound<NativeStreamEligibility> eligibility(
          content::GetIOThreadTaskRunner({}),
          owner->Capability(StorePurpose::kBlobSpill), source.BindReader(),
          source.BindReader(), bytes.size());
      base::test::TestFuture<NativeStreamEligibilityResult> eligibility_result;
      eligibility.AsyncCall(&NativeStreamEligibility::Probe)
          .Then(eligibility_result.GetCallback());
      const auto eligibility_sample = eligibility_result.Take();
      ASSERT_TRUE(eligibility_sample.scoped_shape &&
                  eligibility_sample.ordinary_shape);
      ASSERT_FALSE(eligibility_sample.scoped_direct);
      ASSERT_TRUE(eligibility_sample.ordinary_direct);
      ASSERT_EQ(eligibility_sample.scoped_result, net::ERR_ACCESS_DENIED);
      eligibility.Reset();
      RecordProperty("eligibility_pending_blob_" + variant,
                     "scoped-direct-denied-ordinary-direct-eligible");
      std::vector<scoped_refptr<content::ChromeBlobStorageContext>> contexts;
      auto scoped_context = [&](auto scope) {
        auto context = base::MakeRefCounted<content::ChromeBlobStorageContext>(
            base::BindRepeating(
                [](RecordCapability capability) {
                  return capability.Allows(StorePurpose::kBlobSpill,
                                           StorageAccess::kReadMemory);
                },
                scope->Capability(StorePurpose::kBlobSpill)));
        content::GetIOThreadTaskRunner({})->PostTask(
            FROM_HERE,
            base::BindOnce(
                &content::ChromeBlobStorageContext::InitializeOnIOThread,
                context, base::FilePath(), base::FilePath(), nullptr));
        mojo::Remote<storage::mojom::BlobStorageContext> remote;
        content::GetIOThreadTaskRunner({})->PostTask(
            FROM_HERE,
            base::BindOnce(&content::ChromeBlobStorageContext::BindMojoContext,
                           context, remote.BindNewPipeAndPassReceiver()));
        contexts.push_back(std::move(context));
        return remote;
      };
      auto first_context = scoped_context(owner);
      auto peer_context = scoped_context(peer_owner);
      mojo::Remote<storage::mojom::BlobStorageContext> input_context(
          content::ChromeBlobStorageContext::GetRemoteFor(profile()));
      auto register_item = [&](auto& context) {
        auto item = storage::mojom::BlobDataItem::New();
        item->type = storage::mojom::BlobDataItemType::kIndexedDB;
        item->size = bytes.size();
        item->content_type = "application/octet-stream";
        item->reader = source.BindReader();
        mojo::Remote<blink::mojom::Blob> blob;
        context->RegisterFromDataItem(
            blob.BindNewPipeAndPassReceiver(),
            base::Uuid::GenerateRandomV4().AsLowercaseString(),
            std::move(item));
        return blob;
      };
      auto original = register_item(input_context);
      auto create = [&](auto& context) {
        if (!imported) {
          return register_item(context);
        }
        mojo::PendingRemote<blink::mojom::Blob> input;
        original->Clone(input.InitWithNewPipeAndPassReceiver());
        mojo::Remote<blink::mojom::Blob> blob;
        context->RegisterFromBlob(blob.BindNewPipeAndPassReceiver(),
                                  std::move(input), "application/octet-stream",
                                  bytes.size());
        return blob;
      };
      auto first = create(first_context), peer = create(peer_context);
      NativeBlobRead positive, peer_positive, original_positive;
      ASSERT_EQ(positive.Read(*first.get()), bytes);
      ASSERT_EQ(peer_positive.Read(*peer.get()), bytes);
      ASSERT_EQ(original_positive.Read(*original.get()), bytes);
      NativeBlobRead endpoint_positive;
      ASSERT_EQ(
          endpoint_positive.ReadAtBoundary(*first.get(), base::DoNothing(),
                                           delivery, bytes.size() - 1),
          delivery == NativeBlobRead::Delivery::kRange ? bytes.substr(1)
                                                       : bytes);
      RecordProperty("positive_pending_blob_" + variant,
                     "scoped-peer-and-original-native-bytes-before-retirement");
      base::test::TestFuture<void> arrived;
      source.DeferNextRead(arrived.GetCallback());
      NativeBlobRead pending;
      const auto late = pending.ReadAtBoundary(
          *first.get(),
          base::BindOnce(
              [](base::test::TestFuture<void>* arrived,
                 scoped_refptr<EmbeddedState> owner, std::string variant,
                 NativeDeferredBlobSource* source) {
                CHECK(arrived->Wait());
                EXPECT_TRUE(
                    owner->Capability(StorePurpose::kBlobSpill).active());
                RecordProperty(
                    "started_pending_blob_" + variant,
                    "real-data-item-source-holds-request-before-retirement");
                owner->Retire();
                source->CompletePendingRead();
              },
              base::Unretained(&arrived), owner, variant,
              base::Unretained(&source)),
          delivery, bytes.size() - 1);
      EXPECT_TRUE(late.empty())
          << "source bytes reached consumer after scope retirement: " << late;
      ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
      EXPECT_FALSE(owner->Capability(StorePurpose::kBlobSpill).active());
      NativeBlobRead peer_after, original_after;
      ASSERT_EQ(peer_after.Read(*peer.get()), bytes);
      ASSERT_EQ(original_after.Read(*original.get()), bytes);
      ASSERT_FALSE(profile()->IsOffTheRecord());
      if (!HasFailure()) {
        RecordProperty(
            "retired_pending_blob_" + variant,
            "late-source-bytes-unavailable-peer-original-still-readable");
      }
    }
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       BackpressuredBlobStreamKeepsPrefixButPublishesNoRetiredSuffix) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  for (bool imported : {false, true}) {
    const std::string variant = imported ? "imported" : "direct";
    SCOPED_TRACE(variant);
    MojoCreateDataPipeOptions options{sizeof(MojoCreateDataPipeOptions),
                                      MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1, 64};
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    ASSERT_EQ(mojo::CreateDataPipe(&options, producer, consumer),
              MOJO_RESULT_OK);
    // Requested capacity is a minimum. Observe the actual full-pipe boundary
    // before transferring the producer to the native Blob reader.
    size_t capacity = 0;
    const std::string calibration(64, 'C');
    while (true) {
      size_t written = 0;
      const auto result = producer->WriteData(
          base::as_byte_span(calibration), MOJO_WRITE_DATA_FLAG_NONE, written);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        break;
      }
      ASSERT_EQ(result, MOJO_RESULT_OK);
      ASSERT_GT(written, 0u);
      capacity += written;
      ASSERT_LE(capacity, 1024u);
    }
    ASSERT_GT(capacity, 0u);
    std::vector<uint8_t> calibration_bytes(capacity);
    size_t drained = 0;
    ASSERT_EQ(consumer->ReadData(MOJO_READ_DATA_FLAG_ALL_OR_NONE,
                                 base::span(calibration_bytes), drained),
              MOJO_RESULT_OK);
    ASSERT_EQ(drained, capacity);
    const std::string prefix(capacity, 'P');
    const std::string bytes = prefix + std::string(3 * capacity, 'S');
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return content::NavigationSimulator::NavigateAndCommitFromDocument(
          GURL(url), initial);
    };
    auto* alpha = append("backpressure-alpha", "https://alpha.test/");
    auto* beta = append("backpressure-beta", "https://beta.test/");
    auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
    auto peer_owner = content::GetAlloyEmbeddedStateForDocument(beta).temporary;
    ASSERT_TRUE(owner && peer_owner);
    NativeDeferredBlobSource source(bytes);
    std::vector<scoped_refptr<content::ChromeBlobStorageContext>> contexts;
    auto scoped_context = [&](auto scope) {
      auto context = base::MakeRefCounted<content::ChromeBlobStorageContext>(
          base::BindRepeating(
              [](RecordCapability capability) {
                return capability.Allows(StorePurpose::kBlobSpill,
                                         StorageAccess::kReadMemory);
              },
              scope->Capability(StorePurpose::kBlobSpill)));
      content::GetIOThreadTaskRunner({})->PostTask(
          FROM_HERE,
          base::BindOnce(
              &content::ChromeBlobStorageContext::InitializeOnIOThread, context,
              base::FilePath(), base::FilePath(), nullptr));
      mojo::Remote<storage::mojom::BlobStorageContext> remote;
      content::GetIOThreadTaskRunner({})->PostTask(
          FROM_HERE,
          base::BindOnce(&content::ChromeBlobStorageContext::BindMojoContext,
                         context, remote.BindNewPipeAndPassReceiver()));
      contexts.push_back(std::move(context));
      return remote;
    };
    auto first_context = scoped_context(owner);
    auto peer_context = scoped_context(peer_owner);
    mojo::Remote<storage::mojom::BlobStorageContext> input_context(
        content::ChromeBlobStorageContext::GetRemoteFor(profile()));
    auto register_item = [&](auto& context) {
      auto item = storage::mojom::BlobDataItem::New();
      item->type = storage::mojom::BlobDataItemType::kIndexedDB;
      item->size = bytes.size();
      item->reader = source.BindReader();
      mojo::Remote<blink::mojom::Blob> blob;
      context->RegisterFromDataItem(
          blob.BindNewPipeAndPassReceiver(),
          base::Uuid::GenerateRandomV4().AsLowercaseString(), std::move(item));
      return blob;
    };
    auto original = register_item(input_context);
    auto create = [&](auto& context) {
      if (!imported) {
        return register_item(context);
      }
      mojo::PendingRemote<blink::mojom::Blob> input;
      original->Clone(input.InitWithNewPipeAndPassReceiver());
      mojo::Remote<blink::mojom::Blob> blob;
      context->RegisterFromBlob(blob.BindNewPipeAndPassReceiver(),
                                std::move(input), "application/octet-stream",
                                bytes.size());
      return blob;
    };
    auto first = create(first_context), peer = create(peer_context);
    NativeBlobRead positive, peer_positive, original_positive;
    ASSERT_EQ(positive.Read(*first.get()), bytes);
    ASSERT_EQ(peer_positive.Read(*peer.get()), bytes);
    ASSERT_EQ(original_positive.Read(*original.get()), bytes);
    RecordProperty("positive_backpressured_blob_" + variant,
                   "whole-native-scoped-peer-and-original-bytes");
    base::test::TestFuture<void> arrived;
    source.DeferNextRead(arrived.GetCallback());
    first->ReadAll(std::move(producer), mojo::NullRemote());
    ASSERT_TRUE(arrived.Wait());
    source.PublishPendingPrefix(capacity);
    using Ready = base::test::TestFuture<MojoResult, mojo::HandleSignalsState>;
    Ready ready;
    mojo::SimpleWatcher readable(FROM_HERE,
                                 mojo::SimpleWatcher::ArmingPolicy::MANUAL);
    ASSERT_EQ(readable.Watch(
                  consumer.get(),
                  MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                  MOJO_WATCH_CONDITION_SATISFIED,
                  base::BindRepeating(
                      [](Ready* ready, MojoResult result,
                         const mojo::HandleSignalsState& state) {
                        ready->SetValue(result, state);
                      },
                      base::Unretained(&ready))),
              MOJO_RESULT_OK);
    readable.ArmOrNotify();
    ASSERT_TRUE(ready.Wait());
    ASSERT_EQ(ready.Get<0>(), MOJO_RESULT_OK);
    ASSERT_TRUE(ready.Get<1>().readable());
    ASSERT_FALSE(ready.Get<1>().peer_closed());
    readable.Cancel();
    size_t queued = 0;
    ASSERT_EQ(consumer->ReadData(MOJO_READ_DATA_FLAG_QUERY, {}, queued),
              MOJO_RESULT_OK);
    ASSERT_EQ(queued, capacity);
    ASSERT_TRUE(owner->Capability(StorePurpose::kBlobSpill).active());
    RecordProperty("full_prefix_backpressured_blob_" + variant,
                   "actual-capacity-prefix-queued-before-retirement");
    owner->Retire();
    source.CompletePendingRead();
    NativeBlobRead late, peer_after, original_after;
    ASSERT_EQ(late.Drain(std::move(consumer)), prefix)
        << "suffix was published after retirement under backpressure";
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    ASSERT_EQ(peer_after.Read(*peer.get()), bytes);
    ASSERT_EQ(original_after.Read(*original.get()), bytes);
    ASSERT_FALSE(profile()->IsOffTheRecord());
    RecordProperty("retired_backpressured_blob_" + variant,
                   "admitted-prefix-preserved-no-suffix-peer-original-whole");
  }
}

TEST_F(NativeEmbeddedStateTest, HeldCacheEndpointEndsWithSiteOwner) {
  for (bool named : {true, false}) {
    SCOPED_TRACE(named ? "NAMED_CACHE" : "DEFAULT_CACHE");
    auto* frame =
        static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
    auto* peer =
        static_cast<content::RenderFrameHostImpl*>(Embed(second_.get()));
    const auto owner = frame->GetBucketStorageOwner().temporary;
    ASSERT_TRUE(owner);
    mojo::Remote<blink::mojom::BucketManagerHost> first_manager, second_manager;
    mojo::Remote<blink::mojom::BucketHost> first_bucket, second_bucket;
    mojo::Remote<blink::mojom::CacheStorage> storage, peer_storage;
    if (named) {
      first_manager = Buckets(frame);
      second_manager = Buckets(peer);
      first_bucket = CreateBucket(first_manager, "held_cache");
      second_bucket = CreateBucket(second_manager, "held_cache");
      first_bucket->GetCaches(storage.BindNewPipeAndPassReceiver());
      second_bucket->GetCaches(peer_storage.BindNewPipeAndPassReceiver());
    } else {
      frame->BindCacheStorage(storage.BindNewPipeAndPassReceiver());
      peer->BindCacheStorage(peer_storage.BindNewPipeAndPassReceiver());
    }
    base::test::TestFuture<blink::mojom::CacheStorage::OpenResult> opened,
        peer_opened;
    storage->Open(u"held", 1, opened.GetCallback());
    peer_storage->Open(u"peer", 2, peer_opened.GetCallback());
    auto first = opened.Take(), second = peer_opened.Take();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    mojo::AssociatedRemote<blink::mojom::CacheStorageCache> cache(
        std::move(first.value()));
    mojo::AssociatedRemote<blink::mojom::CacheStorageCache> peer_cache(
        std::move(second.value()));
    auto request = [] {
      auto value = blink::mojom::FetchAPIRequest::New();
      value->url = GURL("https://embedded.test/held-value");
      value->method = "GET";
      return value;
    };
    auto operation = blink::mojom::BatchOperation::New();
    operation->operation_type = blink::mojom::OperationType::kPut;
    operation->request = request();
    operation->response = blink::mojom::FetchAPIResponse::New();
    operation->response->status_code = 200;
    operation->response->status_text = "OK";
    operation->response->headers["x-alloy-marker"] = "held-native-cache-value";
    operation->response->response_time = base::Time::Now();
    std::vector<blink::mojom::BatchOperationPtr> operations;
    operations.push_back(std::move(operation));
    base::test::TestFuture<blink::mojom::CacheStorageVerboseErrorPtr> stored;
    cache->Batch(std::move(operations), 3, stored.GetCallback());
    ASSERT_EQ(stored.Get()->value, blink::mojom::CacheStorageError::kSuccess);
    using Match = blink::mojom::CacheStorageCache::MatchResult;
    base::test::TestFuture<Match> before;
    cache->Match(request(), blink::mojom::CacheQueryOptions::New(), false,
                 false, 4, before.GetCallback());
    ASSERT_TRUE(before.Get().has_value());
    const auto& matched = before.Get().value();
    const auto* response = matched->is_response()
                               ? matched->get_response().get()
                               : matched->get_eager_response()->response.get();
    ASSERT_TRUE(response->headers.contains("x-alloy-marker"));
    ASSERT_EQ(response->headers.at("x-alloy-marker"),
              "held-native-cache-value");
    base::test::TestFuture<blink::mojom::CacheStorageError> peer_before;
    peer_storage->Has(u"peer", 5, peer_before.GetCallback());
    ASSERT_EQ(peer_before.Get(), blink::mojom::CacheStorageError::kSuccess);
    SCOPED_TRACE("HELD_CACHE_AFTER_POSITIVE_MATCH");
    base::test::TestFuture<void> disconnected;
    cache.set_disconnect_handler(disconnected.GetCallback());
    owner->Retire();
    EXPECT_TRUE(frame->IsActive());
    base::test::TestFuture<Match> after;
    cache->Match(request(), blink::mojom::CacheQueryOptions::New(), false,
                 false, 6,
                 mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                     after.GetCallback(),
                     Match(base::unexpected(
                         blink::mojom::CacheStorageError::kErrorStorage))));
    EXPECT_FALSE(after.Get().has_value())
        << "held cache returned data after site retirement";
    ASSERT_TRUE(disconnected.Wait());
    EXPECT_FALSE(cache.is_connected());
    base::test::TestFuture<blink::mojom::CacheStorageError> peer_after;
    peer_storage->Has(u"peer", 7, peer_after.GetCallback());
    EXPECT_EQ(peer_after.Get(), blink::mojom::CacheStorageError::kSuccess);
    peer_cache.FlushForTesting();
    EXPECT_TRUE(peer_cache.is_connected());
    EXPECT_TRUE(first_);
    EXPECT_TRUE(second_);
  }
}

TEST_F(NativeEmbeddedStateTest, QuotaEstimateEndpointEndsWithItsTabScope) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  mojo::Remote<blink::mojom::QuotaManagerHost> quota;
  content::BindAlloyBucketQuotaManagerHost(*partition, *frame,
                                           quota.BindNewPipeAndPassReceiver());
  using Reply =
      base::test::TestFuture<blink::mojom::QuotaStatusCode, int64_t, int64_t,
                             blink::mojom::UsageBreakdownPtr>;
  Reply initial;
  quota->QueryStorageUsageAndQuota(initial.GetCallback());
  EXPECT_EQ(initial.Get<0>(), blink::mojom::QuotaStatusCode::kOk);
  EXPECT_EQ(initial.Get<1>(), 0);
  EXPECT_GT(initial.Get<2>(), 0);
  first_.reset();
  Reply retired;
  quota->QueryStorageUsageAndQuota(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      retired.GetCallback(), blink::mojom::QuotaStatusCode::kErrorAbort,
      int64_t{0}, int64_t{0}, blink::mojom::UsageBreakdownPtr{}));
  EXPECT_NE(retired.Get<0>(), blink::mojom::QuotaStatusCode::kOk);
  EXPECT_EQ(retired.Get<1>(), 0);
  EXPECT_EQ(retired.Get<2>(), 0);
  quota.FlushForTesting();
  EXPECT_FALSE(quota.is_connected());
  EXPECT_TRUE(second_);
}

class NativeDiskEmbeddedCacheMetadataTest
    : public NativeDiskEmbeddedLocalStorageTest,
      public testing::WithParamInterface<Scope> {};

TEST_P(NativeDiskEmbeddedCacheMetadataTest,
       HeldCacheBodyAndMetadataBlobsEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), GetParam()));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  mojo::Remote<storage::mojom::BlobStorageContext> input_context(
      content::ChromeBlobStorageContext::GetRemoteFor(profile()));
  auto make_blob = [&](const std::string& bytes) {
    auto blob = blink::mojom::SerializedBlob::New();
    blob->uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
    blob->size = bytes.size();
    input_context->RegisterFromMemory(
        blob->blob.InitWithNewPipeAndPassReceiver(), blob->uuid,
        base::as_byte_span(bytes));
    return blob;
  };
  for (bool named : {false, true}) {
    const std::string namespace_name = named ? "named" : "default";
    SCOPED_TRACE(namespace_name);
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                      initial));
    };
    auto* alpha = append("cache-body-alpha", "https://alpha.test/");
    auto* beta = append("cache-body-beta", "https://beta.test/");
    auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
    ASSERT_TRUE(owner);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        alpha->GetStoragePartition());
    std::vector<mojo::Remote<blink::mojom::BucketManagerHost>> managers;
    std::vector<mojo::Remote<blink::mojom::BucketHost>> buckets;
    std::vector<mojo::Remote<blink::mojom::CacheStorage>> storages;
    auto open = [&](content::RenderFrameHostImpl* frame) {
      mojo::Remote<blink::mojom::CacheStorage> storage;
      if (named) {
        mojo::Remote<blink::mojom::BucketManagerHost> manager;
        partition->GetBucketManager()->BindReceiver(
            frame->GetWeakPtr(), manager.BindNewPipeAndPassReceiver(),
            base::BindOnce(
                [](std::string_view reason) { ADD_FAILURE() << reason; }));
        base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                               blink::mojom::BucketError>
            opened;
        manager->OpenBucket("held_body", blink::mojom::BucketPolicies::New(),
                            opened.GetCallback());
        auto [remote, error] = opened.Take();
        EXPECT_TRUE(remote.is_valid());
        mojo::Remote<blink::mojom::BucketHost> bucket(std::move(remote));
        bucket->GetCaches(storage.BindNewPipeAndPassReceiver());
        managers.push_back(std::move(manager));
        buckets.push_back(std::move(bucket));
      } else {
        frame->BindCacheStorage(storage.BindNewPipeAndPassReceiver());
      }
      base::test::TestFuture<blink::mojom::CacheStorage::OpenResult> opened;
      storage->Open(u"held-body", 1, opened.GetCallback());
      auto result = opened.Take();
      CHECK(result.has_value());
      storages.push_back(std::move(storage));
      return mojo::AssociatedRemote<blink::mojom::CacheStorageCache>(
          std::move(result.value()));
    };
    auto cache = open(alpha), peer = open(beta);
    auto request = [](content::RenderFrameHostImpl* frame) {
      auto value = blink::mojom::FetchAPIRequest::New();
      value->url = frame->GetLastCommittedURL().Resolve("body-value");
      value->method = "GET";
      return value;
    };
    auto write = [&](auto& endpoint, content::RenderFrameHostImpl* frame,
                     const std::string& marker) {
      auto operation = blink::mojom::BatchOperation::New();
      operation->operation_type = blink::mojom::OperationType::kPut;
      operation->request = request(frame);
      operation->response = blink::mojom::FetchAPIResponse::New();
      operation->response->status_code = 200;
      operation->response->status_text = "OK";
      operation->response->response_time = base::Time::Now();
      operation->response->blob = make_blob(marker);
      const auto input_id = operation->response->blob->uuid;
      operation->response->side_data_blob_for_cache_put =
          make_blob(marker + "-metadata");
      std::vector<blink::mojom::BatchOperationPtr> operations;
      operations.push_back(std::move(operation));
      base::test::TestFuture<blink::mojom::CacheStorageVerboseErrorPtr> stored;
      endpoint->Batch(std::move(operations), 2, stored.GetCallback());
      EXPECT_EQ(stored.Get()->value, blink::mojom::CacheStorageError::kSuccess);
      return input_id;
    };
    const auto input_id = write(cache, alpha, "alpha-cache-body");
    write(peer, beta, "beta-cache-body");
    auto match = [&](auto& endpoint, content::RenderFrameHostImpl* frame) {
      base::test::TestFuture<blink::mojom::CacheStorageCache::MatchResult>
          matched;
      endpoint->Match(request(frame), blink::mojom::CacheQueryOptions::New(),
                      false, false, 3, matched.GetCallback());
      auto result = matched.Take();
      CHECK(result.has_value() && result.value()->is_response());
      return std::move(result.value()->get_response());
    };
    auto response = match(cache, alpha), peer_response = match(peer, beta);
    ASSERT_TRUE(response->blob && response->side_data_blob &&
                peer_response->blob && peer_response->side_data_blob);
    ASSERT_NE(response->blob->uuid, input_id);
    mojo::Remote<blink::mojom::Blob> body(std::move(response->blob->blob));
    mojo::Remote<blink::mojom::Blob> metadata(
        std::move(response->side_data_blob->blob));
    mojo::Remote<blink::mojom::Blob> peer_body(
        std::move(peer_response->blob->blob));
    mojo::Remote<blink::mojom::Blob> peer_metadata(
        std::move(peer_response->side_data_blob->blob));
    auto read_metadata = [](auto& blob) {
      base::test::TestFuture<std::optional<mojo_base::BigBuffer>> value;
      blob->ReadSideData(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          value.GetCallback(), std::optional<mojo_base::BigBuffer>()));
      auto bytes = value.Take();
      return bytes ? std::string(reinterpret_cast<const char*>(bytes->data()),
                                 bytes->size())
                   : std::string();
    };
    NativeBlobRead positive, peer_positive;
    ASSERT_EQ(positive.Read(*body.get()), "alpha-cache-body");
    ASSERT_EQ(peer_positive.Read(*peer_body.get()), "beta-cache-body");
    ASSERT_EQ(read_metadata(metadata), "alpha-cache-body-metadata");
    ASSERT_EQ(read_metadata(peer_metadata), "beta-cache-body-metadata");
    RecordProperty("positive_cache_blob_" + namespace_name,
                   "native-body-metadata-and-peer-readback");
    // Chromium 156.0.8078.48 gives each matched response its original cache's
    // one-shot writer (cache_storage_cache.cc, SideDataWriter). CodeCacheHost
    // no longer selects a CacheStorageControl by process/profile lookup.
    ASSERT_TRUE(response->cache_storage_side_data_writer.is_valid());
    mojo::Remote<network::mojom::CacheStorageSideDataWriter> writer(
        std::move(response->cache_storage_side_data_writer));
    const std::string generated = "alpha-native-generated-code";
    writer->WriteSideData(mojo_base::BigBuffer(base::as_byte_span(generated)));
    writer.FlushForTesting();
    ASSERT_FALSE(writer.is_connected());
    ASSERT_TRUE(base::test::RunUntil([&] {
      auto current = match(cache, alpha);
      if (!current->side_data_blob)
        return false;
      mojo::Remote<blink::mojom::Blob> data(
          std::move(current->side_data_blob->blob));
      return read_metadata(data) == generated;
    }));
    auto current = match(cache, alpha);
    ASSERT_TRUE(current->cache_storage_side_data_writer.is_valid());
    mojo::Remote<network::mojom::CacheStorageSideDataWriter> late_writer(
        std::move(current->cache_storage_side_data_writer));
    base::test::TestFuture<void> writer_closed;
    late_writer.set_disconnect_handler(writer_closed.GetCallback());
    RecordProperty("positive_cache_side_writer_" + namespace_name,
                   "native-one-shot-write-readback-and-retained-writer");
    owner->Retire();
    ASSERT_TRUE(writer_closed.Wait());
    EXPECT_FALSE(late_writer.is_connected());
    late_writer->WriteSideData(mojo_base::BigBuffer(
        base::as_byte_span(std::string("late-generated-code"))));
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    NativeBlobRead denied, peer_after;
    EXPECT_EQ(denied.Read(*body.get()), std::string())
        << "held body read after owner retirement";
    EXPECT_EQ(read_metadata(metadata), std::string())
        << "held metadata read after owner retirement";
    ASSERT_EQ(peer_after.Read(*peer_body.get()), "beta-cache-body");
    ASSERT_EQ(read_metadata(peer_metadata), "beta-cache-body-metadata");
    if (!HasFailure())
      RecordProperty("retired_cache_blob_" + namespace_name,
                     "old-bytes-unavailable-peer-bytes-live");
  }
}

INSTANTIATE_TEST_SUITE_P(BothScopes,
                         NativeDiskEmbeddedCacheMetadataTest,
                         testing::Values(Scope::kPerTab, Scope::kPerOuterSite));

TEST_F(NativeEmbeddedStateTest, FileSystemReplyRechecksCapturedAdmission) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto owner = frame->GetBucketStorageOwner();
  auto* store =
      content::GetAlloyEmbeddedBucketStorage(*partition, owner.temporary);
  ASSERT_TRUE(store);
  auto manager = base::WrapRefCounted(store->file_system_access());
  ASSERT_TRUE(manager);
  content::FileSystemAccessManagerImpl::BindingContext binding(
      frame->GetStorageKey(), frame->GetLastCommittedURL(),
      frame->GetGlobalId());
  using Reply = base::test::TestFuture<
      blink::mojom::FileSystemAccessErrorPtr,
      mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>;
  Reply positive;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                  positive.GetCallback());
  ASSERT_EQ(positive.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOk);
  ASSERT_TRUE(positive.Get<1>().is_valid());

  bool admitted = true;
  binding.storage_admission =
      base::BindRepeating([](bool* value) { return *value; }, &admitted);
  Reply pending;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                  pending.GetCallback());
  // Admission succeeded; only the native IO reply can now observe revocation.
  admitted = false;
  ASSERT_EQ(pending.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOperationAborted);
  EXPECT_FALSE(pending.Get<1>().is_valid());
  EXPECT_TRUE(owner.temporary->Capability(StorePurpose::kFileSystem).active());
}

TEST_F(NativeEmbeddedStateTest,
       FileSystemNestedReplyRechecksCapturedAdmission) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto* store = content::GetAlloyEmbeddedBucketStorage(
      *partition, frame->GetBucketStorageOwner().temporary);
  ASSERT_TRUE(store);
  auto manager = base::WrapRefCounted(store->file_system_access());
  content::FileSystemAccessManagerImpl::BindingContext binding(
      frame->GetStorageKey(), frame->GetLastCommittedURL(),
      frame->GetGlobalId());
  using Reply = base::test::TestFuture<
      blink::mojom::FileSystemAccessErrorPtr,
      mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>;
  Reply positive;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                  positive.GetCallback());
  auto [error, remote] = positive.Take();
  ASSERT_EQ(error->status, blink::mojom::FileSystemAccessStatus::kOk);
  mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> root(
      std::move(remote));
  Reply created;
  root->GetDirectory("nested", true, created.GetCallback());
  ASSERT_EQ(created.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOk);
  ASSERT_TRUE(created.Get<1>().is_valid());

  int checks = 0;
  bool admitted = true;
  binding.storage_admission = base::BindRepeating(
      [](int* checks, bool* admitted) {
        // Revoke on the UI queue after root-open admission but before the
        // nested DirectoryExists reply. Native filesystem work, not a fake
        // reply, runs.
        if (++*checks == 2) {
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE,
              base::BindOnce([](bool* value) { *value = false; }, admitted));
        }
        return *admitted;
      },
      &checks, &admitted);
  Reply pending;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {"nested"},
                                  pending.GetCallback());
  ASSERT_EQ(pending.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOperationAborted);
  EXPECT_FALSE(pending.Get<1>().is_valid());
}

TEST_F(NativeEmbeddedStateTest, FileSystemHandlesEndWithTheirTabScope) {
  auto* frame = static_cast<content::RenderFrameHostImpl*>(Embed(first_.get()));
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto owner = frame->GetBucketStorageOwner();
  auto* store =
      content::GetAlloyEmbeddedBucketStorage(*partition, owner.temporary);
  ASSERT_TRUE(store);
  // Retain the native manager across retirement: explicit scope shutdown must
  // close the receiver graph even when refcounted clients keep it alive.
  auto manager = base::WrapRefCounted(store->file_system_access());
  content::FileSystemAccessManagerImpl::BindingContext binding(
      frame->GetStorageKey(), frame->GetLastCommittedURL(),
      frame->GetGlobalId());
  using Reply = base::test::TestFuture<
      blink::mojom::FileSystemAccessErrorPtr,
      mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>;
  Reply opened;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                  opened.GetCallback());
  auto [error, remote] = opened.Take();
  ASSERT_EQ(error->status, blink::mojom::FileSystemAccessStatus::kOk);
  mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> root(
      std::move(remote));
  Reply created;
  root->GetDirectory("scope-data", true, created.GetCallback());
  ASSERT_EQ(created.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOk);
  mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> child(
      std::move(std::get<1>(created.Take())));
  Reply read;
  root->GetDirectory("scope-data", false, read.GetCallback());
  ASSERT_EQ(read.Get<0>()->status, blink::mojom::FileSystemAccessStatus::kOk);
  ASSERT_TRUE(read.Get<1>().is_valid());
  first_.reset();
  root.FlushForTesting();
  child.FlushForTesting();
  EXPECT_FALSE(root.is_connected());
  EXPECT_FALSE(child.is_connected());
  Reply retired;
  manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                  retired.GetCallback());
  EXPECT_NE(retired.Get<0>()->status,
            blink::mojom::FileSystemAccessStatus::kOk);
  EXPECT_FALSE(retired.Get<1>().is_valid());
  EXPECT_TRUE(second_);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldOPFSFileBrokerAndTransferEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  struct Handles {
    scoped_refptr<content::FileSystemAccessManagerImpl> manager;
    mojo::Remote<blink::mojom::FileSystemAccessManager> client;
    mojo::Remote<blink::mojom::BucketManagerHost> buckets;
    mojo::Remote<blink::mojom::BucketHost> bucket;
    mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> root;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> file;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> redeemed;
    mojo::Remote<blink::mojom::FileSystemAccessTransferToken> token;
    mojo::Remote<blink::mojom::FileSystemAccessTransferToken> late_token;
    mojo::Remote<blink::mojom::FileSystemAccessAccessHandleHost> access;
    mojo::Remote<blink::mojom::FileSystemAccessFileDelegateHost> delegate;
  };
  using Error = blink::mojom::FileSystemAccessErrorPtr;
  using Identity = base::test::TestFuture<Error, const std::string&>;
  using Read = base::test::TestFuture<std::optional<mojo_base::BigBuffer>,
                                      base::File::Error, int32_t>;
  auto aborted = [] {
    auto error = blink::mojom::FileSystemAccessError::New();
    error->status = blink::mojom::FileSystemAccessStatus::kOperationAborted;
    return error;
  };
  for (bool named : {false, true}) {
    const std::string namespace_name = named ? "named" : "default";
    SCOPED_TRACE(namespace_name);
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                      initial));
    };
    auto* alpha = append("opfs-alpha", "https://alpha.test/");
    auto* beta = append("opfs-beta", "https://beta.test/");
    const auto owner =
        content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
    ASSERT_TRUE(owner);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        alpha->GetStoragePartition());
    auto open = [&](content::RenderFrameHostImpl* frame) {
      auto handles = std::make_unique<Handles>();
      const auto selection = frame->GetBucketStorageOwner();
      auto* graph = content::GetAlloyEmbeddedBucketStorage(*partition,
                                                           selection.temporary);
      CHECK(graph && graph->file_system_access());
      handles->manager = base::WrapRefCounted(graph->file_system_access());
      content::FileSystemAccessManagerImpl::BindingContext binding(
          frame->GetStorageKey(), frame->GetLastCommittedURL(),
          frame->GetGlobalId());
      handles->manager->BindReceiver(
          binding, handles->client.BindNewPipeAndPassReceiver());
      base::test::TestFuture<
          Error,
          mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>
          root;
      if (named) {
        partition->GetBucketManager()->BindReceiver(
            frame->GetWeakPtr(), handles->buckets.BindNewPipeAndPassReceiver(),
            base::BindOnce(
                [](std::string_view reason) { ADD_FAILURE() << reason; }));
        base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                               blink::mojom::BucketError>
            bucket;
        handles->buckets->OpenBucket("opfs_handles",
                                     blink::mojom::BucketPolicies::New(),
                                     bucket.GetCallback());
        auto [remote, error] = bucket.Take();
        CHECK(remote.is_valid());
        handles->bucket.Bind(std::move(remote));
        handles->bucket->GetDirectory(root.GetCallback());
      } else {
        handles->manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                                 root.GetCallback());
      }
      auto [root_error, directory] = root.Take();
      CHECK(root_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            directory.is_valid());
      handles->root.Bind(std::move(directory));
      base::test::TestFuture<
          Error, mojo::PendingRemote<blink::mojom::FileSystemAccessFileHandle>>
          file;
      handles->root->GetFile("held.bin", true, file.GetCallback());
      auto [file_error, file_remote] = file.Take();
      CHECK(file_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            file_remote.is_valid());
      handles->file.Bind(std::move(file_remote));
      handles->file->Transfer(handles->token.BindNewPipeAndPassReceiver());
      mojo::Remote<blink::mojom::FileSystemAccessTransferToken> token_copy;
      handles->token->Clone(token_copy.BindNewPipeAndPassReceiver());
      handles->client->GetFileHandleFromToken(
          token_copy.Unbind(), handles->redeemed.BindNewPipeAndPassReceiver());
      Identity original_id, redeemed_id;
      handles->file->GetUniqueId(original_id.GetCallback());
      handles->redeemed->GetUniqueId(redeemed_id.GetCallback());
      EXPECT_EQ(original_id.Get<0>()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      EXPECT_EQ(redeemed_id.Get<0>()->status,
                blink::mojom::FileSystemAccessStatus::kOk);
      EXPECT_FALSE(original_id.Get<1>().empty());
      EXPECT_EQ(original_id.Get<1>(), redeemed_id.Get<1>());
      handles->token->Clone(handles->late_token.BindNewPipeAndPassReceiver());
      handles->late_token.FlushForTesting();
      EXPECT_TRUE(handles->late_token.is_connected());
      base::test::TestFuture<
          Error, blink::mojom::FileSystemAccessAccessHandleFilePtr,
          mojo::PendingRemote<blink::mojom::FileSystemAccessAccessHandleHost>>
          access;
      handles->file->OpenAccessHandle(
          blink::mojom::FileSystemAccessAccessHandleLockMode::kReadwrite,
          access.GetCallback());
      auto [access_error, access_file, host] = access.Take();
      CHECK(access_error->status == blink::mojom::FileSystemAccessStatus::kOk);
      CHECK(access_file && access_file->is_incognito_file_delegate() &&
            host.is_valid());
      handles->access.Bind(std::move(host));
      handles->delegate.Bind(
          std::move(access_file->get_incognito_file_delegate()));
      return handles;
    };
    auto first = open(alpha), peer = open(beta);
    auto write = [](auto& delegate, const std::string& bytes) {
      mojo::ScopedDataPipeProducerHandle producer;
      mojo::ScopedDataPipeConsumerHandle consumer;
      CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer),
               MOJO_RESULT_OK);
      CHECK(mojo::BlockingCopyFromString(bytes, producer));
      producer.reset();
      base::test::TestFuture<base::File::Error, int32_t> result;
      delegate->Write(0, std::move(consumer), result.GetCallback());
      EXPECT_EQ(result.Get<0>(), base::File::FILE_OK);
      EXPECT_EQ(result.Get<1>(), static_cast<int32_t>(bytes.size()));
    };
    auto read = [](auto& delegate) {
      Read result;
      delegate->Read(0, 1024, result.GetCallback());
      auto [bytes, error, size] = result.Take();
      EXPECT_EQ(error, base::File::FILE_OK);
      CHECK(bytes && size >= 0 && static_cast<size_t>(size) <= bytes->size());
      return std::string(reinterpret_cast<const char*>(bytes->data()), size);
    };
    write(first->delegate, "alpha-held-opfs");
    write(peer->delegate, "beta-held-opfs");
    ASSERT_EQ(read(first->delegate), "alpha-held-opfs");
    ASSERT_EQ(read(peer->delegate), "beta-held-opfs");
    base::test::TestFuture<base::File::Error> truncated;
    first->delegate->SetLength(5, truncated.GetCallback());
    ASSERT_EQ(truncated.Get(), base::File::FILE_OK);
    ASSERT_EQ(read(first->delegate), "alpha");
    RecordProperty(
        "positive_opfs_handles_" + namespace_name,
        "file-transfer-redemption-memory-broker-write-read-truncate-peer");
    owner->Retire();
    // No queue drain before these new operations: every retained capability is
    // challenged while both documents and the refcounted native manager live.
    Read denied;
    base::test::TestFuture<base::File::Error> denied_write;
    Identity denied_file, denied_redeemed, denied_late;
    first->delegate->Read(
        0, 1024,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            denied.GetCallback(), std::optional<mojo_base::BigBuffer>(),
            base::File::FILE_ERROR_ABORT, int32_t{0}));
    first->delegate->SetLength(
        0, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
               denied_write.GetCallback(), base::File::FILE_ERROR_ABORT));
    first->file->GetUniqueId(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_file.GetCallback(), aborted(), std::string()));
    first->redeemed->GetUniqueId(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_redeemed.GetCallback(), aborted(), std::string()));
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> late;
    first->client->GetFileHandleFromToken(first->late_token.Unbind(),
                                          late.BindNewPipeAndPassReceiver());
    late->GetUniqueId(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_late.GetCallback(), aborted(), std::string()));
    EXPECT_NE(denied.Get<1>(), base::File::FILE_OK);
    EXPECT_FALSE(denied.Get<0>());
    EXPECT_EQ(denied.Get<2>(), 0);
    EXPECT_NE(denied_write.Get(), base::File::FILE_OK);
    EXPECT_NE(denied_file.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_NE(denied_redeemed.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_NE(denied_late.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    first->token.FlushForTesting();
    first->access.FlushForTesting();
    first->delegate.FlushForTesting();
    EXPECT_FALSE(first->token.is_connected());
    EXPECT_FALSE(first->access.is_connected());
    EXPECT_FALSE(first->delegate.is_connected());
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    ASSERT_FALSE(profile()->IsOffTheRecord());
    ASSERT_EQ(read(peer->delegate), "beta-held-opfs");
    peer->token.FlushForTesting();
    EXPECT_TRUE(peer->token.is_connected());
    if (!HasFailure())
      RecordProperty("retired_opfs_handles_" + namespace_name,
                     "file-transfer-broker-closed-peer-bytes-live");
  }
}

TEST_F(NativeOuterEmbeddedStateTest, NamedBucketsUseLiveOuterScope) {
  auto first = Buckets(Embed(first_.get()));
  auto second = Buckets(Embed(second_.get()));
  auto bucket = CreateBucket(first, "shared_bucket");
  first_.reset();
  base::test::TestFuture<std::vector<std::string>, bool> keys;
  second->Keys(keys.GetCallback<const std::vector<std::string>&, bool>());
  EXPECT_TRUE(keys.Get<1>());
  EXPECT_EQ(keys.Get<0>(), (std::vector<std::string>{"shared_bucket"}));
  content::ShutdownAlloyEmbeddedState(*child_);
  bucket.FlushForTesting();
  first.FlushForTesting();
  second.FlushForTesting();
  EXPECT_FALSE(bucket.is_connected());
  EXPECT_FALSE(first.is_connected());
  EXPECT_FALSE(second.is_connected());
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       HeldManagedFileBlobsAndWritersEndWithSiteOwner) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  using Error = blink::mojom::FileSystemAccessErrorPtr;
  using Snapshot = base::test::TestFuture<Error, const base::File::Info&,
                                          blink::mojom::SerializedBlobPtr>;
  struct Files {
    scoped_refptr<content::FileSystemAccessManagerImpl> manager;
    mojo::Remote<blink::mojom::BucketManagerHost> buckets;
    mojo::Remote<blink::mojom::BucketHost> bucket;
    mojo::Remote<blink::mojom::FileSystemAccessDirectoryHandle> root;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> snapshot_file;
    mojo::Remote<blink::mojom::FileSystemAccessFileHandle> pending_file;
    mojo::Remote<blink::mojom::FileSystemAccessFileWriter> writer;
    mojo::Remote<blink::mojom::Blob> blob;
    mojo::Remote<blink::mojom::Blob> clone;
    mojo::Remote<network::mojom::DataPipeGetter> getter;
  };
  auto aborted = [] {
    auto result = blink::mojom::FileSystemAccessError::New();
    result->status = blink::mojom::FileSystemAccessStatus::kOperationAborted;
    return result;
  };
  auto stream = [](const std::string& bytes) {
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    CHECK(mojo::BlockingCopyFromString(bytes, producer));
    producer.reset();
    return consumer;
  };
  auto writer_for = [](auto& file) {
    base::test::TestFuture<
        Error, mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter>>
        opened;
    file->CreateFileWriter(
        false, false,
        blink::mojom::FileSystemAccessWritableFileStreamLockMode::kExclusive,
        opened.GetCallback());
    auto [error, remote] = opened.Take();
    CHECK(error->status == blink::mojom::FileSystemAccessStatus::kOk &&
          remote.is_valid());
    return mojo::Remote<blink::mojom::FileSystemAccessFileWriter>(
        std::move(remote));
  };
  auto write = [&](auto& writer, const std::string& bytes) {
    base::test::TestFuture<Error, uint64_t> result;
    writer->Write(0, stream(bytes), result.GetCallback());
    EXPECT_EQ(result.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_EQ(result.Get<1>(), bytes.size());
  };
  auto close = [](auto& writer) {
    base::test::TestFuture<Error> result;
    writer->Close(result.GetCallback());
    EXPECT_EQ(result.Get()->status, blink::mojom::FileSystemAccessStatus::kOk);
  };
  for (bool named : {false, true}) {
    const std::string namespace_name = named ? "named" : "default";
    SCOPED_TRACE(namespace_name);
    auto append = [&](const char* name, const char* url) {
      auto* initial = content::RenderFrameHostTester::For(
                          web_contents()->GetPrimaryMainFrame())
                          ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(GURL(url),
                                                                      initial));
    };
    auto* alpha = append("file-blob-alpha", "https://alpha.test/");
    auto* beta = append("file-blob-beta", "https://beta.test/");
    auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
    ASSERT_TRUE(owner);
    auto* partition = static_cast<content::StoragePartitionImpl*>(
        alpha->GetStoragePartition());
    auto open = [&](content::RenderFrameHostImpl* frame,
                    const std::string& marker) {
      auto files = std::make_unique<Files>();
      auto* graph = content::GetAlloyEmbeddedBucketStorage(
          *partition, frame->GetBucketStorageOwner().temporary);
      CHECK(graph && graph->file_system_access());
      files->manager = base::WrapRefCounted(graph->file_system_access());
      content::FileSystemAccessManagerImpl::BindingContext binding(
          frame->GetStorageKey(), frame->GetLastCommittedURL(),
          frame->GetGlobalId());
      base::test::TestFuture<
          Error,
          mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>>
          root;
      if (named) {
        partition->GetBucketManager()->BindReceiver(
            frame->GetWeakPtr(), files->buckets.BindNewPipeAndPassReceiver(),
            base::BindOnce(
                [](std::string_view reason) { ADD_FAILURE() << reason; }));
        base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                               blink::mojom::BucketError>
            bucket;
        files->buckets->OpenBucket("file_blobs",
                                   blink::mojom::BucketPolicies::New(),
                                   bucket.GetCallback());
        auto [remote, error] = bucket.Take();
        CHECK(remote.is_valid());
        files->bucket.Bind(std::move(remote));
        files->bucket->GetDirectory(root.GetCallback());
      } else {
        files->manager->GetSandboxedFileSystem(binding, std::nullopt, {},
                                               root.GetCallback());
      }
      auto [root_error, root_remote] = root.Take();
      CHECK(root_error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            root_remote.is_valid());
      files->root.Bind(std::move(root_remote));
      auto file_for = [&](const char* name) {
        base::test::TestFuture<
            Error,
            mojo::PendingRemote<blink::mojom::FileSystemAccessFileHandle>>
            result;
        files->root->GetFile(name, true, result.GetCallback());
        auto [error, remote] = result.Take();
        CHECK(error->status == blink::mojom::FileSystemAccessStatus::kOk &&
              remote.is_valid());
        return mojo::Remote<blink::mojom::FileSystemAccessFileHandle>(
            std::move(remote));
      };
      files->snapshot_file = file_for("snapshot.bin");
      auto initial = writer_for(files->snapshot_file);
      write(initial, marker);
      close(initial);
      Snapshot snapshot;
      files->snapshot_file->AsBlob(snapshot.GetCallback());
      auto [error, info, blob] = snapshot.Take();
      CHECK(error->status == blink::mojom::FileSystemAccessStatus::kOk &&
            blob && blob->blob.is_valid());
      EXPECT_EQ(info.size, static_cast<int64_t>(marker.size()));
      files->blob.Bind(std::move(blob->blob));
      files->blob->Clone(files->clone.BindNewPipeAndPassReceiver());
      files->blob->AsDataPipeGetter(files->getter.BindNewPipeAndPassReceiver());
      files->pending_file = file_for("pending.bin");
      files->writer = writer_for(files->pending_file);
      write(files->writer, marker + "-staged");
      return files;
    };
    auto first = open(alpha, "alpha-file-snapshot"),
         peer = open(beta, "beta-file-snapshot");
    NativeBlobRead original, cloned, peer_before;
    ASSERT_EQ(original.Read(*first->blob.get()), "alpha-file-snapshot");
    ASSERT_EQ(cloned.Read(*first->clone.get()), "alpha-file-snapshot");
    ASSERT_EQ(peer_before.Read(*peer->blob.get()), "beta-file-snapshot");
    NativeBlobRead getter_before;
    const auto getter_positive = getter_before.ReadGetter(*first->getter.get());
    ASSERT_EQ(getter_positive.first, net::OK);
    ASSERT_EQ(getter_positive.second, "alpha-file-snapshot");
    RecordProperty("positive_file_blob_getter_" + namespace_name,
                   "native-data-pipe-getter-bytes");
    RecordProperty("positive_file_blob_writer_" + namespace_name,
                   "native-file-blob-clone-and-writer-peer-readback");
    owner->Retire();
    // Send fresh mutating and read requests before any queue drain.
    base::test::TestFuture<Error, uint64_t> denied_write;
    base::test::TestFuture<Error> denied_truncate, denied_close;
    Snapshot denied_snapshot;
    first->writer->Write(
        0, stream("forbidden"),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(denied_write.GetCallback(),
                                                    aborted(), uint64_t{0}));
    first->writer->Truncate(0, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                                   denied_truncate.GetCallback(), aborted()));
    first->writer->Close(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_close.GetCallback(), aborted()));
    first->snapshot_file->AsBlob(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        denied_snapshot.GetCallback(), aborted(), base::File::Info(),
        blink::mojom::SerializedBlobPtr()));
    NativeBlobRead ended, ended_clone;
    EXPECT_EQ(ended.Read(*first->blob.get()), std::string())
        << "retained managed-file Blob read after retirement";
    EXPECT_EQ(ended_clone.Read(*first->clone.get()), std::string())
        << "retained managed-file Blob clone read after retirement";
    NativeBlobRead getter_ended;
    const auto getter_denied = getter_ended.ReadGetter(*first->getter.get());
    EXPECT_NE(getter_denied.first, net::OK);
    EXPECT_TRUE(getter_denied.second.empty());
    EXPECT_NE(denied_write.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_EQ(denied_write.Get<1>(), 0u);
    EXPECT_NE(denied_truncate.Get()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_NE(denied_close.Get()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_NE(denied_snapshot.Get<0>()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    EXPECT_FALSE(denied_snapshot.Get<2>());
    ASSERT_TRUE(alpha->IsActive() && beta->IsActive());
    ASSERT_FALSE(profile()->IsOffTheRecord());
    NativeBlobRead peer_after;
    ASSERT_EQ(peer_after.Read(*peer->blob.get()), "beta-file-snapshot");
    NativeBlobRead getter_peer;
    const auto peer_getter = getter_peer.ReadGetter(*peer->getter.get());
    ASSERT_EQ(peer_getter.first, net::OK);
    ASSERT_EQ(peer_getter.second, "beta-file-snapshot");
    write(peer->writer, "beta-after");
    base::test::TestFuture<Error> truncated;
    peer->writer->Truncate(10, truncated.GetCallback());
    ASSERT_EQ(truncated.Get()->status,
              blink::mojom::FileSystemAccessStatus::kOk);
    close(peer->writer);
    Snapshot committed;
    peer->pending_file->AsBlob(committed.GetCallback());
    auto [commit_error, commit_info, committed_blob] = committed.Take();
    ASSERT_EQ(commit_error->status, blink::mojom::FileSystemAccessStatus::kOk);
    ASSERT_TRUE(committed_blob && committed_blob->blob.is_valid());
    mojo::Remote<blink::mojom::Blob> fresh_peer(
        std::move(committed_blob->blob));
    NativeBlobRead committed_bytes;
    ASSERT_EQ(committed_bytes.Read(*fresh_peer.get()), "beta-after");
    if (!HasFailure())
      RecordProperty("retired_file_blob_getter_" + namespace_name,
                     "denied-no-bytes-peer-pipe-live");
    if (!HasFailure())
      RecordProperty("retired_file_blob_writer_" + namespace_name,
                     "old-blobs-writes-denied-peer-commit-bytes-live");
  }
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       PrimedBlobReadersHonorAdmissionWithoutBreakingOrdinarySnapshots) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto* initial =
      content::RenderFrameHostTester::For(web_contents()->GetPrimaryMainFrame())
          ->AppendChild("primed-blob");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://alpha.test/"), initial);
  const auto owner = content::GetAlloyEmbeddedStateForDocument(frame).temporary;
  ASSERT_TRUE(owner);
  base::SequenceBound<NativePrimedBlobReaders> readers(
      content::GetIOThreadTaskRunner({}),
      owner->Capability(StorePurpose::kBlobSpill));
  base::test::TestFuture<NativePrimedBlobResults> primed;
  readers.AsyncCall(&NativePrimedBlobReaders::Prime).Then(primed.GetCallback());
  const auto& before = primed.Get();
  ASSERT_EQ(before.scoped.status, storage::BlobReader::Status::DONE);
  ASSERT_EQ(before.scoped.error, net::OK);
  ASSERT_EQ(before.scoped.value, "alpha");
  ASSERT_EQ(before.ordinary.status, storage::BlobReader::Status::DONE);
  ASSERT_EQ(before.ordinary.error, net::OK);
  ASSERT_EQ(before.ordinary.value, "plain");
  RecordProperty("positive_primed_blob_readers",
                 "scoped-and-unscoped-native-prefix-before-retirement");
  owner->Retire();
  base::test::TestFuture<NativePrimedBlobResults> finished;
  readers.AsyncCall(&NativePrimedBlobReaders::AfterRetirement)
      .Then(finished.GetCallback());
  const auto& after = finished.Get();
  EXPECT_EQ(after.scoped.status, storage::BlobReader::Status::NET_ERROR);
  EXPECT_EQ(after.scoped.error, net::ERR_ACCESS_DENIED);
  EXPECT_EQ(after.scoped.bytes, 0);
  EXPECT_TRUE(after.scoped.value.empty());
  EXPECT_EQ(after.late_size, storage::BlobReader::Status::NET_ERROR);
  EXPECT_EQ(after.late_size_error, net::ERR_ACCESS_DENIED);
  ASSERT_EQ(after.ordinary.status, storage::BlobReader::Status::DONE);
  ASSERT_EQ(after.ordinary.error, net::OK);
  ASSERT_EQ(after.ordinary.value, "-primed-reader");
  ASSERT_TRUE(frame->IsActive());
  readers.SynchronouslyResetForTest();
  if (!HasFailure())
    RecordProperty(
        "retired_primed_blob_readers",
        "primed-scoped-denied-late-size-denied-unscoped-snapshot-readable");
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       TemporaryBucketMetadataNeverEntersDiskCatalogue) {
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto* main = web_contents()->GetPrimaryMainFrame();
  auto* initial =
      content::RenderFrameHostTester::For(main)->AppendChild("embedded");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://embedded.test/"), initial);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(main->GetStoragePartition());
  ASSERT_TRUE(partition->GetStoragePartitionPath());
  auto open = [&](content::RenderFrameHost* target) {
    mojo::Remote<blink::mojom::BucketManagerHost> manager;
    partition->GetBucketManager()->BindReceiver(
        static_cast<content::RenderFrameHostImpl*>(target)->GetWeakPtr(),
        manager.BindNewPipeAndPassReceiver(),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    base::test::TestFuture<mojo::PendingRemote<blink::mojom::BucketHost>,
                           blink::mojom::BucketError>
        result;
    manager->OpenBucket("catalogue_bucket", blink::mojom::BucketPolicies::New(),
                        result.GetCallback());
    auto [pending, error] = result.Take();
    EXPECT_TRUE(pending.is_valid());
    return mojo::Remote<blink::mojom::BucketHost>(std::move(pending));
  };
  auto real = open(main);
  auto temporary = open(frame);
  base::test::TestFuture<storage::QuotaErrorOr<storage::BucketInfo>> real_info,
      temporary_info;
  auto* proxy = partition->GetQuotaManagerProxy();
  proxy->GetBucketByNameUnsafe(main->GetStorageKey(), "catalogue_bucket",
                               base::SequencedTaskRunner::GetCurrentDefault(),
                               real_info.GetCallback());
  proxy->GetBucketByNameUnsafe(frame->GetStorageKey(), "catalogue_bucket",
                               base::SequencedTaskRunner::GetCurrentDefault(),
                               temporary_info.GetCallback());
  EXPECT_TRUE(real_info.Get().has_value());
  ASSERT_FALSE(temporary_info.Get().has_value());
  EXPECT_EQ(temporary_info.Get().error().quota_error,
            storage::QuotaError::kNotFound);
}

TEST_F(NativeEmbeddedStateTest, SessionAdmissionPreservesPendingNativeClone) {
  auto* frame = Embed(first_.get());
  auto* dom = Dom(frame);
  auto* control = dom->GetSessionStorageControl();
  const auto source = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const auto target = base::Uuid::GenerateRandomV4().AsLowercaseString();
  control->CreateNamespace(source);
  control->CloneNamespace(
      source, target,
      storage::mojom::SessionStorageCloneType::kWaitForCloneOnNamespace);
  // The first temporary bind happens AFTER the native browser clone request.
  auto area = OpenSession(frame, source);
  base::test::TestFuture<bool> written;
  area->Put({1}, {31}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  auto clone = OpenSession(frame, target);
  base::test::TestFuture<
      std::vector<storage::mojom::SessionStorageUsageInfoPtr>>
      dispatched;
  control->GetUsage(dispatched.GetCallback());
  ASSERT_TRUE(dispatched.Wait());
  mojo::Remote<blink::mojom::SessionStorageNamespace> ns;
  dom->BindNamespace(source, base::BindOnce([](std::string_view reason) {
                       ADD_FAILURE() << reason;
                     }),
                     ns.BindNewPipeAndPassReceiver());
  ns->Clone(target);
  ns.FlushForTesting();
  EXPECT_EQ(SessionValue(clone), (std::vector<uint8_t>{31}));
  control->DeleteNamespace(source, false);
  control->DeleteNamespace(target, false);
}

TEST_F(NativeEmbeddedStateTest, SessionPendingCloneSurvivesSourceDeletion) {
  auto* frame = Embed(first_.get());
  auto* control = Dom(frame)->GetSessionStorageControl();
  const auto source = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const auto target = base::Uuid::GenerateRandomV4().AsLowercaseString();
  control->CreateNamespace(source);
  auto area = OpenSession(frame, source);
  base::test::TestFuture<bool> written;
  area->Put({1}, {32}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  control->CloneNamespace(
      source, target,
      storage::mojom::SessionStorageCloneType::kWaitForCloneOnNamespace);
  control->DeleteNamespace(source, false);
  auto clone = OpenSession(frame, target);
  EXPECT_EQ(SessionValue(clone), (std::vector<uint8_t>{32}));
  control->DeleteNamespace(target, false);
}

TEST_F(NativeEmbeddedStateTest, SessionMoveAndBackNeverReviveOldScope) {
  auto* frame = Embed(first_.get());
  auto* control = Dom(frame)->GetSessionStorageControl();
  const auto ns = base::Uuid::GenerateRandomV4().AsLowercaseString();
  control->CreateNamespace(ns);
  auto area = OpenSession(frame, ns);
  base::test::TestFuture<bool> written;
  area->Put({1}, {33}, std::nullopt, nullptr, written.GetCallback());
  ASSERT_TRUE(written.Get());
  Navigate(first_.get(), "https://different.test/");
  area.FlushForTesting();
  EXPECT_FALSE(area.is_connected());
  Navigate(first_.get(), "https://outer.test/back");
  auto fresh = OpenSession(Embed(first_.get()), ns);
  EXPECT_TRUE(SessionValue(fresh).empty());
  EXPECT_TRUE(second_);
  control->DeleteNamespace(ns, false);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       TemporarySessionAreaNeverEntersDiskController) {
  ASSERT_FALSE(profile()->IsOffTheRecord());
  ASSERT_TRUE(
      content::ConfigureAlloyEmbeddedStateScope(*profile(), Scope::kPerTab));
  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL("https://outer.test/"));
  auto* main = web_contents()->GetPrimaryMainFrame();
  auto* initial =
      content::RenderFrameHostTester::For(main)->AppendChild("child");
  auto* frame = content::NavigationSimulator::NavigateAndCommitFromDocument(
      GURL("https://embedded.test/"), initial);
  auto* partition =
      static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  ASSERT_TRUE(partition->GetStoragePartitionPath());
  auto* dom = static_cast<content::DOMStorageContextWrapper*>(
      partition->GetDOMStorageContext());
  auto* control = dom->GetSessionStorageControl();
  auto* router = static_cast<content::AlloySessionStorageRouter*>(control);
  const auto ns = base::Uuid::GenerateRandomV4().AsLowercaseString();
  control->CreateNamespace(ns);
  auto open = [&](content::RenderFrameHost* target) {
    mojo::Remote<blink::mojom::StorageArea> area;
    dom->BindStorageArea(
        target->GetStorageKey(), target->GetFrameToken(), ns,
        area.BindNewPipeAndPassReceiver(),
        content::ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
            target->GetProcess()->GetID()),
        base::BindOnce(
            [](std::string_view reason) { ADD_FAILURE() << reason; }));
    return area;
  };
  auto real = open(main);
  auto temporary = open(frame);
  base::test::TestFuture<bool> real_written, temporary_written;
  real->Put({1}, {34}, std::nullopt, nullptr, real_written.GetCallback());
  temporary->Put({1}, {35}, std::nullopt, nullptr,
                 temporary_written.GetCallback());
  ASSERT_TRUE(real_written.Get());
  ASSERT_TRUE(temporary_written.Get());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> read;
  temporary->GetAll(mojo::NullRemote(), read.GetCallback());
  auto values = read.Take();
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0]->value, (std::vector<uint8_t>{35}));
  base::test::TestFuture<
      std::vector<storage::mojom::SessionStorageUsageInfoPtr>>
      usage;
  router->GetRealControlForTesting().GetUsage(usage.GetCallback());
  auto entries = usage.Take();
  EXPECT_TRUE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == main->GetStorageKey() &&
           entry->namespace_id == ns;
  }));
  EXPECT_FALSE(std::ranges::any_of(entries, [&](const auto& entry) {
    return entry->storage_key == frame->GetStorageKey();
  }));
  control->DeleteNamespace(ns, false);
}

// These non-network consumers must select the same native temporary owner as
// storage, even in a truthful disk Profile. A Profile-wide memory map is not a
// scope: its values and chooser capabilities outlive the embedded issuer.
class NativeDiskEmbeddedPermissionTest
    : public ChromeRenderViewHostTestHarness {
 protected:
  void TearDown() override {
    alpha_ = nullptr;
    beta_ = nullptr;
    second_ = nullptr;
    foreign_ = nullptr;
    owner_.reset();
    foreign_tab_.reset();
    second_tab_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  content::RenderFrameHostImpl* Append(content::WebContents* tab,
                                       const char* name,
                                       const GURL& url) {
    auto* initial =
        content::RenderFrameHostTester::For(tab->GetPrimaryMainFrame())
            ->AppendChild(name);
    return static_cast<content::RenderFrameHostImpl*>(
        content::NavigationSimulator::NavigateAndCommitFromDocument(url,
                                                                    initial));
  }

  void Prepare(Scope scope) {
    ASSERT_FALSE(profile()->IsOffTheRecord());
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    content::WebContentsTester::For(web_contents())->NavigateAndCommit(outer_);
    second_tab_ =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    foreign_tab_ =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(second_tab_.get())
        ->NavigateAndCommit(outer_);
    content::WebContentsTester::For(foreign_tab_.get())
        ->NavigateAndCommit(GURL("https://foreign-outer.test/"));
    alpha_ = Append(web_contents(), "permission-alpha", alpha_url_);
    beta_ =
        Append(web_contents(), "permission-beta", GURL("https://beta.test/"));
    second_ = Append(second_tab_.get(), "permission-second", alpha_url_);
    foreign_ = Append(foreign_tab_.get(), "permission-foreign", alpha_url_);
    owner_ = content::GetAlloyEmbeddedStateForDocument(alpha_).temporary;
    auto second_owner =
        content::GetAlloyEmbeddedStateForDocument(second_).temporary;
    ASSERT_TRUE(owner_ && second_owner);
    ASSERT_EQ(owner_ == second_owner, scope == Scope::kPerOuterSite);
    ASSERT_NE(owner_,
              content::GetAlloyEmbeddedStateForDocument(foreign_).temporary);
    ASSERT_EQ(alpha_->GetStoragePartition(), foreign_->GetStoragePartition());
  }

  void CheckSettings(Scope scope) {
    base::test::ScopedFeatureList features;
    features.InitWithFeatures(
        {blink::features::kGeolocationElement,
         permissions::features::kPermissionHeuristicAutoGrant},
        {});
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    const bool shared = scope == Scope::kPerOuterSite;
    auto* client = ChromePermissionsClient::GetInstance();
    auto map = [&](content::RenderFrameHost* frame) {
      return client->GetSettingsMapForDocument(profile(), frame);
    };
    scoped_refptr<HostContentSettingsMap> first = map(alpha_);
    auto* second = map(second_);
    auto* foreign = map(foreign_);
    auto* beta = map(beta_);
    auto* real = HostContentSettingsMapFactory::GetForProfile(profile());
    ASSERT_TRUE(first && second && foreign && beta && real);
    EXPECT_NE(first.get(), real);
    EXPECT_NE(first.get(), foreign);
    EXPECT_NE(first.get(), beta);
    EXPECT_EQ(first.get() == second, shared);
    constexpr auto type = ContentSettingsType::MEDIASTREAM_MIC;
    real->SetContentSettingDefaultScope(alpha_url_, outer_, type,
                                        CONTENT_SETTING_BLOCK);
    EXPECT_EQ(first->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_ASK);
    first->SetContentSettingDefaultScope(alpha_url_, outer_, type,
                                         CONTENT_SETTING_ALLOW);
    EXPECT_EQ(first->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_ALLOW);
    EXPECT_EQ(second->GetContentSetting(alpha_url_, outer_, type),
              shared ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_ASK);
    EXPECT_EQ(foreign->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_ASK);
    EXPECT_EQ(real->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_BLOCK);
    auto* history =
        client->GetPermissionActionsHistoryForDocument(profile(), alpha_);
    auto* second_history =
        client->GetPermissionActionsHistoryForDocument(profile(), second_);
    auto* foreign_history =
        client->GetPermissionActionsHistoryForDocument(profile(), foreign_);
    auto* blocker =
        client->GetPermissionDecisionAutoBlockerForDocument(profile(), alpha_);
    auto* second_blocker =
        client->GetPermissionDecisionAutoBlockerForDocument(profile(), second_);
    auto* foreign_blocker = client->GetPermissionDecisionAutoBlockerForDocument(
        profile(), foreign_);
    ASSERT_TRUE(history && second_history && foreign_history && blocker &&
                second_blocker && foreign_blocker);
    // The native temporary-grant heuristic supports only geolocation, behind
    // its two feature gates. Keep microphone setting/embargo coverage separate.
    constexpr auto heuristic_type = ContentSettingsType::GEOLOCATION;
    for (int i = 0; i != 3; ++i) {
      history->RecordTemporaryGrant(alpha_url_, heuristic_type);
      blocker->RecordDismissAndEmbargo(alpha_url_, type, false);
    }
    EXPECT_EQ(
        history->GetTemporaryGrantCountForTesting(alpha_url_, heuristic_type),
        3);
    EXPECT_EQ(second_history->GetTemporaryGrantCountForTesting(alpha_url_,
                                                               heuristic_type),
              shared ? 3 : 0);
    EXPECT_EQ(foreign_history->GetTemporaryGrantCountForTesting(alpha_url_,
                                                                heuristic_type),
              0);
    EXPECT_EQ(blocker->GetDismissCount(alpha_url_, type), 3);
    EXPECT_EQ(second_blocker->GetDismissCount(alpha_url_, type),
              shared ? 3 : 0);
    EXPECT_EQ(foreign_blocker->GetDismissCount(alpha_url_, type), 0);
    if (!HasFailure()) {
      RecordProperty("positive_embedded_permission_owner",
                     shared ? "native-map-history-embargo-outer-shared"
                            : "native-map-history-embargo-tab-isolated");
    }
    owner_->Retire();
    EXPECT_TRUE(alpha_->IsActive());
    EXPECT_EQ(map(alpha_), nullptr);
    EXPECT_EQ(client->GetPermissionActionsHistoryForDocument(profile(), alpha_),
              nullptr);
    EXPECT_EQ(
        client->GetPermissionDecisionAutoBlockerForDocument(profile(), alpha_),
        nullptr);
    EXPECT_EQ(map(beta_), beta);
    EXPECT_EQ(map(foreign_), foreign);
    if (shared) {
      EXPECT_EQ(map(second_), nullptr);
    } else {
      EXPECT_EQ(map(second_), second);
    }
    auto* replacement = Append(web_contents(), "permission-fresh", alpha_url_);
    auto* fresh = map(replacement);
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh, first.get());
    EXPECT_EQ(fresh->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_ASK);
    EXPECT_EQ(real->GetContentSetting(alpha_url_, outer_, type),
              CONTENT_SETTING_BLOCK);
    if (!HasFailure()) {
      RecordProperty("retired_embedded_permission_owner",
                     "old-document-denied-fresh-empty-other-owners-live");
    }
  }

  void CheckChoosers(Scope scope) {
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    const bool shared = scope == Scope::kPerOuterSite;
    auto* usb = UsbChooserContextFactory::GetForFrame(alpha_);
    auto* second_usb = UsbChooserContextFactory::GetForFrame(second_);
    auto* foreign_usb = UsbChooserContextFactory::GetForFrame(foreign_);
    auto* hid = HidChooserContextFactory::GetForFrame(alpha_);
    auto* second_hid = HidChooserContextFactory::GetForFrame(second_);
    auto* foreign_hid = HidChooserContextFactory::GetForFrame(foreign_);
    auto* serial = SerialChooserContextFactory::GetForFrame(alpha_);
    auto* second_serial = SerialChooserContextFactory::GetForFrame(second_);
    auto* foreign_serial = SerialChooserContextFactory::GetForFrame(foreign_);
    auto* bluetooth = BluetoothChooserContextFactory::GetForFrame(alpha_);
    auto* second_bluetooth =
        BluetoothChooserContextFactory::GetForFrame(second_);
    auto* foreign_bluetooth =
        BluetoothChooserContextFactory::GetForFrame(foreign_);
    ASSERT_TRUE(usb && second_usb && foreign_usb && hid && second_hid &&
                foreign_hid && serial && second_serial && foreign_serial &&
                bluetooth && second_bluetooth && foreign_bluetooth);
    EXPECT_EQ(usb == second_usb, shared);
    EXPECT_EQ(hid == second_hid, shared);
    EXPECT_EQ(serial == second_serial, shared);
    EXPECT_EQ(bluetooth == second_bluetooth, shared);
    EXPECT_NE(usb, foreign_usb);
    EXPECT_NE(hid, foreign_hid);
    EXPECT_NE(serial, foreign_serial);
    EXPECT_NE(bluetooth, foreign_bluetooth);
    device::FakeUsbDeviceManager manager;
    auto device =
        manager.CreateAndAddDevice(1000, 1, "Alloy", "Embedded", "serial");
    std::vector<UsbChooserContext*> initialized;
    for (auto* context : {usb, second_usb, foreign_usb}) {
      if (std::ranges::find(initialized, context) != initialized.end()) {
        continue;
      }
      initialized.push_back(context);
      mojo::PendingRemote<device::mojom::UsbDeviceManager> remote;
      manager.AddReceiver(remote.InitWithNewPipeAndPassReceiver());
      context->SetDeviceManagerForTesting(std::move(remote));
      base::test::TestFuture<std::vector<device::mojom::UsbDeviceInfoPtr>>
          devices;
      context->GetDevices(devices.GetCallback());
      ASSERT_EQ(devices.Get().size(), 1u);
    }
    const auto origin = alpha_->GetLastCommittedOrigin();
    usb->GrantDevicePermission(origin, *device);
    EXPECT_TRUE(usb->HasDevicePermission(origin, *device));
    EXPECT_EQ(second_usb->HasDevicePermission(origin, *device), shared);
    EXPECT_FALSE(foreign_usb->HasDevicePermission(origin, *device));
    device::MockBluetoothDevice radio(nullptr, 0, "Embedded fixture",
                                      "00:11:22:33:44:55", false, false);
    const device::BluetoothUUID service("180d");
    auto options = blink::mojom::WebBluetoothRequestDeviceOptions::New();
    options->accept_all_devices = true;
    options->optional_services.push_back(service);
    const auto id =
        bluetooth->GrantServiceAccessPermission(origin, &radio, options.get());
    EXPECT_TRUE(bluetooth->HasDevicePermission(origin, id));
    EXPECT_TRUE(bluetooth->IsAllowedToAccessService(origin, id, service));
    EXPECT_EQ(second_bluetooth->HasDevicePermission(origin, id), shared);
    EXPECT_FALSE(foreign_bluetooth->HasDevicePermission(origin, id));
    auto usb_weak = usb->AsWeakPtr();
    auto hid_weak = hid->AsWeakPtr();
    auto serial_weak = serial->AsWeakPtr();
    if (!HasFailure()) {
      RecordProperty(
          "positive_embedded_chooser_owner",
          shared ? "four-native-choosers-outer-shared-usb-bluetooth-live"
                 : "four-native-choosers-tab-isolated-usb-bluetooth-live");
    }
    owner_->Retire();
    EXPECT_FALSE(usb_weak);
    EXPECT_FALSE(hid_weak);
    EXPECT_FALSE(serial_weak);
    EXPECT_EQ(UsbChooserContextFactory::GetForFrame(alpha_), nullptr);
    EXPECT_EQ(HidChooserContextFactory::GetForFrame(alpha_), nullptr);
    EXPECT_EQ(SerialChooserContextFactory::GetForFrame(alpha_), nullptr);
    EXPECT_EQ(BluetoothChooserContextFactory::GetForFrame(alpha_), nullptr);
    EXPECT_EQ(UsbChooserContextFactory::GetForFrame(foreign_), foreign_usb);
    EXPECT_FALSE(foreign_usb->HasDevicePermission(origin, *device));
    auto* replacement = Append(web_contents(), "chooser-fresh", alpha_url_);
    auto* fresh_usb = UsbChooserContextFactory::GetForFrame(replacement);
    auto* fresh_bluetooth =
        BluetoothChooserContextFactory::GetForFrame(replacement);
    ASSERT_TRUE(fresh_usb && fresh_bluetooth);
    EXPECT_FALSE(fresh_usb->HasDevicePermission(origin, *device));
    EXPECT_FALSE(fresh_bluetooth->HasDevicePermission(origin, id));
    if (!HasFailure()) {
      RecordProperty(
          "retired_embedded_chooser_owner",
          "native-grant-weak-pointers-revoked-old-document-denied-fresh-empty");
    }
  }

  const GURL outer_{"https://outer.test/"};
  const GURL alpha_url_{"https://alpha.test/"};
  std::unique_ptr<content::WebContents> second_tab_;
  std::unique_ptr<content::WebContents> foreign_tab_;
  raw_ptr<content::RenderFrameHostImpl> alpha_ = nullptr;
  raw_ptr<content::RenderFrameHostImpl> beta_ = nullptr;
  raw_ptr<content::RenderFrameHostImpl> second_ = nullptr;
  raw_ptr<content::RenderFrameHostImpl> foreign_ = nullptr;
  scoped_refptr<EmbeddedState> owner_;
};

TEST_F(NativeDiskEmbeddedPermissionTest, PerTabPermissionMapsAndHeuristics) {
  CheckSettings(Scope::kPerTab);
}

TEST_F(NativeDiskEmbeddedPermissionTest, OuterSitePermissionMapsAndHeuristics) {
  CheckSettings(Scope::kPerOuterSite);
}

TEST_F(NativeDiskEmbeddedPermissionTest, PerTabNativeChooserGrants) {
  CheckChoosers(Scope::kPerTab);
}

TEST_F(NativeDiskEmbeddedPermissionTest, OuterSiteNativeChooserGrants) {
  CheckChoosers(Scope::kPerOuterSite);
}

class NativeDiskEmbeddedMediaTest : public NativeDiskEmbeddedPermissionTest {
 protected:
  content::MediaDeviceSaltAndOrigin ReadIdentity(
      content::RenderFrameHost* frame) {
    base::test::TestFuture<const content::MediaDeviceSaltAndOrigin&> reply;
    content::GetMediaDeviceSaltAndOrigin(frame->GetGlobalId(),
                                         reply.GetCallback());
    return reply.Take();
  }

  void CheckMediaIdentity(Scope scope) {
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    const bool shared = scope == Scope::kPerOuterSite;
    auto* real_settings =
        HostContentSettingsMapFactory::GetForProfile(profile());
    profile()->GetPrefs()->SetInteger(
        prefs::kCookieControlsMode,
        static_cast<int>(content_settings::CookieControlsMode::kOff));
    CookieSettingsFactory::GetForProfile(profile())->SetDefaultCookieSetting(
        CONTENT_SETTING_ALLOW);
    // This automatic cookie exception is deliberately present. It cannot
    // change the already captured temporary owner into real salt storage.
    brave_shields::SetBraveShieldsEnabled(real_settings, false, outer_);
    brave_shields::SetBraveShieldsEnabled(real_settings, false,
                                          GURL("https://foreign-outer.test/"));
    ASSERT_EQ(content::GetAlloyEmbeddedStateForDocument(alpha_).temporary,
              owner_);
    auto* real_salts =
        MediaDeviceSaltServiceFactory::GetForBrowserContext(profile());
    ASSERT_TRUE(real_salts);
    auto* top = web_contents()->GetPrimaryMainFrame();
    const auto top_key = top->GetStorageKey();
    base::test::TestFuture<const std::string&> top_salt;
    real_salts->GetSalt(top_key, top_salt.GetCallback());
    ASSERT_FALSE(top_salt.Get().empty());
    base::test::TestFuture<std::vector<blink::StorageKey>> before;
    real_salts->GetAllStorageKeys(before.GetCallback());
    const auto before_keys = before.Take();
    ASSERT_TRUE(std::ranges::find(before_keys, top_key) != before_keys.end());
    base::test::TestFuture<bool, const std::string&> allowed;
    content::GetContentClientForTesting()->browser()->GetMediaDeviceIDSalt(
        alpha_, alpha_->ComputeSiteForCookies(), alpha_->GetStorageKey(),
        allowed.GetCallback());
    ASSERT_TRUE(allowed.Get<0>())
        << "fixture must enter native salt storage, not frame-salt fallback";
    ASSERT_FALSE(allowed.Get<1>().empty());
    auto first = ReadIdentity(alpha_);
    auto repeated = ReadIdentity(alpha_);
    auto second = ReadIdentity(second_);
    auto foreign = ReadIdentity(foreign_);
    auto beta = ReadIdentity(beta_);
    auto top_identity = ReadIdentity(top);
    const blink::WebMediaDeviceInfo device(
        "scoped-raw-device", "Native fixture", "scoped-raw-group");
    const auto original =
        content::TranslateMediaDeviceInfo(true, first, device);
    const auto second_value =
        content::TranslateMediaDeviceInfo(true, second, device);
    const auto foreign_value =
        content::TranslateMediaDeviceInfo(true, foreign, device);
    const auto beta_id =
        content::GetHMACForRawMediaDeviceID(beta, device.device_id);
    const auto top_id =
        content::GetHMACForRawMediaDeviceID(top_identity, device.device_id);
    ASSERT_FALSE(original.device_id.empty());
    ASSERT_FALSE(original.group_id.empty());
    ASSERT_EQ(original.label, device.label);
    ASSERT_NE(original.device_id, device.device_id);
    EXPECT_EQ(content::GetHMACForRawMediaDeviceID(repeated, device.device_id),
              original.device_id);
    EXPECT_EQ(original.device_id == second_value.device_id, shared);
    EXPECT_NE(original.device_id, foreign_value.device_id);
    // Native group identifiers remain document-specific even when the salt
    // service and device identity share the per-outer-site scope.
    EXPECT_NE(original.group_id, second_value.group_id);
    EXPECT_TRUE(content::DoesRawMediaDeviceIDMatchHMAC(
        first, original.device_id, device.device_id));
    if (!HasFailure()) {
      RecordProperty(
          "positive_embedded_media_values",
          shared ? "native-device-hmac-outer-shared-group-document-specific"
                 : "native-device-hmac-tab-isolated-group-document-specific");
    }
    base::test::TestFuture<std::vector<blink::StorageKey>> real_keys;
    real_salts->GetAllStorageKeys(real_keys.GetCallback());
    const auto keys = real_keys.Take();
    for (auto* frame :
         {alpha_.get(), beta_.get(), second_.get(), foreign_.get()}) {
      EXPECT_TRUE(std::ranges::find(keys, frame->GetStorageKey()) == keys.end())
          << "temporary media salt reached the real disk-Profile service";
    }
    EXPECT_TRUE(std::ranges::find(keys, top_key) != keys.end());
    auto held = first;
    base::test::TestFuture<const content::MediaDeviceSaltAndOrigin&> late;
    content::GetMediaDeviceSaltAndOrigin(alpha_->GetGlobalId(),
                                         late.GetCallback());
    ASSERT_FALSE(late.IsReady())
        << "positive native DB request must remain asynchronous";
    owner_->Retire();
    ASSERT_TRUE(alpha_->IsActive() && beta_->IsActive());
    const auto late_value = late.Take();
    const auto retired = content::TranslateMediaDeviceInfo(true, held, device);
    EXPECT_TRUE(retired.device_id.empty());
    EXPECT_TRUE(retired.group_id.empty());
    EXPECT_TRUE(retired.label.empty());
    EXPECT_TRUE(
        content::GetHMACForRawMediaDeviceID(held, device.device_id).empty());
    EXPECT_FALSE(content::DoesRawMediaDeviceIDMatchHMAC(
        held, original.device_id, device.device_id));
    EXPECT_TRUE(
        content::GetHMACForRawMediaDeviceID(late_value, device.device_id)
            .empty());
    EXPECT_TRUE(content::GetHMACForRawMediaDeviceID(ReadIdentity(alpha_),
                                                    device.device_id)
                    .empty());
    EXPECT_EQ(content::GetHMACForRawMediaDeviceID(second, device.device_id),
              shared ? std::string() : second_value.device_id);
    EXPECT_EQ(content::GetHMACForRawMediaDeviceID(ReadIdentity(beta_),
                                                  device.device_id),
              beta_id);
    EXPECT_EQ(content::GetHMACForRawMediaDeviceID(ReadIdentity(top),
                                                  device.device_id),
              top_id);
    auto* replacement = Append(web_contents(), "fresh-media-owner", alpha_url_);
    const auto fresh = content::TranslateMediaDeviceInfo(
        true, ReadIdentity(replacement), device);
    ASSERT_FALSE(fresh.device_id.empty());
    EXPECT_NE(fresh.device_id, original.device_id);
    if (!HasFailure()) {
      RecordProperty(
          "retired_embedded_media_values",
          "held-late-and-old-document-identities-denied-fresh-peer-real-live");
    }
  }
};

TEST_F(NativeDiskEmbeddedMediaTest, PerTabMediaSaltsAndDeviceIdsEndWithScope) {
  CheckMediaIdentity(Scope::kPerTab);
}

TEST_F(NativeDiskEmbeddedMediaTest,
       OuterSiteMediaSaltsAndDeviceIdsEndWithScope) {
  CheckMediaIdentity(Scope::kPerOuterSite);
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       MemoryGeneratedCodeCacheKeepsCompleteNonceIdentityAndBytes) {
  using Cache = content::GeneratedCodeCache;
  const GURL resource("https://cdn.test/script.js");
  const GURL origin("https://embedded.test/");
  const net::SchemefulSite top(GURL("https://outer.test/"));
  const net::SchemefulSite frame(origin);
  const net::NetworkIsolationKey alpha(top, frame,
                                       base::UnguessableToken::Create());
  const net::NetworkIsolationKey beta(top, frame,
                                      base::UnguessableToken::Create());
  const auto response_time = base::Time::Now();
  auto read = [&](Cache& cache, const net::NetworkIsolationKey& key) {
    base::test::TestFuture<base::Time, mojo_base::BigBuffer> result;
    cache.FetchEntry(
        resource, origin, key,
        result.GetCallback<const base::Time&, mojo_base::BigBuffer>());
    auto values = result.Take();
    auto& data = std::get<1>(values);
    if (!data.size())
      return std::string();
    EXPECT_EQ(response_time, std::get<0>(values));
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
  };
  auto shutdown = [](Cache& cache) {
    base::RunLoop done;
    cache.ShutdownForTesting(done.QuitClosure());
    done.Run();
  };
  for (auto type : {Cache::kJavaScript, Cache::kWebAssembly}) {
    auto owner = std::make_unique<Cache>(base::FilePath(), 2 * 1024 * 1024,
                                         type, Cache::Backing::kMemory);
    auto peer = std::make_unique<Cache>(base::FilePath(), 2 * 1024 * 1024, type,
                                        Cache::Backing::kMemory);
    base::test::TestFuture<disk_cache::Backend*> backend;
    owner->GetBackend(backend.GetCallback());
    ASSERT_TRUE(backend.Get());
    ASSERT_EQ(net::MEMORY_CACHE, backend.Get()->GetCacheType());
    ASSERT_TRUE(owner->path().empty());
    const std::string a("memory-code-alpha"), b("memory-code-beta"),
        p("memory-peer");
    owner->WriteEntry(resource, origin, alpha, response_time,
                      mojo_base::BigBuffer(base::as_byte_span(a)));
    ASSERT_EQ(a, read(*owner, alpha));
    ASSERT_TRUE(read(*owner, beta).empty());
    owner->WriteEntry(resource, origin, beta, response_time,
                      mojo_base::BigBuffer(base::as_byte_span(b)));
    ASSERT_EQ(a, read(*owner, alpha));
    ASSERT_EQ(b, read(*owner, beta));
    ASSERT_TRUE(read(*peer, alpha).empty());
    peer->WriteEntry(resource, origin, alpha, response_time,
                     mojo_base::BigBuffer(base::as_byte_span(p)));
    ASSERT_EQ(p, read(*peer, alpha));
    owner->DeleteEntry(resource, origin, alpha);
    ASSERT_TRUE(read(*owner, alpha).empty());
    ASSERT_EQ(b, read(*owner, beta));
    shutdown(*owner);
    owner.reset();
    auto fresh = std::make_unique<Cache>(base::FilePath(), 2 * 1024 * 1024,
                                         type, Cache::Backing::kMemory);
    ASSERT_TRUE(read(*fresh, alpha).empty());
    ASSERT_TRUE(read(*fresh, beta).empty());
    ASSERT_EQ(p, read(*peer, alpha));
    shutdown(*fresh);
    shutdown(*peer);
  }
  RecordProperty("positive_memory_generated_code_cache",
                 "native-js-wasm-memory-backend-bytes-complete-nonce-identity");
  RecordProperty("retired_memory_generated_code_cache",
                 "shutdown-fresh-empty-independent-native-memory-peer-live");
}

TEST_F(NativeDiskEmbeddedLocalStorageTest,
       DiskGeneratedCodeCacheRejectsTransientWithoutLosingStableControl) {
  using Cache = content::GeneratedCodeCache;
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const GURL resource("https://cdn.test/script.js"),
      origin("https://embedded.test/");
  const net::SchemefulSite top(GURL("https://outer.test/")), frame(origin);
  const net::NetworkIsolationKey stable(top, frame);
  const net::NetworkIsolationKey nonce(top, frame,
                                       base::UnguessableToken::Create());
  const auto opaque = net::NetworkIsolationKey::CreateTransientForTesting();
  const auto response_time = base::Time::Now();
  Cache cache(directory.GetPath().AppendASCII("code"), 2 * 1024 * 1024,
              Cache::kJavaScript);
  auto read = [&](const net::NetworkIsolationKey& key) {
    base::test::TestFuture<base::Time, mojo_base::BigBuffer> result;
    cache.FetchEntry(
        resource, origin, key,
        result.GetCallback<const base::Time&, mojo_base::BigBuffer>());
    auto values = result.Take();
    auto& data = std::get<1>(values);
    if (!data.size())
      return std::string();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
  };
  const std::string good("disk-stable-code"),
      forbidden("forbidden-transient-code");
  cache.WriteEntry(resource, origin, stable, response_time,
                   mojo_base::BigBuffer(base::as_byte_span(good)));
  ASSERT_EQ(good, read(stable));
  for (const auto& transient : {nonce, opaque}) {
    cache.WriteEntry(resource, origin, transient, response_time,
                     mojo_base::BigBuffer(base::as_byte_span(forbidden)));
    ASSERT_TRUE(read(transient).empty());
    cache.DeleteEntry(resource, origin, transient);
    ASSERT_EQ(good, read(stable));
  }
  base::RunLoop done;
  cache.ShutdownForTesting(done.QuitClosure());
  done.Run();
  RecordProperty("positive_disk_code_cache_control",
                 "native-disk-stable-nik-bytes-live");
  RecordProperty(
      "transient_disk_code_cache_denied",
      "nonce-and-opaque-nik-cannot-read-write-delete-stable-control");
}
// Exercise the actual frame binder and exported-reader protocol, not a manually
// selected cache or a browser-side testing fetch that Blink never uses.
class NativeDiskEmbeddedCodeCacheTest
    : public NativeDiskEmbeddedLocalStorageTest {
 protected:
  NativeDiskEmbeddedCodeCacheTest() {
    features_.InitWithFeatures(
        {blink::features::kUsePersistentCacheForCodeCache,
         net::features::kSplitCacheByNetworkIsolationKey,
         blink::features::kInlineScriptCache},
        {});
    command_line_.GetProcessCommandLine()->AppendSwitch(
        switches::kSitePerProcess);
    content::SiteIsolationPolicy::DisableFlagCachingForTesting();
  }

  using Host = mojo::Remote<blink::mojom::CodeCacheHost>;
  using Reader = std::unique_ptr<persistent_cache::PersistentCache>;
  using Type = blink::mojom::CodeCacheType;

  static Host Bind(content::RenderFrameHostImpl* frame) {
    Host host;
    frame->CreateCodeCacheHost(host.BindNewPipeAndPassReceiver());
    return host;
  }

  static std::optional<persistent_cache::PendingBackend> Export(Host& host,
                                                                Type type) {
    base::test::TestFuture<std::optional<persistent_cache::PendingBackend>>
        reply;
    host->GetPendingBackend(
        type, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                  reply.GetCallback(),
                  std::optional<persistent_cache::PendingBackend>()));
    return reply.Take();
  }

  static Reader Open(Host& host, Type type) {
    auto pending = Export(host, type);
    EXPECT_TRUE(pending);
    if (!pending) {
      return nullptr;
    }
    auto result = persistent_cache::PersistentCache::Bind(
        persistent_cache::Client::kCodeCache, std::move(*pending));
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? std::move(*result) : nullptr;
  }

  struct Observation {
    bool operational = false;
    bool hit = false;
    std::string bytes;
    int64_t signature = 0;
    std::optional<persistent_cache::TransactionError> error;
  };

  static Observation Read(Reader& reader, base::span<const uint8_t> key) {
    Observation observed;
    auto result = reader->Find(key, [&](size_t size) {
      observed.bytes.resize(size);
      return base::as_writable_byte_span(observed.bytes);
    });
    observed.operational = result.has_value();
    if (!result.has_value()) {
      observed.error = result.error();
    } else if (result->has_value()) {
      observed.hit = true;
      observed.signature = result->value().input_signature;
    }
    return observed;
  }

  void CheckFrameExports(Scope scope) {
    ASSERT_TRUE(blink::features::IsPersistentCacheForCodeCacheEnabled());
    ASSERT_TRUE(blink::features::IsInlineScriptCacheEnabled());
    ASSERT_FALSE(profile()->IsOffTheRecord());
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    content::WebContentsTester::For(web_contents())
        ->NavigateAndCommit(GURL("https://outer.test/"));
    auto second_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(second_tab.get())
        ->NavigateAndCommit(GURL("https://outer.test/second"));
    auto other_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(other_tab.get())
        ->NavigateAndCommit(GURL("https://other-outer.test/"));
    auto append = [](content::WebContents* tab, const char* name) {
      auto* initial =
          content::RenderFrameHostTester::For(tab->GetPrimaryMainFrame())
              ->AppendChild(name);
      return static_cast<content::RenderFrameHostImpl*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(
              GURL("https://alpha.test/"), initial));
    };
    auto* alpha = append(web_contents(), "code-alpha");
    auto* second = append(second_tab.get(), "code-second");
    auto* peer = append(other_tab.get(), "code-peer");
    auto owner = content::GetAlloyEmbeddedStateForDocument(alpha).temporary;
    ASSERT_TRUE(owner);
    const bool shared = scope == Scope::kPerOuterSite;
    ASSERT_EQ(
        owner == content::GetAlloyEmbeddedStateForDocument(second).temporary,
        shared);
    ASSERT_NE(owner, content::GetAlloyEmbeddedStateForDocument(peer).temporary);
    auto* top = static_cast<content::RenderFrameHostImpl*>(
        web_contents()->GetPrimaryMainFrame());
    ASSERT_EQ(alpha->GetStoragePartition(), top->GetStoragePartition());
    for (auto* frame : {top, alpha, second, peer}) {
      ASSERT_TRUE(content::ChildProcessSecurityPolicyImpl::GetInstance()
                      ->GetProcessLock(frame->GetProcess()->GetID())
                      .IsLockedToSite())
          << "the native code-cache control requires real process isolation";
    }
    ASSERT_TRUE(alpha->GetStoragePartition()->GetGeneratedCodeCacheContext());
    auto first_host = Bind(alpha);
    auto second_host = Bind(second);
    auto peer_host = Bind(peer);
    auto top_host = Bind(top);
    const GURL resource("https://cdn.test/scoped-code.js");
    const std::string resource_key = blink::UrlToCodeCacheKey(resource);
    const auto source_hash = crypto::hash::Sha256("console.log('scope88')");
    const auto source_key = blink::ComposeSourceKeyedCacheKey(source_hash);
    const auto timestamp = base::Time::Now();
    const int64_t signature =
        timestamp.ToDeltaSinceWindowsEpoch().InMicroseconds();
    auto put = [&](Host& host, Type type, std::string_view bytes) {
      host->DidGenerateCacheableMetadata(
          type, resource, timestamp,
          mojo_base::BigBuffer(base::as_byte_span(bytes)));
      host.FlushForTesting();
    };
    struct Retained {
      Type type;
      Reader first;
      Reader second;
      Reader peer;
      Reader top;
      std::optional<persistent_cache::PendingBackend> pending;
    };
    std::vector<Retained> retained;
    for (Type type : {Type::kJavascript, Type::kWebAssembly}) {
      put(top_host, type, "TOP_CDN_CODE88");
      auto top_reader = Open(top_host, type);
      ASSERT_TRUE(top_reader);
      auto ordinary = Read(top_reader, base::as_byte_span(resource_key));
      ASSERT_TRUE(ordinary.operational && ordinary.hit);
      ASSERT_EQ(ordinary.bytes, "TOP_CDN_CODE88");
      ASSERT_EQ(ordinary.signature, signature);
      if (type == Type::kJavascript) {
        top_host->DidGenerateSourceKeyedCacheableMetadata(
            std::vector<uint8_t>(source_hash.begin(), source_hash.end()),
            mojo_base::BigBuffer(base::byte_span_from_cstring("TOP_SOURCE88")));
        top_host.FlushForTesting();
        auto source = Read(top_reader, source_key);
        ASSERT_TRUE(source.operational && source.hit);
        ASSERT_EQ(source.bytes, "TOP_SOURCE88");
        RecordProperty(
            "positive_frame_code_cache_control",
            "native-exported-js-and-source-bytes-with-response-time");
      }
      put(first_host, type, "EMBEDDED_CODE88");
      put(peer_host, type, "PEER_CODE88");
      Retained readers{type,
                       Open(first_host, type),
                       Open(second_host, type),
                       Open(peer_host, type),
                       std::move(top_reader),
                       Export(first_host, type)};
      ASSERT_TRUE(readers.first && readers.second && readers.peer &&
                  readers.pending);
      auto first_value = Read(readers.first, base::as_byte_span(resource_key));
      ASSERT_TRUE(first_value.operational && first_value.hit);
      ASSERT_EQ(first_value.bytes, "EMBEDDED_CODE88");
      ASSERT_EQ(first_value.signature, signature);
      auto second_value =
          Read(readers.second, base::as_byte_span(resource_key));
      ASSERT_TRUE(second_value.operational);
      ASSERT_EQ(second_value.hit, shared);
      if (shared) {
        ASSERT_EQ(second_value.bytes, "EMBEDDED_CODE88");
      }
      ASSERT_EQ(Read(readers.peer, base::as_byte_span(resource_key)).bytes,
                "PEER_CODE88");
      ASSERT_EQ(Read(readers.top, base::as_byte_span(resource_key)).bytes,
                "TOP_CDN_CODE88");
      if (type == Type::kJavascript) {
        first_host->DidGenerateSourceKeyedCacheableMetadata(
            std::vector<uint8_t>(source_hash.begin(), source_hash.end()),
            mojo_base::BigBuffer(
                base::byte_span_from_cstring("SOURCE_CODE88")));
        first_host.FlushForTesting();
        auto source = Read(readers.first, source_key);
        ASSERT_TRUE(source.operational && source.hit);
        ASSERT_EQ(source.bytes, "SOURCE_CODE88");
        ASSERT_FALSE(Read(readers.peer, source_key).hit);
        ASSERT_EQ(Read(readers.top, source_key).bytes, "TOP_SOURCE88");
      }
      retained.push_back(std::move(readers));
    }
    RecordProperty(
        "positive_frame_code_cache_exports",
        shared ? "native-js-wasm-source-bytes-outer-shared-peer-top-isolated"
               : "native-js-wasm-source-bytes-tab-isolated-peer-top-live");
    owner->Retire();
    task_environment()->RunUntilIdle();
    for (auto& readers : retained) {
      auto old = Read(readers.first, base::as_byte_span(resource_key));
      EXPECT_FALSE(old.operational);
      EXPECT_EQ(old.error,
                persistent_cache::TransactionError::kConnectionError);
      auto delayed = persistent_cache::PersistentCache::Bind(
          persistent_cache::Client::kCodeCache, std::move(*readers.pending));
      EXPECT_FALSE(delayed.has_value());
      if (!delayed.has_value()) {
        EXPECT_EQ(delayed.error(),
                  persistent_cache::TransactionError::kConnectionError);
      }
      EXPECT_EQ(Read(readers.peer, base::as_byte_span(resource_key)).bytes,
                "PEER_CODE88");
      EXPECT_EQ(Read(readers.top, base::as_byte_span(resource_key)).bytes,
                "TOP_CDN_CODE88");
      if (shared) {
        EXPECT_FALSE(
            Read(readers.second, base::as_byte_span(resource_key)).operational);
      } else {
        EXPECT_TRUE(
            Read(readers.second, base::as_byte_span(resource_key)).operational);
      }
      auto late_host = Bind(alpha);
      EXPECT_FALSE(Export(late_host, readers.type));
    }
    auto fresh_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(fresh_tab.get())
        ->NavigateAndCommit(GURL("https://outer.test/fresh"));
    auto fresh_host = Bind(append(fresh_tab.get(), "code-fresh"));
    for (Type type : {Type::kJavascript, Type::kWebAssembly}) {
      auto fresh = Open(fresh_host, type);
      ASSERT_TRUE(fresh);
      auto value = Read(fresh, base::as_byte_span(resource_key));
      EXPECT_TRUE(value.operational);
      EXPECT_FALSE(value.hit);
    }
    if (!HasFailure()) {
      RecordProperty("retired_frame_code_cache_exports",
                     "old-and-pending-readers-denied-late-bind-denied-fresh-"
                     "empty-peer-top-live");
    }
  }

 private:
  base::test::ScopedFeatureList features_;
  base::test::ScopedCommandLine command_line_;
};

TEST_F(NativeDiskEmbeddedCodeCacheTest,
       PerTabFrameCodeCacheExportsEndWithScope) {
  CheckFrameExports(Scope::kPerTab);
}

TEST_F(NativeDiskEmbeddedCodeCacheTest,
       OuterSiteFrameCodeCacheExportsEndWithScope) {
  CheckFrameExports(Scope::kPerOuterSite);
}
TEST_F(NativeDiskEmbeddedCodeCacheTest,
       MemoryPersistentCacheExportsBytesAndAbandonsOriginalReaders) {
  using Cache = persistent_cache::PersistentCache;
  using Storage = persistent_cache::BackendStorage;
  using Collection = persistent_cache::PersistentCacheCollection;
  constexpr auto client = persistent_cache::Client::kCodeCache;
  const auto key = base::byte_span_from_cstring("memory-key");
  const auto body = base::byte_span_from_cstring("NATIVE_MEMORY_PERSISTENT91");
  for (bool wal : {false, true}) {
    auto storage = Storage::CreateInMemory(client);
    ASSERT_TRUE(storage.in_memory());
    ASSERT_TRUE(storage.directory().empty());
    const base::FilePath name("native-owner");
    auto backend = storage.MakeBackend(name, false, wal);
    ASSERT_TRUE(backend.has_value());
    Cache writer(client, std::move(*backend));
    ASSERT_TRUE(writer.Insert(key, body, {.input_signature = 91}).has_value());
    auto exported = storage.ShareReadOnlyConnection(name, writer);
    auto pending = storage.ShareReadOnlyConnection(name, writer);
    ASSERT_TRUE(exported && pending);
    auto bound = Cache::Bind(client, std::move(*exported));
    ASSERT_TRUE(bound.has_value());
    Reader reader = std::move(*bound);
    ASSERT_TRUE(reader->GetBackendForTesting()->IsReadOnly());
    auto value = Read(reader, key);
    ASSERT_TRUE(value.operational && value.hit);
    ASSERT_EQ(value.bytes, "NATIVE_MEMORY_PERSISTENT91");
    ASSERT_EQ(value.signature, 91);
    auto denied_write = reader->Insert(key, body);
    ASSERT_FALSE(denied_write.has_value());
    ASSERT_EQ(Read(reader, key).bytes, "NATIVE_MEMORY_PERSISTENT91");
    writer.Abandon();
    auto retired = Read(reader, key);
    EXPECT_FALSE(retired.operational);
    EXPECT_EQ(retired.error,
              persistent_cache::TransactionError::kConnectionError);
    auto delayed = Cache::Bind(client, std::move(*pending));
    EXPECT_FALSE(delayed.has_value());
    if (!delayed.has_value()) {
      EXPECT_EQ(delayed.error(),
                persistent_cache::TransactionError::kConnectionError);
    }
  }
  auto lifetime = std::make_shared<persistent_cache::MemoryBackendLifetime>();
  auto owner = Collection::CreateInMemory(
      1024 * 1024, client, Collection::kDefaultLruCacheCapacity, lifetime);
  auto peer = Collection::CreateInMemory(1024 * 1024, client);
  ASSERT_TRUE(
      owner->Insert("context", key, body, {.input_signature = 91}).has_value());
  ASSERT_TRUE(
      peer->Insert("context", key, body, {.input_signature = 92}).has_value());
  auto exported = owner->ShareReadOnlyConnection("context");
  auto pending = owner->ShareReadOnlyConnection("context");
  auto peer_exported = peer->ShareReadOnlyConnection("context");
  ASSERT_TRUE(exported && pending && peer_exported);
  auto bound = Cache::Bind(client, std::move(*exported));
  auto peer_bound = Cache::Bind(client, std::move(*peer_exported));
  ASSERT_TRUE(bound.has_value() && peer_bound.has_value());
  Reader reader = std::move(*bound);
  Reader peer_reader = std::move(*peer_bound);
  ASSERT_EQ(Read(reader, key).signature, 91);
  ASSERT_EQ(Read(peer_reader, key).signature, 92);
  RecordProperty(
      "positive_memory_persistent_cache",
      "native-sqlite-rollback-wal-bytes-metadata-readonly-peer-isolated");
  lifetime->Abandon();
  auto immediate = Read(reader, key);
  EXPECT_FALSE(immediate.operational);
  EXPECT_EQ(immediate.error,
            persistent_cache::TransactionError::kConnectionError);
  owner.reset();
  auto retired = Read(reader, key);
  EXPECT_FALSE(retired.operational);
  EXPECT_EQ(retired.error,
            persistent_cache::TransactionError::kConnectionError);
  auto delayed = Cache::Bind(client, std::move(*pending));
  EXPECT_FALSE(delayed.has_value());
  EXPECT_EQ(Read(peer_reader, key).bytes, "NATIVE_MEMORY_PERSISTENT91");
  EXPECT_EQ(Read(peer_reader, key).signature, 92);
  auto fresh = Collection::CreateInMemory(1024 * 1024, client);
  auto fresh_exported = fresh->ShareReadOnlyConnection("context");
  ASSERT_TRUE(fresh_exported);
  auto fresh_bound = Cache::Bind(client, std::move(*fresh_exported));
  ASSERT_TRUE(fresh_bound.has_value());
  Reader fresh_reader = std::move(*fresh_bound);
  auto empty = Read(fresh_reader, key);
  EXPECT_TRUE(empty.operational);
  EXPECT_FALSE(empty.hit);
  if (!HasFailure()) {
    RecordProperty("retired_memory_persistent_cache",
                   "native-abandon-and-owner-destruction-deny-bound-pending-"
                   "fresh-empty-peer-live");
  }
}
TEST_F(NativeDiskEmbeddedCodeCacheTest,
       DiskCollectionClosePreservesHeldReadAndColdReopen) {
  using Cache = persistent_cache::PersistentCache;
  using Collection = persistent_cache::PersistentCacheCollection;
  constexpr auto client = persistent_cache::Client::kCodeCache;
  const auto key = base::byte_span_from_cstring("disk-key");
  const std::string body = "native-disk-cache-survives-owner-close";
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  auto owner = std::make_unique<Collection>(directory.GetPath(), 1024 * 1024,
                                            client);
  ASSERT_TRUE(owner->Insert("context", key, base::as_byte_span(body),
                             {.input_signature = 94}).has_value());
  auto pending = owner->ShareReadOnlyConnection("context");
  ASSERT_TRUE(pending);
  auto bound = Cache::Bind(client, std::move(*pending));
  ASSERT_TRUE(bound.has_value());
  Reader reader = std::move(*bound);
  std::string bytes;
  bool closed_during_read = false;
  // SqliteBackendImpl::FindImpl invokes the buffer provider while its native
  // read transaction/blob holds a shared lock. Close only the independent
  // collection owner at this boundary, not the reader whose call is active.
  auto found = reader->Find(key, [&](size_t size) {
    bytes.resize(size);
    owner.reset();
    closed_during_read = true;
    return base::as_writable_byte_span(bytes);
  });
  ASSERT_TRUE(closed_during_read);
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(found->has_value());
  EXPECT_EQ(bytes, body);
  EXPECT_EQ(found->value().input_signature, 94);
  reader.reset();
  Collection reopened(directory.GetPath(), 1024 * 1024, client);
  auto fresh_pending = reopened.ShareReadOnlyConnection("context");
  ASSERT_TRUE(fresh_pending);
  auto fresh_bound = Cache::Bind(client, std::move(*fresh_pending));
  ASSERT_TRUE(fresh_bound.has_value());
  Reader fresh = std::move(*fresh_bound);
  const auto retained = Read(fresh, key);
  EXPECT_TRUE(retained.operational && retained.hit);
  EXPECT_EQ(retained.bytes, body);
  EXPECT_EQ(retained.signature, 94);
}

class NativeDiskEmbeddedCookieTest : public NativeDiskEmbeddedLocalStorageTest {
 protected:
  NativeDiskEmbeddedCookieTest() {
    command_line_.GetProcessCommandLine()->AppendSwitch(
        switches::kSitePerProcess);
    content::SiteIsolationPolicy::DisableFlagCachingForTesting();
  }
  enum class Exception { kEntity, kReverseEntity, kGrant, kShieldsDown };
  using Remote = mojo::Remote<network::mojom::RestrictedCookieManager>;
  using Frame = content::RenderFrameHostImpl;

  static Remote Bind(Frame* frame) {
    Remote remote;
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(
            "alloy-test-cookie-omit-owner")) {
      // Test-only counterfactual: keep native policy/origin/isolation checks
      // and the embedder hook, but omit the captured owner at the routing
      // boundary.
      auto* partition = static_cast<content::StoragePartitionImpl*>(
          frame->GetStoragePartition());
      partition->CreateRestrictedCookieManager(
          network::mojom::RestrictedCookieManagerRole::SCRIPT,
          frame->GetLastCommittedOrigin(),
          frame->GetIsolationInfoForSubresources(), false,
          frame->GetProcess()->GetDeprecatedID(), frame->GetRoutingID(),
          content::GetContentClientForTesting()
                  ->browser()
                  ->GetEffectiveTopFrameForPartitioning(frame) != nullptr,
          frame->GetCookieSettingOverrides(), {},
          remote.BindNewPipeAndPassReceiver(), mojo::NullRemote());
    } else {
      frame->BindRestrictedCookieManager(remote.BindNewPipeAndPassReceiver());
    }
    return remote;
  }

  static bool Put(Remote& remote,
                  Frame* frame,
                  std::string_view name,
                  std::string_view value,
                  net::StorageAccessApiStatus status) {
    const auto& isolation = frame->GetIsolationInfoForSubresources();
    base::test::TestFuture<bool> reply;
    const auto now = base::Time::Now();
    remote->SetCanonicalCookie(
        network::mojom::RestrictedCanonicalCookieParams::New(
            std::string(name), std::string(value),
            std::string(frame->GetLastCommittedURL().host()), "/", now,
            now + base::Days(1), now, true, false,
            net::CookieSameSite::NO_RESTRICTION, net::COOKIE_PRIORITY_DEFAULT,
            network::mojom::RestrictedCookiePartition::UNPARTITIONED),
        frame->GetLastCommittedURL(), isolation.site_for_cookies(),
        *isolation.top_frame_origin(), status, false, false,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(reply.GetCallback(),
                                                    false));
    return reply.Get();
  }

  static std::vector<net::CookieWithAccessResult>
  Read(Remote& remote, Frame* frame, net::StorageAccessApiStatus status) {
    const auto& isolation = frame->GetIsolationInfoForSubresources();
    auto options = network::mojom::CookieManagerGetOptions::New();
    options->name = "";
    options->match_type = network::mojom::CookieMatchType::STARTS_WITH;
    base::test::TestFuture<const std::vector<net::CookieWithAccessResult>&>
        reply;
    remote->GetAllForUrl(
        frame->GetLastCommittedURL(), isolation.site_for_cookies(),
        *isolation.top_frame_origin(), status, std::move(options), false, false,
        false,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            reply.GetCallback(), std::vector<net::CookieWithAccessResult>()));
    return reply.Take();
  }

  static std::string Value(
      const std::vector<net::CookieWithAccessResult>& values,
      std::string_view name) {
    for (const auto& item : values) {
      if (item.cookie.Name() == name) {
        EXPECT_TRUE(item.access_result.status.IsInclude());
        return item.cookie.Value();
      }
    }
    return {};
  }

  void CheckCookies(Scope scope, Exception exception) {
    ASSERT_FALSE(profile()->IsOffTheRecord());
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    const bool reverse_entity = exception == Exception::kReverseEntity;
    const bool entity = exception == Exception::kEntity || reverse_entity;
    const bool grant = exception == Exception::kGrant;
    const GURL outer(reverse_entity ? "https://wp.com/"
                     : entity       ? "https://wordpress.com/"
                                    : "https://outer.test/");
    const GURL embedded(reverse_entity ? "https://wordpress.com/"
                        : entity       ? "https://wp.com/"
                                       : "https://alpha.test/");
    const GURL foreign_outer("https://other-outer.test/");
    auto settings = CookieSettingsFactory::GetForProfile(profile());
    ASSERT_TRUE(settings);
    settings->SetDefaultCookieSetting(CONTENT_SETTING_ALLOW);
    profile()->GetPrefs()->SetInteger(
        ::prefs::kCookieControlsMode,
        static_cast<int>(
            content_settings::CookieControlsMode::kBlockThirdParty));
    ASSERT_TRUE(settings->ShouldBlockThirdPartyCookies());
    const content_settings::CookieSettingsBase& policy = *settings;
    const auto outer_origin = url::Origin::Create(outer);
    ASSERT_FALSE(policy.IsFullCookieAccessAllowed(
        GURL("https://unrelated.test/"), net::SiteForCookies(), outer_origin,
        {}, std::nullopt));
    auto* map = HostContentSettingsMapFactory::GetForProfile(profile());
    if (!entity) {
      ASSERT_FALSE(policy.IsFullCookieAccessAllowed(
          embedded, net::SiteForCookies(), outer_origin, {}, std::nullopt));
    }
    if (grant) {
      map->SetContentSettingDefaultScope(embedded, outer,
                                         ContentSettingsType::STORAGE_ACCESS,
                                         CONTENT_SETTING_ALLOW);
      ASSERT_EQ(map->GetContentSetting(embedded, outer,
                                       ContentSettingsType::STORAGE_ACCESS),
                CONTENT_SETTING_ALLOW);
      ASSERT_FALSE(policy.IsFullCookieAccessAllowed(
          embedded, net::SiteForCookies(), outer_origin, {}, std::nullopt))
          << "a stored grant still requires the native API opt-in";
    } else if (exception == Exception::kShieldsDown) {
      ASSERT_TRUE(brave_shields::IsBraveShieldsEnabled(map, outer));
      brave_shields::SetBraveShieldsEnabled(map, false, outer);
      ASSERT_FALSE(brave_shields::IsBraveShieldsEnabled(map, outer));
    }
    net::CookieSettingOverrides overrides;
    if (grant)
      overrides.Put(net::CookieSettingOverride::kStorageAccessGrantEligible);
    ASSERT_TRUE(policy.IsFullCookieAccessAllowed(
        embedded, net::SiteForCookies(), outer_origin, overrides,
        std::nullopt));
    const char* policy_value =
        reverse_entity ? "native-reverse-entity-pair-full-cookie-access"
        : entity       ? "native-entity-pair-full-cookie-access"
        : grant        ? "native-storage-access-grant-full-cookie-access"
                       : "native-shields-down-full-cookie-access";
    RecordProperty("positive_cookie_exception_policy", policy_value);

    content::WebContentsTester::For(web_contents())->NavigateAndCommit(outer);
    auto second_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(second_tab.get())->NavigateAndCommit(outer);
    auto peer_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(peer_tab.get())
        ->NavigateAndCommit(foreign_outer);
    auto real_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(real_tab.get())
        ->NavigateAndCommit(embedded);
    auto append = [&](content::WebContents* tab, const char* name) {
      auto* initial =
          content::RenderFrameHostTester::For(tab->GetPrimaryMainFrame())
              ->AppendChild(name);
      return static_cast<Frame*>(
          content::NavigationSimulator::NavigateAndCommitFromDocument(embedded,
                                                                      initial));
    };
    auto* first = append(web_contents(), "cookie-first");
    auto* second = append(second_tab.get(), "cookie-second");
    auto* peer = append(peer_tab.get(), "cookie-peer");
    auto* real = static_cast<Frame*>(real_tab->GetPrimaryMainFrame());
    for (auto* frame : {first, second, peer, real}) {
      ASSERT_TRUE(content::ChildProcessSecurityPolicyImpl::GetInstance()
                      ->GetProcessLock(frame->GetProcess()->GetID())
                      .IsLockedToSite());
    }
    auto owner = content::GetAlloyEmbeddedStateForDocument(first).temporary;
    auto second_owner =
        content::GetAlloyEmbeddedStateForDocument(second).temporary;
    auto peer_owner = content::GetAlloyEmbeddedStateForDocument(peer).temporary;
    const bool shared = scope == Scope::kPerOuterSite;
    ASSERT_TRUE(owner && second_owner && peer_owner);
    ASSERT_EQ(owner == second_owner, shared);
    ASSERT_NE(owner, peer_owner);
    ASSERT_FALSE(content::GetAlloyEmbeddedStateForDocument(first)
                     .considered_first_party);
    auto first_lease = content::GetAlloyNetworkGenerationForDocument(first);
    ASSERT_TRUE(first_lease && first_lease->memory_only());
    ASSERT_NE(first_lease->GetNetworkContext(),
              content::GetAlloyNetworkContextForDocument(real));
    ASSERT_EQ(first->GetStoragePartition(), real->GetStoragePartition());
    auto real_remote = Bind(real);
    constexpr auto no_api = net::StorageAccessApiStatus::kNone;
    const auto api =
        grant ? net::StorageAccessApiStatus::kAccessViaAPI : no_api;
    ASSERT_TRUE(Put(real_remote, real, "real_only", "REAL_COOKIE93", no_api));
    ASSERT_EQ(Value(Read(real_remote, real, no_api), "real_only"),
              "REAL_COOKIE93");
    RecordProperty("positive_cookie_exception_control",
                   "native-first-party-restricted-cookie-control-live");
    auto first_remote = Bind(first);
    auto second_remote = Bind(second);
    auto peer_remote = Bind(peer);
    ASSERT_TRUE(Value(Read(first_remote, first, api), "real_only").empty());
    ASSERT_TRUE(Value(Read(second_remote, second, api), "real_only").empty());
    ASSERT_TRUE(
        Put(first_remote, first, "temporary_cookie", "TEMP_COOKIE93", api));
    ASSERT_EQ(Value(Read(first_remote, first, api), "temporary_cookie"),
              "TEMP_COOKIE93");
    ASSERT_EQ(Value(Read(second_remote, second, api), "temporary_cookie"),
              shared ? "TEMP_COOKIE93" : "");
    ASSERT_TRUE(
        Put(peer_remote, peer, "temporary_cookie", "PEER_COOKIE93", no_api));
    ASSERT_EQ(Value(Read(peer_remote, peer, no_api), "temporary_cookie"),
              "PEER_COOKIE93");
    ASSERT_EQ(Value(Read(first_remote, first, api), "temporary_cookie"),
              "TEMP_COOKIE93");
    ASSERT_TRUE(Value(Read(first_remote, first, api), "real_only").empty());
    ASSERT_TRUE(
        Value(Read(real_remote, real, no_api), "temporary_cookie").empty());
    ASSERT_EQ(Value(Read(real_remote, real, no_api), "real_only"),
              "REAL_COOKIE93");
    RecordProperty(
        "positive_cookie_exception_storage",
        shared ? "native-restricted-cookie-bytes-outer-shared-real-hidden"
               : "native-restricted-cookie-bytes-tab-isolated-real-hidden");
    owner->Retire();
    task_environment()->RunUntilIdle();
    EXPECT_TRUE(Read(first_remote, first, api).empty());
    EXPECT_FALSE(Put(first_remote, first, "late_cookie", "LATE_COOKIE93", api));
    auto late = Bind(first);
    EXPECT_TRUE(Read(late, first, api).empty());
    EXPECT_FALSE(Put(late, first, "late_cookie", "LATE_COOKIE93", api));
    if (shared) {
      EXPECT_TRUE(Read(second_remote, second, api).empty());
    } else {
      ASSERT_TRUE(
          Put(second_remote, second, "second_cookie", "SECOND_COOKIE93", api));
      EXPECT_EQ(Value(Read(second_remote, second, api), "second_cookie"),
                "SECOND_COOKIE93");
    }
    EXPECT_EQ(Value(Read(peer_remote, peer, no_api), "temporary_cookie"),
              "PEER_COOKIE93");
    EXPECT_EQ(Value(Read(real_remote, real, no_api), "real_only"),
              "REAL_COOKIE93");
    auto fresh_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(fresh_tab.get())->NavigateAndCommit(outer);
    auto* fresh = append(fresh_tab.get(), "cookie-fresh");
    ASSERT_NE(content::GetAlloyEmbeddedStateForDocument(fresh).temporary,
              owner);
    auto fresh_remote = Bind(fresh);
    EXPECT_TRUE(Read(fresh_remote, fresh, api).empty());
    ASSERT_TRUE(
        Put(fresh_remote, fresh, "fresh_cookie", "FRESH_COOKIE93", api));
    EXPECT_EQ(Value(Read(fresh_remote, fresh, api), "fresh_cookie"),
              "FRESH_COOKIE93");
    if (!HasFailure())
      RecordProperty("retired_cookie_exception_storage",
                     "old-and-late-denied-fresh-empty-peer-real-live");
  }

 private:
  base::test::ScopedCommandLine command_line_;
};

TEST_F(NativeDiskEmbeddedCookieTest, PerTabEntityCookiesStayTemporary) {
  CheckCookies(Scope::kPerTab, Exception::kEntity);
}
TEST_F(NativeDiskEmbeddedCookieTest, OuterSiteEntityCookiesStayTemporary) {
  CheckCookies(Scope::kPerOuterSite, Exception::kEntity);
}
TEST_F(NativeDiskEmbeddedCookieTest, PerTabReverseEntityCookiesStayTemporary) {
  CheckCookies(Scope::kPerTab, Exception::kReverseEntity);
}
TEST_F(NativeDiskEmbeddedCookieTest,
       OuterSiteReverseEntityCookiesStayTemporary) {
  CheckCookies(Scope::kPerOuterSite, Exception::kReverseEntity);
}
TEST_F(NativeDiskEmbeddedCookieTest, PerTabStorageAccessCookiesStayTemporary) {
  CheckCookies(Scope::kPerTab, Exception::kGrant);
}
TEST_F(NativeDiskEmbeddedCookieTest,
       OuterSiteStorageAccessCookiesStayTemporary) {
  CheckCookies(Scope::kPerOuterSite, Exception::kGrant);
}
TEST_F(NativeDiskEmbeddedCookieTest, PerTabShieldsDownCookiesStayTemporary) {
  CheckCookies(Scope::kPerTab, Exception::kShieldsDown);
}
TEST_F(NativeDiskEmbeddedCookieTest, OuterSiteShieldsDownCookiesStayTemporary) {
  CheckCookies(Scope::kPerOuterSite, Exception::kShieldsDown);
}
class NativeDiskEmbeddedOriginTrialsTest
    : public NativeDiskEmbeddedLocalStorageTest {
 protected:
  NativeDiskEmbeddedOriginTrialsTest() {
    command_line_.GetProcessCommandLine()->AppendSwitch(
        switches::kSitePerProcess);
    content::SiteIsolationPolicy::DisableFlagCachingForTesting();
  }
  using Frame = content::RenderFrameHostImpl;
  using TrialHost = mojo::Remote<blink::mojom::OriginTrialStateHost>;
  // Published Chromium fixture, origin_trials_unittest.cc. Real signature;
  // native ScopedTestOriginTrialPolicy supplies its test public key.
  static constexpr char kToken[] =
      "A5J4wscfmyiy3zsOMNASl25jJffHq/XGKNoFCSWoAsL0aXv+ugtTQoE/"
      "Cs3a6pDezyQpKfYy10kcbdVC4i1n8AQAAABleyJvcmlnaW4iOiAiaHR0cHM6Ly9lbmFibGVk"
      "Lm"
      "V4YW1wbGUuY29tOjQ0MyIsICJmZWF0dXJlIjogIkZyb2J1bGF0ZVBlcnNpc3RlbnQiLCAiZX"
      "hw"
      "aXJ5IjogMjAwMDAwMDAwMH0=";

  static scoped_refptr<net::HttpResponseHeaders> Headers(bool token) {
    return net::HttpResponseHeaders::TryToCreate(
        std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n") +
        (token ? std::string("Origin-Trial: ") + kToken + "\r\n" : "") +
        "\r\n");
  }
  static TrialHost Bind(Frame* frame) {
    TrialHost host;
    content::OriginTrialStateHostImpl::Create(
        frame, host.BindNewPipeAndPassReceiver());
    return host;
  }
  static void Script(TrialHost& host, const std::string& token) {
    host->EnablePersistentTrial(token, {});
    host.FlushForTesting();
  }
  static Frame* Navigate(content::WebContents* contents,
                         const GURL& url,
                         bool token) {
    auto navigation =
        content::NavigationSimulator::CreateBrowserInitiated(url, contents);
    navigation->Start();
    navigation->SetResponseHeaders(Headers(token));
    navigation->SetContentsMimeType("text/html");
    navigation->Commit();
    return static_cast<Frame*>(navigation->GetFinalRenderFrameHost());
  }

  void CheckProfileAdmission(Scope scope, bool script) {
    ASSERT_FALSE(profile()->IsOffTheRecord());
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    const GURL enabled("https://enabled.example.com/");
    const GURL outer("https://trial-outer.test/");
    const auto origin = url::Origin::Create(enabled);
    const auto partition = url::Origin::Create(outer);
    auto* delegate = profile()->GetOriginTrialsControllerDelegate();
    ASSERT_TRUE(delegate);
    task_environment()->RunUntilIdle();
    delegate->ClearPersistedTokens();
    blink::TrialTokenValidator validator;
    ASSERT_EQ(validator.ValidateTokenAndTrial(kToken, origin, base::Time::Now())
                  .Status(),
              blink::OriginTrialTokenStatus::kSuccess);
    ASSERT_NE(
        validator
            .ValidateTokenAndTrial("invalid-token", origin, base::Time::Now())
            .Status(),
        blink::OriginTrialTokenStatus::kSuccess);
    ASSERT_NE(
        validator.ValidateTokenAndTrial(kToken, partition, base::Time::Now())
            .Status(),
        blink::OriginTrialTokenStatus::kSuccess);
    auto enabled_for = [&](const url::Origin& top) {
      return delegate->IsFeaturePersistedForOrigin(
          origin, top,
          blink::mojom::OriginTrialFeature::
              kOriginTrialsSampleAPIPersistentFeature,
          base::Time::Now());
    };
    ASSERT_FALSE(enabled_for(origin));
    ASSERT_FALSE(enabled_for(partition));
    auto real_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    auto* real = Navigate(real_tab.get(), enabled, !script);
    TrialHost real_host;
    if (script) {
      real_host = Bind(real);
      Script(real_host, "invalid-token");
      ASSERT_FALSE(enabled_for(origin));
      Script(real_host, kToken);
    }
    ASSERT_TRUE(enabled_for(origin))
        << "ordinary signed-token producer must enable the native feature";
    ASSERT_FALSE(enabled_for(partition));
    RecordProperty("positive_origin_trial_control",
                   script ? "native-signed-script-feature-enabled"
                          : "native-signed-header-feature-enabled");

    content::WebContentsTester::For(web_contents())->NavigateAndCommit(outer);
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild("origin-trial-frame");
    auto navigation =
        content::NavigationSimulator::CreateRendererInitiated(enabled, initial);
    navigation->Start();
    navigation->SetResponseHeaders(Headers(!script));
    navigation->SetContentsMimeType("text/html");
    navigation->Commit();
    auto* frame = static_cast<Frame*>(navigation->GetFinalRenderFrameHost());
    ASSERT_EQ(frame->GetStoragePartition(), real->GetStoragePartition());
    auto owner = content::GetAlloyEmbeddedStateForDocument(frame).temporary;
    ASSERT_TRUE(owner);
    TrialHost held;
    if (script) {
      held = Bind(frame);
      Script(held, kToken);
    }
    ASSERT_FALSE(enabled_for(partition))
        << "an unlisted embedded producer must not seed the ordinary Profile "
           "trial store";
    ASSERT_TRUE(enabled_for(origin));
    RecordProperty("isolated_origin_trial_profile",
                   "embedded-token-not-in-profile-store");
    auto scoped =
        delegate->GetForStateOwner(content::AlloyDocumentStateOwner(frame));
    ASSERT_TRUE(scoped && scoped->IsStateAdmissionOpen());
    auto scoped_enabled =
        [&](const std::shared_ptr<content::OriginTrialsControllerDelegate>&
                value,
            const url::Origin& top) {
          return value->IsFeaturePersistedForOrigin(
              origin, top,
              blink::mojom::OriginTrialFeature::
                  kOriginTrialsSampleAPIPersistentFeature,
              base::Time::Now());
        };
    ASSERT_TRUE(scoped_enabled(scoped, partition));
    auto second_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(second_tab.get())->NavigateAndCommit(outer);
    auto* second_initial =
        content::RenderFrameHostTester::For(second_tab->GetPrimaryMainFrame())
            ->AppendChild("trial-second");
    auto second_navigation =
        content::NavigationSimulator::CreateRendererInitiated(enabled,
                                                              second_initial);
    second_navigation->Start();
    auto second = delegate->GetForStateOwner(content::AlloyNavigationStateOwner(
        second_navigation->GetNavigationHandle()));
    ASSERT_TRUE(second && second->IsStateAdmissionOpen());
    const bool shared = scope == Scope::kPerOuterSite;
    ASSERT_EQ(scoped_enabled(second, partition), shared);
    content::CriticalOriginTrialsThrottle critical(second, partition,
                                                   std::nullopt);
    network::ResourceRequest request;
    request.url = enabled;
    request.method = "GET";
    request.resource_type =
        static_cast<int>(blink::mojom::ResourceType::kSubFrame);
    bool defer = false;
    critical.WillStartRequest(&request, &defer);
    ASSERT_FALSE(defer);
    network::mojom::URLResponseHead critical_response;
    critical_response.headers = net::HttpResponseHeaders::TryToCreate(
        std::string("HTTP/1.1 200 OK\r\nOrigin-Trial: ") + kToken +
        "\r\nCritical-Origin-Trial: FrobulatePersistent\r\n\r\n");
    blink::URLLoaderThrottle::RestartWithURLReset restart(false);
    critical.BeforeWillProcessResponse(enabled, critical_response, &restart);
    ASSERT_EQ(restart.value(), !shared);
    ASSERT_TRUE(scoped_enabled(second, partition));
    second_navigation->SetResponseHeaders(Headers(true));
    second_navigation->SetContentsMimeType("text/html");
    second_navigation->Commit();
    auto peer_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    const GURL peer_outer("https://trial-peer.test/");
    const auto peer_partition = url::Origin::Create(peer_outer);
    content::WebContentsTester::For(peer_tab.get())
        ->NavigateAndCommit(peer_outer);
    auto* peer_initial =
        content::RenderFrameHostTester::For(peer_tab->GetPrimaryMainFrame())
            ->AppendChild("trial-peer");
    auto peer_navigation =
        content::NavigationSimulator::CreateRendererInitiated(enabled,
                                                              peer_initial);
    peer_navigation->Start();
    auto peer = delegate->GetForStateOwner(content::AlloyNavigationStateOwner(
        peer_navigation->GetNavigationHandle()));
    ASSERT_TRUE(peer && peer->IsStateAdmissionOpen());
    ASSERT_FALSE(scoped_enabled(peer, peer_partition));
    peer_navigation->SetResponseHeaders(Headers(true));
    peer_navigation->SetContentsMimeType("text/html");
    peer_navigation->Commit();
    ASSERT_TRUE(scoped_enabled(peer, peer_partition));
    ASSERT_TRUE(scoped_enabled(scoped, partition));
    ASSERT_FALSE(enabled_for(partition));
    RecordProperty("positive_scoped_origin_trial",
                   "native-signed-feature-scope-sharing-critical-restart-"
                   "profile-isolated");

    if (script) {
      owner->Retire();
      Script(held, kToken);
      EXPECT_FALSE(enabled_for(partition));
      auto late = Bind(frame);
      Script(late, kToken);
      EXPECT_FALSE(enabled_for(partition));
    } else {
      auto pending =
          content::NavigationSimulator::CreateRendererInitiated(enabled, frame);
      pending->Start();
      auto captured = content::GetAlloyEmbeddedStateForNavigation(
          static_cast<content::NavigationRequest*>(
              pending->GetNavigationHandle()),
          origin);
      ASSERT_EQ(captured.temporary, owner);
      owner->Retire();
      pending->SetResponseHeaders(Headers(true));
      pending->SetContentsMimeType("text/html");
      pending->Commit();
      EXPECT_FALSE(enabled_for(partition));
    }
    EXPECT_TRUE(enabled_for(origin));
    EXPECT_FALSE(scoped->IsStateAdmissionOpen());
    EXPECT_FALSE(scoped_enabled(scoped, partition));
    EXPECT_EQ(scoped_enabled(second, partition), !shared);
    EXPECT_TRUE(scoped_enabled(peer, peer_partition));
    critical.BeforeWillProcessResponse(enabled, critical_response, &restart);
    EXPECT_FALSE(scoped->IsStateAdmissionOpen());
    auto fresh_tab =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::WebContentsTester::For(fresh_tab.get())->NavigateAndCommit(outer);
    auto* fresh_initial =
        content::RenderFrameHostTester::For(fresh_tab->GetPrimaryMainFrame())
            ->AppendChild("trial-fresh");
    auto fresh_navigation =
        content::NavigationSimulator::CreateRendererInitiated(enabled,
                                                              fresh_initial);
    fresh_navigation->Start();
    fresh_navigation->SetResponseHeaders(Headers(false));
    fresh_navigation->SetContentsMimeType("text/html");
    fresh_navigation->Commit();
    auto* fresh_frame =
        static_cast<Frame*>(fresh_navigation->GetFinalRenderFrameHost());
    auto fresh = delegate->GetForStateOwner(
        content::AlloyDocumentStateOwner(fresh_frame));
    ASSERT_TRUE(fresh && fresh->IsStateAdmissionOpen());
    ASSERT_FALSE(scoped_enabled(fresh, partition));
    auto fresh_host = Bind(fresh_frame);
    Script(fresh_host, kToken);
    EXPECT_TRUE(scoped_enabled(fresh, partition));
    EXPECT_FALSE(enabled_for(partition));
    if (!HasFailure())
      RecordProperty("retired_scoped_origin_trial",
                     "closed-native-delegate-fresh-empty-writable-peer-live");
    if (!HasFailure())
      RecordProperty(
          "retired_origin_trial_profile",
          script
              ? "retained-and-late-script-no-profile-write-real-control-live"
              : "captured-pending-header-no-profile-write-real-control-live");
  }

  void CheckRuntimeProjection(Scope scope) {
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    content::WebContentsTester::For(web_contents())
        ->NavigateAndCommit(GURL("https://runtime-outer.test/"));
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild("runtime-trial");
    auto* frame = static_cast<Frame*>(
        content::NavigationSimulator::NavigateAndCommitFromDocument(
            GURL("https://127.0.0.1:44444/title.html"), initial));
    auto owner = content::GetAlloyEmbeddedStateForDocument(frame).temporary;
    ASSERT_TRUE(owner);
    // Published real browser-readable token from
    // render_frame_host_impl_browsertest.
    const std::string token =
        "A9pmJuEBwdw0u+"
        "U0mayTSMzJDSdJCwGCtqJP3g9T6Xp5wRvqlo4rzhykbgMsYtCrQdcywA7sjs2bQIpEMtyN"
        "rg"
        "kAAACAeyJvcmlnaW4iOiAiaHR0cHM6Ly8xMjcuMC4wLjE6NDQ0NDQiLCAiZmVhdHVyZSI6"
        "IC"
        "JUZXN0RmVhdHVyZUZvckJyb3dzZXJQcm9jZXNzUmVhZFdyaXRlQWNjZXNzT3JpZ2luVHJp"
        "YW"
        "wiLCAiZXhwaXJ5IjogMjAwMDAwMDAwMH0=";
    auto host = Bind(frame);
    auto* data =
        content::RuntimeFeatureStateDocumentData::GetForCurrentDocument(frame);
    ASSERT_TRUE(data);
    const auto baseline =
        data->runtime_feature_state_read_context().GetFeatureOverrides();
    const bool baseline_enabled =
        data->runtime_feature_state_read_context()
            .IsTestFeatureForBrowserProcessReadWriteAccessOriginTrialEnabled();
    auto apply = [&](TrialHost& remote) {
      base::flat_map<blink::mojom::RuntimeFeature,
                     blink::mojom::OriginTrialFeatureStatePtr>
          changes;
      changes[blink::mojom::RuntimeFeature::
                  kTestFeatureForBrowserProcessReadWriteAccessOriginTrial] =
          blink::mojom::OriginTrialFeatureState::New(
              true, std::vector<std::string>{token});
      remote->ApplyFeatureDiffForOriginTrial(std::move(changes));
      remote.FlushForTesting();
    };
    apply(host);
    ASSERT_TRUE(
        data->runtime_feature_state_read_context()
            .IsTestFeatureForBrowserProcessReadWriteAccessOriginTrialEnabled());
    RecordProperty("positive_runtime_origin_trial",
                   "native-signed-browser-feature-override-live");
    owner->Retire();
    EXPECT_EQ(data->runtime_feature_state_read_context().GetFeatureOverrides(),
              baseline);
    EXPECT_EQ(
        data->runtime_feature_state_read_context()
            .IsTestFeatureForBrowserProcessReadWriteAccessOriginTrialEnabled(),
        baseline_enabled);
    apply(host);
    auto late = Bind(frame);
    apply(late);
    EXPECT_EQ(data->runtime_feature_state_read_context().GetFeatureOverrides(),
              baseline);
    if (!HasFailure())
      RecordProperty("retired_runtime_origin_trial",
                     "original-feature-baseline-restored-held-late-denied");
  }

 private:
  base::test::ScopedCommandLine command_line_;
  blink::ScopedTestOriginTrialPolicy trial_policy_;
};

TEST_F(NativeDiskEmbeddedOriginTrialsTest, PerTabHeaderTrialsDoNotSeedProfile) {
  CheckProfileAdmission(Scope::kPerTab, false);
}
TEST_F(NativeDiskEmbeddedOriginTrialsTest,
       OuterSiteHeaderTrialsDoNotSeedProfile) {
  CheckProfileAdmission(Scope::kPerOuterSite, false);
}
TEST_F(NativeDiskEmbeddedOriginTrialsTest, PerTabScriptTrialsDoNotSeedProfile) {
  CheckProfileAdmission(Scope::kPerTab, true);
}
TEST_F(NativeDiskEmbeddedOriginTrialsTest,
       OuterSiteScriptTrialsDoNotSeedProfile) {
  CheckProfileAdmission(Scope::kPerOuterSite, true);
}
TEST_F(NativeDiskEmbeddedOriginTrialsTest,
       PerTabRuntimeTrialProjectionEndsWithScope) {
  CheckRuntimeProjection(Scope::kPerTab);
}
TEST_F(NativeDiskEmbeddedOriginTrialsTest,
       OuterSiteRuntimeTrialProjectionEndsWithScope) {
  CheckRuntimeProjection(Scope::kPerOuterSite);
}
class NativeDiskEmbeddedFaviconTest : public ChromeRenderViewHostTestHarness {
 public:
  void Check(Scope scope) {
    ASSERT_TRUE(content::ConfigureAlloyEmbeddedStateScope(*profile(), scope));
    ASSERT_TRUE(HistoryServiceFactory::GetInstance()->SetTestingFactoryAndUse(
        profile(), HistoryServiceFactory::GetDefaultFactory()));
    auto* service = static_cast<favicon::FaviconService*>(
        FaviconServiceFactory::GetInstance()->SetTestingFactoryAndUse(
            profile(), FaviconServiceFactory::GetDefaultFactory()));
    ASSERT_TRUE(service);
    favicon::ContentFaviconDriver::CreateForWebContents(web_contents(),
                                                        service);
    auto* driver =
        favicon::ContentFaviconDriver::FromWebContents(web_contents());
    auto* tester = content::WebContentsTester::For(web_contents());
    const GURL top("https://favicon-outer.test/page");
    const GURL embedded("https://favicon-cdn.test/frame");
    const GURL icon("https://favicon-cdn.test/outer-icon.png");
    const GURL rejected("https://favicon-cdn.test/embedded-icon.png");
    service->AddPageNoVisitForBookmark(top, u"Top-level favicon owner");
    tester->NavigateAndCommit(top);
    auto* initial = content::RenderFrameHostTester::For(
                        web_contents()->GetPrimaryMainFrame())
                        ->AppendChild("favicon-child");
    auto* child = content::NavigationSimulator::NavigateAndCommitFromDocument(
        embedded, initial);
    auto selected = content::GetAlloyEmbeddedStateForDocument(child).temporary;
    ASSERT_TRUE(selected);
    auto candidates = [](const GURL& url) {
      std::vector<blink::mojom::FaviconURLPtr> result;
      result.push_back(blink::mojom::FaviconURL::New(
          url, blink::mojom::FaviconIconType::kFavicon,
          std::vector<gfx::Size>{gfx::Size(16, 16)}, false));
      return result;
    };
    auto update = [&](content::RenderFrameHost* frame, const GURL& url) {
      static_cast<content::WebContentsImpl*>(web_contents())
          ->UpdateFaviconURL(static_cast<content::RenderFrameHostImpl*>(frame),
                             candidates(url),
                             blink::mojom::FaviconUpdateReason::kPageLoad);
    };
    update(web_contents()->GetPrimaryMainFrame(), icon);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return tester->HasPendingDownloadImage(icon); }));
    SkBitmap blue;
    blue.allocN32Pixels(16, 16);
    blue.eraseColor(SK_ColorBLUE);
    ASSERT_TRUE(
        tester->TestDidDownloadImage(icon, 200, {blue}, {gfx::Size(16, 16)}));
    auto read_icon = [&](const GURL& page) {
      base::CancelableTaskTracker tracker;
      base::test::TestFuture<const favicon_base::FaviconRawBitmapResult&>
          result;
      service->GetRawFaviconForPageURL(page, {favicon_base::IconType::kFavicon},
                                       16, false, result.GetCallback(),
                                       &tracker);
      return result.Get();
    };
    const auto control = read_icon(top);
    ASSERT_TRUE(control.is_valid());
    ASSERT_EQ(control.icon_url, icon);
    ASSERT_EQ(control.pixel_size, gfx::Size(16, 16));
    ASSERT_TRUE(driver->FaviconIsValid());
    ASSERT_EQ(driver->GetFavicon().AsBitmap().getColor(0, 0), SK_ColorBLUE);
    ASSERT_FALSE(read_icon(icon).is_valid());
    ASSERT_FALSE(read_icon(embedded).is_valid());
    RecordProperty("positive_embedded_favicon_owner",
                   "native-primary-foreign-icon-bitmap-and-page-mapping-live");
    update(child, rejected);
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(tester->HasPendingDownloadImage(rejected));
    EXPECT_FALSE(read_icon(embedded).is_valid());
    selected->Retire();
    EXPECT_EQ(read_icon(top).icon_url, icon);
    EXPECT_EQ(driver->GetFavicon().AsBitmap().getColor(0, 0), SK_ColorBLUE);
    const GURL old_page("https://favicon-outer.test/late-page");
    const GURL late_icon("https://favicon-cdn.test/late-icon.png");
    const GURL replacement("https://favicon-replacement.test/page");
    tester->NavigateAndCommit(old_page);
    update(web_contents()->GetPrimaryMainFrame(), late_icon);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return tester->HasPendingDownloadImage(late_icon); }));
    tester->NavigateAndCommit(replacement);
    SkBitmap red;
    red.allocN32Pixels(16, 16);
    red.eraseColor(SK_ColorRED);
    ASSERT_TRUE(tester->TestDidDownloadImage(late_icon, 200, {red},
                                             {gfx::Size(16, 16)}));
    EXPECT_FALSE(read_icon(replacement).is_valid());
    EXPECT_FALSE(read_icon(old_page).is_valid());
    EXPECT_EQ(read_icon(top).icon_url, icon);
    service->DeleteFaviconMappings({top}, favicon_base::IconType::kFavicon);
    EXPECT_FALSE(read_icon(top).is_valid());
    if (!HasFailure())
      RecordProperty("retired_embedded_favicon_owner",
                     "native-nonprimary-rejected-scope-retirement-top-live-"
                     "late-ignored-delete");
  }
};
TEST_F(NativeDiskEmbeddedFaviconTest,
       PerTabPrimaryOwnsForeignIconAndRejectsEmbedded) {
  Check(Scope::kPerTab);
}
TEST_F(NativeDiskEmbeddedFaviconTest,
       OuterSitePrimaryOwnsForeignIconAndRejectsEmbedded) {
  Check(Scope::kPerOuterSite);
}

}  // namespace
}  // namespace alloy::site_records
