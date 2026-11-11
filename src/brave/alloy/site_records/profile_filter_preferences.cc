// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/alloy/site_records/profile_filter_resources.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/strings/string_view_util.h"
#include "base/task/thread_pool.h"
#include "brave/alloy/site_records/pref_cache_clear.h"
#include "brave/alloy/site_records/request_context_policy.h"

namespace alloy::site_records {
namespace {
constexpr char kContextSlot = 0;
struct Text {
  explicit Text(std::string bytes) : bytes(std::move(bytes)) {}
  Text(Text&&) = default;
  ~Text() { Cleanse(base::as_writable_byte_span(bytes)); }
  std::string bytes;
};
struct Initial {
  Initial() = default;
  Initial(Initial&&) = default;
  ~Initial() { ClearPrefCache(values); }
  FileStatus status = FileStatus::kOk;
  base::DictValue values;
};
class ContextData final : public base::SupportsUserData::Data {
 public:
  explicit ContextData(scoped_refptr<ProfileFilterPreferences> prefs) : prefs(std::move(prefs)) {}
  ~ContextData() override {
    // Encrypted configuration belongs to its profile, not to a prepared input
    // or individual context association. Its profile closes it after native
    // teardown. Guest configuration has exactly the context's lifetime.
    if (prefs->mode() != StorageMode::kEncrypted) prefs->Close({});
  }
  const scoped_refptr<ProfileFilterPreferences> prefs;
};
}  // namespace

ProfileFilterPreferences::ProfileFilterPreferences(std::shared_ptr<ProfileFilterStorage> storage,
    scoped_refptr<base::SequencedTaskRunner> runner, Error error, base::DictValue initial,
    base::RepeatingCallback<bool()> admitted)
    : storage_(std::move(storage)), runner_(std::move(runner)), error_(std::move(error)),
      admitted_(std::move(admitted)) {
  for (auto entry : initial) {
    keys_.insert(entry.first);
    InMemoryPrefStore::SetValueSilently(entry.first, std::move(entry.second), 0);
  }
}
ProfileFilterPreferences::~ProfileFilterPreferences() { Wipe(); }
bool ProfileFilterPreferences::available() const {
  return !closing_ && status_ == FileStatus::kOk && storage_->active() &&
      (!admitted_ || admitted_.Run());
}
brave_shields::AdBlockDATCacheManager::Backend ProfileFilterPreferences::DATBackend() {
  return MakeProfileFilterDATBackend(storage_,runner_,base::BindRepeating(
      &ProfileFilterPreferences::Failed,base::WrapRefCounted(this)),
      base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self) {
        // An expected lifecycle cut cancels new work and successful retired
        // replies. Storage revocation/failure is not that cut: report it.
        return !self->closing_ && (!self->admitted_ || self->admitted_.Run());
      },base::WrapRefCounted(this)));
}
base::CallbackListSubscription ProfileFilterPreferences::ObserveClose(base::RepeatingClosure closed) {
  return consumers_closed_.Add(std::move(closed));
}
void ProfileFilterPreferences::ConsumerFailed(FileStatus status) { Failed(status); }
bool ProfileFilterPreferences::ReadOnly() const { return !available() || storage_->mode() == StorageMode::kStateless; }
bool ProfileFilterPreferences::GetValue(std::string_view key, const base::Value** out) const {
  return available() && InMemoryPrefStore::GetValue(key, out);
}
base::DictValue ProfileFilterPreferences::GetValues() const {
  return available() ? InMemoryPrefStore::GetValues() : base::DictValue();
}
bool ProfileFilterPreferences::GetMutableValue(std::string_view key, base::Value** out) {
  return !ReadOnly() && InMemoryPrefStore::GetMutableValue(key, out);
}
void ProfileFilterPreferences::Load(std::shared_ptr<ProfileFilterStorage> storage,
    scoped_refptr<base::SequencedTaskRunner> runner, Error error, Loaded loaded,
    base::RepeatingCallback<bool()> admitted) {
  CHECK(storage && runner && error && loaded);
  auto completion = std::make_shared<Loaded>(std::move(loaded));
  const bool posted = runner->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
      [](std::shared_ptr<ProfileFilterStorage> storage) {
        Initial result;
        // Stateless uses registered defaults and never reads private inputs.
        if (storage->mode() == StorageMode::kStateless) return result;
        ProfileFilterStorage::Bytes bytes;
        result.status = storage->Read("configuration", &bytes);
        if (result.status == FileStatus::kNotFound) result.status = FileStatus::kOk;
        else if (result.status == FileStatus::kOk) {
          auto value = base::JSONReader::ReadDict(base::as_string_view(bytes), base::JSON_PARSE_RFC);
          if (!value) result.status = FileStatus::kIntegrity;
          else result.values = std::move(*value);
        }
        Cleanse(bytes); return result;
      }, storage), base::BindOnce([](std::shared_ptr<ProfileFilterStorage> storage,
          scoped_refptr<base::SequencedTaskRunner> runner, Error error, std::shared_ptr<Loaded> completion,
          base::RepeatingCallback<bool()> admitted, Initial result) {
        if (result.status == FileStatus::kOk && admitted && !admitted.Run()) {
          std::move(*completion).Run(FileStatus::kCancelled, nullptr); return;
        }
        if (result.status == FileStatus::kOk && !storage->active()) result.status = FileStatus::kDenied;
        if (result.status != FileStatus::kOk) {
          error.Run(result.status); std::move(*completion).Run(result.status, nullptr); return;
        }
        auto prefs = base::WrapRefCounted(new ProfileFilterPreferences(
            storage, runner, error, std::move(result.values), std::move(admitted)));
        std::move(*completion).Run(FileStatus::kOk, std::move(prefs));
      }, storage, runner, error, completion, std::move(admitted)));
  if (!posted) { error.Run(FileStatus::kCancelled); std::move(*completion).Run(FileStatus::kCancelled, nullptr); }
}
void ProfileFilterPreferences::Install(std::string_view key, base::Value value, uint32_t flags, bool notify) {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  if (ReadOnly()) return;
  const base::Value* old = nullptr;
  if (InMemoryPrefStore::GetValue(key, &old) && *old == value) return;
  keys_.insert(std::string(key));
  InMemoryPrefStore::SetValueSilently(key, std::move(value), flags);
  Publish();
  if (notify && !ReadOnly()) InMemoryPrefStore::ReportValueChanged(key, flags);
}
void ProfileFilterPreferences::SetValue(std::string_view key, base::Value value, uint32_t flags) {
  Install(key, std::move(value), flags, true);
}
void ProfileFilterPreferences::SetValueSilently(std::string_view key, base::Value value, uint32_t flags) {
  Install(key, std::move(value), flags, false);
}
void ProfileFilterPreferences::ReportValueChanged(std::string_view key, uint32_t flags) {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  if (ReadOnly()) return;
  keys_.insert(std::string(key)); Publish();
  if (!ReadOnly()) InMemoryPrefStore::ReportValueChanged(key, flags);
}
void ProfileFilterPreferences::RemoveValue(std::string_view key, uint32_t flags) {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  if (ReadOnly()) return;
  Publish(key);
  if (!ReadOnly()) { InMemoryPrefStore::RemoveValue(key, flags); keys_.erase(std::string(key)); }
}
void ProfileFilterPreferences::RemoveValuesByPrefixSilently(std::string_view prefix) {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  if (ReadOnly()) return;
  InMemoryPrefStore::RemoveValuesByPrefixSilently(prefix);
  for (auto it = keys_.begin(); it != keys_.end();) {
    if (it->starts_with(prefix)) it = keys_.erase(it); else ++it;
  }
  Publish();
}
void ProfileFilterPreferences::Publish(std::string_view excluded) {
  base::DictValue flat;
  for (const auto& key : keys_) {
    if (key == excluded) continue;
    const base::Value* value = nullptr;
    if (InMemoryPrefStore::GetValue(key, &value)) flat.Set(key, value->Clone());
  }
  std::string bytes;
  const bool encoded = base::JSONWriter::Write(flat, &bytes); ClearPrefCache(flat);
  if (!encoded) { Cleanse(base::as_writable_byte_span(bytes)); Failed(FileStatus::kInvalid); return; }
  const bool posted = runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
      [](std::shared_ptr<ProfileFilterStorage> storage, Text bytes) {
        return storage->Write("configuration", base::as_byte_span(bytes.bytes));
      }, storage_, Text(std::move(bytes))), base::BindOnce(
          &ProfileFilterPreferences::Failed, base::WrapRefCounted(this)));
  if (!posted) Failed(FileStatus::kCancelled);
}
/// Latch and report real filter storage failures while preserving lifecycle refusal semantics.
void ProfileFilterPreferences::Failed(FileStatus status) {
  if (status == FileStatus::kOk || IsAdmissionRefusal(status) || status_ != FileStatus::kOk) return;
  scoped_refptr<ProfileFilterPreferences> alive(this);
  status_ = status; error_.Run(status);
}
void ProfileFilterPreferences::CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) {
  auto ui = base::SequencedTaskRunner::GetCurrentDefault();
  const bool posted = runner_->PostTask(FROM_HERE, base::BindOnce(
      [](scoped_refptr<base::SequencedTaskRunner> ui, base::OnceClosure reply, base::OnceClosure done) {
        if (done) std::move(done).Run();
        if (reply) ui->PostTask(FROM_HERE, std::move(reply));
      }, ui, std::move(reply), std::move(done)));
  if (!posted) Failed(FileStatus::kCancelled);
}
void ProfileFilterPreferences::Wipe() {
  for (const auto& key : keys_) {
    base::Value* value = nullptr;
    if (InMemoryPrefStore::GetMutableValue(key, &value)) {
      base::DictValue moved; moved.Set("value", std::move(*value)); ClearPrefCache(moved);
    }
  }
  InMemoryPrefStore::RemoveValuesByPrefixSilently(""); keys_.clear();
}
void ProfileFilterPreferences::Close(Closed done) {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  if (closed_) { if (done) std::move(done).Run(status_); return; }
  if (done) closed_callbacks_.push_back(std::move(done));
  if (closing_) return;
  closing_ = true; consumers_closed_.Notify(); Wipe();
  if (resources_) {
    runner_->PostTask(FROM_HERE, base::BindOnce(&ProfileFilterResources::Close, resources_));
    resources_ = nullptr;
  }
  // Already-admitted snapshots must land before invalidating their backing.
  const bool posted = runner_->PostTaskAndReply(FROM_HERE, base::BindOnce(
      [](std::shared_ptr<ProfileFilterStorage> storage) { storage->Close(); }, storage_),
      base::BindOnce(&ProfileFilterPreferences::FinishClose, alive));
  if (!posted) { Failed(FileStatus::kCancelled); FinishClose(); }
}
void ProfileFilterPreferences::FinishClose() {
  scoped_refptr<ProfileFilterPreferences> alive(this);
  closed_ = true;
  auto callbacks = std::move(closed_callbacks_);
  for (auto& callback : callbacks) std::move(callback).Run(status_);
}
void ProfileFilterPreferences::Attach(base::SupportsUserData& context, scoped_refptr<ProfileFilterPreferences> prefs) {
  // Construction may finish unwinding after key revocation. Binding the
  // original descriptor is safe; Get/read/edit admission still fails closed.
  CHECK(prefs && !context.GetUserData(&kContextSlot));
  const auto* policy = RequestContextPolicy::Get(context);
  CHECK(policy && policy->profile() == prefs->storage_->original_generation().profile());
  CHECK(policy->storage_mode() == prefs->storage_->mode());
  context.SetUserData(&kContextSlot, std::make_unique<ContextData>(std::move(prefs)));
}
scoped_refptr<ProfileFilterPreferences> ProfileFilterPreferences::Get(const base::SupportsUserData& context) {
  const auto* policy = RequestContextPolicy::Get(context);
  const auto* data = static_cast<const ContextData*>(context.GetUserData(&kContextSlot));
  if (!data || !policy || !policy->StorageCapability(StorePurpose::kProfileCustomFilters).active() ||
      !data->prefs->available()) return nullptr;
  return data->prefs;
}
scoped_refptr<ProfileFilterPreferences> ProfileFilterPreferences::EnsureTransient(base::SupportsUserData& context) {
  if(context.GetUserData(&kContextSlot))return Get(context);
  const auto* policy=RequestContextPolicy::Get(context);
  if(!policy || policy->storage_mode()==StorageMode::kEncrypted)return nullptr;
  std::shared_ptr<ProfileFilterStorage> storage;
  if(ProfileFilterStorage::Create(nullptr,policy->StorageCapability(StorePurpose::kProfileCustomFilters),&storage)!=FileStatus::kOk)return nullptr;
  auto runner=base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(),base::TaskPriority::USER_VISIBLE,base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
  // Transient failures still latch status_/close admission. No replacement input
  // is created for this context after failure, and no persistent fallback exists.
  auto prefs=base::WrapRefCounted(new ProfileFilterPreferences(storage,runner,
      base::BindRepeating([](FileStatus){}),{}));
  Attach(context,prefs);return Get(context);
}
}  // namespace alloy::site_records
