// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/browser/cosmetic_filters/profile_cosmetic_filters_resources.h"
#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/cosmetic_filters/browser/cosmetic_filters_resources.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/document_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
namespace cosmetic_filters {
namespace {
using Native=brave_shields::AdBlockService;
using Wrapper=brave_shields::AdBlockEngineWrapper;
bool CanQuery(content::RenderFrameHost& frame){
  using State=content::RenderFrameHost::LifecycleState;
  return content::AlloyDocumentStateAdmitted(&frame)&&
      (frame.IsActive()||frame.IsInLifecycleState(State::kPendingCommit)||frame.IsInLifecycleState(State::kPrerendering));
}
class ProfileResources final:public content::DocumentService<mojom::CosmeticFiltersResources>,
    public content::WebContentsObserver {
 public:
  ProfileResources(content::RenderFrameHost& frame,
      mojo::PendingReceiver<mojom::CosmeticFiltersResources> receiver,
      brave_shields::AdBlockServiceLease original)
      :DocumentService(frame,std::move(receiver)),WebContentsObserver(content::WebContents::FromRenderFrameHost(&frame)),original_(std::move(original)),
       document_(content::AlloyDocumentStateOwner(&frame)) {
    close_=original_.Get()->ObserveClose(base::BindRepeating(&ProfileResources::Retire,weak_.GetWeakPtr()));
  }
  void HiddenClassIdSelectors(const std::string& input,const std::vector<std::string>& exceptions,
      HiddenClassIdSelectorsCallback callback) override {
    Dispatch(base::BindOnce([](std::string input,std::vector<std::string> exceptions,Wrapper* engine){
      // Native formatting is synchronous. No receiver retains this engine view.
      ::cosmetic_filters::CosmeticFiltersResources native(engine);base::Value result;
      native.HiddenClassIdSelectors(input,exceptions,base::BindOnce(
          [](base::Value* result,base::DictValue selectors){*result=base::Value(std::move(selectors));},&result));
      return result;
    },input,exceptions),base::BindOnce([](HiddenClassIdSelectorsCallback callback,base::Value result){
      std::move(callback).Run(std::move(result).TakeDict());
    },std::move(callback)));
  }
  void UrlCosmeticResources(const std::string& url,bool aggressive,UrlCosmeticResourcesCallback callback) override {
    Dispatch(base::BindOnce([](std::string url,bool aggressive,Wrapper* engine){
      ::cosmetic_filters::CosmeticFiltersResources native(engine);base::Value result;
      native.UrlCosmeticResources(url,aggressive,base::BindOnce(
          [](base::Value* result,base::Value resources){*result=std::move(resources);},&result));
      return result;
    },url,aggressive),std::move(callback));
  }
 private:
  void RenderFrameHostChanged(content::RenderFrameHost* old,content::RenderFrameHost*) override {
    if(old==&render_frame_host())Retire();
  }
  void RenderFrameHostStateChanged(content::RenderFrameHost* frame,
      content::RenderFrameHost::LifecycleState,content::RenderFrameHost::LifecycleState next) override {
    using State=content::RenderFrameHost::LifecycleState;
    if(frame==&render_frame_host()&&(next==State::kInBackForwardCache||next==State::kPendingDeletion))Retire();
  }
  Native* Get(){
    auto& frame=render_frame_host();
    return CanQuery(frame)&&
        content::AlloyStateOwnerForContext(document_,frame.GetBrowserContext())?original_.Get():nullptr;
  }
  void Retire(){ResetAndDeleteThis();}
  void Dispatch(base::OnceCallback<base::Value(Wrapper*)> task,base::OnceCallback<void(base::Value)> reply){
    auto* service=Get();if(!service){Retire();return;}
    if(!service->AsyncCallAndReplyWithResult<base::Value>(std::move(task),base::BindOnce(
        [](base::WeakPtr<ProfileResources> self,base::OnceCallback<void(base::Value)> reply,base::Value result){
          if(!self)return;
          if(!self->Get()){self->Retire();return;}
          std::move(reply).Run(std::move(result));
        },weak_.GetWeakPtr(),std::move(reply))))Retire();
  }
  const brave_shields::AdBlockServiceLease original_;
  const base::WeakPtr<base::SupportsUserData> document_;
  base::CallbackListSubscription close_;
  base::WeakPtrFactory<ProfileResources> weak_{this};
};
}
void BindProfileCosmeticFiltersResources(content::RenderFrameHost& frame,
    mojo::PendingReceiver<mojom::CosmeticFiltersResources> receiver){
  if(!CanQuery(frame))return;
  brave_shields::AdBlockServiceLease original(*frame.GetBrowserContext());if(!original.Get())return;
  new ProfileResources(frame,std::move(receiver),std::move(original));
}
}
