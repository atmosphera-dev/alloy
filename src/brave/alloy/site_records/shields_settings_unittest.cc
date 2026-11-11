// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "brave/components/brave_shields/core/browser/brave_shields_p3a.h"
#include "brave/components/brave_shields/core/browser/brave_shields_settings_service.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/prefs/pref_registry_simple.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
namespace shields = brave_shields;
SecretKey ShieldsKey() { ObjectId key; key.fill(0x91); return SecretKey(key); }

class ShieldsSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir()); path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, ShieldsKey(), {}, &files_));
    Open();
  }
  void TearDown() override { Close(); }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    HostContentSettingsMap::RegisterProfilePrefs(registry.get());
    shields::BraveShieldsSettingsService::RegisterProfilePrefs(registry.get());
    shields::RegisterShieldsP3AProfilePrefs(registry.get());
    PrefServiceFactory factory; factory.set_user_prefs(store_); prefs_ = factory.Create(registry);
    local_store_ = base::MakeRefCounted<RecordPrefStore>(files_,
        files_->ProfileCapability(StorePurpose::kProfileSettings), StorePurpose::kProfileSettings,
        "local-state", base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, local_store_->ReadPrefs());
    auto local_registry = base::MakeRefCounted<PrefRegistrySimple>();
    shields::RegisterShieldsP3ALocalPrefs(local_registry.get());
    PrefServiceFactory local_factory; local_factory.set_user_prefs(local_store_);
    local_ = local_factory.Create(local_registry);
    map_ = base::MakeRefCounted<HostContentSettingsMap>(prefs_.get(), false, false, false, false);
    service_ = std::make_unique<shields::BraveShieldsSettingsService>(*map_, local_.get(), prefs_.get());
  }
  void Close() {
    service_.reset();
    if (map_) { map_->ShutdownOnUIThread(); map_.reset(); }
    prefs_.reset(); store_.reset(); local_.reset(); local_store_.reset();
  }
  void Reopen() {
    Close(); ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, ShieldsKey(), &files_)); Open();
  }
  std::string Seed(const GURL& url) {
    const auto value = map_->GetWebsiteSetting(url, url, ContentSettingsType::BRAVE_SHIELDS_METADATA);
    if (!value.is_dict()) return {};
    const auto* token = value.GetDict().FindString("farbling_token");
    return token ? *token : std::string();
  }
  void Flush() {
    ASSERT_EQ(FileStatus::kOk, store_->Flush()); ASSERT_EQ(FileStatus::kOk, local_store_->Flush());
  }
  void CheckCookieProjectionDuringHeldRetirement(ContentSetting alpha, ContentSetting beta) {
    class PrefObserver final : public PrefStore::Observer {
     public:
      void OnPrefValueChanged(std::string_view) override { ++events; }
      int events = 0;
    } pref_observer;
    class SettingsObserver final : public content_settings::Observer {
     public:
      void OnContentSettingChanged(const ContentSettingsPattern&,
          const ContentSettingsPattern&, ContentSettingsTypeSet) override { ++events; }
      int events = 0;
    } settings_observer;
    store_->AddObserver(&pref_observer);
    map_->AddObserver(&settings_observer);
    base::RunLoop prepared;
    SitePrefStore::Completion drained;
    base::test::TestFuture<FileStatus, RecordDeletion> result;
    store_->DeleteSiteWithTeardownAsync(a_, base::BindLambdaForTesting(
        [&](SitePrefStore::Completion resume) {
          drained = std::move(resume);
          prepared.Quit();
        }), result.GetCallback());
    prepared.Run();
    EXPECT_TRUE(drained);
    EXPECT_FALSE(result.IsReady());
    EXPECT_EQ(alpha, map_->GetContentSetting(a_, a_, ContentSettingsType::COOKIES));
    EXPECT_EQ(beta, map_->GetContentSetting(b_, b_, ContentSettingsType::COOKIES));
    RecordInfo refused;
    EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &refused));
    EXPECT_EQ(0, pref_observer.events);
    EXPECT_EQ(0, settings_observer.events);
    if (drained) std::move(drained).Run(FileStatus::kOk);
    EXPECT_TRUE(result.Wait());
    if (result.IsReady()) EXPECT_EQ(FileStatus::kOk, result.Get<0>());
    EXPECT_GT(pref_observer.events, 0);
    EXPECT_GT(settings_observer.events, 0);
    map_->RemoveObserver(&settings_observer);
    store_->RemoveObserver(&pref_observer);
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  scoped_refptr<RecordPrefStore> local_store_;
  std::unique_ptr<PrefService> prefs_, local_;
  scoped_refptr<HostContentSettingsMap> map_;
  std::unique_ptr<shields::BraveShieldsSettingsService> service_;
  const GURL a_{"https://one.alpha.test/"}, b_{"https://beta.test/"};
};

TEST_F(ShieldsSettingsTest, RealNativeSettingsAndRandomSiteSeedsColdReopen) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(shields::features::kBraveFarblingTokenReset);
  const auto first = service_->GetFarblingToken(a_, {});
  const auto second = service_->GetFarblingToken(b_, {});
  ASSERT_FALSE(first.is_zero()); ASSERT_FALSE(second.is_zero()); EXPECT_NE(first, second);
  EXPECT_EQ(first.ToString(), Seed(a_)); EXPECT_EQ(second.ToString(), Seed(b_));
  service_->SetNoScriptEnabled(true, a_);
  service_->SetNoScriptEnabled(false, b_);
  Flush(); Reopen();
  EXPECT_EQ(first, service_->GetFarblingToken(a_, {}));
  EXPECT_EQ(second, service_->GetFarblingToken(b_, {}));
  EXPECT_TRUE(service_->IsNoScriptEnabled(a_)); EXPECT_FALSE(service_->IsNoScriptEnabled(b_));
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(a_, [&] {
    EXPECT_TRUE(Seed(a_).empty()); EXPECT_EQ(second.ToString(), Seed(b_));
    EXPECT_FALSE(service_->IsNoScriptEnabled(a_));
  }, &deletion));
  Reopen();
  EXPECT_TRUE(Seed(a_).empty()); EXPECT_EQ(second.ToString(), Seed(b_));
  // This explicit fresh native call is after teardown and cold reopen. It must
  // allocate a new seed, not reconstruct the deleted identity from a profile seed.
  EXPECT_NE(first, service_->GetFarblingToken(a_, {}));
  EXPECT_EQ(second, service_->GetFarblingToken(b_, {}));
}

TEST_F(ShieldsSettingsTest, NativeSessionEntropyDoesNotBecomeAPersistentGlobalSiteSeed) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(shields::features::kBraveFarblingTokenReset);
  const auto first = service_->GetFarblingToken(a_, {});
  const auto seed = Seed(a_); ASSERT_FALSE(seed.empty());
  Flush(); Reopen();
  EXPECT_EQ(seed, Seed(a_));
  const auto second = service_->GetFarblingToken(a_, {});
  EXPECT_NE(first, second);
  EXPECT_EQ(seed, Seed(a_));
}

TEST_F(ShieldsSettingsTest, DerivedCookieRuleCacheIsPrunedBeforeHeldDrainAndSurvivesReopen) {
  map_->SetContentSettingDefaultScope(a_, a_, ContentSettingsType::BRAVE_COOKIES, CONTENT_SETTING_BLOCK);
  map_->SetContentSettingDefaultScope(b_, b_, ContentSettingsType::BRAVE_COOKIES, CONTENT_SETTING_BLOCK);
  Flush(); Reopen();
  ASSERT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(a_, a_, ContentSettingsType::COOKIES));
  ASSERT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(b_, b_, ContentSettingsType::COOKIES));
  CheckCookieProjectionDuringHeldRetirement(CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK);
  Reopen();
  EXPECT_EQ(CONTENT_SETTING_ALLOW, map_->GetContentSetting(a_, a_, ContentSettingsType::COOKIES));
  EXPECT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(b_, b_, ContentSettingsType::COOKIES));
}

TEST_F(ShieldsSettingsTest, DerivedShieldsDownCookieExceptionIsRevokedBeforeHeldDrain) {
  map_->SetDefaultContentSetting(ContentSettingsType::COOKIES, CONTENT_SETTING_BLOCK);
  service_->SetBraveShieldsEnabled(false, a_);
  Flush(); Reopen();
  ASSERT_EQ(CONTENT_SETTING_ALLOW, map_->GetContentSetting(a_, a_, ContentSettingsType::COOKIES));
  ASSERT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(b_, b_, ContentSettingsType::COOKIES));
  CheckCookieProjectionDuringHeldRetirement(CONTENT_SETTING_BLOCK, CONTENT_SETTING_BLOCK);
  Reopen();
  EXPECT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(a_, a_, ContentSettingsType::COOKIES));
  EXPECT_EQ(CONTENT_SETTING_BLOCK, map_->GetContentSetting(b_, b_, ContentSettingsType::COOKIES));
}

TEST_F(ShieldsSettingsTest, FunctionalChangesDoNotRetainUsageOrDomainCounters) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(shields::features::kBraveShowStrictFingerprintingMode);
  shields::MaybeRecordInitialShieldsSettings(local_.get(), prefs_.get(), map_.get(),
      shields::ControlType::BLOCK_THIRD_PARTY, shields::ControlType::DEFAULT);
  service_->SetBraveShieldsEnabled(false, a_);
  service_->SetAdBlockMode(shields::mojom::AdBlockMode::AGGRESSIVE, a_);
  service_->SetFingerprintMode(shields::mojom::FingerprintMode::STRICT_MODE, b_);
  service_->SetNoScriptEnabled(true, b_);
  shields::RecordManualShredP3A(*local_);
  EXPECT_FALSE(service_->IsBraveShieldsEnabled(a_));
  EXPECT_EQ(shields::mojom::AdBlockMode::AGGRESSIVE, service_->GetAdBlockMode(a_));
  EXPECT_EQ(shields::mojom::FingerprintMode::STRICT_MODE, service_->GetFingerprintMode(b_));
  EXPECT_TRUE(service_->IsNoScriptEnabled(b_));
  for (const auto* key : {shields::kUsagePrefName, shields::kManualShredTriggeredPrefName})
    EXPECT_EQ(nullptr, local_->GetUserPrefValue(key));
  for (const auto* key : {shields::kFirstReportedRevisionPrefName, shields::kAdsStrictCountPrefName,
      shields::kAdsStandardCountPrefName, shields::kAdsAllowCountPrefName,
      shields::kFPStrictCountPrefName, shields::kFPStandardCountPrefName, shields::kFPAllowCountPrefName})
    EXPECT_EQ(nullptr, prefs_->GetUserPrefValue(key));
  Flush(); Reopen();
  EXPECT_FALSE(service_->IsBraveShieldsEnabled(a_));
  EXPECT_EQ(shields::mojom::AdBlockMode::AGGRESSIVE, service_->GetAdBlockMode(a_));
  EXPECT_EQ(shields::mojom::FingerprintMode::STRICT_MODE, service_->GetFingerprintMode(b_));
  EXPECT_TRUE(service_->IsNoScriptEnabled(b_));
  EXPECT_EQ(nullptr, local_->GetUserPrefValue(shields::kUsagePrefName));
  EXPECT_EQ(nullptr, prefs_->GetUserPrefValue(shields::kFPStrictCountPrefName));
}
}  // namespace
}  // namespace alloy::site_records
