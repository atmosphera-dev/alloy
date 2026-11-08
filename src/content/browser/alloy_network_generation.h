// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_ALLOY_NETWORK_GENERATION_H_
#define CONTENT_BROWSER_ALLOY_NETWORK_GENERATION_H_

#include <memory>
#include <optional>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "brave/alloy/site_records/record_capability.h"
#include "content/public/browser/alloy_document_state.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/cert_verifier_service_updater.mojom-forward.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace content {

class RenderFrameHostImpl;

// CUT documents own their lease. Temporary embedded scopes own their lease;
// frame/navigation/worker borrowers cannot extend either owner's authority.
// StoragePartition keeps weak registrations only, so retired generations do
// not accumulate for a request context's lifetime. No StoragePartition swap.
class AlloyNetworkGeneration : public base::RefCounted<AlloyNetworkGeneration> {
 public:
  explicit AlloyNetworkGeneration(
      std::optional<alloy::site_records::RecordCapability> memory_owner =
          std::nullopt);
  bool memory_only() const { return memory_owner_.has_value(); }
  void Bind(
      mojo::PendingRemote<network::mojom::NetworkContext> context,
      mojo::PendingRemote<cert_verifier::mojom::CertVerifierServiceUpdater>
          verifier,
      base::OnceClosure remove_client,
      base::RepeatingClosure recreate);
  network::mojom::NetworkContext* GetNetworkContext();
  cert_verifier::mojom::CertVerifierServiceUpdater* GetVerifier();
  base::WeakPtr<AlloyNetworkGeneration> GetWeakPtr();
  // Retains only the current physical endpoints for an entered native drain.
  // Shutdown still closes admission and gives late callers a disconnected pipe.
  base::OnceClosure RetainNativeRetirementEndpoints();
  void Shutdown();

 private:
  friend class base::RefCounted<AlloyNetworkGeneration>;
  ~AlloyNetworkGeneration();
  bool CheckAdmission();
  struct Endpoints;
  std::shared_ptr<Endpoints> endpoints_;
  const std::optional<alloy::site_records::RecordCapability> memory_owner_;
  bool closed_ = false;
  base::WeakPtrFactory<AlloyNetworkGeneration> weak_factory_{this};
};

scoped_refptr<AlloyNetworkGeneration> GetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame,
    const AlloyEmbeddedResolution* pending_owner = nullptr);
network::mojom::NetworkContext* GetAlloyNetworkContextForDocument(
    RenderFrameHostImpl* frame);
void SetAlloyNetworkGenerationForDocument(
    RenderFrameHostImpl* frame,
    scoped_refptr<AlloyNetworkGeneration> generation);
// Called by the canonical document owner before any teardown callback. Borrowed
// subframe/worker references cannot extend a top-level generation's authority.
void RetireAlloyNetworkGenerationForDocument(RenderFrameHostImpl* frame);

}  // namespace content

#endif  // CONTENT_BROWSER_ALLOY_NETWORK_GENERATION_H_
