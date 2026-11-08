// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_BUCKETS_ALLOY_EMBEDDED_BUCKET_STORAGE_H_
#define CONTENT_BROWSER_BUCKETS_ALLOY_EMBEDDED_BUCKET_STORAGE_H_

#include "base/memory/scoped_refptr.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/browser/alloy_embedded_state.h"
#include "content/public/browser/file_system_access_entry_factory.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-forward.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-forward.h"
#include "third_party/blink/public/mojom/quota/quota_manager_host.mojom-forward.h"

namespace storage {
class QuotaManagerProxy;
struct BucketLocator;
}  // namespace storage
namespace blink {
class StorageKey;
}
namespace network {
struct CrossOriginEmbedderPolicy;
struct DocumentIsolationPolicy;
namespace mojom {
class CrossOriginEmbedderPolicyReporter;
class DocumentIsolationPolicyReporter;
}  // namespace mojom
}  // namespace network
namespace content {
class BucketContext;
class BucketManager;
class CacheStorageControlWrapper;
class FileSystemAccessManagerImpl;
class QuotaContext;
class ServiceWorkerContextWrapper;
class StoragePartitionImpl;
class RenderProcessHost;
namespace indexed_db {
class IndexedDBControlWrapper;
}

// The quota namespace and its native clients have one scope owner. Construct
// all clients before exposing quota; bucket IDs never cross quota namespaces.
class AlloyEmbeddedBucketStorage : public KeyedService {
 public:
  virtual BucketManager* buckets() = 0;
  virtual QuotaContext* quota_context() = 0;
  virtual storage::QuotaManagerProxy* quota_proxy() = 0;
  virtual indexed_db::IndexedDBControlWrapper* indexed_db() = 0;
  virtual CacheStorageControlWrapper* cache_storage() = 0;
  virtual FileSystemAccessManagerImpl* file_system_access() = 0;
  virtual ServiceWorkerContextWrapper* service_worker() = 0;
};

CONTENT_EXPORT AlloyEmbeddedBucketStorage* GetAlloyEmbeddedBucketStorage(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner);
// The caller supplies a browser-owned client resolution, never a renderer key.
CONTENT_EXPORT scoped_refptr<ServiceWorkerContextWrapper>
GetAlloyServiceWorkerContextForOwner(StoragePartitionImpl& partition,
                                     AlloyEmbeddedResolution owner);

CONTENT_EXPORT void BindAlloyBucketQuotaManagerHost(
    StoragePartitionImpl& partition,
    BucketContext& context,
    mojo::PendingReceiver<blink::mojom::QuotaManagerHost> receiver);

CONTENT_EXPORT void BindAlloyBucketIndexedDB(
    StoragePartitionImpl& partition,
    BucketContext& context,
    const storage::BucketLocator& bucket,
    mojo::PendingReceiver<blink::mojom::IDBFactory> receiver);

// Inputs are captured from the native frame/worker, never a renderer-supplied
// nonce. Preserve native origin and COEP/DIP checks inside the selected
// service.
CONTENT_EXPORT void BindAlloyBucketCacheStorage(
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
    mojo::PendingReceiver<blink::mojom::CacheStorage> receiver);

CONTENT_EXPORT FileSystemAccessEntryFactory::BindingContext
WithAlloyBucketFileSystem(StoragePartitionImpl& partition,
                          BucketContext& context,
                          FileSystemAccessEntryFactory::BindingContext binding);

CONTENT_EXPORT void BindAlloyWorkerFileSystemAccess(
    RenderProcessHost& process,
    BucketContext& context,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessManager> receiver);
CONTENT_EXPORT void GetAlloyWorkerBucketDirectory(
    RenderProcessHost& process,
    BucketContext& context,
    const storage::BucketLocator& bucket,
    const std::vector<std::string>& path,
    blink::mojom::FileSystemAccessManager::GetSandboxedFileSystemCallback
        callback);
}  // namespace content

#endif  // CONTENT_BROWSER_BUCKETS_ALLOY_EMBEDDED_BUCKET_STORAGE_H_
