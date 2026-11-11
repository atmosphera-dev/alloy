// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/app_service/public/cpp/preferred_apps_impl.h"

#include <set>
#include <utility>

#include "base/check.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/services/app_service/public/cpp/preferred_apps_converter.h"

namespace apps {
namespace {
using Result = PreferredAppsImpl::StorageResult;
using Snapshot = PreferredAppsImpl::Snapshot;
using Version = PreferredAppsImpl::Version;

struct FileReadResult {
  Result status = Result::kOk;
  PreferredApps entries;
  bool requires_write_back = false;
};

// Ordinary file composition retains its native asynchronous boundary. Encrypted
// composition never constructs this backend, including after a storage error.
class FileStorage final : public PreferredAppsImpl::Storage {
 public:
  explicit FileStorage(base::FilePath directory)
      : path_(directory.empty() ? base::FilePath() : directory.AppendASCII("PreferredApps")),
        runner_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}
  void SetChangedCallback(Changed) override {}
  void Read(Reply reply) override {
    runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce([](base::FilePath path) {
      base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
      FileReadResult result;
      if (path.empty()) { result.status = Result::kDenied; return result; }
      base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
      if (!file.IsValid()) {
        if (file.error_details() == base::File::FILE_ERROR_NOT_FOUND)
          result.requires_write_back = true;
        else result.status = Result::kIo;
        return result;
      }
      std::string bytes;
      if (!base::ReadFileToString(path, &bytes)) { result.status = Result::kIo; return result; }
      auto value = base::JSONReader::Read(bytes, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      const auto* list = !value ? nullptr : value->is_list() ? &value->GetList()
          : value->is_dict() ? value->GetDict().FindList(kPreferredAppsKey) : nullptr;
      if (!list) { result.status = Result::kInvalid; return result; }
      // Validate every shape before invoking native conversion (which assumes
      // dictionaries); malformed input must not become a successful empty list.
      for (const auto& row : *list) {
        if (!row.is_dict() || !row.GetDict().FindString(kAppIdKey)) {
          result.status = Result::kInvalid; return result;
        }
        const auto* conditions = row.GetDict().FindList(kIntentFilterKey);
        if (!conditions) { result.status = Result::kInvalid; return result; }
        for (const auto& condition : *conditions) {
          if (!condition.is_dict()) { result.status = Result::kInvalid; return result; }
          const auto* values = condition.GetDict().FindList("condition_values");
          if (!values) { result.status = Result::kInvalid; return result; }
          for (const auto& item : *values)
            if (!item.is_dict()) { result.status = Result::kInvalid; return result; }
        }
        if (!apps_util::ConvertListToIntentFilterConditions(conditions)) {
          result.status = Result::kInvalid; return result;
        }
      }
      result.entries = ParseValueToPreferredApps(*value);
      result.requires_write_back = !IsUpgradedForSharing(*value);
      if (result.requires_write_back) UpgradePreferredApps(result.entries);
      return result;
    }, path_), base::BindOnce([](base::WeakPtr<FileStorage> owner, Reply reply,
                                FileReadResult result) {
      if (!owner) return;
      Snapshot snapshot;
      if (result.status == Result::kOk) {
        snapshot.version = owner->version_;
        snapshot.entries = std::move(result.entries);
        snapshot.admitted = base::BindRepeating([] { return true; });
        snapshot.requires_write_back = result.requires_write_back;
      }
      std::move(reply).Run(result.status, std::move(snapshot));
    }, weak_.GetWeakPtr(), std::move(reply)));
  }
  void Write(Version expected, PreferredApps entries, Reply reply) override {
    if (expected != version_) { std::move(reply).Run(Result::kConflict, {}); return; }
    std::string bytes;
    if (!base::JSONWriter::Write(ConvertPreferredAppsToValue(entries), &bytes)) {
      std::move(reply).Run(Result::kInvalid, {}); return;
    }
    runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(
        [](base::FilePath path, std::string bytes) {
          return !path.empty() && base::ImportantFileWriter::WriteFileAtomically(path, bytes);
        }, path_, std::move(bytes)), base::BindOnce(
        [](base::WeakPtr<FileStorage> owner, Version expected, PreferredApps entries,
           Reply reply, bool ok) {
          if (!owner) return;
          if (expected != owner->version_) { std::move(reply).Run(Result::kConflict, {}); return; }
          if (!ok) { std::move(reply).Run(Result::kIo, {}); return; }
          owner->version_.revision = base::UnguessableToken::Create();
          Snapshot snapshot{owner->version_, std::move(entries), base::BindRepeating([] { return true; })};
          std::move(reply).Run(Result::kOk, std::move(snapshot));
        }, weak_.GetWeakPtr(), expected, std::move(entries), std::move(reply)));
  }
 private:
  const base::FilePath path_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  Version version_{base::UnguessableToken::Create(), base::UnguessableToken::Create()};
  base::WeakPtrFactory<FileStorage> weak_{this};
};
}  // namespace

PreferredAppsImpl::PreferredAppsImpl(Host* host, const base::FilePath& profile_dir,
    base::OnceClosure read_completed, base::OnceClosure write_completed)
    : PreferredAppsImpl(host, std::make_shared<FileStorage>(profile_dir),
        std::move(read_completed), std::move(write_completed)) {}
PreferredAppsImpl::PreferredAppsImpl(Host* host, std::shared_ptr<Storage> storage,
    base::OnceClosure read_completed, base::OnceClosure write_completed)
    : host_(host), preferred_apps_list_(this), storage_(std::move(storage)) {
  read_completed_for_testing_ = std::move(read_completed);
  write_completed_for_testing_ = std::move(write_completed);
  CHECK(host_ && storage_);
  preferred_apps_list_.SetAvailable(false);
  preferred_apps_list_.SetAvailabilityCallback(base::BindRepeating(
      [](base::WeakPtr<PreferredAppsImpl> owner) { return owner && owner->Admitted(); },
      weak_ptr_factory_.GetWeakPtr()));
  preferred_apps_list_.AddObserver(this);
  storage_->SetChangedCallback(base::BindRepeating(&PreferredAppsImpl::StorageChanged,
                                                 weak_ptr_factory_.GetWeakPtr()));
  // No external callback can retire a not-yet-returned constructor.
  ScheduleRead();
}
PreferredAppsImpl::~PreferredAppsImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  storage_->SetChangedCallback({});
  preferred_apps_list_.RemoveObserver(this);
}
bool PreferredAppsImpl::Admitted() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return ready_ && snapshot_ && snapshot_->admitted && snapshot_->admitted.Run();
}
bool PreferredAppsImpl::Current(base::UnguessableToken epoch, uint64_t revision) const {
  return epoch == epoch_ && revision == revision_ && Admitted();
}
bool PreferredAppsImpl::QueryConflict(const std::string& first, const IntentFilterPtr& first_filter,
    const std::string& second, const IntentFilterPtr& second_filter) {
  return host_->QueryConflict(first, first_filter, second, second_filter);
}
bool PreferredAppsImpl::IsWebAppInExtendedScope(const GURL& url, const std::string& app_id) const {
  return host_->IsWebAppInExtendedScope(url, app_id);
}
void PreferredAppsImpl::OnPreferredAppChanged(const std::string& app_id, bool value) {
  host_->OnSupportedLinksPreferenceChanged(app_id, value);
}
void PreferredAppsImpl::SetLongestPrefixMatchEnabled(bool enabled) {
  longest_prefix_match_enabled_ = enabled;
  ++revision_;
  preferred_apps_list_.SetLongestPrefixMatchEnabled(enabled);
}
void PreferredAppsImpl::ScheduleRead() {
  read_ticket_ = base::UnguessableToken::Create();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
      &PreferredAppsImpl::StartRead, weak_ptr_factory_.GetWeakPtr(), epoch_, read_ticket_));
}
void PreferredAppsImpl::StartRead(base::UnguessableToken epoch, base::UnguessableToken ticket) {
  if (epoch != epoch_ || ticket != read_ticket_) return;
  auto storage = storage_;
  storage->Read(base::BindOnce(&PreferredAppsImpl::ReadCompleted,
                               weak_ptr_factory_.GetWeakPtr(), epoch, ticket));
}
void PreferredAppsImpl::ReadCompleted(base::UnguessableToken epoch, base::UnguessableToken ticket,
    StorageResult result, Snapshot snapshot) {
  if (epoch != epoch_ || ticket != read_ticket_) return;
  if (result != StorageResult::kOk || !snapshot.admitted || !snapshot.admitted.Run()) {
    Invalidate(result == StorageResult::kOk ? StorageResult::kDenied : result, false);
    return;
  }
  auto entries = std::move(snapshot.entries);
  snapshot_ = std::move(snapshot);
  status_ = StorageResult::kOk;
  ready_ = true;
  ++revision_;
  if (snapshot_->requires_write_back)
    desired_ = Desired{revision_, ClonePreferredApps(entries)};
  auto weak = weak_ptr_factory_.GetWeakPtr();
  preferred_apps_list_.Init(std::move(entries));
  if (!weak || epoch != epoch_) return;
  while (!pending_.empty() && Admitted()) {
    auto change = std::move(pending_.front());
    pending_.pop_front();
    Execute(std::move(change));
    if (!weak || epoch != epoch_) return;
  }
  PumpWrite();
  if (!weak || epoch != epoch_) return;
  if (read_completed_for_testing_) std::move(read_completed_for_testing_).Run();
}
void PreferredAppsImpl::StorageChanged(StorageResult result) {
  Invalidate(result, result == StorageResult::kOk);
}
void PreferredAppsImpl::Invalidate(StorageResult result, bool refresh) {
  epoch_ = base::UnguessableToken::Create();
  ++revision_;
  if (desired_) {
    unsaved_.push_back(std::move(desired_->entries));
    desired_.reset();
    if (result == StorageResult::kOk) result = StorageResult::kConflict;
  }
  if (!pending_.empty() && result == StorageResult::kOk) result = StorageResult::kConflict;
  while (!pending_.empty()) {
    rejected_.push_back(std::move(pending_.front())); pending_.pop_front();
  }
  ready_ = false;
  writing_ = false;
  snapshot_.reset();
  status_ = result;
  if (refresh) ScheduleRead();
  auto weak = weak_ptr_factory_.GetWeakPtr();
  preferred_apps_list_.SetAvailable(false);
  if (!weak) return;
  if (result != StorageResult::kOk) host_->OnPreferredAppsStorageError(result);
}
void PreferredAppsImpl::RemovePreferredApp(const std::string& app_id) {
  Submit({ChangeKind::kRemoveApp, app_id, {}});
}
void PreferredAppsImpl::SetSupportedLinksPreference(const std::string& app_id, IntentFilters filters) {
  Submit({ChangeKind::kSetLinks, app_id, std::move(filters)});
}
void PreferredAppsImpl::RemoveSupportedLinksPreference(const std::string& app_id) {
  Submit({ChangeKind::kRemoveLinks, app_id, {}});
}
#if BUILDFLAG(IS_CHROMEOS)
void PreferredAppsImpl::SetProtocolLinkPreference(const std::string& app_id, IntentFilterPtr filter) {
  IntentFilters filters; filters.push_back(std::move(filter));
  Submit({ChangeKind::kSetProtocol, app_id, std::move(filters)});
}
void PreferredAppsImpl::RemoveProtocolLinkFilters(const std::string& app_id, IntentFilters filters) {
  Submit({ChangeKind::kRemoveProtocol, app_id, std::move(filters)});
}
#endif
void PreferredAppsImpl::Submit(Change change) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (Admitted()) { Execute(std::move(change)); return; }
  if (ready_) {  // Lease loss without a lifecycle notification is still denial.
    rejected_.push_back(std::move(change));
    Invalidate(StorageResult::kDenied, false);
  } else if (status_ == StorageResult::kOk) pending_.push_back(std::move(change));
  else {
    rejected_.push_back(std::move(change));
    host_->OnPreferredAppsStorageError(status_);
  }
}
void PreferredAppsImpl::Execute(Change change) {
  const auto epoch = epoch_;
  const auto revision = revision_;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  PreferredAppsList candidate(this);
  candidate.Init(preferred_apps_list_.GetValue());
  candidate.SetLongestPrefixMatchEnabled(longest_prefix_match_enabled_);
  candidate.SetAvailabilityCallback(base::BindRepeating(
      [](base::WeakPtr<PreferredAppsImpl> owner, base::UnguessableToken epoch, uint64_t revision) {
        return owner && owner->Current(epoch, revision);
      }, weak, epoch, revision));
  switch (change.kind) {
    case ChangeKind::kRemoveApp: candidate.DeleteAppId(change.app_id); break;
    case ChangeKind::kRemoveLinks: candidate.DeleteSupportedLinks(change.app_id); break;
    case ChangeKind::kSetLinks: {
      std::set<std::string> removed;
      for (const auto& filter : change.filters) {
        CHECK(apps_util::IsSupportedLinkForApp(change.app_id, filter));
        auto replaced = candidate.AddPreferredApp(change.app_id, filter);
        if (!weak) return;
        if (!Current(epoch, revision)) { Reject(std::move(change)); return; }
        for (const auto& [id, filters] : replaced) {
          if (removed.contains(id)) continue;
          for (const auto& removed_filter : filters) if (apps_util::IsSupportedLinkForApp(id, removed_filter)) {
            candidate.DeleteSupportedLinks(id); removed.insert(id); break;
          }
        }
      }
      break;
    }
    case ChangeKind::kSetProtocol:
      for (const auto& filter : change.filters) {
        CHECK(!apps_util::IsSupportedLinkForApp(change.app_id, filter));
        candidate.AddPreferredApp(change.app_id, filter);
        if (!weak) return;
        if (!Current(epoch, revision)) { Reject(std::move(change)); return; }
      }
      break;
    case ChangeKind::kRemoveProtocol:
      for (const auto& filter : change.filters) {
        CHECK(!apps_util::IsSupportedLinkForApp(change.app_id, filter));
        candidate.DeletePreferredApp(change.app_id, filter);
      }
      break;
  }
  if (!Current(epoch, revision)) { Reject(std::move(change)); return; }
  auto entries = candidate.GetValue();
  if (apps::IsEqual(entries, preferred_apps_list_.GetValue())) return;
  desired_ = Desired{++revision_, ClonePreferredApps(entries)};
  preferred_apps_list_.Replace(std::move(entries));
  if (!weak || epoch != epoch_) return;
  PumpWrite();
}
void PreferredAppsImpl::Reject(Change change) {
  rejected_.push_back(std::move(change));
  host_->OnPreferredAppsStorageError(StorageResult::kConflict);
}
void PreferredAppsImpl::PumpWrite() {
  if (writing_ || !desired_ || !Admitted()) return;
  writing_ = true;
  auto storage = storage_;
  storage->Write(snapshot_->version, ClonePreferredApps(desired_->entries), base::BindOnce(
      &PreferredAppsImpl::WriteCompleted, weak_ptr_factory_.GetWeakPtr(), epoch_, desired_->revision));
}
void PreferredAppsImpl::WriteCompleted(base::UnguessableToken epoch, uint64_t revision,
    StorageResult result, Snapshot snapshot) {
  if (epoch != epoch_) return;
  writing_ = false;
  if (result != StorageResult::kOk || !Admitted() || !snapshot.admitted || !snapshot.admitted.Run() ||
      snapshot.version.admission != snapshot_->version.admission) {
    Invalidate(result == StorageResult::kOk ? StorageResult::kDenied : result,
               result == StorageResult::kConflict);
    return;
  }
  snapshot_ = std::move(snapshot);
  if (desired_ && desired_->revision == revision) desired_.reset();
  if (desired_) { PumpWrite(); return; }
  if (write_completed_for_testing_) std::move(write_completed_for_testing_).Run();
}
std::vector<PreferredApps> PreferredAppsImpl::GetUnsavedValues() const {
  std::vector<PreferredApps> values;
  for (const auto& value : unsaved_) values.push_back(ClonePreferredApps(value));
  if (desired_) values.push_back(ClonePreferredApps(desired_->entries));
  return values;
}
std::vector<PreferredAppsImpl::Change> PreferredAppsImpl::GetRejectedChanges() const {
  std::vector<Change> changes;
  for (const auto& change : rejected_)
    changes.push_back({change.kind, change.app_id, CloneIntentFilters(change.filters)});
  return changes;
}
}  // namespace apps
