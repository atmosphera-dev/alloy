// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/process/launch.h"
#include "base/test/multiprocess_test.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "components/prefs/wrap_with_prefix_pref_store.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
SecretKey EmbedKey() { ObjectId bytes; bytes.fill(0x31); return SecretKey(bytes); }
class EmbedObserver final : public PrefStore::Observer {
 public:
  explicit EmbedObserver(SitePrefStore* store) : store_(store) { store_->AddObserver(this); }
  ~EmbedObserver() override { store_->RemoveObserver(this); }
  void OnInitializationCompleted(bool) override {}
  void OnPrefValueChanged(std::string_view key) override {
    if (key == SitePrefStore::kEmbedList && changed) changed.Run();
  }
  base::RepeatingClosure changed;
 private:
  SitePrefStore* const store_;
};
class EmbedListTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir()); path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, EmbedKey(), {}, &files_));
    Open();
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
  }
  void Reopen() {
    store_.reset(); ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, EmbedKey(), &files_)); Open();
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  const GURL outer_{"https://one.alpha.test/"}, b_{"https://beta.test/"},
      c_{"https://gamma.test/"}, d_{"https://delta.test/"};
};

TEST_F(EmbedListTest, CallbackDeletionCannotAcknowledgeAnOldListAsPublished) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  bool deleted = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      if (deleted) return;
      deleted = true; RecordDeletion result;
      ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(outer_, {}, &result));
    });
    EXPECT_NE(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, c_, true));
    EXPECT_TRUE(deleted);
  }
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
  Reopen();
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
}

TEST_F(EmbedListTest, ReentrantReplacementCannotAcknowledgeADifferentList) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  bool replaced = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      if (replaced) return;
      replaced = true;
      ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, d_, true));
    });
    EXPECT_NE(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, c_, true));
    EXPECT_TRUE(replaced);
  }
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, c_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, d_));
  Reopen(); EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, d_));
}

TEST_F(EmbedListTest, PrefixObserverNestedDeletionPreservesPreparationAndPublicationOrder) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  bool deleted = false;
  int events = 0, preparations = 0;
  {
    auto prefixed = base::MakeRefCounted<WrapWithPrefixPrefStore>(store_, "alloy");
    class Observer final : public PrefStore::Observer {
     public:
      Observer(WrapWithPrefixPrefStore* source, int* events, int* preparations)
          : source_(source), events_(events), preparations_(preparations) {
        source_->AddObserver(this);
      }
      ~Observer() override { source_->RemoveObserver(this); }
      void OnPrefValueChanged(std::string_view key) override {
        if (key != "considered_first_party") return;
        ++*events_;
        if (changed) changed.Run();
      }
      base::OnceClosure PreparePrefValueChange(std::string_view key) override {
        if (key != "considered_first_party") return {};
        ++*preparations_;
        return {};
      }
      base::RepeatingClosure changed;
     private:
      WrapWithPrefixPrefStore* const source_;
      int* const events_;
      int* const preparations_;
    } observer(prefixed.get(), &events, &preparations);
    observer.changed = base::BindLambdaForTesting([&] {
      if (deleted) return;
      deleted = true;
      RecordDeletion result;
      EXPECT_EQ(FileStatus::kOk, store_->DeleteSite(outer_, [&] {
        EXPECT_GT(preparations, 0);
        EXPECT_EQ(1, events);
        RecordInfo record;
        EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(
            outer_, url::Origin::Create(outer_), false, &record));
        const base::Value* value = nullptr;
        if (prefixed->GetValue("considered_first_party", &value)) {
          ASSERT_TRUE(value->is_dict());
          EXPECT_FALSE(value->GetDict().contains("alpha.test"));
        }
      }, &result));
    });
    EXPECT_EQ(FileStatus::kBusy, store_->SetConsideredFirstParty(outer_, c_, true));
    EXPECT_TRUE(deleted);
    EXPECT_EQ(2, events);
  }
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
  Reopen();
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
}

TEST_F(EmbedListTest, RemovingAnAbsentEntryDoesNotCreateARecord) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, false));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records)); EXPECT_TRUE(records.empty());
  Reopen(); ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records)); EXPECT_TRUE(records.empty());
}

TEST_F(EmbedListTest, CallbackSeesDurableCandidateBeforeLockRetiresItsAcknowledgement) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  bool observed = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      if (observed) return;
      observed = true;
      RecordInfo record;
      ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(outer_, url::Origin::Create(outer_), false, &record));
      auto shard = base::MakeRefCounted<RecordPrefStore>(files_,
          files_->Capability(record.id, StorePurpose::kSiteSettings), StorePurpose::kSiteSettings,
          "preferences", base::BindRepeating([](FileStatus) {}));
      ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, shard->ReadPrefs());
      const base::Value* values = nullptr;
      ASSERT_TRUE(shard->GetValue("values", &values));
      const auto* embeds = values->GetDict().FindDict(SitePrefStore::kEmbedList);
      ASSERT_TRUE(embeds);
      const auto* entries = embeds->FindList("alpha.test");
      ASSERT_TRUE(entries);
      EXPECT_EQ(2u, entries->size());
      EXPECT_EQ("gamma.test", (*entries)[1].GetString());
      ASSERT_EQ(FileStatus::kOk, files_->Lock());
    });
    EXPECT_NE(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, c_, true));
  }
  EXPECT_TRUE(observed);
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
  EXPECT_EQ(FileStatus::kDenied, store_->SetConsideredFirstParty(outer_, d_, false));
  Reopen();
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, c_));
}

TEST_F(EmbedListTest, PrefixRemovalAndSilentAbaCannotImpersonateThePublishedRevision) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  bool replaced = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      if (replaced) return;
      replaced = true;
      const base::Value* value = nullptr;
      ASSERT_TRUE(store_->GetValue(SitePrefStore::kEmbedList, &value));
      auto restored = value->Clone();
      store_->RemoveValuesByPrefixSilently("alloy.");
      store_->SetValueSilently(SitePrefStore::kEmbedList, std::move(restored), 0);
      ASSERT_EQ(FileStatus::kOk, store_->Flush());
    });
    EXPECT_EQ(FileStatus::kBusy, store_->SetConsideredFirstParty(outer_, c_, true));
  }
  EXPECT_TRUE(replaced);
  Reopen();
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, c_));
}

TEST_F(EmbedListTest, UnrelatedCallbackWriteDoesNotChangeAnotherSettingsRevision) {
  const auto preferred = store_->preferred_version();
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      store_->SetValue("appearance.theme", base::Value("dark"), 0);
    });
    EXPECT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  }
  EXPECT_EQ(preferred, store_->preferred_version());
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  Reopen();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue("appearance.theme", &value));
  EXPECT_EQ("dark", value->GetString());
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, b_));
}

TEST_F(EmbedListTest, FailedPublicationCannotNotifyOrReplayTheRejectedList) {
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  bool notified = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] { notified = true; });
    EXPECT_EQ(FileStatus::kCommitUncertain, store_->SetConsideredFirstParty(outer_, c_, true));
  }
  EXPECT_FALSE(notified);
  EXPECT_FALSE(files_->unlocked());
  files_->SetFaultForTesting({});
  store_.reset();
  EXPECT_EQ(FileStatus::kCommitUncertain, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, EmbedKey(), &files_));
  Open();
  EXPECT_TRUE(store_->IsConsideredFirstParty(outer_, b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, c_));
}

TEST_F(EmbedListTest, CandidateListOwnsCrossSitePermissionPersistenceOnAddAndRemove) {
  constexpr char key[] = "profile.content_settings.exceptions.notifications";
  base::DictValue settings;
  base::DictValue grant;
  grant.Set("setting", 1);
  settings.Set("https://beta.test:443,https://one.alpha.test:443", std::move(grant));
  store_->SetValue(key, base::Value(std::move(settings)), 0);
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, true));
  Reopen();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue(key, &value));
  EXPECT_EQ(1u, value->GetDict().size());
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(outer_, b_, false));
  Reopen();
  EXPECT_FALSE(store_->GetValue(key, &value));
  EXPECT_FALSE(store_->IsConsideredFirstParty(outer_, b_));
}

TEST_F(EmbedListTest, WholeListsCanonicalizeWithoutCreatingEmbeddedRecords) {
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_,
      {b_, GURL("http://two.beta.test:8080/path"), GURL("https://127.0.0.1/"),
       GURL("https://[::1]/"), GURL("http://nomad.home:8090/")}));
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(c_, {d_}));
  Reopen();
  std::vector<std::string> domains;
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(outer_, &domains));
  EXPECT_EQ((std::vector<std::string>{"beta.test", "127.0.0.1", "[::1]", "nomad.home"}), domains);
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(c_, &domains));
  EXPECT_EQ((std::vector<std::string>{"delta.test"}), domains);
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  EXPECT_EQ(2u, records.size());
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {}));
  Reopen();
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(outer_, &domains));
  EXPECT_TRUE(domains.empty());
  EXPECT_TRUE(store_->IsConsideredFirstParty(c_, d_));
}

TEST_F(EmbedListTest, InvalidWholeListPreservesThePriorListWithoutPartialPublication) {
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {b_}));
  bool notified = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] { notified = true; });
    EXPECT_EQ(FileStatus::kInvalid, store_->SetFirstPartyEmbeds(outer_, {c_, GURL("data:text/plain,x")}));
  }
  EXPECT_FALSE(notified);
  EXPECT_EQ(FileStatus::kInvalid, store_->SetFirstPartyEmbeds(GURL("file:///private"), {c_}));
  Reopen();
  std::vector<std::string> domains;
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(outer_, &domains));
  EXPECT_EQ((std::vector<std::string>{"beta.test"}), domains);
  EXPECT_EQ(FileStatus::kInvalid, store_->GetFirstPartyEmbeds(outer_, nullptr));
}

TEST_F(EmbedListTest, WholeListCallbackReplacementOwnsTheOnlyAcknowledgedList) {
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {b_}));
  bool replaced = false;
  {
    EmbedObserver observer(store_.get());
    observer.changed = base::BindLambdaForTesting([&] {
      if (replaced) return;
      replaced = true;
      ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {d_}));
    });
    EXPECT_EQ(FileStatus::kBusy, store_->SetFirstPartyEmbeds(outer_, {b_, c_}));
  }
  EXPECT_TRUE(replaced);
  Reopen();
  std::vector<std::string> domains;
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(outer_, &domains));
  EXPECT_EQ((std::vector<std::string>{"delta.test"}), domains);
}

TEST_F(EmbedListTest, WholeListEnumerationSurvivesAnIndependentConsumerProcess) {
  std::vector<std::string> domains{"old-output"};
  ASSERT_EQ(FileStatus::kOk, store_->GetFirstPartyEmbeds(outer_, &domains));
  EXPECT_TRUE(domains.empty());
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {}));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(outer_, {b_, c_}));
  store_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("embed-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyEmbedListChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  EXPECT_EQ(0, exit);
}
}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyEmbedListChild) {
  base::test::TaskEnvironment tasks;
  std::shared_ptr<RecordFileSystem> files;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("embed-profile");
  if (RecordFileSystem::Open(path, EmbedKey(), &files) != FileStatus::kOk) return 1;
  auto store = base::MakeRefCounted<SitePrefStore>(files, base::BindRepeating([](FileStatus) {}));
  if (store->ReadPrefs() != PersistentPrefStore::PREF_READ_ERROR_NONE) return 2;
  const GURL outer("https://one.alpha.test/");
  std::vector<std::string> domains;
  if (store->GetFirstPartyEmbeds(outer, &domains) != FileStatus::kOk ||
      domains != std::vector<std::string>{"beta.test", "gamma.test"}) return 3;
  store->Invalidate();
  if (store->GetFirstPartyEmbeds(outer, &domains) != FileStatus::kDenied ||
      store->SetFirstPartyEmbeds(outer, {}) != FileStatus::kDenied) return 4;
  return files->Lock() == FileStatus::kOk ? 0 : 5;
}
}  // namespace alloy::site_records
