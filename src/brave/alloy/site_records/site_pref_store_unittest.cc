// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/files/scoped_temp_dir.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr char kZoom[] = "partition.per_host_zoom_levels";
constexpr char kPermission[] = "profile.content_settings.exceptions.notifications";
SecretKey SettingsKey() {
  ObjectId bytes;
  bytes.fill(0x72);
  return SecretKey(bytes);
}

class SitePrefStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, SettingsKey(), {}, &files_));
    Open();
  }
  void TearDown() override { CloseNative(); }
  void CloseNative() {
    if (map_) { map_->ShutdownOnUIThread(); map_.reset(); }
    prefs_.reset();
    store_.reset();
    tasks_.RunUntilIdle();  // Quiesce the asynchronous native writer before raw fixture access.
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating(
        [](std::vector<FileStatus>* errors, FileStatus status) { errors->push_back(status); }, &errors_));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    HostContentSettingsMap::RegisterProfilePrefs(registry.get());
    registry->RegisterDictionaryPref(kZoom);
    registry->RegisterDictionaryPref(SitePrefStore::kEmbedList);
    registry->RegisterStringPref("appearance.theme", "system");
    PrefServiceFactory factory;
    factory.set_user_prefs(store_);
    prefs_ = factory.Create(registry);
    map_ = base::MakeRefCounted<HostContentSettingsMap>(prefs_.get(), false, false, false, false);
  }
  void Reopen() {
    CloseNative();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, SettingsKey(), &files_));
    Open();
  }
  void Permission(const GURL& site, ContentSetting value) {
    map_->SetContentSettingDefaultScope(site, site, ContentSettingsType::NOTIFICATIONS, value);
  }
  ContentSetting Permission(const GURL& site) {
    return map_->GetContentSetting(site, site, ContentSettingsType::NOTIFICATIONS);
  }
  void Zoom(const std::string& host, double level) {
    ScopedDictPrefUpdate update(prefs_.get(), kZoom);
    auto* partition = update->FindDict("x");
    if (!partition) partition = &update->Set("x", base::DictValue())->GetDict();
    base::DictValue zoom;
    zoom.Set("zoom_level", level);
    partition->Set(host, std::move(zoom));
  }
  base::DictValue Shard(const GURL& site) {
    RecordInfo record;
    if (files_->ResolveRecord(site, url::Origin::Create(site), false, &record) != FileStatus::kOk) return {};
    auto shard = base::MakeRefCounted<RecordPrefStore>(files_,
        files_->Capability(record.id, StorePurpose::kSiteSettings), StorePurpose::kSiteSettings,
        "preferences", base::BindRepeating([](FileStatus) {}));
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, shard->ReadPrefs());
    const base::Value* value = nullptr;
    if (!shard->GetValue("values", &value)) return {};
    return value->GetDict().Clone();
  }
  std::string Text(const base::DictValue& value) {
    std::string text;
    EXPECT_TRUE(base::JSONWriter::Write(value, &text));
    return text;
  }
  void Populate() {
    Permission(a_, CONTENT_SETTING_ALLOW);
    Permission(b_, CONTENT_SETTING_BLOCK);
    Zoom("one.alpha.test", 1.25);
    Zoom("beta.test", 2.5);
    ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(a_, b_, true));
    ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(a_, c_, true));
    prefs_->SetString("appearance.theme", "dark");
    ASSERT_EQ(FileStatus::kOk, store_->Flush());
  }
  void CiphertextScan() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-F", "-i", "-l",
                            "-e", "alpha.test", "-e", "beta.test", "-e", "gamma.test"}) {
      command.AppendArg(arg);
    }
    command.AppendArgPath(path_);
    std::string output;
    int exit = -1;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  const GURL a_{"https://one.alpha.test/"}, b_{"https://beta.test/"}, c_{"https://gamma.test/"};
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  std::unique_ptr<PrefService> prefs_;
  scoped_refptr<HostContentSettingsMap> map_;
  std::vector<FileStatus> errors_;
};

TEST_F(SitePrefStoreTest, NativePermissionsZoomAndEmbedListColdReopenInOwningShards) {
  Populate();
  const auto first = Shard(a_), second = Shard(b_);
  EXPECT_NE(std::string::npos, Text(first).find("one.alpha.test"));
  // Beta in the first shard is the explicit outer-site embed list, not a
  // second copy of Beta's native permission or zoom dictionary.
  ASSERT_TRUE(first.FindDict(kPermission));
  EXPECT_EQ(1u, first.FindDict(kPermission)->size());
  EXPECT_EQ(1u, second.FindDict(kPermission)->size());
  EXPECT_EQ(nullptr, first.Find("appearance.theme"));
  EXPECT_EQ(nullptr, second.Find(SitePrefStore::kEmbedList));
  Reopen();
  EXPECT_EQ(CONTENT_SETTING_ALLOW, Permission(a_));
  EXPECT_EQ(CONTENT_SETTING_BLOCK, Permission(b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(a_, b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(a_, c_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(b_, a_));
  const auto* zooms = prefs_->GetDict(kZoom).FindDict("x");
  ASSERT_TRUE(zooms);
  EXPECT_EQ(1.25, zooms->FindDict("one.alpha.test")->FindDouble("zoom_level"));
  EXPECT_EQ(2.5, zooms->FindDict("beta.test")->FindDouble("zoom_level"));
  EXPECT_EQ("dark", prefs_->GetString("appearance.theme"));
}

TEST_F(SitePrefStoreTest, DeletionRevokesNativePermissionAndGeneratedReferencesBeforeCallback) {
  Populate();
  bool callback = false;
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(b_, [&] {
    callback = true;
    EXPECT_EQ(CONTENT_SETTING_ASK, Permission(b_));
    EXPECT_EQ(CONTENT_SETTING_ALLOW, Permission(a_));
    EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
    EXPECT_TRUE(store_->IsConsideredFirstParty(a_, c_));
    EXPECT_EQ(std::string::npos, Text(Shard(a_)).find("beta.test"));
    RecordInfo record;
    // R13 keeps admission closed throughout live teardown, even for lookup.
    EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(b_, url::Origin::Create(b_), false, &record));
    std::vector<RecordInfo> records;
    ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
    for (const auto& live : records) EXPECT_NE("beta.test", live.domain);
  }, &deletion));
  EXPECT_TRUE(callback);
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(b_, url::Origin::Create(b_), false, &absent));
  Reopen();
  EXPECT_EQ(CONTENT_SETTING_ASK, Permission(b_));
  EXPECT_EQ(CONTENT_SETTING_ALLOW, Permission(a_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(a_, c_));
  EXPECT_EQ("dark", prefs_->GetString("appearance.theme"));
  const auto* zooms = prefs_->GetDict(kZoom).FindDict("x");
  ASSERT_TRUE(zooms);
  EXPECT_EQ(nullptr, zooms->Find("beta.test"));
  ASSERT_TRUE(zooms->Find("one.alpha.test"));
}

TEST_F(SitePrefStoreTest, UnlistedCrossSitePermissionIsVolatileAndNeverCreatesAnEmbeddedRecord) {
  base::DictValue settings;
  base::DictValue decision;
  decision.Set("setting", static_cast<int>(CONTENT_SETTING_ALLOW));
  settings.Set("https://beta.test:443,https://one.alpha.test:443", std::move(decision));
  store_->SetValue(kPermission, base::Value(std::move(settings)), 0);
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  EXPECT_TRUE(records.empty());
  Reopen();
  EXPECT_TRUE(prefs_->GetDict(kPermission).empty());
}

TEST_F(SitePrefStoreTest, ListedPatternPairDependencyIsRemovedFromAnotherOwnersRecord) {
  Populate();
  ASSERT_EQ(FileStatus::kOk, store_->SetConsideredFirstParty(b_, a_, true));
  const char* name = "profile.content_settings.exceptions.storage_access";
  base::DictValue settings;
  base::DictValue decision;
  decision.Set("setting", 1);
  settings.Set("https://one.alpha.test:443,https://beta.test:443", std::move(decision));
  store_->SetValue(name, base::Value(std::move(settings)), 0);
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  ASSERT_TRUE(Shard(a_).FindDict(name));
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(b_, {}, &deletion));
  EXPECT_EQ(nullptr, Shard(a_).Find(name));
  EXPECT_EQ(std::string::npos, Text(Shard(a_)).find("beta.test"));
  Reopen();
  const base::Value* value = nullptr;
  // The native provider initializes every registered dictionary when opened.
  // Its empty projection is not a persisted grant or generated site reference.
  ASSERT_TRUE(store_->GetValue(name, &value));
  ASSERT_TRUE(value->is_dict());
  EXPECT_TRUE(value->GetDict().empty());
}

TEST_F(SitePrefStoreTest, FailedRootPublicationCannotAcknowledgePartialDependencyCleanup) {
  Populate();
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootWritten ? FileStatus::kIo : FileStatus::kOk;
  });
  bool callback = false;
  RecordDeletion deletion;
  EXPECT_EQ(FileStatus::kCommitUncertain, store_->DeleteSite(b_, [&] { callback = true; }, &deletion));
  EXPECT_FALSE(callback);
  EXPECT_FALSE(files_->unlocked());
  files_->SetFaultForTesting({});
  CloseNative();
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, SettingsKey(), &files_));
  Open();
  EXPECT_EQ(CONTENT_SETTING_BLOCK, Permission(b_));
  EXPECT_TRUE(store_->IsConsideredFirstParty(a_, b_));
}

TEST_F(SitePrefStoreTest, PostRenameFailureReopensOnlyTheCleanDeletedGeneration) {
  Populate();
  files_->SetFaultForTesting([](PublishFence fence, const base::FilePath&) {
    return fence == PublishFence::kRootRenamed ? FileStatus::kIo : FileStatus::kOk;
  });
  RecordDeletion deletion;
  EXPECT_EQ(FileStatus::kCommitUncertain, store_->DeleteSite(b_, {}, &deletion));
  EXPECT_FALSE(files_->unlocked());
  files_->SetFaultForTesting({});
  CloseNative();
  files_.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, SettingsKey(), &files_));
  Open();
  EXPECT_EQ(CONTENT_SETTING_ASK, Permission(b_));
  EXPECT_FALSE(store_->IsConsideredFirstParty(a_, b_));
  EXPECT_EQ(std::string::npos, Text(Shard(a_)).find("beta.test"));
}

TEST_F(SitePrefStoreTest, InvalidBatchCannotPublishAnEarlierValidReplacement) {
  Populate();
  RecordInfo record;
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &record));
  const auto cap = files_->Capability(record.id, StorePurpose::kSiteSettings);
  constexpr std::string_view bytes = "should-not-publish";
  const SettingsReplacement replacements[] = {
      {cap, StorePurpose::kSiteSettings, "uncommitted", base::as_byte_span(bytes)},
      {cap, StorePurpose::kCookies, "wrong-purpose", base::as_byte_span(bytes)}};
  EXPECT_EQ(FileStatus::kInvalid, files_->ReplaceSettings(replacements));
  ASSERT_EQ(FileStatus::kOk, files_->Sync());
  RecordFileInfo info;
  EXPECT_EQ(FileStatus::kNotFound, files_->Stat(cap, StorePurpose::kSiteSettings, "uncommitted", &info));
  EXPECT_EQ(CONTENT_SETTING_ALLOW, Permission(a_));
}

TEST_F(SitePrefStoreTest, SeparateProcessReadsNativeSettingsAndNoPlaintextDomains) {
  Populate();
  CiphertextScan();
  CloseNative();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloyNativeSettingsChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  ASSERT_EQ(0, exit);
  CiphertextScan();
}

TEST_F(SitePrefStoreTest, AuthenticatedWrongOwnerCannotBecomeProfileConfiguration) {
  CloseNative();
  auto raw = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->ProfileCapability(StorePurpose::kProfileSettings), StorePurpose::kProfileSettings,
      "preferences", base::BindRepeating([](FileStatus) {}));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, raw->ReadPrefs());
  base::DictValue decision;
  decision.Set("setting", 1);
  base::DictValue grants;
  grants.Set("https://beta.test:443,*", std::move(decision));
  base::DictValue values;
  values.Set(kPermission, std::move(grants));
  raw->SetValue("values", base::Value(std::move(values)), 0);
  ASSERT_EQ(FileStatus::kOk, raw->Flush());
  raw.reset();
  auto rejected = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
  EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_FILE_OTHER, rejected->ReadPrefs());
  EXPECT_EQ(FileStatus::kIntegrity, rejected->last_status());
  EXPECT_TRUE(rejected->GetValues().empty());
  EXPECT_EQ(FileStatus::kIntegrity, rejected->Flush());
  raw = base::MakeRefCounted<RecordPrefStore>(files_,
      files_->ProfileCapability(StorePurpose::kProfileSettings), StorePurpose::kProfileSettings,
      "preferences", base::BindRepeating([](FileStatus) {}));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, raw->ReadPrefs());
  const base::Value* original = nullptr;
  ASSERT_TRUE(raw->GetValue("values", &original));
  EXPECT_TRUE(original->GetDict().FindDict(kPermission));
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyNativeSettingsChild) {
  base::test::TaskEnvironment tasks;
  std::shared_ptr<RecordFileSystem> files;
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  if (RecordFileSystem::Open(path, SettingsKey(), &files) != FileStatus::kOk) return 1;
  auto store = base::MakeRefCounted<SitePrefStore>(files, base::BindRepeating([](FileStatus) {}));
  if (store->ReadPrefs() != PersistentPrefStore::PREF_READ_ERROR_NONE) return 2;
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  registry->RegisterDictionaryPref(kZoom);
  registry->RegisterStringPref("appearance.theme", "system");
  PrefServiceFactory factory;
  factory.set_user_prefs(store);
  auto prefs = factory.Create(registry);
  auto map = base::MakeRefCounted<HostContentSettingsMap>(prefs.get(), false, false, false, false);
  const GURL first("https://one.alpha.test/"), second("https://beta.test/");
  bool good = map->GetContentSetting(first, first, ContentSettingsType::NOTIFICATIONS) == CONTENT_SETTING_ALLOW &&
      map->GetContentSetting(second, second, ContentSettingsType::NOTIFICATIONS) == CONTENT_SETTING_BLOCK &&
      store->IsConsideredFirstParty(first, second) && prefs->GetString("appearance.theme") == "dark";
  const auto* zooms = prefs->GetDict(kZoom).FindDict("x");
  good = good && zooms && zooms->FindDict("one.alpha.test") && zooms->FindDict("beta.test") &&
      zooms->FindDict("one.alpha.test")->FindDouble("zoom_level") == 1.25 &&
      zooms->FindDict("beta.test")->FindDouble("zoom_level") == 2.5;
  map->ShutdownOnUIThread();
  map.reset();
  prefs.reset();
  good = good && store->Flush() == FileStatus::kOk;
  store.reset();
  good = good && files->Lock() == FileStatus::kOk;
  return good ? 0 : 3;
}

}  // namespace alloy::site_records
