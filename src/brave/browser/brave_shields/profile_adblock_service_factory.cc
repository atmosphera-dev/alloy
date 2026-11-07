// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include <algorithm>
#include <vector>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/supports_user_data.h"
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "brave/alloy/site_records/profile_adblock_service.h"
#include "brave/alloy/site_records/profile_subscription_fetch.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/browser/brave_browser_process.h"
#include "chrome/browser/browser_process.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
namespace brave_shields {
namespace {
using Input=alloy::site_records::ProfileFilterPreferences;
using Policy=alloy::site_records::RequestContextPolicy;
using Configuration=alloy::site_records::NativeProfileConfiguration;
const char kContextOwner=0,kInputMemo=0;
bool Admitted(content::BrowserContext& context) {
  const auto* policy=Policy::Get(context);if(!policy)return false;
  const auto* configuration=Configuration::Get(context);
  return (!configuration||configuration->admitted())&&
      policy->StorageCapability(alloy::site_records::StorePurpose::kProfileCustomFilters).active();
}
class Owner final:public base::RefCountedDeleteOnSequence<Owner> {
 public:
  explicit Owner(scoped_refptr<Input> input):base::RefCountedDeleteOnSequence<Owner>(base::SequencedTaskRunner::GetCurrentDefault()),input_(std::move(input)) {}
  bool Initialize() {
    native_=alloy::site_records::ProfileAdBlockService::Create(input_,
        g_browser_process->GetApplicationLocale(),g_browser_process->component_updater(),
        base::BindRepeating([](base::WeakPtr<Owner> self,const GURL& url,bool ui,
            AdBlockSubscriptionFiltersProvider::SourceReply done)->base::OnceClosure {
          if(!self){std::move(done).Run(AdBlockSubscriptionFiltersProvider::SourceResult::kCancelled,{});return base::DoNothing();}
          return self->Fetch(url,ui,std::move(done));
        },weak_.GetWeakPtr()));
    if(!accepting_&&native_)native_->Close(base::DoNothing());
    return accepting_&&native_&&native_->Get();
  }
  void Attach(content::BrowserContext& context){contexts_.push_back({&context,context.GetWeakPtr()});}
  void Detach(content::BrowserContext* key) {
    std::erase_if(contexts_,[&](const Context& c){return c.key==key;});
    if(contexts_.empty()) {
      accepting_=false;
      if(native_&&!native_->Close(base::DoNothing()))input_->ConsumerFailed(alloy::site_records::FileStatus::kCancelled);
    }
  }
  bool AcceptsContext() const {return accepting_&&input_->available();}
  AdBlockService* Get(){return AcceptsContext()&&HasContext()&&native_?native_->Get():nullptr;}
  PrefService* Prefs(){return Get()?native_->prefs():nullptr;}
  base::WeakPtr<Owner> weak(){return weak_.GetWeakPtr();}
 private:
  friend class base::RefCountedDeleteOnSequence<Owner>;
  friend class base::DeleteHelper<Owner>;
  ~Owner()=default;
  struct Context {content::BrowserContext* key;base::WeakPtr<content::BrowserContext> weak;};
  content::BrowserContext* HasContext() {
    for(const auto& context:contexts_)if(context.weak&&Admitted(*context.weak))return context.weak.get();
    return nullptr;
  }
  base::OnceClosure Fetch(const GURL& url,bool ui,AdBlockSubscriptionFiltersProvider::SourceReply done) {
    scoped_refptr<Owner> alive(this);auto* context=HasContext();
    if(!context||!input_->available()) {
      std::move(done).Run(AdBlockSubscriptionFiltersProvider::SourceResult::kCancelled,{});return base::DoNothing();
    }
    auto factory=context->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess();
    return alloy::site_records::MakeProfileSubscriptionFetch(std::move(factory),
        base::BindRepeating(&Input::available,input_)).Run(url,ui,std::move(done));
  }
  const scoped_refptr<Input> input_;
  std::vector<Context> contexts_;
  std::unique_ptr<alloy::site_records::ProfileAdBlockService> native_;
  bool accepting_=true;
  base::WeakPtrFactory<Owner> weak_{this};
};
class ContextData final:public base::SupportsUserData::Data {
 public:
  ContextData(content::BrowserContext& context,scoped_refptr<Owner> owner):key_(&context),owner(std::move(owner)){this->owner->Attach(context);}
  ~ContextData() override {owner->Detach(key_);}
  content::BrowserContext* const key_;
  const scoped_refptr<Owner> owner;
};
class Memo final:public base::SupportsUserData::Data {
 public:
  explicit Memo(base::WeakPtr<Owner> owner):owner(std::move(owner)){}
  base::WeakPtr<Owner> owner;
};
scoped_refptr<Owner> Lookup(content::BrowserContext& context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if(!Admitted(context)||!g_browser_process||!g_brave_browser_process)return nullptr;
  if(auto* data=static_cast<ContextData*>(context.GetUserData(&kContextOwner)))return data->owner;
  auto input=Input::Get(context);if(!input)input=Input::EnsureTransient(context);
  if(!input)return nullptr;
  scoped_refptr<Owner> owner;
  if(auto* memo=static_cast<Memo*>(input->GetUserData(&kInputMemo))) {
    if(memo->owner&&memo->owner->AcceptsContext())owner=base::WrapRefCounted(memo->owner.get());
  }
  auto weak_context=context.GetWeakPtr();
  if(!owner) {
    owner=base::MakeRefCounted<Owner>(input);
    // Publish the construction descriptor before callbacks can reenter lookup.
    // A reentrant lookup sees this same not-yet-ready owner, never a replacement.
    context.SetUserData(&kContextOwner,std::make_unique<ContextData>(context,owner));
    input->SetUserData(&kInputMemo,std::make_unique<Memo>(owner->weak()));
    if(!owner->Initialize()||!weak_context||!input->available())return nullptr;
    return owner;
  }
  if(!weak_context||!input->available())return nullptr;
  context.SetUserData(&kContextOwner,std::make_unique<ContextData>(context,owner));
  input->SetUserData(&kInputMemo,std::make_unique<Memo>(owner->weak()));
  return owner;
}
bool Legacy(content::BrowserContext& context) {
  // Nullable ContextMode is ordinary legacy CEF by the existing adapter
  // contract. A declared encrypted configuration without its policy is refused.
  return !Policy::Get(context)&&!Configuration::Get(context);
}
}  // namespace
AdBlockService* AdBlockServiceForContext(content::BrowserContext& context) {
  if(Legacy(context))return g_brave_browser_process?g_brave_browser_process->ad_block_service():nullptr;
  auto owner=Lookup(context);return owner?owner->Get():nullptr;
}
PrefService* AdBlockPrefsForContext(content::BrowserContext& context) {
  if(Legacy(context))return g_browser_process?g_browser_process->local_state():nullptr;
  auto owner=Lookup(context);return owner?owner->Prefs():nullptr;
}
AdBlockServiceLease::AdBlockServiceLease(content::BrowserContext& context):context_(context.GetWeakPtr()) {
  if(auto* service=AdBlockServiceForContext(context))if(!service->IsClosed())service_=service->GetWeakPtr();
}
AdBlockService* AdBlockServiceLease::Get() const {
  if(!context_||!service_||service_->IsClosed())return nullptr;
  return AdBlockServiceForContext(*context_)==service_.get()?service_.get():nullptr;
}
base::RepeatingCallback<AdBlockService*()> AdBlockServiceLease::Getter() const {
  // Own the weak lease value, rather than binding a member function to a stack
  // receiver or turning it into a borrowed/unretained pointer.
  return base::BindRepeating([](AdBlockServiceLease original){return original.Get();},*this);
}
}  // namespace brave_shields
