// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread_restrictions.h"
#include "base/synchronization/waitable_event.h"

#include <atomic>
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
SecretKey AsyncKey() { ObjectId key; key.fill(0x27); return SecretKey(key); }
class SitePrefStoreAsyncTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir()); path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, AsyncKey(), {}, &files_));
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
  }
  void Initialize() {
    bool called = false;
    { base::ScopedDisallowBlocking restricted;
      store_->InitializeAsync(base::BindLambdaForTesting([&](FileStatus status) {
        EXPECT_EQ(FileStatus::kOk, status); called = true;
      }));
      tasks_.RunUntilIdle();
    }
    ASSERT_TRUE(called);
  }
  void ColdRead(const GURL& outer, const GURL& embedded, bool expected) {
    store_.reset(); tasks_.RunUntilIdle();
    ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AsyncKey(), &files_));
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    Initialize(); EXPECT_EQ(expected, store_->IsConsideredFirstParty(outer, embedded));
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  const GURL a_{"https://alpha.test/"}, b_{"https://beta.test/"}, c_{"https://gamma.test/"};
};

TEST_F(SitePrefStoreAsyncTest, RestrictedOwnerInitializesPublishesAndFlushesOnTheIoSequence) {
  Initialize();
  int replies = 0;
  { base::ScopedDisallowBlocking restricted;
    store_->SetFirstPartyEmbedsAsync(a_, {b_}, base::BindLambdaForTesting([&](FileStatus status) {
      EXPECT_EQ(FileStatus::kOk, status); ++replies;
    }));
    store_->SetValue("appearance.theme", base::Value("dark"), 0);
    store_->CommitPendingWrite(base::BindLambdaForTesting([&] { ++replies; }));
    store_->GetFirstPartyEmbedsAsync(a_, base::BindLambdaForTesting(
        [&](FileStatus status, std::vector<std::string> entries) {
          EXPECT_EQ(FileStatus::kOk, status);
          EXPECT_EQ((std::vector<std::string>{"beta.test"}), entries); ++replies;
        }));
    EXPECT_EQ(0, replies);
    tasks_.RunUntilIdle();
  }
  EXPECT_EQ(3, replies);
  ColdRead(a_, b_, true);
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue("appearance.theme", &value)); EXPECT_EQ("dark", value->GetString());
}

TEST_F(SitePrefStoreAsyncTest, NativePendingReadReportsIncompleteWithoutBlockingTheOwner) {
  { base::ScopedDisallowBlocking restricted;
    store_->ReadPrefsAsync(nullptr);
    EXPECT_TRUE(store_->HasReadErrorDelegate());
    EXPECT_FALSE(store_->IsInitializationComplete());
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE, store_->ReadPrefs());
    tasks_.RunUntilIdle();
    EXPECT_TRUE(store_->IsInitializationComplete());
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->GetReadError());
  }
}

TEST_F(SitePrefStoreAsyncTest, NativeShutdownDoneDoesNotDependOnPumpingTheOwner) {
  Initialize();
  auto owner = base::SequencedTaskRunner::GetCurrentDefault();
  base::WaitableEvent done;
  std::atomic<bool> on_worker{false};
  bool replied = false;
  { base::ScopedDisallowBlocking restricted;
    store_->SetValue("appearance.theme", base::Value("dark"), 0);
    store_->CommitPendingWrite(base::BindLambdaForTesting([&] { replied = true; }),
        base::BindLambdaForTesting([&] {
          on_worker = !owner->RunsTasksInCurrentSequence(); done.Signal();
        }));
  }
  // Deliberately do not pump UI: this is PersistentPrefStore's shutdown contract.
  ASSERT_TRUE(done.TimedWait(base::Seconds(5)));
  EXPECT_TRUE(on_worker); EXPECT_FALSE(replied);
  tasks_.RunUntilIdle(); EXPECT_TRUE(replied);
  ColdRead(a_, b_, false);
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue("appearance.theme", &value)); EXPECT_EQ("dark", value->GetString());
}

TEST_F(SitePrefStoreAsyncTest, CompletionAfterActualPublicationAndLockIsCancelled) {
  Initialize();
  // Allocate and acknowledge the owner record before arming the root fence.
  // Otherwise ResolveRecord's earlier catalogue publication can trigger lock
  // before the settings transaction being tested has committed.
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  auto owner = base::SequencedTaskRunner::GetCurrentDefault();
  bool locked = false, replied = false;
  files_->SetFaultForTesting([&, owner](PublishFence fence, const base::FilePath&) {
    if (fence == PublishFence::kRootDirectorySynced) {
      owner->PostTask(FROM_HERE, base::BindLambdaForTesting([&] {
        EXPECT_EQ(FileStatus::kOk, files_->Lock()); locked = true;
      }));
    }
    return FileStatus::kOk;
  });
  store_->SetFirstPartyEmbedsAsync(a_, {c_}, base::BindLambdaForTesting([&](FileStatus status) {
    EXPECT_TRUE(locked); EXPECT_EQ(FileStatus::kCancelled, status); replied = true;
  }));
  tasks_.RunUntilIdle(); EXPECT_TRUE(replied);
  files_->SetFaultForTesting({});
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
  ColdRead(a_, c_, true);
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
}

TEST_F(SitePrefStoreAsyncTest, DeleteRetiresTheOldQueuedAdmissionBeforeFreshOperations) {
  Initialize();
  ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  bool teardown = false, deleted = false, stale = false, fresh = false;
  { base::ScopedDisallowBlocking restricted;
    store_->DeleteSiteAsync(a_, base::BindLambdaForTesting([&] {
      teardown = true;
      EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
      store_->SetFirstPartyEmbedsAsync(c_, {b_}, base::BindLambdaForTesting([&](FileStatus status) {
        EXPECT_EQ(FileStatus::kOk, status); fresh = true;
      }));
    }), base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
      EXPECT_TRUE(teardown); EXPECT_EQ(FileStatus::kOk, status); deleted = true;
    }));
    store_->SetFirstPartyEmbedsAsync(a_, {c_}, base::BindLambdaForTesting([&](FileStatus status) {
      EXPECT_EQ(FileStatus::kCancelled, status); stale = true;
    }));
    tasks_.RunUntilIdle();
  }
  EXPECT_TRUE(deleted); EXPECT_TRUE(stale); EXPECT_TRUE(fresh);
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, c_));
  ColdRead(c_, b_, true);
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, c_));
}

TEST_F(SitePrefStoreAsyncTest, PendingInitializationAndEveryQueuedWriteRefuseALockedGeneration) {
  int cancelled = 0;
  store_->InitializeAsync(base::BindLambdaForTesting([&](FileStatus status) {
    EXPECT_EQ(FileStatus::kCancelled, status); ++cancelled;
  }));
  store_->SetFirstPartyEmbedsAsync(a_, {b_}, base::BindLambdaForTesting([&](FileStatus status) {
    EXPECT_EQ(FileStatus::kCancelled, status); ++cancelled;
  }));
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  { base::ScopedDisallowBlocking restricted; tasks_.RunUntilIdle(); }
  EXPECT_EQ(2, cancelled);
  ColdRead(a_, b_, false);
}

TEST_F(SitePrefStoreAsyncTest, OldReplyAfterFreshUnlockCannotAcquireTheReplacementProjection) {
  Initialize(); ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  auto old = store_;
  bool cancelled = false;
  old->GetFirstPartyEmbedsAsync(a_, base::BindLambdaForTesting(
      [&](FileStatus status, std::vector<std::string> entries) {
        EXPECT_EQ(FileStatus::kCancelled, status); EXPECT_TRUE(entries.empty()); cancelled = true;
      }));
  ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AsyncKey(), &files_));
  store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
  Initialize();
  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(store_->IsConsideredFirstParty(a_, b_));
  EXPECT_FALSE(old->IsConsideredFirstParty(a_, b_));
}

TEST_F(SitePrefStoreAsyncTest, ShutdownCheckpointDuringRetirementPrunesOldReferencesAndKeepsOtherEdits) {
  Initialize(); ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  bool deleted = false;
  base::WaitableEvent done;
  { base::ScopedDisallowBlocking restricted;
    base::DictValue pending;
    base::ListValue first; first.Append("beta.test");
    base::ListValue other; other.Append("beta.test");
    pending.Set("alpha.test", std::move(first)); pending.Set("gamma.test", std::move(other));
    store_->SetValue(SitePrefStore::kEmbedList, base::Value(std::move(pending)), 0);
    store_->DeleteSiteAsync(a_, base::BindLambdaForTesting([&] {
      EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
      EXPECT_TRUE(store_->IsConsideredFirstParty(c_, b_));
    }), base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
      EXPECT_EQ(FileStatus::kOk, status); deleted = true;
    }));
    store_->CommitPendingWrite({}, base::BindLambdaForTesting([&] { done.Signal(); }));
  }
  ASSERT_TRUE(done.TimedWait(base::Seconds(5)));  // No owner pumping before this fence.
  RecordInfo record;
  EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &record));
  tasks_.RunUntilIdle(); EXPECT_TRUE(deleted);
  ColdRead(c_, b_, true);
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
}

TEST_F(SitePrefStoreAsyncTest, DelayedDeleteFinishCannotUnlinkAFreshlyUnlockedRecordGeneration) {
  Initialize(); ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  base::WaitableEvent retired;
  bool cancelled = false, teardown = false;
  store_->DeleteSiteAsync(a_, base::BindLambdaForTesting([&] { teardown = true; }),
      base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
        EXPECT_EQ(FileStatus::kCancelled, status); cancelled = true;
      }));
  store_->CommitPendingWrite({}, base::BindLambdaForTesting([&] { retired.Signal(); }));
  ASSERT_TRUE(retired.TimedWait(base::Seconds(5)));  // Retirement is durable; UI is still stopped.
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  std::shared_ptr<RecordFileSystem> fresh;
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AsyncKey(), &fresh));
  RecordInfo record;
  ASSERT_EQ(FileStatus::kOk, fresh->ResolveRecord(a_, url::Origin::Create(a_), true, &record));
  constexpr std::string_view marker = "fresh-generation-metadata";
  ASSERT_EQ(FileStatus::kOk, fresh->SetRecordMetadata(record.id, base::as_byte_span(marker)));
  tasks_.RunUntilIdle(); EXPECT_TRUE(teardown); EXPECT_TRUE(cancelled);
  // Cold-open the new coordinator: an in-memory catalogue alone cannot prove
  // that the old finish did not unlink its replacement's physical directory.
  ASSERT_EQ(FileStatus::kOk, fresh->Lock()); fresh.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, AsyncKey(), &fresh));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, fresh->ListRecords(&records)); ASSERT_EQ(1u, records.size());
  EXPECT_EQ(record.generation, records[0].generation);
  EXPECT_EQ((std::vector<uint8_t>(marker.begin(), marker.end())), records[0].metadata);
}

TEST_F(SitePrefStoreAsyncTest, AsyncNativeBarrierWaitsAfterDurableKeyRetirement) {
  Initialize(); ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  RecordInfo original;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &original));
  const auto old = files_->Capability(original.id, StorePurpose::kSiteSettings);
  ASSERT_TRUE(old.active());
  SitePrefStore::Completion resume;
  bool deleted = false, fresh = false;
  store_->DeleteSiteWithTeardownAsync(a_, base::BindLambdaForTesting(
      [&](SitePrefStore::Completion next) {
        EXPECT_FALSE(old.active());
        EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
        resume = std::move(next);
        store_->SetFirstPartyEmbedsAsync(c_, {b_}, base::BindLambdaForTesting([&](FileStatus status) {
          EXPECT_EQ(FileStatus::kOk, status); fresh = true;
        }));
      }), base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
        EXPECT_EQ(FileStatus::kOk, status); deleted = true;
      }));
  tasks_.RunUntilIdle();
  ASSERT_TRUE(resume); EXPECT_FALSE(deleted); EXPECT_FALSE(fresh);
  RecordInfo refused;
  EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, url::Origin::Create(a_), true, &refused));
  std::move(resume).Run(FileStatus::kOk);
  tasks_.RunUntilIdle(); EXPECT_TRUE(deleted); EXPECT_TRUE(fresh);
  ColdRead(c_, b_, true); EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
}

TEST_F(SitePrefStoreAsyncTest, FailedNativeBarrierNeverAcknowledgesSuccessfulDeletion) {
  Initialize(); ASSERT_EQ(FileStatus::kOk, store_->SetFirstPartyEmbeds(a_, {b_}));
  RecordInfo original;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &original));
  const auto old = files_->Capability(original.id, StorePurpose::kSiteSettings);
  bool completed = false;
  store_->DeleteSiteWithTeardownAsync(a_, base::BindLambdaForTesting(
      [&](SitePrefStore::Completion done) {
        EXPECT_FALSE(old.active()); std::move(done).Run(FileStatus::kIo);
      }), base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
        EXPECT_EQ(FileStatus::kIo, status); completed = true;
      }));
  tasks_.RunUntilIdle(); EXPECT_TRUE(completed); EXPECT_FALSE(old.active());
  ColdRead(a_, b_, false);
}
}  // namespace
}  // namespace alloy::site_records
