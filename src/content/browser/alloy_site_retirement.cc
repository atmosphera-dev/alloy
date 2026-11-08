// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/public/browser/alloy_site_retirement.h"

#include <set>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "content/browser/renderer_host/frame_tree.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/preloading/prerender/prerender_metrics.h"
#include "content/browser/preloading/prerender/prerender_final_status.h"
#include "content/browser/storage_partition_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/browser/worker_host/dedicated_worker_host.h"
#include "content/browser/worker_host/dedicated_worker_service_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/dedicated_worker_service.h"
#include "content/public/browser/shared_worker_service.h"
#include "content/public/browser/service_worker_context.h"
#include "content/public/browser/storage_usage_info.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/alloy_document_state.h"

namespace content {
namespace {
class Retirement;
class PageWatch final : public WebContentsObserver {
 public:
  PageWatch(WebContents* contents, Retirement* owner);
  void RenderFrameDeleted(RenderFrameHost* frame) override;
  void WebContentsDestroyed() override;
 private:
  const raw_ptr<Retirement> owner_;
};

class PartitionWatch final : public DedicatedWorkerService::Observer,
                             public SharedWorkerService::Observer {
 public:
  PartitionWatch(StoragePartition* partition, Retirement* owner);
  ~PartitionWatch() override;
  void OnWorkerCreated(const blink::DedicatedWorkerToken&, ChildProcessId,
                       const url::Origin&, DedicatedWorkerCreator) override;
  void OnBeforeWorkerDestroyed(const blink::DedicatedWorkerToken&, DedicatedWorkerCreator) override;
  void OnFinalResponseURLDetermined(const blink::DedicatedWorkerToken&, const GURL&) override {}
  void OnFinalResponseURLDetermined(const blink::SharedWorkerToken&, const GURL&) override {}
  void OnWorkerCreated(const blink::SharedWorkerToken&, ChildProcessId, const url::Origin&,
                       const base::UnguessableToken&) override;
  void OnBeforeWorkerDestroyed(const blink::SharedWorkerToken&) override {}
  void OnClientAdded(const blink::SharedWorkerToken&, GlobalRenderFrameHostId) override {}
  void OnClientRemoved(const blink::SharedWorkerToken&, GlobalRenderFrameHostId) override {}
  void StopShared();
  std::set<blink::DedicatedWorkerToken> dedicated;
 private:
  const base::WeakPtr<StoragePartitionImpl> partition_;
  const raw_ptr<Retirement> owner_;
  std::vector<blink::SharedWorkerToken> shared_;
  void TerminateShared(const blink::SharedWorkerToken& token, bool defer);
  bool stopping_ = false;
};

class Retirement final : public base::RefCounted<Retirement> {
 public:
  Retirement(base::RepeatingCallback<bool(const url::Origin&)> owns,
             base::RepeatingCallback<void(WebContents*)> close,
             base::OnceCallback<void(bool, base::OnceClosure)> done)
      : owns_(std::move(owns)), close_(std::move(close)), done_(std::move(done)) {}
  bool Owns(const url::Origin& origin) const { return !origin.opaque() && owns_.Run(origin); }
  void Prepare(BrowserContext* context) {
    for (const auto& entry : WebContentsImpl::GetAllWebContents()) {
      auto* page = entry.get();
      if (page->GetBrowserContext() != context) continue;
      pages_.push_back(std::make_unique<PageWatch>(page, this));
      page->ForEachRenderFrameHost([&](RenderFrameHost* frame) {
        if (Owns(frame->GetLastCommittedOrigin())) {
          frames_.insert(frame->GetGlobalId()); targets_.push_back(frame->GetGlobalId());
          RetireAlloyDocumentState(frame);
        }
      });
    }
    auto* permissions = context->GetPermissionControllerDelegate();
    if (permissions) revoke_ = permissions->PrepareSitePermissions(owns_);
    success_ = !!revoke_;
    if (success_) publication_ = permissions->TakeSitePermissionRetirementPublication();
  }
  void Start(BrowserContext* context) {
    alive_ = this;
    context->ForEachLoadedStoragePartition([&](StoragePartition* partition) {
      partitions_.push_back(std::make_unique<PartitionWatch>(partition, this));
      ++pending_;
      auto weak = static_cast<StoragePartitionImpl*>(partition)->GetWeakPtr();
      partition->GetServiceWorkerContext()->GetAllStorageKeysInfo(base::BindOnce(
          [](scoped_refptr<Retirement> self, base::WeakPtr<StoragePartitionImpl> partition,
             const std::vector<StorageUsageInfo>& keys) {
            if (partition) for (const auto& entry : keys) {
              if (!self->Owns(entry.storage_key.origin())) continue;
              ++self->pending_;
              partition->GetServiceWorkerContext()->DeleteForStorageKey(entry.storage_key,
                  base::BindOnce([](scoped_refptr<Retirement> self, bool ok) {
                    self->success_ = self->success_ && ok; self->Completed();
                  }, self));
            }
            self->Completed();
          }, base::WrapRefCounted(this), weak));
    });
    // Native permission owners are reached through the existing profile
    // delegate. Its map enumerates registered grant owners, closes their old
    // admission and stages revocation before delivering any notification.
    auto* permissions = context->GetPermissionControllerDelegate();
    if (revoke_) std::move(revoke_).Run();
    if (permissions && success_) {
      ++pending_;
      permissions->DrainSitePermissions(base::BindOnce(
          [](scoped_refptr<Retirement> self, bool ok) {
            self->success_ = self->success_ && ok; self->Completed();
          }, base::WrapRefCounted(this)));
    }
    for (const auto& partition : partitions_) partition->StopShared();
    // Outermost matching documents own their matching descendants. Resolve IDs
    // afresh because teardown may synchronously remove another selected frame.
    std::set<WebContents*> closing;
    for (const auto id : targets_) {
      auto* frame = RenderFrameHostImpl::FromID(id);
      if (!frame) continue;
      if (frame->IsInBackForwardCache()) {
        frame->EvictFromBackForwardCacheWithReason(BackForwardCacheMetrics::NotRestoredReason::kCacheFlushed);
      } else if (frame->CancelPrerendering(PrerenderCancellationReason(PrerenderFinalStatus::kBrowsingDataRemoved))) {
        continue;
      } else if (!frame->GetParentOrOuterDocument()) {
        auto* contents = WebContents::FromRenderFrameHost(frame);
        if (closing.insert(contents).second) close_.Run(contents);
      } else {
        auto* node = frame->frame_tree_node();
        node->frame_tree().RemoveFrame(node);
      }
    }
    starting_ = false;
    MaybeFinish();
  }
  void FrameGone(GlobalRenderFrameHostId id) { frames_.erase(id); MaybeFinish(); }
  void PageGone(WebContents* page) {
    for (auto it = frames_.begin(); it != frames_.end();) {
      auto* frame = RenderFrameHost::FromID(*it);
      if (!frame || WebContents::FromRenderFrameHost(frame) == page) it = frames_.erase(it);
      else ++it;
    }
    MaybeFinish();
  }
  void Pending() { ++pending_; }
  void Completed() { CHECK_GT(pending_, 0u); --pending_; MaybeFinish(); }
  bool Drained() const {
    if (starting_ || pending_ || !frames_.empty()) return false;
    for (const auto& partition : partitions_) if (!partition->dedicated.empty()) return false;
    return true;
  }
  void MaybeFinish() {
    if (!Drained()) return;
    if (finishing_) return;
    finishing_ = true;
    // Let native service/frame destructors finish before deletion can unlink
    // or acknowledge. This is an event barrier, never a timer/quiet heuristic.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](scoped_refptr<Retirement> self) {
          // An already-started worker may announce itself in the unwind turn.
          // Completion is owned by the current barrier state, not a stale
          // observation that happened to be empty when this task was posted.
          if (!self->Drained()) {
            self->finishing_ = false;
            self->MaybeFinish();
            return;
          }
          self->pages_.clear(); self->partitions_.clear();
          auto done = std::move(self->done_); self->alive_ = nullptr;
          std::move(done).Run(self->success_, std::move(self->publication_));
        }, base::WrapRefCounted(this)));
  }
 private:
  friend class base::RefCounted<Retirement>;
  ~Retirement() = default;
  const base::RepeatingCallback<bool(const url::Origin&)> owns_;
  const base::RepeatingCallback<void(WebContents*)> close_;
  base::OnceCallback<void(bool, base::OnceClosure)> done_;
  base::OnceClosure publication_;
  base::OnceClosure revoke_;
  scoped_refptr<Retirement> alive_;
  std::vector<std::unique_ptr<PageWatch>> pages_;
  std::vector<std::unique_ptr<PartitionWatch>> partitions_;
  std::set<GlobalRenderFrameHostId> frames_;
  std::vector<GlobalRenderFrameHostId> targets_;
  size_t pending_ = 0;
  bool starting_ = true, finishing_ = false;
  bool success_ = true;
};
PageWatch::PageWatch(WebContents* contents, Retirement* owner) : WebContentsObserver(contents), owner_(owner) {}
void PageWatch::RenderFrameDeleted(RenderFrameHost* frame) { owner_->FrameGone(frame->GetGlobalId()); }
void PageWatch::WebContentsDestroyed() { owner_->PageGone(web_contents()); }
PartitionWatch::PartitionWatch(StoragePartition* partition, Retirement* owner)
    : partition_(static_cast<StoragePartitionImpl*>(partition)->GetWeakPtr()), owner_(owner) {
  partition->GetDedicatedWorkerService()->AddObserver(this);
  partition->GetSharedWorkerService()->AddObserver(this);
  partition->GetDedicatedWorkerService()->EnumerateDedicatedWorkers(this);
  partition->GetSharedWorkerService()->EnumerateSharedWorkers(this);
}
PartitionWatch::~PartitionWatch() {
  if (!partition_) return;
  partition_->GetDedicatedWorkerService()->RemoveObserver(this);
  partition_->GetSharedWorkerService()->RemoveObserver(this);
}
void PartitionWatch::OnWorkerCreated(const blink::DedicatedWorkerToken& token, ChildProcessId,
    const url::Origin& origin, DedicatedWorkerCreator) {
  bool owned = owner_->Owns(origin);
  if (!owned && partition_) {
    auto* service = static_cast<DedicatedWorkerServiceImpl*>(partition_->GetDedicatedWorkerService());
    auto* host = service->GetDedicatedWorkerHostFromToken(token);
    auto* frame = host ? host->GetAncestorRenderFrameHost() : nullptr;
    owned = frame && owner_->Owns(frame->GetLastCommittedOrigin());
  }
  if (owned) dedicated.insert(token);
}
void PartitionWatch::OnBeforeWorkerDestroyed(const blink::DedicatedWorkerToken& token, DedicatedWorkerCreator) {
  dedicated.erase(token); owner_->MaybeFinish();
}
void PartitionWatch::OnWorkerCreated(const blink::SharedWorkerToken& token, ChildProcessId,
    const url::Origin& origin, const base::UnguessableToken&) {
  if (!owner_->Owns(origin)) return;
  if (stopping_) TerminateShared(token, true);
  else shared_.push_back(token);
}
void PartitionWatch::TerminateShared(const blink::SharedWorkerToken& token, bool defer) {
  owner_->Pending();
  if (!defer && partition_) {
    partition_->GetSharedWorkerService()->TerminateWorkerAndWait(token,
        base::BindOnce(&Retirement::Completed, base::WrapRefCounted(owner_.get())));
    return;
  }
  // OnWorkerCreated can run in the host constructor. Unwind it before asking
  // the service to destroy that host; the native completion remains counted.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<StoragePartitionImpl> partition, blink::SharedWorkerToken token,
         scoped_refptr<Retirement> owner) {
        if (!partition) { owner->Completed(); return; }
        partition->GetSharedWorkerService()->TerminateWorkerAndWait(token,
            base::BindOnce(&Retirement::Completed, owner));
      }, partition_, token, base::WrapRefCounted(owner_.get())));
}
void PartitionWatch::StopShared() {
  stopping_ = true;
  if (!partition_) return;
  for (const auto& token : shared_) TerminateShared(token, false);
  shared_.clear();
}
}  // namespace
base::OnceClosure PrepareAlloySiteRetirement(BrowserContext* context,
    base::RepeatingCallback<bool(const url::Origin&)> owns,
    base::RepeatingCallback<void(WebContents*)> close_page,
    base::OnceCallback<void(bool, base::OnceClosure)> done) {
  CHECK(context); CHECK(owns); CHECK(close_page); CHECK(done);
  auto retirement = base::MakeRefCounted<Retirement>(std::move(owns), std::move(close_page), std::move(done));
  retirement->Prepare(context);
  return base::BindOnce(&Retirement::Start, std::move(retirement), context);
}
void RetireAlloySiteState(BrowserContext* context,
    base::RepeatingCallback<bool(const url::Origin&)> owns,
    base::RepeatingCallback<void(WebContents*)> close_page,
    base::OnceCallback<void(bool, base::OnceClosure)> done) {
  CHECK(context); CHECK(owns); CHECK(close_page); CHECK(done);
  PrepareAlloySiteRetirement(context, std::move(owns), std::move(close_page), std::move(done)).Run();
}
}  // namespace content
