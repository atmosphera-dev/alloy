// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_profile.h"

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include "brave/alloy/site_records/site_pref_store_io.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "components/content_settings/core/browser/owned_settings_registries.h"
#include "base/feature_list.h"
#include <set>
#include <chrono>
#include <thread>

namespace alloy::site_records {
namespace {
class DecaySource : public KeySource {
 public:
  KeySourceKind kind() const override { return KeySourceKind::kExternal; }
  FileStatus Derive(const KeySourceParameters&, SecretKey* key) override {
    if (check) check();
    ObjectId bytes{}; bytes.fill(0x73); *key = SecretKey(bytes); return FileStatus::kOk;
  }
  std::function<void()> check;
};
class SiteRecordDecayTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir()); path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source_, &profile_));
    files_ = profile_->files();
    // Runtime injection also makes selection/publication races deterministic.
    profile_->BindNativeDecay(files_, [] {});
  }
  RecordInfo Site(const GURL& site) {
    RecordInfo record;
    EXPECT_EQ(FileStatus::kOk, files_->ResolveRecord(site, url::Origin::Create(site), true, &record));
    return record;
  }
  scoped_refptr<SitePrefStore> Prefs() {
    auto prefs = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    EXPECT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, prefs->ReadPrefs()); return prefs;
  }
  base::test::TaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  DecaySource source_;
  std::unique_ptr<EncryptedProfile> profile_;
  std::shared_ptr<RecordFileSystem> files_;
  const GURL a_{"https://alpha.test/"}, b_{"https://beta.test/"}, c_{"https://gamma.test/"};
};
TEST(SiteRecordRegistryTest, OwnedAndGlobalGraphsClassifyEveryRegisteredTypeAndPrefKeyIdentically) {
  using namespace content_settings;
  auto* global_content = ContentSettingsRegistry::GetInstance();
  auto* global_permissions = PermissionSettingsRegistry::GetInstance();
  const auto* global_website = WebsiteSettingsRegistry::GetInstance();
  std::set<ContentSettingsType> types;
  std::set<std::string> keys{"profile.content_settings.exceptions.definitely_unknown",
      "profile.content_settings.partitioned_exceptions.definitely_unknown", "ordinary.configuration"};
  {
    OwnedSettingsRegistries owned;
    EXPECT_NE(global_content, &owned.content());
    EXPECT_NE(global_permissions, &owned.permissions());
    EXPECT_NE(global_website, &owned.website());
    for (const auto* registry : {global_website, &owned.website()}) {
      for (const auto* info : *registry) {
        types.insert(info->type());
        keys.insert(info->pref_name()); keys.insert(info->partitioned_pref_name());
        keys.insert(info->default_value_pref_name());
        keys.insert(info->pref_name() + ".definitely_unknown");
      }
    }
    ASSERT_FALSE(types.empty());
    for (const auto type : types) {
      const auto* global = global_website->Get(type);
      const auto* local = owned.website().Get(type);
      ASSERT_TRUE(global); ASSERT_TRUE(local);
      EXPECT_EQ(global->name(), local->name());
      EXPECT_EQ(global->pref_name(), local->pref_name());
      EXPECT_EQ(global->partitioned_pref_name(), local->partitioned_pref_name());
      EXPECT_EQ(global->default_value_pref_name(), local->default_value_pref_name());
      EXPECT_EQ(!!global_content->Get(type), !!owned.content().Get(type));
      EXPECT_EQ(!!global_permissions->Get(type), !!owned.permissions().Get(type));
    }
    for (const auto& key : keys) {
      EXPECT_EQ(internal::SitePrefStore::IsRegisteredExceptionPref(key),
                internal::SitePrefStore::IsRegisteredExceptionPref(key, &owned.website())) << key;
    }
    EXPECT_FALSE(internal::SitePrefStore::IsRegisteredExceptionPref(
        "profile.content_settings.exceptions.definitely_unknown", &owned.website()));
  }
  // Scoped destruction neither replaces nor destroys any global registry.
  EXPECT_EQ(global_content, ContentSettingsRegistry::GetInstance());
  EXPECT_EQ(global_permissions, PermissionSettingsRegistry::GetInstance());
  EXPECT_EQ(global_website, WebsiteSettingsRegistry::GetInstance());
  for (const auto type : types) EXPECT_TRUE(global_website->Get(type));
}
TEST_F(SiteRecordDecayTest, OwnedRegistryKeepsUnknownSettingRejectionStrict) {
  content_settings::OwnedSettingsRegistries owned;
  auto prefs = base::MakeRefCounted<internal::SitePrefStore>(files_,
      base::BindRepeating([](FileStatus) {}), &owned.website());
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, prefs->ReadPrefs());
  base::DictValue permissions;
  permissions.Set("https://alpha.test:443,*", base::DictValue().Set("setting", 1));
  prefs->SetValue("profile.content_settings.exceptions.notifications", base::Value(std::move(permissions)), 0);
  ASSERT_EQ(FileStatus::kOk, prefs->Flush());
  constexpr char unknown[] = "profile.content_settings.exceptions.definitely_unknown";
  prefs->SetValue(unknown, base::Value(base::DictValue()), 0);
  EXPECT_EQ(FileStatus::kInvalid, prefs->Flush());
  base::DictValue snapshot; snapshot.Set(unknown, base::DictValue());
  base::DictValue retained;
  EXPECT_EQ(FileStatus::kInvalid, internal::SitePrefStore::PruneSnapshot(std::move(snapshot), {}, &retained));
}
TEST(SiteRecordRegistryTest, OwnedConstructionDoesNotReadRuntimeFeaturesBeforeInitialization) {
  auto features = base::FeatureList::ClearInstanceForTesting();
  {
    content_settings::OwnedSettingsRegistries owned;
    EXPECT_TRUE(owned.website().Get(ContentSettingsType::BRAVE_REMEMBER_1P_STORAGE));
    EXPECT_EQ(nullptr, base::FeatureList::GetEarlyAccessedFeatureForTesting());
  }
  base::FeatureList::RestoreInstanceForTesting(std::move(features));
}
TEST_F(SiteRecordDecayTest, NoDefaultAndEncryptedPolicyColdReopensWithoutResupplyingIt) {
  DecayPolicy policy;
  ASSERT_EQ(FileStatus::kOk, profile_->GetDecayPolicy(&policy)); EXPECT_EQ(0, policy.period_us);
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({2000000}));
  EXPECT_EQ(FileStatus::kInvalid, profile_->SetDecayPolicy({-1}));
  EXPECT_EQ(FileStatus::kInvalid, profile_->SetDecayPolicy({1, static_cast<DecayTimebase>(9)}));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  EXPECT_EQ(FileStatus::kDenied, profile_->GetDecayPolicy(&policy));
  EXPECT_EQ(FileStatus::kDenied, profile_->SetDecayPolicy({3000000}));
  profile_.reset();
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Open(path_, &profile_));
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(source_));
  ASSERT_EQ(FileStatus::kOk, profile_->GetDecayPolicy(&policy));
  EXPECT_EQ(2000000, policy.period_us); EXPECT_EQ(DecayTimebase::kSinceLastVisit, policy.timebase);
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({0}));
}
TEST_F(SiteRecordDecayTest, UnlockPrunesWholeExpiredRecordAndReferencesBeforePublishingAnyReads) {
  auto prefs = Prefs();
  ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(a_, {b_}));
  ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(b_, {a_, c_}));
  auto alpha = Site(a_), beta = Site(b_);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(alpha.id, DecayNowMicros() - 10000000));
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(beta.id, DecayNowMicros()));
  std::shared_ptr<RecordFile> pending;
  auto old = files_->Capability(alpha.id, StorePurpose::kIndexedDb);
  ASSERT_EQ(FileStatus::kOk, files_->OpenFile(old, StorePurpose::kIndexedDb, "pending", RecordOpen::kCreate, &pending));
  ASSERT_EQ(FileStatus::kOk, pending->AppendBuffered(base::as_byte_span(std::string_view("old-data"))));
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({5000000}));
  prefs.reset(); tasks_.RunUntilIdle();
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  source_.check = [&] { EXPECT_FALSE(profile_->files()); EXPECT_EQ(ProfileState::kTransition, profile_->state()); };
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(source_));
  files_ = profile_->files(); profile_->BindNativeDecay(files_, [] {});
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records)); ASSERT_EQ(1u, records.size());
  EXPECT_EQ(beta.id, records[0].id);
  prefs = Prefs();
  EXPECT_FALSE(prefs->IsConsideredFirstParty(a_, b_));
  EXPECT_FALSE(prefs->IsConsideredFirstParty(b_, a_));
  EXPECT_TRUE(prefs->IsConsideredFirstParty(b_, c_));
  EXPECT_FALSE(old.active()); EXPECT_TRUE(pending->KeysClearedForTesting());
  EXPECT_EQ(FileStatus::kDenied, pending->FlushBuffered());
  EXPECT_EQ(FileStatus::kDenied, pending->Write(0, base::as_byte_span(std::string_view("late"))));
}
TEST_F(SiteRecordDecayTest, NeverVisitedUsesCreationAndFutureVisitsClampOnce) {
  const auto created = Site(a_);
  const auto future = Site(b_);
  const auto now = DecayNowMicros();
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(future.id, now + 100000000));
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({1000000}));
  std::optional<RecordInfo> expired;
  ASSERT_EQ(FileStatus::kOk, NextDecayedRecord(*files_, now, &expired)); EXPECT_FALSE(expired);
  std::vector<RecordInfo> records; ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  for (const auto& record : records) if (record.id == future.id) EXPECT_EQ(now, record.last_visit_us);
  ASSERT_EQ(FileStatus::kOk, NextDecayedRecord(*files_, now + 1000001, &expired)); ASSERT_TRUE(expired);
  EXPECT_TRUE(expired->id == created.id || expired->id == future.id);
  // A future value must not be treated as a fresh 'now' on every subsequent sweep.
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(created.id, now + 1000001));
  ASSERT_EQ(FileStatus::kOk, NextDecayedRecord(*files_, now + 1000001, &expired));
  ASSERT_TRUE(expired); EXPECT_EQ(future.id, expired->id);
}
TEST_F(SiteRecordDecayTest, AtomicVisitGuardCannotDeleteANewerVisitOrItsSettings) {
  auto prefs = Prefs(); ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(a_, {b_}));
  const auto original = Site(a_);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(original.id, DecayNowMicros()));
  base::UnguessableToken ticket;
  EXPECT_EQ(FileStatus::kBusy, files_->RetireRecordWithSettings(original.id, {}, &ticket, 0));
  auto selected = base::MakeRefCounted<internal::SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, selected->ReadPrefs());
  EXPECT_EQ(FileStatus::kBusy, selected->RetireRecord(original, &ticket));
  EXPECT_TRUE(ticket.is_empty()); EXPECT_TRUE(prefs->IsConsideredFirstParty(a_, b_));
  EXPECT_TRUE(files_->Capability(original.id, StorePurpose::kSiteSettings).active());
}
TEST_F(SiteRecordDecayTest, EngineTimerExpiresRecordsWithoutAnyNativeContextOrUiPump) {
  const auto record = Site(a_);
  const auto old = files_->Capability(record.id, StorePurpose::kCookies);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(record.id, DecayNowMicros() - 5000000));
  profile_->BindNativeDecay(files_, {});
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({20000}));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (old.active() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(old.active());
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &absent));
  EXPECT_TRUE(profile_->unlocked());
}
TEST_F(SiteRecordDecayTest, ExpirationSnapshotCannotBorrowAReplacementGenerationWithTheSameTimestamp) {
  auto prefs = base::MakeRefCounted<internal::SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
  ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, prefs->ReadPrefs());
  ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(a_, {b_}));
  const auto original = Site(a_);
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, prefs->DeleteSite(a_, {}, &deletion));
  ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(a_, {c_}));
  const auto fresh = Site(a_); EXPECT_NE(original.id, fresh.id);
  EXPECT_EQ(original.last_visit_us, fresh.last_visit_us);
  base::UnguessableToken ticket;
  EXPECT_EQ(FileStatus::kBusy, prefs->RetireSite(a_, &ticket, &original));
  EXPECT_EQ(FileStatus::kNotFound, prefs->RetireRecord(original, &ticket));
  EXPECT_TRUE(ticket.is_empty()); EXPECT_TRUE(prefs->IsConsideredFirstParty(a_, c_));
  EXPECT_TRUE(files_->Capability(fresh.id, StorePurpose::kSiteSettings).active());
}
TEST_F(SiteRecordDecayTest, InvalidStoredPolicyRefusesUnlockInsteadOfIgnoringEnforcement) {
  const std::string invalid = "not-a-decay-policy";
  const SettingsReplacement replacement{files_->ProfileCapability(StorePurpose::kProfileSettings),
      StorePurpose::kProfileSettings, "site-record-decay", base::as_byte_span(invalid)};
  ASSERT_EQ(FileStatus::kOk, files_->ReplaceSettings(base::span_from_ref(replacement)));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  EXPECT_EQ(FileStatus::kIntegrity, profile_->Unlock(source_));
  EXPECT_FALSE(profile_->files()); EXPECT_TRUE(profile_->KeysClearedForTesting());
}
TEST_F(SiteRecordDecayTest, NativeDecayPreservesDottedRecordIdentity) {
  const GURL dotted("https://baz.com./"), plain("https://baz.com/");
  const auto expired = Site(dotted), retained = Site(plain);
  ASSERT_NE(expired.id, retained.id);
  ASSERT_EQ("baz.com.", expired.domain);
  const auto old = files_->Capability(expired.id, StorePurpose::kCookies);
  const auto other = files_->Capability(retained.id, StorePurpose::kCookies);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(expired.id, DecayNowMicros() - 10000000));
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(retained.id, DecayNowMicros()));
  auto prefs = Prefs();
  ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(b_, {dotted, plain}));
  ASSERT_TRUE(prefs->IsConsideredFirstParty(b_, dotted));
  ASSERT_TRUE(prefs->IsConsideredFirstParty(b_, plain));
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({5000000}));
  base::test::TestFuture<FileStatus, bool> result;
  bool teardown = false;
  prefs->DecayNextAsync(base::BindLambdaForTesting([&](GURL site, SitePrefStore::Completion done) {
    teardown = true;
    EXPECT_EQ(expired.domain, RecordDomainForPrincipal(site, url::Origin::Create(site)));
    EXPECT_FALSE(old.active());
    EXPECT_TRUE(other.active());
    std::move(done).Run(FileStatus::kOk);
  }), result.GetCallback());
  ASSERT_TRUE(result.Wait());
  EXPECT_EQ(FileStatus::kOk, result.Get<0>());
  EXPECT_TRUE(result.Get<1>());
  EXPECT_TRUE(teardown);
  EXPECT_FALSE(old.active());
  EXPECT_TRUE(other.active());
  EXPECT_FALSE(prefs->IsConsideredFirstParty(b_, dotted));
  EXPECT_TRUE(prefs->IsConsideredFirstParty(b_, plain));
  RecordInfo absent;
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(dotted, url::Origin::Create(dotted), false, &absent));
}

TEST_F(SiteRecordDecayTest, UnlockRetiresDottedRecordWithoutSelectingItsUndottedNeighbor) {
  const GURL dotted("https://baz.com./"), plain("https://baz.com/");
  const auto expired = Site(dotted), retained = Site(plain);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(expired.id, DecayNowMicros() - 10000000));
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(retained.id, DecayNowMicros()));
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({5000000}));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(source_));
  files_ = profile_->files();
  profile_->BindNativeDecay(files_, [] {});
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records));
  ASSERT_EQ(1u, records.size());
  EXPECT_EQ(retained.id, records[0].id);
}

TEST_F(SiteRecordDecayTest, NativeDecayUsesTheDeletionBarrierAndCancelsOldQueuedWriters) {
  auto prefs = Prefs(); ASSERT_EQ(FileStatus::kOk, prefs->SetFirstPartyEmbeds(a_, {b_}));
  const auto original = Site(a_); const auto old = files_->Capability(original.id, StorePurpose::kSiteSettings);
  ASSERT_EQ(FileStatus::kOk, files_->RecordVisit(original.id, DecayNowMicros() - 5000000));
  ASSERT_EQ(FileStatus::kOk, profile_->SetDecayPolicy({1000000}));
  SitePrefStore::Completion resume;
  bool deleted = false, stale = false;
  prefs->DecayNextAsync(base::BindLambdaForTesting([&](GURL site, SitePrefStore::Completion done) {
    EXPECT_EQ("alpha.test", site.host()); EXPECT_FALSE(old.active()); resume = std::move(done);
    prefs->DeferNativeRetirementNotification(base::BindLambdaForTesting([&] {
      EXPECT_FALSE(prefs->RecordWorkingStateAdmitted("alpha.test"));
      EXPECT_EQ(FileStatus::kBusy, prefs->Flush());
    }));
  }), base::BindLambdaForTesting([&](FileStatus status, bool retired) {
    EXPECT_EQ(FileStatus::kOk, status); EXPECT_TRUE(retired); deleted = true;
  }));
  prefs->SetFirstPartyEmbedsAsync(a_, {c_}, base::BindLambdaForTesting([&](FileStatus status) {
    EXPECT_EQ(FileStatus::kCancelled, status); stale = true;
  }));
  prefs->RecordVisitAsync(a_, DecayNowMicros());
  tasks_.RunUntilIdle(); ASSERT_TRUE(resume); EXPECT_FALSE(deleted); EXPECT_FALSE(stale);
  RecordInfo refused;
  EXPECT_EQ(FileStatus::kBusy, files_->ResolveRecord(a_, url::Origin::Create(a_), true, &refused));
  std::move(resume).Run(FileStatus::kOk); tasks_.RunUntilIdle(); EXPECT_TRUE(deleted); EXPECT_TRUE(stale);
  EXPECT_EQ(FileStatus::kNotFound, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &refused));
  EXPECT_FALSE(prefs->IsConsideredFirstParty(a_, b_));
  prefs->RecordVisitAsync(a_, DecayNowMicros()); tasks_.RunUntilIdle();
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(a_, url::Origin::Create(a_), false, &refused));
  EXPECT_NE(original.generation, refused.generation);
}
}  // namespace
}  // namespace alloy::site_records
