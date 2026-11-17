// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_SANDBOX_PREFLIGHT_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_SANDBOX_PREFLIGHT_H_

#include <sstream>
#include <sys/prctl.h>
#include <unistd.h>
#include "brave/alloy/guest/linux_lock/protection.h"
#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "cef/include/cef_client.h"
#include "cef/include/cef_load_handler.h"
#include "cef/include/cef_life_span_handler.h"
#include "cef/include/cef_render_handler.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/zygote_host/zygote_host_linux.h"
#include "sandbox/policy/linux/sandbox_linux.h"

namespace alloy::guest {
// Header-only because this CEF-native client compiles in libcef_static, not in
// the foundation/provider layer. It has no app view, document, resource lease or
// requested URL. PREPARED waits for a real renderer to finish this private blank
// document, then for its browser to close.
class LinuxSandboxPreflight final : public CefClient,
                                    public CefRenderHandler,
                                    public CefLifeSpanHandler,
                                    public CefLoadHandler,
                                    public content::WebContentsObserver {
 public:
  bool Start(CefRefPtr<CefRequestContext> context) {
    CefWindowInfo window;
    window.SetAsWindowless(kNullWindowHandle);
    bool created = !!CefBrowserHost::CreateBrowserSync(window, this, "about:blank",
        CefBrowserSettings(), nullptr, context);
    if (!created) done_ = true;
    return created;
  }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1, 1); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
               const void*, int, int) override {}
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browser_ = browser;
    Observe(CefBrowserHostBase::FromBrowser(browser)->GetWebContents());
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    Observe(nullptr);
    browser_ = nullptr;
    done_ = true;
  }
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
    if (!frame->IsMain() || cancelled_ || !browser_) return;
    auto* process = web_contents()->GetPrimaryMainFrame()->GetProcess();
    uint64_t caps = 0;
    facts_.browser_nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1;
    facts_.browser_seccomp = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    facts_.observed = process->IsReady() && ReadRendererStatus(process->GetProcess().Pid(), &caps);
    facts_.renderer_capabilities = caps;
    facts_.native_flags = content::ZygoteHost::GetInstance()->GetRendererSandboxStatus();
    using Status = sandbox::policy::SandboxLinux;
    if (facts_.native_flags & Status::kUserNS) facts_.route = "userns";
    else if (facts_.native_flags & Status::kSUID) facts_.route = "setuid";
    Cancel();
  }
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   ErrorCode, const CefString&, const CefString&) override {
    if (frame->IsMain()) Cancel();
  }
  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override { Cancel(); }
  void Cancel() { cancelled_ = true; }
  // Called by the host outside native callback notification. Force-close may
  // synchronously destroy WebContents, which cannot happen inside OnLoadEnd or
  // WebContentsObserver callbacks.
  void Drain() {
    if (cancelled_ && browser_ && !close_requested_) {
      close_requested_ = true;
      browser_->GetHost()->CloseBrowser(true);
    }
  }
  bool done() const { return done_; }
  const LinuxSandboxFacts& facts() const { return facts_; }
 private:
  bool ReadRendererStatus(pid_t pid, uint64_t* capabilities) {
    std::string contents;
    if (pid <= 1 || !base::ReadFileToString(
        base::FilePath("/proc/" + std::to_string(pid) + "/status"), &contents)) return false;
    bool nnp = false, seccomp = false, caps = false;
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string_view key(line.data(), colon);
      std::string_view value = base::TrimWhitespaceASCII(
          std::string_view(line).substr(colon + 1), base::TRIM_ALL);
      if (key == "NoNewPrivs") {
        int number = 0;
        nnp = base::StringToInt(value, &number);
        facts_.renderer_nnp = number == 1;
      } else if (key == "Seccomp") {
        seccomp = base::StringToInt(value, &facts_.renderer_seccomp);
      } else if (key == "CapEff") {
        caps = base::HexStringToUInt64(value, capabilities);
      }
    }
    return nnp && seccomp && caps;
  }
  CefRefPtr<CefBrowser> browser_;
  LinuxSandboxFacts facts_;
  bool done_ = false;
  bool cancelled_ = false;
  bool close_requested_ = false;
  IMPLEMENT_REFCOUNTING(LinuxSandboxPreflight);
};
}  // namespace alloy::guest
#endif
