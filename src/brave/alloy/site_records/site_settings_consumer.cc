// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// SDK-side C and C++ consumers share only test control, not library internals.
#include "include/alloy/alloy_profile.h"
#include "include/capi/alloy/alloy_profile_capi.h"
#include "include/cef_api_hash.h"
#include "include/cef_app.h"
#include "include/cef_task.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using Entries = std::vector<std::string>;
using Done = std::function<void(int, Entries)>;
int failures = 0;
std::atomic<int> live_callbacks{0};
constexpr char kPassword[] = "SETTINGS_API_PASSWORD_65d2";
constexpr char kOuter[] = "https://one.alpha.test/";
constexpr char kOther[] = "https://gamma.test/";
void Check(bool value, const char* label) {
  if (!value) { std::fprintf(stderr, "FAIL site-settings-%s\n", label); ++failures; }
}
class Reply final : public AlloySiteSettingsCallback {
 public:
  explicit Reply(Done done) : done_(std::move(done)) { ++live_callbacks; }
  void OnComplete(alloy_site_settings_result_t result, const std::vector<CefString>& values) override {
    Entries entries; for (const auto& value : values) entries.push_back(value.ToString());
    auto done = done_; done(result, std::move(entries));
  }
 private:
  ~Reply() override { --live_callbacks; }
  const Done done_;
  IMPLEMENT_REFCOUNTING(Reply);
};
class CReply final : public alloy_site_settings_callback_t {
 public:
  explicit CReply(Done done) : done_(std::move(done)) {
    ++live_callbacks;
    base.size = sizeof(alloy_site_settings_callback_t);
    base.add_ref = [](cef_base_ref_counted_t* b) { ++Self(b)->references_; };
    base.release = [](cef_base_ref_counted_t* b) {
      auto* self = Self(b); if (--self->references_) return 0; delete self; return 1;
    };
    base.has_one_ref = [](cef_base_ref_counted_t* b) { return Self(b)->references_ == 1 ? 1 : 0; };
    base.has_at_least_one_ref = [](cef_base_ref_counted_t* b) { return Self(b)->references_ > 0 ? 1 : 0; };
    on_complete = [](alloy_site_settings_callback_t* c, alloy_site_settings_result_t result,
                     cef_string_list_t values) {
      Entries entries;
      for (size_t i = 0; i < cef_string_list_size(values); ++i) {
        cef_string_t value{}; cef_string_list_value(values, i, &value);
        entries.push_back(CefString(&value).ToString()); cef_string_clear(&value);
      }
      auto done = static_cast<CReply*>(c)->done_; done(result, std::move(entries));
    };
  }
 private:
  static CReply* Self(cef_base_ref_counted_t* b) {
    return static_cast<CReply*>(reinterpret_cast<alloy_site_settings_callback_t*>(b));
  }
  ~CReply() { --live_callbacks; }
  std::atomic<int> references_{1};
  const Done done_;
};

class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(int)> done) : done_(std::move(done)) { ++live_callbacks; }
  void OnComplete(alloy_site_settings_result_t result) override {
    auto done = done_; done(result);
  }
 private:
  ~LockReply() override { --live_callbacks; }
  const std::function<void(int)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};
class CLockReply final : public alloy_profile_lock_callback_t {
 public:
  explicit CLockReply(std::function<void(int)> done) : done_(std::move(done)) {
    ++live_callbacks;
    base.size = sizeof(alloy_profile_lock_callback_t);
    base.add_ref = [](cef_base_ref_counted_t* b) { ++Self(b)->references_; };
    base.release = [](cef_base_ref_counted_t* b) {
      auto* self = Self(b); if (--self->references_) return 0; delete self; return 1;
    };
    base.has_one_ref = [](cef_base_ref_counted_t* b) { return Self(b)->references_ == 1 ? 1 : 0; };
    base.has_at_least_one_ref = [](cef_base_ref_counted_t* b) { return Self(b)->references_ > 0 ? 1 : 0; };
    on_complete = [](alloy_profile_lock_callback_t* callback, alloy_site_settings_result_t result) {
      auto done = static_cast<CLockReply*>(callback)->done_; done(result);
    };
  }
 private:
  static CLockReply* Self(cef_base_ref_counted_t* b) {
    return static_cast<CLockReply*>(reinterpret_cast<alloy_profile_lock_callback_t*>(b));
  }
  ~CLockReply() { --live_callbacks; }
  std::atomic<int> references_{1};
  const std::function<void(int)> done_;
};

class UITask final : public CefTask {
 public:
  explicit UITask(std::function<void()> task) : task_(std::move(task)) {}
  void Execute() override { task_(); }
 private:
  ~UITask() override = default;
  const std::function<void()> task_;
  IMPLEMENT_REFCOUNTING(UITask);
};

class App final : public CefApp, public CefBrowserProcessHandler {
 public:
  App(std::string mode, std::string path) : mode_(std::move(mode)), raw_(mode_.starts_with("c-")) {
    const bool create = mode_ == "c-write";
    if (raw_) {
      CefString name(path), password(kPassword);
      c_profile_ = create ? alloy_create_profile(name.GetStruct(), password.GetStruct())
                          : alloy_open_profile(name.GetStruct());
      Check(c_profile_, "c-create-open");
      if (c_profile_ && !create) Check(c_profile_->unlock_with_password(c_profile_,
          CefString(mode_ == "c-wrong" ? "wrong-password" : kPassword).GetStruct()) ==
          (mode_ != "c-wrong"), "c-unlock");
    } else {
      profile_ = AlloyOpenProfile(path); Check(static_cast<bool>(profile_), "cpp-open");
      if (profile_) Check(profile_->UnlockWithPassword(kPassword), "cpp-unlock");
    }
  }
  bool valid() const { return c_profile_ || profile_; }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  bool Get(const char* site, Done done) {
    if (raw_) {
      CefString outer(site);
      return c_profile_->get_first_party_embeds(c_profile_, outer.GetStruct(), new CReply(std::move(done)));
    }
    return profile_->GetFirstPartyEmbeds(site, new Reply(std::move(done)));
  }
  bool Set(const char* site, const Entries& entries, Done done) {
    if (raw_) {
      CefString outer(site); auto list = cef_string_list_alloc();
      for (const auto& entry : entries) { CefString value(entry); cef_string_list_append(list, value.GetStruct()); }
      const bool accepted = c_profile_->set_first_party_embeds(
          c_profile_, outer.GetStruct(), list, new CReply(std::move(done)));
      cef_string_list_free(list); return accepted;
    }
    std::vector<CefString> values; for (const auto& entry : entries) values.emplace_back(entry);
    return profile_->SetFirstPartyEmbeds(site, values, new Reply(std::move(done)));
  }
  void OnContextInitialized() override {
    Check(CefCurrentlyOn(TID_UI), "context-on-ui");
    if (mode_ == "c-wrong") {
      Check(Get(kOuter, [this](int result, Entries entries) {
        Check(result == ALLOY_SETTINGS_CANCELLED && entries.empty(), "wrong-password-read-refused"); Finish();
      }), "wrong-accepted");
    } else if (mode_ == "c-write") {
      Check(Set(kOuter, {"https://beta.test/", "http://two.beta.test/", "https://delta.test/"},
          [this](int result, Entries entries) {
            Check(result == ALLOY_SETTINGS_OK && entries.empty(), "c-first-durable");
            Check(Set(kOther, {"https://beta.test/"}, [this](int result, Entries) {
              Check(result == ALLOY_SETTINGS_OK, "c-second-durable"); ReadBoth();
            }), "c-second-accepted");
          }), "c-first-accepted");
    } else ReadBoth();
  }
  void ReadBoth() {
    Check(Get(kOuter, [this](int result, Entries entries) {
      Check(result == ALLOY_SETTINGS_OK && entries == Entries{"beta.test", "delta.test"}, "cold-first-list");
      Check(Get(kOther, [this](int result, Entries entries) {
        Check(result == ALLOY_SETTINGS_OK && entries == Entries{"beta.test"}, "cold-second-list");
        Check(Set(kOuter, {"https://gamma.test/", "data:text/plain,invalid"},
            [this](int result, Entries entries) {
              Check(result == ALLOY_SETTINGS_INVALID && entries.empty(), "invalid-list-refused");
              Check(Get(kOuter, [this](int result, Entries entries) {
                Check(result == ALLOY_SETTINGS_OK && entries == Entries{"beta.test", "delta.test"},
                    "invalid-preserves-prior-list"); CheckEmpty();
              }), "post-invalid-read-accepted");
            }), "invalid-input-accepted-for-validation");
      }), "second-read-accepted");
    }), "first-read-accepted");
  }
  void CheckEmpty() {
    Check(Set(kOther, {}, [this](int result, Entries entries) {
      Check(result == ALLOY_SETTINGS_OK && entries.empty(), "empty-list-durable");
      Check(Get(kOther, [this](int result, Entries entries) {
        Check(result == ALLOY_SETTINGS_OK && entries.empty(), "empty-list-readback");
        Check(Set(kOther, {"https://beta.test/"}, [this](int result, Entries) {
          Check(result == ALLOY_SETTINGS_OK, "restore-other-list"); Finish();
        }), "restore-accepted");
      }), "empty-read-accepted");
    }), "empty-list-accepted");
  }
  void Finish() {
    // Existing key-source lifecycle is blocking and explicitly background-only.
    // No join or filesystem work executes inside the UI callback.
    threads_.emplace_back([this] {
      auto called = std::make_shared<std::atomic<bool>>(false);
      const bool wrong_read_refused = !Get(kOuter, [called](int, Entries) { *called = true; });
      const bool wrong_write_refused = !Set(kOther, {}, [called](int, Entries) { *called = true; });
      CefPostTask(TID_UI, new UITask([this, called, wrong_read_refused, wrong_write_refused] {
        Check(wrong_read_refused && wrong_write_refused && !*called, "wrong-thread-not-queued");
        auto locked = [this](int result) {
          Check(CefCurrentlyOn(TID_UI), "lock-callback-ui");
          Check(result == ALLOY_SETTINGS_OK, "async-lock-complete");
          const auto state = raw_ ? c_profile_->get_state(c_profile_) : profile_->GetState();
          Check(state == ALLOY_PROFILE_LOCKED, "locked-state-at-completion");
          Check(Get(kOuter, [this](int result, Entries entries) {
            Check(result == ALLOY_SETTINGS_CANCELLED && entries.empty(), "locked-read-cancelled");
            if (mode_ == "c-wrong") CefQuitMessageLoop();
            else Reunlock();
          }), "locked-read-accepted");
        };
        const bool accepted = raw_ ? c_profile_->lock_async(c_profile_, new CLockReply(std::move(locked)))
                                   : profile_->LockAsync(new LockReply(std::move(locked)));
        Check(accepted, "async-lock-accepted");
      }));
    });
  }
  void Reunlock() {
    threads_.emplace_back([this] {
      const bool unlocked = raw_ ? c_profile_->unlock_with_password(
          c_profile_, CefString(kPassword).GetStruct()) : profile_->UnlockWithPassword(kPassword);
      CefPostTask(TID_UI, new UITask([this, unlocked] {
        Check(unlocked, "same-profile-background-unlock");
        Check(Get(kOuter, [this](int result, Entries entries) {
          Check(result == ALLOY_SETTINGS_OK && entries == Entries{"beta.test", "delta.test"},
              "fresh-unlocked-projection");
          threads_.emplace_back([this] {
            const bool locked = raw_ ? c_profile_->lock(c_profile_) : profile_->Lock();
            CefPostTask(TID_UI, new UITask([locked] {
              Check(locked, "final-background-lock"); CefQuitMessageLoop();
            }));
          });
        }), "fresh-generation-read-accepted");
      }));
    });
  }
  void BeforeInitialization() {
    bool called = false;
    Check(!Get(kOuter, [&](int, Entries) { called = true; }), "pre-init-not-queued");
    Check(!called, "pre-init-no-callback");
  }
  void Close() {
    for (auto& thread : threads_) thread.join();
    profile_ = nullptr;
    if (c_profile_) { c_profile_->base.release(&c_profile_->base); c_profile_ = nullptr; }
    Check(live_callbacks == 0, "callback-references-released");
  }
 private:
  ~App() override = default;
  const std::string mode_;
  const bool raw_;
  alloy_profile_t* c_profile_ = nullptr;
  CefRefPtr<AlloyProfile> profile_;
  std::vector<std::thread> threads_;
  IMPLEMENT_REFCOUNTING(App);
};
}  // namespace

int main(int argc, char** argv) {
  Check(cef_api_hash(CEF_API_VERSION, 0) != nullptr, "api-version");
  CefMainArgs args(argc, argv);
  const int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0) return child;
  const char* mode = std::getenv("ALLOY_SETTINGS_MODE");
  const char* path = std::getenv("ALLOY_SETTINGS_PROFILE");
  const char* runtime = std::getenv("ALLOY_SETTINGS_RUNTIME");
  if (!mode || !path || !runtime) return 2;
  const std::string owned_mode(mode), owned_runtime(runtime);
  CefRefPtr<App> app = new App(owned_mode, path);
  if (!app->valid()) return 1;
  app->BeforeInitialization();
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  CefString(&settings.root_cache_path) = owned_runtime;
  CefString(&settings.cache_path) = owned_runtime;
  if (!CefInitialize(args, settings, app, nullptr)) return 3;
  CefRunMessageLoop();
  app->Close();
  CefDoMessageLoopWork();
  CefShutdown();
  if (failures) return 1;
  std::printf("PASS site-settings-api-%s\n", owned_mode.c_str());
  return 0;
}
