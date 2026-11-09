// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/callback_helpers.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/test/simple_test_clock.h"
#include "base/test/simple_test_tick_clock.h"
#include "brave/alloy/site_records/network_retirement_test_context.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/dom_storage/session_storage_impl.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/features.h"
#include "net/base/network_isolation_key.h"
#include "net/http/http_cache.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/no_vary_search_cache.h"
#include "net/http/no_vary_search_cache_test_utils.h"
#include "net/dns/host_cache.h"
#include "net/socket/ssl_client_socket.h"
#include "net/network_error_logging/network_error_logging_service.h"
#include "net/network_error_logging/mock_persistent_nel_store.h"
#include "net/reporting/reporting_service.h"
#include "net/reporting/reporting_test_util.h"
#include "net/reporting/mock_persistent_reporting_store.h"
#include "net/http/http_server_properties.h"
#include "net/http/transport_security_state.h"
#include "net/url_request/url_request_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"

namespace alloy::site_records {
namespace {
using Area = mojo::Remote<blink::mojom::StorageArea>;
using Values = std::vector<blink::mojom::KeyValuePtr>;
class FirstPartyRetirementTest : public testing::Test {
 protected:
  void Put(Area& area) {
    base::test::TestFuture<bool> written;
    area->Put({'k'}, {'v'}, std::nullopt,
        blink::mojom::StorageAreaSource::New(GURL("https://alpha.test/"), base::Token(1, 1)),
        written.GetCallback());
    ASSERT_TRUE(written.Get());
  }
  Values Read(Area& area) {
    base::test::TestFuture<Values> read;
    area->GetAll(mojo::NullRemote(), read.GetCallback());
    return read.Take();
  }
  void CannotWrite(Area& area) {
    base::test::TestFuture<bool> written;
    area->Put({'k'}, {'x'}, std::nullopt,
        blink::mojom::StorageAreaSource::New(GURL("https://alpha.test/"), base::Token(1, 1)),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(written.GetCallback(), false));
    EXPECT_FALSE(written.Get());
    EXPECT_FALSE(area.is_connected());
  }
  base::test::TaskEnvironment tasks_{base::test::TaskEnvironment::MainThreadType::IO};
  const blink::StorageKey alpha_ = blink::StorageKey::CreateFirstParty(url::Origin::Create(GURL("https://alpha.test/")));
  const blink::StorageKey beta_ = blink::StorageKey::CreateFirstParty(url::Origin::Create(GURL("https://beta.test/")));
};
TEST_F(FirstPartyRetirementTest, LocalStorageRetiresRetainedEndpointAndKeepsOtherKey) {
  storage::LocalStorageImpl storage({}, base::NullCallback(), mojo::NullReceiver());
  Area old, other;
  storage.BindStorageArea(alpha_, old.BindNewPipeAndPassReceiver());
  storage.BindStorageArea(beta_, other.BindNewPipeAndPassReceiver());
  Put(old); Put(other);
  base::test::TestFuture<bool> retired;
  storage.RetireStorage(alpha_, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  CannotWrite(old);
  Area fresh;
  storage.BindStorageArea(alpha_, fresh.BindNewPipeAndPassReceiver());
  EXPECT_TRUE(Read(fresh).empty());
  EXPECT_EQ(1u, Read(other).size());
}
TEST_F(FirstPartyRetirementTest, SessionStorageRetiresAllClonesAndLateCloneCannotRestore) {
  mojo::Remote<storage::mojom::SessionStorageControl> control;
  storage::SessionStorageImpl storage({}, storage::SessionStorageImpl::BackingMode::kNoDisk,
      base::NullCallback(), control.BindNewPipeAndPassReceiver());
  const std::string source(36, '1'), clone(36, '2');
  storage.CreateNamespace(source);
  Area old, other;
  storage.BindStorageArea(alpha_, source, old.BindNewPipeAndPassReceiver());
  storage.BindStorageArea(beta_, source, other.BindNewPipeAndPassReceiver());
  Put(old); Put(other);
  mojo::Remote<blink::mojom::SessionStorageNamespace> original_namespace;
  storage.BindNamespace(source, original_namespace.BindNewPipeAndPassReceiver());
  storage.CloneNamespace(source, clone, storage::mojom::SessionStorageCloneType::kWaitForCloneOnNamespace);
  base::test::TestFuture<bool> retired;
  storage.RetireStorage(alpha_, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  CannotWrite(old);
  original_namespace->Clone(clone);
  original_namespace.FlushForTesting();
  EXPECT_TRUE(original_namespace.is_connected());
  Area fresh, cloned_other;
  storage.BindStorageArea(alpha_, clone, fresh.BindNewPipeAndPassReceiver());
  storage.BindStorageArea(beta_, clone, cloned_other.BindNewPipeAndPassReceiver());
  EXPECT_TRUE(Read(fresh).empty());
  EXPECT_EQ(1u, Read(other).size());
  EXPECT_EQ(1u, Read(cloned_other).size());
  Put(fresh);
  EXPECT_EQ(1u, Read(fresh).size());
}
TEST_F(FirstPartyRetirementTest, CacheOwnerMatchesNativeTopFrameAndPartitionKeys) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kSplitCacheByNetworkIsolationKey);
  const GURL cdn("https://cdn.test/value");
  const net::SchemefulSite a(GURL("https://alpha.test/")), b(GURL("https://beta.test/"));
  for (bool subframe : {false, true}) for (bool navigation : {false, true}) {
    const auto alpha = net::HttpCache::GenerateCacheKey(cdn, 0, net::NetworkIsolationKey(a, a), {},
        subframe, navigation, false, url::Origin::Create(GURL("https://beta.test/")));
    const auto beta = net::HttpCache::GenerateCacheKey(cdn, 0, net::NetworkIsolationKey(b, b), {},
        subframe, navigation, false, url::Origin::Create(GURL("https://beta.test/")));
    ASSERT_TRUE(alpha); ASSERT_TRUE(beta);
    EXPECT_TRUE(net::HttpCache::CacheKeyReferencesRecord(*alpha, "alpha.test"));
    EXPECT_FALSE(net::HttpCache::CacheKeyReferencesRecord(*beta, "alpha.test"));
    EXPECT_EQ(cdn.spec(), net::HttpCache::GetResourceURLFromHttpCacheKey(*alpha));
    const auto partition = alpha->substr(0, alpha->size() - cdn.spec().size() - 1);
    EXPECT_TRUE(net::HttpCache::CacheKeyReferencesRecord(partition, "alpha.test", true));
  }
}
TEST_F(FirstPartyRetirementTest, NativeNetworkMetadataRetiresSubdomainsAndPreservesOtherSite) {
  NetworkRetirementTestContext owner;
  auto* native = owner.context().url_request_context();
  auto* hsts = native->transport_security_state();
  auto* properties = native->http_server_properties();
  const url::SchemeHostPort a(GURL("https://sub.alpha.test/")), b(GURL("https://beta.test/"));
  const net::SchemefulSite as(a.GetURL()), bs(b.GetURL());
  const auto ak = net::NetworkAnonymizationKey::CreateFromFrameSite(as, as);
  const auto bk = net::NetworkAnonymizationKey::CreateFromFrameSite(bs, bs);
  hsts->AddHSTS("sub.alpha.test", base::Time::Now() + base::Days(1), false);
  hsts->AddHSTS("beta.test", base::Time::Now() + base::Days(1), false);
  properties->SetSupportsSpdy(a, ak, true); properties->SetSupportsSpdy(b, bk, true);
  ASSERT_TRUE(hsts->ShouldUpgradeToSSL("sub.alpha.test", true));
  ASSERT_TRUE(hsts->ShouldUpgradeToSSL("beta.test", true));
  base::test::TestFuture<bool> retired;
  owner.remote()->RetireAlloyRecordData("alpha.test", retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  EXPECT_FALSE(hsts->ShouldUpgradeToSSL("sub.alpha.test", true));
  EXPECT_TRUE(hsts->ShouldUpgradeToSSL("beta.test", true));
  EXPECT_FALSE(properties->GetSupportsSpdy(a, ak));
  EXPECT_TRUE(properties->GetSupportsSpdy(b, bk));
}

TEST_F(FirstPartyRetirementTest, AuthAndClientDecisionsRetireWithoutClearingProxyOrOtherSite) {
  NetworkRetirementTestContext owner;
  auto* session = owner.context().url_request_context()->http_transaction_factory()->GetSession();
  auto* auth = session->http_auth_cache();
  auto* ssl = session->ssl_client_context();
  const url::SchemeHostPort a(GURL("https://alpha.test/")), b(GURL("https://beta.test/"));
  const net::SchemefulSite as(a.GetURL()), bs(b.GetURL());
  const auto ak = net::NetworkAnonymizationKey::CreateFromFrameSite(as, as);
  const auto bk = net::NetworkAnonymizationKey::CreateFromFrameSite(bs, bs);
  const net::AuthCredentials credentials(u"user", u"secret");
  auto add = [&](const url::SchemeHostPort& url, net::HttpAuth::Target role,
                  const net::NetworkAnonymizationKey& key) {
    auth->Add(url, role, "realm", net::HttpAuth::AUTH_SCHEME_BASIC, key,
              "Basic realm=\"realm\"", credentials, "/");
  };
  add(a, net::HttpAuth::AUTH_SERVER, ak);
  add(b, net::HttpAuth::AUTH_SERVER, bk);
  add(a, net::HttpAuth::AUTH_PROXY, {});
  // A cached refusal is still a client-certificate decision to retire.
  const net::HostPortPair ap("alpha.test", 443), bp("beta.test", 443);
  ssl->SetClientCertificate(ap, nullptr, nullptr);
  ssl->SetClientCertificate(bp, nullptr, nullptr);
  scoped_refptr<net::X509Certificate> cert;
  scoped_refptr<net::SSLPrivateKey> key;
  ASSERT_TRUE(ssl->GetClientCertificate(ap, &cert, &key));
  base::test::TestFuture<bool> retired;
  owner.remote()->RetireAlloyRecordData("alpha.test", retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  EXPECT_FALSE(auth->Lookup(a, net::HttpAuth::AUTH_SERVER, "realm", net::HttpAuth::AUTH_SCHEME_BASIC, ak));
  EXPECT_TRUE(auth->Lookup(b, net::HttpAuth::AUTH_SERVER, "realm", net::HttpAuth::AUTH_SCHEME_BASIC, bk));
  EXPECT_TRUE(auth->Lookup(a, net::HttpAuth::AUTH_PROXY, "realm", net::HttpAuth::AUTH_SCHEME_BASIC, {}));
  EXPECT_FALSE(ssl->GetClientCertificate(ap, &cert, &key));
  EXPECT_TRUE(ssl->GetClientCertificate(bp, &cert, &key));
}

TEST_F(FirstPartyRetirementTest, DnsRetirementUsesTopSiteAndLeavesOtherCachedAnswers) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kPartitionConnectionsByNetworkIsolationKey);
  NetworkRetirementTestContext owner({}, true);
  auto* resolver = &owner.resolver();
  const net::SchemefulSite a(GURL("https://alpha.test/")), b(GURL("https://beta.test/"));
  const auto ak = net::NetworkAnonymizationKey::CreateFromFrameSite(a, a);
  const auto bk = net::NetworkAnonymizationKey::CreateFromFrameSite(b, b);
  for (const auto& key : {ak, bk}) EXPECT_EQ(net::OK, resolver->LoadIntoCache(
      std::variant<url::SchemeHostPort, net::HostPortPair>(url::SchemeHostPort(GURL("https://cdn.test/"))),
      key, std::nullopt));
  auto* cache = resolver->GetHostCache();
  ASSERT_EQ(2u, cache->entries().size());
  const auto originals = cache->entries();
  base::test::TestFuture<bool> retired;
  owner.remote()->RetireAlloyRecordData("alpha.test", retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  for (const auto& [key, value] : originals) EXPECT_EQ(key.network_anonymization_key == bk,
      cache->Lookup(key, base::TimeTicks::Now()) != nullptr);
}

TEST_F(FirstPartyRetirementTest, NoVarySearchRetiresCdnMappingByNativeTopSite) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kSplitCacheByNetworkIsolationKey);
  net::NoVarySearchCache cache(10);
  const GURL url("https://cdn.test/value?ignored=1");
  const net::SchemefulSite a(GURL("https://alpha.test/")), b(GURL("https://beta.test/"));
  const auto ar = net::no_vary_search_cache_test_utils::CreateTestRequest(url, net::NetworkIsolationKey(a, a));
  const auto br = net::no_vary_search_cache_test_utils::CreateTestRequest(url, net::NetworkIsolationKey(b, b));
  const auto header = net::no_vary_search_cache_test_utils::CreateTestHeaders("params");
  cache.MaybeInsert(ar, *header); cache.MaybeInsert(br, *header);
  ASSERT_TRUE(cache.Lookup(ar)); ASSERT_TRUE(cache.Lookup(br));
  EXPECT_TRUE(cache.ClearData(net::UrlFilterType::kTrueIfMatches, {}, {"alpha.test"},
      base::Time(), base::Time::Max(), "alpha.test"));
  EXPECT_FALSE(cache.Lookup(ar)); EXPECT_TRUE(cache.Lookup(br));
}

TEST_F(FirstPartyRetirementTest, ReportingRetirementWaitsForLoadAndPreservesOtherEndpoints) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kPartitionConnectionsByNetworkIsolationKey);
  base::SimpleTestClock clock;
  base::SimpleTestTickClock ticks;
  clock.SetNow(base::Time::Now()); ticks.SetNowTicks(base::TimeTicks::Now());
  net::MockPersistentReportingStore store;
  auto context = std::make_unique<net::TestReportingContext>(&clock, &ticks, net::ReportingPolicy(), &store);
  auto* cache = context->cache();
  auto service = net::ReportingService::CreateForTesting(std::move(context));
  const auto a = url::Origin::Create(GURL("https://alpha.test/"));
  const auto b = url::Origin::Create(GURL("https://beta.test/"));
  const auto cdn = url::Origin::Create(GURL("https://cdn.test/"));
  const net::SchemefulSite as(a), bs(b);
  const auto ak = net::NetworkAnonymizationKey::CreateFromFrameSite(as, as);
  const auto bk = net::NetworkAnonymizationKey::CreateFromFrameSite(bs, bs);
  const std::string header = R"({"max_age":86400,"endpoints":[{"url":"https://alpha.test/report"},{"url":"https://beta.test/report"}]})";
  service->ProcessReportToHeader(cdn, ak, header);
  service->ProcessReportToHeader(b, bk, header);
  const auto source = base::UnguessableToken::Create();
  service->SetDocumentReportingEndpoints(source, b,
      net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther, b, b,
                                  net::SiteForCookies::FromOrigin(b)),
      {{"alpha", "https://alpha.test/report"}, {"beta", "https://beta.test/report"}});
  base::test::TestFuture<bool> retired;
  service->RetireAlloyRecord("alpha.test", retired.GetCallback());
  EXPECT_FALSE(retired.IsReady());
  store.FinishLoading(true);
  ASSERT_TRUE(retired.Get());
  EXPECT_FALSE(cache->GetAllOrigins().contains(cdn));
  EXPECT_TRUE(cache->GetAllOrigins().contains(b));
  const auto endpoints = cache->GetV1ReportingEndpointsByOrigin();
  ASSERT_TRUE(endpoints.contains(b));
  ASSERT_EQ(1u, endpoints.at(b).size());
  EXPECT_EQ(GURL("https://beta.test/report"), endpoints.at(b).front().info.url);
}

TEST_F(FirstPartyRetirementTest, NelRetirementWaitsForLoadAndRemovesTopSitePolicyOnly) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kPartitionConnectionsByNetworkIsolationKey);
  net::MockPersistentNelStore store;
  auto service = net::NetworkErrorLoggingService::Create(&store);
  const net::SchemefulSite a(GURL("https://alpha.test/")), b(GURL("https://beta.test/"));
  const auto ak = net::NetworkAnonymizationKey::CreateFromFrameSite(a, a);
  const auto bk = net::NetworkAnonymizationKey::CreateFromFrameSite(b, b);
  const auto cdn = url::Origin::Create(GURL("https://cdn.test/"));
  for (const auto& key : {ak, bk}) service->OnHeader(key, cdn, net::IPAddress::IPv4Localhost(),
      R"({"report_to":"default","max_age":86400,"include_subdomains":true})");
  base::test::TestFuture<bool> retired;
  service->RetireAlloyRecord("alpha.test", retired.GetCallback());
  EXPECT_FALSE(retired.IsReady());
  store.FinishLoading(true);
  ASSERT_TRUE(retired.Get());
  const auto policies = service->GetPolicyKeysForTesting();
  EXPECT_FALSE(policies.contains({ak, cdn}));
  EXPECT_TRUE(policies.contains({bk, cdn}));
}
}  // namespace
}  // namespace alloy::site_records
