// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/alloy_embedded_state.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "content/browser/back_forward_cache/back_forward_cache_disable.h"
#include "content/browser/code_cache/generated_code_cache_context.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace content {
namespace {

using alloy::site_records::EmbeddedFirstPartyPolicy;
using alloy::site_records::EmbeddedStateRegistry;
using alloy::site_records::EmbeddedStateScope;
using alloy::site_records::RequestContextPolicy;
using alloy::site_records::StorageMode;
using alloy::site_records::StorePurpose;
using Backing = EmbeddedStateRegistry::Backing;
const char kContextKey = 0;
const char kSelectionKey = 0;

class ContextData final : public base::SupportsUserData::Data {
 public:
  explicit ContextData(BrowserContext& context) : context_(context) {}
  ~ContextData() override { Shutdown(); }

  EmbeddedStateRegistry* registry() {
    if (closed_ || context_->ShutdownStarted()) {
      return nullptr;
    }
    if (!registry_) {
      const auto* mode = RequestContextPolicy::Get(*context_);
      registry_ = std::make_unique<EmbeddedStateRegistry>(
          mode ? mode->profile() : context_->UniqueToken(),
          mode ? mode->storage_mode() : StorageMode::kEncrypted, scope_,
          std::move(policy_));
    }
    return registry_.get();
  }
  bool ConfigureScope(EmbeddedStateScope scope) {
    if (closed_ || registry_) {
      return false;
    }
    scope_ = scope;
    return true;
  }
  bool ConfigurePolicy(std::unique_ptr<EmbeddedFirstPartyPolicy> policy) {
    if (closed_ || registry_) {
      return false;
    }
    policy_ = std::move(policy);
    return true;
  }
  void Shutdown() {
    if (closed_) {
      return;
    }
    closed_ = true;
    weak_factory_.InvalidateWeakPtrs();
    if (registry_) {
      registry_->Shutdown();
    }
    policy_.reset();
  }
  void RevokeSite(const GURL& url, const url::Origin& origin) {
    if (!closed_ && registry_) {
      registry_->RevokeSite(url, origin);
    }
  }
  base::WeakPtr<ContextData> GetWeakPtr() {
    return closed_ ? base::WeakPtr<ContextData>() : weak_factory_.GetWeakPtr();
  }

 private:
  const raw_ref<BrowserContext> context_;
  EmbeddedStateScope scope_ = EmbeddedStateScope::kPerOuterSite;
  std::unique_ptr<EmbeddedFirstPartyPolicy> policy_;
  std::unique_ptr<EmbeddedStateRegistry> registry_;
  bool closed_ = false;
  base::WeakPtrFactory<ContextData> weak_factory_{this};
};

ContextData* Context(BrowserContext& context) {
  if (context.ShutdownStarted()) {
    return nullptr;
  }
  auto* data = static_cast<ContextData*>(context.GetUserData(&kContextKey));
  if (!data) {
    auto value = std::make_unique<ContextData>(context);
    data = value.get();
    context.SetUserData(&kContextKey, std::move(value));
  }
  return data->GetWeakPtr() ? data : nullptr;
}

class TabData final : public WebContentsObserver,
                      public WebContentsUserData<TabData> {
 public:
  ~TabData() override { Close(); }
  EmbeddedStateRegistry* registry() {
    return context_ ? context_->registry() : nullptr;
  }
  base::UnguessableToken tab() const { return tab_; }

 private:
  explicit TabData(WebContents* contents)
      : WebContentsObserver(contents), WebContentsUserData(*contents) {
    if (auto* data = Context(*contents->GetBrowserContext())) {
      context_ = data->GetWeakPtr();
    }
    if (auto* frame = contents->GetPrimaryMainFrame()) {
      Commit(frame->GetLastCommittedURL(), frame->GetLastCommittedOrigin());
    }
  }
  void DidFinishNavigation(NavigationHandle* navigation) override {
    if (navigation->HasCommitted() && navigation->IsInPrimaryMainFrame()) {
      Commit(navigation->GetURL(),
             navigation->GetRenderFrameHost()->GetLastCommittedOrigin());
    }
  }
  void WebContentsDestroyed() override { Close(); }
  void Commit(const GURL& url, const url::Origin& origin) {
    if (!context_) {
      return;
    }
    // Blank/component startup must not freeze the app's request-context option.
    // Once a real web site is admitted, non-network moves still retire it.
    if (!admitted_ &&
        alloy::site_records::RecordDomainForPrincipal(url, origin).empty()) {
      return;
    }
    admitted_ = true;
    if (auto* owner = registry()) {
      owner->CommitOuterSite(tab_, url, origin);
    }
  }
  void Close() {
    if (admitted_) {
      admitted_ = false;
      if (auto* owner = registry()) {
        owner->CloseTab(tab_);
      }
    }
    context_.reset();
  }
  base::WeakPtr<ContextData> context_;
  const base::UnguessableToken tab_ = base::UnguessableToken::Create();
  bool admitted_ = false;
  friend class WebContentsUserData<TabData>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(TabData);

class EmbeddedCodeCacheService final : public KeyedService {
 public:
  EmbeddedCodeCacheService()
      : context_(base::MakeRefCounted<GeneratedCodeCacheContext>()) {
    context_->InitializeInMemory(20 * 1024 * 1024);
    receivers_ = std::make_unique<CodeCacheHostImpl::ReceiverSet>(context_);
  }
  ~EmbeddedCodeCacheService() override { Shutdown(); }
  void Shutdown() override {
    if (!context_)
      return;
    // Destruction and context teardown are ordered on the native cache runner.
    receivers_.reset();
    context_->Shutdown();
    context_.reset();
  }
  CodeCacheHostImpl::ReceiverSet* receivers() { return receivers_.get(); }

 private:
  scoped_refptr<GeneratedCodeCacheContext> context_;
  std::unique_ptr<CodeCacheHostImpl::ReceiverSet> receivers_;
};
class Selection final : public base::SupportsUserData::Data {
 public:
  GURL document_url;
  url::Origin origin;
  AlloyEmbeddedResolution value;
};

AlloyEmbeddedResolution Admitted(AlloyEmbeddedResolution value) {
  if (value.backing == Backing::kTemporary &&
      (!value.temporary ||
       !value.temporary->Capability(StorePurpose::kDomStorage).active())) {
    value.backing = Backing::kNone;
    value.temporary.reset();
    value.temporary_admission_closed = true;
  }
  return value;
}

AlloyEmbeddedResolution Resolve(RenderFrameHost* frame,
                                const GURL& url,
                                const url::Origin& origin,
                                base::WeakPtr<base::SupportsUserData> owner,
                                bool navigation) {
  if (!frame || !owner || frame->GetBrowserContext()->ShutdownStarted()) {
    return {};
  }
  if (alloy::site_records::RecordDomainForPrincipal(url, origin).empty()) {
    return {};
  }
  auto* context = Context(*frame->GetBrowserContext());
  if (!context) {
    return {};
  }
  auto context_lifetime = context->GetWeakPtr();
  auto* selected = static_cast<Selection*>(owner->GetUserData(&kSelectionKey));
  if (selected && (!navigation || (selected->document_url == url &&
                                   selected->origin == origin))) {
    return Admitted(selected->value);
  }
  auto* top = frame->GetOutermostMainFrame();
  auto* contents = WebContents::FromRenderFrameHost(top);
  if (!contents ||
      contents->GetBrowserContext() != frame->GetBrowserContext()) {
    return {};
  }
  TabData::CreateForWebContents(contents);
  auto* tab = TabData::FromWebContents(contents);
  auto* registry = tab->registry();
  if (!registry) {
    return {};
  }
  AlloyEmbeddedResolution result;
  if (!frame->GetParentOrOuterDocument()) {
    if (!RequestContextPolicy::IsStateless(*frame->GetBrowserContext()) &&
        !alloy::site_records::RecordDomainForPrincipal(url, origin).empty()) {
      result.backing = Backing::kRealSite;
    }
  } else {
    result = registry->Resolve(tab->tab(), url, origin);
  }
  if (!owner || !context_lifetime) {
    return {};
  }
  auto selection = std::make_unique<Selection>();
  selection->document_url = url;
  selection->origin = origin;
  selection->value = result;
  owner->SetUserData(&kSelectionKey, std::move(selection));
  if (result.backing == Backing::kTemporary) {
    // A restored renderer would retain its own old storage cache even after the
    // native backing retires. Pages with temporary storage must load afresh.
    BackForwardCache::DisableForRenderFrameHost(
        top,
        BackForwardCacheDisable::DisabledReason(
            BackForwardCacheDisable::DisabledReasonId::kAlloyEmbeddedState));
  }
  return Admitted(std::move(result));
}

std::optional<base::UnguessableToken> Nonce(AlloyEmbeddedResolution value) {
  if (value.backing != Backing::kTemporary || !value.temporary) {
    return std::nullopt;
  }
  return value.temporary->Capability(StorePurpose::kDomStorage).generation();
}

}  // namespace

CodeCacheHostImpl::ReceiverSet* GetAlloyEmbeddedCodeCacheReceivers(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!owner)
    return nullptr;
  static const char kCodeCacheService = 0;
  auto* service =
      static_cast<EmbeddedCodeCacheService*>(owner->GetOrCreateService(
          &kCodeCacheService, StorePurpose::kCodeCache,
          base::BindOnce(
              [](alloy::site_records::RecordCapability)
                  -> std::unique_ptr<KeyedService> {
                return std::make_unique<EmbeddedCodeCacheService>();
              }),
          &partition));
  return service ? service->receivers() : nullptr;
}

bool ConfigureAlloyEmbeddedStateScope(BrowserContext& context,
                                      EmbeddedStateScope scope) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (scope != EmbeddedStateScope::kPerOuterSite &&
      scope != EmbeddedStateScope::kPerTab) {
    return false;
  }
  auto* data = Context(context);
  return data && data->ConfigureScope(scope);
}

bool ConfigureAlloyEmbeddedFirstPartyPolicy(
    BrowserContext& context,
    std::unique_ptr<EmbeddedFirstPartyPolicy> policy) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto* data = Context(context);
  return data && data->ConfigurePolicy(std::move(policy));
}

void InitializeAlloyEmbeddedTab(WebContents* contents) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  TabData::CreateForWebContents(contents);
}

void ShutdownAlloyEmbeddedState(BrowserContext& context) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (auto* data =
          static_cast<ContextData*>(context.GetUserData(&kContextKey))) {
    data->Shutdown();
  }
}

void RevokeAlloyEmbeddedSite(BrowserContext& context,
                             const GURL& document_url,
                             const url::Origin& document_origin) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (auto* data =
          static_cast<ContextData*>(context.GetUserData(&kContextKey))) {
    data->RevokeSite(document_url, document_origin);
  }
}

AlloyEmbeddedResolution GetAlloyEmbeddedStateForDocument(
    RenderFrameHost* frame) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!frame || !frame->IsActive()) {
    return {};
  }
  auto owner = AlloyDocumentStateOwner(frame);
  const bool captured = owner && owner->GetUserData(&kSelectionKey);
  auto result = Resolve(frame, frame->GetLastCommittedURL(),
                        frame->GetLastCommittedOrigin(), owner, false);
  if (result.backing == Backing::kTemporary && !captured) {
    // A document first bound after deletion cannot adopt a replacement
    // generation. Its native key was committed by the original navigation;
    // this rejects a newly resolved replacement, never looks up a backend by
    // nonce. An ORIGINAL selection already captured on the native handle is
    // authority independently of credentialless/fenced-frame security nonces.
    const auto& native_key = frame->GetStorageKey();
    if (!native_key.nonce() || !result.temporary ||
        *native_key.nonce() !=
            result.temporary->Capability(StorePurpose::kDomStorage)
                .generation()) {
      return {.temporary_admission_closed = true};
    }
  }
  return result;
}

AlloyEmbeddedResolution GetAlloyEmbeddedStateForNavigation(
    NavigationRequest* navigation,
    const url::Origin& origin) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!navigation) {
    return {};
  }
  return Resolve(navigation->frame_tree_node()->current_frame_host(),
                 navigation->GetURL(), origin,
                 AlloyNavigationStateOwner(navigation), true);
}

std::optional<base::UnguessableToken> GetAlloyEmbeddedNonceForCandidate(
    RenderFrameHostImpl* frame,
    const url::Origin& origin) {
  return Nonce(GetAlloyEmbeddedStateForCandidate(frame, origin));
}

AlloyEmbeddedResolution GetAlloyEmbeddedStateForCandidate(
    RenderFrameHostImpl* frame,
    const url::Origin& origin) {
  if (!frame) {
    return {};
  }
  if (auto* navigation = frame->frame_tree_node()->navigation_request()) {
    if (!navigation->IsSameDocument()) {
      return GetAlloyEmbeddedStateForNavigation(navigation, origin);
    }
  }
  if (!frame->IsActive()) {
    return {};
  }
  // Candidate resolution computes the native key itself, before that key can
  // be validated by the ordinary document/store admission entry point above.
  return Resolve(frame, frame->GetLastCommittedURL(),
                 frame->GetLastCommittedOrigin(),
                 AlloyDocumentStateOwner(frame), false);
}

std::optional<base::UnguessableToken> GetAlloyEmbeddedNonceForNavigation(
    NavigationRequest* navigation,
    const url::Origin& origin) {
  return Nonce(GetAlloyEmbeddedStateForNavigation(navigation, origin));
}

}  // namespace content
