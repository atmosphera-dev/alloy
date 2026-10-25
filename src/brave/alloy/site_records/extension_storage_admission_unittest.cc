// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "chrome/browser/extensions/chrome_extension_system.h"
#include "chrome/browser/extensions/chrome_extension_system_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/value_store/value_store_factory.h"
#include "extensions/browser/api/storage/session_storage_manager.h"
#include "extensions/browser/api/storage/storage_frontend.h"
#include "extensions/browser/api/storage/storage_utils.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/event_router_factory.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_system.h"
#include "extensions/common/extension_builder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

TEST_F(ChromeRenderViewHostTestHarness,
       StatelessExtensionStorageRejectsValuesAndAccessMetadata) {
  using Frontend = extensions::StorageFrontend;
  using Area = extensions::StorageAreaNamespace;
  using Access = extensions::api::storage::AccessLevel;
  const auto extension = extensions::ExtensionBuilder("storage admission")
                             .SetID("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
                             .Build();
  auto* parent_manager =
      extensions::SessionStorageManager::GetForBrowserContext(profile());
  ASSERT_NE(parent_manager, nullptr);
  std::map<std::string, base::Value> parent_values;
  parent_values.emplace("parent", base::Value("parent-only"));
  std::vector<extensions::SessionStorageManager::ValueChange> changes;
  std::string error;
  ASSERT_TRUE(parent_manager->Set(extension->id(), std::move(parent_values),
                                  changes, &error));
  ASSERT_NE(parent_manager->Get(extension->id(), "parent"), nullptr);
  EXPECT_EQ(parent_manager->Get(extension->id(), "parent")->GetString(),
            "parent-only");

  for (auto mode : {NetworkState::kKeep, NetworkState::kCut}) {
    auto* child = TestingProfile::Builder().BuildOffTheRecord(
        profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(mode));
    extensions::ChromeExtensionSystemFactory::GetInstance()
        ->SetTestingFactoryAndUse(
            child, base::BindRepeating([](content::BrowserContext* context)
                                           -> std::unique_ptr<KeyedService> {
              return std::make_unique<extensions::ChromeExtensionSystem>(
                  Profile::FromBrowserContext(context));
            }));
    auto* system = extensions::ExtensionSystem::Get(child);
    ASSERT_NE(system, nullptr);
    extensions::EventRouterFactory::GetInstance()->SetTestingFactoryAndUse(
        child, base::BindRepeating([](content::BrowserContext* context)
                                       -> std::unique_ptr<KeyedService> {
          return std::make_unique<extensions::EventRouter>(
              context, extensions::ExtensionPrefs::Get(context));
        }));
    auto storage = Frontend::CreateForTesting(system->store_factory(), child);
    const auto before =
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS);
    for (auto area :
         {Area::kSession, Area::kLocal, Area::kSync, Area::kManaged}) {
      SCOPED_TRACE(extensions::StorageAreaToString(area));
      base::test::TestFuture<Frontend::ResultStatus> written;
      storage->Set(extension, area,
                   base::DictValue().Set("private", "must-not-be-retained"),
                   written.GetCallback());
      ASSERT_TRUE(written.Wait());
      ASSERT_FALSE(written.Get().success);
      EXPECT_TRUE(written.Get().error.has_value());
      base::test::TestFuture<Frontend::GetResult> read;
      storage->GetValues(extension, area, std::nullopt, read.GetCallback());
      ASSERT_TRUE(read.Wait());
      EXPECT_FALSE(read.Get().status.success);
      EXPECT_FALSE(read.Get().data.has_value());
      base::test::TestFuture<Frontend::GetKeysResult> keys;
      storage->GetKeys(extension, area, keys.GetCallback());
      ASSERT_TRUE(keys.Wait());
      EXPECT_FALSE(keys.Get().status.success);
      EXPECT_FALSE(keys.Get().data.has_value());
      base::test::TestFuture<size_t> bytes;
      storage->GetBytesInUse(extension, area, std::nullopt,
                             bytes.GetCallback());
      ASSERT_TRUE(bytes.Wait());
      EXPECT_EQ(bytes.Get(), 0u);
      base::test::TestFuture<Frontend::ResultStatus> removed;
      storage->Remove(extension, area, {"private"}, removed.GetCallback());
      ASSERT_TRUE(removed.Wait());
      EXPECT_FALSE(removed.Get().success);
      base::test::TestFuture<Frontend::ResultStatus> cleared;
      storage->Clear(extension, area, cleared.GetCallback());
      ASSERT_TRUE(cleared.Wait());
      EXPECT_FALSE(cleared.Get().success);
      extensions::storage_utils::SetAccessLevelForArea(
          extension->id(), *child, area, Access::kTrustedAndUntrustedContexts);
    }
    EXPECT_EQ(
        child->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS),
        before);
    EXPECT_EQ(extensions::SessionStorageManager::GetForBrowserContext(child),
              nullptr);
    EXPECT_EQ(parent_manager->Get(extension->id(), "private"), nullptr);
    EXPECT_EQ(parent_manager->Get(extension->id(), "parent")->GetString(),
              "parent-only");
  }
}

}  // namespace
}  // namespace alloy::site_records
