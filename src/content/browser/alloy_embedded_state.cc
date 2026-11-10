// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/alloy_embedded_state.h"

#include <atomic>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/alloy/site_records/native_profile_configuration.h"
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

// Admission-cap fallback for the cross-record entry-LRU follow-up. This is a
// fixed process total, matching Chromium's 80 MiB default code-cache size, not
// a sum that grows with visited sites. Entry LRU: github.com/atmosphera-dev/alloy/issues/13.
// Each bundle reserves all four possible
// native data budgets: JS, Wasm, the JS memory front-cache and persistent-cache
// collection. Non-cache state is never evicted to make room.
constexpr size_t kCodeCacheTotalBytes = 80 * 1024 * 1024;
constexpr int kCodeCacheBackendBytes = 4 * 1024 * 1024;
constexpr size_t kCodeCacheBundleBytes = 4 * kCodeCacheBackendBytes;
std::atomic<size_t> g_code_cache_reserved_bytes{0};

class CodeCacheReservation final {
 public:
  CodeCacheReservation(const CodeCacheReservation&) = delete;
  CodeCacheReservation& operator=(const CodeCacheReservation&) = delete;
  static std::unique_ptr<CodeCacheReservation> Acquire() {
    size_t current = g_code_cache_reserved_bytes.load();
    while (current <= kCodeCacheTotalBytes - kCodeCacheBundleBytes) {
      if (g_code_cache_reserved_bytes.compare_exchange_weak(
              current, current + kCodeCacheBundleBytes))
        return std::unique_ptr<CodeCacheReservation>(new CodeCacheReservation);
    }
    return nullptr;
  }
  ~CodeCacheReservation() {
    CHECK_GE(g_code_cache_reserved_bytes.fetch_sub(kCodeCacheBundleBytes),
             kCodeCacheBundleBytes);
  }
 private:
  CodeCacheReservation() = default;
};

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
      const auto* input = alloy::site_records::NativeProfileConfiguration::Get(*context_);
      registry_ = std::make_unique<EmbeddedStateRegistry>(
          mode ? mode->profile() : context_->UniqueToken(),
          mode ? mode->storage_mode() : StorageMode::kEncrypted, scope_,
          std::move(policy_), input ? std::make_optional(input->original_generation())
                                    : std::nullopt,
          base::BindRepeating(
              [](base::WeakPtr<ContextData> owner,
                 base::RepeatingCallback<bool(std::string_view)> admitted,
                 std::string_view site) {
                return owner && !owner->retiring_sites_.contains(site) &&
                    (!admitted || admitted.Run(site));
              }, weak_factory_.GetWeakPtr(),
              input ? input->record_working_state_admission()
                    : base::RepeatingCallback<bool(std::string_view)>()));
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
  base::OnceClosure BeginFirstPartyRetirement(const GURL& site) {
    if (closed_ || context_->ShutdownStarted()) return {};
    const auto origin = url::Origin::Create(site);
    if (registry_) return registry_->BeginFirstPartyRetirement(site, origin);
    // Keep the pre-document configuration window open. A later registry uses
    // the same context-owned admission block, even without profile settings.
    const auto domain = alloy::site_records::RecordDomainForPrincipal(site, origin);
    CHECK(!domain.empty());
    CHECK(retiring_sites_.insert(domain).second);
    return base::BindOnce([](base::WeakPtr<ContextData> owner, std::string domain) {
      if (owner) owner->retiring_sites_.erase(domain);
    }, weak_factory_.GetWeakPtr(), domain);
  }
  base::WeakPtr<ContextData> GetWeakPtr() {
    return closed_ ? base::WeakPtr<ContextData>() : weak_factory_.GetWeakPtr();
  }

 private:
  const raw_ref<BrowserContext> context_;
  EmbeddedStateScope scope_ = EmbeddedStateScope::kPerOuterSite;
  std::unique_ptr<EmbeddedFirstPartyPolicy> policy_;
  std::unique_ptr<EmbeddedStateRegistry> registry_;
  std::set<std::string, std::less<>> retiring_sites_;
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
  static std::unique_ptr<KeyedService> Create() {
    auto reservation = CodeCacheReservation::Acquire();
    if (!reservation) return nullptr;
    return std::make_unique<EmbeddedCodeCacheService>(std::move(reservation));
  }
  explicit EmbeddedCodeCacheService(std::unique_ptr<CodeCacheReservation> reservation)
      : reservation_(std::move(reservation)),
        context_(base::MakeRefCounted<GeneratedCodeCacheContext>()) {
    context_->InitializeInMemory(kCodeCacheBackendBytes);
    receivers_ = std::make_unique<CodeCacheHostImpl::ReceiverSet>(context_);
  }
  ~EmbeddedCodeCacheService() override { Shutdown(); }
  void Shutdown() override {
    if (!context_)
      return;
    // Destruction and context teardown are ordered on the native cache runner.
    auto runner = GeneratedCodeCacheContext::GetTaskRunner(context_);
    receivers_.reset();
    context_->Shutdown();
    // Do not make capacity reusable merely because shutdown was scheduled.
    // The native shutdown task must first destroy the actual cache backends.
    runner->PostTask(FROM_HERE, base::BindOnce(
        [](scoped_refptr<GeneratedCodeCacheContext>,
           std::unique_ptr<CodeCacheReservation>) {},
        std::move(context_), std::move(reservation_)));
  }
  CodeCacheHostImpl::ReceiverSet* receivers() { return receivers_.get(); }

 private:
  std::unique_ptr<CodeCacheReservation> reservation_;
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
  if (value.backing == Backing::kRealSite && !value.first_party.active()) {
    value.backing = Backing::kNone;
    value.temporary_admission_closed = true;
  }
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
      result = registry->ResolveFirstParty(url, origin);
    }
  } else {
    result = registry->Resolve(tab->tab(), url, origin);
  }
  // Unconfigured legacy contexts keep their ordinary native persistence.
  if (!RequestContextPolicy::Get(*frame->GetBrowserContext()))
    result.first_party_state.reset();
  if (!owner || !context_lifetime) {
    return {};
  }
  if (!navigation && result.backing == Backing::kTemporary) {
    // A first bind must match the key committed for this document. Validate
    // before publishing the captured selection, so a rejected replacement
    // cannot become authority on a second lookup. Navigation candidates have
    // their explicit original owner and mint this key before commit.
    const auto& key = frame->GetStorageKey();
    if (!key.nonce() || !result.temporary ||
        *key.nonce() != result.temporary->Capability(StorePurpose::kDomStorage)
                            .generation()) {
      result = {.temporary_admission_closed = true};
    }
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

std::optional<url::Origin> AlloyConsideredFirstPartyCookieOrigin(
    const base::WeakPtr<base::SupportsUserData>& owner,
    BrowserContext* context) {
  const auto resolved = AlloyEmbeddedStateForStateOwner(owner, context);
  if (!resolved.considered_first_party || resolved.backing != Backing::kRealSite ||
      !AlloyStateOwnerForContext(owner, context)) {
    return std::nullopt;
  }
  const auto* selection =
      static_cast<const Selection*>(owner->GetUserData(&kSelectionKey));
  return selection && !selection->origin.opaque()
             ? std::make_optional(selection->origin)
             : std::nullopt;
}

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
                return EmbeddedCodeCacheService::Create();
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

base::OnceClosure BeginAlloyFirstPartyRetirement(BrowserContext& context,
                                                const GURL& site) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto* data = Context(context);
  return data ? data->BeginFirstPartyRetirement(site) : base::OnceClosure();
}

AlloyEmbeddedResolution GetAlloyEmbeddedStateForDocument(
    RenderFrameHost* frame) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!frame || !frame->IsActive()) {
    return {};
  }
  auto owner = AlloyDocumentStateOwner(frame);
  return Resolve(frame, frame->GetLastCommittedURL(),
                 frame->GetLastCommittedOrigin(), owner, false);
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
    const url::Origin& origin,
    NavigationRequest* navigation) {
  return Nonce(GetAlloyEmbeddedStateForCandidate(frame, origin, navigation));
}

AlloyEmbeddedResolution GetAlloyEmbeddedStateForCandidate(
    RenderFrameHostImpl* frame,
    const url::Origin& origin,
    NavigationRequest* navigation) {
  if (!frame) {
    return {};
  }
  // A frame may have a different cross-document navigation pending while its
  // current document commits pushState. Only the actual caller's navigation
  // can select navigation authority; never infer it from the frame-tree node.
  if (navigation && !navigation->IsSameDocument()) {
    return GetAlloyEmbeddedStateForNavigation(navigation, origin);
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
