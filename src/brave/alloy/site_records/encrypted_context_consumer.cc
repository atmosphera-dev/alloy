// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Real SDK browser consumer; no Chromium-internal types cross the SDK boundary.
#include "include/alloy/alloy_profile.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_preference.h"
#include "include/cef_render_handler.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr char kPassword[] = "ENCRYPTED_CONTEXT_PASSWORD_32";
constexpr char kMetadataName[] = "ALLOY_NATIVE_PROFILE_METADATA_88";
constexpr const char* kMetadataKeys[] = {"profile.name", "profile.avatar_index",
    "profile.using_default_name", "profile.using_default_avatar"};
void Check(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "ENCRYPTED_FAIL %s\n", message); std::abort(); }
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
  void OnComplete(alloy_site_settings_result_t result, const std::vector<CefString>& values) override {
    done_(result, values);
  }
 private:
  const Done done_;
  IMPLEMENT_REFCOUNTING(SettingsReply);
};
class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done) : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result) override { done_(result); }
 private:
  const std::function<void(alloy_site_settings_result_t)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};
class DeleteReply final : public AlloyRecordDeletionCallback {
 public:
  void OnComplete(alloy_site_settings_result_t result, bool pending) override {
    std::printf("ENCRYPTED_EVENT {\"event\":\"deletion-result\",\"result\":%d,\"pending\":%s}\n",
                static_cast<int>(result), pending ? "true" : "false");
    Check(result == ALLOY_SETTINGS_OK && !pending, "live record deletion complete");
    std::puts("ENCRYPTED_EVENT {\"event\":\"deleted\"}");
  }
 private:
  IMPLEMENT_REFCOUNTING(DeleteReply);
};
class Consumer final : public CefApp, public CefBrowserProcessHandler, public CefClient,
                       public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler {
 public:
  Consumer(std::string phase, std::string path, std::string first, std::string second)
      : phase_(std::move(phase)), first_(std::move(first)), second_(std::move(second)) {
    const bool keyring = std::getenv("ALLOY_ENCRYPTED_KEYRING") != nullptr;
    int keyring_result = ALLOY_KEYRING_FAILED;
    profile_ = (phase_ == "write" || phase_ == "decay-seed" || phase_ == "decay-live") ? keyring ? AlloyCreateProfileWithKeyring(path, keyring_result)
        : AlloyCreateProfile(path, kPassword) : AlloyOpenProfile(path);
    Check(!!profile_, "profile create/open");
    if (phase_ != "write" && phase_ != "decay-seed" && phase_ != "decay-live" && phase_ != "locked") {
      const bool unlocked = keyring && phase_ != "wrong" ? profile_->UnlockWithKeyring() == ALLOY_KEYRING_OK :
          profile_->UnlockWithPassword(phase_ == "wrong" ? "wrong-password" : kPassword);
      Check(unlocked == (phase_ != "wrong"), "profile password result");
    }
    if (phase_ == "decay-read") {
      int64_t period = -1; int timebase = -1;
      Check(profile_->GetDecaySettings(period, timebase) && period == 6 &&
          timebase == ALLOY_DECAY_SINCE_LAST_VISIT, "persisted decay policy before native reads");
      std::puts("ENCRYPTED_EVENT {\"event\":\"decay-policy-reopened\",\"seconds\":6}");
    }
  }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  void ReleaseRuntime() {
    // Retained SDK objects must leave the live CEF runtime before CefShutdown.
    browsers_.clear();
    context_ = nullptr;
    std::thread release([profile = std::move(profile_)]() mutable { profile = nullptr; });
    release.join();
  }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1000, 700); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}
  void OnContextInitialized() override {
    Check(AlloyCreateEncryptedRequestContext(profile_, nullptr, new ContextReply(
        [self = CefRefPtr<Consumer>(this)](auto result, auto context) { self->Created(result, context); })),
        "encrypted creation queued");
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browsers_.push_back(browser);
    Check(browser->GetHost()->GetRequestContext()->IsSharingWith(context_), "actual browser context");
    std::printf("ENCRYPTED_EVENT {\"event\":\"browser-created\",\"id\":%d}\n", browser->GetIdentifier());
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    ++closed_;
    std::printf("ENCRYPTED_EVENT {\"event\":\"browser-closed\",\"id\":%d}\n", browser->GetIdentifier());
  }
  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    const auto raw = title.ToString();
    const std::string prefix = "ALLOY_ENCRYPTED:";
    if (!raw.starts_with(prefix)) return;
    const auto separator = raw.find(':', prefix.size());
    Check(separator != std::string::npos, "control sequence present");
    const auto serial = raw.substr(prefix.size(), separator-prefix.size());
    Check(!serial.empty() && serial.find_first_not_of("0123456789") == std::string::npos,
          "control sequence valid");
    const auto sequence = std::strtoul(serial.c_str(), nullptr, 10);
    Check(sequence != 0, "positive control sequence");
    // CEF also reports the current title when a child frame stops loading.
    // A driver command executes once, independent of notification replay.
    if (sequence <= last_control_) return;
    last_control_ = sequence;
    const auto value = prefix+raw.substr(separator+1);
    if (value == "ALLOY_ENCRYPTED:write") Write();
    if (value == "ALLOY_ENCRYPTED:read") Read();
    if (value == "ALLOY_ENCRYPTED:enable-decay") {
      Check(phase_ == "decay-seed" || phase_ == "decay-live", "decay phase");
      std::thread([profile = profile_] {
        int64_t period = -1; int timebase = -1;
        Check(profile->GetDecaySettings(period, timebase) && period == 0,
              "no built-in decay default");
        Check(profile->SetDecaySettings(6, ALLOY_DECAY_SINCE_LAST_VISIT), "decay setter durable");
        Check(!profile->SetDecaySettings(-1, ALLOY_DECAY_SINCE_LAST_VISIT), "negative decay refused");
        Check(!profile->SetDecaySettings(6, static_cast<alloy_decay_timebase_t>(9)), "unknown decay clock refused");
        Check(profile->GetDecaySettings(period, timebase) && period == 6 && timebase == ALLOY_DECAY_SINCE_LAST_VISIT,
              "decay policy readback");
        std::puts("ENCRYPTED_EVENT {\"event\":\"decay-enabled\",\"seconds\":6}");
      }).detach();
    }
    if (value == "ALLOY_ENCRYPTED:delete") {
      Check(phase_ == "live", "live delete phase");
      Check(profile_->DeleteSiteRecord(first_, new DeleteReply), "live delete queued");
    }
    if (value == "ALLOY_ENCRYPTED:recreate") {
      Check((phase_ == "live" || phase_ == "decay-live") && !recreated_, "fresh live navigation ordering");
      recreated_ = true;
      CreateBrowserForSite(first_+"/page/fresh");
    }
    if (value == "ALLOY_ENCRYPTED:finish" && !locking_) Finish();
  }
 private:
  bool ContainsMetadata(CefRefPtr<CefValue> value) const {
    if (!value) return false;
    if (value->GetType() == VTYPE_STRING)
      return value->GetString().ToString().find(kMetadataName) != std::string::npos;
    if (value->GetType() == VTYPE_DICTIONARY) {
      auto dictionary = value->GetDictionary();
      std::vector<CefString> keys;
      Check(dictionary->GetKeys(keys), "native attributes dictionary keys");
      for (const auto& key : keys) if (ContainsMetadata(dictionary->GetValue(key))) return true;
    } else if (value->GetType() == VTYPE_LIST) {
      auto list = value->GetList();
      for (size_t i = 0; i < list->GetSize(); ++i) if (ContainsMetadata(list->GetValue(i))) return true;
    }
    return false;
  }
  void NoGlobalMetadata() const {
    auto manager = CefPreferenceManager::GetGlobalPreferenceManager();
    Check(manager && manager->HasPreference("profile.info_cache"), "native global catalogue registered");
    Check(!ContainsMetadata(manager->GetPreference("profile.info_cache")), "no retained private metadata in global catalogue");
  }
  void ReadMetadata() const {
    auto name = context_->GetPreference(kMetadataKeys[0]);
    auto avatar = context_->GetPreference(kMetadataKeys[1]);
    Check(name && name->GetType() == VTYPE_STRING && name->GetString() == kMetadataName,
          "native private profile name readback");
    Check(avatar && avatar->GetType() == VTYPE_INT && avatar->GetInt() == 7,
          "native private profile avatar readback");
    for (size_t i : {size_t{2}, size_t{3}}) {
      auto flag = context_->GetPreference(kMetadataKeys[i]);
      Check(flag && flag->GetType() == VTYPE_BOOL && !flag->GetBool(), "native private profile flag readback");
    }
  }
  void WriteMetadata() {
    for (size_t i = 0; i < 4; ++i) {
      Check(context_->HasPreference(kMetadataKeys[i]) && context_->CanSetPreference(kMetadataKeys[i]),
            "actual native metadata preference writable");
      auto value = CefValue::Create();
      if (i == 0) value->SetString(kMetadataName);
      else if (i == 1) value->SetInt(7);
      else value->SetBool(false);
      CefString error;
      Check(context_->SetPreference(kMetadataKeys[i], value, error), "native profile metadata write");
    }
    ReadMetadata();
    std::puts("ENCRYPTED_EVENT {\"event\":\"metadata-written\",\"fields\":4}");
  }
  void Created(alloy_site_settings_result_t result, CefRefPtr<CefRequestContext> context) {
    if (phase_ == "locked" || phase_ == "wrong") {
      Check(result == ALLOY_SETTINGS_CANCELLED && !context, "locked context refused");
      NoGlobalMetadata();
      std::puts("ENCRYPTED_EVENT {\"event\":\"metadata-refused\",\"fields\":4}");
      Check(profile_->GetFirstPartyEmbeds(first_, new SettingsReply(
          [](auto status, const auto& entries) {
            Check(status == ALLOY_SETTINGS_CANCELLED && entries.empty(), "locked list refused");
            std::puts("ENCRYPTED_EVENT {\"event\":\"refused\"}");
            CefQuitMessageLoop();
          })), "locked read queued");
      return;
    }
    Check(result == ALLOY_SETTINGS_OK && context && !context->IsGlobal(), "native encrypted context created");
    Check(context->GetCachePath().empty(), "no ordinary disk backing");
    auto readable = context->GetPreference("homepage");
    Check(readable && readable->GetType() == VTYPE_STRING, "pre-lock native preference readable");
    context_ = context;
    CreateBrowserForSite(first_+"/page/a");
    CreateBrowserForSite(second_+"/page/b");
  }
  void CreateBrowserForSite(const std::string& url) {
    CefWindowInfo window;
    window.SetAsWindowless(0);
    window.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    Check(CefBrowserHost::CreateBrowser(window, this, url, settings, nullptr, context_), "site browser");
  }
  void Write() {
    Check((phase_ == "write" || phase_ == "live" || phase_ == "decay-seed" || phase_ == "decay-live") && browsers_.size() == 2 && !written_, "write command ordering");
    written_ = true;
    WriteMetadata();
    context_->SetContentSetting(first_, first_, CEF_CONTENT_SETTING_TYPE_GEOLOCATION, CEF_CONTENT_SETTING_VALUE_ALLOW);
    context_->SetContentSetting(second_, second_, CEF_CONTENT_SETTING_TYPE_GEOLOCATION, CEF_CONTENT_SETTING_VALUE_BLOCK);
    for (const auto& browser : browsers_) {
      const auto url = browser->GetMainFrame()->GetURL().ToString();
      browser->GetHost()->SetZoomLevel(url.starts_with(first_) ? 1.0 : 2.0);
    }
    Check(AlloySetFirstPartyEmbeds(context_, first_, {second_}, new SettingsReply(
        [self = CefRefPtr<Consumer>(this)](auto result, const auto&) {
          Check(result == ALLOY_SETTINGS_OK, "first list durable");
          Check(self->profile_->SetFirstPartyEmbeds(self->second_, {self->first_}, new SettingsReply(
              [](auto status, const auto&) {
                Check(status == ALLOY_SETTINGS_OK, "second list durable");
                std::puts("ENCRYPTED_EVENT {\"event\":\"written\"}");
              })), "second list queued");
        })), "first list queued");
  }
  void Read() {
    Check(!read_, "one read command");
    read_ = true;
    ReadMetadata();
    std::puts("ENCRYPTED_EVENT {\"event\":\"metadata-read\",\"fields\":4}");
    for (const auto& outer : {first_, second_}) {
      Check(profile_->GetFirstPartyEmbeds(outer, new SettingsReply(
          [outer, context = context_](auto status, const auto& values) {
            Check(status == ALLOY_SETTINGS_OK, "list read");
            Check(AlloyGetFirstPartyEmbeds(context, outer, new SettingsReply(
                [outer, expected = values](auto status, const auto& actual) {
                  Check(status == ALLOY_SETTINGS_OK && actual == expected, "context-profile-list-owner");
                  std::printf("ENCRYPTED_EVENT {\"event\":\"embeds\",\"outer\":\"%s\",\"count\":%zu,\"domain\":\"%s\"}\n",
                      outer.c_str(), actual.size(), actual.empty() ? "" : actual[0].ToString().c_str());
                })), "context list read queued");
          })), "list read queued");
    }
  }
  void Finish() {
    locking_ = true;
    Check(profile_->LockAsync(new LockReply([self = CefRefPtr<Consumer>(this)](auto result) {
      Check(result == ALLOY_SETTINGS_OK && self->profile_->GetState() == ALLOY_PROFILE_LOCKED,
            "key/native/cache lock completion");
      Check(self->closed_ == static_cast<int>(self->browsers_.size()), "every actual browser retired before lock completion");
      Check(!self->context_->GetPreference("homepage"), "old readable context preference refused");
      for (const char* key : kMetadataKeys)
        Check(!self->context_->GetPreference(key), "locked original native metadata getter refused");
      self->NoGlobalMetadata();
      std::puts("ENCRYPTED_EVENT {\"event\":\"metadata-revoked\",\"fields\":4}");
      Check(AlloyCreateEncryptedRequestContext(self->profile_, nullptr, new ContextReply(
          [](auto status, auto context) {
            Check(status == ALLOY_SETTINGS_CANCELLED && !context, "new locked context refused");
            std::puts("ENCRYPTED_EVENT {\"event\":\"locked\"}");
            CefQuitMessageLoop();
          })), "post-lock context request queued");
    })), "native lock queued");
  }
  const std::string phase_, first_, second_;
  CefRefPtr<AlloyProfile> profile_;
  CefRefPtr<CefRequestContext> context_;
  std::vector<CefRefPtr<CefBrowser>> browsers_;
  bool written_ = false, read_ = false, locking_ = false, recreated_ = false;
  unsigned long last_control_ = 0;
  int closed_ = 0;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "experimental API hash");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* phase = std::getenv("ALLOY_ENCRYPTED_PHASE");
  const char* path = std::getenv("ALLOY_ENCRYPTED_PATH");
  const char* first = std::getenv("ALLOY_ENCRYPTED_FIRST");
  const char* second = std::getenv("ALLOY_ENCRYPTED_SECOND");
  Check(phase && path && first && second, "fixture inputs");
  CefRefPtr<Consumer> consumer = new Consumer(phase, path, first, second);
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  Check(CefInitialize(args, settings, consumer, nullptr), "CEF initialization");
  CefRunMessageLoop();
  consumer->ReleaseRuntime();
  CefDoMessageLoopWork();
  CefShutdown();
  consumer = nullptr;
  std::puts("PASS encrypted-context-consumer");
  return 0;
}
