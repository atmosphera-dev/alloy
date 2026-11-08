// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/alloy_network_generation.h"
#include "content/public/browser/alloy_network_retirement.h"

#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "content/browser/alloy_embedded_state.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/document_user_data.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace content {

struct AlloyNetworkGeneration::Endpoints {
  mojo::Remote<network::mojom::NetworkContext> context;
  mojo::Remote<cert_verifier::mojom::CertVerifierServiceUpdater> verifier;
  base::ScopedClosureRunner remove_client;
  base::RepeatingClosure recreate;
};

AlloyNetworkGeneration::AlloyNetworkGeneration(
    std::optional<alloy::site_records::RecordCapability> memory_owner)
    : endpoints_(std::make_shared<Endpoints>()),
      memory_owner_(std::move(memory_owner)) {}

AlloyNetworkGeneration::~AlloyNetworkGeneration() {
  Shutdown();
}

bool AlloyNetworkGeneration::CheckAdmission() {
  if (memory_owner_ &&
      !memory_owner_->Allows(
          alloy::site_records::StorePurpose::kNetworkWorkingState,
          alloy::site_records::StorageAccess::kWriteMemory)) {
    Shutdown();
  }
  return !closed_;
}

void AlloyNetworkGeneration::Bind(
    mojo::PendingRemote<network::mojom::NetworkContext> context,
    mojo::PendingRemote<cert_verifier::mojom::CertVerifierServiceUpdater>
        verifier,
    base::OnceClosure remove_client,
    base::RepeatingClosure recreate) {
  if (!CheckAdmission())
    return;
  endpoints_->recreate.Reset();
  if (endpoints_->context.is_bound())
    endpoints_->context.set_disconnect_handler(base::DoNothing());
  // An older retirement owns its original physical endpoints, never this bind.
  endpoints_ = std::make_shared<Endpoints>();
  endpoints_->context.Bind(std::move(context));
  endpoints_->verifier.Bind(std::move(verifier));
  endpoints_->remove_client.ReplaceClosure(std::move(remove_client));
  endpoints_->recreate = std::move(recreate);
  endpoints_->context.set_disconnect_handler(
      base::BindOnce([](base::RepeatingClosure recreate) { recreate.Run(); },
                     endpoints_->recreate));
}

network::mojom::NetworkContext* AlloyNetworkGeneration::GetNetworkContext() {
  // A late native client sees ordinary Mojo disconnection, never a newly
  // created context or the partition's shared state after its owner retires.
  if (!CheckAdmission())
    return endpoints_->context.get();
  if ((!endpoints_->context.is_bound() ||
       !endpoints_->context.is_connected()) &&
      endpoints_->recreate) {
    endpoints_->recreate.Run();
  }
  CHECK(endpoints_->context.is_bound());
  return endpoints_->context.get();
}

cert_verifier::mojom::CertVerifierServiceUpdater*
AlloyNetworkGeneration::GetVerifier() {
  if (!CheckAdmission())
    return nullptr;
  GetNetworkContext();
  return endpoints_->verifier.is_bound() ? endpoints_->verifier.get() : nullptr;
}

base::WeakPtr<AlloyNetworkGeneration> AlloyNetworkGeneration::GetWeakPtr() {
  return !CheckAdmission() ? base::WeakPtr<AlloyNetworkGeneration>()
                           : weak_factory_.GetWeakPtr();
}

base::OnceClosure AlloyNetworkGeneration::RetainNativeRetirementEndpoints() {
  CHECK(!closed_);
  return base::BindOnce([](std::shared_ptr<Endpoints>) {}, endpoints_);
}

void AlloyNetworkGeneration::Shutdown() {
  if (closed_)
    return;
  closed_ = true;
  weak_factory_.InvalidateWeakPtrs();
  endpoints_->recreate.Reset();
  if (endpoints_->context.is_bound())
    endpoints_->context.set_disconnect_handler(base::DoNothing());
  // Physical old endpoints survive ONLY an already-entered retirement. The
  // closed generation itself retains no live authority and cannot recreate it.
  endpoints_ = std::make_shared<Endpoints>();
  auto disconnected = endpoints_->context.BindNewPipeAndPassReceiver();
  disconnected.reset();
}

namespace {
// The existing scope service graph owns the SAME native generation registered
// weakly by StoragePartition. There is no additional selector or registry.
class EmbeddedNetworkService final : public KeyedService {
 public:
  explicit EmbeddedNetworkService(scoped_refptr<AlloyNetworkGeneration> lease)
      : lease_(std::move(lease)) {}
  ~EmbeddedNetworkService() override { Shutdown(); }
  void Shutdown() override {
    if (lease_)
      lease_->Shutdown();
    lease_.reset();
  }
  scoped_refptr<AlloyNetworkGeneration> lease() const { return lease_; }

 private:
  scoped_refptr<AlloyNetworkGeneration> lease_;
};

// One in-flight operation owns physical prepared endpoints. Context/generation
// discovery remains exclusively StoragePartitionImpl::ForEachNetworkContext.
class NetworkServiceRetirement final
    : public base::RefCounted<NetworkServiceRetirement> {
 public:
  NetworkServiceRetirement(base::OnceCallback<void(base::OnceClosure)> prepared,
                           base::OnceCallback<void(bool)> drained)
      : prepared_(std::move(prepared)), drained_(std::move(drained)) {}
  void Start(BrowserContext* context,
             const net::AlloyNetworkRecordTarget& target) {
    std::vector<network::mojom::NetworkContext*> originals;
    context->ForEachLoadedStoragePartition([&](StoragePartition* partition) {
      static_cast<StoragePartitionImpl*>(partition)->ForEachNetworkContext(
          [&](network::mojom::NetworkContext* native,
              cert_verifier::mojom::CertVerifierServiceUpdater*) {
            originals.push_back(native);
          },
          [&](AlloyNetworkGeneration* generation) {
            original_lifetimes_.push_back(
                generation->RetainNativeRetirementEndpoints());
          });
    });
    endpoints_.resize(originals.size());
    auto all_prepared = base::BarrierClosure(
        originals.size(), base::BindOnce(&NetworkServiceRetirement::Prepared,
                                         base::WrapRefCounted(this)));
    for (size_t i = 0; i < originals.size(); ++i) {
      originals[i]->PrepareAlloyNetworkServiceRetirement(
          target.domain(), endpoints_[i].BindNewPipeAndPassReceiver(),
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(&NetworkServiceRetirement::OnPrepared,
                             base::WrapRefCounted(this), all_prepared),
              false));
    }
  }

 private:
  friend class base::RefCounted<NetworkServiceRetirement>;
  ~NetworkServiceRetirement() = default;
  void OnPrepared(base::OnceClosure finish, bool ok) {
    success_ = success_ && ok;
    std::move(finish).Run();
  }
  void Prepared() {
    std::move(prepared_).Run(net::CommitNetworkRetirementOnDrop(base::BindOnce(
        &NetworkServiceRetirement::Commit, base::WrapRefCounted(this))));
  }
  void Commit() {
    auto all_drained = base::BarrierClosure(
        endpoints_.size(), base::BindOnce(&NetworkServiceRetirement::Finished,
                                          base::WrapRefCounted(this)));
    for (auto& endpoint : endpoints_) {
      endpoint->Commit(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&NetworkServiceRetirement::OnPrepared,
                         base::WrapRefCounted(this), all_drained),
          false));
    }
  }
  void Finished() {
    endpoints_.clear();
    original_lifetimes_.clear();
    std::move(drained_).Run(success_);
  }
  std::vector<mojo::Remote<network::mojom::AlloyNetworkRetirement>> endpoints_;
  std::vector<base::OnceClosure> original_lifetimes_;
  base::OnceCallback<void(base::OnceClosure)> prepared_;
  base::OnceCallback<void(bool)> drained_;
  bool success_ = true;
};

network::mojom::NetworkContext* UnavailableNetworkContext() {
  // Closed document admission must not fall through to the shared partition.
  // This immutable disconnected endpoint has no Network Service/server state.
  static base::NoDestructor<mojo::Remote<network::mojom::NetworkContext>>
      closed;
  if (!closed->is_bound()) {
    auto disconnected = closed->BindNewPipeAndPassReceiver();
    disconnected.reset();
  }
  return closed->get();
}

class AlloyDocumentNetworkData final
    : public DocumentUserData<AlloyDocumentNetworkData> {
 public:
  ~AlloyDocumentNetworkData() override { Retire(); }
  void Retire() {
    if (owns_generation_ && generation)
      generation->Shutdown();
  }
  scoped_refptr<AlloyNetworkGeneration> generation;

 private:
  explicit AlloyDocumentNetworkData(RenderFrameHost* frame)
      : DocumentUserData(frame),
        owns_generation_(!frame->GetParentOrOuterDocument()) {}
  const bool owns_generation_;
  friend class DocumentUserData<AlloyDocumentNetworkData>;
  DOCUMENT_USER_DATA_KEY_DECL();
};
DOCUMENT_USER_DATA_KEY_IMPL(AlloyDocumentNetworkData);
}  // namespace

void PrepareAlloyNetworkServiceRetirement(
    BrowserContext* context,
    const net::AlloyNetworkRecordTarget& target,
    base::OnceCallback<void(base::OnceClosure)> prepared,
    base::OnceCallback<void(bool)> drained) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  CHECK(context);
  CHECK(prepared);
  CHECK(drained);
  auto retirement = base::MakeRefCounted<NetworkServiceRetirement>(
      std::move(prepared), std::move(drained));
  retirement->Start(context, target);
}

scoped_refptr<AlloyNetworkGeneration> GetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame,
    const AlloyEmbeddedResolution* pending_owner) {
  if (!frame || (!pending_owner && !AlloyDocumentStateAdmitted(frame)) ||
      frame->GetBrowserContext()->ShutdownStarted())
    return nullptr;
  const auto selected =
      pending_owner ? *pending_owner : GetAlloyEmbeddedStateForDocument(frame);
  if (selected.temporary) {
    using alloy::site_records::RecordCapability;
    using alloy::site_records::StorePurpose;
    static const char kNetworkService = 0;
    auto* partition =
        static_cast<StoragePartitionImpl*>(frame->GetStoragePartition());
    auto* service = static_cast<EmbeddedNetworkService*>(
        selected.temporary->GetOrCreateService(
            &kNetworkService, StorePurpose::kNetworkWorkingState,
            base::BindOnce(
                [](StoragePartitionImpl* partition, RecordCapability capability)
                    -> std::unique_ptr<KeyedService> {
                  return std::make_unique<EmbeddedNetworkService>(
                      partition->CreateAlloyNetworkGeneration(capability));
                },
                base::Unretained(partition)),
            partition));
    return service ? service->lease() : nullptr;
  }
  // The canonical captured selection, not an unrelated native nonce, tells us
  // whether a temporary principal retired. Never borrow CUT's parent lease.
  if (selected.temporary_admission_closed)
    return nullptr;
  const auto* policy = alloy::site_records::RequestContextPolicy::Get(
      *frame->GetBrowserContext());
  if (!policy ||
      policy->network_state() != alloy::site_records::NetworkState::kCut)
    return nullptr;
  auto* data = AlloyDocumentNetworkData::GetOrCreateForCurrentDocument(frame);
  if (!data->generation) {
    if (auto* parent = frame->GetParentOrOuterDocument()) {
      data->generation = GetAlloyNetworkGenerationForDocument(parent);
    } else {
      data->generation =
          static_cast<StoragePartitionImpl*>(frame->GetStoragePartition())
              ->CreateAlloyNetworkGeneration();
    }
  }
  return data->generation;
}

void SetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame,
    scoped_refptr<AlloyNetworkGeneration> generation) {
  if (generation && AlloyDocumentStateAdmitted(frame) &&
      !frame->GetBrowserContext()->ShutdownStarted()) {
    AlloyDocumentNetworkData::GetOrCreateForCurrentDocument(frame)->generation =
        std::move(generation);
  }
}

network::mojom::NetworkContext* GetAlloyNetworkContextForDocument(
    RenderFrameHostImpl* frame) {
  if (!frame)
    return UnavailableNetworkContext();
  const auto* policy = alloy::site_records::RequestContextPolicy::Get(
      *frame->GetBrowserContext());
  if (!AlloyDocumentStateAdmitted(frame) ||
      frame->GetBrowserContext()->ShutdownStarted()) {
    return UnavailableNetworkContext();
  }
  auto generation = GetAlloyNetworkGenerationForDocument(frame);
  if (generation)
    return generation->GetNetworkContext();
  if (GetAlloyEmbeddedStateForDocument(frame).temporary_admission_closed)
    return UnavailableNetworkContext();
  if (policy &&
      policy->network_state() == alloy::site_records::NetworkState::kCut)
    return UnavailableNetworkContext();
  return frame->GetStoragePartition()->GetNetworkContext();
}

void RetireAlloyNetworkGenerationForDocument(RenderFrameHostImpl* frame) {
  if (auto* data = AlloyDocumentNetworkData::GetForCurrentDocument(frame))
    data->Retire();
}

}  // namespace content
