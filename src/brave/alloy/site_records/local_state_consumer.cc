// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "include/cef_app.h"
#include "include/cef_api_hash.h"
#include "include/cef_preference.h"
#include "include/cef_task.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <set>
#include <thread>
#include <vector>
#include <functional>
#include <utility>
#include <map>

namespace {
constexpr char kName[] = "alloy.bootstrap_probe";
constexpr char kDefault[] = "compiled-default";
constexpr char kSession[] = "memory-session-value";
int failures = 0;
void Check(bool value, const char* reason) {
  if (!value) {
    ++failures;
    std::fprintf(stderr, "FAIL local-state-%s\n", reason);
  }
}
class Quit final : public CefTask {
 public:
  void Execute() override { CefQuitMessageLoop(); }
 private:
  IMPLEMENT_REFCOUNTING(Quit);
};
class ContextReply final : public AlloyRequestContextCallback {
 public:
  explicit ContextReply(std::function<void(alloy_site_settings_result_t,CefRefPtr<CefRequestContext>)> done)
      : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result,CefRefPtr<CefRequestContext> context) override {
    done_(result,std::move(context));
  }
 private:
  std::function<void(alloy_site_settings_result_t,CefRefPtr<CefRequestContext>)> done_;
  IMPLEMENT_REFCOUNTING(ContextReply);
};
class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done):done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result) override {done_(result);}
 private:
  std::function<void(alloy_site_settings_result_t)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};
class App final : public CefApp, public CefBrowserProcessHandler,
                  public CefClient, public CefLifeSpanHandler, public CefRenderHandler {
 public:
  App(std::string mode,std::string encrypted,bool children,bool inventory=false)
      :mode_(std::move(mode)),children_(children),inventory_(inventory) {
    if (!children_) return;
    if (mode_=="legacy") {
      profile_=AlloyCreateProfile(encrypted,"LOCAL_STATE_CHILD_TEST_PASSWORD_78");
      Check(!!profile_,"explicit-encrypted-profile");
      kinds_={"legacy","memory","stateless-keep","stateless-cut","encrypted"};
    } else kinds_={"memory","stateless-keep","stateless-cut"};
  }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {return this;}
  CefRefPtr<CefRenderHandler> GetRenderHandler() override {return this;}
  void GetViewRect(CefRefPtr<CefBrowser>,CefRect& rect) override {rect=CefRect(0,0,800,600);}
  void OnPaint(CefRefPtr<CefBrowser>,PaintElementType,const RectList&,const void*,int,int) override {}
  void OnRegisterCustomPreferences(
      cef_preferences_type_t type,
      CefRawPtr<CefPreferenceRegistrar> registrar) override {
    if (type != CEF_PREFERENCES_TYPE_GLOBAL) return;
    auto value = CefValue::Create();
    value->SetString(kDefault);
    Check(registrar->AddPreference(kName, value), "registered-default");
  }
  void OnContextInitialized() override {
    Check(CefCurrentlyOn(TID_UI), "ui-owner");
    auto manager = CefPreferenceManager::GetGlobalPreferenceManager();
    Check(manager && manager->HasPreference(kName), "native-global-owner");
    if (!manager || !manager->HasPreference(kName)) {CefPostTask(TID_UI,new Quit);return;}
    if (manager) {
      auto before = manager->GetPreference(kName);
      Check(before && before->GetType() == VTYPE_STRING &&
            before->GetString() == kDefault, "plaintext-seed-not-loaded");
      auto value = CefValue::Create();
      value->SetString(kSession);
      CefString error;
      Check(manager->SetPreference(kName, value, error), "live-set");
      auto after = manager->GetPreference(kName);
      Check(after && after->GetString() == kSession, "native-live-readback");
    }
    if (!children_) {CefPostTask(TID_UI,new Quit);return;}
    manager_=manager;
    if (inventory_) SnapshotPreferences("before");  // LOCAL87_PREF_INVENTORY
    previous_id_=SessionValue();
    std::puts("LOCAL_STATE_EVENT {\"event\":\"start-children\"}");
    NextContext();
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Check(CefCurrentlyOn(TID_UI),"child-ui-owner");
    const auto value=SessionValue();
    Check(!value.empty()&&value!=previous_id_,"native-session-pref-advanced");
    Check(session_values_.insert(value).second,"native-session-allocation-unique");
    Check(browser->GetIdentifier()>0&&browser_ids_.insert(browser->GetIdentifier()).second,"native-browser-unique");
    previous_id_=value;
    Check(manager_->GetPreference(kName)->GetString()==kSession,"global-value-survives-child");
    ++created_;
    std::printf("LOCAL_STATE_EVENT {\"event\":\"created\",\"kind\":\"%s\",\"browser\":%d,\"session_pref\":\"%s\"}\n",
        kinds_[kind_].c_str(),browser->GetIdentifier(),value.c_str());
    browser->GetHost()->CloseBrowser(true);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    ++closed_;
    std::printf("LOCAL_STATE_EVENT {\"event\":\"closed\",\"browser\":%d}\n",browser->GetIdentifier());
    if (++peer_<2) CreateChild();
    else {peer_=0;++kind_;NextContext();}
  }
  void ReleaseRuntime() {
    contexts_.clear();manager_=nullptr;
    std::thread release([profile=std::move(profile_)]() mutable {profile=nullptr;});release.join();
  }
 private:
  // LOCAL87_PREF_INVENTORY: test metadata only, never print preference values.
  using PreferenceSnapshot=std::map<std::string,CefRefPtr<CefValue>>;
  static std::string HexName(const std::string& name) {
    constexpr char digits[]="0123456789abcdef";
    std::string encoded;
    for (char c:name) {
      const auto byte=static_cast<unsigned char>(c);
      encoded+=digits[byte>>4];encoded+=digits[byte&15];
    }
    return encoded;
  }
  void CollectPreferences(CefRefPtr<CefDictionaryValue> dictionary,
                          const std::string& prefix,PreferenceSnapshot& values) {
    Check(!!dictionary,"inventory-dictionary");if (!dictionary) return;
    std::vector<CefString> keys;
    Check(dictionary->GetKeys(keys),"inventory-keys");
    for (const auto& key:keys) {
      const auto name=prefix+key.ToString();
      if (manager_->HasPreference(name)) {
        auto value=manager_->GetPreference(name);
        Check(!!value,"inventory-native-value");
        Check(values.emplace(name,std::move(value)).second,"inventory-unique-name");
      } else {
        auto value=dictionary->GetValue(key);
        const bool nested=value&&value->GetType()==VTYPE_DICTIONARY;
        Check(nested,"inventory-registered-hierarchy");
        if (nested) CollectPreferences(value->GetDictionary(),name+".",values);
      }
    }
  }
  void SnapshotPreferences(const char* phase) {
    PreferenceSnapshot values;
    CollectPreferences(manager_->GetAllPreferences(true),"",values);
    Check(values.count(kName)==1&&values.count("session_id_generator_last_value")==1,
          "inventory-native-controls");
    for (const auto& [name,value]:values) {
      if (!value) continue;
      std::printf("LOCAL_STATE_PREF {\"phase\":\"%s\",\"name_hex\":\"%s\",\"type\":%d,\"can_set\":%s}\n",
          phase,HexName(name).c_str(),static_cast<int>(value->GetType()),
          manager_->CanSetPreference(name)?"true":"false");
    }
    if (std::string(phase)=="before") initial_preferences_=std::move(values);
    else {
      for (const auto& [name,value]:initial_preferences_) {
        auto after=values.find(name);
        if (after==values.end()||!value||!value->IsEqual(after->second))
          std::printf("LOCAL_STATE_PREF_CHANGED {\"name_hex\":\"%s\",\"kind\":\"changed-or-removed\"}\n",HexName(name).c_str());
      }
      for (const auto& [name,value]:values) {
        if (!initial_preferences_.contains(name))
          std::printf("LOCAL_STATE_PREF_CHANGED {\"name_hex\":\"%s\",\"kind\":\"added\"}\n",HexName(name).c_str());
      }
    }
  }
  std::string SessionValue() {
    Check(manager_&&manager_->HasPreference("session_id_generator_last_value"),"native-session-pref-registered");
    auto value=manager_?manager_->GetPreference("session_id_generator_last_value"):nullptr;
    Check(value&&value->GetType()==VTYPE_STRING,"native-session-int64-encoding");
    return value&&value->GetType()==VTYPE_STRING?value->GetString().ToString():std::string();
  }
  void NextContext() {
    if (kind_==kinds_.size()) {Finish();return;}
    const auto& kind=kinds_[kind_];
    if (kind=="encrypted") {
      if (!profile_) {Check(false,"encrypted-owner-present");Finish();return;}
      const bool queued=AlloyCreateEncryptedRequestContext(profile_,nullptr,new ContextReply(
          [self=CefRefPtr<App>(this)](auto result,auto context) {
            Check(result==ALLOY_SETTINGS_OK&&!!context,"encrypted-native-context");
            self->UseContext(std::move(context));
          }));
      Check(queued,"encrypted-context-queued");if (!queued) Finish();
    } else if (kind=="legacy") UseContext(CefRequestContext::GetGlobalContext());
    else if (kind=="memory") UseContext(AlloyCreateMemoryRequestContext(nullptr));
    else UseContext(AlloyCreateStatelessRequestContext(kind=="stateless-cut"?ALLOY_NETWORK_STATE_CUT:ALLOY_NETWORK_STATE_KEEP,nullptr));
  }
  void UseContext(CefRefPtr<CefRequestContext> context) {
    Check(!!context,"actual-request-context");
    if (!context) {Finish();return;}
    contexts_.push_back(std::move(context));CreateChild();
  }
  void CreateChild() {
    std::printf("LOCAL_STATE_EVENT {\"event\":\"creating\",\"kind\":\"%s\",\"peer\":%zu}\n",kinds_[kind_].c_str(),peer_);
    CefWindowInfo window;window.SetAsWindowless(0);window.runtime_style=CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    const bool queued=CefBrowserHost::CreateBrowser(window,this,"about:blank",settings,nullptr,contexts_.back());
    Check(queued,"native-browser-queued");if (!queued) Finish();
  }
  void Finish() {
    Check(created_==static_cast<int>(kinds_.size()*2)&&created_==closed_,"all-native-children-closed");
    if (inventory_) SnapshotPreferences("after");  // LOCAL87_PREF_INVENTORY
    std::printf("LOCAL_STATE_EVENT {\"event\":\"children-complete\",\"created\":%d,\"closed\":%d}\n",created_,closed_);
    if (profile_) {
      const bool queued=profile_->LockAsync(new LockReply([self=CefRefPtr<App>(this)](auto result) {
        Check(result==ALLOY_SETTINGS_OK&&self->profile_->GetState()==ALLOY_PROFILE_LOCKED,"encrypted-lock-complete");
        CefPostTask(TID_UI,new Quit);
      }));
      Check(queued,"encrypted-lock-queued");if (!queued) CefPostTask(TID_UI,new Quit);
    } else CefPostTask(TID_UI,new Quit);
  }
  const std::string mode_;
  const bool children_;
  const bool inventory_;  // LOCAL87_PREF_INVENTORY
  PreferenceSnapshot initial_preferences_;  // LOCAL87_PREF_INVENTORY
  std::vector<std::string> kinds_;
  size_t kind_=0,peer_=0;
  int created_=0,closed_=0;
  std::string previous_id_;
  std::set<std::string> session_values_;
  std::set<int> browser_ids_;
  CefRefPtr<CefPreferenceManager> manager_;
  CefRefPtr<AlloyProfile> profile_;
  std::vector<CefRefPtr<CefRequestContext>> contexts_;
  IMPLEMENT_REFCOUNTING(App);
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout,nullptr,_IOLBF,0);
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 2;
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* root = std::getenv("ALLOY_LOCAL_STATE_RUNTIME");
  const char* mode = std::getenv("ALLOY_LOCAL_STATE_MODE");
  if (!root || !mode) return 2;
  const std::string owned_root(root), owned_mode(mode);
  if (owned_mode != "legacy" && owned_mode != "no-disk") return 2;
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  CefString(&settings.root_cache_path) = owned_root;
  if (owned_mode == "legacy") CefString(&settings.cache_path) = owned_root;
  const char* encrypted=std::getenv("ALLOY_LOCAL_STATE_ENCRYPTED");
  const char* child_mode=std::getenv("ALLOY_LOCAL_STATE_CHILDREN");
  const bool children=child_mode&&std::string(child_mode)=="1";
  if (children&&owned_mode=="legacy"&&!encrypted) return 2;
  const char* inventory_mode=std::getenv("ALLOY_LOCAL_STATE_PREF_INVENTORY");
  const bool inventory=inventory_mode&&std::string(inventory_mode)=="1";
  CefRefPtr<App> app = new App(owned_mode,encrypted?encrypted:"",children,inventory);
  if (!CefInitialize(args, settings, app, nullptr)) return 3;
  CefRunMessageLoop();
  app->ReleaseRuntime();
  CefDoMessageLoopWork();
  app = nullptr;
  CefShutdown();
  if (failures) return 1;
  std::printf("PASS memory-local-state-%s\n", owned_mode.c_str());
  return 0;
}
