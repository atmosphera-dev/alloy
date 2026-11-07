// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_subscription_fetch.h"

#include <tuple>

#include "base/files/scoped_temp_dir.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service_factory.h"
#include "net/base/load_flags.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
using Provider = brave_shields::AdBlockSubscriptionFiltersProvider;
using Result = Provider::SourceResult;
using Manager = brave_shields::AdBlockSubscriptionServiceManager;

class ProfileSubscriptionFetchTest : public ChromeRenderViewHostTestHarness {
 protected:
  ProfileFilterPreferences::SubscriptionFetch Fetcher() {
    return MakeProfileSubscriptionFetch(factory.GetSafeWeakWrapper(),
        base::BindLambdaForTesting([this] { return admitted; }));
  }
  network::TestURLLoaderFactory factory;
  bool admitted = true;
};

TEST_F(ProfileSubscriptionFetchTest, NativeStreamPreservesBytesAboveStringCeilingAndRequestPolicy) {
  const GURL url("https://subscription.invalid/private-list");
  const std::string text(5 * 1024 * 1024 + 31, '!');
  base::test::TestFuture<Result, DATFileDataBuffer> done;
  auto cancel = Fetcher().Run(url, true, done.GetCallback());
  ASSERT_TRUE(factory.IsPending(url.spec()));
  auto* pending = factory.GetPendingRequest(0);
  ASSERT_TRUE(pending);
  EXPECT_EQ(network::mojom::CredentialsMode::kOmit, pending->request.credentials_mode);
  EXPECT_EQ(net::LOAD_DISABLE_CACHE, pending->request.load_flags);
  EXPECT_EQ(net::HIGHEST, pending->request.priority);
  EXPECT_EQ(128537722, pending->traffic_annotation.unique_id_hash_code);
  ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), text));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(done.IsReady());
  EXPECT_EQ(Result::kOk, done.Get<0>());
  EXPECT_EQ(text, std::string(done.Get<1>().begin(), done.Get<1>().end()));
  std::move(cancel).Run();  // Completion followed by retirement is idempotent.
}

TEST_F(ProfileSubscriptionFetchTest, OriginalAdmissionAndCancellationRetirePendingNetworkReply) {
  const GURL url("https://subscription.invalid/held");
  auto fetch = Fetcher();
  admitted = false;
  base::test::TestFuture<Result, DATFileDataBuffer> denied;
  auto denied_cancel = fetch.Run(url, false, denied.GetCallback());
  ASSERT_TRUE(denied.IsReady());
  EXPECT_EQ(Result::kCancelled, denied.Get<0>());
  EXPECT_EQ(0u, factory.total_requests());
  admitted = true;
  base::test::TestFuture<Result, DATFileDataBuffer> retired;
  auto retired_cancel = fetch.Run(url, false, retired.GetCallback());
  ASSERT_TRUE(factory.IsPending(url.spec()));
  admitted = false;
  ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), "private bytes"));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(retired.IsReady());
  EXPECT_EQ(Result::kCancelled, retired.Get<0>());
  EXPECT_TRUE(retired.Get<1>().empty());
  std::move(retired_cancel).Run();
  admitted = true;
  base::test::TestFuture<Result, DATFileDataBuffer> cancelled;
  auto cancel = fetch.Run(url, false, cancelled.GetCallback());
  ASSERT_TRUE(factory.IsPending(url.spec()));
  std::move(cancel).Run();
  ASSERT_TRUE(cancelled.IsReady());
  EXPECT_EQ(Result::kCancelled, cancelled.Get<0>());
  EXPECT_TRUE(cancelled.Get<1>().empty());
  EXPECT_FALSE(factory.IsPending(url.spec()));
}

TEST_F(ProfileSubscriptionFetchTest, StreamAcceptsExactly64MiB) {
  const GURL url("https://subscription.invalid/maximum");
  const std::string text(64u * 1024 * 1024, '!');
  base::test::TestFuture<Result, DATFileDataBuffer> done;
  auto cancel = Fetcher().Run(url, false, done.GetCallback());
  ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), text));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(done.IsReady());
  EXPECT_EQ(Result::kOk, done.Get<0>());
  EXPECT_EQ(text, std::string(done.Get<1>().begin(), done.Get<1>().end()));
  std::move(cancel).Run();
}

TEST_F(ProfileSubscriptionFetchTest, StreamRejectsOneByteAbove64MiBWithoutReturningBytes) {
  const GURL url("https://subscription.invalid/oversized");
  const std::string text(64u * 1024 * 1024 + 1, '!');
  base::test::TestFuture<Result, DATFileDataBuffer> done;
  auto cancel = Fetcher().Run(url, false, done.GetCallback());
  ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), text));
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(done.IsReady());
  EXPECT_EQ(Result::kError, done.Get<0>());
  EXPECT_TRUE(done.Get<1>().empty());
  EXPECT_FALSE(factory.IsPending(url.spec()));
  std::move(cancel).Run();
}

TEST_F(ProfileSubscriptionFetchTest, HttpFailureAndNonNetworkSourceCannotReturnSuccessfulPrivateBytes) {
  auto fetch = Fetcher();
  const GURL url("https://subscription.invalid/failure");
  factory.AddResponse(url.spec(), "not a filter list", net::HTTP_INTERNAL_SERVER_ERROR);
  base::test::TestFuture<Result, DATFileDataBuffer> failed;
  auto cancel = fetch.Run(url, false, failed.GetCallback());
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(failed.IsReady());
  EXPECT_EQ(Result::kError, failed.Get<0>());
  EXPECT_TRUE(failed.Get<1>().empty());
  base::test::TestFuture<Result, DATFileDataBuffer> file;
  auto file_cancel = fetch.Run(GURL("file:///private-list"), true, file.GetCallback());
  ASSERT_TRUE(file.IsReady());
  EXPECT_EQ(Result::kError, file.Get<0>());
  EXPECT_TRUE(file.Get<1>().empty());
  EXPECT_EQ(1u, factory.total_requests());
}

TEST_F(ProfileSubscriptionFetchTest, NativeManagerPublishesFetchedBytesEncryptedAndPreservesThemOnFailure) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  auto path = temp.GetPath().AppendASCII("profile");
  auto key = [] { ObjectId value{}; value.fill(0x78); return SecretKey(value); };
  std::shared_ptr<RecordFileSystem> files;
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path, key(), {}, &files));
  std::shared_ptr<ProfileFilterStorage> storage;
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files,
      files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  base::test::TestFuture<FileStatus, scoped_refptr<ProfileFilterPreferences>> loaded;
  ProfileFilterPreferences::Load(storage, base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating([](FileStatus) { ADD_FAILURE(); }), loaded.GetCallback());
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(loaded.IsReady());
  ASSERT_EQ(FileStatus::kOk, loaded.Get<0>());
  auto owner = std::get<1>(loaded.Take());
  const GURL url("https://subscription.invalid/persist");
  const std::string text("||fetched-private-rule.invalid^$important\n");
  {
    auto registry = base::MakeRefCounted<PrefRegistrySimple>();
    brave_shields::RegisterPrefsForAdBlockService(registry.get());
    PrefServiceFactory prefs_factory;
    prefs_factory.set_user_prefs(owner);
    auto prefs = prefs_factory.Create(registry);
    auto backend = owner->SubscriptionBackend(Fetcher(),
        base::BindRepeating([](const GURL&) { return GURL("chrome://adblock/source"); }));
    brave_shields::AdBlockFiltersProviderManager providers;
    Manager manager(prefs.get(), &providers, backend, nullptr);
    ASSERT_TRUE(manager.CreateSubscription(url));
    ASSERT_TRUE(factory.IsPending(url.spec()));
    ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), text));
    task_environment()->RunUntilIdle();
    ASSERT_EQ(1u, manager.GetSubscriptions().size());
    auto success = manager.GetSubscriptions()[0].last_successful_update_attempt;
    EXPECT_NE(base::Time::Min(), success);
    ASSERT_TRUE(manager.RefreshSubscription(url, true));
    ASSERT_TRUE(factory.IsPending(url.spec()));
    ASSERT_TRUE(factory.SimulateResponseForPendingRequest(url.spec(), "failure", net::HTTP_SERVICE_UNAVAILABLE));
    task_environment()->RunUntilIdle();
    EXPECT_EQ(success, manager.GetSubscriptions()[0].last_successful_update_attempt);
    base::test::TestFuture<Result, DATFileDataBuffer> read;
    backend.source.Run(url).read.Run(read.GetCallback());
    task_environment()->RunUntilIdle();
    ASSERT_TRUE(read.IsReady());
    EXPECT_EQ(Result::kOk, read.Get<0>());
    EXPECT_EQ(text, std::string(read.Get<1>().begin(), read.Get<1>().end()));
  }
  base::test::TestFuture<FileStatus> closed;
  owner->Close(closed.GetCallback());
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(closed.IsReady());
  ASSERT_EQ(FileStatus::kOk, closed.Get());
  owner.reset();
  storage.reset();
  ASSERT_EQ(FileStatus::kOk, files->Lock());
  files.reset();
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path, key(), &files));
  ASSERT_EQ(FileStatus::kOk, ProfileFilterStorage::Create(files,
      files->ProfileCapability(StorePurpose::kProfileCustomFilters), &storage));
  base::test::TestFuture<FileStatus, scoped_refptr<ProfileFilterPreferences>> reopened;
  ProfileFilterPreferences::Load(storage, base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating([](FileStatus) { ADD_FAILURE(); }), reopened.GetCallback());
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(reopened.IsReady());
  ASSERT_EQ(FileStatus::kOk, reopened.Get<0>());
  owner = std::get<1>(reopened.Take());
  auto backend = owner->SubscriptionBackend(Fetcher(),
      base::BindRepeating([](const GURL&) { return GURL("chrome://adblock/source"); }));
  base::test::TestFuture<Result, DATFileDataBuffer> read;
  backend.source.Run(url).read.Run(read.GetCallback());
  task_environment()->RunUntilIdle();
  ASSERT_TRUE(read.IsReady());
  EXPECT_EQ(Result::kOk, read.Get<0>());
  EXPECT_EQ(text, std::string(read.Get<1>().begin(), read.Get<1>().end()));
}
}  // namespace
}  // namespace alloy::site_records
