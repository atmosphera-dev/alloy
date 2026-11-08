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
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/barrier_closure.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_restrictions.h"
#include "cef/libcef/browser/alloy/profile_settings.h"
#include "cef/libcef/browser/thread_util.h"
#include "cef/libcef/browser/browser_context.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "cef/libcef/browser/context.h"
#include "cef/libcef/browser/request_context_impl.h"
#include "chrome/browser/profiles/profile.h"
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
    native.push_back(browser_context);
    keepalive.push_back(context->GetAnyRequestContext(false));
  }
  auto success = std::make_shared<bool>(true);
  // One completion from each site owner and each network owner, plus the
  // commit-iteration gate. A synchronous drain cannot release our keepalives.
  auto completed = base::BarrierClosure(native.size() * 2 + 1, base::BindOnce(
      [](std::vector<CefRefPtr<CefRequestContextImpl>>, std::shared_ptr<bool> success,
         sr::SitePrefStore::Completion done) {
        std::move(done).Run(*success ? sr::FileStatus::kOk : sr::FileStatus::kIo);
      }, std::move(keepalive), success, std::move(done)));
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
                base::RepeatingCallback<bool()> admitted)
      : files_(std::move(files)), runner_(std::move(runner)), admitted_(std::move(admitted)) {
    prefs_ = base::MakeRefCounted<sr::SitePrefStore>(files_,
        base::BindRepeating(&SettingsOwner::OnError, weak_factory_.GetWeakPtr()), runner_, admitted_);
  }
  ~SettingsOwner() override {
    CEF_REQUIRE_UIT(); prefs_->Invalidate();
    if (filters_) filters_->Close({});
  }
  bool Matches(const std::shared_ptr<sr::RecordFileSystem>& files) const { return files == files_; }
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
    if (status != sr::FileStatus::kCancelled && status != sr::FileStatus::kDenied &&
        failure_ == sr::FileStatus::kOk) failure_ = status;
    auto ui = CEF_TASK_RUNNER<CEF_UIT>();
    runner_->PostTask(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<sr::RecordFileSystem> files,
           scoped_refptr<base::SingleThreadTaskRunner> ui, base::WeakPtr<SettingsOwner> owner) {
          files->Lock();
          ui->PostTask(FROM_HERE, base::BindOnce([](base::WeakPtr<SettingsOwner> owner) {
            if (owner) owner->CloseCaches({});
          }, std::move(owner)));
        }, files_, std::move(ui), weak_factory_.GetWeakPtr()));
  }
  const std::shared_ptr<sr::RecordFileSystem> files_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  const base::RepeatingCallback<bool()> admitted_;
  scoped_refptr<sr::SitePrefStore> prefs_;
  scoped_refptr<sr::ProfileFilterPreferences> filters_;
  bool filters_loading_ = false;
  std::vector<FiltersReady> filters_ready_;
  sr::FileStatus failure_ = sr::FileStatus::kOk;
  base::WeakPtrFactory<SettingsOwner> weak_factory_{this};
  IMPLEMENT_REFCOUNTING_DELETE_ON_UIT(SettingsOwner);
};

class AlloyProfileImpl final : public AlloyProfile {
 public:
  explicit AlloyProfileImpl(std::unique_ptr<sr::EncryptedProfile> profile)
      : profile_(std::move(profile)) {}
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
    scoped_refptr<base::SequencedTaskRunner> runner;
    bool has_settings = false;
    {
      std::lock_guard lock(settings_mutex_);
      runner = settings_runner_;
      has_settings = !!settings_;
    }
    if (!runner || !has_settings) {
      const bool success = profile_->Lock() == sr::FileStatus::kOk;
      *lock_pending_ = false;
      return success;
    }
    if (CEF_CURRENTLY_ON_UIT() || runner->RunsTasksInCurrentSequence()) {
      *lock_pending_ = false;
      return false;
    }
    // The public lifecycle API is synchronous on an embedder background thread.
    // Native UI edits already entered this same queue; never wait for UI here.
    base::AssertBlockingAllowed();
    base::WaitableEvent finished;
    sr::FileStatus status = sr::FileStatus::kCancelled;
    if (!StartRuntimeLock(runner, base::BindOnce(
        [](sr::FileStatus* status, base::WaitableEvent* finished, sr::FileStatus result) {
          *status = result; finished->Signal();
        }, &status, &finished))) {
      *lock_pending_ = false;
      return false;
    }
    finished.Wait();
    return status == sr::FileStatus::kOk;
  }
  bool LockAsync(CefRefPtr<AlloyProfileLockCallback> callback) override {
    if (!CEF_CURRENTLY_ON_UIT() || !callback) return false;
    bool expected = false;
    if (!lock_pending_->compare_exchange_strong(expected, true)) return false;
    auto runner = SettingsRunner();
    if (StartRuntimeLock(runner, base::BindOnce(
        [](CefRefPtr<AlloyProfileLockCallback> callback, sr::FileStatus result) {
          callback->OnComplete(SettingsResult(result));
        }, std::move(callback)))) return true;
    *lock_pending_ = false;
    return false;
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
  scoped_refptr<sr::SitePrefStore> SettingsProjection() {
    return ProjectionWithGeneration(nullptr);
  }
  scoped_refptr<sr::SitePrefStore> ProjectionWithGeneration(
      std::shared_ptr<sr::RecordFileSystem>* original) {
    CEF_REQUIRE_UIT();
    auto runner = SettingsRunner();
    std::lock_guard lock(settings_mutex_);
    if (lock_pending_->load()) return nullptr;
    auto files = profile_->files();
    if (!files) return nullptr;
    if (!settings_ || !settings_->Matches(files)) settings_ = new SettingsOwner(files, runner,
        base::BindRepeating([](std::shared_ptr<std::atomic<bool>> pending) {
          return !pending->load();
        }, lock_pending_));
    if (original) *original = files;
    return settings_->prefs();
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
    CefRefPtr<SettingsOwner> settings = settings_;
    prefs->InitializeAsync(base::BindOnce(
        [](base::OnceClosure release, sr::RecordCapability original,
            scoped_refptr<sr::SitePrefStore> prefs,
           CefRefPtr<SettingsOwner> settings,
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
                }
                std::move(callback).Run(SettingsResult(status), std::move(configuration));
              }, std::move(release), std::move(original), prefs, std::move(callback)));
        }, RetainForReply(), std::move(original), prefs, settings, std::move(callback)));
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
  bool StartRuntimeLock(scoped_refptr<base::SequencedTaskRunner> runner,
                        base::OnceCallback<void(sr::FileStatus)> completion) {
    return runner->PostTask(FROM_HERE, base::BindOnce(
        [](CefRefPtr<AlloyProfileImpl> owner,
           base::OnceCallback<void(sr::FileStatus)> completion) {
          const auto result = owner->profile_->Lock();
          CEF_POST_TASK(CEF_UIT, base::BindOnce(&AlloyProfileImpl::FinishRuntimeLock,
              owner, result, std::move(completion)));
        }, CefRefPtr<AlloyProfileImpl>(this), std::move(completion)));
  }
  void FinishRuntimeLock(sr::FileStatus result, base::OnceCallback<void(sr::FileStatus)> completion) {
    CEF_REQUIRE_UIT();
    if (result == sr::FileStatus::kOk && settings_ && settings_->failure() != sr::FileStatus::kOk)
      result = settings_->failure();
    auto completed = base::BindOnce(
        [](CefRefPtr<AlloyProfileImpl> owner, base::OnceClosure release,
           sr::FileStatus result, base::OnceCallback<void(sr::FileStatus)> completion) {
           {
             std::lock_guard lock(owner->settings_mutex_);
             if (result == sr::FileStatus::kOk && owner->settings_ &&
                 owner->settings_->failure() != sr::FileStatus::kOk) result = owner->settings_->failure();
             owner->settings_ = nullptr;
             *owner->lock_pending_ = false;
          }
          std::move(completion).Run(result);
          std::move(release).Run();
        }, CefRefPtr<AlloyProfileImpl>(this), RetainForReply(), result, std::move(completion));
    if (settings_) settings_->CloseCaches(std::move(completed));
    else CEF_POST_TASK(CEF_UIT, std::move(completed));
  }
  scoped_refptr<base::SequencedTaskRunner> SettingsRunner() {
    std::lock_guard lock(settings_mutex_);
    if (!settings_runner_) settings_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    return settings_runner_;
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
  std::unique_ptr<sr::EncryptedProfile> profile_;
  // The same lock cut outlives this API owner when native inputs are retained.
  // Their original capabilities still prevent reuse after lock/unlock.
  const std::shared_ptr<std::atomic<bool>> lock_pending_ = std::make_shared<std::atomic<bool>>(false);
  std::mutex settings_mutex_;
  scoped_refptr<base::SequencedTaskRunner> settings_runner_;
  CefRefPtr<SettingsOwner> settings_;
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
