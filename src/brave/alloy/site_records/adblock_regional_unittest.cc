// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "brave/components/brave_shields/core/browser/ad_block_filter_list_catalog_provider.h"
#include "brave/components/brave_shields/core/browser/ad_block_list_p3a.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/in_memory_pref_store.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service_factory.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::site_records {
namespace {
using Manager=brave_shields::AdBlockComponentServiceManager;
using Entry=brave_shields::FilterListCatalogEntry;
namespace names=brave_shields::prefs;
struct Input {
  explicit Input(StorageMode mode):issuer(base::UnguessableToken::Create(),base::UnguessableToken::Create(),base::UnguessableToken::Create(),mode) {}
  RecordCapabilityIssuer issuer;
  std::shared_ptr<ProfileFilterStorage> storage;
  scoped_refptr<ProfileFilterPreferences> input;
  std::unique_ptr<PrefService> prefs;
};
class AdBlockRegionalTest:public ChromeRenderViewHostTestHarness {
 protected:
  std::unique_ptr<Input> MakeInput(StorageMode mode,scoped_refptr<PrefStore> startup=nullptr) {
    auto owner=std::make_unique<Input>(mode);
    EXPECT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(nullptr,owner->issuer.Issue(StorePurpose::kProfileCustomFilters),&owner->storage));
    base::test::TestFuture<FileStatus,scoped_refptr<ProfileFilterPreferences>> loaded;
    ProfileFilterPreferences::Load(owner->storage,base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindRepeating([](FileStatus){ADD_FAILURE();}),loaded.GetCallback());task_environment()->RunUntilIdle();
    if(!loaded.IsReady()){ADD_FAILURE();return nullptr;}EXPECT_EQ(FileStatus::kOk,loaded.Get<0>());owner->input=loaded.Get<1>();
    auto registry=base::MakeRefCounted<PrefRegistrySimple>();brave_shields::RegisterPrefsForAdBlockService(registry.get());
    PrefServiceFactory factory;factory.set_user_prefs(owner->input);if(startup)factory.set_command_line_prefs(startup);
    owner->prefs=factory.Create(registry);return owner;
  }
  std::vector<Entry> Catalog() {
    std::vector<Entry> rows;
    auto add=[&](const char* uuid,std::vector<std::string> languages,bool enabled,uint8_t key) {
      rows.emplace_back();auto& entry=rows.back();entry.uuid=uuid;entry.title=uuid;entry.langs=std::move(languages);entry.default_enabled=enabled;
      std::array<uint8_t,crypto::kSHA256Length> bytes{};bytes.fill(key);entry.public_key_sha256=bytes;
    };
    add("locale-list",{"en"},false,1);add("default-list",{},true,2);add("optional-list",{},false,3);return rows;
  }
  bool Listed(Manager& manager,const char* uuid) {
    auto rows=manager.GetRegionalLists();for(const auto& row:rows)if(row.GetDict().FindString("uuid")&&*row.GetDict().FindString("uuid")==uuid)return row.GetDict().FindBool("enabled").value_or(false);
    ADD_FAILURE()<<"regional catalog entry not exposed";return false;
  }
  void TearDown() override {task_environment()->RunUntilIdle();ChromeRenderViewHostTestHarness::TearDown();}
};
TEST_F(AdBlockRegionalTest, ReadonlyLocaleAndCatalogDefaultsNeedNoPrivateChoicesAndEditsRefuse) {
  auto owner=MakeInput(StorageMode::kStateless);ASSERT_TRUE(owner);
  brave_shields::AdBlockFiltersProviderManager providers;brave_shields::AdBlockFilterListCatalogProvider catalog(nullptr);brave_shields::AdBlockListP3A metrics(owner->prefs.get());
  Manager manager(owner->prefs.get(),&providers,"en-US",nullptr,&catalog,&metrics);const auto initial=providers.GetProviders(false).size();
  manager.SetFilterListCatalog(Catalog());
  EXPECT_TRUE(Listed(manager,"locale-list"));EXPECT_TRUE(Listed(manager,"default-list"));EXPECT_FALSE(Listed(manager,"optional-list"));
  EXPECT_EQ(initial+2,providers.GetProviders(false).size());const auto stable=providers.GetProviders(false).size();
  EXPECT_FALSE(manager.EnableFilterList("locale-list",false));EXPECT_FALSE(manager.EnableFilterList("optional-list",true));
  EXPECT_EQ(stable,providers.GetProviders(false).size());EXPECT_TRUE(Listed(manager,"locale-list"));EXPECT_FALSE(Listed(manager,"optional-list"));
  EXPECT_TRUE(owner->input->GetValues().empty());
  EXPECT_FALSE(owner->prefs->GetBoolean(names::kAdBlockCheckedAllDefaultRegions));
  EXPECT_TRUE(owner->prefs->GetDict(names::kAdBlockRegionalFilters).empty());
}
TEST_F(AdBlockRegionalTest, ExplicitReadonlyStartupChoiceOverridesDerivedLocaleDefault) {
  auto startup=base::MakeRefCounted<InMemoryPrefStore>();base::DictValue choices;choices.Set("locale-list",base::DictValue().Set("enabled",false));
  startup->SetValue(names::kAdBlockRegionalFilters,base::Value(std::move(choices)),0);
  auto owner=MakeInput(StorageMode::kStateless,startup);ASSERT_TRUE(owner);
  brave_shields::AdBlockFiltersProviderManager providers;brave_shields::AdBlockFilterListCatalogProvider catalog(nullptr);brave_shields::AdBlockListP3A metrics(owner->prefs.get());
  Manager manager(owner->prefs.get(),&providers,"en-US",nullptr,&catalog,&metrics);const auto initial=providers.GetProviders(false).size();manager.SetFilterListCatalog(Catalog());
  EXPECT_FALSE(Listed(manager,"locale-list"));EXPECT_TRUE(Listed(manager,"default-list"));EXPECT_EQ(initial+1,providers.GetProviders(false).size());
  EXPECT_FALSE(manager.EnableFilterList("locale-list",true));EXPECT_FALSE(Listed(manager,"locale-list"));EXPECT_TRUE(owner->input->GetValues().empty());
}
TEST_F(AdBlockRegionalTest, WritableChoiceUpdatesProjectionAndRetainsExplicitDisableAcrossReset) {
  auto owner=MakeInput(StorageMode::kMemory);ASSERT_TRUE(owner);
  brave_shields::AdBlockFiltersProviderManager providers;brave_shields::AdBlockFilterListCatalogProvider catalog(nullptr);brave_shields::AdBlockListP3A metrics(owner->prefs.get());
  Manager manager(owner->prefs.get(),&providers,"en-US",nullptr,&catalog,&metrics);manager.SetFilterListCatalog(Catalog());
  EXPECT_TRUE(Listed(manager,"locale-list"));EXPECT_TRUE(owner->prefs->GetBoolean(names::kAdBlockCheckedAllDefaultRegions));
  auto count=providers.GetProviders(false).size();EXPECT_TRUE(manager.EnableFilterList("locale-list",false));EXPECT_FALSE(Listed(manager,"locale-list"));EXPECT_EQ(count-1,providers.GetProviders(false).size());
  manager.ResetProviders();EXPECT_FALSE(Listed(manager,"locale-list"));
  EXPECT_TRUE(manager.EnableFilterList("optional-list",true));EXPECT_TRUE(Listed(manager,"optional-list"));
  EXPECT_TRUE(owner->prefs->GetDict(names::kAdBlockRegionalFilters).FindDict("optional-list")->FindBool("enabled").value());
  task_environment()->RunUntilIdle();
}
TEST_F(AdBlockRegionalTest, ReentrantReplacementCannotProjectOrAcknowledgeObsoleteChoice) {
  auto owner=MakeInput(StorageMode::kMemory);ASSERT_TRUE(owner);owner->prefs->SetBoolean(names::kAdBlockCheckedAllDefaultRegions,true);
  brave_shields::AdBlockFiltersProviderManager providers;brave_shields::AdBlockFilterListCatalogProvider catalog(nullptr);brave_shields::AdBlockListP3A metrics(owner->prefs.get());
  Manager manager(owner->prefs.get(),&providers,"en-US",nullptr,&catalog,&metrics);manager.SetFilterListCatalog(Catalog());const auto count=providers.GetProviders(false).size();
  bool replaced=false;PrefChangeRegistrar observer;observer.Init(owner->prefs.get());observer.Add(names::kAdBlockRegionalFilters,base::BindLambdaForTesting([&] {
    if(replaced)return;replaced=true;ScopedDictPrefUpdate update(owner->prefs.get(),names::kAdBlockRegionalFilters);update->Set("optional-list",base::DictValue().Set("enabled",false));
  }));
  EXPECT_FALSE(manager.EnableFilterList("optional-list",true));EXPECT_TRUE(replaced);EXPECT_FALSE(Listed(manager,"optional-list"));EXPECT_EQ(count,providers.GetProviders(false).size());
  task_environment()->RunUntilIdle();
}
}  // namespace
}  // namespace alloy::site_records
