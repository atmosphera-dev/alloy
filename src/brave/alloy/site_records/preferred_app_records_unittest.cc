// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/preferred_app_records.h"

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/services/app_service/public/cpp/intent_util.h"
#include "components/services/app_service/public/cpp/preferred_apps_list.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace alloy::site_records {
namespace {
SecretKey AppKey() { ObjectId key; key.fill(0x56); return SecretKey(key); }
apps::IntentFilterPtr Filter(std::string host, apps::PatternMatchType match = apps::PatternMatchType::kLiteral) {
  auto filter = apps_util::MakeIntentFilterForUrlScope(GURL("https://alpha.test/"));
  for (auto& condition : filter->conditions) if (condition->condition_type == apps::ConditionType::kAuthority) {
    condition->condition_values.clear();
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>(host, match));
  }
  return filter;
}
apps::PreferredAppPtr App(std::string id, apps::IntentFilterPtr filter) {
  return std::make_unique<apps::PreferredApp>(std::move(filter), std::move(id));
}
std::string Text(const base::DictValue& dict) {
  std::string text;
  EXPECT_TRUE(base::JSONWriter::Write(dict, &text));
  return text;
}

class PreferredAppRecordsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, AppKey(), {}, &files_));
    Open();
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
  }
  void Reopen() {
    store_.reset();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AppKey(), &files_));
    Open();
  }
  apps::PreferredApps Read() {
    apps::PreferredApps entries;
    EXPECT_EQ(FileStatus::kOk, store_->GetPreferredApps(&entries));
    return entries;
  }
  base::DictValue Shard(std::string domain) {
    RecordCapability cap = files_->ProfileCapability(StorePurpose::kProfileSettings);
    auto purpose = StorePurpose::kProfileSettings;
    if (!domain.empty()) {
      const GURL url("https://" + domain + "/");
      RecordInfo record;
      EXPECT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), false, &record));
      purpose = StorePurpose::kSiteSettings;
      cap = files_->Capability(record.id, purpose);
    }
    auto shard = base::MakeRefCounted<RecordPrefStore>(files_, cap, purpose, "preferences",
        base::BindRepeating([](FileStatus) {}));
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, shard->ReadPrefs());
    const base::Value* value = nullptr;
    if (!shard->GetValue("values", &value)) return {};
    return value->GetDict().Clone();
  }
  void NoPlaintext() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-F", "-i", "-l",
                            "-e", "alpha.test", "-e", "beta.test", "-e", "native-chooser-private"}) command.AppendArg(arg);
    command.AppendArgPath(path_);
    std::string output;
    int exit = -1;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
};

TEST_F(PreferredAppRecordsTest, LosslessNativeIdentityAttributesAuthorityAndConditionOrder) {
  auto filter = Filter("alpha.test:443");
  filter->activity_name = "native-chooser-private";
  filter->activity_label = "native label";
  for (auto& condition : filter->conditions) if (condition->condition_type == apps::ConditionType::kAuthority) {
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>("beta.test:8443", apps::PatternMatchType::kLiteral));
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>(".alpha.test", apps::PatternMatchType::kSuffix));
  }
  apps::PreferredApps original;
  original.push_back(App("chooser", std::move(filter)));
  base::ListValue fragments;
  ASSERT_EQ(FileStatus::kOk, EncodePreferredAppRecords(original, &fragments));
  ASSERT_EQ(2u, fragments.size());
  apps::PreferredApps joined;
  ASSERT_EQ(FileStatus::kOk, DecodePreferredAppRecords(fragments, &joined));
  ASSERT_EQ(1u, joined.size());
  EXPECT_EQ(*original[0]->intent_filter, *joined[0]->intent_filter);
  EXPECT_EQ(original[0]->app_id, joined[0]->app_id);
  apps::PreferredAppsList native(nullptr);
  native.Init(std::move(joined));
  EXPECT_EQ("chooser", native.FindPreferredAppForUrl(GURL("https://beta.test:8443/")));
  EXPECT_FALSE(native.FindPreferredAppForUrl(GURL("https://beta.test:443/")));
  EXPECT_EQ("chooser", native.FindPreferredAppForUrl(GURL("https://sub.alpha.test/")));
}

TEST_F(PreferredAppRecordsTest, ShardsContainOnlyTheirNativeUrlGrantsAndGlobalChoiceSurvives) {
  auto filter = Filter("alpha.test:443");
  for (auto& condition : filter->conditions) if (condition->condition_type == apps::ConditionType::kAuthority)
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>("beta.test:443", apps::PatternMatchType::kLiteral));
  auto mime = std::make_unique<apps::IntentFilter>();
  mime->AddSingleValueCondition(apps::ConditionType::kMimeType, "text/plain", apps::PatternMatchType::kMimeType);
  apps::PreferredApps original;
  original.push_back(App("chooser", std::move(filter)));
  original.push_back(App("text-app", std::move(mime)));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  EXPECT_EQ(std::string::npos, Text(Shard("alpha.test")).find("beta.test"));
  EXPECT_EQ(std::string::npos, Text(Shard("beta.test")).find("alpha.test"));
  EXPECT_EQ(std::string::npos, Text(Shard("")).find("alpha.test"));
  EXPECT_NE(std::string::npos, Text(Shard("")).find("text-app"));
  NoPlaintext();
  Reopen();
  auto joined = Read();
  ASSERT_EQ(2u, joined.size());
  EXPECT_EQ(*original[0]->intent_filter, *joined[0]->intent_filter);
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(GURL("https://alpha.test/"), {}, &result));
  apps::PreferredAppsList native(nullptr);
  native.Init(Read());
  EXPECT_FALSE(native.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_EQ("chooser", native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  EXPECT_EQ(2u, native.GetEntrySize());
  EXPECT_EQ(std::string::npos, Text(Shard("beta.test")).find("alpha.test"));
  Reopen();
  EXPECT_EQ(2u, Read().size());
}

TEST_F(PreferredAppRecordsTest, DeletingAnAuthorityNeverRemovesAnAndConstraintAndBroadensGrant) {
  auto constrained = Filter("alpha.test:443");
  for (auto& condition : constrained->conditions) if (condition->condition_type == apps::ConditionType::kAuthority)
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>("beta.test:443", apps::PatternMatchType::kLiteral));
  constrained->AddSingleValueCondition(apps::ConditionType::kAuthority, "alpha.test:443", apps::PatternMatchType::kLiteral);
  apps::PreferredApps original;
  original.push_back(App("constrained", std::move(constrained)));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  apps::PreferredAppsList native(nullptr);
  native.Init(Read());
  EXPECT_EQ("constrained", native.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_FALSE(native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(GURL("https://alpha.test/"), {}, &result));
  Reopen();
  native.Init(Read());
  EXPECT_FALSE(native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  ASSERT_EQ(1u, native.GetEntrySize());
  const auto& last = native.GetReference()[0]->intent_filter->conditions.back();
  EXPECT_EQ(apps::ConditionType::kAuthority, last->condition_type);
  EXPECT_TRUE(last->condition_values.empty());
}

TEST_F(PreferredAppRecordsTest, NativeLastMatchOrderingIsIndependentOfShardEnumeration) {
  apps::PreferredApps original;
  original.push_back(App("site-choice", Filter("beta.test:443")));
  original.push_back(App("global-choice", Filter("beta.", apps::PatternMatchType::kPrefix)));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  apps::PreferredAppsList native(nullptr);
  native.Init(Read());
  EXPECT_EQ("global-choice", native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  Reopen();
  native.Init(Read());
  EXPECT_EQ("global-choice", native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  EXPECT_EQ("site-choice", native.GetReference()[0]->app_id);
  EXPECT_EQ("global-choice", native.GetReference()[1]->app_id);
}

TEST_F(PreferredAppRecordsTest, SingleHostGlobsRetireWithTheirRecordAfterColdReopen) {
  struct Case { const char* pattern; const char* url; };
  for (const auto& sample : {Case{"localhost", "https://localhost/"},
                            Case{R"(local\host)", "https://localhost/"},
                            Case{"localhost:8443", "https://localhost:8443/"},
                            Case{"[::1]:443", "https://[::1]/"}}) {
    apps::PreferredApps choices;
    choices.push_back(App("single-host", Filter(sample.pattern, apps::PatternMatchType::kGlob)));
    choices.push_back(App("unrelated", Filter("beta.test")));
    ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(choices));
    const GURL url(sample.url);
    RecordInfo visit;
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &visit));
    Reopen();
    apps::PreferredAppsList native(nullptr);
    native.Init(Read());
    ASSERT_EQ("single-host", native.FindPreferredAppForUrl(url)) << sample.pattern;
    ASSERT_EQ("unrelated", native.FindPreferredAppForUrl(GURL("https://beta.test/")));
    RecordDeletion deletion;
    ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(url, {}, &deletion));
    Reopen();
    native.Init(Read());
    ASSERT_FALSE(native.FindPreferredAppForUrl(url)) << sample.pattern;
    EXPECT_EQ("unrelated", native.FindPreferredAppForUrl(GURL("https://beta.test/")));
  }
}

TEST_F(PreferredAppRecordsTest, MultiHostGlobsRetainTheirExplicitGlobalDispatchChoice) {
  struct Case { const char* pattern; const char* first; const char* second; };
  for (const auto& sample : {Case{".*", "localhost", "beta.test"},
                            Case{"alpha.test", "alpha.test", "alphaxtest"},
                            Case{R"(alpha\.test)", "alpha.test", "alphaxtest"},
                            Case{"localh*ost", "localhost", "localost"}}) {
    ASSERT_TRUE(apps_util::MatchGlob(sample.first, sample.pattern));
    ASSERT_TRUE(apps_util::MatchGlob(sample.second, sample.pattern));
    apps::PreferredApps choices;
    choices.push_back(App("global", Filter(sample.pattern, apps::PatternMatchType::kGlob)));
    ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(choices));
    const GURL first("https://"+std::string(sample.first)+"/");
    RecordInfo visit;
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(first, url::Origin::Create(first), true, &visit));
    RecordDeletion deletion;
    ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(first, {}, &deletion));
    Reopen();
    apps::PreferredAppsList native(nullptr);
    native.Init(Read());
    EXPECT_EQ("global", native.FindPreferredAppForUrl(first));
    EXPECT_EQ("global", native.FindPreferredAppForUrl(GURL("https://"+std::string(sample.second)+"/")));
  }
}

TEST_F(PreferredAppRecordsTest, MalformedOrSplicedFragmentsFailWithoutAReplacement) {
  apps::PreferredApps original;
  original.push_back(App("chooser", Filter("alpha.test:443")));
  base::ListValue fragments;
  ASSERT_EQ(FileStatus::kOk, EncodePreferredAppRecords(original, &fragments));
  auto corrupted = fragments.Clone();
  auto* filter = corrupted[0].GetDict().FindDict("filter");
  ASSERT_TRUE(filter);
  auto* conditions = filter->FindList("conditions");
  ASSERT_TRUE(conditions);
  (*conditions)[0].GetDict().Set("condition_type", 999);
  apps::PreferredApps output;
  EXPECT_EQ(FileStatus::kInvalid, DecodePreferredAppRecords(corrupted, &output));
  auto duplicated = fragments.Clone();
  duplicated.Append(fragments[0].Clone());
  EXPECT_EQ(FileStatus::kInvalid, DecodePreferredAppRecords(duplicated, &output));
  EXPECT_TRUE(output.empty());
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  EXPECT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  EXPECT_EQ(1u, Read().size());
}

TEST_F(PreferredAppRecordsTest, FailedPublicationDoesNotPersistOrReplayAChangedChoice) {
  apps::PreferredApps original;
  original.push_back(App("old", Filter("alpha.test:443")));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  apps::PreferredApps changed;
  changed.push_back(App("new", Filter("alpha.test:443")));
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  EXPECT_NE(FileStatus::kOk, store_->SetPreferredApps(changed));
  EXPECT_FALSE(files_->unlocked());
  files_->SetFaultForTesting({});
  store_.reset();
  EXPECT_NE(FileStatus::kOk, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AppKey(), &files_));
  Open();
  apps::PreferredAppsList native(nullptr);
  native.Init(Read());
  EXPECT_EQ("old", native.FindPreferredAppForUrl(GURL("https://alpha.test/")));
}

TEST_F(PreferredAppRecordsTest, OldRevisionCannotOverwriteANewerChoice) {
  const auto original = store_->preferred_version();
  apps::PreferredApps entries;
  entries.push_back(App("first", Filter("alpha.test:443")));
  SitePrefStore::PreferredVersion published;
  ASSERT_EQ(FileStatus::kOk, store_->PublishPreferredApps(original, entries, &published));
  EXPECT_EQ(published, store_->preferred_version());
  entries.clear();
  entries.push_back(App("stale", Filter("beta.test:443")));
  EXPECT_EQ(FileStatus::kBusy, store_->PublishPreferredApps(original, entries, &published));
  ASSERT_EQ(1u, Read().size());
  EXPECT_EQ("first", Read()[0]->app_id);
  Reopen();
  EXPECT_EQ("first", Read()[0]->app_id);
}

TEST_F(PreferredAppRecordsTest, DeletionRetiresAdmissionEvenWhenThePreferredValueIsUnchanged) {
  apps::PreferredApps entries;
  entries.push_back(App("first", Filter("alpha.test:443")));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(entries));
  const auto original = store_->preferred_version();
  RecordInfo beta;
  const GURL url("https://beta.test/");
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &beta));
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(url, {}, &result));
  EXPECT_FALSE(store_->PreferredAdmission(original));
  EXPECT_TRUE(store_->PreferredAdmission(store_->preferred_version()));
  SitePrefStore::PreferredVersion published;
  EXPECT_EQ(FileStatus::kBusy, store_->PublishPreferredApps(original, entries, &published));
  EXPECT_EQ("first", Read()[0]->app_id);
}

TEST_F(PreferredAppRecordsTest, CallbackDeletionCannotAcknowledgeADifferentPublishedChoice) {
  apps::PreferredApps original;
  original.push_back(App("old", Filter("alpha.test:443")));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  class DeletingObserver final : public PrefStore::Observer {
   public:
    explicit DeletingObserver(SitePrefStore* store) : store(store) { store->AddObserver(this); }
    ~DeletingObserver() override { store->RemoveObserver(this); }
    void OnInitializationCompleted(bool) override {}
    void OnPrefValueChanged(std::string_view key) override {
      if (key != SitePrefStore::kPreferredApps || deleted) return;
      deleted = true;
      RecordDeletion result;
      EXPECT_EQ(FileStatus::kOk, store->DeleteSite(GURL("https://alpha.test/"), {}, &result));
    }
    SitePrefStore* store;
    bool deleted = false;
  } observer(store_.get());
  apps::PreferredApps desired;
  desired.push_back(App("new", Filter("alpha.test:443")));
  EXPECT_NE(FileStatus::kOk, store_->SetPreferredApps(desired));
  EXPECT_TRUE(observer.deleted);
  EXPECT_TRUE(Read().empty());
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
}

TEST_F(PreferredAppRecordsTest, IndependentProcessRejoinsTheNativeFilterAndReadsTheChoice) {
  apps::PreferredApps original;
  auto filter = Filter("alpha.test:443");
  for (auto& condition : filter->conditions) if (condition->condition_type == apps::ConditionType::kAuthority)
    condition->condition_values.push_back(std::make_unique<apps::ConditionValue>("beta.test:443", apps::PatternMatchType::kLiteral));
  original.push_back(App("native-chooser-private", std::move(filter)));
  ASSERT_EQ(FileStatus::kOk, store_->SetPreferredApps(original));
  NoPlaintext();
  store_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyPreferredAppsChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  EXPECT_EQ(0, exit);
  NoPlaintext();
}
}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyPreferredAppsChild) {
  base::test::TaskEnvironment tasks;
  std::shared_ptr<RecordFileSystem> files;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  if (RecordFileSystem::Open(path, AppKey(), &files) != FileStatus::kOk) return 1;
  auto store = base::MakeRefCounted<SitePrefStore>(files, base::BindRepeating([](FileStatus) {}));
  if (store->ReadPrefs() != PersistentPrefStore::PREF_READ_ERROR_NONE) return 2;
  apps::PreferredApps entries;
  if (store->GetPreferredApps(&entries) != FileStatus::kOk || entries.size() != 1) return 3;
  apps::PreferredAppsList native(nullptr);
  native.Init(std::move(entries));
  return native.FindPreferredAppForUrl(GURL("https://alpha.test/")) == "native-chooser-private" &&
      native.FindPreferredAppForUrl(GURL("https://beta.test/")) == "native-chooser-private" ? 0 : 4;
}

}  // namespace alloy::site_records
