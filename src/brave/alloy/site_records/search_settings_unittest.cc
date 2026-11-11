// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_pref_store.h"

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/test/scoped_command_line.h"
#include "base/test/task_environment.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/regional_capabilities/regional_capabilities_switches.h"
#include "components/regional_capabilities/regional_capabilities_test_utils.h"
#include "components/search_engines/default_search_manager.h"
#include "components/search_engines/search_engines_test_environment.h"
#include "components/search_engines/template_url_data.h"
#include "components/search_engines/template_url_data_util.h"
#include "components/search_engines/template_url_prepopulate_data_resolver.h"
#include "components/search_engines/template_url_service_test_util.h"
#include "components/variations/scoped_variations_ids_provider.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
using Manager = DefaultSearchManager;
SecretKey SearchKey() { ObjectId bytes; bytes.fill(0x49); return SecretKey(bytes); }
TemplateURLData Definition() {
  TemplateURLData data;
  data.SetShortName(u"explicit private search choice");
  data.SetKeyword(u"private-keyword");
  data.SetURL("https://search-provider.test/find?q={searchTerms}");
  data.suggestions_url = "https://search-provider.test/suggest?q={searchTerms}";
  data.search_url_post_params = "q={searchTerms}";
  data.image_url = "https://search-provider.test/image?q={searchTerms}";
  data.image_url_post_params = "image={searchTerms}";
  data.alternate_urls = {"https://search-provider.test/alternate?q={searchTerms}"};
  data.input_encodings = {"UTF-8", "UTF-16"};
  data.date_created = base::Time::FromInternalValue(1234567);
  data.last_modified = base::Time::FromInternalValue(7654321);
  data.last_visited = base::Time::FromInternalValue(987654321);
  data.usage_count = 42;
  data.originating_url = GURL("https://discovery-origin.test/private-opensearch-marker");
  data.send_x_geo_header = true;
  data.preconnect_to_search_url = true;
  data.prefetch_likely_navigations = true;
  return data;
}
void FunctionalConfig(const TemplateURLData& actual) {
  const auto expected = Definition();
  EXPECT_EQ(expected.short_name(), actual.short_name());
  EXPECT_EQ(expected.keyword(), actual.keyword());
  EXPECT_EQ(expected.url(), actual.url());
  EXPECT_EQ(expected.suggestions_url, actual.suggestions_url);
  EXPECT_EQ(expected.search_url_post_params, actual.search_url_post_params);
  EXPECT_EQ(expected.image_url, actual.image_url);
  EXPECT_EQ(expected.image_url_post_params, actual.image_url_post_params);
  EXPECT_EQ(expected.alternate_urls, actual.alternate_urls);
  EXPECT_EQ(expected.input_encodings, actual.input_encodings);
  EXPECT_EQ(expected.date_created, actual.date_created);
  EXPECT_EQ(expected.last_modified, actual.last_modified);
  EXPECT_TRUE(actual.send_x_geo_header);
  EXPECT_TRUE(actual.preconnect_to_search_url);
  EXPECT_TRUE(actual.prefetch_likely_navigations);
}
void NoActivity(const TemplateURLData& actual) {
  EXPECT_TRUE(actual.last_visited.is_null());
  EXPECT_EQ(0, actual.usage_count);
  EXPECT_TRUE(actual.originating_url.is_empty());
}

class SearchSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    command_.GetProcessCommandLine()->AppendSwitchASCII(switches::kSearchEngineChoiceCountry, "US");
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, SearchKey(), {}, &files_));
    Open();
  }
  void Open() {
    store_ = base::MakeRefCounted<SitePrefStore>(files_, base::BindRepeating([](FileStatus) {}));
    ASSERT_EQ(PersistentPrefStore::PREF_READ_ERROR_NONE, store_->ReadPrefs());
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    RegisterPrefsForTemplateURLService(registry.get());
    PrefServiceFactory factory;
    factory.set_user_prefs(store_);
    prefs_ = factory.Create(registry);
    regional_ = regional_capabilities::CreateServiceWithFakeClient(*prefs_);
    resolver_ = std::make_unique<TemplateURLPrepopulateData::Resolver>(*prefs_, *regional_);
    // The native test environment supplies only the non-guest choice-service
    // dependency. The manager, resolver and both of their pref dictionaries
    // use the real encrypted PrefService above, not the environment's prefs.
    manager_ = std::make_unique<Manager>(prefs_.get(), &dependencies_.search_engine_choice_service(),
                                        *resolver_, Manager::ObserverCallback());
  }
  void Close() {
    manager_.reset(); resolver_.reset(); regional_.reset(); prefs_.reset(); store_.reset();
  }
  void Reopen() {
    Close();
    ASSERT_EQ(FileStatus::kOk, files_->Lock()); files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, SearchKey(), &files_));
    Open();
  }
  void Selected() {
    Manager::Source source = Manager::FROM_FALLBACK;
    const auto* selected = manager_->GetDefaultSearchEngine(&source);
    ASSERT_TRUE(selected);
    EXPECT_EQ(Manager::FROM_USER, source);
    FunctionalConfig(*selected);
    NoActivity(*selected);
  }
  base::test::TaskEnvironment tasks_;
  base::test::ScopedCommandLine command_;
  variations::test::ScopedVariationsIdsProvider variations_{variations::VariationsIdsProvider::Mode::kUseSignedInState};
  search_engines::SearchEnginesTestEnvironment dependencies_;
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::shared_ptr<RecordFileSystem> files_;
  scoped_refptr<SitePrefStore> store_;
  std::unique_ptr<PrefService> prefs_;
  std::unique_ptr<regional_capabilities::RegionalCapabilitiesService> regional_;
  std::unique_ptr<TemplateURLPrepopulateData::Resolver> resolver_;
  std::unique_ptr<Manager> manager_;
};

TEST_F(SearchSettingsTest, ActualNativeDefaultAndMirrorKeepConfigurationWithoutActivity) {
  manager_->SetUserSelectedDefaultSearchEngine(Definition());
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  Selected();
  for (const auto* key : {Manager::kDefaultSearchProviderDataPrefName, Manager::kMirroredDefaultSearchProviderDataPrefName}) {
    const auto& dict = prefs_->GetDict(key);
    EXPECT_FALSE(dict.contains(Manager::kLastVisited));
    EXPECT_FALSE(dict.contains(Manager::kUsageCount));
    EXPECT_FALSE(dict.contains(Manager::kOriginatingURL));
  }
  Reopen(); Selected();
  EXPECT_EQ(prefs_->GetDict(Manager::kDefaultSearchProviderDataPrefName),
            prefs_->GetDict(Manager::kMirroredDefaultSearchProviderDataPrefName));
  base::CommandLine command(base::FilePath("/usr/bin/timeout"));
  for (const auto* arg : {"30", "rg", "-a", "-i", "-F", "-l", "-e", "search-provider.test",
                          "-e", "discovery-origin.test", "-e", "private search choice"}) command.AppendArg(arg);
  command.AppendArgPath(path_);
  std::string output; int exit = -1;
  ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
  EXPECT_EQ(1, exit) << output;
}

TEST_F(SearchSettingsTest, SharedNativeDictionaryReaderDoesNotRestoreVisitOrDiscoveryFacts) {
  auto dict = TemplateURLDataToDictionary(Definition());
  // This is not a migration/backfill. Untrusted/native input to the reusable
  // configuration reader must not acquire browsing-state semantics.
  dict.Set(Manager::kLastVisited, "987654321");
  dict.Set(Manager::kUsageCount, 42);
  dict.Set(Manager::kOriginatingURL, "https://discovery-origin.test/private-opensearch-marker");
  auto decoded = TemplateURLDataFromDictionary(dict);
  ASSERT_TRUE(decoded); FunctionalConfig(*decoded); NoActivity(*decoded);
}

TEST_F(SearchSettingsTest, NativePrefMapAndExternalChangeMirrorDoNotCopyActivity) {
  auto input = TemplateURLDataToDictionary(Definition());
  input.Set(Manager::kLastVisited, "987654321");
  input.Set(Manager::kUsageCount, 42);
  input.Set(Manager::kOriginatingURL, "https://discovery-origin.test/private-opensearch-marker");
  PrefValueMap map;
  Manager::AddPrefValueToMap(input.Clone(), &map);
  const base::Value* value = nullptr;
  ASSERT_TRUE(map.GetValue(Manager::kDefaultSearchProviderDataPrefName, &value));
  ASSERT_TRUE(value->is_dict());
  for (const auto* key : {Manager::kLastVisited, Manager::kUsageCount, Manager::kOriginatingURL})
    EXPECT_FALSE(value->GetDict().contains(key));
  // External/read-only input is not rewritten. The manager's own generated
  // mirror must carry only configuration, as must its decoded consumer view.
  prefs_->SetDict(Manager::kDefaultSearchProviderDataPrefName, std::move(input));
  Selected();
  const auto& mirror = prefs_->GetDict(Manager::kMirroredDefaultSearchProviderDataPrefName);
  for (const auto* key : {Manager::kLastVisited, Manager::kUsageCount, Manager::kOriginatingURL})
    EXPECT_FALSE(mirror.contains(key));
  EXPECT_EQ(Definition().url(), *mirror.FindString(Manager::kURL));
}

TEST_F(SearchSettingsTest, ExplicitGlobalDefinitionDoesNotCreateOrDependOnASiteRecord) {
  manager_->SetUserSelectedDefaultSearchEngine(Definition());
  ASSERT_EQ(FileStatus::kOk, store_->Flush());
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files_->ListRecords(&records)); EXPECT_TRUE(records.empty());
  RecordInfo record; const GURL url("https://search-provider.test/");
  ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &record));
  RecordDeletion result;
  ASSERT_EQ(FileStatus::kOk, store_->DeleteSite(url, {}, &result));
  Selected(); Reopen(); Selected();
}
}  // namespace
}  // namespace alloy::site_records
