// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <thread>
#include <functional>
#include <atomic>

#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/run_loop.h"
#include "base/threading/thread_restrictions.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include "brave/alloy/site_records/native_pref_view.h"
#include "brave/alloy/site_records/site_profile_configuration.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include "base/test/test_future.h"
#include "components/prefs/pref_registry_simple.h"
#include "cef/include/alloy/alloy_profile.h"
#include "cef/include/cef_api_hash.h"
#include "cef/libcef/browser/alloy/profile_settings.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "content/public/test/browser_task_environment.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/browser/profiles/profile_key.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/content/refcounted_browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/keyed_service/core/refcounted_keyed_service.h"
#include "components/keyed_service/core/service_creation_gate.h"
#include "components/keyed_service/core/simple_dependency_manager.h"
#include "components/keyed_service/core/simple_keyed_service_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
constexpr char kNativeSetting[] = "alloy.native_lock_boundary";

struct ConstructionCount { int created = 0; int destroyed = 0; };
class CountedService final : public KeyedService {
 public:
  explicit CountedService(ConstructionCount* count) : count_(count) { ++count_->created; }
  ~CountedService() override { ++count_->destroyed; }
 private:
  ConstructionCount* const count_;
};
class CountedRefService final : public RefcountedKeyedService {
 public:
  explicit CountedRefService(ConstructionCount* count)
      : RefcountedKeyedService(base::SequencedTaskRunner::GetCurrentDefault()), count_(count) { ++count_->created; }
  void ShutdownOnUIThread() override {}
 private:
  ~CountedRefService() override { ++count_->destroyed; }
  ConstructionCount* const count_;
};
class CountedBrowserFactory final : public BrowserContextKeyedServiceFactory {
 public:
  CountedBrowserFactory(const char* name, ConstructionCount* count)
      : BrowserContextKeyedServiceFactory(name, BrowserContextDependencyManager::GetInstance()), count_(count) {}
  KeyedService* Get(content::BrowserContext* context) { return GetServiceForBrowserContext(context, true); }
 private:
  bool ServiceIsNULLWhileTesting() const override { return false; }
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(content::BrowserContext*) const override {
    return std::make_unique<CountedService>(count_);
  }
  ConstructionCount* const count_;
};
class CountedRefFactory final : public RefcountedBrowserContextKeyedServiceFactory {
 public:
  explicit CountedRefFactory(ConstructionCount* count)
      : RefcountedBrowserContextKeyedServiceFactory("AlloyBoundedRefCreation", BrowserContextDependencyManager::GetInstance()), count_(count) {}
  scoped_refptr<RefcountedKeyedService> Get(content::BrowserContext* context) { return GetServiceForBrowserContext(context, true); }
 private:
  bool ServiceIsNULLWhileTesting() const override { return false; }
  scoped_refptr<RefcountedKeyedService> BuildServiceInstanceFor(content::BrowserContext*) const override {
    return base::MakeRefCounted<CountedRefService>(count_);
  }
  ConstructionCount* const count_;
};
class CountedSimpleFactory final : public SimpleKeyedServiceFactory {
 public:
  explicit CountedSimpleFactory(ConstructionCount* count)
      : SimpleKeyedServiceFactory("AlloyBoundedSimpleCreation", SimpleDependencyManager::GetInstance()), count_(count) {}
  KeyedService* Get(SimpleFactoryKey* key) { return GetServiceForKey(key, true); }
 private:
  bool ServiceIsNULLWhileTesting() const override { return false; }
  std::unique_ptr<KeyedService> BuildServiceInstanceFor(SimpleFactoryKey*) const override {
    return std::make_unique<CountedService>(count_);
  }
  ConstructionCount* const count_;
};

class LockReply final : public AlloyProfileLockCallback {
 public:
  explicit LockReply(std::function<void(alloy_site_settings_result_t)> done)
      : done_(std::move(done)) {}
  void OnComplete(alloy_site_settings_result_t result) override { done_(result); }
 private:
  const std::function<void(alloy_site_settings_result_t)> done_;
  IMPLEMENT_REFCOUNTING(LockReply);
};

class BeforeMutableStore final : public PersistentPrefStore {
 public:
  BeforeMutableStore(scoped_refptr<PersistentPrefStore> source, std::function<void()> before)
      : source_(std::move(source)), before_(std::move(before)) {}
  bool GetValue(std::string_view key, const base::Value** out) const override { return source_->GetValue(key, out); }
  base::DictValue GetValues() const override { return source_->GetValues(); }
  void AddObserver(PrefStore::Observer* observer) override { source_->AddObserver(observer); }
  void RemoveObserver(PrefStore::Observer* observer) override { source_->RemoveObserver(observer); }
  bool HasObservers() const override { return source_->HasObservers(); }
  bool IsInitializationComplete() const override { return source_->IsInitializationComplete(); }
  bool GetMutableValue(std::string_view key, base::Value** out) override {
    if (before_) { auto before = std::move(before_); before(); }
    return source_->GetMutableValue(key, out);
  }
  void SetValue(std::string_view key, base::Value value, uint32_t flags) override { source_->SetValue(key, std::move(value), flags); }
  void SetValueSilently(std::string_view key, base::Value value, uint32_t flags) override { source_->SetValueSilently(key, std::move(value), flags); }
  void ReportValueChanged(std::string_view key, uint32_t flags) override { source_->ReportValueChanged(key, flags); }
  void RemoveValue(std::string_view key, uint32_t flags) override { source_->RemoveValue(key, flags); }
  void RemoveValuesByPrefixSilently(std::string_view prefix) override { source_->RemoveValuesByPrefixSilently(prefix); }
  bool ReadOnly() const override { return source_->ReadOnly(); }
  PrefReadError GetReadError() const override { return source_->GetReadError(); }
  PrefReadError ReadPrefs() override { return source_->ReadPrefs(); }
  void ReadPrefsAsync(ReadErrorDelegate* delegate) override { source_->ReadPrefsAsync(delegate); }
  void CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) override { source_->CommitPendingWrite(std::move(reply), std::move(done)); }
  void SchedulePendingLossyWrites() override { source_->SchedulePendingLossyWrites(); }
  void OnStoreDeletionFromDisk() override { source_->OnStoreDeletionFromDisk(); }
  bool HasReadErrorDelegate() const override { return source_->HasReadErrorDelegate(); }
 private:
  ~BeforeMutableStore() override = default;
  const scoped_refptr<PersistentPrefStore> source_;
  std::function<void()> before_;
};

class ProfileSettingsLifecycleTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(nullptr, cef_api_hash(CEF_API_VERSION, 0));
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    const auto path = temp_.GetPath().AppendASCII("encrypted").AsUTF8Unsafe();
    std::thread create([&] { profile_ = AlloyCreateProfile(path, "lifecycle-password"); });
    create.join();
    ASSERT_TRUE(profile_);
    OpenProjection();
  }
  void TearDown() override {
    prefs_.reset(); store_.reset(); tasks_.RunUntilIdle();
    std::thread release([&] { profile_ = nullptr; }); release.join();
    tasks_.RunUntilIdle();
  }
  void OpenProjection() {
    store_ = alloy::GetProfilePrefStore(profile_);
    ASSERT_TRUE(store_);
    bool ready = false;
    store_->InitializeAsync(base::BindLambdaForTesting([&](FileStatus status) {
      EXPECT_EQ(FileStatus::kOk, status); ready = true;
    }));
    tasks_.RunUntilIdle(); ASSERT_TRUE(ready);
    auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
    registry->RegisterStringPref(kNativeSetting, "");
    PrefServiceFactory factory; factory.set_user_prefs(store_);
    prefs_ = factory.Create(registry);
  }
  content::BrowserTaskEnvironment tasks_;
  base::ScopedTempDir temp_;
  CefRefPtr<AlloyProfile> profile_;
  scoped_refptr<SitePrefStore> store_;
  std::unique_ptr<PrefService> prefs_;
};

TEST(ProfileAutoLockApiTest, RuntimeAdoptionPreservesCoreOnlyBlockingLockWithoutUiPumping) {
  content::BrowserTaskEnvironment tasks;
  ASSERT_NE(nullptr, cef_api_hash(CEF_API_VERSION, 0));
  base::ScopedTempDir temp; ASSERT_TRUE(temp.CreateUniqueTempDir());
  CefRefPtr<AlloyProfile> profile;
  const auto path = temp.GetPath().AppendASCII("standalone").AsUTF8Unsafe();
  std::thread create([&] { profile = AlloyCreateProfile(path, "standalone-password"); });
  create.join(); ASSERT_TRUE(profile);
  alloy::InitializeProfileAutoLock();
  base::WaitableEvent completed;
  bool locked = false;
  std::thread lock([&] { locked = profile->Lock(); completed.Signal(); });
  const bool without_pumping = completed.TimedWait(base::Seconds(5));
  // If the regression is present, drain its queued UI lock before joining so
  // the test records a failure rather than hanging the entire native suite.
  if (!without_pumping) tasks.RunUntilIdle();
  lock.join(); EXPECT_TRUE(without_pumping); ASSERT_TRUE(locked);
  EXPECT_EQ(ALLOY_PROFILE_LOCKED, profile->GetState());
  bool unlocked = false;
  std::thread unlock([&] { unlocked = profile->UnlockWithPassword("standalone-password"); });
  unlock.join(); ASSERT_TRUE(unlocked);
  auto fresh = alloy::GetProfilePrefStore(profile); ASSERT_TRUE(fresh);
  tasks.RunUntilIdle();
  EXPECT_EQ(ALLOY_PROFILE_UNLOCKED, profile->GetState());
  EXPECT_EQ(fresh, alloy::GetProfilePrefStore(profile));
  fresh.reset();
  std::thread release([&] { profile = nullptr; }); release.join();
  tasks.RunUntilIdle();
}

TEST_F(ProfileSettingsLifecycleTest, NativeEditBeforeLockSurvivesWithoutOwnerPumpingBeforeRevocation) {
  bool locked = false;
  const auto cache = store_->GetNativeCacheForTesting();
  base::WaitableEvent revoked;
  {
    base::ScopedDisallowBlocking restricted;
    prefs_->SetString(kNativeSetting, "completed-native-value");
    ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t result) {
      EXPECT_EQ(ALLOY_SETTINGS_OK, result);
      EXPECT_FALSE(cache); locked = true;
    })));
    store_->CommitPendingWrite({}, base::BindLambdaForTesting([&] { revoked.Signal(); }));
  }
  // The I/O fence is not a key-revocation fence. Until UI/native teardown can
  // run, existing consumers retain admitted storage for their final writes.
  ASSERT_TRUE(revoked.TimedWait(base::Seconds(5)));
  EXPECT_FALSE(store_->ReadOnly()); EXPECT_FALSE(locked); EXPECT_TRUE(cache);
  EXPECT_EQ(ALLOY_PROFILE_TRANSITION, profile_->GetState());
  tasks_.RunUntilIdle();
  ASSERT_TRUE(locked); EXPECT_EQ(ALLOY_PROFILE_LOCKED, profile_->GetState());
  prefs_.reset(); store_.reset(); tasks_.RunUntilIdle();
  bool unlocked = false;
  std::thread unlock([&] { unlocked = profile_->UnlockWithPassword("lifecycle-password"); });
  unlock.join(); ASSERT_TRUE(unlocked);
  OpenProjection();
  EXPECT_EQ("completed-native-value", prefs_->GetString(kNativeSetting));
}

TEST_F(ProfileSettingsLifecycleTest, KeyRevocationDuringNativeConstructionMustNotCrash) {
  prefs_->SetString(kNativeSetting, "native-private-marker");
  auto view = store_->CreateNativeView(); ASSERT_TRUE(view);
  const auto cache = store_->GetNativeCacheForTesting();
  std::unique_ptr<PrefService> native_prefs;
  scoped_refptr<HostContentSettingsMap> map;
  bool completed = false, torn_down = false;
  ASSERT_TRUE(view->BeginConstruction(base::BindLambdaForTesting([&] {
    ASSERT_TRUE(map); map->ShutdownOnUIThread(); map = nullptr;
    native_prefs.reset(); torn_down = true;
    view->FinishTeardown();
  })));
  auto adapter = base::MakeRefCounted<BeforeMutableStore>(view, [&] {
    EXPECT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t result) {
      EXPECT_EQ(ALLOY_SETTINGS_OK, result);
      EXPECT_TRUE(torn_down); EXPECT_FALSE(map); EXPECT_FALSE(native_prefs);
      EXPECT_FALSE(cache); EXPECT_TRUE(view->GetValues().empty());
      EXPECT_TRUE(prefs_->GetString(kNativeSetting).empty());
      EXPECT_EQ(nullptr, alloy::GetProfilePrefStore(profile_)); completed = true;
    })));
    base::WaitableEvent revoked;
    store_->CommitPendingWrite({}, base::BindLambdaForTesting([&] { revoked.Signal(); }));
    EXPECT_TRUE(revoked.TimedWait(base::Seconds(5)));
    EXPECT_FALSE(store_->ReadOnly()); EXPECT_FALSE(completed);
    const base::Value* external = nullptr;
    EXPECT_TRUE(store_->GetValue(kNativeSetting, &external));
    EXPECT_TRUE(view->CanStartServiceConstruction());
    EXPECT_EQ(nullptr, alloy::GetProfilePrefStore(profile_));
  });
  auto registry = base::MakeRefCounted<user_prefs::PrefRegistrySyncable>();
  HostContentSettingsMap::RegisterProfilePrefs(registry.get());
  PrefServiceFactory factory; factory.set_user_prefs(adapter);
  native_prefs = factory.Create(registry);
  map = base::MakeRefCounted<HostContentSettingsMap>(native_prefs.get(), false, false, false, false);
  view->ConstructionFinished();
  tasks_.RunUntilIdle();
  EXPECT_TRUE(completed); EXPECT_TRUE(torn_down);
  EXPECT_EQ(ALLOY_PROFILE_LOCKED, profile_->GetState());
  EXPECT_TRUE(store_->ReadOnly());
  EXPECT_TRUE(view->GetValues().empty());
  bool unlocked = false;
  std::thread unlock([&] { unlocked = profile_->UnlockWithPassword("lifecycle-password"); });
  unlock.join(); ASSERT_TRUE(unlocked); OpenProjection();
  EXPECT_EQ("native-private-marker", prefs_->GetString(kNativeSetting));
}

TEST_F(ProfileSettingsLifecycleTest, LockCallbackWaitsForEveryNativeViewAndRejectsPreparedConstruction) {
  prefs_->SetString(kNativeSetting, "bounded-private-cache");
  auto active = store_->CreateNativeView(); auto prepared = store_->CreateNativeView();
  ASSERT_TRUE(active); ASSERT_TRUE(prepared);
  bool teardown_requested = false, completed = false;
  ASSERT_TRUE(active->BeginConstruction(base::BindLambdaForTesting([&] { teardown_requested = true; })));
  active->ConstructionFinished();
  const auto cache = store_->GetNativeCacheForTesting();
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t result) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, result); EXPECT_FALSE(cache);
    EXPECT_TRUE(active->GetValues().empty()); EXPECT_TRUE(prepared->GetValues().empty());
    completed = true;
  })));
  tasks_.RunUntilIdle();
  EXPECT_TRUE(teardown_requested); EXPECT_FALSE(completed); EXPECT_TRUE(cache);
  EXPECT_EQ(ALLOY_PROFILE_TRANSITION, profile_->GetState());
  EXPECT_FALSE(prepared->BeginConstruction(base::BindOnce([] {})));
  EXPECT_FALSE(store_->CreateNativeView());
  EXPECT_FALSE(profile_->LockAsync(new LockReply([](alloy_site_settings_result_t) { ADD_FAILURE(); })));
  bool unlocked = true;
  std::thread unlock([&] { unlocked = profile_->UnlockWithPassword("lifecycle-password"); });
  unlock.join(); EXPECT_FALSE(unlocked);
  active->FinishTeardown(); tasks_.RunUntilIdle();
  EXPECT_TRUE(completed); EXPECT_FALSE(cache);
  const base::Value* value = nullptr;
  EXPECT_FALSE(store_->GetValue(kNativeSetting, &value));
  EXPECT_FALSE(active->GetValue(kNativeSetting, &value));
  EXPECT_EQ(nullptr, alloy::GetProfilePrefStore(profile_));
}

TEST_F(ProfileSettingsLifecycleTest, RevokedGenerationCannotStartNativeFactoryConstructors) {
  ConstructionCount warm_count, late_count, ref_count, simple_count;
  CountedBrowserFactory warm("AlloyBoundedWarmCreation", &warm_count);
  CountedBrowserFactory late("AlloyBoundedLateCreation", &late_count);
  CountedRefFactory ref(&ref_count);
  CountedSimpleFactory simple(&simple_count);
  auto native = std::make_unique<TestingProfile>();
  auto view = store_->CreateNativeView(); ASSERT_TRUE(view);
  bool teardown_requested = false, complete = false;
  ASSERT_TRUE(view->BeginConstruction(base::BindLambdaForTesting([&] { teardown_requested = true; })));
  view->ConstructionFinished();
  auto admitted = base::BindRepeating([](scoped_refptr<NativePrefView> view) {
    return view->CanStartServiceConstruction();
  }, view);
  ServiceCreationGate::Attach(*native, admitted);
  native->GetProfileKey()->SetServiceCreationGate(admitted);
  auto* existing = warm.Get(native.get()); ASSERT_TRUE(existing);
  ASSERT_EQ(1, warm_count.created);
  const auto cache = store_->GetNativeCacheForTesting();
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t result) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, result); EXPECT_FALSE(native);
    EXPECT_EQ(1, warm_count.destroyed); EXPECT_FALSE(cache); complete = true;
  })));
  tasks_.RunUntilIdle(); ASSERT_TRUE(teardown_requested); EXPECT_FALSE(complete);
  // Existing native working objects may finish teardown; all three native
  // factory families must refuse to start another actual service constructor.
  EXPECT_EQ(existing, warm.Get(native.get()));
  EXPECT_EQ(nullptr, late.Get(native.get()));
  EXPECT_FALSE(ref.Get(native.get()));
  EXPECT_EQ(nullptr, simple.Get(native->GetProfileKey()));
  EXPECT_EQ(0, late_count.created); EXPECT_EQ(0, ref_count.created); EXPECT_EQ(0, simple_count.created);
  native.reset();
  view->FinishTeardown(); tasks_.RunUntilIdle();
  EXPECT_TRUE(complete); EXPECT_FALSE(cache);
}

void PrepareFilterInput(CefRefPtr<AlloyProfile> profile,
    content::BrowserTaskEnvironment& tasks, std::unique_ptr<NativeProfileConfiguration>* out) {
  base::test::TestFuture<alloy_site_settings_result_t, std::unique_ptr<NativeProfileConfiguration>> reply;
  alloy::PrepareNativeProfileConfiguration(profile, reply.GetCallback());
  tasks.RunUntilIdle();
  if (!reply.IsReady()) { ADD_FAILURE() << "native filter input preparation did not complete"; return; }
  auto [status, input] = reply.Take();
  EXPECT_EQ(ALLOY_SETTINGS_OK, status); *out = std::move(input);
}
std::unique_ptr<PrefService> NativeFilterPrefs(scoped_refptr<ProfileFilterPreferences> prefs) {
  auto registry = base::MakeRefCounted<PrefRegistrySimple>();
  brave_shields::RegisterPrefsForAdBlockService(registry.get());
  PrefServiceFactory factory; factory.set_user_prefs(prefs); return factory.Create(registry);
}

TEST_F(ProfileSettingsLifecycleTest, FilterInputSharesProfileAndLockDrainsNativeEditBeforeUnlock) {
  std::unique_ptr<NativeProfileConfiguration> input;
  PrepareFilterInput(profile_, tasks_, &input); ASSERT_TRUE(input); ASSERT_TRUE(input->BeginConstruction());
  auto native = std::make_unique<TestingProfile>(); input->Attach(*native);
  auto filters = ProfileFilterPreferences::Get(*native); ASSERT_TRUE(filters);
  auto native_prefs = NativeFilterPrefs(filters);
  brave_shields::AdBlockFiltersProviderManager manager;
  brave_shields::AdBlockCustomFiltersProvider producer(native_prefs.get(), &manager);
  ASSERT_TRUE(producer.UpdateCustomFilters("||cef-filter-generation.invalid^"));
  std::unique_ptr<NativeProfileConfiguration> unused;
  PrepareFilterInput(profile_, tasks_, &unused); ASSERT_TRUE(unused); unused.reset();
  EXPECT_EQ(filters, ProfileFilterPreferences::Get(*native));
  ASSERT_TRUE(producer.UpdateCustomFilters("||cef-filter-completed-native-edit.invalid^"));
  bool retired = false, locked = false;
  input->SetTeardownHandler(base::BindLambdaForTesting([&] {
    native.reset(); input.reset(); retired = true;
  }));
  input->ConstructionFinished();
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t status) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, status); EXPECT_TRUE(retired); EXPECT_TRUE(filters->ReadOnly());
    EXPECT_TRUE(filters->GetValues().empty()); EXPECT_TRUE(producer.GetCustomFilters().empty()); locked = true;
  })));
  tasks_.RunUntilIdle(); ASSERT_TRUE(locked); EXPECT_FALSE(producer.UpdateCustomFilters("late"));
  bool unlocked = false;
  std::thread unlock([&] { unlocked = profile_->UnlockWithPassword("lifecycle-password"); });
  unlock.join(); ASSERT_TRUE(unlocked);
  PrepareFilterInput(profile_, tasks_, &input); ASSERT_TRUE(input); ASSERT_TRUE(input->BeginConstruction());
  native = std::make_unique<TestingProfile>(); input->Attach(*native);
  auto fresh = ProfileFilterPreferences::Get(*native); ASSERT_TRUE(fresh); EXPECT_NE(filters, fresh);
  auto fresh_prefs = NativeFilterPrefs(fresh);
  brave_shields::AdBlockFiltersProviderManager fresh_manager;
  brave_shields::AdBlockCustomFiltersProvider fresh_producer(fresh_prefs.get(), &fresh_manager);
  EXPECT_EQ("||cef-filter-completed-native-edit.invalid^", fresh_producer.GetCustomFilters());
  EXPECT_TRUE(producer.GetCustomFilters().empty()); EXPECT_FALSE(producer.UpdateCustomFilters("old-generation"));
  input->SetTeardownHandler(base::BindOnce([] {})); input->ConstructionFinished();
  native.reset(); input.reset(); tasks_.RunUntilIdle();
}

TEST_F(ProfileSettingsLifecycleTest, StartedNativeConstructionFinishesBeforeServiceGateCloses) {
  std::unique_ptr<NativeProfileConfiguration> input;
  PrepareFilterInput(profile_, tasks_, &input); ASSERT_TRUE(input); ASSERT_TRUE(input->BeginConstruction());
  auto native = std::make_unique<TestingProfile>();
  bool retired = false, locked = false;
  input->SetTeardownHandler(base::BindLambdaForTesting([&] { native.reset(); input.reset(); retired = true; }));
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t status) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, status); EXPECT_TRUE(retired); locked = true;
  })));
  base::WaitableEvent revoked;
  store_->CommitPendingWrite({}, base::BindLambdaForTesting([&] { revoked.Signal(); }));
  ASSERT_TRUE(revoked.TimedWait(base::Seconds(5))); EXPECT_FALSE(locked);
  input->Attach(*native);
  EXPECT_TRUE(ProfileFilterPreferences::Get(*native)); EXPECT_TRUE(input->admitted());
  EXPECT_FALSE(alloy::ProfileNavigationAllowed(*input));
  input->ConstructionFinished(); tasks_.RunUntilIdle(); EXPECT_TRUE(locked);
}

TEST_F(ProfileSettingsLifecycleTest, ProfileLockWipesFilterInputRetainedByPreparedConfiguration) {
  std::unique_ptr<NativeProfileConfiguration> active, prepared;
  PrepareFilterInput(profile_, tasks_, &active); ASSERT_TRUE(active); ASSERT_TRUE(active->BeginConstruction());
  auto native = std::make_unique<TestingProfile>(); active->Attach(*native);
  auto filters = ProfileFilterPreferences::Get(*native); ASSERT_TRUE(filters);
  auto native_prefs = NativeFilterPrefs(filters);
  brave_shields::AdBlockFiltersProviderManager manager;
  brave_shields::AdBlockCustomFiltersProvider producer(native_prefs.get(), &manager);
  ASSERT_TRUE(producer.UpdateCustomFilters("||prepared-filter-private.invalid^"));
  active->SetTeardownHandler(base::BindOnce([] {})); active->ConstructionFinished();
  native.reset(); active.reset(); tasks_.RunUntilIdle();
  EXPECT_TRUE(filters->available());
  PrepareFilterInput(profile_, tasks_, &prepared); ASSERT_TRUE(prepared);
  bool locked = false;
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t status) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, status); EXPECT_TRUE(filters->ReadOnly());
    EXPECT_TRUE(filters->GetValues().empty()); locked = true;
  })));
  tasks_.RunUntilIdle(); ASSERT_TRUE(locked);
  EXPECT_FALSE(prepared->BeginConstruction()); EXPECT_TRUE(producer.GetCustomFilters().empty());
  prepared.reset(); tasks_.RunUntilIdle();
}

TEST_F(ProfileSettingsLifecycleTest, NativeNamespaceSharesDurabilityIsolationAndLockLifetime) {
  std::unique_ptr<NativeProfileConfiguration> configuration;
  alloy::PrepareNativeProfileConfiguration(profile_, base::BindLambdaForTesting(
      [&](alloy_site_settings_result_t result, std::unique_ptr<NativeProfileConfiguration> input) {
        EXPECT_EQ(ALLOY_SETTINGS_OK, result); configuration = std::move(input);
      }));
  tasks_.RunUntilIdle(); ASSERT_TRUE(configuration);
  ASSERT_TRUE(configuration->BeginConstruction());
  auto family = SiteProfilePrefsForNamespace(*configuration, "alloy.family_link");
  auto other = SiteProfilePrefsForNamespace(*configuration, "alloy.other_owner");
  prefs_->SetString(kNativeSetting, "outside-namespace");
  family->SetValue("setting", base::Value("family-value"), 0);
  other->SetValue("setting", base::Value("other-value"), 0);
  auto values = family->GetValues();
  ASSERT_EQ(1u, values.size());
  ASSERT_TRUE(values.FindString("setting"));
  EXPECT_EQ("family-value", *values.FindString("setting"));
  const auto ui = std::this_thread::get_id();
  std::atomic<bool> backend_thread{false};
  bool replied = false;
  base::WaitableEvent committed;
  family->CommitPendingWrite(base::BindLambdaForTesting([&] { replied = true; }),
      base::BindLambdaForTesting([&] {
        backend_thread.store(std::this_thread::get_id() != ui); committed.Signal();
      }));
  ASSERT_TRUE(committed.TimedWait(base::Seconds(5)));
  EXPECT_TRUE(backend_thread.load());
  tasks_.RunUntilIdle(); EXPECT_TRUE(replied);
  bool torn_down = false, locked = false;
  configuration->SetTeardownHandler(base::BindLambdaForTesting([&] {
    family.reset(); other.reset(); configuration.reset(); torn_down = true;
  }));
  configuration->ConstructionFinished();
  const auto cache = store_->GetNativeCacheForTesting();
  ASSERT_TRUE(profile_->LockAsync(new LockReply([&](alloy_site_settings_result_t result) {
    EXPECT_EQ(ALLOY_SETTINGS_OK, result); EXPECT_TRUE(torn_down);
    EXPECT_FALSE(configuration); EXPECT_FALSE(cache); locked = true;
  })));
  tasks_.RunUntilIdle(); ASSERT_TRUE(locked);
  prefs_.reset(); store_.reset(); tasks_.RunUntilIdle();
  bool unlocked = false;
  std::thread unlock([&] { unlocked = profile_->UnlockWithPassword("lifecycle-password"); });
  unlock.join(); ASSERT_TRUE(unlocked); OpenProjection();
  const base::Value* value = nullptr;
  ASSERT_TRUE(store_->GetValue("alloy.family_link.setting", &value));
  EXPECT_EQ("family-value", value->GetString());
  ASSERT_TRUE(store_->GetValue("alloy.other_owner.setting", &value));
  EXPECT_EQ("other-value", value->GetString());
  EXPECT_EQ("outside-namespace", prefs_->GetString(kNativeSetting));
}
}  // namespace
}  // namespace alloy::site_records
