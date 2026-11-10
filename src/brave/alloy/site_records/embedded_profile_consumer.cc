// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Public SDK fixture: real profile/list/record callbacks, no Chromium internals.
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "brave/alloy/site_records/profile_control.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr char kPassword[] = "EMBEDDED_PROFILE_PASSWORD_6E";
void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "EMBEDDED_PROFILE_FAIL %s\n", message); std::abort(); }
}
class ContextReply final : public AlloyRequestContextCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t, CefRefPtr<CefRequestContext>)>;
  explicit ContextReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status, CefRefPtr<CefRequestContext> context) override {
    done_(status, context);
  }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(ContextReply);
};
class ListReply final : public AlloySiteSettingsCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t, const std::vector<CefString>&)>;
  explicit ListReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status, const std::vector<CefString>& values) override {
    done_(status, values);
  }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(ListReply);
};
class RecordsReply final : public AlloySiteRecordsCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t,
      const std::vector<CefRefPtr<AlloySiteRecord>>&)>;
  explicit RecordsReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status,
      const std::vector<CefRefPtr<AlloySiteRecord>>& records) override { done_(status, records); }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(RecordsReply);
};
class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status) override { done_(status); }
 private:
  const std::function<void(alloy_site_settings_result_t)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};
class DeleteReply final : public AlloyRecordDeletionCallback {
 public:
  explicit DeleteReply(std::function<void()> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t status, bool pending) override {
    Check(status == ALLOY_SETTINGS_OK && !pending, "record deletion completed"); done_();
  }
 private:
  const std::function<void()> done_;
  IMPLEMENT_REFCOUNTING(DeleteReply);
};

class Consumer final : public CefApp, public CefBrowserProcessHandler, public CefClient,
                       public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler {
 public:
  Consumer(std::string phase, std::string root, std::string base, std::string scope)
      : phase_(std::move(phase)), base_(std::move(base)), scope_(std::move(scope)) {
    for (int i = 0; i < 2; ++i) {
      const auto path = root+"/profile-"+std::to_string(i);
      profiles_[i] = phase_ == "write" ? AlloyCreateProfile(path, kPassword) : AlloyOpenProfile(path);
      Check(!!profiles_[i], "explicit encrypted profile create/open");
      if (phase_ != "write" && phase_ != "locked") {
        const bool unlocked = profiles_[i]->UnlockWithPassword(phase_ == "wrong" ? "wrong" : kPassword);
        Check(unlocked == (phase_ != "wrong"), "explicit profile unlock result");
      }
    }
  }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 900, 650); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnContextInitialized() override { CreateContext(0); }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    int owner = browser->GetHost()->GetRequestContext()->IsSharingWith(contexts_[0]) ? 0 : 1;
    Check(contexts_[owner] && browser->GetHost()->GetRequestContext()->IsSharingWith(contexts_[owner]),
          "browser original profile context");
    browsers_.push_back({browser, owner});
    ++created_[owner];
    Event("created", owner);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    for (const auto& entry : browsers_) if (entry.browser->IsSame(browser)) {
      ++closed_[entry.owner]; Event("closed", entry.owner); return;
    }
    Check(false, "close belongs to an original browser");
  }
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
    const auto value = title.ToString();
    const int status = profile_control_receive(&transport_, browser->GetIdentifier(), value.c_str(),
        [](void* self, const profile_control_request* request) {
          static_cast<Consumer*>(self)->Command(request->action);
        }, [](void* self, const profile_control_request* request, int replayed) {
          std::printf("PROFILE_EVENT {\"event\":\"control-reply\",\"request_id\":\"%s\","
              "\"token\":\"%s\",\"browser\":%d,\"executions\":%u,\"phase\":\"%s\"}\n",
              request->id, request->action, request->browser, request->executions,
              request->complete ? (replayed ? "replayed" : "dispatched") : "pending");
        }, this);
    Check(status >= 0, "identified command transport");
  }
  void ReleaseRuntime() {
    browsers_.clear(); contexts_ = {};
    std::thread release([profiles = std::move(profiles_)]() mutable { profiles = {}; });
    release.join(); profile_control_destroy(&transport_);
  }
 private:
  struct Browser { CefRefPtr<CefBrowser> browser; int owner; };
  std::string Url(const char* host) const {
    const auto colon = base_.rfind(':');
    return std::string("https://")+host+base_.substr(colon);
  }
  static void Event(const char* event, int owner) {
    std::printf("EMBEDDED_PROFILE_EVENT {\"event\":\"%s\",\"profile\":%d}\n", event, owner);
  }
  void CreateContext(int i) {
    Check(AlloyCreateEncryptedRequestContext(profiles_[i], nullptr, new ContextReply(
        [self = CefRefPtr<Consumer>(this), i](auto status, auto context) {
          if (self->phase_ == "locked" || self->phase_ == "wrong") {
            Check(status == ALLOY_SETTINGS_CANCELLED && !context, "locked context refuses admission");
            Check(self->profiles_[i]->GetFirstPartyEmbeds(self->Url("alpha.test"), new ListReply(
                [self, i](auto result, const auto& values) {
                  Check(result == ALLOY_SETTINGS_CANCELLED && values.empty(), "locked list refuses admission");
                  Event("refused", i);
                  if (i == 0) self->CreateContext(1); else CefQuitMessageLoop();
                })), "locked list queued");
            return;
          }
          Check(status == ALLOY_SETTINGS_OK && context, "native encrypted context admitted");
          Check(context->GetCachePath().empty(), "no ordinary context disk path");
          self->contexts_[i] = context;
          Check(AlloySetEmbeddedStateScope(context, self->scope_), "scope configured before documents");
          if (i == 0) self->CreateContext(1); else self->Prepare();
        })), "encrypted context creation queued");
  }
  void Prepare() {
    if (phase_ != "write") { ReadLists([self = CefRefPtr<Consumer>(this)] { self->StartBrowsers(); }); return; }
    for (int i = 0; i < 2; ++i) {
      contexts_[i]->SetContentSetting(Url("beta.test"), "", CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS,
          i == 0 ? CEF_CONTENT_SETTING_VALUE_ALLOW : CEF_CONTENT_SETTING_VALUE_BLOCK);
    }
    Check(profiles_[0]->SetFirstPartyEmbeds(Url("alpha.test"), {Url("beta.test")}, new ListReply(
        [self = CefRefPtr<Consumer>(this)](auto status, const auto&) {
          Check(status == ALLOY_SETTINGS_OK, "directional list durable publication");
          self->ReadLists([self] { self->CheckRecordList(0); });
        })), "initial list queued");
  }
  /// Check canonical, profile-isolated public snapshots before any top-level visit occurs.
  void CheckRecordList(int owner) {
    Check(profiles_[owner]->ListSiteRecords(new RecordsReply(
        [self = CefRefPtr<Consumer>(this), owner](auto status, const auto& records) {
          Check(status == ALLOY_SETTINGS_OK && records.size() == (owner == 0 ? 2u : 1u),
                "public catalogue includes only this profile's real site records");
          bool alpha = false, beta = false;
          for (const auto& record : records) {
            const auto domain = record->GetDomain();
            alpha |= domain == "alpha.test"; beta |= domain == "beta.test";
            Check(record->GetLastVisit() == 0, "settings-only public record has no top-level visit");
          }
          Check(beta && alpha == (owner == 0), "canonical profile-isolated public record domains");
          Event("records-listed", owner);
          if (owner == 0) self->CheckRecordList(1); else self->StartBrowsers();
        })), "public site catalogue queued");
  }
  void ReadLists(std::function<void()> done) {
    Check(AlloyGetFirstPartyEmbeds(contexts_[0], Url("alpha.test"), new ListReply(
        [self = CefRefPtr<Consumer>(this), done](auto status, const auto& values) {
          Check(status == ALLOY_SETTINGS_OK &&
              (self->phase_ == "after-delete" ? values.empty() : values.size() == 1 && values[0] == "beta.test"),
              "profile zero original directional list");
          Check(AlloyGetFirstPartyEmbeds(self->contexts_[1], self->Url("alpha.test"), new ListReply(
              [done](auto result, const auto& other) {
                Check(result == ALLOY_SETTINGS_OK && other.empty(), "profile one has no inherited list"); done();
              })), "independent profile list queued");
        })), "original context list read queued");
  }
  void StartBrowsers() {
    for (int i = 0; i < 2; ++i) {
      Check(contexts_[i]->GetContentSetting(Url("beta.test"), Url("beta.test"),
          CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS) ==
          (i == 1 ? CEF_CONTENT_SETTING_VALUE_BLOCK : phase_ == "after-delete"
              ? CEF_CONTENT_SETTING_VALUE_ASK : CEF_CONTENT_SETTING_VALUE_ALLOW),
          "real beta record survives unlock in its own profile");
      for (const char* host : {"alpha.test", "beta.test", "gamma.test"}) Open(i, host, "initial");
    }
    Event("ready", 0);
  }
  void Open(int owner, const char* host, const char* label) {
    CefWindowInfo window; window.SetAsWindowless(0); window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    const auto url = Url(host)+"/page/p"+std::to_string(owner)+"/"+label;
    Check(CefBrowserHost::CreateBrowser(window, this, url, settings, nullptr, contexts_[owner]), "explicit site browser");
  }
  void Command(const std::string& action) {
    if (action == "clear-list" || action == "restore-list") {
      const bool restore = action == "restore-list";
      Check(AlloySetFirstPartyEmbeds(contexts_[0], Url("alpha.test"),
          restore ? std::vector<CefString>{Url("beta.test")} : std::vector<CefString>{},
          new ListReply([restore](auto status, const auto&) {
            Check(status == ALLOY_SETTINGS_OK, "list replacement published");
            Event(restore ? "list-restored" : "list-cleared", 0);
          })), "list replacement queued");
    } else if (action == "block-cookies" || action == "unblock-cookies") {
      const bool block = action == "block-cookies";
      contexts_[0]->SetContentSetting(Url("beta.test"), "",
          CEF_CONTENT_SETTING_TYPE_COOKIES,
          block ? CEF_CONTENT_SETTING_VALUE_BLOCK : CEF_CONTENT_SETTING_VALUE_DEFAULT);
      Check(contexts_[0]->GetContentSetting(Url("beta.test"), Url("alpha.test"),
          CEF_CONTENT_SETTING_TYPE_COOKIES) ==
          (block ? CEF_CONTENT_SETTING_VALUE_BLOCK : CEF_CONTENT_SETTING_VALUE_ALLOW),
          "native site-cookie setting published");
      Event(block ? "cookies-blocked" : "cookies-unblocked", 0);
    } else if (action == "delete-outer") {
      Check(profiles_[0]->DeleteSiteRecord(Url("alpha.test"), new DeleteReply(
          [self = CefRefPtr<Consumer>(this)] {
            Check(AlloyGetFirstPartyEmbeds(self->contexts_[0], self->Url("alpha.test"), new ListReply(
                [self](auto status, const auto& values) {
                  Check(status == ALLOY_SETTINGS_OK && values.empty(), "deleted outer lost its list");
                  Event("outer-deleted", 0); self->Open(0, "alpha.test", "after-delete");
                })), "deleted list read queued");
          })), "outer deletion queued");
    } else if (action == "delete-inner") {
      Check(profiles_[0]->DeleteSiteRecord(Url("beta.test"), new DeleteReply(
          [self = CefRefPtr<Consumer>(this)] {
            Check(AlloyGetFirstPartyEmbeds(self->contexts_[0], self->Url("alpha.test"), new ListReply(
                [self](auto status, const auto& values) {
                  Check(status == ALLOY_SETTINGS_OK && values.empty(), "deleted embedded record lost generated list references");
                  Event("inner-deleted", 0); self->Open(0, "beta.test", "after-inner-delete");
                })), "deleted embedded references read queued");
          })), "embedded record deletion queued");
    } else if (action == "lock-zero") {
      Check(profiles_[0]->LockAsync(new LockReply([self = CefRefPtr<Consumer>(this)](auto status) {
        Check(status == ALLOY_SETTINGS_OK && self->created_[0] == self->closed_[0], "lock retired all original profile browsers");
        Check(!self->contexts_[0]->GetPreference("homepage"), "old context cannot read after lock");
        Check(AlloyGetFirstPartyEmbeds(self->contexts_[0], self->Url("alpha.test"), new ListReply(
            [](auto result, const auto& values) {
              Check(result == ALLOY_SETTINGS_CANCELLED && values.empty(), "locked original list cancelled"); Event("zero-locked", 0);
            })), "locked original list queued");
      })), "profile zero lock queued");
    } else if (action == "finish") {
      int owner = profiles_[0]->GetState() == ALLOY_PROFILE_LOCKED ? 1 : 0;
      FinishLock(owner);
    } else { Check(false, "known embedded profile command"); }
  }
  void FinishLock(int owner) {
    Check(profiles_[owner]->LockAsync(new LockReply([self = CefRefPtr<Consumer>(this), owner](auto status) {
      Check(status == ALLOY_SETTINGS_OK && self->created_[owner] == self->closed_[owner], "final native lock completion");
      if (owner == 0) self->FinishLock(1); else { Event("finished", 1); CefQuitMessageLoop(); }
    })), "final lock queued");
  }
  const std::string phase_, base_, scope_;
  std::array<CefRefPtr<AlloyProfile>, 2> profiles_;
  std::array<CefRefPtr<CefRequestContext>, 2> contexts_;
  std::array<int, 2> created_{}, closed_{};
  std::vector<Browser> browsers_;
  profile_control_transport transport_{};
  IMPLEMENT_REFCOUNTING(Consumer);
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "experimental API hash");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* phase = std::getenv("ALLOY_EMBEDDED_PHASE");
  const char* root = std::getenv("ALLOY_EMBEDDED_PROFILES");
  const char* base = std::getenv("ALLOY_EMBEDDED_BASE");
  const char* scope = std::getenv("ALLOY_EMBEDDED_SCOPE");
  const char* runtime = std::getenv("ALLOY_EMBEDDED_RUNTIME");
  Check(phase && root && base && scope && runtime, "fixture inputs copied before native initialization");
  CefRefPtr<Consumer> consumer = new Consumer(phase, root, base, scope);
  CefSettings settings; settings.windowless_rendering_enabled = true;
  CefString(&settings.root_cache_path) = runtime;
  Check(CefInitialize(args, settings, consumer, nullptr), "CEF initialization");
  CefRunMessageLoop(); consumer->ReleaseRuntime(); CefDoMessageLoopWork(); CefShutdown(); consumer = nullptr;
  std::puts("PASS embedded-profile-consumer");
  return 0;
}
