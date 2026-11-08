// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/preferred_app_storage.h"

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/test/bind.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/services/app_service/public/cpp/preferred_apps_converter.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace alloy::site_records {
namespace {
using Native = apps::PreferredAppsImpl;
using Result = Native::StorageResult;
apps::IntentFilters Links(std::string host) {
  apps::IntentFilters filters;
  filters.push_back(apps_util::MakeIntentFilterForUrlScope(GURL("https://" + host + "/")));
  return filters;
}
apps::PreferredApps Entries(std::string id, std::string host) {
  apps::PreferredApps entries;
  entries.push_back(std::make_unique<apps::PreferredApp>(std::move(Links(host)[0]), id));
  return entries;
}
SecretKey Key() { ObjectId key; key.fill(0x63); return SecretKey(key); }
class Host final : public Native::Host {
 public:
  void OnSupportedLinksPreferenceChanged(const std::string& id, bool value) override {
    events.emplace_back(id, value);
    visible[id] = value;
    if (changed) changed.Run(id, value);
  }
  bool QueryConflict(const std::string&, const apps::IntentFilterPtr&,
                     const std::string&, const apps::IntentFilterPtr&) override {
    return !conflict || conflict.Run();
  }
  bool IsWebAppInExtendedScope(const GURL&, const std::string&) const override { return false; }
  void OnPreferredAppsStorageError(Result result) override {
    errors.push_back(result);
    if (error) error.Run();
  }
  std::map<std::string, bool> visible;
  std::vector<std::pair<std::string, bool>> events;
  std::vector<Result> errors;
  base::RepeatingCallback<void(std::string, bool)> changed;
  base::RepeatingCallback<bool()> conflict;
  base::RepeatingClosure error;
};
// Delays actual native backend requests/acknowledgements without replacing its
// storage, semantic validation, version checks, notifications or encrypted I/O.
class DelayedStorage final : public Native::Storage {
 public:
  explicit DelayedStorage(std::shared_ptr<Native::Storage> backend) : backend(std::move(backend)) {}
  void SetChangedCallback(Changed changed) override {
    backend->SetChangedCallback(std::move(changed));
  }
  void Read(Reply reply) override {
    backend->Read(base::BindOnce([](DelayedStorage* self, Reply reply, Result result, Native::Snapshot snapshot) {
      if (self->delay_reads) self->reads.push_back(base::BindOnce(std::move(reply), result, std::move(snapshot)));
      else std::move(reply).Run(result, std::move(snapshot));
    }, this, std::move(reply)));
  }
  void Write(Native::Version expected, apps::PreferredApps entries, Reply reply) override {
    auto request = base::BindOnce([](std::shared_ptr<Native::Storage> backend, Native::Version expected,
        apps::PreferredApps entries, Reply reply) {
      backend->Write(expected, std::move(entries), std::move(reply));
    }, backend, expected, std::move(entries), base::BindOnce(
        [](DelayedStorage* self, Reply reply, Result result, Native::Snapshot snapshot) {
          if (self->delay_acks) self->acks.push_back(base::BindOnce(std::move(reply), result, std::move(snapshot)));
          else std::move(reply).Run(result, std::move(snapshot));
        }, this, std::move(reply)));
    if (delay_writes) writes.push_back(std::move(request));
    else std::move(request).Run();
  }
  static void Release(std::deque<base::OnceClosure>* queue) {
    ASSERT_FALSE(queue->empty());
    auto task = std::move(queue->front()); queue->pop_front(); std::move(task).Run();
  }
  std::shared_ptr<Native::Storage> backend;
  bool delay_reads = false, delay_writes = false, delay_acks = false;
  std::deque<base::OnceClosure> reads, writes, acks;
};

class PreferredAppPersistenceTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, Key(), {}, &files_));
    Open();
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
  }
  void Start(std::shared_ptr<Native::Storage> backend = {}) {
    impl_ = std::make_unique<Native>(&host_, backend ? std::move(backend) : MakePreferredAppStorage(store_));
    tasks_.RunUntilIdle();
  }
  void Reopen() {
    tasks_.RunUntilIdle();
    impl_.reset(); store_.reset();
    tasks_.RunUntilIdle();
    ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, Key(), &files_));
    Open(); Start();
  }
  void Delete(std::string host) {
    tasks_.RunUntilIdle();  // The baseline choices must be acknowledged before deletion.
    RecordDeletion result;
    ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(GURL("https://" + host + "/"), {}, &result));
  }
  std::optional<std::string> Find(std::string host) {
    return impl_->preferred_apps_list().FindPreferredAppForUrl(GURL("https://" + host + "/"));
  }
  apps::PreferredApps Read() {
    tasks_.RunUntilIdle();
    apps::PreferredApps entries;
    EXPECT_EQ(FileStatus::kOk, store_->GetPreferredApps(&entries)); return entries;
  }
  void NoRecord(std::string host) {
    std::vector<RecordInfo> records;
    ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
    for (const auto& record : records) EXPECT_NE(host, record.domain);
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  Host host_;
  std::unique_ptr<Native> impl_;
};

TEST_F(PreferredAppPersistenceTest, ActualNativeSetterColdReopensEncryptedChoicesOnTwoSites) {
  Start();
  ASSERT_TRUE(impl_->preferred_apps_list().IsInitialized());
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  impl_->SetSupportedLinksPreference("second", Links("beta.test"));
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
  EXPECT_TRUE(host_.visible["first"]); EXPECT_TRUE(host_.visible["second"]);
  tasks_.RunUntilIdle();
  EXPECT_TRUE(impl_->GetUnsavedValues().empty());
  Reopen();
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
  EXPECT_EQ(2u, Read().size());
  base::CommandLine command(base::FilePath("/usr/bin/timeout"));
  for (const auto* arg : {"30", "rg", "-a", "-F", "-i", "-l", "-e", "alpha.test", "-e", "beta.test"})
    command.AppendArg(arg);
  command.AppendArgPath(path_);
  std::string output; int exit = -1;
  ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
  EXPECT_EQ(1, exit) << output;
}

TEST_F(PreferredAppPersistenceTest, RecordDeletionRevokesHostAndStableHandleThenRetainsTheOtherSite) {
  Start();
  auto* handle = &impl_->preferred_apps_list();
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  impl_->SetSupportedLinksPreference("second", Links("beta.test"));
  Delete("alpha.test");
  EXPECT_FALSE(handle->FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_FALSE(host_.visible["first"]);
  tasks_.RunUntilIdle();
  EXPECT_EQ(handle, &impl_->preferred_apps_list());
  EXPECT_EQ("second", Find("beta.test")); EXPECT_FALSE(Find("alpha.test"));
  EXPECT_TRUE(host_.visible["second"]);
  Reopen();
  EXPECT_FALSE(Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
}

TEST_F(PreferredAppPersistenceTest, ANewUnrelatedHostActionIsNotCancelledByADuplicateDeletionEvent) {
  Start(); impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  bool submitted = false;
  host_.changed = base::BindLambdaForTesting([&](std::string id, bool value) {
    if (id == "first" && !value && !submitted) {
      // This explicit new host action is submitted after invalidation, in the
      // fresh controller epoch. It does not touch the deleted record.
      submitted = true;
      impl_->SetSupportedLinksPreference("fresh", Links("gamma.test"));
    }
  });
  Delete("alpha.test"); tasks_.RunUntilIdle();
  ASSERT_TRUE(submitted);
  EXPECT_EQ("fresh", Find("gamma.test")); EXPECT_FALSE(Find("alpha.test"));
  EXPECT_TRUE(impl_->GetRejectedChanges().empty());
  EXPECT_TRUE(host_.errors.empty());
  NoRecord("alpha.test");
  Reopen(); EXPECT_EQ("fresh", Find("gamma.test"));
}

TEST_F(PreferredAppPersistenceTest, LeaseDeniesCachedReadsAndLiveInvalidationNotifiesHostOnLock) {
  Start(); impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  EXPECT_FALSE(Find("alpha.test"));
  EXPECT_TRUE(impl_->preferred_apps_list().GetReference().empty());
  store_->Invalidate();
  EXPECT_FALSE(host_.visible["first"]);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(impl_->preferred_apps_list().IsInitialized());
  impl_->SetSupportedLinksPreference("blocked", Links("beta.test"));
  EXPECT_EQ(1u, impl_->GetRejectedChanges().size());
}

TEST_F(PreferredAppPersistenceTest, FailedPublicationRetainsDesiredStateReportsErrorAndCannotReplayIt) {
  Start(); impl_->SetSupportedLinksPreference("old", Links("alpha.test"));
  tasks_.RunUntilIdle();
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  impl_->SetSupportedLinksPreference("new", Links("alpha.test"));
  tasks_.RunUntilIdle();
  EXPECT_FALSE(Find("alpha.test")); EXPECT_FALSE(host_.visible["new"]);
  ASSERT_FALSE(host_.errors.empty());
  EXPECT_EQ(Result::kIo, host_.errors.front());
  ASSERT_EQ(1u, impl_->GetUnsavedValues().size());
  EXPECT_EQ("new", impl_->GetUnsavedValues()[0][0]->app_id);
  files_->SetFaultForTesting({});
  impl_.reset(); store_.reset(); files_.reset();
  tasks_.RunUntilIdle();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, Key(), &files_));
  Open(); Start();
  EXPECT_EQ("old", Find("alpha.test"));
}

TEST_F(PreferredAppPersistenceTest, WholeMultiFilterMutationIsCancelledByDelegateReentry) {
  Start();
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  impl_->SetSupportedLinksPreference("second", Links("beta.test"));
  int calls = 0;
  host_.conflict = base::BindLambdaForTesting([&] {
    EXPECT_EQ(2u, impl_->preferred_apps_list().GetEntrySize());
    if (++calls == 2) impl_->SetSupportedLinksPreference("fresh", Links("gamma.test"));
    return true;
  });
  auto filters = Links("alpha.test");
  filters.push_back(std::move(Links("beta.test")[0]));
  impl_->SetSupportedLinksPreference("interrupted", std::move(filters));
  EXPECT_EQ(2, calls);
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
  EXPECT_EQ("fresh", Find("gamma.test"));
  EXPECT_FALSE(host_.visible["interrupted"]);
  ASSERT_EQ(1u, impl_->GetRejectedChanges().size());
  EXPECT_EQ("interrupted", impl_->GetRejectedChanges()[0].app_id);
  Reopen();
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("fresh", Find("gamma.test"));
}

TEST_F(PreferredAppPersistenceTest, HostMayRetireTheWholeOwnerDuringModelPublication) {
  Start();
  host_.changed = base::BindLambdaForTesting([&](std::string id, bool value) {
    if (id == "new" && value) impl_.reset();
  });
  impl_->SetSupportedLinksPreference("new", Links("alpha.test"));
  EXPECT_FALSE(impl_);
  EXPECT_TRUE(Read().empty()); NoRecord("alpha.test");
}

TEST_F(PreferredAppPersistenceTest, DelegateMayRetireTheWholeOwnerBeforeAnyMutationIsInstalled) {
  Start(); impl_->SetSupportedLinksPreference("old", Links("alpha.test"));
  tasks_.RunUntilIdle();
  host_.conflict = base::BindLambdaForTesting([&] { impl_.reset(); return true; });
  impl_->SetSupportedLinksPreference("new", Links("alpha.test"));
  EXPECT_FALSE(impl_); ASSERT_EQ(1u, Read().size()); EXPECT_EQ("old", Read()[0]->app_id);
}

TEST_F(PreferredAppPersistenceTest, QueuedInitialWorkAndOldReadCannotAcquirePostDeletionAuthority) {
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(Entries("first", "alpha.test")));
  RecordInfo record; const GURL beta("https://beta.test/");
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(beta, url::Origin::Create(beta), true, &record));
  auto delayed = std::make_shared<DelayedStorage>(MakePreferredAppStorage(store_));
  delayed->delay_reads = true;
  Start(delayed);
  ASSERT_EQ(1u, delayed->reads.size());
  impl_->SetSupportedLinksPreference("old-request", Links("beta.test"));
  Delete("beta.test");  // preferred value unchanged, ownership still retires
  tasks_.RunUntilIdle();
  ASSERT_EQ(2u, delayed->reads.size());
  DelayedStorage::Release(&delayed->reads);
  EXPECT_FALSE(impl_->preferred_apps_list().IsInitialized());
  DelayedStorage::Release(&delayed->reads);
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_FALSE(Find("beta.test"));
  NoRecord("beta.test"); EXPECT_EQ(1u, impl_->GetRejectedChanges().size());
  ASSERT_FALSE(host_.errors.empty()); EXPECT_EQ(Result::kConflict, host_.errors.back());
}

TEST_F(PreferredAppPersistenceTest, QueuedWriteCannotRecreateDeletedGenerationAndIntentIsRetained) {
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(Entries("old", "alpha.test")));
  auto delayed = std::make_shared<DelayedStorage>(MakePreferredAppStorage(store_));
  delayed->delay_writes = true;
  Start(delayed);
  impl_->SetSupportedLinksPreference("new", Links("alpha.test"));
  ASSERT_EQ(1u, delayed->writes.size());
  Delete("alpha.test"); tasks_.RunUntilIdle();
  DelayedStorage::Release(&delayed->writes);
  EXPECT_FALSE(Find("alpha.test")); EXPECT_TRUE(Read().empty()); NoRecord("alpha.test");
  ASSERT_EQ(1u, impl_->GetUnsavedValues().size());
  EXPECT_EQ("new", impl_->GetUnsavedValues()[0][0]->app_id);
}

TEST_F(PreferredAppPersistenceTest, OldWriteCannotOverwriteAReplacementFromAnotherNativeProducer) {
  Start();
  auto delayed = std::make_shared<DelayedStorage>(MakePreferredAppStorage(store_));
  delayed->delay_writes = true; Start(delayed);
  impl_->SetSupportedLinksPreference("old-request", Links("alpha.test"));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(Entries("external", "beta.test")));
  tasks_.RunUntilIdle();
  DelayedStorage::Release(&delayed->writes);
  EXPECT_FALSE(Find("alpha.test")); EXPECT_EQ("external", Find("beta.test"));
  ASSERT_EQ(1u, impl_->GetUnsavedValues().size());
  Reopen(); EXPECT_EQ("external", Find("beta.test")); EXPECT_FALSE(Find("alpha.test"));
}

TEST_F(PreferredAppPersistenceTest, OldAcknowledgementCannotReplaceTheNewLiveProjection) {
  auto delayed = std::make_shared<DelayedStorage>(MakePreferredAppStorage(store_));
  delayed->delay_acks = true; Start(delayed);
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, delayed->acks.size());
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(Entries("external", "beta.test")));
  tasks_.RunUntilIdle();
  DelayedStorage::Release(&delayed->acks);
  EXPECT_FALSE(Find("alpha.test")); EXPECT_EQ("external", Find("beta.test"));
  EXPECT_FALSE(host_.visible["first"]); EXPECT_TRUE(host_.visible["external"]);
}

TEST_F(PreferredAppPersistenceTest, InFlightAcknowledgementChainsOnlyItsOwnNewerDesiredVersion) {
  auto delayed = std::make_shared<DelayedStorage>(MakePreferredAppStorage(store_));
  delayed->delay_acks = true; Start(delayed);
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  impl_->SetSupportedLinksPreference("second", Links("beta.test"));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, delayed->acks.size()); EXPECT_EQ(1u, Read().size());
  DelayedStorage::Release(&delayed->acks);
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, delayed->acks.size()); EXPECT_EQ(2u, Read().size());
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
  DelayedStorage::Release(&delayed->acks);
  EXPECT_TRUE(impl_->GetUnsavedValues().empty());
  Reopen(); EXPECT_EQ("first", Find("alpha.test")); EXPECT_EQ("second", Find("beta.test"));
}

TEST_F(PreferredAppPersistenceTest, OrdinaryNativeFileCompositionRemainsAsyncAndColdReadable) {
  const auto directory = temp_.GetPath().AppendASCII("ordinary");
  ASSERT_TRUE(base::CreateDirectory(directory));
  impl_ = std::make_unique<Native>(&host_, directory);
  EXPECT_FALSE(impl_->preferred_apps_list().IsInitialized());
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  tasks_.RunUntilIdle();
  EXPECT_EQ("first", Find("alpha.test")); EXPECT_TRUE(impl_->GetUnsavedValues().empty());
  impl_.reset();
  impl_ = std::make_unique<Native>(&host_, directory); tasks_.RunUntilIdle();
  EXPECT_EQ("first", Find("alpha.test"));
  EXPECT_EQ(Result::kOk, impl_->storage_status());
}

TEST_F(PreferredAppPersistenceTest, MissingOrdinaryFileIsInitializedBeforeWriteCompletion) {
  const auto directory = temp_.GetPath().AppendASCII("ordinary");
  ASSERT_TRUE(base::CreateDirectory(directory));
  const auto path = directory.AppendASCII("PreferredApps");
  bool completed = false;
  impl_ = std::make_unique<Native>(&host_, directory, base::OnceClosure(),
      base::BindLambdaForTesting([&] {
        std::string bytes;
        ASSERT_TRUE(base::ReadFileToString(path, &bytes));
        auto value = base::JSONReader::Read(bytes, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
        ASSERT_TRUE(value && value->is_dict());
        EXPECT_TRUE(apps::IsUpgradedForSharing(*value));
        EXPECT_TRUE(apps::ParseValueToPreferredApps(*value).empty());
        completed = true;
      }));
  EXPECT_FALSE(completed);
  tasks_.RunUntilIdle();
  EXPECT_TRUE(completed);
  EXPECT_TRUE(impl_->preferred_apps_list().IsInitialized());
  EXPECT_EQ(Result::kOk, impl_->storage_status());
}

TEST_F(PreferredAppPersistenceTest, OrdinaryFormatUpgradeIsDurableAndPreservesNativeChoices) {
  const auto directory = temp_.GetPath().AppendASCII("ordinary");
  ASSERT_TRUE(base::CreateDirectory(directory));
  const auto path = directory.AppendASCII("PreferredApps");
  auto old = apps::ConvertPreferredAppsToValue(Entries("first", "alpha.test"));
  old.GetDict().Set(apps::kVersionKey, 0);
  std::string bytes;
  ASSERT_TRUE(base::JSONWriter::Write(old, &bytes));
  ASSERT_TRUE(base::WriteFile(path, bytes));
  bool completed = false;
  impl_ = std::make_unique<Native>(&host_, directory, base::OnceClosure(),
      base::BindLambdaForTesting([&] {
        std::string persisted;
        ASSERT_TRUE(base::ReadFileToString(path, &persisted));
        auto value = base::JSONReader::Read(persisted, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
        ASSERT_TRUE(value && value->is_dict());
        EXPECT_TRUE(apps::IsUpgradedForSharing(*value));
        const auto entries = apps::ParseValueToPreferredApps(*value);
        ASSERT_EQ(1u, entries.size());
        EXPECT_EQ("first", entries[0]->app_id);
        completed = true;
      }));
  EXPECT_FALSE(completed);
  tasks_.RunUntilIdle();
  EXPECT_TRUE(completed);
  EXPECT_EQ("first", Find("alpha.test"));
  impl_.reset();
  impl_ = std::make_unique<Native>(&host_, directory);
  tasks_.RunUntilIdle();
  EXPECT_EQ("first", Find("alpha.test"));
}

TEST_F(PreferredAppPersistenceTest, OrdinaryAsyncWriteReportsFailureAndRetainsDesiredState) {
  const auto directory = temp_.GetPath().AppendASCII("ordinary");
  ASSERT_TRUE(base::CreateDirectory(directory));
  impl_ = std::make_unique<Native>(&host_, directory); tasks_.RunUntilIdle();
  ASSERT_TRUE(impl_->preferred_apps_list().IsInitialized());
  // A non-file leaf cannot be atomically replaced by the native writer.
  ASSERT_TRUE(base::DeleteFile(directory.AppendASCII("PreferredApps")));
  ASSERT_TRUE(base::CreateDirectory(directory.AppendASCII("PreferredApps")));
  impl_->SetSupportedLinksPreference("unsaved", Links("alpha.test"));
  tasks_.RunUntilIdle();
  EXPECT_EQ(Result::kIo, impl_->storage_status()); EXPECT_FALSE(Find("alpha.test"));
  ASSERT_FALSE(host_.errors.empty()); EXPECT_EQ(Result::kIo, host_.errors.back());
  ASSERT_EQ(1u, impl_->GetUnsavedValues().size());
  EXPECT_EQ("unsaved", impl_->GetUnsavedValues()[0][0]->app_id);
}

TEST_F(PreferredAppPersistenceTest, DeniedOrdinaryDirectoryIsNotAValidNewEmptyModel) {
  const auto directory = temp_.GetPath().AppendASCII("denied");
  ASSERT_TRUE(base::CreateDirectory(directory));
  ASSERT_TRUE(base::WriteFile(directory.AppendASCII("PreferredApps"), "private existing data"));
  ASSERT_TRUE(base::SetPosixFilePermissions(directory, 0000));
  impl_ = std::make_unique<Native>(&host_, directory); tasks_.RunUntilIdle();
  ASSERT_TRUE(base::SetPosixFilePermissions(directory, 0700));
  EXPECT_FALSE(impl_->preferred_apps_list().IsInitialized());
  EXPECT_EQ(Result::kIo, impl_->storage_status());
  ASSERT_FALSE(host_.errors.empty()); EXPECT_EQ(Result::kIo, host_.errors.back());
}

TEST_F(PreferredAppPersistenceTest, NativeErrorCallbackCanRetireTheOwnerDuringPublicationFailure) {
  Start();
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  host_.error = base::BindLambdaForTesting([&] { impl_.reset(); });
  impl_->SetSupportedLinksPreference("failed", Links("alpha.test"));
  tasks_.RunUntilIdle();
  EXPECT_FALSE(impl_); ASSERT_FALSE(host_.errors.empty());
  EXPECT_EQ(Result::kIo, host_.errors.front());
  files_->SetFaultForTesting({});
}

TEST_F(PreferredAppPersistenceTest, IndependentNativeImplProcessReadsAcknowledgedEncryptedChoices) {
  Start();
  impl_->SetSupportedLinksPreference("first", Links("alpha.test"));
  impl_->SetSupportedLinksPreference("second", Links("beta.test"));
  tasks_.RunUntilIdle();
  impl_.reset(); store_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyPreferredImplChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped); EXPECT_EQ(0, exit);
}

TEST_F(PreferredAppPersistenceTest, MalformedOrdinaryFileIsNotARepairedSuccessfulEmptyModel) {
  const auto directory = temp_.GetPath().AppendASCII("ordinary");
  ASSERT_TRUE(base::CreateDirectory(directory));
  const auto path = directory.AppendASCII("PreferredApps");
  const std::string malformed = "{\"preferred_apps\":[1],\"version\":1}";
  ASSERT_TRUE(base::WriteFile(path, malformed));
  impl_ = std::make_unique<Native>(&host_, directory); tasks_.RunUntilIdle();
  EXPECT_FALSE(impl_->preferred_apps_list().IsInitialized());
  EXPECT_EQ(Result::kInvalid, impl_->storage_status());
  ASSERT_FALSE(host_.errors.empty());
  impl_->SetSupportedLinksPreference("blocked", Links("alpha.test"));
  tasks_.RunUntilIdle();
  std::string actual; ASSERT_TRUE(base::ReadFileToString(path, &actual));
  EXPECT_EQ(malformed, actual); EXPECT_EQ(1u, impl_->GetRejectedChanges().size());
}
}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyPreferredImplChild) {
  base::test::TaskEnvironment tasks;
  std::shared_ptr<RecordFileSystem> files;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  if (RecordFileSystem::Open(path, Key(), &files) != FileStatus::kOk) return 1;
  auto store = base::MakeRefCounted<SitePrefStore>(files, base::BindRepeating([](FileStatus) {}));
  if (store->ReadPrefs() != PersistentPrefStore::PREF_READ_ERROR_NONE) return 2;
  Host host;
  Native native(&host, MakePreferredAppStorage(store)); tasks.RunUntilIdle();
  const auto& handle = native.preferred_apps_list();
  return handle.IsInitialized() && handle.GetEntrySize() == 2 &&
      handle.FindPreferredAppForUrl(GURL("https://alpha.test/")) == "first" &&
      handle.FindPreferredAppForUrl(GURL("https://beta.test/")) == "second" &&
      host.visible["first"] && host.visible["second"] ? 0 : 3;
}
}  // namespace alloy::site_records
