// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/buckets/alloy_embedded_bucket_storage.h"

#include <map>
#include <memory>
#include <utility>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "brave/alloy/site_records/embedded_state.h"
#include "components/services/storage/public/cpp/buckets/bucket_locator.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/browser/buckets/bucket_context.h"
#include "content/browser/buckets/bucket_manager.h"
#include "content/browser/cache_storage/cache_storage_control_wrapper.h"
#include "content/browser/file_system_access/file_system_access_error.h"
#include "content/browser/file_system_access/file_system_access_manager_impl.h"
#include "content/browser/indexed_db/indexed_db_control_wrapper.h"
#include "content/browser/quota/quota_context.h"
#include "content/browser/renderer_host/indexed_db_client_state_checker_factory.h"
#include "content/browser/security/cpsp/child_process_security_policy_impl.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_client.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/file_system/external_mount_points.h"
#include "storage/browser/file_system/file_system_backend.h"
#include "storage/browser/file_system/file_system_context.h"
#include "storage/browser/file_system/file_system_options.h"
#include "storage/browser/quota/quota_features.h"
#include "storage/browser/quota/quota_manager.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "storage/browser/quota/quota_settings.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace content {
namespace {
using alloy::site_records::RecordCapability;
using alloy::site_records::StorageAccess;
using alloy::site_records::StorePurpose;
using Capabilities = std::map<StorePurpose, RecordCapability>;
constexpr StorePurpose kPurposes[] = {
    StorePurpose::kQuota,        StorePurpose::kIndexedDb,
    StorePurpose::kCacheStorage, StorePurpose::kFileSystem,
    StorePurpose::kBlobSpill,    StorePurpose::kServiceWorkers};

class BucketStorage final : public AlloyEmbeddedBucketStorage {
 public:
  BucketStorage(StoragePartitionImpl& partition,
                Capabilities capabilities,
                scoped_refptr<alloy::site_records::EmbeddedState> owner)
      : capabilities_(std::move(capabilities)) {
    auto quota_capability = capabilities_.at(StorePurpose::kQuota);
    const bool report_static_storage_quota =
        GetContentClient()
            ->browser()
            ->GetOverrideValueForStaticStorageQuota(partition.browser_context())
            .value_or(base::FeatureList::IsEnabled(
                storage::features::kStaticStorageQuota));
    quota_context_ = base::MakeRefCounted<QuotaContext>(
        true, base::FilePath(), nullptr,
        base::BindRepeating(
            [](storage::OptionalQuotaSettingsCallback callback) {
              storage::GetNominalDynamicSettings(
                  base::FilePath(), true, storage::GetDefaultDeviceInfoHelper(),
                  std::move(callback));
            }),
        report_static_storage_quota,
        base::BindRepeating(
            [](RecordCapability capability) {
              return capability.Allows(StorePurpose::kQuota,
                                       StorageAccess::kWriteMemory);
            },
            quota_capability));
    proxy_ = quota_context_->quota_manager()->proxy();

    // Native managed filesystem memory mode is explicit. A disk profile's
    // CanUseDiskWhenOffTheRecord override cannot enable a temporary backend.
    file_system_ = storage::FileSystemContext::Create(
        GetIOThreadTaskRunner({}),
        base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
        partition.browser_context()->GetMountPoints(), nullptr, proxy_, {}, {},
        base::FilePath(),
        storage::FileSystemOptions(
            storage::FileSystemOptions::PROFILE_MODE_INCOGNITO, false, {}));
    for (const auto type : file_system_->GetFileSystemTypes()) {
      ChildProcessSecurityPolicyImpl::GetInstance()
          ->RegisterFileSystemPermissionPolicy(
              type, storage::FileSystemContext::GetPermissionPolicy(type));
    }
    blob_ = base::MakeRefCounted<ChromeBlobStorageContext>(base::BindRepeating(
        [](RecordCapability capability) {
          return capability.Allows(StorePurpose::kBlobSpill,
                                   StorageAccess::kReadMemory);
        },
        capabilities_.at(StorePurpose::kBlobSpill)));
    GetIOThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&ChromeBlobStorageContext::InitializeOnIOThread, blob_,
                       base::FilePath(), base::FilePath(), nullptr));
    file_system_access_ = base::MakeRefCounted<FileSystemAccessManagerImpl>(
        file_system_, blob_,
        partition.browser_context()->GetFileSystemAccessPermissionContext(),
        true,
        base::BindRepeating(
            [](RecordCapability capability) {
              return capability.Allows(StorePurpose::kFileSystem,
                                       StorageAccess::kWriteMemory);
            },
            capabilities_.at(StorePurpose::kFileSystem)));
    mojo::PendingRemote<storage::mojom::FileSystemAccessContext> file_context;
    file_system_access_->BindInternalsReceiver(
        file_context.InitWithNewPipeAndPassReceiver());
    indexed_db_ = std::make_unique<indexed_db::IndexedDBControlWrapper>(
        base::FilePath(), nullptr, proxy_, BlobRemote(),
        std::move(file_context), GetIOThreadTaskRunner({}),
        IndexedDBClientStateCheckerFactory::GetClientStateCheckerCallback());
    cache_storage_ = std::make_unique<CacheStorageControlWrapper>(
        GetIOThreadTaskRunner({}), base::FilePath(), nullptr, proxy_,
        BlobRemote());
    // Register the SW quota client before the whole graph is exposed. Its
    // backing is explicitly native memory, even inside a disk BrowserContext.
    AlloyEmbeddedResolution storage_owner;
    storage_owner.backing =
        alloy::site_records::EmbeddedStateRegistry::Backing::kTemporary;
    storage_owner.temporary = std::move(owner);
    service_worker_ = base::MakeRefCounted<ServiceWorkerContextWrapper>(
        partition.browser_context(), std::move(storage_owner));
    if (!service_worker_->InitForEmbeddedMemoryScope(partition, proxy_.get(),
                                                     blob_.get())) {
      service_worker_.reset();
    }
    buckets_ = std::make_unique<BucketManager>(&partition, proxy_);
  }
  ~BucketStorage() override { Shutdown(); }
  void Shutdown() override {
    if (service_worker_) {
      service_worker_->Shutdown();
      service_worker_.reset();
    }
    // Bucket/lock receivers close on their native UI sequence before releasing
    // the native clients and the IO-owned memory catalogue.
    buckets_.reset();
    indexed_db_.reset();
    cache_storage_.reset();
    if (file_system_access_) {
      file_system_access_->ShutdownStorageScope();
    }
    file_system_access_.reset();
    file_system_.reset();
    blob_.reset();
    if (quota_context_) {
      quota_context_->Shutdown();
    }
    quota_context_.reset();
    proxy_.reset();
  }
  BucketManager* buckets() override {
    return Admitted(StorePurpose::kQuota) ? buckets_.get() : nullptr;
  }
  QuotaContext* quota_context() override {
    return Admitted(StorePurpose::kQuota) ? quota_context_.get() : nullptr;
  }
  storage::QuotaManagerProxy* quota_proxy() override {
    return Admitted(StorePurpose::kQuota) ? proxy_.get() : nullptr;
  }
  indexed_db::IndexedDBControlWrapper* indexed_db() override {
    return Admitted(StorePurpose::kIndexedDb) ? indexed_db_.get() : nullptr;
  }
  CacheStorageControlWrapper* cache_storage() override {
    return Admitted(StorePurpose::kCacheStorage) ? cache_storage_.get()
                                                 : nullptr;
  }
  FileSystemAccessManagerImpl* file_system_access() override {
    return Admitted(StorePurpose::kFileSystem) ? file_system_access_.get()
                                               : nullptr;
  }
  ServiceWorkerContextWrapper* service_worker() override {
    return Admitted(StorePurpose::kServiceWorkers) ? service_worker_.get()
                                                   : nullptr;
  }

 private:
  bool Admitted(StorePurpose purpose) const {
    return capabilities_.at(purpose).Allows(purpose,
                                            StorageAccess::kWriteMemory);
  }
  mojo::PendingRemote<storage::mojom::BlobStorageContext> BlobRemote() {
    mojo::PendingRemote<storage::mojom::BlobStorageContext> result;
    GetIOThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&ChromeBlobStorageContext::BindMojoContext, blob_,
                       result.InitWithNewPipeAndPassReceiver()));
    return result;
  }
  const Capabilities capabilities_;
  scoped_refptr<QuotaContext> quota_context_;
  scoped_refptr<storage::QuotaManagerProxy> proxy_;
  scoped_refptr<ChromeBlobStorageContext> blob_;
  scoped_refptr<storage::FileSystemContext> file_system_;
  scoped_refptr<FileSystemAccessManagerImpl> file_system_access_;
  std::unique_ptr<indexed_db::IndexedDBControlWrapper> indexed_db_;
  std::unique_ptr<CacheStorageControlWrapper> cache_storage_;
  scoped_refptr<ServiceWorkerContextWrapper> service_worker_;
  std::unique_ptr<BucketManager> buckets_;
};

const char kBucketStorageKey = 0;

std::optional<storage::BucketLocator> TemporaryBucket(
    const storage::BucketLocator& bucket,
    const blink::StorageKey& native_key) {
  if (bucket.storage_key.origin() != native_key.origin()) {
    return std::nullopt;
  }
  if (bucket.is_default) {
    // Storage-access grants may ask for an unpartitioned default key. They
    // never change the native temporary backing or escape its namespace.
    return storage::BucketLocator::ForDefaultBucket(native_key);
  }
  if (bucket.storage_key != native_key) {
    return std::nullopt;
  }
  return bucket;
}
}  // namespace

AlloyEmbeddedBucketStorage* GetAlloyEmbeddedBucketStorage(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner) {
  if (!owner) {
    return nullptr;
  }
  Capabilities capabilities;
  for (auto purpose : kPurposes) {
    auto capability = owner->Capability(purpose);
    if (!capability.Allows(purpose, StorageAccess::kWriteMemory)) {
      return nullptr;
    }
    capabilities.emplace(purpose, std::move(capability));
  }
  // Native constructors may reenter and retire the scope. Construct the whole
  // partition-bound graph inside the reserved factory slot, before publication;
  // a published owner must never lazily construct children across callbacks.
  return static_cast<AlloyEmbeddedBucketStorage*>(owner->GetOrCreateService(
      &kBucketStorageKey, StorePurpose::kQuota,
      base::BindOnce(
          [](StoragePartitionImpl* partition, Capabilities capabilities,
             scoped_refptr<alloy::site_records::EmbeddedState> owner,
             RecordCapability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<BucketStorage>(
                *partition, std::move(capabilities), std::move(owner));
          },
          &partition, std::move(capabilities), owner),
      &partition));
}

scoped_refptr<ServiceWorkerContextWrapper> GetAlloyServiceWorkerContextForOwner(
    StoragePartitionImpl& partition,
    AlloyEmbeddedResolution owner) {
  using Backing = alloy::site_records::EmbeddedStateRegistry::Backing;
  if (owner.backing == Backing::kRealSite) {
    return partition.GetServiceWorkerContext();
  }
  if (owner.backing == Backing::kTemporary) {
    auto* graph = GetAlloyEmbeddedBucketStorage(partition, owner.temporary);
    return graph ? graph->service_worker() : nullptr;
  }
  return nullptr;
}

void BindAlloyBucketQuotaManagerHost(
    StoragePartitionImpl& partition,
    BucketContext& context,
    mojo::PendingReceiver<blink::mojom::QuotaManagerHost> receiver) {
  const auto selected = context.GetBucketStorageOwner();
  using Backing = alloy::site_records::EmbeddedStateRegistry::Backing;
  if (selected.backing == Backing::kRealSite) {
    partition.GetQuotaContext()->BindQuotaManagerHost(
        context.GetBucketStorageKey(), std::move(receiver));
  } else if (selected.backing == Backing::kTemporary) {
    auto* storage =
        GetAlloyEmbeddedBucketStorage(partition, selected.temporary);
    if (storage && storage->quota_context()) {
      storage->quota_context()->BindQuotaManagerHost(
          context.GetBucketStorageKey(), std::move(receiver));
    }
  }
}

void BindAlloyBucketIndexedDB(
    StoragePartitionImpl& partition,
    BucketContext& context,
    const storage::BucketLocator& bucket,
    mojo::PendingReceiver<blink::mojom::IDBFactory> receiver) {
  const auto owner = context.GetBucketStorageOwner();
  using Backing = alloy::site_records::EmbeddedStateRegistry::Backing;
  if (owner.backing == Backing::kRealSite) {
    partition.BindIndexedDB(bucket, context.GetBucketClientInfo(),
                            std::move(receiver));
  } else if (owner.backing == Backing::kTemporary) {
    const auto selected =
        TemporaryBucket(bucket, context.GetBucketStorageKey());
    if (!selected) {
      return;
    }
    auto* store = GetAlloyEmbeddedBucketStorage(partition, owner.temporary);
    if (store && store->indexed_db()) {
      store->indexed_db()->BindIndexedDB(
          *selected, context.GetBucketClientInfo(), std::move(receiver));
    }
  }
}

void BindAlloyBucketCacheStorage(
    RenderProcessHost& process,
    AlloyEmbeddedResolution owner,
    const blink::StorageKey& native_key,
    const network::CrossOriginEmbedderPolicy& coep,
    mojo::PendingRemote<network::mojom::CrossOriginEmbedderPolicyReporter>
        coep_reporter,
    const network::DocumentIsolationPolicy& dip,
    mojo::PendingRemote<network::mojom::DocumentIsolationPolicyReporter>
        dip_reporter,
    const storage::BucketLocator& bucket,
    mojo::PendingReceiver<blink::mojom::CacheStorage> receiver) {
  using Backing = alloy::site_records::EmbeddedStateRegistry::Backing;
  if (owner.backing == Backing::kRealSite) {
    process.BindCacheStorage(coep, std::move(coep_reporter), dip,
                             std::move(dip_reporter), bucket,
                             std::move(receiver));
  } else if (owner.backing == Backing::kTemporary) {
    const auto selected = TemporaryBucket(bucket, native_key);
    if (!selected) {
      return;
    }
    auto& partition =
        *static_cast<StoragePartitionImpl*>(process.GetStoragePartition());
    auto* store = GetAlloyEmbeddedBucketStorage(partition, owner.temporary);
    if (store && store->cache_storage()) {
      store->cache_storage()->AddReceiver(
          coep, std::move(coep_reporter), dip, std::move(dip_reporter),
          *selected, storage::mojom::CacheStorageOwner::kCacheAPI,
          std::move(receiver));
    }
  }
}

FileSystemAccessEntryFactory::BindingContext WithAlloyBucketFileSystem(
    StoragePartitionImpl& partition,
    BucketContext& context,
    FileSystemAccessEntryFactory::BindingContext binding) {
  auto owner = context.GetBucketStorageOwner();
  using Backing = alloy::site_records::EmbeddedStateRegistry::Backing;
  if (owner.backing == Backing::kRealSite) {
    return binding;
  }
  if (owner.temporary) {
    binding.storage_admission = base::BindRepeating(
        [](RecordCapability capability) {
          return capability.Allows(StorePurpose::kFileSystem,
                                   StorageAccess::kWriteMemory);
        },
        owner.temporary->Capability(StorePurpose::kFileSystem));
  }
  const auto native_key = context.GetBucketStorageKey();
  binding.sandboxed_delegate = base::BindRepeating(
      [](base::WeakPtr<StoragePartitionImpl> partition,
         AlloyEmbeddedResolution owner, blink::StorageKey native_key,
         const FileSystemAccessEntryFactory::BindingContext& binding,
         const std::optional<storage::BucketLocator>& bucket,
         const std::vector<std::string>& path,
         FileSystemAccessEntryFactory::BindingContext::SandboxedCallback
             callback) {
        auto denied = [&] {
          std::move(callback).Run(file_system_access_error::FromFileError(
                                      base::File::FILE_ERROR_SECURITY),
                                  mojo::NullRemote());
        };
        if (!partition || !owner.temporary ||
            !owner.temporary->Capability(StorePurpose::kFileSystem)
                 .Allows(StorePurpose::kFileSystem,
                         StorageAccess::kWriteMemory) ||
            binding.storage_key.origin() != native_key.origin()) {
          denied();
          return;
        }
        auto selected = bucket;
        if (bucket) {
          selected = TemporaryBucket(*bucket, native_key);
          if (!selected) {
            denied();
            return;
          }
        }
        auto* store =
            GetAlloyEmbeddedBucketStorage(*partition, owner.temporary);
        if (!store || !store->file_system_access()) {
          denied();
          return;
        }
        auto native_binding = binding;
        native_binding.storage_key = native_key;
        store->file_system_access()->GetSandboxedFileSystem(
            native_binding, selected, path, std::move(callback));
      },
      partition.GetWeakPtr(), std::move(owner), native_key);
  return binding;
}

void BindAlloyWorkerFileSystemAccess(
    RenderProcessHost& process,
    BucketContext& context,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessManager> receiver) {
  auto& partition =
      *static_cast<StoragePartitionImpl*>(process.GetStoragePartition());
  const auto key = context.GetBucketStorageKey();
  auto binding = WithAlloyBucketFileSystem(
      partition, context,
      FileSystemAccessEntryFactory::BindingContext(key, key.origin().GetURL(),
                                                   process.GetDeprecatedID()));
  partition.GetFileSystemAccessManager()->BindReceiver(binding,
                                                       std::move(receiver));
}

void GetAlloyWorkerBucketDirectory(
    RenderProcessHost& process,
    BucketContext& context,
    const storage::BucketLocator& bucket,
    const std::vector<std::string>& path,
    blink::mojom::FileSystemAccessManager::GetSandboxedFileSystemCallback
        callback) {
  auto& partition =
      *static_cast<StoragePartitionImpl*>(process.GetStoragePartition());
  auto binding = WithAlloyBucketFileSystem(
      partition, context,
      FileSystemAccessEntryFactory::BindingContext(
          bucket.storage_key, bucket.storage_key.origin().GetURL(),
          process.GetDeprecatedID()));
  partition.GetFileSystemAccessManager()->GetSandboxedFileSystem(
      binding, bucket, path, std::move(callback));
}
}  // namespace content
