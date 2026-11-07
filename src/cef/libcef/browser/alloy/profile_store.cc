// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cef/include/alloy/alloy_profile.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include <utility>
#include <vector>

#include "brave/alloy/site_records/encrypted_profile.h"
#include "brave/alloy/site_records/native_profile_preferences.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/alloy/site_records/secret_service_key_source.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/barrier_closure.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_restrictions.h"
#include "cef/libcef/browser/alloy/profile_configuration.h"
#include "cef/libcef/browser/thread_util.h"
#include "cef/libcef/browser/browser_context.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "content/public/browser/browser_context.h"
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

class SettingsOwner final : public CefBaseRefCounted {
 public:
  SettingsOwner(std::shared_ptr<sr::RecordFileSystem> files,
                scoped_refptr<base::SequencedTaskRunner> runner,
                base::RepeatingCallback<bool()> admitted)
      : files_(std::move(files)), runner_(std::move(runner)), admitted_(std::move(admitted)) {
    prefs_ = base::MakeRefCounted<sr::NativeProfilePreferences>(files_->ProfileCapability(sr::StorePurpose::kProfileSettings),runner_,admitted_,
        base::BindRepeating(&SettingsOwner::NativeFailed,weak_factory_.GetWeakPtr()));
  }
  ~SettingsOwner() override {
    CEF_REQUIRE_UIT(); prefs_->Close({});
    if (filters_) filters_->Close({});
  }
  bool Matches(const std::shared_ptr<sr::RecordFileSystem>& files) const { return files == files_; }
  scoped_refptr<sr::NativeProfilePreferences> prefs() const { return prefs_; }
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
    prefs_->Close(base::BindOnce(
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
  void NativeFailed() { if(failure_==sr::FileStatus::kOk)failure_=sr::FileStatus::kCancelled;OnError(sr::FileStatus::kCancelled); }
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
  scoped_refptr<sr::NativeProfilePreferences> prefs_;
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
  void PrepareNativeConfiguration(alloy::NativeProfileConfigurationCallback callback) {
    CEF_REQUIRE_UIT();
    auto runner=SettingsRunner();std::shared_ptr<sr::RecordFileSystem> files;
    CefRefPtr<SettingsOwner> settings;
    {
      std::lock_guard lock(settings_mutex_);
      if(!lock_pending_->load())files=profile_->files();
      if(files){
        if(!settings_||!settings_->Matches(files))settings_=new SettingsOwner(files,runner,
            base::BindRepeating([](std::shared_ptr<std::atomic<bool>> pending){return !pending->load();},lock_pending_));
        settings=settings_;
      }
    }
    if(!settings){
      CEF_POST_TASK(CEF_UIT,base::BindOnce([](base::OnceClosure release,alloy::NativeProfileConfigurationCallback callback){
        base::ScopedClosureRunner keepalive(std::move(release));std::move(callback).Run(ALLOY_SETTINGS_CANCELLED,nullptr);
      },RetainForReply(),std::move(callback)));return;
    }
    auto original=files->ProfileCapability(sr::StorePurpose::kProfileSettings);
    settings->PrepareFilters(base::BindOnce([](base::OnceClosure release,sr::RecordCapability original,
        CefRefPtr<SettingsOwner> settings,alloy::NativeProfileConfigurationCallback callback,
        sr::FileStatus status,scoped_refptr<sr::ProfileFilterPreferences> filters){
      base::ScopedClosureRunner keepalive(std::move(release));
      auto prefs=settings->prefs();
      if(status==sr::FileStatus::kOk&&(!original.active()||prefs->ReadOnly()||!filters||!filters->available()))status=sr::FileStatus::kCancelled;
      std::unique_ptr<sr::NativeProfileConfiguration> configuration;
      if(status==sr::FileStatus::kOk){
        auto view=prefs->CreateNativeView();
        if(!view)status=sr::FileStatus::kCancelled;
        else {
          sr::NativeProfileConfiguration::Bindings bindings;bindings.prefs=view;
          bindings.admitted=base::BindRepeating([](scoped_refptr<sr::NativePrefView> view,
              scoped_refptr<sr::ProfileFilterPreferences> filters){return view->CanStartServiceConstruction()&&filters->available();},view,filters);
          bindings.begin_construction=base::BindOnce(&sr::NativePrefView::BeginConstruction,view);
          bindings.construction_finished=base::BindRepeating(&sr::NativePrefView::ConstructionFinished,view);
          bindings.finish_teardown=base::BindOnce(&sr::NativePrefView::FinishTeardown,view);
          bindings.attach_consumer=base::BindOnce([](scoped_refptr<sr::ProfileFilterPreferences> filters,
              base::SupportsUserData& context){sr::ProfileFilterPreferences::Attach(context,filters);},filters);
          configuration=std::make_unique<sr::NativeProfileConfiguration>(std::move(original),std::move(bindings),keepalive.Release());
        }
      }
      std::move(callback).Run(SettingsResult(status),std::move(configuration));
    },RetainForReply(),std::move(original),settings,std::move(callback)));
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
}  // namespace

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
