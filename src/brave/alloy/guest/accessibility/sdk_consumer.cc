// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Synthetic app-side public SDK consumer. No internal/native AX headers.
#include "include/alloy/alloy_guest_session.h"
#include "include/cef_api_hash.h"
#include "include/cef_parser.h"
#include <condition_variable>
#include <charconv>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

namespace {
std::mutex output_mutex;
void Event(const char* name, CefRefPtr<CefDictionaryValue> details = nullptr) {
  if (!details) details = CefDictionaryValue::Create();
  details->SetString("event", name);
  auto value = CefValue::Create(); value->SetDictionary(details);
  std::lock_guard guard(output_mutex);
  std::cout << "GUEST_AX_EVENT " << CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() << std::endl;
}
class App final : public AlloyGuestSessionHandler, public AlloyGuestViewHandler,
                  public AlloyGuestAccessibilityHandler, public AlloyGuestStreamHandler {
 public:
  bool Start() {
    auto settings = AlloyCreateGuestSettings("memory", "keep", "unavailable", true, "");
    auto session = AlloyStartGuestSession(settings, this);
    if (!session) return false;
    std::lock_guard guard(mutex_); session_ = session; return true;
  }
  void OnPrepared(CefRefPtr<AlloyGuestSession> session, CefRefPtr<AlloyGuestProtection>,
                    CefRefPtr<AlloyGuestCapabilities> capabilities) override {
    { std::lock_guard guard(mutex_); session_ = session; }
    auto data = CefDictionaryValue::Create();
    data->SetBool("accessibility", capabilities->IsAvailable("accessibility"));
    Event("prepared", data); session->Accept();
  }
  void OnStateChanged(CefRefPtr<AlloyGuestSession> session, const CefString& state) override {
    if (state == "ACTIVE") session->CreateView(800, 600, this);
  }
  void OnClosed(CefRefPtr<AlloyGuestSession>, const CefString& reason) override {
    auto data = CefDictionaryValue::Create(); data->SetString("reason", reason); Event("closed", data);
    std::lock_guard guard(mutex_);
    closed_ = true; session_ = nullptr; view_ = nullptr; stream_ = nullptr; ax_ = nullptr; cv_.notify_all();
  }
  void OnReady(CefRefPtr<AlloyGuestView> view) override {
    auto stream = view->GetStream(); auto ax = view->GetAccessibility();
    { std::lock_guard guard(mutex_); view_ = view; stream_ = stream; ax_ = ax; }
    if (!stream || !ax || !stream->Start(this, 2, 32 * 1024 * 1024) || !ax->Start(this)) Event("unavailable");
    Event("ready");
  }
  void OnTitleChanged(CefRefPtr<AlloyGuestView>, uint64_t document, const CefString& title) override {
    auto data = CefDictionaryValue::Create(); data->SetString("document", std::to_string(document));
    data->SetString("title", title); Event("title", data);
  }
  void OnNavigationComplete(CefRefPtr<AlloyGuestView>, uint64_t document, int status) override {
    auto data = CefDictionaryValue::Create(); data->SetString("document", std::to_string(document));
    data->SetInt("status", status); Event("load", data);
  }
  void OnClosed(CefRefPtr<AlloyGuestView>, const CefString&) override { Event("view-closed"); }
  void OnUpdate(CefRefPtr<CefDictionaryValue> update) override { Event("update", update); }
  void OnUnavailable() override { Event("unavailable"); }
  void OnClosed() override { Event("endpoint-closed"); }
  void OnConfigured(uint64_t generation, int width, int height, double scale) override {
    auto data = CefDictionaryValue::Create();
    data->SetString("geometry", std::to_string(generation)); data->SetDouble("scale", scale);
    data->SetInt("width", width); data->SetInt("height", height); Event("configured", data);
  }
  void OnFrame(CefRefPtr<AlloyGuestFrame>) override {}
  void OnInteraction(CefRefPtr<AlloyGuestInteraction> receipt) override {
    auto data = receipt->GetDetails();
    data->SetString("id", std::to_string(receipt->GetId()));
    data->SetString("source", receipt->GetSource()); data->SetString("kind", receipt->GetKind());
    data->SetString("document", std::to_string(receipt->GetDocumentId()));
    data->SetString("geometry", std::to_string(receipt->GetGeneration()));
    data->SetString("capture_us", std::to_string(receipt->GetAppCaptureTimeUs()));
    data->SetString("dispatch_us", std::to_string(receipt->GetDispatchTimeUs()));
    data->SetString("outcome", receipt->GetOutcome()); Event("interaction", data);
  }
  void OnPopup(bool visible, const CefRect& bounds, uint64_t generation) override {
    auto data = CefDictionaryValue::Create(); data->SetBool("visible", visible);
    data->SetString("geometry", std::to_string(generation));
    data->SetInt("x", bounds.x); data->SetInt("y", bounds.y);
    data->SetInt("width", bounds.width); data->SetInt("height", bounds.height); Event("popup", data);
  }
  void OnWindowOpen(CefRefPtr<AlloyGuestPopup> popup, const CefString&, const CefString&,
                    int, bool, const CefRect&) override { popup->Deny(); }
  void OnWindowRetired(uint64_t, const CefString&) override {}
  void OnCursor(int, CefRefPtr<CefBinaryValue>, int, int, int, int, double, uint64_t) override {}
  void OnImeBounds(const CefRange&, const std::vector<CefRect>&, uint64_t) override {}
  bool Command(CefRefPtr<CefDictionaryValue> data) {
    CefRefPtr<AlloyGuestView> view; CefRefPtr<AlloyGuestStream> stream;
    CefRefPtr<AlloyGuestAccessibility> ax; CefRefPtr<AlloyGuestSession> session;
    { std::lock_guard guard(mutex_); view = view_; stream = stream_; ax = ax_; session = session_; }
    std::string operation = data->GetString("op").ToString();
    bool ok = false;
    uint64_t id = 0;
    if (operation == "quit") {
      if (session) session->Close();
      std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return closed_; });
      return false;
    }
    if (operation == "navigate" && view) ok = view->Navigate(data->GetString("url"));
    else if (operation == "resync" && ax) ok = ax->Resync();
    else if (operation == "configure" && stream)
      ok = stream->Configure(data->GetInt("width"), data->GetInt("height"), data->GetDouble("scale"), 30);
    else if (operation == "action" && ax) {
      id = ax->SendAction(data->GetString("source"), Number(data, "document"), Number(data, "geometry"),
          data->GetString("tree"), Number(data, "tree_generation"), data->GetInt("node"),
          Number(data, "node_generation"), data->GetString("action"), data->GetString("value"),
          data->GetInt("anchor"), data->GetInt("focus"), Number(data, "capture_us"));
      ok = id != 0;
    } else if (operation == "focus" && stream) {
      id = stream->SendFocus("app", Number(data, "document"), Number(data, "geometry"),
          data->HasKey("focused") ? data->GetBool("focused") : true, 10); ok = id != 0;
    } else if (operation == "wheel" && stream) {
      id = stream->SendWheel("app", Number(data, "document"), Number(data, "geometry"), 0,
          300, 300, 0, data->GetInt("delta"), 0, 11); ok = id != 0;
    } else if (operation == "pointer" && stream) {
      id = stream->SendPointer("app", Number(data, "document"), Number(data, "geometry"), 0,
          data->GetInt("x"), data->GetInt("y"), data->GetString("action"), 0, 1, 0, 12); ok = id != 0;
    }
    auto result = CefDictionaryValue::Create(); result->SetString("op", operation);
    result->SetBool("ok", ok); result->SetString("id", std::to_string(id)); Event("queued", result);
    return true;
  }
 private:
  static uint64_t Number(CefRefPtr<CefDictionaryValue> data, const char* key) {
    auto text = data->GetString(key).ToString();
    uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? result : 0;
  }
  std::mutex mutex_; std::condition_variable cv_;
  bool closed_ = false;
  CefRefPtr<AlloyGuestSession> session_;
  CefRefPtr<AlloyGuestView> view_;
  CefRefPtr<AlloyGuestStream> stream_;
  CefRefPtr<AlloyGuestAccessibility> ax_;
  IMPLEMENT_REFCOUNTING(App);
};
}
int main() {
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  if (!hash || std::string(hash) != CEF_API_HASH_PLATFORM) return 2;
  CefRefPtr<App> app = new App;
  if (!app->Start()) return 3;
  std::string line;
  while (std::getline(std::cin, line)) {
    auto parsed = CefParseJSON(line, JSON_PARSER_RFC);
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) { Event("invalid-command"); continue; }
    if (!app->Command(parsed->GetDictionary())) return 0;
  }
  auto quit = CefDictionaryValue::Create(); quit->SetString("op", "quit"); app->Command(quit);
  return 0;
}
