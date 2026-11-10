// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Public SDK consumer: the browser owns mechanisms, this fixture owns tab UI.
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_values.h"
#include "include/cef_frame.h"
#include "include/cef_load_handler.h"
#include "brave/alloy/site_records/profile_control.h"
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const char* why) {
  if (!ok) { std::fprintf(stderr, "AUTOLOCK_FAIL %s\n", why); std::abort(); }
}
void Event(const char* event, int value = 0) {
  std::printf("AUTOLOCK_EVENT {\"event\":\"%s\",\"value\":%d}\n", event, value);
}
class ContextReply final : public AlloyRequestContextCallback {
 public:
  explicit ContextReply(std::function<void(CefRefPtr<CefRequestContext>)> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status, CefRefPtr<CefRequestContext> context) override {
    Check(status == ALLOY_SETTINGS_OK && context, "new original context admitted"); done_(context);
  }
 private:
  const std::function<void(CefRefPtr<CefRequestContext>)> done_;
  IMPLEMENT_REFCOUNTING(ContextReply);
};
class RetiredContextReply final : public AlloyRequestContextCallback {
 public:
  void OnComplete(alloy_site_settings_result_t status, CefRefPtr<CefRequestContext> context) override {
    Check(status == ALLOY_SETTINGS_CANCELLED && !context,
          "context queued before retirement cannot construct a late native owner");
    Event("late-context-refused");
  }
 private:
  IMPLEMENT_REFCOUNTING(RetiredContextReply);
};
class ListReply final : public AlloySiteSettingsCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t, const std::vector<CefString>&)>;
  explicit ListReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status, const std::vector<CefString>& values) override { done_(status, values); }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(ListReply);
};
class LockReply final : public AlloyProfileLockCallback {
 public:
  void OnComplete(alloy_site_settings_result_t status) override {
    Check(status == ALLOY_SETTINGS_OK, "explicit pending-navigation lock completed");
    Event("explicit-complete");
  }
 private:
  IMPLEMENT_REFCOUNTING(LockReply);
};
class RefusedDeletionReply final : public AlloyRecordDeletionCallback {
 public:
  void OnComplete(alloy_site_settings_result_t status, bool files_pending) override {
    Check(status == ALLOY_SETTINGS_CANCELLED && !files_pending, "pending-lock record deletion refused");
    Event("pending-mutation-refused");
  }
 private:
  IMPLEMENT_REFCOUNTING(RefusedDeletionReply);
};
class Consumer final : public CefApp, public CefBrowserProcessHandler, public CefClient,
                        public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler,
                       public AlloyProfileLockHandler {
 public:
  Consumer(CefRefPtr<AlloyProfile> profile, std::string root, std::string base, bool show_locked)
      : profile_(std::move(profile)), root_(std::move(root)), base_(std::move(base)), show_locked_(show_locked) {}
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   TransitionType) override {
    // CEF invokes this directly inside WebContentsObserver::DidFinishNavigation.
    if (navigation_callback_lock_ && frame->IsMain() && managed_ && browser->IsSame(managed_)) {
      navigation_callback_lock_ = false;
      LockFromCallback("navigation-callback-returned");
    }
  }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 800, 600); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnContextInitialized() override {
    Check(!profile_->SetAutoLock(-1, this), "negative idle timeout refused");
    Check(profile_->SetAutoLock(0, this), "auto-lock configured before native creation");
    if (std::getenv("ALLOY_AUTOLOCK_REOPEN")) { Context(true); return; }
    Check(!profile_->NotifyLockSignal(ALLOY_LOCK_IDLE), "embedder cannot fake idle reason");
    control_context_ = AlloyCreateStatelessRequestContext(ALLOY_NETWORK_STATE_KEEP, nullptr);
    Check(!!control_context_, "control has separate stateless context");
    Open(control_context_, base_+"/control");
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browsers_.push_back(browser);
    if (browser->GetHost()->GetRequestContext()->IsSharingWith(control_context_)) {
      if (!control_) { control_ = browser; Event("control-created"); }
      else Event("locked-page-created");
    } else { managed_ = browser; ++created_; Event("managed-created"); }
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    Check(!inside_lock_callback_, "native destruction waits for lock callback to unwind");
    if (managed_ && browser->IsSame(managed_)) { ++closed_; Event("managed-closed"); }
  }
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
    const auto text = title.ToString();
    const int result = profile_control_receive(&transport_, browser->GetIdentifier(), text.c_str(),
        [](void* self, const profile_control_request* request) {
          static_cast<Consumer*>(self)->Command(request->action, request->browser);
        },
        [](void*, const profile_control_request* request, int replayed) {
          std::printf("PROFILE_EVENT {\"event\":\"control-reply\",\"request_id\":\"%s\",\"token\":\"%s\","
              "\"browser\":%d,\"executions\":%u,\"phase\":\"%s\"}\n", request->id, request->action,
              request->browser, request->executions, request->complete ? (replayed ? "replayed" : "dispatched") : "pending");
        }, this);
    Check(result >= 0, "identified command");
  }
  void OnLockTriggersChanged(int available) override { Event("available", available); }
  void OnLocked(alloy_profile_lock_reason_t reason, alloy_site_settings_result_t status) override {
    Check(status == ALLOY_SETTINGS_OK, "lock acknowledged without error");
    Check(profile_->GetState() == ALLOY_PROFILE_LOCKED, "keys no longer admitted at callback");
    Check(created_ == closed_, "all original browser documents torn down before callback");
    Check(!context_->GetPreference("homepage"), "old native context refuses reads");
    Check(!context_->CanSetPreference("homepage"), "retired context cannot admit native preferences");
    if (reason != ALLOY_LOCK_BROWSER_CLOSE) {
      CefWindowInfo window; window.SetAsWindowless(0); window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
      CefBrowserSettings browser_settings;
      Check(!CefBrowserHost::CreateBrowserSync(window, this, base_+"/late-admission",
          browser_settings, nullptr, context_), "retained context cannot create late native services or documents");
    }
    Event("callback", reason);
    if (reason == ALLOY_LOCK_BROWSER_CLOSE) { shutdown_callback_ = true; return; }
    Check(profile_->GetFirstPartyEmbeds("https://alpha.test", new ListReply(
        [self = CefRefPtr<Consumer>(this), reason](auto result, const auto& values) {
          Check(result == ALLOY_SETTINGS_CANCELLED && values.empty(), "locked profile read refused");
          if (self->show_locked_) self->Open(self->control_context_, self->base_+"/locked");
          else self->tab_shell_open_ = false;
          Event("locked", reason);
        })), "locked read queued");
  }
  void AfterShutdown() {
    Check(shutdown_callback_, "browser-close callback delivered with retained profile");
    Check(profile_->GetState() == ALLOY_PROFILE_LOCKED, "CefShutdown wiped retained profile");
    if (!std::getenv("ALLOY_AUTOLOCK_REOPEN"))
      Check(late_ && late_->GetState() == ALLOY_PROFILE_LOCKED, "browser close adopted late standalone registration");
    browsers_.clear(); managed_ = nullptr; control_ = nullptr; context_ = nullptr; control_context_ = nullptr;
    late_ = nullptr; profile_control_destroy(&transport_);
  }
 private:
  void Open(CefRefPtr<CefRequestContext> context, const std::string& url) {
    CefWindowInfo window; window.SetAsWindowless(0); window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    Check(CefBrowserHost::CreateBrowser(window, this, url, settings, nullptr, context), "explicit fixture browser");
  }
  void Context(bool reopen) {
    Check(AlloyCreateEncryptedRequestContext(profile_, nullptr, new ContextReply(
        [self = CefRefPtr<Consumer>(this), reopen](CefRefPtr<CefRequestContext> context) {
          self->context_ = context;
          if (!reopen) {
            Check(AlloySetAdBlockCustomFilters(context, "||admitted-before-lock.test^"), "baseline filter edit admitted");
            CefRefPtr<CefValue> homepage = CefValue::Create();
            homepage->SetString("https://completed-before-lock.test/");
            CefString error;
            Check(context->SetPreference("homepage", homepage, error), "baseline preference edit admitted");
            Check(self->profile_->SetFirstPartyEmbeds("https://alpha.test", {"https://beta.test"}, new ListReply(
                [self](auto status, const auto&) {
                  Check(status == ALLOY_SETTINGS_OK, "setting durably written before lock");
                  self->Open(self->context_, self->base_+"/managed");
                })), "initial setting queued");
          } else {
            CefString filters;
            Check(AlloyGetAdBlockCustomFilters(context, filters) && filters == "||admitted-before-lock.test^",
                  "accepted filters persisted without pending-lock replacement");
            if (self->pending_write_ || std::getenv("ALLOY_AUTOLOCK_REOPEN")) {
              auto homepage = context->GetPreference("homepage");
              Check(homepage && homepage->GetString() == "https://completed-before-lock.test/",
                    "completed native preference write flushed before lock");
              Event("completed-write-readback");
            }
            if (std::getenv("ALLOY_AUTOLOCK_REOPEN")) { CefQuitMessageLoop(); return; }
            Check(AlloyGetFirstPartyEmbeds(context, "https://alpha.test", new ListReply(
                [self](auto status, const auto& values) {
                   Check(status == ALLOY_SETTINGS_OK && values.size() == 1 && values[0] == "beta.test",
                       "fresh context reads encrypted value only after unlock");
                   if (self->pending_mutations_) Event("pending-mutations-not-persisted");
                  self->Open(self->context_, self->base_+"/reopened"); Event("unlocked");
                })), "fresh generation read queued");
          }
        })), "encrypted context queued");
  }
  /// Start lock reentrantly and verify every public mutation refuses pending-transition admission.
  void LockFromCallback(const char* returned_event) {
    inside_lock_callback_ = true;
    Check(profile_->LockAsync(new LockReply), "first callback lock accepted");
    Check(created_ != closed_, "entry did not destroy the generation inline");
    Check(profile_->GetState() == ALLOY_PROFILE_TRANSITION, "mutation probes run before lock completion");
    Check(!AlloySetAdBlockCustomFilters(context_, "||refused-during-lock.test^"), "pending-lock filter replacement refused");
    CefRefPtr<CefValue> homepage = CefValue::Create(); homepage->SetString("https://refused-during-lock.test/");
    CefString error;
    Check(!context_->CanSetPreference("homepage") && !context_->SetPreference("homepage", homepage, error),
          "pending-lock public preference replacement refused");
    Check(!AlloySetEmbeddedStateScope(context_, "per-tab"), "pending-lock embedded-state edit refused");
    auto refused_list = [] {
      return new ListReply([](auto status, const auto& values) {
        Check(status == ALLOY_SETTINGS_CANCELLED && values.empty(), "pending-lock embed-list replacement refused");
        Event("pending-mutation-refused");
      });
    };
    Check(profile_->SetFirstPartyEmbeds("https://alpha.test", {"https://refused-during-lock.test"}, refused_list()),
          "profile embed-list refusal queued");
    Check(AlloySetFirstPartyEmbeds(context_, "https://alpha.test", {"https://refused-during-lock.test"}, refused_list()),
          "context embed-list refusal queued");
    Check(profile_->DeleteSiteRecord("https://alpha.test", new RefusedDeletionReply), "profile deletion refusal queued");
    Check(profile_->DeleteAllSiteRecords(new RefusedDeletionReply), "pending-lock delete-all refusal queued");
    pending_mutations_ = true; pending_write_ = true;
    Event("pending-writers-refused");
    Check(profile_->LockAsync(new LockReply), "second callback lock joins the first");
    inside_lock_callback_ = false;
    Event(returned_event);
  }
  void Command(const std::string& command, int browser_id) {
    if (command == "ready") Event("ready");
    else if (command == "start") Context(false);
    else if (command == "title-callback-lock") {
      Check(managed_ && managed_->GetIdentifier() == browser_id, "lock originated in managed OnTitleChange");
      LockFromCallback("title-callback-returned");
    } else if (command == "navigation-callback-lock") {
      navigation_callback_lock_ = true;
      managed_->GetMainFrame()->LoadURL(base_+"/callback-lock");
    }
    else if (command == "pending-navigation") {
      CefRefPtr<CefValue> value = CefValue::Create();
      value->SetString("https://completed-before-lock.test/");
      CefString error;
      Check(context_->SetPreference("homepage", value, error), "completed native write admitted before pending navigation");
      pending_write_ = true;
      managed_->GetMainFrame()->LoadURL(base_+"/pending");
      Event("pending-navigation-started");
    } else if (command == "explicit-lock") {
      Check(AlloyCreateEncryptedRequestContext(profile_, nullptr, new RetiredContextReply),
          "context preparation queued immediately before lock");
      Check(profile_->LockAsync(new LockReply), "explicit lock accepted during pending navigation");
    }
    else if (command == "arm-idle") Check(profile_->SetAutoLock(1500, this), "short test idle period");
    else if (command == "input") {
      CefKeyEvent input; input.type = KEYEVENT_RAWKEYDOWN; input.windows_key_code = 65;
      managed_->GetHost()->SendKeyEvent(input); Event("native-input");
    } else if (command == "embedder-input") {
      Check(profile_->NotifyUserActivity(), "trusted embedder input accepted"); Event("embedder-input");
    } else if (command == "embedder-sleep") {
      Check(profile_->NotifyLockSignal(ALLOY_LOCK_SLEEP), "trusted embedder sleep signal accepted");
    } else if (command == "unlock") {
      Check(profile_->SetAutoLock(0, this), "idle disabled while checking fresh unlock");
      bool unlocked = false;
      std::thread work([&] { unlocked = profile_->UnlockWithPassword("AUTOLOCK_PASSWORD"); });
      work.join(); Check(unlocked, "explicit password unlock");
      Check(tab_shell_open_ == show_locked_, "embedder chose close versus preserved locked shell"); Context(true);
    } else if (command == "finish") {
      const auto path = root_+"/late";
      std::thread create([&] {
        late_ = AlloyCreateProfile(path, "AUTOLOCK_PASSWORD");
        if (!late_) std::fprintf(stderr, "AUTOLOCK_LATE_CREATE %s errno=%d\n", path.c_str(), errno);
      });
      create.join(); Check(!!late_, "late standalone profile registered before loop quit");
      // Its posted UI adoption has not run; shutdown must adopt the registry.
      CefQuitMessageLoop();
    }
    else Check(false, "known auto-lock command");
  }
  CefRefPtr<AlloyProfile> profile_;
  CefRefPtr<AlloyProfile> late_;
  const std::string root_, base_;
  const bool show_locked_;
  bool tab_shell_open_ = true, shutdown_callback_ = false;
  bool pending_write_ = false;
  bool pending_mutations_ = false;
  bool navigation_callback_lock_ = false, inside_lock_callback_ = false;
  int created_ = 0, closed_ = 0;
  CefRefPtr<CefRequestContext> control_context_, context_;
  CefRefPtr<CefBrowser> control_, managed_;
  std::vector<CefRefPtr<CefBrowser>> browsers_;
  profile_control_transport transport_{};
  IMPLEMENT_REFCOUNTING(Consumer);
};
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "experimental API hash");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* root = std::getenv("ALLOY_AUTOLOCK_ROOT");
  const char* base = std::getenv("ALLOY_AUTOLOCK_BASE");
  const char* choice = std::getenv("ALLOY_AUTOLOCK_CHOICE");
  Check(root && base && choice, "explicit fixture inputs");
  const std::string root_path(root);
  const bool reopen = std::getenv("ALLOY_AUTOLOCK_REOPEN") != nullptr;
  auto profile = reopen ? AlloyOpenProfile(root_path+"/encrypted") :
      AlloyCreateProfile(root_path+"/encrypted", "AUTOLOCK_PASSWORD");
  Check(!!profile, "profile bootstrap before CEF");
  if (reopen) Check(profile->UnlockWithPassword("AUTOLOCK_PASSWORD"), "reopen browser-close profile with password");
  auto standalone = AlloyCreateProfile(root_path+(reopen ? "/standalone-reopen" : "/standalone"), "AUTOLOCK_PASSWORD");
  Check(!!standalone, "pre-CEF standalone profile with no context or auto-lock settings");
  CefRefPtr<Consumer> consumer = new Consumer(profile, root_path, base, std::string(choice) == "locked-page");
  CefSettings settings; settings.windowless_rendering_enabled = true;
  CefString(&settings.root_cache_path) = root_path+"/runtime";
  Check(CefInitialize(args, settings, consumer, nullptr), "CEF initialize");
  CefRunMessageLoop();
  CefShutdown(); consumer->AfterShutdown();
  Check(standalone->GetState() == ALLOY_PROFILE_LOCKED, "browser close wiped standalone profile too");
  consumer = nullptr; profile = nullptr; standalone = nullptr;
  std::puts("PASS profile-auto-lock-consumer"); return 0;
}
