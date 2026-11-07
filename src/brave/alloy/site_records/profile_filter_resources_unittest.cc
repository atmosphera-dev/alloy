// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/testing_pref_service.h"
#include "components/prefs/pref_registry_simple.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::site_records {
namespace {
using Provider = brave_shields::AdBlockCustomResourceProvider;
class PublicResources : public brave_shields::AdBlockResourceProvider {
 public:
  void LoadResources(base::OnceCallback<void(brave_shields::AdblockResourceStorageBox)> done) override {
    std::move(done).Run(adblock::new_empty_resource_storage());
  }
};
base::Value Resource(const char* name) {
  base::DictValue value; value.Set("name", name); value.Set("content", "YWxlcnQoMSk7");
  base::DictValue kind; kind.Set("mime", "application/javascript"); value.Set("kind", std::move(kind));
  return base::Value(std::move(value));
}
SecretKey ResourceKey() { ObjectId bytes{}; bytes.fill(0x75); return SecretKey(bytes); }
class ProfileFilterResourcesTest : public ChromeRenderViewHostTestHarness {
 protected:
  scoped_refptr<ProfileFilterPreferences> Load(std::shared_ptr<ProfileFilterStorage> storage) {
    base::test::TestFuture<FileStatus, scoped_refptr<ProfileFilterPreferences>> done;
    ProfileFilterPreferences::Load(storage, base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindLambdaForTesting([&](FileStatus status) { errors.push_back(status); }), done.GetCallback());
    task_environment()->RunUntilIdle();
    if (!done.IsReady()) { ADD_FAILURE(); return nullptr; }
    EXPECT_EQ(FileStatus::kOk, done.Get<0>()); return done.Get<1>();
  }
  std::vector<FileStatus> errors;
};
TEST_F(ProfileFilterResourcesTest, NativeConcurrentMutationsPersistAndLoadPrivateResourceStorage) {
  base::ScopedTempDir temp; ASSERT_TRUE(temp.CreateUniqueTempDir()); auto path = temp.GetPath().AppendASCII("profile");
  std::shared_ptr<RecordFileSystem> files; ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path, ResourceKey(), {}, &files));
  std::shared_ptr<ProfileFilterStorage> storage; ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files, files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  auto owner = Load(storage); ASSERT_TRUE(owner);
  TestingPrefServiceSimple dev; dev.registry()->RegisterBooleanPref(brave_shields::prefs::kAdBlockDeveloperMode, true);
  {
    Provider provider(owner->ResourceBackend(), std::make_unique<PublicResources>());
    base::test::TestFuture<Provider::ErrorCode> first, second;
    provider.AddResource(&dev, Resource("user-first.js"), first.GetCallback());
    provider.AddResource(&dev, Resource("user-second.js"), second.GetCallback());
    task_environment()->RunUntilIdle(); ASSERT_TRUE(first.IsReady() && second.IsReady());
    EXPECT_EQ(Provider::ErrorCode::kOk, first.Get()); EXPECT_EQ(Provider::ErrorCode::kOk, second.Get());
    base::test::TestFuture<Provider::ErrorCode, base::Value> read;
    provider.GetCustomResourcesWithStatus(read.GetCallback()); task_environment()->RunUntilIdle();
    ASSERT_TRUE(read.IsReady()); EXPECT_EQ(Provider::ErrorCode::kOk, read.Get<0>()); ASSERT_EQ(2u, read.Get<1>().GetList().size());
    base::test::TestFuture<brave_shields::AdblockResourceStorageBox> loaded;
    provider.LoadResources(loaded.GetCallback()); task_environment()->RunUntilIdle(); ASSERT_TRUE(loaded.IsReady());
    EXPECT_TRUE(adblock::has_resource_for_testing(*loaded.Get(), std::string("user-first.js")));
    bool closed = false; owner->Close(base::BindLambdaForTesting([&](FileStatus status) { EXPECT_EQ(FileStatus::kOk, status); closed = true; }));
    task_environment()->RunUntilIdle(); EXPECT_TRUE(closed);
    base::test::TestFuture<Provider::ErrorCode, base::Value> denied;
    provider.GetCustomResourcesWithStatus(denied.GetCallback()); ASSERT_TRUE(denied.IsReady()); EXPECT_EQ(Provider::ErrorCode::kCancelled, denied.Get<0>());
  }
  owner.reset(); storage.reset(); ASSERT_EQ(FileStatus::kOk, files->Lock()); files.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path, ResourceKey(), &files));
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files, files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  owner = Load(storage); ASSERT_TRUE(owner);
  Provider reopened(owner->ResourceBackend(), std::make_unique<PublicResources>());
  base::test::TestFuture<Provider::ErrorCode, base::Value> read;
  reopened.GetCustomResourcesWithStatus(read.GetCallback()); task_environment()->RunUntilIdle();
  ASSERT_TRUE(read.IsReady()); EXPECT_EQ(Provider::ErrorCode::kOk, read.Get<0>()); EXPECT_EQ(2u, read.Get<1>().GetList().size()); EXPECT_TRUE(errors.empty());
}
TEST_F(ProfileFilterResourcesTest, StatelessResourceMutationHasExplicitNativeRefusal) {
  RecordCapabilityIssuer issuer(base::UnguessableToken::Create(),base::UnguessableToken::Create(),base::UnguessableToken::Create(),StorageMode::kStateless);
  std::shared_ptr<ProfileFilterStorage> storage; ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(nullptr, issuer.Issue(StorePurpose::kProfileCustomFilters), &storage));
  auto owner = Load(storage); ASSERT_TRUE(owner);
  Provider provider(owner->ResourceBackend(), std::make_unique<PublicResources>());
  TestingPrefServiceSimple dev; dev.registry()->RegisterBooleanPref(brave_shields::prefs::kAdBlockDeveloperMode, true);
  base::test::TestFuture<Provider::ErrorCode> done; provider.AddResource(&dev, Resource("user-forbidden.js"), done.GetCallback());
  ASSERT_TRUE(done.IsReady()); EXPECT_EQ(Provider::ErrorCode::kReadOnly, done.Get());
  base::test::TestFuture<Provider::ErrorCode, base::Value> read;
  provider.GetCustomResourcesWithStatus(read.GetCallback()); task_environment()->RunUntilIdle();
  ASSERT_TRUE(read.IsReady()); EXPECT_EQ(Provider::ErrorCode::kOk, read.Get<0>()); EXPECT_TRUE(read.Get<1>().GetList().empty()); EXPECT_TRUE(errors.empty());
}
}  // namespace
}  // namespace alloy::site_records
