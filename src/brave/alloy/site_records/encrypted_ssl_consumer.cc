// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-only real SDK/network consumer. No Chromium-private types or trust bypass.
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
void Check(bool ok, const char* why) {
  if (!ok) { std::fprintf(stderr, "SSL_FAIL %s\n", why); std::abort(); }
}
class ContextReply final : public AlloyRequestContextCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t, CefRefPtr<CefRequestContext>)>;
  explicit ContextReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result, CefRefPtr<CefRequestContext> context) override {
    done_(result, context);
  }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(ContextReply);
};
class SettingsReply final : public AlloySiteSettingsCallback {
 public:
  using Done = std::function<void(alloy_site_settings_result_t, const std::vector<CefString>&)>;
  explicit SettingsReply(Done done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result, const std::vector<CefString>& values) override { done_(result, values); }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(SettingsReply);
};
class DeleteReply final : public AlloyRecordDeletionCallback {
 public:
  explicit DeleteReply(std::function<void(alloy_site_settings_result_t, bool)> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result, bool pending) override { done_(result, pending); }
 private:
  const std::function<void(alloy_site_settings_result_t, bool)> done_;
  IMPLEMENT_REFCOUNTING(DeleteReply);
};
class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result) override { done_(result); }
 private:
  const std::function<void(alloy_site_settings_result_t)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};
class Consumer final : public CefApp, public CefBrowserProcessHandler, public CefClient,
    public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler,
    public CefRequestHandler, public CefLoadHandler {
 public:
  Consumer(std::string path, std::string first, std::string second)
      : first_(std::move(first)), second_(std::move(second)) {
    Check(first_.starts_with("https://alpha.test:") && second_.starts_with("https://beta.test:"), "fixture origins");
    profile_ = AlloyCreateProfile(path, "SSL_TRANSPORT_FIXTURE_PASSWORD_43");
    Check(!!profile_, "encrypted profile create");
  }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1000, 700); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnContextInitialized() override {
    Check(AlloyCreateEncryptedRequestContext(profile_, nullptr, new ContextReply(
        [self = CefRefPtr<Consumer>(this)](auto result, auto context) {
          Check(result == ALLOY_SETTINGS_OK && context && !context->IsGlobal(), "native encrypted context");
          self->context_ = context;
          Check(self->profile_->SetFirstPartyEmbeds(self->second_, {self->first_}, new SettingsReply(
              [self](auto status, const auto&) {
                Check(status == ALLOY_SETTINGS_OK, "durable explicit embed exception");
                self->CreateBrowser(self->first_+"/page/initial");
                self->CreateBrowser(self->second_+"/page/initial");
              })), "embed exception queued");
        })), "encrypted context queued");
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    Check(browser->GetHost()->GetRequestContext()->IsSharingWith(context_), "native browser owner");
    browsers_.push_back(browser);
    std::printf("SSL_EVENT {\"event\":\"created\",\"browser\":%d}\n", browser->GetIdentifier());
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    ++closed_;
    std::printf("SSL_EVENT {\"event\":\"closed\",\"browser\":%d}\n", browser->GetIdentifier());
  }
  bool OnCertificateError(CefRefPtr<CefBrowser> browser, cef_errorcode_t error,
      const CefString& request_url, CefRefPtr<CefSSLInfo>, CefRefPtr<CefCallback> callback) override {
    const auto url = request_url.ToString();
    const bool alpha = url.starts_with(first_+"/");
    Check(alpha || url.starts_with(second_+"/"), "fixture certificate URL");
    Check(error == ERR_CERT_AUTHORITY_INVALID, "real untrusted fixture certificate");
    const bool hold = alpha && policy_ == Policy::kHold;
    const bool deny = alpha && policy_ == Policy::kDeny;
    std::printf("SSL_EVENT {\"event\":\"certificate\",\"browser\":%d,\"url\":\"%s\",\"error\":%d,\"held\":%s,\"denied\":%s}\n",
        browser->GetIdentifier(), url.c_str(), static_cast<int>(error), hold ? "true" : "false", deny ? "true" : "false");
    if (hold) held_.push_back(callback);
    else if (deny) callback->Cancel();
    else callback->Continue();
    return true;
  }
  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
      cef_errorcode_t error, const CefString&, const CefString& url) override {
    std::printf("SSL_EVENT {\"event\":\"load-error\",\"browser\":%d,\"main\":%s,\"url\":\"%s\",\"error\":%d}\n",
        browser->GetIdentifier(), frame->IsMain() ? "true" : "false", url.ToString().c_str(), static_cast<int>(error));
  }
  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    const auto raw = title.ToString();
    const std::string prefix = "SSL_PROBE:";
    if (!raw.starts_with(prefix)) return;
    const auto separator = raw.find(':', prefix.size());
    Check(separator != std::string::npos, "control sequence present");
    const auto serial = raw.substr(prefix.size(), separator-prefix.size());
    Check(!serial.empty() && serial.find_first_not_of("0123456789") == std::string::npos, "control sequence valid");
    const auto sequence = std::strtoul(serial.c_str(), nullptr, 10);
    Check(sequence != 0, "positive control sequence");
    // Native title notifications may repeat during child-frame lifecycle. A
    // controller request executes once, independent of UI notification replay.
    if (sequence <= last_control_) return;
    last_control_ = sequence;
    const auto command = prefix+raw.substr(separator+1);
    if (command == "SSL_PROBE:audit") Audit();
    if (command == "SSL_PROBE:arm") {
      Check(policy_ == Policy::kAccept && !deleted_, "certificate control order");
      policy_ = Policy::kHold; std::puts("SSL_EVENT {\"event\":\"armed\"}");
    }
    if (command == "SSL_PROBE:delete") {
      Check(policy_ == Policy::kHold && !deleting_, "delete order"); deleting_ = true;
      Check(profile_->DeleteSiteRecord(first_, new DeleteReply(
          [self = CefRefPtr<Consumer>(this)](auto result, bool pending) {
            Check(result == ALLOY_SETTINGS_OK && !pending, "key/native deletion completion");
            self->deleted_ = true; std::puts("SSL_EVENT {\"event\":\"deleted\"}");
          })), "record delete queued");
    }
    if (command == "SSL_PROBE:fresh") {
      Check(deleted_ && !fresh_, "fresh browser order"); fresh_ = true; CreateBrowser(first_+"/page/fresh");
    }
    if (command == "SSL_PROBE:release") {
      Check(deleted_ && policy_ == Policy::kHold, "held reply release order");
      policy_ = Policy::kDeny;
      auto pending = std::move(held_); held_.clear();
      for (auto& reply : pending) reply->Cancel();
      std::printf("SSL_EVENT {\"event\":\"released\",\"count\":%zu}\n", pending.size());
    }
    if (command == "SSL_PROBE:finish") {
      Check(deleted_ && policy_ == Policy::kDeny && held_.empty() && !locking_, "finish order");
      locking_ = true;
      Check(profile_->LockAsync(new LockReply([self = CefRefPtr<Consumer>(this)](auto result) {
        Check(result == ALLOY_SETTINGS_OK && self->profile_->GetState() == ALLOY_PROFILE_LOCKED, "native lock complete");
        Check(self->closed_ == static_cast<int>(self->browsers_.size()), "all browsers closed before lock");
        std::puts("SSL_EVENT {\"event\":\"locked\"}"); CefQuitMessageLoop();
      })), "lock queued");
    }
  }
  void ReleaseRuntime() {
    held_.clear(); browsers_.clear(); context_ = nullptr;
    std::thread release([profile = std::move(profile_)]() mutable { profile = nullptr; }); release.join();
  }
 private:
  enum class Policy { kAccept, kHold, kDeny };
  void CreateBrowser(const std::string& url) {
    CefWindowInfo window; window.SetAsWindowless(0); window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    Check(CefBrowserHost::CreateBrowser(window, this, url, settings, nullptr, context_), "create native browser");
  }
  bool HasCertDecision(const char* site) {
    auto value = context_->GetWebsiteSetting(site, site, CEF_CONTENT_SETTING_TYPE_SSL_CERT_DECISIONS);
    if (!value || value->GetType() != VTYPE_DICTIONARY) return false;
    auto grants = value->GetDictionary()->GetDictionary("cert_exceptions_map");
    return grants && grants->GetSize() != 0;
  }
  void Audit() {
    const bool alpha = HasCertDecision("https://alpha.test"), beta = HasCertDecision("https://beta.test");
    Check(alpha == !deleted_ && beta, "native remembered grant projection");
    const int sequence = ++audits_;
    std::printf("SSL_EVENT {\"event\":\"settings\",\"sequence\":%d,\"alpha\":%s,\"beta\":%s}\n",
        sequence, alpha ? "true" : "false", beta ? "true" : "false");
    Check(profile_->GetFirstPartyEmbeds(second_, new SettingsReply(
        [sequence, deleted = deleted_](auto result, const auto& values) {
          Check(result == ALLOY_SETTINGS_OK && values.size() == (deleted ? 0u : 1u), "native embed-list retirement");
          if (!deleted) Check(values[0].ToString() == "alpha.test", "native listed alpha control");
          std::printf("SSL_EVENT {\"event\":\"embed-list\",\"sequence\":%d,\"count\":%zu}\n", sequence, values.size());
        })), "native embed-list read queued");
  }
  const std::string first_, second_;
  CefRefPtr<AlloyProfile> profile_;
  CefRefPtr<CefRequestContext> context_;
  std::vector<CefRefPtr<CefBrowser>> browsers_;
  std::vector<CefRefPtr<CefCallback>> held_;
  Policy policy_ = Policy::kAccept;
  bool deleting_ = false, deleted_ = false, fresh_ = false, locking_ = false;
  int audits_ = 0, closed_ = 0;
  unsigned long last_control_ = 0;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "experimental API hash");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* path = std::getenv("ALLOY_SSL_PATH");
  const char* first = std::getenv("ALLOY_SSL_FIRST");
  const char* second = std::getenv("ALLOY_SSL_SECOND");
  Check(path && first && second, "fixture inputs");
  CefRefPtr<Consumer> consumer = new Consumer(path, first, second);
  CefSettings settings; settings.windowless_rendering_enabled = true;
  Check(CefInitialize(args, settings, consumer, nullptr), "CEF initialize");
  CefRunMessageLoop(); consumer->ReleaseRuntime(); CefDoMessageLoopWork(); CefShutdown(); consumer = nullptr;
  std::puts("PASS encrypted-ssl-transport-consumer");
  return 0;
}
