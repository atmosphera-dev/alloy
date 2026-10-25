// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/alloy_network_generation.h"

#include <utility>

#include "base/check.h"
#include "base/no_destructor.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/bind.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/document_user_data.h"
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

AlloyNetworkGeneration::AlloyNetworkGeneration()
    : endpoints_(std::make_unique<Endpoints>()) {}

AlloyNetworkGeneration::~AlloyNetworkGeneration() {
  Shutdown();
}

void AlloyNetworkGeneration::Bind(
    mojo::PendingRemote<network::mojom::NetworkContext> context,
    mojo::PendingRemote<cert_verifier::mojom::CertVerifierServiceUpdater> verifier,
    base::OnceClosure remove_client, base::RepeatingClosure recreate) {
  if (closed_) return;
  endpoints_->remove_client.RunAndReset();
  endpoints_->context.reset();
  endpoints_->verifier.reset();
  endpoints_->context.Bind(std::move(context));
  endpoints_->verifier.Bind(std::move(verifier));
  endpoints_->remove_client.ReplaceClosure(std::move(remove_client));
  endpoints_->recreate = std::move(recreate);
  endpoints_->context.set_disconnect_handler(base::BindOnce(
      [](base::RepeatingClosure recreate) { recreate.Run(); }, endpoints_->recreate));
}

network::mojom::NetworkContext* AlloyNetworkGeneration::GetNetworkContext() {
  // A late native client sees ordinary Mojo disconnection, never a newly
  // created context or the partition's shared state after its owner retires.
  if (closed_) return endpoints_->context.get();
  if ((!endpoints_->context.is_bound() || !endpoints_->context.is_connected()) &&
      endpoints_->recreate) {
    endpoints_->recreate.Run();
  }
  CHECK(endpoints_->context.is_bound());
  return endpoints_->context.get();
}

cert_verifier::mojom::CertVerifierServiceUpdater*
AlloyNetworkGeneration::GetVerifier() {
  if (closed_) return nullptr;
  GetNetworkContext();
  return endpoints_->verifier.is_bound() ? endpoints_->verifier.get() : nullptr;
}

base::WeakPtr<AlloyNetworkGeneration> AlloyNetworkGeneration::GetWeakPtr() {
  return closed_ ? base::WeakPtr<AlloyNetworkGeneration>()
                 : weak_factory_.GetWeakPtr();
}

void AlloyNetworkGeneration::Shutdown() {
  if (closed_) return;
  closed_ = true;
  weak_factory_.InvalidateWeakPtrs();
  endpoints_->recreate.Reset();
  endpoints_->context.reset();
  // Destroy the server endpoint immediately. Keep only a disconnected client
  // proxy for already-issued native callbacks; this never opens a network store.
  auto disconnected = endpoints_->context.BindNewPipeAndPassReceiver();
  disconnected.reset();
  endpoints_->verifier.reset();
  endpoints_->remove_client.RunAndReset();
}

namespace {
network::mojom::NetworkContext* UnavailableNetworkContext() {
  // Closed document admission must not fall through to the shared partition.
  // This immutable disconnected endpoint has no Network Service/server state.
  static base::NoDestructor<mojo::Remote<network::mojom::NetworkContext>> closed;
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
    if (owns_generation_ && generation) generation->Shutdown();
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

scoped_refptr<AlloyNetworkGeneration> GetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame) {
  if (!AlloyDocumentStateAdmitted(frame) ||
      frame->GetBrowserContext()->ShutdownStarted()) return nullptr;
  const auto* policy = alloy::site_records::RequestContextPolicy::Get(
      *frame->GetBrowserContext());
  if (!policy || policy->network_state() != alloy::site_records::NetworkState::kCut)
    return nullptr;
  auto* data = AlloyDocumentNetworkData::GetOrCreateForCurrentDocument(frame);
  if (!data->generation) {
    if (auto* parent = frame->GetParentOrOuterDocument()) {
      data->generation = GetAlloyNetworkGenerationForDocument(parent);
    } else {
      data->generation = static_cast<StoragePartitionImpl*>(frame->GetStoragePartition())
                             ->CreateAlloyNetworkGeneration();
    }
  }
  return data->generation;
}

void SetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame, scoped_refptr<AlloyNetworkGeneration> generation) {
  if (generation && AlloyDocumentStateAdmitted(frame) &&
      !frame->GetBrowserContext()->ShutdownStarted()) {
    AlloyDocumentNetworkData::GetOrCreateForCurrentDocument(frame)->generation =
        std::move(generation);
  }
}

network::mojom::NetworkContext* GetAlloyNetworkContextForDocument(
    RenderFrameHostImpl* frame) {
  if (!frame) return UnavailableNetworkContext();
  const auto* policy = alloy::site_records::RequestContextPolicy::Get(
      *frame->GetBrowserContext());
  if (!policy) return frame->GetStoragePartition()->GetNetworkContext();
  if (!AlloyDocumentStateAdmitted(frame) ||
      frame->GetBrowserContext()->ShutdownStarted()) {
    return UnavailableNetworkContext();
  }
  auto generation = GetAlloyNetworkGenerationForDocument(frame);
  if (generation) return generation->GetNetworkContext();
  if (policy && policy->network_state() == alloy::site_records::NetworkState::kCut)
    return UnavailableNetworkContext();
  return frame->GetStoragePartition()->GetNetworkContext();
}

void RetireAlloyNetworkGenerationForDocument(RenderFrameHostImpl* frame) {
  if (auto* data = AlloyDocumentNetworkData::GetForCurrentDocument(frame))
    data->Retire();
}

}  // namespace content
