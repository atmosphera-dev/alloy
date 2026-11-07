// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// SDK-only lifecycle/browser consumer. Native implementation types stay behind CEF.
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
namespace {
constexpr char kPassword[]="ADBLOCK_PROFILE_PASSWORD_133";
constexpr char kRule[]="adblock-private-resource-133.js$important";
constexpr char kBetaRule[]="adblock-beta-private-resource-140.js$important";
constexpr char kRefusedRule[]="adblock-refused-pending-resource-158.js$important";
void Check(bool ok,const char* detail){std::fprintf(stderr,"ADBLOCK_CHECK ok=%d operation=%s\n",ok,detail);std::fflush(stderr);if(!ok){std::fprintf(stderr,"ADBLOCK_FAIL %s\n",detail);std::abort();}}
class ContextReply final:public AlloyRequestContextCallback {
 public:
  using Done=std::function<void(alloy_site_settings_result_t,CefRefPtr<CefRequestContext>)>;
  explicit ContextReply(Done done):done_(std::move(done)){}
  void OnComplete(alloy_site_settings_result_t status,CefRefPtr<CefRequestContext> context) override {done_(status,context);}
 private:const Done done_;IMPLEMENT_REFCOUNTING(ContextReply);
};
class LockReply final:public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done):done_(std::move(done)){}
  void OnComplete(alloy_site_settings_result_t status) override {done_(status);}
 private:const std::function<void(alloy_site_settings_result_t)> done_;IMPLEMENT_REFCOUNTING(LockReply);
};
class Consumer final:public CefApp,public CefBrowserProcessHandler,public CefClient,
    public CefRenderHandler,public CefLifeSpanHandler,public CefLoadHandler,public CefDisplayHandler {
 public:
  Consumer(std::string phase,std::string alpha,std::string beta,std::string fixture)
      :phase_(std::move(phase)),fixture_(std::move(fixture)) {
    if(phase_=="modes")return;
    profiles_[0]=(phase_=="write"||phase_=="isolation")?AlloyCreateProfile(alpha,kPassword):AlloyOpenProfile(alpha);
    Check(!!profiles_[0],"alpha create/open");
    if(phase_=="locked"||phase_=="wrong") {
      if(phase_=="wrong")Check(!profiles_[0]->UnlockWithPassword("wrong-password"),"wrong password refused");
    }else {
      if(phase_!="write"&&phase_!="isolation")Check(profiles_[0]->UnlockWithPassword(kPassword),"alpha unlock");
      profiles_[1]=(phase_=="write"||phase_=="isolation")?AlloyCreateProfile(beta,kPassword):AlloyOpenProfile(beta);
      Check(!!profiles_[1],"beta create/open");
      if(phase_!="write"&&phase_!="isolation")Check(profiles_[1]->UnlockWithPassword(kPassword),"beta unlock");
    }
  }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override{return this;}
  CefRefPtr<CefRenderHandler> GetRenderHandler() override{return this;}
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override{return this;}
  CefRefPtr<CefLoadHandler> GetLoadHandler() override{return this;}
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override{return this;}
  void GetViewRect(CefRefPtr<CefBrowser>,CefRect& rect) override{rect=CefRect(0,0,1000,700);}
  void OnPaint(CefRefPtr<CefBrowser>,PaintElementType,const RectList&,const void*,int,int) override{}
  void OnContextInitialized() override {
    if(phase_=="modes"){StartModes();return;}
    if(phase_=="isolation"){CreateIsolationContext(0);CreateIsolationContext(1);return;}
    if(phase_=="locked"||phase_=="wrong") {
      Check(AlloyCreateEncryptedRequestContext(profiles_[0],nullptr,new ContextReply([](auto status,auto context){
        Check(status==ALLOY_SETTINGS_CANCELLED&&!context,"locked/wrong context refused");
        std::puts("ADBLOCK_EVENT {\"event\":\"refused\"}");CefQuitMessageLoop();})),"refusal queued");
      return;
    }
    CreateContext(0);CreateContext(1);
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Check(browser->GetHost()->GetRuntimeStyle()==CEF_RUNTIME_STYLE_ALLOY,"OSR Alloy runtime");
    int owner=contexts_[0]&&browser->GetHost()->GetRequestContext()->IsSharingWith(contexts_[0])?0:1;
    Check(contexts_[owner]&&browser->GetHost()->GetRequestContext()->IsSharingWith(contexts_[owner]),"original browser context");
    owners_[browser->GetIdentifier()]=owner;browsers_.push_back(browser);++created_[owner];
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {++closed_[owners_.at(browser->GetIdentifier())];}
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,CefRefPtr<CefFrame> frame,int) override {
    if(!frame->IsMain())return;
    const auto url=frame->GetURL().ToString();const int owner=owners_.at(browser->GetIdentifier());
    if(url.starts_with(fixture_)) {
      // A native mutation rebuilds asynchronously. Synchronize on the actual
      // consumer outcome with a bounded predicate, not a guessed sleep.
      std::string script="(async()=>{try{const control=await fetch('/adblock-public-control.js?nonce='+Math.random(),{cache:'no-store'});if(!control.ok)throw Error('control');}catch{document.title='ALLOY_ADBLOCK:network:bad';return;}const allowed=async path=>{try{return (await fetch(path+'?owner="+std::to_string(owner)+"&nonce='+Math.random(),{cache:'no-store'})).ok;}catch{return false;}};const end=performance.now()+10000;for(;;){const alpha=await allowed('/adblock-private-resource-133.js');const beta=await allowed('/adblock-beta-private-resource-140.js');";
      script+="if("+std::string(owner==0?"!alpha&&beta":"alpha&&!beta")+"){document.title='ALLOY_ADBLOCK:network:ok';return;}if(performance.now()>end){document.title='ALLOY_ADBLOCK:network:bad';return;}await new Promise(r=>requestAnimationFrame(r));}})()";
      frame->ExecuteJavaScript(script,url,0);
    }
  }
  void OnTitleChange(CefRefPtr<CefBrowser> browser,const CefString& title) override {
    const auto text=title.ToString();const int owner=owners_.at(browser->GetIdentifier());
    if(text=="ALLOY_ADBLOCK:network:bad")Check(false,"native network consumer outcome");
    if(text=="ALLOY_ADBLOCK:network:ok") {
      std::printf("ADBLOCK_EVENT {\"event\":\"network\",\"owner\":%d,\"after_alpha_lock\":%s}\n",owner,alpha_locked_?"true":"false");
      if(alpha_locked_){Check(owner==1,"only peer remains after alpha lock");LockBeta();return;}
      network_[owner]=true;if(network_[0]&&network_[1]&&!locking_){locking_=true;LockAlpha();}
    }
  }
  void ReleaseRuntime(){
    browsers_.clear();contexts_[0]=nullptr;contexts_[1]=nullptr;
    for(auto& context:modes_)context=nullptr;
    std::thread release([self=CefRefPtr<Consumer>(this)]{self->profiles_[0]=nullptr;self->profiles_[1]=nullptr;});release.join();
  }
 private:
  void RecordLock(int owner,alloy_site_settings_result_t status){
    const auto context=contexts_[owner];
    std::printf("ADBLOCK_LOCK_CALLBACK {\"case\":\"%s\",\"owner\":%d,\"status\":%d,\"profile_state\":%d,\"context_present\":%d,\"context_global\":%d,\"cache_path_empty\":%d,\"browsers_created\":%d,\"browsers_closed\":%d}\n",
        phase_.c_str(),owner,static_cast<int>(status),static_cast<int>(profiles_[owner]->GetState()),!!context,context?context->IsGlobal():false,context?context->GetCachePath().empty():true,created_[owner],closed_[owner]);
    std::fflush(stdout);
  }
  void CreateIsolationContext(int owner){
    Check(profiles_[owner]->GetState()==ALLOY_PROFILE_UNLOCKED,"isolation profile unlocked");
    Check(AlloyCreateEncryptedRequestContext(profiles_[owner],nullptr,new ContextReply(
        [self=CefRefPtr<Consumer>(this),owner](auto status,auto context){
          std::printf("ADBLOCK_ISOLATION context owner=%d status=%d present=%d\n",owner,status,!!context);
          Check(status==ALLOY_SETTINGS_OK,"isolation context creation status");Check(!!context,"isolation context present");
          Check(!context->IsGlobal()&&context->GetCachePath().empty(),"isolation context has no ordinary backing");
          self->contexts_[owner]=context;
          if(self->contexts_[0]&&self->contexts_[1])self->RunIsolation();
        })),"isolation context creation queued");
  }
  void RunIsolation(){
    Check(!contexts_[0]->IsSharingWith(contexts_[1]),"isolation contexts are distinct");
    CefString alpha,beta;
    Check(AlloyGetAdBlockCustomFilters(contexts_[0],alpha),"isolation initial alpha read admitted");Check(alpha.empty(),"isolation initial alpha empty");
    Check(AlloyGetAdBlockCustomFilters(contexts_[1],beta),"isolation initial beta read admitted");Check(beta.empty(),"isolation initial beta empty");
    Check(AlloySetAdBlockCustomFilters(contexts_[0],kRule),"isolation alpha edit admitted");
    Check(AlloyGetAdBlockCustomFilters(contexts_[0],alpha),"isolation alpha readback admitted");Check(alpha==kRule,"isolation alpha positive readback");
    std::puts("ADBLOCK_EVENT {\"event\":\"isolation-positive\",\"owner\":0}");
    Check(AlloyGetAdBlockCustomFilters(contexts_[1],beta),"isolation beta read admitted");
    std::printf("ADBLOCK_ISOLATION beta_empty=%d beta_equals_alpha=%d\n",beta.empty(),beta==kRule);
    Check(beta.empty(),"encrypted-filter-isolation alpha-not-visible-to-beta");
    Check(AlloySetAdBlockCustomFilters(contexts_[1],kBetaRule),"isolation beta edit admitted");
    Check(AlloyGetAdBlockCustomFilters(contexts_[1],beta),"isolation beta readback admitted");Check(beta==kBetaRule,"isolation beta positive readback");
    Check(AlloyGetAdBlockCustomFilters(contexts_[0],alpha),"isolation alpha peer-check admitted");Check(alpha==kRule,"encrypted-filter-isolation beta-cannot-overwrite-alpha");
    Check(profiles_[0]->LockAsync(new LockReply([self=CefRefPtr<Consumer>(this)](auto status){
      self->RecordLock(0,status);
      Check(status==ALLOY_SETTINGS_OK,"isolation alpha lock completed");CefString text="stale";
      Check(!AlloyGetAdBlockCustomFilters(self->contexts_[0],text),"isolation locked alpha read refused");Check(text.empty(),"isolation locked alpha output cleared");
      Check(!AlloySetAdBlockCustomFilters(self->contexts_[0],"must-not-store"),"isolation locked alpha edit refused");
      Check(AlloyGetAdBlockCustomFilters(self->contexts_[1],text),"isolation peer remains admitted");Check(text==kBetaRule,"isolation peer remains isolated");
      self->VerifyPendingWriteWasNotPersisted();
    })),"isolation alpha lock queued");
    // No UI turn or lock completion has run since LockAsync returned. Refusal
    // must be explicit here, not a later ignored storage failure.
    const bool accepted=AlloySetAdBlockCustomFilters(contexts_[0],kRefusedRule);
    std::printf("ADBLOCK_PENDING_WRITE accepted=%d state=%d\n",accepted,profiles_[0]->GetState());
    Check(!accepted,"isolation pending-lock writer explicitly refused");
    Check(profiles_[0]->GetState()==ALLOY_PROFILE_TRANSITION,"isolation write refused before lock acknowledgement");
    std::puts("ADBLOCK_EVENT {\"event\":\"lock-pending-writer-refused\"}");
  }
  void VerifyPendingWriteWasNotPersisted(){
    bool unlocked=false;
    std::thread unlock([&]{unlocked=profiles_[0]->UnlockWithPassword(kPassword);});unlock.join();
    Check(unlocked,"isolation alpha reopened after successful lock");
    const auto original=contexts_[0];
    Check(AlloyCreateEncryptedRequestContext(profiles_[0],nullptr,new ContextReply(
        [self=CefRefPtr<Consumer>(this),original](auto status,auto context){
          Check(status==ALLOY_SETTINGS_OK&&!!context,"isolation reopened context admitted");
          Check(!context->IsSharingWith(original),"isolation reopened context has fresh generation");
          CefString text="stale";
          Check(AlloyGetAdBlockCustomFilters(context,text),"isolation reopened filter read admitted");
          Check(text==kRule,"isolation accepted rule persisted without refused replacement");
          Check(!AlloySetAdBlockCustomFilters(original,kRefusedRule),"isolation original generation still refuses writes");
          self->contexts_[0]=context;
          Check(self->profiles_[0]->LockAsync(new LockReply([self](auto status){
            self->RecordLock(0,status);Check(status==ALLOY_SETTINGS_OK,"isolation reopened alpha lock completed");
            Check(self->profiles_[1]->LockAsync(new LockReply([self](auto status){self->RecordLock(1,status);Check(status==ALLOY_SETTINGS_OK,"isolation beta lock completed");std::puts("ADBLOCK_EVENT {\"event\":\"encrypted-filter-isolation\"}");CefQuitMessageLoop();})),"isolation beta lock queued");
          })),"isolation reopened alpha lock queued");
        })),"isolation reopened context creation queued");
  }
  void CreateContext(int owner){
    Check(AlloyCreateEncryptedRequestContext(profiles_[owner],nullptr,new ContextReply(
        [self=CefRefPtr<Consumer>(this),owner](auto status,auto context){
          Check(status==ALLOY_SETTINGS_OK&&context&&!context->IsGlobal()&&context->GetCachePath().empty(),"encrypted context admitted without ordinary backing");
          self->contexts_[owner]=context;
          CefString value="stale";
          if(self->phase_=="write") {
            Check(AlloyGetAdBlockCustomFilters(context,value)&&value.empty(),"new profile filters empty");
            Check(AlloySetAdBlockCustomFilters(context,owner==0?kRule:kBetaRule),"native filter edit admitted");
          }
          Check(AlloyGetAdBlockCustomFilters(context,value)&&value==(owner==0?kRule:kBetaRule),"original native filter readback");
          std::printf("ADBLOCK_EVENT {\"event\":\"filters\",\"owner\":%d}\n",owner);self->configured_[owner]=true;
          if(self->configured_[0]&&self->configured_[1]) {
            Check(AlloyGetAdBlockCustomFilters(self->contexts_[0],value)&&value==kRule,"peer mutation did not overwrite alpha");
            Check(AlloyGetAdBlockCustomFilters(self->contexts_[1],value)&&value==kBetaRule,"alpha mutation did not overwrite peer");
            self->CreateBrowser(0,self->fixture_+"/probe");self->CreateBrowser(1,self->fixture_+"/probe");
          }
        })),"creation queued");
  }
  void CreateBrowser(int owner,const std::string& url){
    CefWindowInfo window;window.SetAsWindowless(0);window.runtime_style=CEF_RUNTIME_STYLE_ALLOY;CefBrowserSettings settings;
    Check(CefBrowserHost::CreateBrowser(window,this,url,settings,nullptr,contexts_[owner]),"browser creation queued");
  }
  void LockAlpha(){
    Check(profiles_[0]->LockAsync(new LockReply([self=CefRefPtr<Consumer>(this)](auto status){
      self->RecordLock(0,status);
      Check(status==ALLOY_SETTINGS_OK&&self->profiles_[0]->GetState()==ALLOY_PROFILE_LOCKED,"alpha lock completion");
      Check(self->closed_[0]==self->created_[0],"all alpha browsers retired before acknowledgement");
      Check(!self->contexts_[0]->GetPreference("homepage"),"retained alpha native input denied");
      Check(!!self->contexts_[1]->GetPreference("homepage"),"peer native input remains admitted");
      CefString filters="stale";
      Check(!AlloyGetAdBlockCustomFilters(self->contexts_[0],filters)&&filters.empty(),"locked original filter read refused and cleared");
      Check(!AlloySetAdBlockCustomFilters(self->contexts_[0],"must-not-store"),"locked original filter edit refused");
      Check(AlloyGetAdBlockCustomFilters(self->contexts_[1],filters)&&filters==kBetaRule,"peer filter owner remains admitted");
      self->alpha_locked_=true;
      Check(AlloyCreateEncryptedRequestContext(self->profiles_[0],nullptr,new ContextReply(
          [self](auto result,auto context){
            Check(result==ALLOY_SETTINGS_CANCELLED&&!context,"new locked alpha context denied");
            for(auto browser:self->browsers_)if(self->owners_.at(browser->GetIdentifier())==1&&browser->GetMainFrame()->GetURL().ToString().starts_with(self->fixture_))
              browser->GetMainFrame()->LoadURL(self->fixture_+"/probe?after-lock");
          })),"post-lock refusal queued");
    })),"alpha lock queued");
  }
  void LockBeta(){
    if(beta_locking_)return;beta_locking_=true;
    Check(profiles_[1]->LockAsync(new LockReply([self=CefRefPtr<Consumer>(this)](auto status){
      self->RecordLock(1,status);
      Check(status==ALLOY_SETTINGS_OK&&self->closed_[1]==self->created_[1],"peer lock/native teardown complete");
      Check(!self->contexts_[1]->GetPreference("homepage"),"retained peer input denied");
      CefString filters="stale";
      Check(!AlloyGetAdBlockCustomFilters(self->contexts_[1],filters)&&filters.empty()&&!AlloySetAdBlockCustomFilters(self->contexts_[1],"must-not-store"),"retained peer filter read/edit refused");
      std::puts("ADBLOCK_EVENT {\"event\":\"locked\"}");CefQuitMessageLoop();})),"peer lock queued");
  }
  const std::string phase_,fixture_;
  class Initialized final:public CefRequestContextHandler {
   public:
    explicit Initialized(std::function<void(CefRefPtr<CefRequestContext>)> done):done_(std::move(done)){}
    void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override{done_(context);}
   private:const std::function<void(CefRefPtr<CefRequestContext>)> done_;IMPLEMENT_REFCOUNTING(Initialized);
  };
  void StartModes(){
    std::puts("ADBLOCK_CASE modes start");
    for(int owner=0;owner<3;++owner) {
      std::printf("ADBLOCK_CASE modes creating owner=%d\n",owner);
      CefRefPtr<Initialized> ready=new Initialized([self=CefRefPtr<Consumer>(this),owner](auto context){self->ModeReady(owner,context);});
      modes_[owner]=owner==2?AlloyCreateStatelessRequestContext(ALLOY_NETWORK_STATE_KEEP,ready):AlloyCreateMemoryRequestContext(ready);
      Check(!!modes_[owner],"mode context created");
    }
  }
  void ModeReady(int owner,CefRefPtr<CefRequestContext> context){
    std::printf("ADBLOCK_CASE modes initialized owner=%d\n",owner);
    CefString filters="stale";
    if(owner==2) {
      Check(!AlloyGetAdBlockCustomFilters(context,filters),"stateless native read refused");
      Check(filters.empty(),"stateless refused read cleared output");
      Check(!AlloySetAdBlockCustomFilters(context,kRule),"stateless native edit refused");
    }
    else {
      Check(AlloyGetAdBlockCustomFilters(context,filters),"new memory native read admitted");
      Check(filters.empty(),"new memory context empty");
      Check(AlloySetAdBlockCustomFilters(context,owner==0?kRule:kBetaRule),"memory filter edit admitted");
      Check(AlloyGetAdBlockCustomFilters(context,filters),"memory native readback admitted");
      Check(filters==(owner==0?kRule:kBetaRule),"memory native readback value");
      Check(AlloySetAdBlockCustomFilters(context,""),"empty native edit admitted");
      Check(AlloyGetAdBlockCustomFilters(context,filters),"read after empty edit admitted");
      Check(filters.empty(),"empty input clears native filters");
      Check(AlloySetAdBlockCustomFilters(context,owner==0?kRule:kBetaRule),"restore memory filter after clear");
    }
    mode_ready_[owner]=true;
    if(!mode_ready_[0]||!mode_ready_[1]||!mode_ready_[2])return;
    Check(AlloyGetAdBlockCustomFilters(modes_[0],filters),"memory alpha peer check admitted");
    Check(filters==kRule,"memory alpha isolated");
    Check(AlloyGetAdBlockCustomFilters(modes_[1],filters),"memory beta peer check admitted");
    Check(filters==kBetaRule,"memory beta isolated");
    std::thread wrong_thread([context=modes_[0]]{CefString value="stale";Check(!AlloyGetAdBlockCustomFilters(context,value),"wrong-thread native read refused");Check(value.empty(),"wrong-thread refused read cleared output");Check(!AlloySetAdBlockCustomFilters(context,"must-not-store"),"wrong-thread native edit refused");});wrong_thread.join();
    std::puts("ADBLOCK_EVENT {\"event\":\"memory-isolation-and-stateless-refusal\"}");CefQuitMessageLoop();
  }
  CefRefPtr<AlloyProfile> profiles_[2];CefRefPtr<CefRequestContext> contexts_[2];
  CefRefPtr<CefRequestContext> modes_[3];bool mode_ready_[3]{};
  std::vector<CefRefPtr<CefBrowser>> browsers_;std::map<int,int> owners_;
  int created_[2]{},closed_[2]{};bool configured_[2]{},network_[2]{};
  bool locking_=false,alpha_locked_=false,beta_locking_=false;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}
int main(int argc,char** argv){
  setvbuf(stdout,nullptr,_IOLBF,0);Check(cef_api_hash(CEF_API_VERSION,0)!=nullptr,"experimental API hash");
  CefMainArgs args(argc,argv);const int child=CefExecuteProcess(args,nullptr,nullptr);if(child>=0)return child;
  CefString refused="stale";
  Check(!AlloyGetAdBlockCustomFilters(nullptr,refused),"missing/uninitialized context native read refused");
  Check(refused.empty(),"missing/uninitialized read cleared output");
  Check(!AlloySetAdBlockCustomFilters(nullptr,"must-not-store"),"missing/uninitialized context native edit refused");
  const char* phase=std::getenv("ALLOY_ADBLOCK_PHASE");const char* alpha=std::getenv("ALLOY_ADBLOCK_ALPHA");
  const char* beta=std::getenv("ALLOY_ADBLOCK_BETA");const char* fixture=std::getenv("ALLOY_ADBLOCK_FIXTURE");Check(phase&&alpha&&beta&&fixture,"fixture inputs");
  CefRefPtr<Consumer> app=new Consumer(phase,alpha,beta,fixture);CefSettings settings;settings.windowless_rendering_enabled=true;
  Check(CefInitialize(args,settings,app,nullptr),"CEF initialize");CefRunMessageLoop();app->ReleaseRuntime();CefDoMessageLoopWork();CefShutdown();app=nullptr;
  std::puts("PASS encrypted-adblock-browser-consumer");return 0;
}
