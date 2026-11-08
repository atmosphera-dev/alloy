// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_pref_store.h"

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
SecretKey PrefTestKey() {
  ObjectId bytes;
  bytes.fill(0x34);
  return SecretKey(bytes);
}

class RecordPrefStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, PrefTestKey(), {}, &files_));
    Resolve();
  }
  void Resolve() {
    const GURL url("https://prefs.example/");
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &record_));
  }
  scoped_refptr<RecordPrefStore> Open() {
    auto store = base::MakeRefCounted<RecordPrefStore>(files_,
        files_->Capability(record_.id, StorePurpose::kSiteSettings),
        StorePurpose::kSiteSettings, "settings", ErrorHandler());
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store->ReadPrefs());
    return store;
  }
  RecordPrefStore::ErrorCallback ErrorHandler() {
    return base::BindRepeating([](std::vector<FileStatus>* errors, FileStatus error) {
      errors->push_back(error);
    }, &errors_);
  }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  RecordInfo record_;
  std::vector<FileStatus> errors_;
};

TEST_F(RecordPrefStoreTest, NativePrefServiceMutableUpdatesColdReopen) {
  auto store = Open();
  auto registry = base::MakeRefCounted<PrefRegistrySimple>();
  registry->RegisterDictionaryPref("site.preferences");
  registry->RegisterDoublePref("site.zoom", 0.0);
  PrefServiceFactory factory;
  factory.set_user_prefs(store);
  auto prefs = factory.Create(registry);
  {
    ScopedDictPrefUpdate update(prefs.get(), "site.preferences");
    update->Set("https://prefs.example", 1);
  }
  prefs->SetDouble("site.zoom", 1.75);
  bool immediate = false, replied = false;
  store->CommitPendingWrite(base::BindOnce([](bool* flag) { *flag = true; }, &replied),
      base::BindOnce([](bool* flag) { *flag = true; }, &immediate));
  EXPECT_TRUE(immediate);
  EXPECT_FALSE(replied);
  tasks_.RunUntilIdle();
  EXPECT_TRUE(replied);
  ASSERT_EQ(FileStatus::kOk, store->last_status());
  prefs.reset();
  store.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, PrefTestKey(), &files_));
  Resolve();
  store = Open();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store->GetValue("site.preferences", &value));
  EXPECT_EQ(1, value->GetDict().FindInt("https://prefs.example"));
  ASSERT_TRUE(store->GetValue("site.zoom", &value));
  EXPECT_EQ(1.75, value->GetDouble());
}

TEST_F(RecordPrefStoreTest, SiteDeletionRevokesOldProjectionAndDoesNotResurrect) {
  auto store = Open();
  store->SetValue("permission", base::Value(1), 0);
  ASSERT_EQ(FileStatus::kOk, store->Flush());
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, files_->DeleteRecord(record_.id,
      [&] { store->Invalidate(); }, &result));
  const base::Value* value = nullptr;
  EXPECT_FALSE(store->GetValue("permission", &value));
  EXPECT_TRUE(store->GetValues().empty());
  store->SetValue("permission", base::Value(2), 0);
  EXPECT_EQ(FileStatus::kDenied, store->Flush());
  Resolve();
  auto fresh = Open();
  EXPECT_FALSE(fresh->GetValue("permission", &value));
}

TEST_F(RecordPrefStoreTest, LockDeniesRetainedReadsAndMutableWrites) {
  auto store = Open();
  store->SetValue("permission", base::Value(1), 0);
  ASSERT_EQ(FileStatus::kOk, store->Flush());
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  const base::Value* value = nullptr;
  base::Value* mutable_value = nullptr;
  EXPECT_FALSE(store->GetValue("permission", &value));
  EXPECT_FALSE(store->GetMutableValue("permission", &mutable_value));
  EXPECT_TRUE(store->ReadOnly());
  EXPECT_EQ(FileStatus::kDenied, store->Flush());
}

TEST_F(RecordPrefStoreTest, AuthenticatedInvalidJsonIsNotRepairedOrOverwritten) {
  const auto cap = files_->Capability(record_.id, StorePurpose::kSiteSettings);
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap, StorePurpose::kSiteSettings,
      "settings", RecordOpen::kCreate, &file));
  constexpr std::string_view invalid = "invalid JSON";
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(invalid)));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  auto store = base::MakeRefCounted<RecordPrefStore>(files_, cap,
      StorePurpose::kSiteSettings, "settings", ErrorHandler());
  EXPECT_NE(PersistentPrefStore::PREF_READ_ERROR_NONE, store->ReadPrefs());
  EXPECT_EQ(FileStatus::kIntegrity, store->last_status());
  store->SetValue("permission", base::Value(1), 0);
  EXPECT_EQ(FileStatus::kIntegrity, store->Flush());
  RecordFileInfo info;
  ASSERT_EQ(FileStatus::kOk, files_->Stat(cap, StorePurpose::kSiteSettings, "settings", &info));
  EXPECT_EQ(invalid.size(), info.length);
}

TEST_F(RecordPrefStoreTest, ProfilePrivateSettingsCannotUseSitePurpose) {
  auto cap = files_->ProfileCapability(StorePurpose::kProfileSettings);
  auto store = base::MakeRefCounted<RecordPrefStore>(files_, cap,
      StorePurpose::kSiteSettings, "settings", ErrorHandler());
  EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_ACCESS_DENIED, store->ReadPrefs());
  store->SetValue("permission", base::Value(1), 0);
  EXPECT_EQ(FileStatus::kDenied, store->Flush());
}

TEST_F(RecordPrefStoreTest, FailedStagingLeavesPreviousPreferencesAuthoritative) {
  auto store = Open();
  store->SetValue("permission", base::Value(1), 0);
  ASSERT_EQ(FileStatus::kOk, store->Flush());
  store->SetValue("permission", base::Value(2), 0);
  files_->SetFaultForTesting([](PublishFence, const base::FilePath&) {
    return FileStatus::kIo;
  });
  EXPECT_EQ(FileStatus::kIo, store->Flush());
  files_->SetFaultForTesting({});
  ASSERT_EQ(1u, errors_.size());
  EXPECT_EQ(FileStatus::kIo, errors_[0]);
  // A failed adapter is sticky. It cannot repair or retry over the old state.
  EXPECT_EQ(FileStatus::kIo, store->Flush());
  EXPECT_EQ(1u, errors_.size());
  store.reset();
  store = Open();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store->GetValue("permission", &value));
  EXPECT_EQ(1, value->GetInt());
}

TEST_F(RecordPrefStoreTest, DeletingOneShardPreservesOtherSiteAndProfileSettings) {
  auto first = Open();
  first->SetValue("permission", base::Value(1), 0);
  ASSERT_EQ(FileStatus::kOk, first->Flush());
  const GURL second_url("https://other.example/");
  RecordInfo other;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(second_url,
      url::Origin::Create(second_url), true, &other));
  auto second = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->Capability(other.id, StorePurpose::kSiteSettings),
      StorePurpose::kSiteSettings, "settings", ErrorHandler());
  auto global = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->ProfileCapability(StorePurpose::kProfileSettings),
      StorePurpose::kProfileSettings, "preferences", ErrorHandler());
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, second->ReadPrefs());
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, global->ReadPrefs());
  second->SetValue("permission", base::Value(2), 0);
  global->SetValue("appearance", base::Value("dark"), 0);
  ASSERT_EQ(FileStatus::kOk, second->Flush());
  ASSERT_EQ(FileStatus::kOk, global->Flush());
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, files_->DeleteRecord(record_.id,
      [&] { first->Invalidate(); }, &deletion));
  first.reset();
  second.reset();
  global.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, PrefTestKey(), &files_));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  ASSERT_EQ(1u, records.size());
  EXPECT_EQ("other.example", records[0].domain);
  second = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->Capability(records[0].id, StorePurpose::kSiteSettings),
      StorePurpose::kSiteSettings, "settings", ErrorHandler());
  global = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->ProfileCapability(StorePurpose::kProfileSettings),
      StorePurpose::kProfileSettings, "preferences", ErrorHandler());
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, second->ReadPrefs());
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, global->ReadPrefs());
  const base::Value* value = nullptr;
  ASSERT_TRUE(second->GetValue("permission", &value));
  EXPECT_EQ(2, value->GetInt());
  ASSERT_TRUE(global->GetValue("appearance", &value));
  EXPECT_EQ("dark", value->GetString());
}

TEST_F(RecordPrefStoreTest, CiphertextTamperReachesOwnerWithoutRepair) {
  auto store = Open();
  store->SetValue("permission", base::Value(1), 0);
  ASSERT_EQ(FileStatus::kOk, store->Flush());
  store.reset();
  base::FileEnumerator objects(path_.AppendASCII(ObjectName(record_.directory)),
      false, base::FileEnumerator::FILES);
  size_t count = 0;
  for (auto path = objects.Next(); !path.empty(); path = objects.Next()) {
    auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes && !bytes->empty());
    base::span(*bytes)[0] ^= 1;
    ASSERT_TRUE(base::WriteFile(path, *bytes));
    ++count;
  }
  ASSERT_GT(count, 0u);
  store = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->Capability(record_.id, StorePurpose::kSiteSettings),
      StorePurpose::kSiteSettings, "settings", ErrorHandler());
  EXPECT_NE(PersistentPrefStore::PREF_READ_ERROR_NONE, store->ReadPrefs());
  ASSERT_EQ(1u, errors_.size());
  EXPECT_EQ(FileStatus::kIntegrity, errors_[0]);
  EXPECT_EQ(FileStatus::kIntegrity, store->Flush());
  EXPECT_TRUE(store->GetValues().empty());
  EXPECT_FALSE(files_->unlocked());
}

TEST_F(RecordPrefStoreTest, DelayedWriteFailureReachesOwner) {
  auto store = Open();
  store->SetValue("permission", base::Value(1), 0);
  files_->SetFaultForTesting([](PublishFence, const base::FilePath&) {
    return FileStatus::kIo;
  });
  tasks_.FastForwardBy(base::Seconds(1));
  ASSERT_EQ(1u, errors_.size());
  EXPECT_EQ(FileStatus::kIo, errors_[0]);
  EXPECT_EQ(FileStatus::kIo, store->last_status());
  files_->SetFaultForTesting({});
}

TEST_F(RecordPrefStoreTest, ErrorCallbackMayReenterAndReleaseOwnerReference) {
  scoped_refptr<RecordPrefStore> store;
  store = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->Capability(record_.id, StorePurpose::kSiteSettings),
      StorePurpose::kSiteSettings, "settings",
      base::BindRepeating([](scoped_refptr<RecordPrefStore>* owner, FileStatus error) {
        EXPECT_EQ(FileStatus::kIo, error);
        EXPECT_TRUE((*owner)->ReadOnly());
        owner->reset();
      }, &store));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store->ReadPrefs());
  store->SetValue("permission", base::Value(1), 0);
  files_->SetFaultForTesting([](PublishFence, const base::FilePath&) {
    return FileStatus::kIo;
  });
  EXPECT_EQ(FileStatus::kIo, store->Flush());
  EXPECT_FALSE(store);
  files_->SetFaultForTesting({});
}

TEST_F(RecordPrefStoreTest, DestructionLandsPendingWrite) {
  auto store = Open();
  store->SetValueSilently("pending.setting", base::Value("retained"), 0);
  store.reset();
  store = Open();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store->GetValue("pending.setting", &value));
  EXPECT_EQ("retained", value->GetString());
}

TEST_F(RecordPrefStoreTest, ReadErrorReentryPreservesIntegrityFailure) {
  const auto cap = files_->Capability(record_.id, StorePurpose::kSiteSettings);
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(cap, StorePurpose::kSiteSettings,
      "settings", RecordOpen::kCreate, &file));
  constexpr std::string_view invalid = "invalid JSON";
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(invalid)));
  ASSERT_EQ(FileStatus::kOk, file->Close());
  scoped_refptr<RecordPrefStore> store;
  int callbacks = 0;
  store = base::MakeRefCounted<RecordPrefStore>(files_, cap,
      StorePurpose::kSiteSettings, "settings",
      base::BindRepeating([](scoped_refptr<RecordPrefStore>* owner,
                            int* calls, FileStatus error) {
        ++*calls;
        EXPECT_EQ(FileStatus::kIntegrity, error);
        EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_FILE_OTHER,
                  (*owner)->ReadPrefs());
      }, &store, &callbacks));
  EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_FILE_OTHER, store->ReadPrefs());
  EXPECT_EQ(FileStatus::kIntegrity, store->last_status());
  EXPECT_EQ(1, callbacks);
}

}  // namespace
}  // namespace alloy::site_records
