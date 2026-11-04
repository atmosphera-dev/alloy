// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/isolation_info.h"
#include "services/network/native_network_retirement.h"
#include "services/network/prefetch_cache.h"
#include "services/network/prefetch_url_loader_client.h"
#include "services/network/public/cpp/resource_request.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
class PrefetchRetirementTest : public testing::Test {
 public:
  network::ResourceRequest Request(const char* host) {
    network::ResourceRequest request;
    request.url = GURL(std::string("https://") + host + "/prefetch");
    request.request_initiator = url::Origin::Create(request.url);
    request.trusted_params = network::ResourceRequest::TrustedParams();
    request.trusted_params->isolation_info =
        net::IsolationInfo::CreateForInternalRequest(
            *request.request_initiator);
    return request;
  }

 protected:
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
};

TEST_F(PrefetchRetirementTest,
       DelayedOriginalEraseCannotRemoveSameKeyReplacement) {
  network::PrefetchCache cache;
  auto request = Request("alpha.test");
  auto* original = cache.Emplace(request);
  ASSERT_TRUE(original);
  cache.DelayedErase(original);
  cache.Erase(original);
  auto* fresh = cache.Emplace(request);
  ASSERT_TRUE(fresh);
  tasks_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(fresh,
            cache.Lookup(
                request.trusted_params->isolation_info.network_isolation_key(),
                request.url));
}

TEST_F(PrefetchRetirementTest, DelayedOriginalErasePreservesDifferentKeyBeta) {
  network::PrefetchCache cache;
  auto alpha = Request("alpha.test"), beta = Request("beta.test");
  auto* original = cache.Emplace(alpha);
  auto* unrelated = cache.Emplace(beta);
  ASSERT_TRUE(original);
  ASSERT_TRUE(unrelated);
  cache.DelayedErase(original);
  tasks_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(
      nullptr,
      cache.Lookup(alpha.trusted_params->isolation_info.network_isolation_key(),
                   alpha.url));
  EXPECT_EQ(
      unrelated,
      cache.Lookup(beta.trusted_params->isolation_info.network_isolation_key(),
                   beta.url));
}

TEST_F(PrefetchRetirementTest,
       ConsumedOriginalWithoutReplacementErasesNormally) {
  network::PrefetchCache cache;
  auto request = Request("alpha.test");
  auto* original = cache.Emplace(request);
  ASSERT_TRUE(original);
  cache.Consume(original);
  EXPECT_EQ(nullptr,
            cache.Lookup(
                request.trusted_params->isolation_info.network_isolation_key(),
                request.url));
  cache.Erase(original);
  EXPECT_NE(nullptr, cache.Emplace(request));
}
TEST_F(PrefetchRetirementTest,
       ConsumedOriginalErasePreservesSameKeyReplacement) {
  network::PrefetchCache cache;
  auto request = Request("alpha.test");
  auto* original = cache.Emplace(request);
  ASSERT_TRUE(original);
  cache.Consume(original);
  auto* fresh = cache.Emplace(request);
  ASSERT_TRUE(fresh);
  cache.Erase(original);
  EXPECT_EQ(fresh,
            cache.Lookup(
                request.trusted_params->isolation_info.network_isolation_key(),
                request.url));
}

TEST_F(PrefetchRetirementTest,
       RetirementDetachesOriginalAndPreservesFreshAndBeta) {
  network::PrefetchCache cache;
  auto alpha = Request("alpha.test"), beta = Request("beta.test");
  auto* original = cache.Emplace(alpha);
  auto* unrelated = cache.Emplace(beta);
  ASSERT_TRUE(original);
  ASSERT_TRUE(unrelated);
  cache.DelayedErase(original);
  base::test::TestFuture<void> drained;
  auto commit = cache.PrepareNetworkRetirement(
      base::BindRepeating(
          [](net::NetworkAnonymizationKey key,
             const network::NativeNetworkOwner& owner) {
            return key == owner.network_key;
          },
          alpha.trusted_params->isolation_info.network_anonymization_key()),
      drained.GetCallback());
  EXPECT_EQ(
      nullptr,
      cache.Lookup(alpha.trusted_params->isolation_info.network_isolation_key(),
                   alpha.url));
  auto* fresh = cache.Emplace(alpha);
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
  tasks_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(
      fresh,
      cache.Lookup(alpha.trusted_params->isolation_info.network_isolation_key(),
                   alpha.url));
  EXPECT_EQ(
      unrelated,
      cache.Lookup(beta.trusted_params->isolation_info.network_isolation_key(),
                   beta.url));
}

TEST_F(PrefetchRetirementTest,
       ConsumedOriginalEndpointClosesWithoutTouchingReplacement) {
  network::PrefetchCache cache;
  auto request = Request("alpha.test");
  auto* original = cache.Emplace(request);
  ASSERT_TRUE(original);
  mojo::Remote<network::mojom::URLLoaderClient> producer(
      original->BindNewPipeAndPassRemote());
  base::test::TestFuture<void> disconnected, drained;
  producer.set_disconnect_handler(disconnected.GetCallback());
  cache.Consume(original);
  auto commit = cache.PrepareNetworkRetirement(
      base::BindRepeating(
          [](net::NetworkAnonymizationKey key,
             const network::NativeNetworkOwner& owner) {
            return key == owner.network_key;
          },
          request.trusted_params->isolation_info.network_anonymization_key()),
      drained.GetCallback());
  auto* fresh = cache.Emplace(request);
  ASSERT_TRUE(fresh);
  std::move(commit).Run();
  ASSERT_TRUE(disconnected.Wait());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(fresh,
            cache.Lookup(
                request.trusted_params->isolation_info.network_isolation_key(),
                request.url));
}
}  // namespace
}  // namespace alloy::site_records
