// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/dom_storage/alloy_embedded_local_storage.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "base/barrier_callback.h"
#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "brave/alloy/site_records/embedded_state.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/storage_service.mojom.h"
#include "content/browser/storage_partition_impl.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"

namespace content {
namespace {
using alloy::site_records::RecordCapability;
using alloy::site_records::StorageAccess;
using alloy::site_records::StorePurpose;
using Source = blink::mojom::StorageAreaSourcePtr;
using Bytes = std::vector<uint8_t>;
using Observer = blink::mojom::StorageAreaObserver;

// A native interface-control round trip acknowledges dispatch of all earlier
// messages on that endpoint. Unlike pausing the renderer's callbacks, pausing
// our Receiver until this reply also holds its incoming pipe-control flushes.
void AfterDispatch(mojo::Remote<blink::mojom::SessionStorageNamespace>& ns,
                   base::OnceClosure callback) {
  ns.QueryVersion(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(
          [](base::OnceClosure done, uint32_t) { std::move(done).Run(); },
          std::move(callback)),
      0u));
}

bool Admitted(const RecordCapability& capability, StorageAccess access) {
  return capability.Allows(StorePurpose::kDomStorage, access);
}

// Both replies and observer events cross the browser-owned generation gate.
// Closing the utility control pipe alone would not order another pipe's queued
// mutations/events against scope retirement.
class ObserverBroker final : public Observer {
 public:
  ObserverBroker(RecordCapability capability,
                 mojo::PendingRemote<Observer> client)
      : capability_(std::move(capability)), client_(std::move(client)) {}
  void SetDisconnectHandler(base::OnceClosure handler) {
    client_.set_disconnect_handler(std::move(handler));
  }
  void KeyChanged(const Bytes& key,
                  const Bytes& value,
                  const std::optional<Bytes>& old_value,
                  Source source) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      client_->KeyChanged(key, value, old_value, std::move(source));
    }
  }
  void KeyChangeFailed(const Bytes& key, Source source) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      client_->KeyChangeFailed(key, std::move(source));
    }
  }
  void KeyDeleted(const Bytes& key,
                  const std::optional<Bytes>& old_value,
                  Source source) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      client_->KeyDeleted(key, old_value, std::move(source));
    }
  }
  void AllDeleted(bool was_nonempty, Source source) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      client_->AllDeleted(was_nonempty, std::move(source));
    }
  }
  void ShouldSendOldValueOnMutations(bool value) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      client_->ShouldSendOldValueOnMutations(value);
    }
  }

 private:
  const RecordCapability capability_;
  mojo::Remote<Observer> client_;
};

class AreaBroker final : public blink::mojom::StorageArea {
 public:
  AreaBroker(RecordCapability capability,
             mojo::PendingRemote<blink::mojom::StorageArea> backend)
      : capability_(std::move(capability)), backend_(std::move(backend)) {}
  void SetDisconnectHandler(base::OnceClosure handler) {
    backend_.set_disconnect_handler(std::move(handler));
  }
  base::WeakPtr<AreaBroker> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void BeforeClone(mojo::Remote<blink::mojom::SessionStorageNamespace>& ns) {
    ns.PauseReceiverUntilFlushCompletes(backend_.FlushAsync());
  }
  void AfterClone(mojo::Remote<blink::mojom::SessionStorageNamespace>& ns) {
    backend_.PauseReceiverUntilFlushCompletes(ns.FlushAsync());
  }

  void AddObserver(mojo::PendingRemote<Observer> observer) override {
    if (Admitted(capability_, StorageAccess::kReadMemory)) {
      backend_->AddObserver(Observe(std::move(observer)));
    }
  }
  void Put(const Bytes& key,
           const Bytes& value,
           const std::optional<Bytes>& old_value,
           Source source,
           PutCallback callback) override {
    if (!Admitted(capability_, StorageAccess::kWriteMemory)) {
      std::move(callback).Run(false);
      return;
    }
    backend_->Put(
        key, value, old_value, std::move(source),
        base::BindOnce(&AreaBroker::PutComplete, weak_factory_.GetWeakPtr(),
                       std::move(callback)));
  }
  void Delete(const Bytes& key,
              const std::optional<Bytes>& old_value,
              Source source,
              DeleteCallback callback) override {
    if (!Admitted(capability_, StorageAccess::kWriteMemory)) {
      std::move(callback).Run();
      return;
    }
    backend_->Delete(
        key, old_value, std::move(source),
        base::BindOnce(&AreaBroker::DeleteComplete, weak_factory_.GetWeakPtr(),
                       std::move(callback)));
  }
  void DeleteAll(Source source,
                 mojo::PendingRemote<Observer> observer,
                 DeleteAllCallback callback) override {
    if (!Admitted(capability_, StorageAccess::kWriteMemory)) {
      std::move(callback).Run();
      return;
    }
    backend_->DeleteAll(
        std::move(source), Observe(std::move(observer)),
        base::BindOnce(&AreaBroker::DeleteAllComplete,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
  }
  void GetAll(mojo::PendingRemote<Observer> observer,
              GetAllCallback callback) override {
    if (!Admitted(capability_, StorageAccess::kReadMemory)) {
      std::move(callback).Run({});
      return;
    }
    backend_->GetAll(
        Observe(std::move(observer)),
        base::BindOnce(&AreaBroker::GetAllComplete, weak_factory_.GetWeakPtr(),
                       std::move(callback)));
  }

 private:
  mojo::PendingRemote<Observer> Observe(mojo::PendingRemote<Observer> client) {
    mojo::PendingRemote<Observer> result;
    if (client) {
      auto observer =
          std::make_unique<ObserverBroker>(capability_, std::move(client));
      auto* implementation = observer.get();
      const auto id = observers_.Add(std::move(observer),
                                     result.InitWithNewPipeAndPassReceiver());
      implementation->SetDisconnectHandler(base::BindOnce(
          &AreaBroker::RemoveObserver, weak_factory_.GetWeakPtr(), id));
    }
    return result;
  }
  void RemoveObserver(mojo::ReceiverId id) { observers_.Remove(id); }
  void PutComplete(PutCallback callback, bool success) {
    std::move(callback).Run(success &&
                            Admitted(capability_, StorageAccess::kWriteMemory));
  }
  void DeleteComplete(DeleteCallback callback) { std::move(callback).Run(); }
  void DeleteAllComplete(DeleteAllCallback callback) {
    std::move(callback).Run();
  }
  void GetAllComplete(GetAllCallback callback,
                      std::vector<blink::mojom::KeyValuePtr> values) {
    if (!Admitted(capability_, StorageAccess::kReadMemory)) {
      values.clear();
    }
    std::move(callback).Run(std::move(values));
  }
  const RecordCapability capability_;
  mojo::Remote<blink::mojom::StorageArea> backend_;
  mojo::UniqueReceiverSet<Observer> observers_;
  base::WeakPtrFactory<AreaBroker> weak_factory_{this};
};

class LocalStorageOwner final : public KeyedService {
 public:
  explicit LocalStorageOwner(RecordCapability capability)
      : capability_(std::move(capability)) {}
  void Shutdown() override {
    // Synchronous browser-side disconnection fences every old client and
    // observer before the utility memory backend is released.
    weak_factory_.InvalidateWeakPtrs();
    areas_.Clear();
    controls_.clear();
  }
  void Bind(StoragePartitionImpl& partition,
            const blink::StorageKey& storage_key,
            mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
    if (!Admitted(capability_, StorageAccess::kWriteMemory)) {
      return;
    }
    auto [entry, created] = controls_.try_emplace(&partition);
    auto& control = entry->second;
    if (created) {
      // Native documented memory backing. No profile pathname, even if the
      // containing request context and its first-party stores use disk.
      partition.GetStorageService()->BindLocalStorageControl(
          std::nullopt, control.BindNewPipeAndPassReceiver());
    }
    // A crashed/disconnected old backend never falls through to real data or
    // silently opens a replacement under a retired generation.
    if (!control.is_connected()) {
      return;
    }
    mojo::PendingRemote<blink::mojom::StorageArea> backend;
    control->BindStorageArea(storage_key,
                             backend.InitWithNewPipeAndPassReceiver());
    auto area = std::make_unique<AreaBroker>(capability_, std::move(backend));
    auto* implementation = area.get();
    const auto id = areas_.Add(std::move(area), std::move(receiver));
    implementation->SetDisconnectHandler(base::BindOnce(
        &LocalStorageOwner::RemoveArea, weak_factory_.GetWeakPtr(), id));
  }

 private:
  void RemoveArea(mojo::ReceiverId id) { areas_.Remove(id); }
  const RecordCapability capability_;
  std::map<StoragePartitionImpl*,
           mojo::Remote<storage::mojom::LocalStorageControl>>
      controls_;
  mojo::UniqueReceiverSet<blink::mojom::StorageArea> areas_;
  base::WeakPtrFactory<LocalStorageOwner> weak_factory_{this};
};

const char kLocalStorageOwnerKey = 0;

// A native SessionStorageControl and all of its area/observer/namespace pipes
// are children of the temporary capability. The partition facade keeps only
// weak references; it cannot extend a scope's lifetime.
class SessionShard final {
 public:
  SessionShard(StoragePartitionImpl& partition, RecordCapability capability)
      : capability_(std::move(capability)) {
    partition.GetStorageService()->BindSessionStorageControl(
        std::nullopt, false, control_.BindNewPipeAndPassReceiver());
    control_.set_disconnect_handler(
        base::BindOnce(&SessionShard::Shutdown, weak_factory_.GetWeakPtr()));
  }
  ~SessionShard() { Shutdown(); }
  void Shutdown() {
    weak_factory_.InvalidateWeakPtrs();
    areas_.Clear();
    area_namespaces_.clear();
    namespaces_.clear();
    control_.reset();
  }
  base::WeakPtr<SessionShard> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  storage::mojom::SessionStorageControl* control() {
    return control_ && control_.is_connected() &&
                   Admitted(capability_, StorageAccess::kWriteMemory)
               ? control_.get()
               : nullptr;
  }
  void Bind(const blink::StorageKey& key,
            const std::string& ns,
            mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
    if (!control()) {
      return;
    }
    mojo::PendingRemote<blink::mojom::StorageArea> backend;
    control()->BindStorageArea(key, ns,
                               backend.InitWithNewPipeAndPassReceiver());
    auto area = std::make_unique<AreaBroker>(capability_, std::move(backend));
    auto* implementation = area.get();
    const auto id = areas_.Add(std::move(area), std::move(receiver));
    std::erase_if(area_namespaces_,
                  [](const auto& entry) { return !entry.second.second; });
    area_namespaces_.emplace(id,
                             std::make_pair(ns, implementation->GetWeakPtr()));
    implementation->SetDisconnectHandler(base::BindOnce(
        &SessionShard::RemoveArea, weak_factory_.GetWeakPtr(), id));
  }
  void DeleteNamespace(const std::string& ns) {
    for (auto it = area_namespaces_.begin(); it != area_namespaces_.end();) {
      if (it->second.first == ns) {
        areas_.Remove(it->first);
        it = area_namespaces_.erase(it);
      } else {
        ++it;
      }
    }
    namespaces_.erase(ns);
    if (control()) {
      control()->DeleteNamespace(ns, false);
    }
  }
  void Clone(const std::string& source,
             const std::string& target,
             base::OnceClosure dispatched) {
    if (!control()) {
      std::move(dispatched).Run();
      return;
    }
    auto& ns = namespaces_[source];
    if (!ns) {
      control()->BindNamespace(source, ns.BindNewPipeAndPassReceiver());
    }
    for (const auto& [id, entry] : area_namespaces_) {
      if (entry.first == source && entry.second) {
        entry.second->BeforeClone(ns);
      }
    }
    ns->Clone(target);
    for (const auto& [id, entry] : area_namespaces_) {
      if (entry.first == source && entry.second) {
        entry.second->AfterClone(ns);
      }
    }
    AfterDispatch(ns, std::move(dispatched));
  }

 private:
  void RemoveArea(mojo::ReceiverId id) {
    area_namespaces_.erase(id);
    areas_.Remove(id);
  }
  const RecordCapability capability_;
  mojo::Remote<storage::mojom::SessionStorageControl> control_;
  mojo::UniqueReceiverSet<blink::mojom::StorageArea> areas_;
  std::map<mojo::ReceiverId, std::pair<std::string, base::WeakPtr<AreaBroker>>>
      area_namespaces_;
  std::map<std::string, mojo::Remote<blink::mojom::SessionStorageNamespace>>
      namespaces_;
  base::WeakPtrFactory<SessionShard> weak_factory_{this};
};

class SessionStorageOwner final : public KeyedService {
 public:
  explicit SessionStorageOwner(RecordCapability capability)
      : capability_(std::move(capability)) {}
  void Shutdown() override { shards_.clear(); }
  SessionShard* GetShard(StoragePartitionImpl& partition, bool* created) {
    if (!Admitted(capability_, StorageAccess::kWriteMemory)) {
      return nullptr;
    }
    auto [entry, inserted] = shards_.try_emplace(&partition);
    *created = inserted;
    if (inserted) {
      entry->second = std::make_unique<SessionShard>(partition, capability_);
    }
    return entry->second.get();
  }

 private:
  const RecordCapability capability_;
  std::map<StoragePartitionImpl*, std::unique_ptr<SessionShard>> shards_;
};

const char kSessionStorageOwnerKey = 0;

class SessionRouter final : public AlloySessionStorageRouter {
 public:
  SessionRouter(StoragePartitionImpl& partition,
                storage::mojom::SessionStorageControl& real)
      : partition_(partition), real_(real) {}
  ~SessionRouter() override {
    for (const auto& shard : shards_) {
      if (shard) {
        shard->Shutdown();
      }
    }
  }
  storage::mojom::SessionStorageControl& GetRealControlForTesting() override {
    return *real_;
  }

  void BindTemporary(
      scoped_refptr<alloy::site_records::EmbeddedState> owner,
      const blink::StorageKey& key,
      const std::string& ns,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver) override {
    if (!owner || !live_namespaces_.contains(ns)) {
      return;
    }
    auto* service = static_cast<SessionStorageOwner*>(owner->GetOrCreateService(
        &kSessionStorageOwnerKey, StorePurpose::kDomStorage,
        base::BindOnce(
            [](RecordCapability capability) -> std::unique_ptr<KeyedService> {
              return std::make_unique<SessionStorageOwner>(
                  std::move(capability));
            })));
    bool created = false;
    auto* shard = service ? service->GetShard(*partition_, &created) : nullptr;
    if (!shard || !shard->control()) {
      return;
    }
    if (created) {
      std::erase_if(shards_, [](const auto& entry) { return !entry; });
      shards_.push_back(shard->GetWeakPtr());
      // No bytes predate admission, but native pending-clone dependencies do:
      // a target opened before its renderer Clone must still wait for that
      // Clone, including any intervening writes to the newly admitted source.
      std::set<std::string> seeded;
      for (const auto& name : live_namespaces_) {
        SeedNamespace(name, *shard->control(), seeded);
      }
    }
    shard->Bind(key, ns, std::move(receiver));
  }

  void BindNamespace(
      const std::string& ns,
      mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver)
      override {
    const auto id = next_broker_++;
    auto broker =
        std::make_unique<NamespaceBroker>(*this, ns, std::move(receiver));
    broker->SetDisconnectHandler(base::BindOnce(
        &SessionRouter::RemoveBroker, weak_factory_.GetWeakPtr(), id));
    brokers_.emplace(id, std::move(broker));
  }
  void BindStorageArea(
      const blink::StorageKey& key,
      const std::string& ns,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver) override {
    // This privileged native entry retains its original contract. The wrapper
    // has already selected temporary frame binds via BindTemporary, including
    // the trusted ForProcess path; nonce alone is not a backing-store policy.
    real_->BindStorageArea(key, ns, std::move(receiver));
  }
  void GetUsage(GetUsageCallback callback) override {
    auto controls = Controls();
    auto barrier = base::BarrierCallback<Usage>(
        controls.size(),
        base::BindOnce(
            [](GetUsageCallback done, std::vector<Usage> batches) {
              Usage result;
              for (auto& batch : batches) {
                for (auto& entry : batch) {
                  result.push_back(std::move(entry));
                }
              }
              std::move(done).Run(std::move(result));
            },
            std::move(callback)));
    real_->GetUsage(barrier);
    for (const auto& shard : shards_) {
      if (shard && shard->control()) {
        shard->control()->GetUsage(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<SessionShard> owner,
                   base::RepeatingCallback<void(Usage)> done, Usage values) {
                  if (!owner || !owner->control()) {
                    values.clear();
                  }
                  done.Run(std::move(values));
                },
                shard, barrier),
            Usage{}));
      }
    }
  }
  void DeleteStorage(const blink::StorageKey& key,
                     const std::string& ns,
                     DeleteStorageCallback callback) override {
    auto controls = Controls();
    auto barrier = base::BarrierClosure(controls.size(), std::move(callback));
    for (auto* control : controls) {
      control->DeleteStorage(key, ns,
                             mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                                 base::OnceClosure(barrier)));
    }
  }
  void CleanUpStorage(CleanUpStorageCallback callback) override {
    auto controls = Controls();
    auto barrier = base::BarrierClosure(controls.size(), std::move(callback));
    for (auto* control : controls) {
      control->CleanUpStorage(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::OnceClosure(barrier)));
    }
  }
  void ScavengeUnusedNamespaces() override {
    for (auto* control : Controls()) {
      control->ScavengeUnusedNamespaces();
    }
  }
  void Flush() override {
    for (auto* control : Controls()) {
      control->Flush();
    }
  }
  void PurgeMemory() override {
    for (auto* control : Controls()) {
      control->PurgeMemory();
    }
  }
  void CreateNamespace(const std::string& ns) override {
    live_namespaces_.insert(ns);
    for (auto* control : Controls()) {
      control->CreateNamespace(ns);
    }
  }
  void CloneNamespace(const std::string& source,
                      const std::string& target,
                      storage::mojom::SessionStorageCloneType type) override {
    if (!live_namespaces_.contains(target)) {
      if (type == storage::mojom::SessionStorageCloneType::
                      kWaitForCloneOnNamespace &&
          live_namespaces_.contains(source) &&
          !renderer_cloned_.contains(target)) {
        pending_clones_.emplace(target, source);
      }
      live_namespaces_.insert(target);
    }
    for (auto* control : Controls()) {
      control->CloneNamespace(source, target, type);
    }
  }
  void DeleteNamespace(const std::string& ns, bool persist) override {
    if (auto pending = pending_clones_.find(ns);
        pending != pending_clones_.end()) {
      abandoned_clones_.emplace(ns, pending->second);
    }
    // Native DeleteNamespace materializes pending children before removing
    // their source. A later-admitted empty shard has no historical bytes to
    // inherit and must not wait on a renderer pipe which no longer exists.
    std::erase_if(pending_clones_, [&](const auto& entry) {
      return entry.first == ns || entry.second == ns;
    });
    std::erase_if(abandoned_clones_,
                  [&](const auto& entry) { return entry.second == ns; });
    renderer_cloned_.erase(ns);
    live_namespaces_.erase(ns);
    std::erase_if(brokers_, [&](const auto& entry) {
      return entry.second->name() == ns;
    });
    real_->DeleteNamespace(ns, persist);
    for (const auto& shard : shards_) {
      if (shard) {
        shard->DeleteNamespace(ns);
      }
    }
  }

 private:
  using Usage = std::vector<storage::mojom::SessionStorageUsageInfoPtr>;
  class NamespaceBroker final : public blink::mojom::SessionStorageNamespace {
   public:
    NamespaceBroker(
        SessionRouter& router,
        std::string ns,
        mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver)
        : router_(router),
          name_(std::move(ns)),
          receiver_(this, std::move(receiver)) {
      router_->real_->BindNamespace(name_, real_.BindNewPipeAndPassReceiver());
    }
    const std::string& name() const { return name_; }
    void SetDisconnectHandler(base::OnceClosure handler) {
      auto handlers = base::SplitOnceCallback(std::move(handler));
      receiver_.set_disconnect_handler(std::move(handlers.first));
      real_.set_disconnect_handler(std::move(handlers.second));
    }
    void Clone(const std::string& target) override {
      router_->RendererCloned(name_, target);
      receiver_.Pause();
      std::vector<base::WeakPtr<SessionShard>> shards;
      for (const auto& shard : router_->shards_) {
        if (shard && shard->control()) {
          shards.push_back(shard);
        }
      }
      auto dispatched = base::BarrierClosure(
          shards.size() + 1,
          base::BindOnce(&NamespaceBroker::Resume, weak_factory_.GetWeakPtr()));
      real_->Clone(target);
      AfterDispatch(real_, dispatched);
      for (const auto& shard : shards) {
        shard->Clone(name_, target, dispatched);
      }
    }

   private:
    void Resume() { receiver_.Resume(); }
    const raw_ref<SessionRouter> router_;
    const std::string name_;
    mojo::Remote<blink::mojom::SessionStorageNamespace> real_;
    mojo::Receiver<blink::mojom::SessionStorageNamespace> receiver_;
    base::WeakPtrFactory<NamespaceBroker> weak_factory_{this};
  };
  std::vector<storage::mojom::SessionStorageControl*> Controls() {
    std::vector<storage::mojom::SessionStorageControl*> result{&*real_};
    for (const auto& shard : shards_) {
      if (shard && shard->control()) {
        result.push_back(shard->control());
      }
    }
    return result;
  }
  void SeedNamespace(const std::string& ns,
                     storage::mojom::SessionStorageControl& control,
                     std::set<std::string>& seeded) {
    if (!seeded.insert(ns).second) {
      return;
    }
    if (auto pending = pending_clones_.find(ns);
        pending != pending_clones_.end()) {
      SeedNamespace(pending->second, control, seeded);
      control.CloneNamespace(
          pending->second, ns,
          storage::mojom::SessionStorageCloneType::kWaitForCloneOnNamespace);
    } else {
      control.CreateNamespace(ns);
    }
  }
  void RendererCloned(const std::string& source, const std::string& target) {
    if (auto abandoned = abandoned_clones_.find(target);
        abandoned != abandoned_clones_.end() && abandoned->second == source) {
      abandoned_clones_.erase(abandoned);
      return;
    }
    pending_clones_.erase(target);
    live_namespaces_.insert(target);
    renderer_cloned_.insert(target);
  }
  void RemoveBroker(uint64_t id) { brokers_.erase(id); }
  const raw_ref<StoragePartitionImpl> partition_;
  const raw_ref<storage::mojom::SessionStorageControl> real_;
  std::set<std::string> live_namespaces_;
  std::map<std::string, std::string> pending_clones_;
  std::map<std::string, std::string> abandoned_clones_;
  std::set<std::string> renderer_cloned_;
  std::vector<base::WeakPtr<SessionShard>> shards_;
  uint64_t next_broker_ = 0;
  std::map<uint64_t, std::unique_ptr<NamespaceBroker>> brokers_;
  base::WeakPtrFactory<SessionRouter> weak_factory_{this};
};
}  // namespace

void BindAlloyEmbeddedLocalStorage(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner,
    const blink::StorageKey& storage_key,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  if (!owner) {
    return;
  }
  auto* service = static_cast<LocalStorageOwner*>(owner->GetOrCreateService(
      &kLocalStorageOwnerKey, StorePurpose::kDomStorage,
      base::BindOnce(
          [](RecordCapability capability) -> std::unique_ptr<KeyedService> {
            return std::make_unique<LocalStorageOwner>(std::move(capability));
          })));
  if (service) {
    service->Bind(partition, storage_key, std::move(receiver));
  }
}

std::unique_ptr<AlloySessionStorageRouter> CreateAlloySessionStorageRouter(
    StoragePartitionImpl& partition,
    storage::mojom::SessionStorageControl& real_control) {
  return std::make_unique<SessionRouter>(partition, real_control);
}

}  // namespace content
