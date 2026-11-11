// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

namespace {
void Check(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "FAIL guest-settings-%s\n", message); std::abort(); }
}
using Entries = std::vector<CefString>;
class Reply final : public AlloySiteSettingsCallback {
 public:
  explicit Reply(std::function<void(alloy_site_settings_result_t, const Entries&)> done)
      : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result, const Entries& entries) override {
    Check(CefCurrentlyOn(TID_UI), "callback-ui"); done_(result, entries);
  }
 private:
  const std::function<void(alloy_site_settings_result_t, const Entries&)> done_;
  IMPLEMENT_REFCOUNTING(Reply);
};
class Ready final : public CefRequestContextHandler {
 public:
  explicit Ready(std::function<void(CefRefPtr<CefRequestContext>)> done) : done_(std::move(done)) {}
  void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override { done_(context); }
 private:
  const std::function<void(CefRefPtr<CefRequestContext>)> done_;
  IMPLEMENT_REFCOUNTING(Ready);
};
class App final : public CefApp, public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnContextInitialized() override {
    memory_ = AlloyCreateMemoryRequestContext(new Ready([this](auto context) {
      Check(context->IsSame(memory_), "original-memory-owner");
      Check(AlloySetFirstPartyEmbeds(context, "https://one.alpha.test/",
          {"https://beta.test/", "http://two.beta.test/", "https://[::1]/"},
          new Reply([this](auto status, const auto& entries) {
            Check(status == ALLOY_SETTINGS_OK && entries.empty(), "memory-published");
            Check(!inside_set_, "asynchronous-write"); ReadShared();
          })), "memory-set-queued");
      inside_set_ = false;
    }));
    Check(!!memory_, "memory-created");
  }
  void ReadShared() {
    sharing_ = CefRequestContext::CreateContext(memory_, nullptr);
    Check(sharing_ && sharing_->IsSharingWith(memory_), "sharing-owner");
    Check(AlloyGetFirstPartyEmbeds(sharing_, "http://two.alpha.test/", new Reply(
        [this](auto status, const auto& entries) {
          Check(status == ALLOY_SETTINGS_OK && entries == Entries{"beta.test", "[::1]"}, "shared-canonical-read");
          Check(AlloySetFirstPartyEmbeds(memory_, "https://alpha.test/",
              {"https://gamma.test/", "data:text/plain,invalid"}, new Reply(
              [this](auto status, const auto&) {
                Check(status == ALLOY_SETTINGS_INVALID, "invalid-list-refused");
                Check(AlloyGetFirstPartyEmbeds(memory_, "https://alpha.test/", new Reply(
                    [this](auto status, const auto& entries) {
                      Check(status == ALLOY_SETTINGS_OK && entries == Entries{"beta.test", "[::1]"}, "invalid-preserves-list");
                      Independent();
                    })), "post-invalid-read");
              })), "invalid-write-queued");
        })), "shared-read-queued");
  }
  void Independent() {
    independent_ = AlloyCreateMemoryRequestContext(new Ready([this](auto context) {
      Check(!context->IsSharingWith(memory_), "independent-owner");
      Check(AlloyGetFirstPartyEmbeds(context, "https://alpha.test/", new Reply(
          [this](auto status, const auto& entries) {
            Check(status == ALLOY_SETTINGS_OK && entries.empty(), "independent-empty"); Stateless();
          })), "independent-read");
    }));
  }
  void Stateless() {
    stateless_ = AlloyCreateStatelessRequestContext(ALLOY_NETWORK_STATE_KEEP,
        new Ready([this](auto context) {
          Check(AlloySetFirstPartyEmbeds(context, "https://alpha.test/", {"https://beta.test/"},
              new Reply([this](auto status, const auto& entries) {
                Check(status == ALLOY_SETTINGS_CANCELLED && entries.empty(), "stateless-write-refused");
                Check(AlloyGetFirstPartyEmbeds(stateless_, "https://alpha.test/", new Reply(
                    [this](auto status, const auto& entries) {
                      Check(status == ALLOY_SETTINGS_CANCELLED && entries.empty(), "stateless-read-refused"); Clear();
                    })), "stateless-read");
              })), "stateless-write");
        }));
  }
  void Clear() {
    Check(AlloySetFirstPartyEmbeds(memory_, "https://alpha.test/", {}, new Reply(
        [this](auto status, const auto&) {
          Check(status == ALLOY_SETTINGS_OK, "clear-published");
          Check(AlloyGetFirstPartyEmbeds(sharing_, "https://alpha.test/", new Reply(
              [](auto status, const auto& entries) {
                Check(status == ALLOY_SETTINGS_OK && entries.empty(), "clear-shared-read");
                std::puts("PASS guest-settings-api"); CefQuitMessageLoop();
              })), "clear-read");
        })), "clear-queued");
  }
  void Release() { sharing_ = nullptr; memory_ = nullptr; independent_ = nullptr; stateless_ = nullptr; }
 private:
  bool inside_set_ = true;
  CefRefPtr<CefRequestContext> memory_, sharing_, independent_, stateless_;
  IMPLEMENT_REFCOUNTING(App);
};
}  // namespace

int main(int argc, char** argv) {
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "experimental-api");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  CefRefPtr<App> app = new App;
  Check(CefInitialize(args, settings, app, nullptr), "initialize");
  CefRunMessageLoop();
  app->Release(); CefDoMessageLoopWork(); CefShutdown(); app = nullptr;
  return 0;
}
