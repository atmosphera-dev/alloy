// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include <algorithm>
#include <functional>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/threading/thread_restrictions.h"
#include "brave/alloy/site_records/site_pref_store_io.h"
#include "brave/alloy/site_records/native_pref_view.h"
#include "brave/alloy/site_records/pref_cache_clear.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
std::string Domain(const GURL& url) {
  return RecordDomainForPrincipal(url, url::Origin::Create(url));
}
GURL HostUrl(std::string_view host) {
  return ContentSettingsPattern::FromString(host).ToRepresentativeUrl();
}
}  // namespace

std::optional<RecordValueStoreBinding> SitePrefStore::BindManagedOrigin(const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!Admitted() || Domain(origin.GetURL()).empty()) return std::nullopt;
  auto error = base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating([](base::WeakPtr<SitePrefStore> owner, FileStatus status) {
        if (owner) owner->Fail(status);
      }, weak_factory_.GetWeakPtr()));
  auto binding = BindOriginValueStore(files_, profile_, origin, io_runner_, std::move(error));
  binding.observe_retirement = base::BindRepeating(
      [](base::WeakPtr<SitePrefStore> owner, base::RepeatingCallback<bool()> admitted,
         base::RepeatingClosure retired) -> base::CallbackListSubscription {
        if (!owner) return {};
        return owner->record_retired_.Add(base::BindRepeating(
            [](base::RepeatingCallback<bool()> admitted, base::RepeatingClosure retired) {
              if (!admitted.Run()) retired.Run();
            }, std::move(admitted), std::move(retired)));
      }, weak_factory_.GetWeakPtr(), binding.admitted);
  return binding;
}

class SitePrefStore::NativeCache {
 public:
  ~NativeCache() { ClearPrefCache(values); }
  PrefValueMap values;
  base::WeakPtrFactory<NativeCache> weak{this};
};

struct SitePrefStore::State {
  FileStatus status = FileStatus::kOk;
  FileStatus failure = FileStatus::kOk;
  base::DictValue values;
  std::map<std::string, RecordCapability> capabilities;
  base::UnguessableToken ticket;
  RecordDeletion deletion = RecordDeletion::kDeleted;
};

struct SitePrefStore::Request {
  enum Kind { kInitialize, kFlush, kEmbeds, kPreferred, kDelete, kQuery } kind;
  explicit Request(Kind kind) : kind(kind) {}
  base::UnguessableToken admission;
  uint64_t mutation = 0;
  PreferredVersion expected;
  PreferredVersion before;
  GURL site;
  std::vector<GURL> embeds;
  apps::PreferredApps entries;
  base::OnceCallback<void(Completion)> teardown;
  FileStatus retired_status = FileStatus::kOk;
  base::OnceCallback<void(FileStatus, PreferredVersion, RecordDeletion,
                          std::vector<std::string>)> reply;
};

class SitePrefStore::IO {
 public:
  explicit IO(std::shared_ptr<RecordFileSystem> files)
      : files_(std::move(files)), store_(base::MakeRefCounted<internal::SitePrefStore>(
            files_, base::BindRepeating([](FileStatus) {}))) {}
  State Execute(Request::Kind kind, base::DictValue values, GURL site,
                std::vector<GURL> embeds, apps::PreferredApps entries) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!store_) return Capture(FileStatus::kCancelled);
    store_->ReadPrefs();
    auto status = store_->last_status();
    base::UnguessableToken ticket;
    if (status == FileStatus::kOk && kind != Request::kInitialize && kind != Request::kQuery) {
      store_->ApplySnapshot(std::move(values));
      if (kind == Request::kEmbeds) status = store_->SetFirstPartyEmbeds(site, embeds);
      else if (kind == Request::kPreferred) status = store_->SetPreferredApps(entries);
      else if (kind == Request::kDelete) {
        status = store_->RetireSite(site, &ticket);
        if (status == FileStatus::kOk) retiring_.emplace(ticket, Domain(site));
      }
      else status = store_->Flush();
    }
    auto state = Capture(status);
    state.ticket = ticket;
    return state;
  }
  State Finish(base::UnguessableToken ticket) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    RecordDeletion deletion = RecordDeletion::kDeletedFilesPending;
    const auto status = files_->FinishRecordDeletion(ticket, &deletion);
    retiring_.erase(ticket);
    auto state = Capture(status);
    state.deletion = deletion;
    return state;
  }
  State Checkpoint(base::DictValue edits, std::vector<std::string> removed, base::OnceClosure done) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!store_) {
      if (done) std::move(done).Run();
      return Capture(FileStatus::kCancelled);
    }
    store_->ReadPrefs();
    auto status = store_->last_status();
    if (status == FileStatus::kOk) {
      auto next = store_->Snapshot();
      for (auto [key, value] : edits) next.Set(key, std::move(value));
      for (const auto& key : removed) next.Remove(key);
      // Native shutdown fences can run while UI teardown is pending. Preserve
      // every other site's edits, but never reintroduce retired references.
      for (const auto& [ticket, owner] : retiring_) {
        base::DictValue retained;
        status = internal::SitePrefStore::PruneSnapshot(next.Clone(), owner, &retained);
        if (status != FileStatus::kOk) break;
        next = std::move(retained);
      }
      if (status == FileStatus::kOk) {
        store_->ApplySnapshot(std::move(next)); status = store_->Flush();
      }
    }
    if (done) std::move(done).Run();
    auto state = Capture(status);
    if (status == FileStatus::kInvalid && state.failure == FileStatus::kOk) state.failure = status;
    return state;
  }
  void SaveOnDestruction(base::DictValue edits, std::vector<std::string> removed) {
    Checkpoint(std::move(edits), std::move(removed), {});
  }
  void Invalidate() { if (store_) store_->Invalidate(); }
  bool Close() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (store_) store_->Invalidate();
    store_ = nullptr; retiring_.clear();
    return true;
  }
 private:
  State Capture(FileStatus status) {
    State state;
    state.status = status;
    if (store_) {
      state.failure = store_->last_status();
      state.values = store_->Snapshot();
      state.capabilities = store_->capabilities();
    }
    return state;
  }
  const std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<internal::SitePrefStore> store_;
  std::map<base::UnguessableToken, std::string> retiring_;
  SEQUENCE_CHECKER(sequence_checker_);
};

SitePrefStore::SitePrefStore(std::shared_ptr<RecordFileSystem> files,
    RecordPrefStore::ErrorCallback on_error, scoped_refptr<base::SequencedTaskRunner> io_runner,
    base::RepeatingCallback<bool()> admitted)
    : files_(std::move(files)),
      profile_(files_ ? files_->ProfileCapability(StorePurpose::kProfileSettings) : RecordCapability()),
      on_error_(std::move(on_error)),
      admitted_(std::move(admitted)),
      io_runner_(io_runner ? std::move(io_runner) : base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      io_(io_runner_, files_), cache_(std::make_unique<NativeCache>()) {
  CHECK(on_error_);
  internal::SitePrefStore::InitializeRegistries();
}

SitePrefStore::~SitePrefStore() {
  if (cache_ && dirty_ && Admitted()) {
    base::DictValue edits; std::vector<std::string> removed;
    for (const auto& key : dirty_keys_) {
      const base::Value* value = nullptr;
      if (cache_->values.GetValue(key, &value)) edits.Set(key, value->Clone());
      else removed.push_back(key);
    }
    io_.AsyncCall(&IO::SaveOnDestruction).WithArgs(std::move(edits), std::move(removed));
  }
}

bool SitePrefStore::Admitted() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!cache_ || invalidated_ || status_ != FileStatus::kOk || (admitted_ && !admitted_.Run()) ||
      !profile_.Allows(StorePurpose::kProfileSettings, StorageAccess::kReadRecordFiles)) return false;
  for (const auto& [owner, cap] : capabilities_) if (!cap.active()) return false;
  return true;
}
bool SitePrefStore::GetValue(std::string_view key, const base::Value** value) const {
  return initialized_ && Admitted() && cache_->values.GetValue(key, value);
}
bool SitePrefStore::GetMutableValue(std::string_view key, base::Value** value) {
  return initialized_ && Admitted() && cache_->values.GetValue(key, value);
}
base::DictValue SitePrefStore::GetValues() const {
  return initialized_ && Admitted() ? cache_->values.AsDict() : base::DictValue();
}
base::DictValue SitePrefStore::Snapshot() const {
  base::DictValue values;
  if (cache_) for (const auto& [key, value] : cache_->values) values.Set(key, value.Clone());
  return values;
}
void SitePrefStore::AddObserver(PrefStore::Observer* observer) { observers_.AddObserver(observer); }
void SitePrefStore::RemoveObserver(PrefStore::Observer* observer) { observers_.RemoveObserver(observer); }
bool SitePrefStore::HasObservers() const { return !observers_.empty(); }
bool SitePrefStore::IsInitializationComplete() const { return initialized_; }
bool SitePrefStore::ReadOnly() const { return !Admitted(); }
bool SitePrefStore::HasReadErrorDelegate() const { return pending_read_delegates_ != 0; }
FileStatus SitePrefStore::last_status() const {
  return status_ != FileStatus::kOk ? status_ : Admitted() ? FileStatus::kOk : FileStatus::kDenied;
}
PersistentPrefStore::PrefReadError SitePrefStore::GetReadError() const {
  if (!initialized_ && reading_) return PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE;
  const auto status = last_status();
  return status == FileStatus::kOk ? PREF_READ_ERROR_NONE :
      status == FileStatus::kDenied || status == FileStatus::kCancelled ?
      PREF_READ_ERROR_ACCESS_DENIED : PREF_READ_ERROR_FILE_OTHER;
}
FileStatus SitePrefStore::Fail(FileStatus status) {
  if (status_ != FileStatus::kOk) return status_;
  if (status != FileStatus::kOk) {
    status_ = status;
    dirty_ = false; dirty_keys_.clear();
    io_.AsyncCall(&IO::Invalidate);
    RetireAdmission();
    if (NativeViewsEmpty() && cache_) cache_->values.Clear();
    NotifyChange(kPreferredApps);
    on_error_.Run(status);
  }
  return status;
}
void SitePrefStore::Changed(std::string_view key, bool notify, bool persist) {
  scoped_refptr<SitePrefStore> alive(this);
  ++mutation_; AdvanceRevision(key);
  // Hand the completed native mutation to the serialized backing owner before
  // returning or notifying observers. A background profile Lock can then fence
  // this sequence without asking UI to pump a timer or flush callback.
  if (persist) {
    dirty_ = true; dirty_keys_.insert(std::string(key));
    CommitPendingWrite();
  }
  if (notify) NotifyChange(key);
}
void SitePrefStore::SetValue(std::string_view key, base::Value value, uint32_t) {
  if (initialized_ && Admitted()) NativeSetValue(key, std::move(value), true);
}
void SitePrefStore::SetValueSilently(std::string_view key, base::Value value, uint32_t) {
  if (initialized_ && Admitted()) NativeSetValue(key, std::move(value), false);
}
void SitePrefStore::ReportValueChanged(std::string_view key, uint32_t) {
  if (initialized_ && Admitted()) Changed(key, true);
}
void SitePrefStore::RemoveValue(std::string_view key, uint32_t) {
  if (initialized_ && Admitted()) NativeRemoveValue(key);
}
void SitePrefStore::RemoveValuesByPrefixSilently(std::string_view prefix) {
  if (!initialized_ || !Admitted()) return;
  std::vector<std::string> removed;
  for (const auto& [key, value] : cache_->values) if (base::StartsWith(key, prefix)) removed.push_back(key);
  cache_->values.ClearWithPrefix(prefix);
  for (const auto& key : removed) Changed(key, false);
}

bool SitePrefStore::CanStartNativeConstruction() const {
  return initialized_ && !closing_cache_ && Admitted();
}
bool SitePrefStore::NativeCacheAlive() const { return cache_ != nullptr; }
bool SitePrefStore::NativeGetValue(std::string_view key, const base::Value** out) const {
  return cache_ && cache_->values.GetValue(key, out);
}
bool SitePrefStore::NativeGetMutableValue(std::string_view key, base::Value** out) {
  return cache_ && cache_->values.GetValue(key, out);
}
base::DictValue SitePrefStore::NativeGetValues() const {
  return cache_ ? cache_->values.AsDict() : base::DictValue();
}
bool SitePrefStore::NativeCanPersist() const {
  return initialized_ && !closing_cache_ && Admitted();
}
void SitePrefStore::NativeSetValue(std::string_view key, base::Value value, bool notify) {
  if (cache_ && cache_->values.SetValue(key, std::move(value)))
    Changed(key, notify, NativeCanPersist());
}
void SitePrefStore::NativeReportValueChanged(std::string_view key, bool notify) {
  if (cache_) Changed(key, notify, NativeCanPersist());
}
void SitePrefStore::NativeRemoveValue(std::string_view key) {
  if (cache_ && cache_->values.RemoveValue(key)) Changed(key, true, NativeCanPersist());
}
void SitePrefStore::NativeRemovePrefix(std::string_view prefix) {
  if (!cache_) return;
  std::vector<std::string> removed;
  for (const auto& [key, value] : cache_->values)
    if (base::StartsWith(key, prefix)) removed.push_back(key);
  cache_->values.ClearWithPrefix(prefix);
  for (const auto& key : removed) Changed(key, false, NativeCanPersist());
}
base::WeakPtr<SitePrefStore::NativeCache> SitePrefStore::GetNativeCacheForTesting() const {
  return cache_ ? cache_->weak.GetWeakPtr() : base::WeakPtr<NativeCache>();
}
void SitePrefStore::CloseNativeCacheAsync(base::OnceClosure done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (backend_closed_) {
    if (done) base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, std::move(done));
    return;
  }
  if (done) close_callbacks_.push_back(std::move(done));
  if (!closing_cache_) {
    closing_cache_ = true;
    invalidated_ = true;
    dirty_ = false; dirty_keys_.clear(); RetireAdmission();
    // Do not replace values while a native constructor/teardown has borrowed
    // them. Only the already-started native owner may finish using this cache.
    CloseNativeViews();
  }
  MaybeCloseNativeCache();
}
void SitePrefStore::NativeViewsChanged() {
  MaybeCloseNativeCache();
}
void SitePrefStore::MaybeCloseNativeCache() {
  if (!closing_cache_ || !NativeViewsEmpty() || busy_ || !requests_.empty() ||
      pending_backend_calls_ || pending_read_delegates_) return;
  cache_.reset(); capabilities_.clear();
  if (closing_backend_) return;
  closing_backend_ = true;
  // This also orders destruction of backend snapshots after all admitted work.
  // The reply is a separate UI turn, after any State/Request stack has unwound.
  io_.AsyncCall(&IO::Close).Then(base::BindOnce(
      [](scoped_refptr<SitePrefStore> source, bool) { source->BackendClosed(); },
      base::WrapRefCounted(this)));
}
void SitePrefStore::BackendClosed() {
  backend_closed_ = true;
  auto callbacks = std::move(close_callbacks_);
  for (auto& callback : callbacks) if (callback) std::move(callback).Run();
}

void SitePrefStore::Enqueue(std::unique_ptr<Request> request) {
  request->admission = admission_;
  requests_.push_back(std::move(request));
  Pump();
}
void SitePrefStore::Pump() {
  if (busy_ || requests_.empty()) return;
  auto request = std::move(requests_.front()); requests_.pop_front();
  if (!initialized_ && request->kind != Request::kInitialize) {
    requests_.push_front(std::move(request));
    request = std::make_unique<Request>(Request::kInitialize);
    request->admission = admission_;
  }
  busy_ = true;
  if (request->kind == Request::kInitialize && !initialized_) reading_ = true;
  auto status = CompletionStatus(*request, FileStatus::kOk);
  if (request->kind == Request::kPreferred && request->expected != preferred_version())
    status = FileStatus::kBusy;
  if (status != FileStatus::kOk || (request->kind == Request::kInitialize && initialized_)) {
    State state; state.status = status;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&SitePrefStore::Complete, base::WrapRefCounted(this),
                       std::move(request), false, std::move(state)));
    return;
  }
  request->mutation = mutation_;
  request->before = Version(request->kind == Request::kPreferred ? kPreferredApps : kEmbedList);
  const auto kind = request->kind;
  auto site = request->site;
  auto embeds = std::move(request->embeds);
  auto entries = std::move(request->entries);
  ++pending_backend_calls_;
  io_.AsyncCall(&IO::Execute).WithArgs(kind, Snapshot(), std::move(site),
      std::move(embeds), std::move(entries)).Then(base::BindOnce(
          &SitePrefStore::Complete, base::WrapRefCounted(this), std::move(request), true));
}

FileStatus SitePrefStore::CompletionStatus(const Request& request, FileStatus status) const {
  if (status_ != FileStatus::kOk) return status_;
  if ((status == FileStatus::kOk || status == FileStatus::kDenied) &&
      (invalidated_ || !profile_.active() || (admitted_ && !admitted_.Run()) ||
       request.admission != admission_)) return FileStatus::kCancelled;
  return status;
}

void SitePrefStore::Retired(std::unique_ptr<Request> request, State state) {
  capabilities_ = std::move(state.capabilities);
  base::DictValue retained;
  request->retired_status = internal::SitePrefStore::PruneSnapshot(
      Snapshot(), Domain(request->site), &retained);
  if (request->retired_status != FileStatus::kOk) retained = std::move(state.values);
  PrefValueMap next;
  for (auto [key, value] : retained) next.SetValue(key, std::move(value));
  dirty_ = mutation_ != request->mutation;
  if (!dirty_) dirty_keys_.clear();
  RetireAdmission();
  request->admission = admission_;
  in_teardown_ = true;
  Install(&next, true, true);
  const auto ticket = state.ticket;
  auto teardown = std::move(request->teardown);
  // A synchronous native completion must unwind preparation before any owner
  // can publish notifications. The completion still represents real teardown.
  auto resume = base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindOnce(&SitePrefStore::FinishRetirement,
          base::WrapRefCounted(this), std::move(request), ticket));
  if (teardown) std::move(teardown).Run(std::move(resume));
  else std::move(resume).Run(FileStatus::kOk);
  // Internal native revocation subscribers (e.g. managed downloaders), after
  // native preparation has closed original document/permission admission.
  record_retired_.Notify();
}

void SitePrefStore::FinishRetirement(std::unique_ptr<Request> request,
                                    base::UnguessableToken ticket, FileStatus teardown_status) {
  if (teardown_status != FileStatus::kOk) request->retired_status = teardown_status;
  ++pending_backend_calls_;
  io_.AsyncCall(&IO::Finish).WithArgs(ticket).Then(base::BindOnce(
      &SitePrefStore::Complete, base::WrapRefCounted(this), std::move(request), true));
}

void SitePrefStore::Complete(std::unique_ptr<Request> request, bool from_backend, State state) {
  scoped_refptr<SitePrefStore> alive(this);
  if (from_backend) { CHECK_GT(pending_backend_calls_, 0u); --pending_backend_calls_; }
  auto status = CompletionStatus(*request, state.status);
  if (!profile_.active()) {
    ClearPrefCache(state.values);
    state.capabilities.clear();
  }
  if (request->kind == Request::kDelete && !state.ticket.is_empty()) {
    // Key retirement is durable. UI teardown runs between two tasks on the
    // same I/O sequence, while the coordinator keeps admission closed.
    Retired(std::move(request), std::move(state));
    return;
  }
  busy_ = false;
  PreferredVersion published;
  std::vector<std::string> domains;
  if (request->kind == Request::kInitialize && !initialized_) {
    initialized_ = true; reading_ = false;
    if (status == FileStatus::kOk) {
      capabilities_ = std::move(state.capabilities);
      if (cache_) for (auto [key, value] : state.values) cache_->values.SetValue(key, std::move(value));
    } else Fail(status);
    observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnInitializationCompleted, status == FileStatus::kOk);
  } else if (status == FileStatus::kOk) {
    if (request->kind == Request::kFlush) {
      capabilities_ = std::move(state.capabilities);
      if (mutation_ == request->mutation) { dirty_ = false; dirty_keys_.clear(); }
    } else if (request->kind == Request::kEmbeds || request->kind == Request::kPreferred) {
      const std::string_view key = request->kind == Request::kPreferred ? kPreferredApps : kEmbedList;
      capabilities_ = std::move(state.capabilities);
      if (Version(key) != request->before) status = FileStatus::kBusy;
      else {
        PrefValueMap next;
        if (cache_) for (const auto& [name, value] : cache_->values) next.SetValue(name, value.Clone());
        if (const auto* value = state.values.Find(key)) next.SetValue(key, value->Clone());
        else next.RemoveValue(key);
        dirty_ = mutation_ != request->mutation;
        if (!dirty_) dirty_keys_.clear();
        const auto previous_publication = preferred_publication_;
        if (request->kind == Request::kPreferred) preferred_publication_ = request->expected;
        published = Install(&next, true, false, key);
        preferred_publication_ = previous_publication;
        if (Version(key) != published) status = FileStatus::kBusy;
      }
    } else if (request->kind == Request::kQuery) {
      status = GetFirstPartyEmbeds(request->site, &domains);
    } else if (request->kind == Request::kDelete) {
      status = request->retired_status;
      if (status != FileStatus::kOk) Fail(status);
    }
  }
  if (state.failure != FileStatus::kOk && state.failure != FileStatus::kDenied)
    status = Fail(state.failure);
  status = CompletionStatus(*request, status);
  if (status == FileStatus::kCancelled && !profile_.active() && !invalidated_) Invalidate();
  if (!profile_.active()) {
    request->entries.clear(); request->embeds.clear(); request->site = GURL();
    request->teardown.Reset();
  }
  if (request->kind == Request::kDelete)
    PublishRetirementNotifications(status == FileStatus::kOk);
  status = CompletionStatus(*request, status);
  if (request->reply) std::move(request->reply).Run(status, published, state.deletion, std::move(domains));
  if (!requests_.empty()) base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&SitePrefStore::Pump, base::WrapRefCounted(this)));
  MaybeCloseNativeCache();
}

void SitePrefStore::InitializeAsync(Completion completion) {
  auto request = std::make_unique<Request>(Request::kInitialize);
  request->reply = base::BindOnce([](Completion reply, FileStatus status, PreferredVersion,
      RecordDeletion, std::vector<std::string>) { std::move(reply).Run(status); }, std::move(completion));
  Enqueue(std::move(request));
}
void SitePrefStore::FlushAsync(Completion completion) {
  auto request = std::make_unique<Request>(Request::kFlush);
  request->reply = base::BindOnce([](Completion reply, FileStatus status, PreferredVersion,
      RecordDeletion, std::vector<std::string>) { std::move(reply).Run(status); }, std::move(completion));
  Enqueue(std::move(request));
}
FileStatus SitePrefStore::Wait(base::OnceCallback<void(Completion)> operation) {
  // Blocking compatibility callers (native tests/pre-init owners) still use
  // exactly the same I/O journal. Browser UI must use the asynchronous methods.
  base::AssertBlockingAllowed();
  if (in_teardown_) return FileStatus::kBusy;
  FileStatus status = FileStatus::kCancelled;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  std::move(operation).Run(base::BindOnce([](FileStatus* result, base::OnceClosure quit, FileStatus value) {
    *result = value; std::move(quit).Run();
  }, &status, loop.QuitClosure()));
  loop.Run();
  return status;
}
PersistentPrefStore::PrefReadError SitePrefStore::ReadPrefs() {
  if (!initialized_ && reading_) return PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE;
  if (!initialized_) Wait(base::BindOnce(&SitePrefStore::InitializeAsync, base::WrapRefCounted(this)));
  return GetReadError();
}
void SitePrefStore::ReadPrefsAsync(ReadErrorDelegate* error_delegate) {
  ++pending_read_delegates_;
  InitializeAsync(base::BindOnce([](scoped_refptr<SitePrefStore> store,
      std::unique_ptr<ReadErrorDelegate> delegate, FileStatus) {
    CHECK_GT(store->pending_read_delegates_, 0u); --store->pending_read_delegates_;
    const auto error = store->GetReadError();
    if (delegate && error != PREF_READ_ERROR_NONE) delegate->OnError(error);
  }, base::WrapRefCounted(this), std::unique_ptr<ReadErrorDelegate>(error_delegate)));
}
FileStatus SitePrefStore::Flush() {
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  return Wait(base::BindOnce(&SitePrefStore::FlushAsync, base::WrapRefCounted(this)));
}
void SitePrefStore::CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) {
  // PersistentPrefStore's synchronous_done_callback must run where I/O
  // completes, even when the owner cannot pump messages during shutdown.
  // Submit native edits directly to the same serialized backend, merging them
  // with earlier typed publications instead of overwriting their pending value.
  if (closing_backend_) {
    // Native PrefService destruction can still issue an empty flush after its
    // view has closed. Keep the callback threading contract, never reopen data.
    if (!done) done = base::BindOnce([] {});
    if (!reply) reply = base::BindOnce([] {});
    io_runner_->PostTaskAndReply(FROM_HERE, std::move(done), std::move(reply));
    return;
  }
  base::DictValue edits; std::vector<std::string> removed;
  if (NativeCanPersist()) for (const auto& key : dirty_keys_) {
    const base::Value* value = nullptr;
    if (cache_->values.GetValue(key, &value)) edits.Set(key, value->Clone());
    else removed.push_back(key);
  }
  ++pending_backend_calls_;
  io_.AsyncCall(&IO::Checkpoint).WithArgs(std::move(edits), std::move(removed), std::move(done))
      .Then(base::BindOnce(&SitePrefStore::NativeFence, base::WrapRefCounted(this),
          admission_, mutation_, std::move(reply)));
}
void SitePrefStore::NativeFence(base::UnguessableToken admission, uint64_t mutation,
    base::OnceClosure reply, State state) {
  CHECK_GT(pending_backend_calls_, 0u); --pending_backend_calls_;
  if (!profile_.active()) {
    ClearPrefCache(state.values);
    state.capabilities.clear();
  }
  if (state.failure != FileStatus::kOk && state.failure != FileStatus::kDenied) Fail(state.failure);
  if (state.status == FileStatus::kOk && admission == admission_ && Admitted()) {
    capabilities_ = std::move(state.capabilities);
    if (mutation_ == mutation) { dirty_ = false; dirty_keys_.clear(); }
  }
  if (reply) std::move(reply).Run();
  MaybeCloseNativeCache();
}
void SitePrefStore::SchedulePendingLossyWrites() { FlushAsync(base::BindOnce([](FileStatus) {})); }
void SitePrefStore::OnStoreDeletionFromDisk() { Invalidate(); }

SitePrefStore::PreferredVersion SitePrefStore::Install(PrefValueMap* next, bool notify,
    bool admission_changed, std::string_view key) {
  std::vector<std::string> changed;
  if (!cache_) return Version(key);
  cache_->values.GetDifferingKeys(next, &changed);
  for (const auto& name : changed) AdvanceRevision(name);
  if (!changed.empty()) ++mutation_;
  if (admission_changed && std::ranges::find(changed, kPreferredApps) == changed.end())
    changed.emplace_back(kPreferredApps);
  cache_->values.Swap(next);
  const auto installed = Version(key);
  if (notify) for (const auto& name : changed) NotifyChange(name);
  return installed;
}
void SitePrefStore::NotifyChange(std::string_view key) {
  if (in_teardown_) {
    retirement_notifications_.emplace(key);
    // Install native projections now, without delivering ordinary pref or native
    // consumer events. These publications join the same native drain barrier.
    // An ordinary pref observer can delete a record from a nested RunLoop.
    // Preparation must preserve the same checked reentrant traversal contract.
    observers_.NotifyAllowReentrancy([&](auto&& observer) {
      auto publication = std::invoke(&PrefStore::Observer::PreparePrefValueChange, observer, key);
      if (publication) native_retirement_publications_.push_back(std::move(publication));
    });
  } else observers_.NotifyAllowReentrancy(&PrefStore::Observer::OnPrefValueChanged, key);
}
void SitePrefStore::DeferNativeRetirementNotification(base::OnceClosure publication) {
  CHECK(in_teardown_);
  if (publication) native_retirement_publications_.push_back(std::move(publication));
}
void SitePrefStore::PublishRetirementNotifications(bool success) {
  in_teardown_ = false;
  auto publications = std::move(native_retirement_publications_);
  auto changed = std::move(retirement_notifications_);
  native_retirement_publications_.clear(); retirement_notifications_.clear();
  if (!success) return;
  for (auto& publication : publications) std::move(publication).Run();
  for (const auto& key : changed) NotifyChange(key);
}
void SitePrefStore::Invalidate() {
  scoped_refptr<SitePrefStore> alive(this);
  if (invalidated_) return;
  if (!NativeViewsEmpty()) { CloseNativeCacheAsync({}); return; }
  invalidated_ = true; dirty_ = false; dirty_keys_.clear();
  io_.AsyncCall(&IO::Invalidate);
  RetireAdmission(); PrefValueMap empty; Install(&empty, true, true);
}
SitePrefStore::PreferredVersion SitePrefStore::Version(std::string_view key) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto found = revisions_.find(key); CHECK(found != revisions_.end());
  return {found->second, admission_};
}
void SitePrefStore::AdvanceRevision(std::string_view key) {
  const auto found = revisions_.find(key);
  if (found != revisions_.end()) found->second = base::UnguessableToken::Create();
}
void SitePrefStore::RetireAdmission() { admission_ = base::UnguessableToken::Create(); }
SitePrefStore::PreferredVersion SitePrefStore::preferred_version() const { return Version(kPreferredApps); }
bool SitePrefStore::PreferredAdmission(const PreferredVersion& version) const {
  return initialized_ && Admitted() && version.admission == admission_;
}
bool SitePrefStore::IsPreferredPublication(const PreferredVersion& expected) const {
  return preferred_publication_ == expected && PreferredAdmission(expected);
}
FileStatus SitePrefStore::GetPreferredApps(apps::PreferredApps* entries) {
  if (!entries) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  const base::Value* value = nullptr;
  if (!cache_->values.GetValue(kPreferredApps, &value)) { entries->clear(); return FileStatus::kOk; }
  if (!value->is_list()) return Fail(FileStatus::kIntegrity);
  const auto status = DecodePreferredAppRecords(value->GetList(), entries);
  return status == FileStatus::kOk ? status : Fail(FileStatus::kIntegrity);
}
void SitePrefStore::PublishPreferredAppsAsync(PreferredVersion expected, apps::PreferredApps entries,
    base::OnceCallback<void(FileStatus, PreferredVersion)> completion) {
  auto request = std::make_unique<Request>(Request::kPreferred);
  request->expected = expected; request->entries = std::move(entries);
  request->reply = base::BindOnce([](base::OnceCallback<void(FileStatus, PreferredVersion)> reply,
      FileStatus status, PreferredVersion version, RecordDeletion, std::vector<std::string>) {
    std::move(reply).Run(status, version);
  }, std::move(completion));
  Enqueue(std::move(request));
}
FileStatus SitePrefStore::PublishPreferredApps(const PreferredVersion& expected,
    const apps::PreferredApps& entries, PreferredVersion* published) {
  if (!published) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  return Wait(base::BindOnce([](scoped_refptr<SitePrefStore> store, PreferredVersion expected,
      apps::PreferredApps entries, PreferredVersion* published, Completion done) {
    store->PublishPreferredAppsAsync(expected, std::move(entries), base::BindOnce(
        [](PreferredVersion* published, Completion done, FileStatus status, PreferredVersion version) {
          if (status == FileStatus::kOk) *published = version;
          std::move(done).Run(status);
        }, published, std::move(done)));
  }, base::WrapRefCounted(this), expected, apps::ClonePreferredApps(entries), published));
}
FileStatus SitePrefStore::SetPreferredApps(const apps::PreferredApps& entries) {
  PreferredVersion published;
  return PublishPreferredApps(preferred_version(), entries, &published);
}

FileStatus SitePrefStore::GetFirstPartyEmbeds(const GURL& outer, std::vector<std::string>* domains) {
  if (!domains) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  const auto owner = Domain(outer); if (owner.empty()) return FileStatus::kInvalid;
  std::vector<std::string> result;
  const base::Value* value = nullptr;
  if (cache_->values.GetValue(kEmbedList, &value)) {
    if (!value->is_dict()) return Fail(FileStatus::kIntegrity);
    if (const auto* entries = value->GetDict().Find(owner)) {
      if (!entries->is_list()) return Fail(FileStatus::kIntegrity);
      for (const auto& entry : entries->GetList()) {
        if (!entry.is_string()) return Fail(FileStatus::kIntegrity);
        auto domain = Domain(HostUrl(entry.GetString()));
        if (domain.empty()) return Fail(FileStatus::kIntegrity);
        if (std::ranges::find(result, domain) == result.end()) result.push_back(std::move(domain));
      }
    }
  }
  *domains = std::move(result); return FileStatus::kOk;
}
bool SitePrefStore::IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const {
  if (!Admitted()) return false;
  const auto owner = Domain(outer), child = Domain(embedded);
  if (owner.empty() || child.empty()) return false;
  const base::Value* value = nullptr;
  if (!cache_->values.GetValue(kEmbedList, &value) || !value->is_dict()) return false;
  const auto* list = value->GetDict().FindList(owner);
  if (list) for (const auto& entry : *list)
    if (entry.is_string() && Domain(HostUrl(entry.GetString())) == child) return true;
  return false;
}
void SitePrefStore::SetFirstPartyEmbedsAsync(GURL outer, std::vector<GURL> embedded, Completion completion) {
  auto request = std::make_unique<Request>(Request::kEmbeds);
  request->site = std::move(outer); request->embeds = std::move(embedded);
  request->reply = base::BindOnce([](Completion reply, FileStatus status, PreferredVersion,
      RecordDeletion, std::vector<std::string>) { std::move(reply).Run(status); }, std::move(completion));
  Enqueue(std::move(request));
}
void SitePrefStore::GetFirstPartyEmbedsAsync(GURL outer,
    base::OnceCallback<void(FileStatus, std::vector<std::string>)> completion) {
  auto request = std::make_unique<Request>(Request::kQuery); request->site = std::move(outer);
  request->reply = base::BindOnce([](base::OnceCallback<void(FileStatus, std::vector<std::string>)> reply,
      FileStatus status, PreferredVersion, RecordDeletion, std::vector<std::string> entries) {
    if (status != FileStatus::kOk) entries.clear();
    std::move(reply).Run(status, std::move(entries));
  }, std::move(completion));
  Enqueue(std::move(request));
}
FileStatus SitePrefStore::SetFirstPartyEmbeds(const GURL& outer, const std::vector<GURL>& embedded) {
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  return Wait(base::BindOnce(&SitePrefStore::SetFirstPartyEmbedsAsync,
      base::WrapRefCounted(this), outer, embedded));
}
FileStatus SitePrefStore::SetConsideredFirstParty(const GURL& outer, const GURL& embedded, bool allowed) {
  std::vector<std::string> domains;
  auto status = GetFirstPartyEmbeds(outer, &domains);
  if (status != FileStatus::kOk) return status;
  const auto child = Domain(embedded); if (child.empty()) return FileStatus::kInvalid;
  std::vector<GURL> sites;
  for (const auto& domain : domains) if (domain != child) sites.push_back(HostUrl(domain));
  if (allowed) sites.push_back(embedded);
  return SetFirstPartyEmbeds(outer, sites);
}
void SitePrefStore::DeleteSiteAsync(GURL site, base::OnceClosure teardown,
    base::OnceCallback<void(FileStatus, RecordDeletion)> completion) {
  DeleteSiteWithTeardownAsync(std::move(site), base::BindOnce(
      [](base::OnceClosure teardown, Completion resumed) {
        if (teardown) std::move(teardown).Run();
        std::move(resumed).Run(FileStatus::kOk);
      }, std::move(teardown)), std::move(completion));
}
void SitePrefStore::DeleteSiteWithTeardownAsync(GURL site,
    base::OnceCallback<void(Completion)> teardown,
    base::OnceCallback<void(FileStatus, RecordDeletion)> completion) {
  auto request = std::make_unique<Request>(Request::kDelete);
  request->site = std::move(site); request->teardown = std::move(teardown);
  request->reply = base::BindOnce([](base::OnceCallback<void(FileStatus, RecordDeletion)> reply,
      FileStatus status, PreferredVersion, RecordDeletion deletion, std::vector<std::string>) {
    std::move(reply).Run(status, deletion);
  }, std::move(completion));
  Enqueue(std::move(request));
}
FileStatus SitePrefStore::DeleteSite(const GURL& site, std::function<void()> teardown,
    RecordDeletion* result) {
  if (!result) return FileStatus::kInvalid;
  if (!initialized_ || !Admitted()) return last_status() == FileStatus::kOk ? FileStatus::kDenied : last_status();
  return Wait(base::BindOnce([](scoped_refptr<SitePrefStore> store, GURL site,
      std::function<void()> teardown, RecordDeletion* result, Completion done) {
    store->DeleteSiteAsync(site, base::BindOnce([](std::function<void()> action) {
      if (action) action();
    }, std::move(teardown)), base::BindOnce([](RecordDeletion* result, Completion done,
        FileStatus status, RecordDeletion deletion) {
      if (status == FileStatus::kOk) *result = deletion;
      std::move(done).Run(status);
    }, result, std::move(done)));
  }, base::WrapRefCounted(this), site, std::move(teardown), result));
}
}  // namespace alloy::site_records
