// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cef/include/alloy/alloy_profile.h"
#include "cef/include/alloy/alloy_request_context.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include <utility>
#include <vector>

#include "brave/alloy/site_records/encrypted_profile.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/alloy/site_records/site_profile_configuration.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/alloy/site_records/secret_service_key_source.h"
#include "brave/alloy/site_records/profile_auto_lock.h"
#include "brave/alloy/site_records/linux_lock_signals.h"
#include "base/callback_list.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/barrier_closure.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_restrictions.h"
#include "cef/libcef/browser/alloy/profile_settings.h"
#include "cef/libcef/browser/thread_util.h"
#include "cef/libcef/browser/browser_context.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "cef/libcef/browser/browser_info_manager.h"
#include "cef/libcef/browser/chrome/chrome_browser_context.h"
#include "content/public/browser/web_contents.h"
#include "cef/libcef/browser/context.h"
#include "cef/libcef/browser/request_context_impl.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/history/history_service_factory.h"
#include "components/history/core/browser/history_service.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/alloy_site_retirement.h"
#include "content/public/browser/alloy_network_retirement.h"
#include "content/public/browser/browser_context.h"
#include "content/browser/alloy_embedded_state.h"
#include "cef/libcef/common/api_version_util.h"
#include "third_party/icu/source/common/unicode/ustring.h"
#include "url/gurl.h"

namespace {
namespace sr = alloy::site_records;

alloy_keyring_result_t KeyringResult(sr::FileStatus status,
                                    const sr::SecretServiceKeySource& source) {
  if (status == sr::FileStatus::kOk) return ALLOY_KEYRING_OK;
  switch (source.result()) {
    case sr::KeyringResult::kUnavailable: return ALLOY_KEYRING_UNAVAILABLE;
    case sr::KeyringResult::kLocked: return ALLOY_KEYRING_LOCKED;
    case sr::KeyringResult::kMissing: return ALLOY_KEYRING_MISSING;
    case sr::KeyringResult::kOk:
    case sr::KeyringResult::kFailed: return ALLOY_KEYRING_FAILED;
  }
  return ALLOY_KEYRING_FAILED;
}

std::unique_ptr<sr::PasswordKeySource> PasswordSource(const CefString& password) {
  // Convert into one caller-sized buffer. CefString::ToString and the generic
  // shrinking string conversion helpers create freed, uncleansed UTF-8 copies.
  // ICU writes directly into this owned buffer and rejects malformed UTF-16.
  if (password.length() > static_cast<size_t>(std::numeric_limits<int32_t>::max() / 3)) {
    return nullptr;
  }
  std::vector<uint8_t> utf8(password.length() * 3 + 1, 0);
  int32_t length = 0;
  UErrorCode error = U_ZERO_ERROR;
  u_strToUTF8(reinterpret_cast<char*>(utf8.data()),
      static_cast<int32_t>(utf8.size()), &length,
      password.c_str(), static_cast<int32_t>(password.length()), &error);
  std::unique_ptr<sr::PasswordKeySource> source;
  if (U_SUCCESS(error)) {
    source = std::make_unique<sr::PasswordKeySource>(
        base::span(utf8).first(static_cast<size_t>(length)));
  }
  sr::Cleanse(utf8);
  return source;
}

class ExternalKeySource final : public sr::KeySource {
 public:
  explicit ExternalKeySource(CefRefPtr<AlloyKeySource> source)
      : source_(std::move(source)) {}
  sr::KeySourceKind kind() const override { return sr::KeySourceKind::kExternal; }
  sr::FileStatus Derive(const sr::KeySourceParameters& parameters,
                        sr::SecretKey* key) override {
    key->Clear();
    if (!source_ || parameters.kind != kind()) {
      return sr::FileStatus::kDenied;
    }
    sr::ObjectId output{};
    const bool success = source_->DeriveKey(parameters.salt.data(),
        parameters.salt.size(), output.data(), output.size());
    if (success) {
      *key = sr::SecretKey(output);
    }
    sr::Cleanse(output);
    return success ? sr::FileStatus::kOk : sr::FileStatus::kDenied;
  }
 private:
  CefRefPtr<AlloyKeySource> source_;
};

alloy_site_settings_result_t SettingsResult(sr::FileStatus status) {
  switch (status) {
    case sr::FileStatus::kOk: return ALLOY_SETTINGS_OK;
    case sr::FileStatus::kDenied:
    case sr::FileStatus::kCancelled: return ALLOY_SETTINGS_CANCELLED;
    case sr::FileStatus::kInvalid: return ALLOY_SETTINGS_INVALID;
    case sr::FileStatus::kBusy: return ALLOY_SETTINGS_CONFLICT;
    case sr::FileStatus::kIntegrity: return ALLOY_SETTINGS_INTEGRITY;
    default: return ALLOY_SETTINGS_IO;
  }
}

void RetireNativeHistory(content::BrowserContext* context, const GURL& site,
                         base::OnceCallback<void(bool)> done) {
  auto* profile = Profile::FromBrowserContext(context);
  auto* original = profile->GetOriginalProfile();
  if (original != profile) {
    const auto* input = sr::NativeProfileConfiguration::Get(*profile);
    const auto* parent = sr::NativeProfileConfiguration::Get(*original);
    if (!input || !parent || !input->SharesOwnerWith(*parent)) {
      std::move(done).Run(false); return;
    }
  }
  // Never construct a dormant history backend merely to delete a record.
  auto* service = HistoryServiceFactory::GetForProfileIfExists(
      profile, ServiceAccessType::EXPLICIT_ACCESS);
  if (!service) { std::move(done).Run(true); return; }
  const auto target = *net::AlloyNetworkRecordTarget::FromPrincipal(site, url::Origin::Create(site));
  service->RetireAlloyRecord(target.domain(), std::move(done));
}

void RetireNativeSite(scoped_refptr<sr::SitePrefStore> prefs, GURL site,
                      sr::SitePrefStore::Completion done) {
  CEF_REQUIRE_UIT();
  const auto target = net::AlloyNetworkRecordTarget::FromPrincipal(
      site, url::Origin::Create(site));
  if (!target) { std::move(done).Run(sr::FileStatus::kInvalid); return; }
  std::vector<content::BrowserContext*> native;
  std::vector<CefRefPtr<CefRequestContextImpl>> keepalive;
  for (auto* context : CefBrowserContext::GetAll()) {
    auto* browser_context = context->AsBrowserContext();
    auto* input = browser_context ? sr::NativeProfileConfiguration::Get(*browser_context) : nullptr;
    if (!input || sr::GetSiteProfileSettings(*input).get() != prefs.get()) continue;
    prefs->DeferNativeRetirementNotification(
        content::BeginAlloyFirstPartyRetirement(*browser_context, site));
    native.push_back(browser_context);
    keepalive.push_back(context->GetAnyRequestContext(false));
  }
  auto success = std::make_shared<bool>(true);
  // One completion from each site owner and each network owner, plus the
  // commit-iteration gate. A synchronous drain cannot release our keepalives.
  auto completed = base::BarrierClosure(native.size() * 2 + 1, base::BindOnce(
      [](std::vector<CefRefPtr<CefRequestContextImpl>> keepalive,
         std::vector<content::BrowserContext*> contexts, GURL site,
         std::shared_ptr<bool> success,
         sr::SitePrefStore::Completion done) {
        // No clear while old writers can still refill a context-wide backend.
        // Keep replacement admission closed until native completion AND the
        // coordinator's FinishRecordDeletion/publication, not just scheduling.
        auto removed = base::BarrierClosure(contexts.size() * 2, base::BindOnce(
            [](std::vector<CefRefPtr<CefRequestContextImpl>>,
               std::shared_ptr<bool> success, sr::SitePrefStore::Completion done) {
              std::move(done).Run(*success ? sr::FileStatus::kOk : sr::FileStatus::kIo);
            }, std::move(keepalive), success, std::move(done)));
        for (auto* context : contexts) {
          auto result = base::BindRepeating([](std::shared_ptr<bool> success,
                                               base::RepeatingClosure removed, bool ok) {
            *success = *success && ok; removed.Run();
          }, success, removed);
          RetireNativeHistory(context, site, result);
          content::RetireAlloyFirstPartyStores(context, site, result);
        }
      }, std::move(keepalive), native, site, success, std::move(done)));
  auto owns = base::BindRepeating([](net::AlloyNetworkRecordTarget target,
                                    const url::Origin& origin) {
    return target.Owns(origin);
  }, *target);
  auto close = base::BindRepeating([](content::WebContents* contents) {
    auto browser = CefBrowserHostBase::GetBrowserForContents(contents);
    CHECK(browser); browser->CloseBrowser(true);
  });
  auto networks = std::make_shared<std::vector<base::OnceClosure>>();
  auto sites = std::make_shared<std::vector<base::OnceClosure>>();
  auto prepared = base::BarrierClosure(native.size() + 1, base::BindOnce(
      [](std::vector<content::BrowserContext*> contexts, GURL site,
         std::shared_ptr<std::vector<base::OnceClosure>> networks,
         std::shared_ptr<std::vector<base::OnceClosure>> sites,
         base::OnceClosure committed) {
        auto network_commits = std::move(*networks);
        auto site_commits = std::move(*sites);
        for (auto& commit : network_commits) std::move(commit).Run();
        for (auto* context : contexts)
          content::RevokeAlloyEmbeddedSite(*context, site, url::Origin::Create(site));
        for (auto& commit : site_commits) std::move(commit).Run();
        std::move(committed).Run();
      }, native, site, networks, sites, completed));
  // Capture/retain every original network endpoint before document preparation
  // shuts down CUT admission. No native commit runs during either prepare pass.
  for (auto* context : native) {
    content::PrepareAlloyNetworkServiceRetirement(context, *target,
        base::BindOnce([](std::shared_ptr<std::vector<base::OnceClosure>> networks,
                          base::OnceClosure prepared, base::OnceClosure commit) {
          networks->push_back(std::move(commit)); std::move(prepared).Run();
        }, networks, prepared),
        base::BindOnce([](std::shared_ptr<bool> success,
                          base::OnceClosure completed, bool ok) {
          *success = *success && ok; std::move(completed).Run();
        }, success, completed));
  }
  for (auto* context : native) sites->push_back(content::PrepareAlloySiteRetirement(context, owns, close,
      base::BindOnce([](scoped_refptr<sr::SitePrefStore> prefs, std::shared_ptr<bool> success,
                       base::OnceClosure completed, bool ok, base::OnceClosure publication) {
        prefs->DeferNativeRetirementNotification(std::move(publication));
        *success = *success && ok; std::move(completed).Run();
      }, prefs, success, completed)));
  // This final gate also covers the no-context / synchronous-prepare cases.
  prepared.Run();
}

class SettingsOwner final : public CefBaseRefCounted {
 public:
  SettingsOwner(std::shared_ptr<sr::RecordFileSystem> files,
                scoped_refptr<base::SequencedTaskRunner> runner,
                base::RepeatingCallback<bool()> admitted, base::RepeatingClosure fail_closed)
      : files_(std::move(files)), runner_(std::move(runner)), admitted_(std::move(admitted)),
        fail_closed_(std::move(fail_closed)) {
    prefs_ = base::MakeRefCounted<sr::SitePrefStore>(files_,
        base::BindRepeating(&SettingsOwner::OnError, weak_factory_.GetWeakPtr()), runner_, admitted_);
  }
  ~SettingsOwner() override {
    CEF_REQUIRE_UIT(); prefs_->Invalidate();
    if (filters_) filters_->Close({});
  }
  bool Matches(const std::shared_ptr<sr::RecordFileSystem>& files) const { return files == files_; }
  bool navigation_open() const { return !retiring_; }
  void StopNavigations() { CEF_REQUIRE_UIT(); retiring_ = true; }
  scoped_refptr<sr::SitePrefStore> prefs() const { return prefs_; }
  sr::FileStatus failure() const { return failure_; }
  using FiltersReady = sr::ProfileFilterPreferences::Loaded;
  void PrepareFilters(FiltersReady ready) {
    CEF_REQUIRE_UIT();
    // Enforce asynchronous delivery at the callback boundary, including storage
    // creation failure and a refused Load task during shutdown.
    ready = base::BindPostTask(CEF_TASK_RUNNER<CEF_UIT>(), std::move(ready));
    if (filters_ && filters_->available()) {
      CEF_POST_TASK(CEF_UIT, base::BindOnce(
          [](scoped_refptr<sr::ProfileFilterPreferences> filters, FiltersReady ready) {
            const auto status = filters->available() ? sr::FileStatus::kOk : sr::FileStatus::kCancelled;
            std::move(ready).Run(status, status == sr::FileStatus::kOk ? filters : nullptr);
          }, filters_, std::move(ready)));
      return;
    }
    filters_ready_.push_back(std::move(ready));
    if (filters_loading_) return;
    std::shared_ptr<sr::ProfileFilterStorage> storage;
    const auto status = sr::ProfileFilterStorage::Create(files_,
        files_->ProfileCapability(sr::StorePurpose::kProfileCustomFilters), &storage);
    if (status != sr::FileStatus::kOk) { FiltersLoaded(status, nullptr); return; }
    filters_loading_ = true;
    sr::ProfileFilterPreferences::Load(storage, runner_,
        base::BindRepeating(&SettingsOwner::OnError, weak_factory_.GetWeakPtr()),
        base::BindOnce([](CefRefPtr<SettingsOwner> owner, sr::FileStatus status,
                          scoped_refptr<sr::ProfileFilterPreferences> filters) {
          owner->FiltersLoaded(status, std::move(filters));
        }, CefRefPtr<SettingsOwner>(this)), admitted_);
  }
  void CloseCaches(base::OnceClosure done) {
    CEF_REQUIRE_UIT();
    // Native keyed services/constructors must release borrowed values first.
    // Prepared-only inputs are also covered: their retained filter cache must
    // be wiped even though no native service ever consumed it.
    prefs_->CloseNativeCacheAsync(base::BindOnce(
        [](CefRefPtr<SettingsOwner> owner, base::OnceClosure done) {
          if (!owner->filters_) { if (done) std::move(done).Run(); return; }
          owner->filters_->Close(base::BindOnce(
              [](CefRefPtr<SettingsOwner> owner, base::OnceClosure done, sr::FileStatus status) {
                if (status != sr::FileStatus::kOk && owner->failure_ == sr::FileStatus::kOk)
                  owner->failure_ = status;
                if (done) std::move(done).Run();
              }, owner, std::move(done)));
        }, CefRefPtr<SettingsOwner>(this), std::move(done)));
  }
 private:
  void FiltersLoaded(sr::FileStatus status, scoped_refptr<sr::ProfileFilterPreferences> filters) {
    CEF_REQUIRE_UIT(); filters_loading_ = false;
    if (status == sr::FileStatus::kOk) filters_ = std::move(filters);
    auto callbacks = std::move(filters_ready_);
    for (auto& callback : callbacks) {
      const auto result = status == sr::FileStatus::kOk && (!filters_ || !filters_->available()) ?
          sr::FileStatus::kCancelled : status;
      std::move(callback).Run(result, result == sr::FileStatus::kOk ? filters_ : nullptr);
    }
  }
  void OnError(sr::FileStatus status) {
    CEF_REQUIRE_UIT();
    if (status == sr::FileStatus::kCancelled || status == sr::FileStatus::kDenied) return;
    if (status != sr::FileStatus::kCancelled && status != sr::FileStatus::kDenied &&
        failure_ == sr::FileStatus::kOk) failure_ = status;
    // Storage failures use the same profile barrier, never wipe underneath a
    // live WebContents or a native service borrowing this generation's cache.
    CEF_POST_TASK(CEF_UIT, fail_closed_);
  }
  const std::shared_ptr<sr::RecordFileSystem> files_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  const base::RepeatingCallback<bool()> admitted_;
  const base::RepeatingClosure fail_closed_;
  scoped_refptr<sr::SitePrefStore> prefs_;
  scoped_refptr<sr::ProfileFilterPreferences> filters_;
  bool filters_loading_ = false;
  std::vector<FiltersReady> filters_ready_;
  sr::FileStatus failure_ = sr::FileStatus::kOk;
  bool retiring_ = false;
  base::WeakPtrFactory<SettingsOwner> weak_factory_{this};
  IMPLEMENT_REFCOUNTING_DELETE_ON_UIT(SettingsOwner);
};

constexpr int kEngineLockTriggers = (1 << ALLOY_LOCK_BROWSER_CLOSE) | (1 << ALLOY_LOCK_IDLE);

// One reconciled producer per process/session, not one NameOwnerChanged handler
// per profile on the shared D-Bus proxies. It contains no profile or key state.
class SystemLockSource final : public CefContext::Observer {
 public:
  static SystemLockSource& Get() {
    static base::NoDestructor<SystemLockSource> source;
    return *source;
  }
  void Start() {
    if (signals_ || !CONTEXT_STATE_VALID()) return;
    CefContext::Get()->AddObserver(this);
    signals_ = std::make_unique<sr::LinuxLockSignals>(
        dbus_thread_linux::GetSharedSystemBus(), dbus_thread_linux::GetSharedSessionBus(),
        base::BindRepeating([](sr::ProfileLockReason reason) { Get().events.Notify(reason); }),
        base::BindRepeating([](int mask) { Get().available.Notify(mask | kEngineLockTriggers); }));
    signals_->Start();
  }
  int mask() const { return kEngineLockTriggers | (signals_ ? signals_->available() : 0); }
  void OnContextDestroyed() override {
    signals_.reset();
    CefContext::Get()->RemoveObserver(this);
  }
  base::RepeatingCallbackList<void(sr::ProfileLockReason)> events;
  base::RepeatingCallbackList<void(int)> available;
 private:
  std::unique_ptr<sr::LinuxLockSignals> signals_;
};

// Runtime state is UI-owned, while the profile's blocking crypto/file lifecycle
// remains on its sequenced runner. Timer/signal callbacks hold weak runtime
// references, never a profile -> callback -> profile reference cycle.
struct ProfileRuntime final : public CefContext::Observer,
                              public std::enable_shared_from_this<ProfileRuntime> {
  ProfileRuntime(std::shared_ptr<sr::EncryptedProfile> profile,
                 std::shared_ptr<std::atomic<bool>> pending,
                 scoped_refptr<base::SequencedTaskRunner> runner)
      : profile(std::move(profile)), pending(std::move(pending)), runner(std::move(runner)) {}
  ~ProfileRuntime() override {
    if (!closed) { CEF_REQUIRE_UIT(); }
    if (observing) CefContext::Get()->RemoveObserver(this);
    // Last-reference release must never durably lock a coordinator on UI.
    runner->PostTask(FROM_HERE, base::BindOnce([](std::shared_ptr<sr::EncryptedProfile>) {}, std::move(profile)));
  }
  void Initialize() {
    CEF_REQUIRE_UIT();
    auto weak = weak_from_this();
    automatic = std::make_unique<sr::ProfileAutoLock>(base::BindRepeating(
        [](std::weak_ptr<ProfileRuntime> weak) {
          auto self = weak.lock();
          auto files = self && !self->pending->load() ? self->profile->files() : nullptr;
          if (!files || !files->unlocked()) return base::UnguessableToken();
          if (self->idle_files.lock() != files) {
            self->idle_files = files;
            self->idle_generation = base::UnguessableToken::Create();
          }
          return self->idle_generation;
        }, weak), base::BindRepeating([](std::weak_ptr<ProfileRuntime> weak, sr::ProfileLockReason reason) {
          if (auto self = weak.lock()) self->AutomaticLock(reason);
        }, weak));
    auto& source = SystemLockSource::Get();
    // Subscribe before starting initial-state reads.
    signal_subscription = source.events.Add(base::BindRepeating(
        [](std::weak_ptr<ProfileRuntime> weak, sr::ProfileLockReason reason) {
          if (auto self = weak.lock()) self->automatic->Signal(reason);
        }, weak));
    available_subscription = source.available.Add(base::BindRepeating(
        [](std::weak_ptr<ProfileRuntime> weak, int mask) {
          if (auto self = weak.lock(); self && self->handler) {
            auto handler = self->handler; handler->OnLockTriggersChanged(mask);
          }
        }, weak));
    if (CONTEXT_STATE_VALID()) {
      observing = true; CefContext::Get()->AddObserver(this); source.Start();
    }
  }
  void AutomaticLock(sr::ProfileLockReason reason) {
    if (profile->state() == sr::ProfileState::kLocked || pending->exchange(true)) return;
    if (!StartLock(reason, true, {})) *pending = false;
  }
  bool StartLock(sr::ProfileLockReason reason, bool retry_busy,
                  base::OnceCallback<void(sr::FileStatus)> completion) {
    CEF_REQUIRE_UIT();
    if (ui_lock_in_flight) {
      if (completion) lock_waiters.push_back(std::move(completion));
      return true;
    }
    // This entry can run inside any native observer or embedder callback. Cut
    // navigation admission now, but never destroy that callback's owner inline.
    if (settings) settings->StopNavigations();
    ui_lock_in_flight = true;
    if (base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&ProfileRuntime::BeginLockTeardown, shared_from_this(),
                       reason, retry_busy, std::move(completion)))) return true;
    ui_lock_in_flight = false;
    return false;
  }
  void BeginLockTeardown(sr::ProfileLockReason reason, bool retry_busy,
                         base::OnceCallback<void(sr::FileStatus)> completion) {
    CEF_REQUIRE_UIT();
    if (!settings) {
      WipeKeys(reason, retry_busy, std::move(completion));
      return;
    }
    // Navigation and native service admission are distinct boundaries. Existing
    // observers and service destructors retain their original admitted settings.
    std::vector<CefRefPtr<CefRequestContextImpl>> keepalive;
    std::vector<ChromeBrowserContext*> contexts;
    std::vector<base::WeakPtr<sr::NativeProfileConfiguration>> native;
    std::vector<CefRefPtr<CefBrowserHostBase>> browsers;
    for (auto* context : CefBrowserContext::GetAll()) {
      auto* browser_context = context->AsBrowserContext();
      auto* input = browser_context ? sr::NativeProfileConfiguration::Get(*browser_context) : nullptr;
      if (!input || sr::GetSiteProfileSettings(*input) != settings->prefs()) continue;
      contexts.push_back(static_cast<ChromeBrowserContext*>(context));
      keepalive.push_back(context->GetAnyRequestContext(false));
      native.push_back(input->GetWeakPtr());
      for (const auto& info : CefBrowserInfoManager::GetInstance()->GetBrowserInfoList()) {
        auto browser = info->browser();
        auto* contents = browser ? browser->GetWebContents() : nullptr;
        if (contents && contents->GetBrowserContext() == browser_context) browsers.push_back(browser);
      }
    }
    // Security retirement is not an embedder-vetoable window-close request.
    for (const auto& browser : browsers) if (browser->GetWebContents()) browser->DestroyBrowser();
    for (auto* context : contexts) context->RetireForProfileLock();
    keepalive.clear();
    AwaitNativeTeardown(std::move(native), reason, retry_busy, std::move(completion));
  }
  void AwaitNativeTeardown(std::vector<base::WeakPtr<sr::NativeProfileConfiguration>> native,
                           sr::ProfileLockReason reason, bool retry_busy,
                           base::OnceCallback<void(sr::FileStatus)> completion) {
    std::erase_if(native, [](const auto& input) { return !input; });
    if (!native.empty()) {
      retry.Start(FROM_HERE, base::Milliseconds(10), base::BindOnce(
          &ProfileRuntime::AwaitNativeTeardown, shared_from_this(), std::move(native),
          reason, retry_busy, std::move(completion)));
      return;
    }
    // Native consumers have sent their teardown notifications. Drain the same
    // write journal before closing cache/service admission and wiping keys.
    settings->prefs()->FlushAsync(base::BindOnce(
        [](std::shared_ptr<ProfileRuntime> self, sr::ProfileLockReason reason,
           bool retry_busy, base::OnceCallback<void(sr::FileStatus)> completion, sr::FileStatus status) {
          self->flush_status = status;
          self->settings->CloseCaches(base::BindOnce(
              [](std::shared_ptr<ProfileRuntime> self, sr::ProfileLockReason reason,
                 bool retry_busy, base::OnceCallback<void(sr::FileStatus)> completion) {
                *self->services_admitted = false;
                self->WipeKeys(reason, retry_busy, std::move(completion));
              }, self, reason, retry_busy, std::move(completion)));
        }, shared_from_this(), reason, retry_busy, std::move(completion)));
  }
  void WipeKeys(sr::ProfileLockReason reason, bool retry_busy,
                base::OnceCallback<void(sr::FileStatus)> completion) {
    auto self = shared_from_this();
    auto ui = base::SingleThreadTaskRunner::GetCurrentDefault();
    auto replies = base::SplitOnceCallback(base::BindOnce(
        [](std::shared_ptr<ProfileRuntime> self, sr::ProfileLockReason reason,
           bool retry_busy, base::OnceCallback<void(sr::FileStatus)> completion, sr::FileStatus status) {
          self->FinishLock(reason, retry_busy, status, std::move(completion));
        }, self, reason, retry_busy, std::move(completion)));
    if (!runner->PostTask(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ProfileRuntime> self, scoped_refptr<base::SingleThreadTaskRunner> ui,
           base::OnceCallback<void(sr::FileStatus)> reply) {
          const auto result = self->profile->Lock();
          ui->PostTask(FROM_HERE, base::BindOnce(std::move(reply), result));
        }, self, ui, std::move(replies.first))))
      std::move(replies.second).Run(sr::FileStatus::kCancelled);
  }
  void FinishLock(sr::ProfileLockReason reason, bool retry_busy, sr::FileStatus result,
                  base::OnceCallback<void(sr::FileStatus)> completion) {
    CEF_REQUIRE_UIT();
    if (retry_busy && result == sr::FileStatus::kBusy) {
      // Lock has cancelled the original unlock. Keep admission closed until
      // its provider returns and every temporary key is wiped, then acknowledge.
      retry.Start(FROM_HERE, base::Milliseconds(10), base::BindOnce(
          [](std::shared_ptr<ProfileRuntime> self, sr::ProfileLockReason reason,
             base::OnceCallback<void(sr::FileStatus)> completion) {
            self->WipeKeys(reason, true, std::move(completion));
          }, shared_from_this(), reason, std::move(completion)));
      return;
    }
    auto completed = base::BindOnce(
        [](std::shared_ptr<ProfileRuntime> self, sr::ProfileLockReason reason,
           sr::FileStatus result, base::OnceCallback<void(sr::FileStatus)> completion) {
          auto joined = std::move(self->lock_waiters);
          auto shutdown_done = std::move(self->shutdown_done);
          if (result == sr::FileStatus::kOk && self->settings &&
              self->settings->failure() != sr::FileStatus::kOk) result = self->settings->failure();
          if (result == sr::FileStatus::kOk) result = self->flush_status;
          self->settings = nullptr;
          self->services_admitted.reset();
          self->flush_status = sr::FileStatus::kOk;
          self->has_settings = false;
          self->ui_lock_in_flight = false;
          *self->pending = false;
          auto handler = self->handler;
          if (handler) handler->OnLocked(static_cast<alloy_profile_lock_reason_t>(reason), SettingsResult(result));
          if (completion) std::move(completion).Run(result);
          for (auto& waiter : joined) std::move(waiter).Run(result);
          if (shutdown_done) std::move(shutdown_done).Run();
        }, shared_from_this(), reason, result, std::move(completion));
    CEF_POST_TASK(CEF_UIT, std::move(completed));
  }
  void StartBrowserCloseLockWhenIdle() {
    CEF_REQUIRE_UIT();
    if (pending->exchange(true)) {
      // Only a UI-owned transition reaches FinishLock and resolves shutdown_done.
      // A standalone Lock may even have started before this runtime existed.
      if (!ui_lock_in_flight) retry.Start(FROM_HERE, base::Milliseconds(10),
          base::BindOnce(&ProfileRuntime::StartBrowserCloseLockWhenIdle, shared_from_this()));
      return;
    }
    if (!StartLock(sr::ProfileLockReason::kBrowserClose, true, {})) {
      *pending = false;
      if (shutdown_done) std::move(shutdown_done).Run();
    }
  }
  void OnContextDestroyed() override {
    CEF_REQUIRE_UIT();
    if (observing) CefContext::Get()->RemoveObserver(this);
    observing = false;
    automatic.reset(); signal_subscription = {}; available_subscription = {};
    // A last-reference release may already have posted deletion to UI. In that
    // case the queued deleter, not this observer, owns the final blocking lock.
    auto keepalive = weak_from_this().lock();
    if (!keepalive) { closed = true; return; }
    // CefShutdown must not return with an externally retained unlocked profile.
    // UI is still pumping here and native retirement already uses event fences.
    base::RunLoop finished(base::RunLoop::Type::kNestableTasksAllowed);
    shutdown_done = finished.QuitClosure();
    StartBrowserCloseLockWhenIdle();
    finished.Run();
    handler = nullptr;
    closed = true;
  }
  std::shared_ptr<sr::EncryptedProfile> profile;
  const std::shared_ptr<std::atomic<bool>> pending;
  const scoped_refptr<base::SequencedTaskRunner> runner;
  CefRefPtr<SettingsOwner> settings;
  std::shared_ptr<std::atomic<bool>> services_admitted;
  std::weak_ptr<sr::RecordFileSystem> idle_files;
  base::UnguessableToken idle_generation;
  sr::FileStatus flush_status = sr::FileStatus::kOk;
  std::atomic<bool> has_settings{false};
  CefRefPtr<AlloyProfileLockHandler> handler;
  std::unique_ptr<sr::ProfileAutoLock> automatic;
  base::CallbackListSubscription signal_subscription, available_subscription;
  base::OneShotTimer retry;
  base::OnceClosure shutdown_done;
  std::vector<base::OnceCallback<void(sr::FileStatus)>> lock_waiters;
  bool observing = false;
  bool ui_lock_in_flight = false;
  std::atomic<bool> closed{false};
};

const char kProfileActivityKey = 0;
struct ProfileActivity final : base::SupportsUserData::Data {
  ProfileActivity(base::RepeatingClosure activity, CefRefPtr<SettingsOwner> settings)
      : activity(std::move(activity)), settings(std::move(settings)) {}
  const base::RepeatingClosure activity;
  const CefRefPtr<SettingsOwner> settings;
};

struct ProfileLifecycle {
  ProfileLifecycle(std::shared_ptr<sr::EncryptedProfile> profile,
                   std::shared_ptr<std::atomic<bool>> pending)
      : profile(std::move(profile)), pending(std::move(pending)) {}
  const std::shared_ptr<sr::EncryptedProfile> profile;
  const std::shared_ptr<std::atomic<bool>> pending;
  std::mutex mutex;
  scoped_refptr<base::SequencedTaskRunner> runner;
  scoped_refptr<base::SingleThreadTaskRunner> ui;
  std::shared_ptr<ProfileRuntime> runtime;
  base::RepeatingClosure standalone_lock_hook_for_testing;
};
std::mutex& ProfilesMutex() { static std::mutex mutex; return mutex; }
std::vector<std::weak_ptr<ProfileLifecycle>>& Profiles() {
  static base::NoDestructor<std::vector<std::weak_ptr<ProfileLifecycle>>> profiles;
  return *profiles;
}
scoped_refptr<base::SequencedTaskRunner> ProfileRunner(const std::shared_ptr<ProfileLifecycle>& state) {
  std::lock_guard lock(state->mutex);
  if (!state->runner) state->runner = base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
  return state->runner;
}
std::shared_ptr<ProfileRuntime> RuntimeFor(const std::shared_ptr<ProfileLifecycle>& state) {
  CEF_REQUIRE_UIT();
  auto runner = ProfileRunner(state);
  std::shared_ptr<ProfileRuntime> runtime;
  bool created = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->runtime) {
      state->ui = base::SingleThreadTaskRunner::GetCurrentDefault();
      state->runtime = std::shared_ptr<ProfileRuntime>(new ProfileRuntime(state->profile, state->pending, runner),
          [ui = state->ui](ProfileRuntime* value) {
            if (value->closed || ui->RunsTasksInCurrentSequence()) delete value;
            else ui->DeleteSoon(FROM_HERE, value);
          });
      created = true;
    }
    runtime = state->runtime;
  }
  if (created) runtime->Initialize();
  return runtime;
}
void RegisterLifecycle(const std::shared_ptr<ProfileLifecycle>& state) {
  {
    std::lock_guard lock(ProfilesMutex());
    std::erase_if(Profiles(), [](const auto& entry) { return entry.expired(); });
    Profiles().push_back(state);
  }
  if (CONTEXT_STATE_VALID()) CEF_POST_TASK(CEF_UIT, base::BindOnce(
      [](std::weak_ptr<ProfileLifecycle> weak) {
        if (CONTEXT_STATE_VALID()) if (auto state = weak.lock()) RuntimeFor(state);
      }, std::weak_ptr<ProfileLifecycle>(state)));
}

class AlloyProfileImpl final : public AlloyProfile {
 public:
  explicit AlloyProfileImpl(std::unique_ptr<sr::EncryptedProfile> profile)
      : profile_(std::move(profile)), lifecycle_(std::make_shared<ProfileLifecycle>(profile_, lock_pending_)) {
    RegisterLifecycle(lifecycle_);
  }
  bool UnlockWithPassword(const CefString& password) override {
    if (lock_pending_->load()) return false;
    auto source = PasswordSource(password);
    return source && profile_->Unlock(*source) == sr::FileStatus::kOk;
  }
  bool UnlockWithKeySource(CefRefPtr<AlloyKeySource> source) override {
    if (lock_pending_->load()) return false;
    ExternalKeySource bridge(std::move(source));
    return profile_->Unlock(bridge) == sr::FileStatus::kOk;
  }
  bool Lock() override {
    bool expected = false;
    if (!lock_pending_->compare_exchange_strong(expected, true)) return false;
    std::shared_ptr<ProfileRuntime> runtime;
    base::RepeatingClosure standalone_lock_hook;
    {
      std::lock_guard lock(lifecycle_->mutex);
      runtime = lifecycle_->runtime;
      standalone_lock_hook = lifecycle_->standalone_lock_hook_for_testing;
    }
    if (!runtime || !runtime->has_settings) {
      if (standalone_lock_hook) standalone_lock_hook.Run();
      const auto result = profile_->Lock();
      *lock_pending_ = false;
      // Preserve the standalone blocking contract: adopting a key-only profile
      // at CefInitialize must not make Lock depend on UI pumping. There is no
      // native plaintext cache to drain. Its transient event is queued on UI and
      // must not clear settings belonging to a subsequent explicit unlock.
      if (runtime) lifecycle_->ui->PostTask(FROM_HERE, base::BindOnce(
          [](std::shared_ptr<ProfileRuntime> runtime, sr::FileStatus result) {
            auto handler = runtime->handler;
            if (handler) handler->OnLocked(ALLOY_LOCK_EXPLICIT, SettingsResult(result));
          }, runtime, result));
      return result == sr::FileStatus::kOk;
    }
    if (CEF_CURRENTLY_ON_UIT() || runtime->runner->RunsTasksInCurrentSequence()) {
      *lock_pending_ = false;
      return false;
    }
    // The public lifecycle API is synchronous on an embedder background thread.
    // Native UI edits already entered this same queue; never wait for UI here.
    base::AssertBlockingAllowed();
    base::WaitableEvent finished;
    sr::FileStatus status = sr::FileStatus::kCancelled;
    auto completed = base::BindOnce(
        [](sr::FileStatus* status, base::WaitableEvent* finished, sr::FileStatus result) {
          *status = result; finished->Signal();
        }, &status, &finished);
    // Enter the same UI-owned runtime even for a background synchronous lock.
    if (!lifecycle_->ui->PostTask(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ProfileRuntime> runtime,
           base::OnceCallback<void(sr::FileStatus)> completion) {
          if (!runtime->StartLock(sr::ProfileLockReason::kExplicit, false, std::move(completion)))
            *runtime->pending = false;
        }, runtime, std::move(completed)))) {
      *lock_pending_ = false;
      return false;
    }
    finished.Wait();
    return status == sr::FileStatus::kOk;
  }
  bool LockAsync(CefRefPtr<AlloyProfileLockCallback> callback) override {
    if (!CEF_CURRENTLY_ON_UIT() || !callback) return false;
    auto runtime = Runtime();
    bool expected = false;
    if (!lock_pending_->compare_exchange_strong(expected, true) && !runtime->ui_lock_in_flight) return false;
    if (runtime->StartLock(sr::ProfileLockReason::kExplicit, false, base::BindOnce(
        [](CefRefPtr<AlloyProfileLockCallback> callback, sr::FileStatus result) {
          callback->OnComplete(SettingsResult(result));
        }, std::move(callback)))) return true;
    *lock_pending_ = false;
    return false;
  }
  void SetStandaloneLockHookForTesting(base::RepeatingClosure hook) {
    std::lock_guard lock(lifecycle_->mutex);
    lifecycle_->standalone_lock_hook_for_testing = std::move(hook);
  }
  alloy_keyring_result_t UnlockWithKeyring() override {
    if (lock_pending_->load()) return ALLOY_KEYRING_FAILED;
    sr::SecretServiceKeySource source;
    return KeyringResult(profile_->Unlock(source), source);
  }
  alloy_profile_state_t GetState() override {
    if (lock_pending_->load()) return ALLOY_PROFILE_TRANSITION;
    switch (profile_->state()) {
      case sr::ProfileState::kLocked: return ALLOY_PROFILE_LOCKED;
      case sr::ProfileState::kUnlocked: return ALLOY_PROFILE_UNLOCKED;
      case sr::ProfileState::kTransition: return ALLOY_PROFILE_TRANSITION;
      case sr::ProfileState::kError: return ALLOY_PROFILE_ERROR;
    }
    return ALLOY_PROFILE_ERROR;
  }
  bool SetAutoLock(int64_t idle_milliseconds, CefRefPtr<AlloyProfileLockHandler> handler) override {
    if (!CONTEXT_STATE_VALID() || !CEF_CURRENTLY_ON_UIT() || idle_milliseconds < 0) return false;
    auto runtime = Runtime();
    runtime->handler = std::move(handler);
    runtime->automatic->SetIdleTimeout(base::Milliseconds(idle_milliseconds));
    auto current = runtime->handler;
    if (current) current->OnLockTriggersChanged(SystemLockSource::Get().mask());
    return true;
  }
  bool NotifyUserActivity() override {
    if (!CONTEXT_STATE_VALID() || !CEF_CURRENTLY_ON_UIT()) return false;
    Runtime()->automatic->UserActivity(); return true;
  }
  bool NotifyLockSignal(alloy_profile_lock_reason_t reason) override {
    if (!CONTEXT_STATE_VALID() || !CEF_CURRENTLY_ON_UIT() || reason < ALLOY_LOCK_SLEEP ||
        reason > ALLOY_LOCK_USER_SWITCH) return false;
    Runtime()->automatic->Signal(static_cast<sr::ProfileLockReason>(reason)); return true;
  }
  scoped_refptr<sr::SitePrefStore> SettingsProjection() {
    return ProjectionWithGeneration(nullptr);
  }
  scoped_refptr<sr::SitePrefStore> ProjectionWithGeneration(
      std::shared_ptr<sr::RecordFileSystem>* original) {
    CEF_REQUIRE_UIT();
    auto runner = SettingsRunner();
    auto runtime = Runtime();
    std::lock_guard lock(lifecycle_->mutex);
    if (lock_pending_->load()) return nullptr;
    auto files = profile_->files();
    if (!files) return nullptr;
    if (!runtime->settings || !runtime->settings->Matches(files)) {
      runtime->services_admitted = std::make_shared<std::atomic<bool>>(true);
      runtime->settings = new SettingsOwner(files, runner,
          base::BindRepeating([](std::shared_ptr<std::atomic<bool>> admitted) {
            return admitted->load();
          }, runtime->services_admitted), base::BindRepeating(
              [](std::weak_ptr<ProfileRuntime> weak, std::shared_ptr<std::atomic<bool>> original) {
                if (auto runtime = weak.lock(); runtime && runtime->services_admitted == original)
                  runtime->AutomaticLock(sr::ProfileLockReason::kExplicit);
              }, std::weak_ptr<ProfileRuntime>(runtime), runtime->services_admitted));
    }
    runtime->has_settings = true;
    if (original) *original = files;
    return runtime->settings->prefs();
  }
  void PrepareNativeConfiguration(alloy::NativeProfileConfigurationCallback callback) {
    CEF_REQUIRE_UIT();
    std::shared_ptr<sr::RecordFileSystem> files;
    auto prefs = ProjectionWithGeneration(&files);
    if (!prefs) {
      CEF_POST_TASK(CEF_UIT, base::BindOnce(
          [](base::OnceClosure release, alloy::NativeProfileConfigurationCallback callback) {
            std::move(callback).Run(ALLOY_SETTINGS_CANCELLED, nullptr);
            std::move(release).Run();
          }, RetainForReply(), std::move(callback)));
      return;
    }
    auto original = files->ProfileCapability(sr::StorePurpose::kProfileSettings);
    CefRefPtr<SettingsOwner> settings = Runtime()->settings;
    auto weak_runtime = std::weak_ptr<ProfileRuntime>(Runtime());
    auto activity = base::BindRepeating([](std::weak_ptr<ProfileRuntime> weak) {
      if (auto runtime = weak.lock(); runtime && runtime->automatic) runtime->automatic->UserActivity();
    }, weak_runtime);
    prefs->InitializeAsync(base::BindOnce(
        [](base::OnceClosure release, sr::RecordCapability original,
            scoped_refptr<sr::SitePrefStore> prefs,
           CefRefPtr<SettingsOwner> settings,
           base::RepeatingClosure activity,
           alloy::NativeProfileConfigurationCallback callback, sr::FileStatus status) {
          if (status == sr::FileStatus::kOk && (!original.active() || prefs->ReadOnly()))
            status = sr::FileStatus::kCancelled;
          if (status != sr::FileStatus::kOk) {
            base::ScopedClosureRunner keepalive(std::move(release));
            std::move(callback).Run(SettingsResult(status), nullptr); return;
          }
          settings->PrepareFilters(base::BindOnce(
              [](base::OnceClosure release, sr::RecordCapability original,
                 scoped_refptr<sr::SitePrefStore> prefs,
                  base::RepeatingClosure activity, CefRefPtr<SettingsOwner> settings,
                  alloy::NativeProfileConfigurationCallback callback, sr::FileStatus status,
                 scoped_refptr<sr::ProfileFilterPreferences> filters) {
                base::ScopedClosureRunner keepalive(std::move(release));
                if (status == sr::FileStatus::kOk && (!original.active() || prefs->ReadOnly() ||
                    !filters || !filters->available())) status = sr::FileStatus::kCancelled;
                std::unique_ptr<sr::NativeProfileConfiguration> configuration;
                if (status == sr::FileStatus::kOk) {
                  auto view = prefs->CreateNativeView();
                  if (!view) status = sr::FileStatus::kCancelled;
                  else configuration = sr::MakeSiteProfileConfiguration(
                      std::move(original), std::move(view), prefs, keepalive.Release(),
                      base::BindOnce([](scoped_refptr<sr::ProfileFilterPreferences> filters,
                                        base::SupportsUserData& context) {
                        sr::ProfileFilterPreferences::Attach(context, filters);
                      }, filters), base::BindRepeating([](scoped_refptr<sr::ProfileFilterPreferences> filters) {
                        return filters->available();
                       }, filters));
                  if (configuration) configuration->SetConsumerData(&kProfileActivityKey,
                       std::make_unique<ProfileActivity>(std::move(activity), settings));
                }
                std::move(callback).Run(SettingsResult(status), std::move(configuration));
               }, std::move(release), std::move(original), prefs, std::move(activity), settings, std::move(callback)));
        }, RetainForReply(), std::move(original), prefs, settings, std::move(activity), std::move(callback)));
  }
  bool GetFirstPartyEmbeds(const CefString& outer,
      CefRefPtr<AlloySiteSettingsCallback> callback) override {
    if (!CEF_CURRENTLY_ON_UIT() || !callback) return false;
    auto prefs = SettingsProjection();
    if (!prefs) { Cancel(std::move(callback)); return true; }
    prefs->GetFirstPartyEmbedsAsync(GURL(outer.ToString()), base::BindOnce(
        [](base::OnceClosure release, scoped_refptr<sr::SitePrefStore> prefs,
           CefRefPtr<AlloySiteSettingsCallback> callback,
           sr::FileStatus status, std::vector<std::string> domains) {
          if (status == sr::FileStatus::kOk && prefs->ReadOnly()) status = sr::FileStatus::kCancelled;
          std::vector<CefString> result;
          if (status == sr::FileStatus::kOk) for (const auto& domain : domains) result.emplace_back(domain);
          callback->OnComplete(SettingsResult(status), result);
          std::move(release).Run();
        }, RetainForReply(), prefs, std::move(callback)));
    return true;
  }
  bool SetFirstPartyEmbeds(const CefString& outer, const std::vector<CefString>& embedded,
      CefRefPtr<AlloySiteSettingsCallback> callback) override {
    if (!CEF_CURRENTLY_ON_UIT() || !callback) return false;
    auto prefs = SettingsProjection();
    if (!prefs) { Cancel(std::move(callback)); return true; }
    std::vector<GURL> sites;
    for (const auto& site : embedded) sites.emplace_back(site.ToString());
    prefs->SetFirstPartyEmbedsAsync(GURL(outer.ToString()), std::move(sites), base::BindOnce(
        [](base::OnceClosure release, scoped_refptr<sr::SitePrefStore> prefs,
           CefRefPtr<AlloySiteSettingsCallback> callback, sr::FileStatus status) {
          if (status == sr::FileStatus::kOk && prefs->ReadOnly()) status = sr::FileStatus::kCancelled;
          callback->OnComplete(SettingsResult(status), {});
          std::move(release).Run();
        }, RetainForReply(), prefs, std::move(callback)));
    return true;
  }
  bool DeleteSiteRecord(const CefString& site,
      CefRefPtr<AlloyRecordDeletionCallback> callback) override {
    if (!CEF_CURRENTLY_ON_UIT() || !callback) return false;
    auto prefs = SettingsProjection();
    if (!prefs) {
      CEF_POST_TASK(CEF_UIT, base::BindOnce([](base::OnceClosure release,
          CefRefPtr<AlloyRecordDeletionCallback> callback) {
        callback->OnComplete(ALLOY_SETTINGS_CANCELLED, false); std::move(release).Run();
      }, RetainForReply(), callback));
      return true;
    }
    GURL url(site.ToString());
    prefs->DeleteSiteWithTeardownAsync(url, base::BindOnce(&RetireNativeSite, prefs, url),
        base::BindOnce([](base::OnceClosure release, scoped_refptr<sr::SitePrefStore> prefs,
            CefRefPtr<AlloyRecordDeletionCallback> callback, sr::FileStatus status, sr::RecordDeletion deleted) {
          if (status == sr::FileStatus::kOk && prefs->ReadOnly()) status = sr::FileStatus::kCancelled;
          callback->OnComplete(SettingsResult(status), deleted == sr::RecordDeletion::kDeletedFilesPending);
          std::move(release).Run();
        }, RetainForReply(), prefs, callback));
    return true;
  }
 private:
  std::shared_ptr<ProfileRuntime> Runtime() {
    return RuntimeFor(lifecycle_);
  }
  scoped_refptr<base::SequencedTaskRunner> SettingsRunner() {
    return ProfileRunner(lifecycle_);
  }
  base::OnceClosure RetainForReply() {
    // Callback-only ownership must not release the last profile on UI, since
    // the existing profile destructor durably locks its coordinator.
    return base::BindPostTask(SettingsRunner(), base::BindOnce(
        [](CefRefPtr<AlloyProfile>) {}, CefRefPtr<AlloyProfile>(this)));
  }
  void Cancel(CefRefPtr<AlloySiteSettingsCallback> callback) {
    CEF_POST_TASK(CEF_UIT, base::BindOnce(
        [](base::OnceClosure release, CefRefPtr<AlloySiteSettingsCallback> callback) {
          callback->OnComplete(ALLOY_SETTINGS_CANCELLED, {});
          std::move(release).Run();
        }, RetainForReply(), std::move(callback)));
  }
  ~AlloyProfileImpl() override = default;
  std::shared_ptr<sr::EncryptedProfile> profile_;
  // The same lock cut outlives this API owner when native inputs are retained.
  // Their original capabilities still prevent reuse after lock/unlock.
  const std::shared_ptr<std::atomic<bool>> lock_pending_ = std::make_shared<std::atomic<bool>>(false);
  const std::shared_ptr<ProfileLifecycle> lifecycle_;
  IMPLEMENT_REFCOUNTING(AlloyProfileImpl);
};

base::FilePath ProfilePath(const CefString& path) {
  return base::FilePath::FromUTF8Unsafe(path.ToString());
}

void ContextEmbedList(CefRefPtr<CefRequestContext> context, GURL outer,
                     std::optional<std::vector<GURL>> replacement,
                     CefRefPtr<AlloySiteSettingsCallback> callback) {
  CEF_REQUIRE_UIT();
  auto reply = base::BindOnce([](CefRefPtr<AlloySiteSettingsCallback> callback,
      sr::FileStatus status, std::vector<std::string> domains) {
    std::vector<CefString> result;
    if (status == sr::FileStatus::kOk)
      for (const auto& domain : domains) result.emplace_back(domain);
    callback->OnComplete(SettingsResult(status), result);
  }, std::move(callback));
  auto* impl = static_cast<CefRequestContextImpl*>(context.get());
  auto* cef_context = impl && impl->VerifyBrowserContext() ? impl->GetBrowserContext() : nullptr;
  auto* native = cef_context ? cef_context->AsBrowserContext() : nullptr;
  const auto* policy = native ? sr::RequestContextPolicy::Get(*native) : nullptr;
  if (!policy || policy->storage_mode() == sr::StorageMode::kStateless) {
    std::move(reply).Run(sr::FileStatus::kCancelled, {}); return;
  }
  const auto original = policy->StorageCapability(sr::StorePurpose::kSiteSettings);
  if (!original.active()) { std::move(reply).Run(sr::FileStatus::kCancelled, {}); return; }
  if (policy->storage_mode() == sr::StorageMode::kEncrypted) {
    const auto* input = sr::NativeProfileConfiguration::Get(*native);
    auto settings = input && input->admitted() ? sr::GetSiteProfileSettings(*input) : nullptr;
    if (!settings) { std::move(reply).Run(sr::FileStatus::kCancelled, {}); return; }
    auto completed = base::BindOnce([](CefRefPtr<CefRequestContext> context,
        scoped_refptr<sr::SitePrefStore> settings,
        base::OnceCallback<void(sr::FileStatus, std::vector<std::string>)> reply,
        sr::FileStatus status, std::vector<std::string> domains) {
      auto* impl = static_cast<CefRequestContextImpl*>(context.get());
      if (status == sr::FileStatus::kOk && (settings->ReadOnly() || !impl->VerifyBrowserContext()))
        status = sr::FileStatus::kCancelled;
      std::move(reply).Run(status, std::move(domains));
    }, context, settings, std::move(reply));
    if (replacement) settings->SetFirstPartyEmbedsAsync(std::move(outer), std::move(*replacement),
        base::BindOnce([](decltype(completed) completed, sr::FileStatus status) {
          std::move(completed).Run(status, {});
        }, std::move(completed)));
    else settings->GetFirstPartyEmbedsAsync(std::move(outer), std::move(completed));
    return;
  }
  auto* prefs = Profile::FromBrowserContext(native)->GetPrefs();
  if (!prefs || !prefs->FindPreference(sr::embed_list::kPreference)) {
    std::move(reply).Run(sr::FileStatus::kCancelled, {}); return;
  }
  if (!sr::embed_list::Canonicalize(outer, {})) {
    std::move(reply).Run(sr::FileStatus::kInvalid, {}); return;
  }
  if (replacement) {
    auto candidate = sr::embed_list::Canonicalize(outer, *replacement);
    if (!candidate) { std::move(reply).Run(sr::FileStatus::kInvalid, {}); return; }
    auto next = prefs->GetDict(sr::embed_list::kPreference).Clone();
    if (candidate->embedded.empty()) next.Remove(candidate->outer);
    else next.Set(candidate->outer, std::move(candidate->embedded));
    auto expected = next.Clone();
    prefs->SetDict(sr::embed_list::kPreference, std::move(next));
    const auto status = !original.active() || !impl->VerifyBrowserContext() ? sr::FileStatus::kCancelled :
        prefs->GetDict(sr::embed_list::kPreference) == expected ? sr::FileStatus::kOk : sr::FileStatus::kBusy;
    std::move(reply).Run(status, {});
  } else {
    base::Value value(prefs->GetDict(sr::embed_list::kPreference).Clone());
    std::vector<std::string> domains;
    const auto status = sr::embed_list::Read(&value, outer, &domains) ? sr::FileStatus::kOk : sr::FileStatus::kIntegrity;
    std::move(reply).Run(status, std::move(domains));
  }
}
}  // namespace

bool AlloyGetFirstPartyEmbeds(CefRefPtr<CefRequestContext> context, const CefString& outer,
                             CefRefPtr<AlloySiteSettingsCallback> callback) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  if (!CONTEXT_STATE_VALID() || !CEF_CURRENTLY_ON_UIT() || !callback) return false;
  CEF_POST_TASK(CEF_UIT, base::BindOnce(&ContextEmbedList, std::move(context),
      GURL(outer.ToString()), std::nullopt, std::move(callback)));
  return true;
}

bool AlloySetFirstPartyEmbeds(CefRefPtr<CefRequestContext> context, const CefString& outer,
                             const std::vector<CefString>& embedded,
                             CefRefPtr<AlloySiteSettingsCallback> callback) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  if (!CONTEXT_STATE_VALID() || !CEF_CURRENTLY_ON_UIT() || !callback) return false;
  std::vector<GURL> sites;
  for (const auto& site : embedded) sites.emplace_back(site.ToString());
  CEF_POST_TASK(CEF_UIT, base::BindOnce(&ContextEmbedList, std::move(context),
      GURL(outer.ToString()), std::make_optional(std::move(sites)), std::move(callback)));
  return true;
}

scoped_refptr<sr::SitePrefStore> alloy::GetProfilePrefStore(CefRefPtr<AlloyProfile> profile) {
  if (!profile || !CEF_CURRENTLY_ON_UIT()) return nullptr;
  return static_cast<AlloyProfileImpl*>(profile.get())->SettingsProjection();
}

bool alloy::ProfileNavigationAllowed(const sr::NativeProfileConfiguration& configuration) {
  const auto* activity = static_cast<const ProfileActivity*>(configuration.GetConsumerData(&kProfileActivityKey));
  return !activity || activity->settings->navigation_open();
}
bool alloy::ProfileNavigationAllowed(content::BrowserContext& context) {
  const auto* configuration = sr::NativeProfileConfiguration::Get(context);
  return !configuration || ProfileNavigationAllowed(*configuration);
}
void alloy::NotifyProfileUserActivity(content::BrowserContext* context) {
  CEF_REQUIRE_UIT();
  const auto* configuration = context ? sr::NativeProfileConfiguration::Get(*context) : nullptr;
  if (!configuration || !configuration->admitted()) return;
  const auto* activity = static_cast<const ProfileActivity*>(configuration->GetConsumerData(&kProfileActivityKey));
  if (activity) activity->activity.Run();
}

void alloy::InitializeProfileAutoLock() {
  CEF_REQUIRE_UIT();
  std::vector<std::shared_ptr<ProfileLifecycle>> live;
  {
    std::lock_guard lock(ProfilesMutex());
    for (const auto& weak : Profiles()) if (auto state = weak.lock()) live.push_back(std::move(state));
  }
  // Includes profiles opened before CefInitialize, even if no request context or
  // idle setting is ever requested. Browser-close and OS locking are mechanisms.
  for (const auto& state : live) RuntimeFor(state);
}
void alloy::SetProfileStandaloneLockHookForTesting(CefRefPtr<AlloyProfile> profile,
                                                  base::RepeatingClosure hook) {
  CHECK(profile);
  static_cast<AlloyProfileImpl*>(profile.get())->SetStandaloneLockHookForTesting(std::move(hook));
}

void alloy::LockProfilesForBrowserClose() {
  CEF_REQUIRE_UIT();
  std::vector<std::shared_ptr<ProfileLifecycle>> live;
  {
    std::lock_guard lock(ProfilesMutex());
    for (const auto& weak : Profiles()) if (auto state = weak.lock()) live.push_back(std::move(state));
  }
  for (const auto& state : live) {
    auto runtime = RuntimeFor(state);
    if (!runtime->closed) runtime->OnContextDestroyed();
  }
}

void alloy::BindNativeEmbedListSettings(Profile& profile) {
  CEF_REQUIRE_UIT();
  const auto* mode = sr::RequestContextPolicy::Get(profile);
  if (!mode || mode->storage_mode() == sr::StorageMode::kStateless) return;
  sr::EmbeddedFirstPartyPolicy::Predicate predicate;
  if (mode->storage_mode() == sr::StorageMode::kEncrypted) {
    const auto* input = sr::NativeProfileConfiguration::Get(profile);
    auto settings = input ? sr::GetSiteProfileSettings(*input) : nullptr;
    if (!settings) return;
    predicate = base::BindRepeating(&sr::SitePrefStore::IsConsideredFirstParty, settings);
  } else {
    predicate = base::BindRepeating([](sr::RecordCapability original, PrefService* prefs,
                                     const GURL& outer, const GURL& embedded) {
      if (!original.active()) return false;
      base::Value value(prefs->GetDict(sr::embed_list::kPreference).Clone());
      return sr::embed_list::Contains(&value, outer, embedded);
    }, mode->StorageCapability(sr::StorePurpose::kSiteSettings), profile.GetPrefs());
  }
  CHECK(content::ConfigureAlloyEmbeddedFirstPartyPolicy(
      profile, sr::EmbeddedFirstPartyPolicy::Bind(std::move(predicate))));
}

void alloy::PrepareNativeProfileConfiguration(
    CefRefPtr<AlloyProfile> profile, NativeProfileConfigurationCallback callback) {
  CEF_REQUIRE_UIT(); CHECK(profile); CHECK(callback);
  static_cast<AlloyProfileImpl*>(profile.get())->PrepareNativeConfiguration(std::move(callback));
}

CefRefPtr<AlloyProfile> AlloyCreateProfile(const CefString& path,
                                           const CefString& password) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  auto source = PasswordSource(password);
  if (!source) return nullptr;
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Create(ProfilePath(path), *source, &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}

CefRefPtr<AlloyProfile> AlloyCreateProfileWithKeySource(
    const CefString& path, CefRefPtr<AlloyKeySource> source) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  ExternalKeySource bridge(std::move(source));
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Create(ProfilePath(path), bridge, &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}

CefRefPtr<AlloyProfile> AlloyOpenProfile(const CefString& path) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Open(ProfilePath(path), &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}

CefRefPtr<AlloyProfile> AlloyCreateProfileWithKeyring(
    const CefString& path, int& result) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  result = ALLOY_KEYRING_FAILED;
  const auto file_path = ProfilePath(path);
  // Refuse normal duplicate creation before provisioning an unreachable key.
  // The file layer still enforces exclusive ownership at publication time.
  if (base::PathExists(file_path) || base::IsLink(file_path)) return nullptr;
  sr::SecretServiceKeySource source;
  std::unique_ptr<sr::EncryptedProfile> profile;
  const auto status = sr::EncryptedProfile::Create(file_path, source, &profile);
  result = KeyringResult(status, source);
  return status == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}
