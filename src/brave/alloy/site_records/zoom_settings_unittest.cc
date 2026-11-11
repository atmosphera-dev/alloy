// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/ui/zoom/chrome_zoom_level_prefs.h"
#include "chrome/common/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service_factory.h"
#include "content/browser/host_zoom_map_impl.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_content_client_initializer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
SecretKey ZoomKey() { ObjectId key; key.fill(0x43); return SecretKey(key); }
class ZoomSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir()); path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, ZoomKey(), {}, &files_)); Open();
  }
  void TearDown() override { Close(); }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    bool initialized = false;
    store_->InitializeAsync(base::BindLambdaForTesting([&](FileStatus status) {
      EXPECT_EQ(FileStatus::kOk, status); initialized = true;
    }));
    tasks_.RunUntilIdle(); ASSERT_TRUE(initialized);
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    registry->RegisterDictionaryPref(prefs::kPartitionDefaultZoomLevel);
    registry->RegisterDictionaryPref(prefs::kPartitionPerHostZoomLevels);
    PrefServiceFactory factory; factory.set_user_prefs(store_); prefs_ = factory.Create(registry);
    map_ = std::make_unique<content::HostZoomMapImpl>();
    delegate_ = std::make_unique<ChromeZoomLevelPrefs>(prefs_.get(), path_, path_, base::WeakPtr<zoom::ZoomEventManager>());
    delegate_->InitHostZoomMap(map_.get());
  }
  void Close() {
    delegate_.reset(); map_.reset(); prefs_.reset(); store_.reset(); tasks_.RunUntilIdle();
  }
  void Reopen() {
    Close(); ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ZoomKey(), &files_)); Open();
  }
  void Flush() {
    bool done = false;
    { base::ScopedDisallowBlocking restricted;
      store_->FlushAsync(base::BindLambdaForTesting([&](FileStatus status) {
        EXPECT_EQ(FileStatus::kOk, status); done = true;
      })); tasks_.RunUntilIdle();
    }
    ASSERT_TRUE(done);
  }
  double Zoom(std::string host) { return map_->GetZoomLevelForHostAndScheme("https", host); }
  base::DictValue Levels(std::string host, double level, std::string stamp = "123456") {
    base::DictValue entry; entry.Set("zoom_level", level); entry.Set("last_modified", stamp);
    base::DictValue hosts; hosts.Set(host, std::move(entry));
    base::DictValue partitions; partitions.Set("x", std::move(hosts)); return partitions;
  }
  content::BrowserTaskEnvironment tasks_;
  content::TestContentClientInitializer content_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  std::unique_ptr<PrefService> prefs_;
  std::unique_ptr<content::HostZoomMapImpl> map_;
  std::unique_ptr<ChromeZoomLevelPrefs> delegate_;
};

TEST_F(ZoomSettingsTest, NativeZoomColdReadAndSelectiveDeletionRevokesTheLiveProjection) {
  delegate_->SetDefaultZoomLevelPref(0.5);
  map_->SetZoomLevelForHost("one.alpha.test", 1.25);
  map_->SetZoomLevelForHost("two.alpha.test", 1.5);
  map_->SetZoomLevelForHost("beta.test", 2.0); Flush(); Reopen();
  EXPECT_DOUBLE_EQ(1.25, Zoom("one.alpha.test")); EXPECT_DOUBLE_EQ(2.0, Zoom("beta.test"));
  bool callback = false, done = false;
  int pref_events = 0, zoom_events = 0;
  PrefChangeRegistrar registrar;
  registrar.Init(prefs_.get());
  registrar.Add(prefs::kPartitionPerHostZoomLevels,
      base::BindLambdaForTesting([&] { ++pref_events; }));
  auto subscription = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange&) { ++zoom_events; }));
  SitePrefStore::Completion drained;
  { base::ScopedDisallowBlocking restricted;
    store_->DeleteSiteWithTeardownAsync(GURL("https://alpha.test/"), base::BindLambdaForTesting([&](SitePrefStore::Completion resume) {
      callback = true;
      EXPECT_DOUBLE_EQ(0.5, Zoom("one.alpha.test")); EXPECT_DOUBLE_EQ(0.5, Zoom("two.alpha.test"));
      EXPECT_DOUBLE_EQ(2.0, Zoom("beta.test"));
      EXPECT_EQ(0, pref_events); EXPECT_EQ(0, zoom_events);
      RecordInfo record;
      EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(GURL("https://alpha.test/"),
          url::Origin::Create(GURL("https://alpha.test/")), false, &record));
      drained = std::move(resume);
    }), base::BindLambdaForTesting([&](FileStatus status, RecordDeletion) {
      EXPECT_EQ(FileStatus::kOk, status); done = true;
    })); tasks_.RunUntilIdle();
  }
  ASSERT_TRUE(callback); ASSERT_TRUE(drained); EXPECT_FALSE(done);
  EXPECT_EQ(0, pref_events); EXPECT_EQ(0, zoom_events);
  std::move(drained).Run(FileStatus::kOk); tasks_.RunUntilIdle();
  EXPECT_TRUE(done); EXPECT_EQ(1, pref_events); EXPECT_EQ(2, zoom_events);
  registrar.Reset(); subscription = {}; Reopen();
  EXPECT_DOUBLE_EQ(0.5, Zoom("one.alpha.test")); EXPECT_DOUBLE_EQ(2.0, Zoom("beta.test"));
}

TEST_F(ZoomSettingsTest, PrefReplacementPublishesOneCoherentMapWithoutEchoingTimestamps) {
  map_->SetZoomLevelForHost("alpha.test", 1.25); map_->SetZoomLevelForHost("beta.test", 2.0);
  map_->SetZoomLevelForHostAndScheme("chrome", "login", 0.0);
  int observed = 0;
  auto subscription = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange&) {
        ++observed; EXPECT_DOUBLE_EQ(0.75, map_->GetDefaultZoomLevel());
        EXPECT_DOUBLE_EQ(0.75, Zoom("alpha.test")); EXPECT_DOUBLE_EQ(3.0, Zoom("beta.test"));
      }));
  base::DictValue defaults; defaults.Set("x", 0.75);
  store_->SetValueSilently(prefs::kPartitionDefaultZoomLevel, base::Value(std::move(defaults)), 0);
  prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("beta.test", 3.0));
  EXPECT_GT(observed, 0); EXPECT_DOUBLE_EQ(0.0, map_->GetZoomLevelForHostAndScheme("chrome", "login"));
  const auto* hosts = prefs_->GetDict(prefs::kPartitionPerHostZoomLevels).FindDict("x");
  ASSERT_TRUE(hosts); ASSERT_TRUE(hosts->FindDict("beta.test"));
  EXPECT_EQ("123456", *hosts->FindDict("beta.test")->FindString("last_modified"));
  Flush();
}

TEST_F(ZoomSettingsTest, FreshNativeActionDuringPrefPublicationIsNotSwallowed) {
  map_->SetZoomLevelForHost("alpha.test", 1.25);
  bool submitted = false;
  auto subscription = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange& event) {
        if (!submitted && event.host == "alpha.test") {
          submitted = true; map_->SetZoomLevelForHost("gamma.test", 4.0);
        }
      }));
  prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("beta.test", 2.0));
  EXPECT_TRUE(submitted); EXPECT_DOUBLE_EQ(4.0, Zoom("gamma.test")); Flush();
  subscription = {}; Reopen(); EXPECT_DOUBLE_EQ(4.0, Zoom("gamma.test"));
}

TEST_F(ZoomSettingsTest, DefaultPrefAndNativeSetterHaveOneNotificationOwner) {
  int changed = 0;
  auto subscription = delegate_->RegisterDefaultZoomLevelCallback(base::BindLambdaForTesting([&] { ++changed; }));
  base::DictValue defaults; defaults.Set("x", 1.5);
  prefs_->SetDict(prefs::kPartitionDefaultZoomLevel, std::move(defaults));
  EXPECT_DOUBLE_EQ(1.5, map_->GetDefaultZoomLevel()); EXPECT_EQ(1, changed);
  delegate_->SetDefaultZoomLevelPref(2.0);
  EXPECT_DOUBLE_EQ(2.0, map_->GetDefaultZoomLevel()); EXPECT_EQ(2, changed); Flush();
}

TEST_F(ZoomSettingsTest, ObserverMayRetireTheNativeDelegateDuringReconciliation) {
  map_->SetZoomLevelForHost("alpha.test", 1.25);
  auto subscription = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange&) { delegate_.reset(); }));
  prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("beta.test", 2.0));
  EXPECT_FALSE(delegate_); EXPECT_DOUBLE_EQ(2.0, Zoom("beta.test"));
}

TEST_F(ZoomSettingsTest, NestedReplacementCannotDeliverAnObsoletePositiveToAnotherObserver) {
  map_->SetZoomLevelForHost("alpha.test", 1.25);
  map_->SetZoomLevelForHost("beta.test", 2.0);
  bool replaced = false;
  auto first = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange& event) {
        if (!replaced && event.host == "alpha.test") {
          replaced = true;
          prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("delta.test", 5.0));
        }
      }));
  int beta_removals = 0;
  auto second = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange& event) {
        if (!replaced) return;
        EXPECT_DOUBLE_EQ(5.0, Zoom("delta.test"));
        EXPECT_DOUBLE_EQ(0.0, Zoom("beta.test"));
        if (event.host == "beta.test") {
          EXPECT_DOUBLE_EQ(0.0, event.zoom_level);
          ++beta_removals;
        }
      }));
  prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("beta.test", 3.0));
  EXPECT_TRUE(replaced); EXPECT_GT(beta_removals, 0);
  Flush(); first = {}; second = {}; Reopen();
  EXPECT_DOUBLE_EQ(5.0, Zoom("delta.test")); EXPECT_DOUBLE_EQ(0.0, Zoom("beta.test"));
}

TEST_F(ZoomSettingsTest, DefaultNotificationMayRetireTheDelegate) {
  auto subscription = delegate_->RegisterDefaultZoomLevelCallback(
      base::BindLambdaForTesting([&] { delegate_.reset(); }));
  base::DictValue defaults; defaults.Set("x", 1.5);
  prefs_->SetDict(prefs::kPartitionDefaultZoomLevel, std::move(defaults));
  EXPECT_FALSE(delegate_); EXPECT_DOUBLE_EQ(1.5, map_->GetDefaultZoomLevel());
}

TEST_F(ZoomSettingsTest, PublicationMayRetireTheWholeNativeOwner) {
  map_->SetZoomLevelForHost("alpha.test", 1.25);
  int stale_callbacks = 0;
  auto first = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange&) {
        delegate_.reset(); map_.reset();
      }));
  auto second = map_->AddZoomLevelChangedCallback(base::BindLambdaForTesting(
      [&](const content::HostZoomMap::ZoomLevelChange&) { ++stale_callbacks; }));
  prefs_->SetDict(prefs::kPartitionPerHostZoomLevels, Levels("beta.test", 2.0));
  EXPECT_FALSE(delegate_); EXPECT_FALSE(map_); EXPECT_EQ(0, stale_callbacks);
}
}  // namespace
}  // namespace alloy::site_records
