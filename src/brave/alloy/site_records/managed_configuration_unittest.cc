// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "chrome/browser/device_api/managed_configuration_api.h"

#include <atomic>
#include <unistd.h>

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/record_value_store.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include "chrome/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "mojo/core/embedder/embedder.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace alloy::site_records {
namespace {
using NativeStore = ManagedConfigurationStore;
using Status = value_store::ValueStore::Status;
SecretKey ManagedKey() { ObjectId key; key.fill(0xa7); return SecretKey(key); }
base::FilePath Database(const url::Origin& origin) {
  return base::FilePath("/managed-" + base::HexEncode(origin.Serialize()));
}
NativeStore::Options Binding(std::shared_ptr<RecordFileSystem> files,
    RecordCapability cap, const url::Origin& origin, std::atomic<int>* errors) {
  NativeStore::Options options;
  options.admitted = base::BindRepeating([](RecordCapability cap) {
    return cap.Allows(StorePurpose::kSiteSettings, StorageAccess::kReadRecordFiles);
  }, cap);
  options.create = base::BindRepeating([](std::shared_ptr<RecordFileSystem> files,
      RecordCapability cap, base::FilePath path) -> std::unique_ptr<value_store::ValueStore> {
    std::unique_ptr<value_store::LeveldbValueStore> store;
    if (CreateRecordValueStore(files, cap, StorePurpose::kSiteSettings, path, &store) != FileStatus::kOk) return nullptr;
    return store;
  }, files, cap, Database(origin));
  options.error = base::BindRepeating([](std::atomic<int>* count, const Status&) {
    if (count) count->fetch_add(1);
  }, errors);
  return options;
}

class ManagedConfigurationTest : public testing::Test,
                                 public ManagedConfigurationAPI::Observer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, ManagedKey(), {}, &files_));
    Admit(a_);
    Admit(b_);
    OpenPrefs();
    OpenApi();
  }
  void TearDown() override { Close(); }
  void OpenPrefs() {
    pref_store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, pref_store_->ReadPrefs());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    ManagedConfigurationAPI::RegisterProfilePrefs(registry.get());
    PrefServiceFactory factory;
    factory.set_user_prefs(pref_store_);
    prefs_ = factory.Create(registry);
  }
  void OpenApi() {
    api_ = std::make_unique<ManagedConfigurationAPI>(prefs_.get(), base::BindRepeating(
        [](ManagedConfigurationTest* fixture, const url::Origin& origin) -> std::optional<NativeStore::Options> {
          ++fixture->bindings_;
          auto found = fixture->admitted_.find(origin);
          if (found == fixture->admitted_.end() || !found->second.active()) return std::nullopt;
          return Binding(fixture->files_, found->second, origin, &fixture->errors_);
        }, base::Unretained(this)), base::BindRepeating(
        [](network::TestURLLoaderFactory* factory) -> network::mojom::URLLoaderFactory* { return factory; }, &network_));
  }
  void Close() {
    if (observing_ && api_) { api_->RemoveObserver(this); observing_ = false; }
    api_.reset();
    tasks_.RunUntilIdle();  // Native SequenceBound owners really close before reopen.
    prefs_.reset();
    pref_store_.reset();
    tasks_.RunUntilIdle();
  }
  void Admit(const url::Origin& origin) {
    RecordInfo record;
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(origin.GetURL(), origin, true, &record));
    admitted_[origin] = files_->Capability(record.id, StorePurpose::kSiteSettings);
  }
  void Policy(const url::Origin& origin, std::string url, std::string hash) {
    policies_[origin] = {std::move(url), std::move(hash)};
    ApplyPolicies();
  }
  void ApplyPolicies() {
    base::ListValue policies;
    for (const auto& [origin, setting] : policies_) {
      base::DictValue policy;
      policy.Set("origin", origin.Serialize());
      policy.Set("managed_configuration_url", setting.first);
      policy.Set("managed_configuration_hash", setting.second);
      policies.Append(std::move(policy));
    }
    prefs_->SetList(prefs::kManagedConfigurationPerOrigin, std::move(policies));
  }
  std::optional<base::DictValue> Get(const url::Origin& origin,
      std::vector<std::string> keys = {"key", "nested.key", "number"}) {
    base::test::TestFuture<std::optional<base::DictValue>> reply;
    api_->GetOriginPolicyConfiguration(origin, keys, reply.GetCallback());
    tasks_.RunUntilIdle();
    return reply.Take();
  }
  void Expect(const url::Origin& origin, std::string key, std::string value) {
    auto result = Get(origin, {key});
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->FindString(key));
    EXPECT_EQ(value, *result->FindString(key));
  }
  NativeStore::Version Version(const url::Origin& origin) {
    if (api_) {
      base::test::TestFuture<NativeStore::Version> reply;
      api_->GetOriginPolicyVersion(origin, reply.GetCallback());
      tasks_.RunUntilIdle();
      return reply.Take();
    }
    NativeStore store(origin, Binding(files_, admitted_.at(origin), origin, &errors_));
    return store.GetVersion();
  }
  void Observe() { observing_ = true; api_->AddObserver(this); }
  void OnManagedConfigurationChanged() override {
    ++notifications_;
    if (revoke_on_notification_) api_->RevokeOrigin(a_);
  }
  const url::Origin& GetOrigin() const override { return a_; }
  void CiphertextScan() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-F", "-i", "-l",
                            "-e", "alpha.test", "-e", "beta.test", "-e", "MANAGED_SNAPSHOT_PAYLOAD_763fa"})
      command.AppendArg(arg);
    command.AppendArgPath(path_);
    std::string output;
    int exit = -1;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }

  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  const url::Origin a_ = url::Origin::Create(GURL("https://alpha.test/"));
  const url::Origin b_ = url::Origin::Create(GURL("https://beta.test/"));
  std::shared_ptr<RecordFileSystem> files_;
  std::map<url::Origin, RecordCapability> admitted_;
  std::map<url::Origin, std::pair<std::string, std::string>> policies_;
  std::atomic<int> errors_{0};
  size_t bindings_ = 0;
  int notifications_ = 0;
  bool observing_ = false;
  bool revoke_on_notification_ = false;
  scoped_refptr<SitePrefStore> pref_store_;
  std::unique_ptr<PrefService> prefs_;
  network::TestURLLoaderFactory network_;
  std::unique_ptr<ManagedConfigurationAPI> api_;
};

TEST_F(ManagedConfigurationTest, NativeNetworkCacheConsumerAndLiteralKeysShareOneVersion) {
  Observe();
  network_.AddResponse("https://config.test/a", R"({"key":"MANAGED_SNAPSHOT_PAYLOAD_763fa","nested.key":{"enabled":true},"number":42})");
  Policy(a_, "https://config.test/a", "version-one");
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"MANAGED_SNAPSHOT_PAYLOAD_763fa\"");
  Expect(a_, "nested.key", "{\"enabled\":true}");
  Expect(a_, "number", "42");
  EXPECT_EQ(1, notifications_);
  auto version = Version(a_);
  EXPECT_TRUE(version.success);
  EXPECT_EQ("version-one", version.hash);
  EXPECT_FALSE(prefs_->FindPreference(prefs::kLastManagedConfigurationHashForOrigin));
  ASSERT_EQ(FileStatus::kOk, pref_store_->Flush());
  CiphertextScan();
  const auto requests = network_.total_requests();
  api_->RefreshOrigin(a_);
  tasks_.RunUntilIdle();
  EXPECT_EQ(requests, network_.total_requests());
}

TEST_F(ManagedConfigurationTest, HashOnlyUpdateDoesNotReportADataChangeAndRemovalClearsSnapshot) {
  Observe();
  network_.AddResponse("https://config.test/a", R"({"key":"stable"})");
  Policy(a_, "https://config.test/a", "v1");
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, notifications_);
  Policy(a_, "https://config.test/a", "v2");
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, notifications_);
  EXPECT_EQ("v2", Version(a_).hash);
  policies_.erase(a_);
  ApplyPolicies();
  tasks_.RunUntilIdle();
  EXPECT_EQ(2, notifications_);
  EXPECT_FALSE(Version(a_).hash);
  EXPECT_FALSE(Get(a_));
}

TEST_F(ManagedConfigurationTest, OldVersionCallbackCannotRecaptureDeletedOrReplacementGeneration) {
  Policy(a_, "https://config.test/a", "v1");  // Native version query remains queued.
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, pref_store_->DeleteSite(a_.GetURL(), [&] { api_->RevokeOrigin(a_); }, &result));
  tasks_.RunUntilIdle();
  EXPECT_EQ(0u, network_.total_requests());
  const auto bindings = bindings_;
  EXPECT_FALSE(Get(a_));
  EXPECT_EQ(bindings, bindings_);
  RecordInfo missing;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_.GetURL(), a_, false, &missing));
  Admit(a_);  // Explicit fresh browser-owned admission.
  network_.AddResponse("https://config.test/a", R"({"key":"fresh"})");
  ApplyPolicies();  // External policy is explicitly reasserted after fresh admission.
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"fresh\"");
  EXPECT_EQ(1u, network_.total_requests());
}

TEST_F(ManagedConfigurationTest, LateNetworkCompletionCannotRecreateADeletedRecordOrAffectAnotherSite) {
  network_.AddResponse("https://config.test/b", R"({"key":"other"})");
  Policy(b_, "https://config.test/b", "b1");
  Policy(a_, "https://config.test/a", "a1");
  tasks_.RunUntilIdle();
  ASSERT_TRUE(network_.IsPending("https://config.test/a"));
  auto late = std::move(network_.pending_requests()->front());
  network_.pending_requests()->erase(network_.pending_requests()->begin());
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, pref_store_->DeleteSite(a_.GetURL(), [&] { api_->RevokeOrigin(a_); }, &result));
  const auto bindings = bindings_;
  network_.SimulateResponseWithoutRemovingFromPendingList(&late, R"({"key":"late"})");
  tasks_.RunUntilIdle();
  EXPECT_FALSE(Get(a_));
  EXPECT_EQ(bindings, bindings_);
  Expect(b_, "key", "\"other\"");
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_.GetURL(), a_, false, &absent));
}

TEST_F(ManagedConfigurationTest, AStaleDownloaderCannotResetOrPublishOverANewerPolicy) {
  Policy(a_, "https://config.test/old", "old");
  tasks_.RunUntilIdle();
  ASSERT_TRUE(network_.IsPending("https://config.test/old"));
  auto late = std::move(network_.pending_requests()->front());
  network_.pending_requests()->erase(network_.pending_requests()->begin());
  Policy(a_, "https://config.test/new", "new");
  tasks_.RunUntilIdle();
  ASSERT_TRUE(network_.IsPending("https://config.test/new"));
  network_.SimulateResponseWithoutRemovingFromPendingList(&late, R"({"key":"old"})");
  tasks_.RunUntilIdle();
  EXPECT_TRUE(network_.IsPending("https://config.test/new"));
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/new", R"({"key":"new"})"));
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"new\"");
  EXPECT_EQ("new", Version(a_).hash);
}

TEST_F(ManagedConfigurationTest, MalformedDownloadPreservesTheOldSnapshotAndCanRetryTheSameDesiredHash) {
  network_.AddResponse("https://config.test/a", R"({"key":"old"})");
  Policy(a_, "https://config.test/a", "v1");
  tasks_.RunUntilIdle();
  network_.AddResponse("https://config.test/a", "{invalid json");
  Policy(a_, "https://config.test/a", "v2");
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"old\"");
  EXPECT_EQ("v1", Version(a_).hash);
  network_.AddResponse("https://config.test/a", R"({"key":"new"})");
  api_->RefreshOrigin(a_);
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"new\"");
  EXPECT_EQ("v2", Version(a_).hash);
}

TEST_F(ManagedConfigurationTest, FailedCachePublicationCannotAdvanceHashOrSuppressRestartRetry) {
  network_.AddResponse("https://config.test/a", R"({"key":"old"})");
  Policy(a_, "https://config.test/a", "v1");
  tasks_.RunUntilIdle();
  network_.EraseResponse(GURL("https://config.test/a"));
  Policy(a_, "https://config.test/a", "v2");
  tasks_.RunUntilIdle();
  ASSERT_TRUE(network_.IsPending("https://config.test/a"));
  ASSERT_EQ(FileStatus::kOk, pref_store_->Flush()); // Desired policy acknowledged, cache still v1.
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/a", R"({"key":"new"})"));
  tasks_.RunUntilIdle();
  EXPECT_FALSE(files_->unlocked());
  EXPECT_FALSE(Get(a_));
  files_->SetFaultForTesting({});
  Close();
  EXPECT_EQ(FileStatus::kIo, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ManagedKey(), &files_));
  admitted_.clear();
  Admit(a_);
  Admit(b_);
  auto old = Version(a_);
  EXPECT_TRUE(old.success);
  EXPECT_EQ("v1", old.hash);
  network_.AddResponse("https://config.test/a", R"({"key":"new"})");
  OpenPrefs();
  OpenApi();
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"new\"");
  EXPECT_EQ("v2", Version(a_).hash);
}

TEST_F(ManagedConfigurationTest, NativeSnapshotChecksExactOriginAndCachedReadsFailAfterLock) {
  const auto sibling = url::Origin::Create(GURL("https://alpha.test:8443/"));
  Admit(sibling);
  auto first = std::make_unique<NativeStore>(a_, Binding(files_, admitted_.at(a_), a_, &errors_));
  auto second = std::make_unique<NativeStore>(sibling, Binding(files_, admitted_.at(sibling), sibling, &errors_));
  base::DictValue one; one.Set("key", "\"first\"");
  base::DictValue two; two.Set("key", "\"second\"");
  ASSERT_TRUE(first->SetCurrentPolicy("one", std::move(one)).success);
  ASSERT_TRUE(second->SetCurrentPolicy("two", std::move(two)).success);
  EXPECT_EQ("\"first\"", *first->Get({"key"})->FindString("key"));
  EXPECT_EQ("\"second\"", *second->Get({"key"})->FindString("key"));
  // The same record groups ports, but origin identity remains inside each native shard.
  first.reset();
  {
    NativeStore wrong(sibling, Binding(files_, admitted_.at(a_), a_, &errors_));
    EXPECT_FALSE(wrong.GetVersion().success);
  }
  first = std::make_unique<NativeStore>(a_, Binding(files_, admitted_.at(a_), a_, &errors_));
  EXPECT_EQ("\"first\"", *first->Get({"key"})->FindString("key"));
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  EXPECT_FALSE(first->Get({"key"}));
  EXPECT_FALSE(second->GetVersion().success);
}

TEST_F(ManagedConfigurationTest, NativeSnapshotSurvivesAnIndependentConsumerProcess) {
  network_.AddResponse("https://config.test/a", R"({"key":"MANAGED_SNAPSHOT_PAYLOAD_763fa"})");
  Policy(a_, "https://config.test/a", "native-cold-version");
  tasks_.RunUntilIdle();
  ASSERT_EQ(FileStatus::kOk, pref_store_->Flush());
  CiphertextScan();
  Close();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyManagedSnapshotChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  EXPECT_EQ(0, exit);
  CiphertextScan();
}

TEST_F(ManagedConfigurationTest, ReentrantRevocationStopsRemainingNativeNotifications) {
  Policy(a_, "https://config.test/a", "v1");
  tasks_.RunUntilIdle();
  ASSERT_TRUE(network_.IsPending("https://config.test/a"));
  Observe();
  class Follower : public ManagedConfigurationAPI::Observer {
   public:
    explicit Follower(url::Origin origin) : origin_(std::move(origin)) {}
    void OnManagedConfigurationChanged() override { ++calls; }
    const url::Origin& GetOrigin() const override { return origin_; }
    url::Origin origin_;
    int calls = 0;
  } follower(a_);
  api_->AddObserver(&follower);
  revoke_on_notification_ = true;
  ASSERT_TRUE(network_.SimulateResponseForPendingRequest("https://config.test/a", R"({"key":"value"})"));
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, notifications_);
  EXPECT_EQ(0, follower.calls);
  EXPECT_FALSE(Get(a_));
  api_->RemoveObserver(&follower);
}

TEST_F(ManagedConfigurationTest, MissingPolicyFieldsDoNotEraseTheLastValidCache) {
  network_.AddResponse("https://config.test/a", R"({"key":"retained"})");
  Policy(a_, "https://config.test/a", "v1");
  tasks_.RunUntilIdle();
  const auto requests = network_.total_requests();
  base::DictValue incomplete;
  incomplete.Set("origin", a_.Serialize());
  base::ListValue list;
  list.Append(std::move(incomplete));
  prefs_->SetList(prefs::kManagedConfigurationPerOrigin, std::move(list));
  tasks_.RunUntilIdle();
  Expect(a_, "key", "\"retained\"");
  EXPECT_EQ("v1", Version(a_).hash);
  EXPECT_EQ(requests, network_.total_requests());
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyManagedSnapshotChild) {
  base::test::TaskEnvironment tasks;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  std::shared_ptr<RecordFileSystem> files;
  if (RecordFileSystem::Open(path, ManagedKey(), &files) != FileStatus::kOk) return 1;
  const auto origin = url::Origin::Create(GURL("https://alpha.test/"));
  RecordInfo record;
  if (files->ResolveRecord(origin.GetURL(), origin, false, &record) != FileStatus::kOk) return 2;
  NativeStore store(origin, Binding(files, files->Capability(record.id, StorePurpose::kSiteSettings), origin, nullptr));
  const auto version = store.GetVersion();
  auto value = store.Get({"key"});
  if (!version.success || version.hash != "native-cold-version" || !value ||
      !value->FindString("key") || *value->FindString("key") != "\"MANAGED_SNAPSHOT_PAYLOAD_763fa\"") return 3;
  return 0;
}

}  // namespace alloy::site_records
