// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_task.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;
void Check(bool value, const char* reason) {
  if (!value) {
    ++failures;
    std::fprintf(stderr, "FAIL browser-creation-%s\n", reason);
  }
}

class CloseTask final : public CefTask {
 public:
  explicit CloseTask(CefRefPtr<CefBrowser> browser) : browser_(browser) {}
  void Execute() override { browser_->GetHost()->CloseBrowser(true); }

 private:
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(CloseTask);
};

class QuitTask final : public CefTask {
 public:
  void Execute() override { CefQuitMessageLoop(); }

 private:
  IMPLEMENT_REFCOUNTING(QuitTask);
};

class App final : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefClient,
                  public CefLifeSpanHandler,
                  public CefRenderHandler {
 public:
  explicit App(std::string mode) : mode_(mode) {}
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
    rect = CefRect(0, 0, 800, 600);
  }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
               const void*, int, int) override {}

  void OnContextInitialized() override {
    Check(CefCurrentlyOn(TID_UI), "creation-ui-thread");
    CefWindowInfo window;
    window.SetAsWindowless(0);
    window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    if (mode_ == "sync-close") {
      auto browser = CefBrowserHost::CreateBrowserSync(
          window, this, "about:blank", settings, nullptr, nullptr);
      Check(!browser, "synchronous-closed-browser-not-returned");
    } else {
      Check(CefBrowserHost::CreateBrowser(window, this, "about:blank", settings,
                                         nullptr, nullptr),
            "asynchronous-create-queued");
    }
    creation_returned_ = true;
    std::puts("BROWSER_CREATION_EVENT creation-returned");
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Check(CefCurrentlyOn(TID_UI), "callback-ui-thread");
    Check(browser && browser->IsValid(), "callback-live-browser");
    ++created_;
    std::puts("BROWSER_CREATION_EVENT created");
    if (mode_ == "ordinary") {
      Check(CefPostTask(TID_UI, new CloseTask(browser)), "ordinary-close-posted");
    } else {
      // Deliberately destructive: creation still owns an active native stack.
      browser->GetHost()->CloseBrowser(true);
    }
    callback_returned_ = true;
    std::puts("BROWSER_CREATION_EVENT callback-returned");
  }

  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    ++closed_;
    std::puts("BROWSER_CREATION_EVENT closed");
    Check(CefPostTask(TID_UI, new QuitTask), "quit-posted");
  }

  void Verify() const {
    Check(created_ == 1, "exactly-one-created");
    Check(closed_ == 1, "exactly-one-closed");
    Check(callback_returned_, "callback-returned");
    Check(creation_returned_, "creation-returned");
  }

 private:
  const std::string mode_;
  int created_ = 0;
  int closed_ = 0;
  bool creation_returned_ = false;
  bool callback_returned_ = false;
  IMPLEMENT_REFCOUNTING(App);
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 2;
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* mode = std::getenv("ALLOY_BROWSER_CREATION_MODE");
  const char* root = std::getenv("ALLOY_BROWSER_CREATION_ROOT");
  if (!mode || !root) return 2;
  const std::string selected(mode);
  if (selected != "async-close" && selected != "sync-close" &&
      selected != "ordinary") return 2;
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  CefString(&settings.root_cache_path) = root;
  CefRefPtr<App> app = new App(selected);
  if (!CefInitialize(args, settings, app, nullptr)) return 3;
  CefRunMessageLoop();
  app->Verify();
  app = nullptr;
  CefShutdown();
  if (failures) return 1;
  std::printf("PASS browser-creation-%s\n", selected.c_str());
  return 0;
}
