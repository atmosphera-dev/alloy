// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "services/network/alloy_guard.h"
#include "services/network/alloy_test_dns_server.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/isolation_info.h"
#include "net/base/features.h"
#include "net/base/network_change_notifier.h"
#include "net/dns/host_resolver.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/dns_transaction.h"
#include "net/dns/public/dns_config_overrides.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/dns/public/secure_dns_mode.h"
#include "net/log/net_log_entry.h"
#include "net/log/test_net_log.h"
#include "net/proxy_resolution/dhcp_pac_file_fetcher.h"
#include "net/proxy_resolution/pac_file_decider.h"
#include "net/proxy_resolution/pac_file_fetcher_impl.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/test/test_doh_server.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_test_util.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
net::MutableNetworkTrafficAnnotationTag DnsTag(std::string_view id) {
  return net::CreateMutableNetworkTrafficAnnotationTag(AlloyGuard::Hash(id));
}
class AlloyGuardDnsTest : public testing::Test {
 protected:
  void CreateContext(bool happy, net::SecureDnsMode mode,
                     const net::DnsOverHttpsConfig& doh = {}) {
    features_.InitWithFeatureState(net::features::kHappyEyeballsV3, happy);
    notifier_ = net::NetworkChangeNotifier::CreateMockIfNeeded();
    service_ = NetworkService::CreateForTesting();
    service_->ConfigureStubHostResolver(net::InsecureDnsMode::kEnabledBuiltIn,
        happy, mode, doh, false, {});
    auto overrides = net::DnsConfigOverrides::CreateOverridingEverythingWithDefaults();
    overrides.nameservers = std::vector<net::IPEndPoint>{dns_.endpoint()};
    overrides.secure_dns_mode = mode;
    overrides.dns_over_https_config = doh;
    overrides.allow_dns_over_https_upgrade = false;
    overrides.use_local_ipv6 = false;
    overrides.attempts = 1;
    service_->host_resolver_manager()->SetDnsConfigOverrides(overrides);
    // NetTestSuite otherwise forces its catch-all localhost resolver instead
    // of the configured client. This is the pinned DNS test API for real,
    // non-public nameservers; keep the catch-all as a fallback safety net.
    service_->host_resolver_manager()->DisableSystemResolverForTesting();
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    log_path_ = dir_.GetPath().AppendASCII("alloy-guard.log");
    auto params = CreateNetworkContextParamsForTesting();
    params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log = base::File(log_path_, base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
    context_ = NetworkContext::CreateForTesting(service_.get(),
        remote_.BindNewPipeAndPassReceiver(), std::move(params), {});
  }
  void TearDown() override {
    context_.reset();
    tasks_.RunUntilIdle();
    service_.reset();
    tasks_.RunUntilIdle();
  }
  void FetchOwnedPage(std::string host = "owned.alloy.test") {
    net::EmbeddedTestServer page;
    page.RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest&) -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response = std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_code(net::HTTP_OK);
          response->set_content("owned-dns-page");
          response->set_content_type("text/plain");
          return response;
        }));
    ASSERT_TRUE(page.Start());
    net::TestDelegate consumer;
    auto request = context_->url_request_context()->CreateRequest(
        page.GetURL(host, "/page"), net::DEFAULT_PRIORITY,
        &consumer, static_cast<net::NetworkTrafficAnnotationTag>(DnsTag("blink_resource_loader")),
        net::handles::kInvalidNetworkHandle);
    request->set_isolation_info(net::IsolationInfo::CreateTransient(/*nonce=*/std::nullopt));
    request->Start();
    consumer.RunUntilComplete();
    EXPECT_EQ(consumer.request_status(), net::OK);
    EXPECT_EQ(consumer.data_received(), "owned-dns-page");
    page_completed_ = consumer.request_status() == net::OK && consumer.data_received() == "owned-dns-page";
    if (page_completed_)
      page_origin_ = net::MutableNetworkTrafficAnnotationTag(request->traffic_annotation());
  }
  int ResolveOwnerless(std::string name) {
    net::HostResolver::ResolveHostParameters params;
    params.dns_query_type = net::DnsQueryType::A;
    auto request = context_->url_request_context()->host_resolver()->CreateRequest(
        net::HostPortPair(name, 80), net::NetworkAnonymizationKey::CreateTransient(),
        net::handles::kInvalidNetworkHandle, net::NetLogWithSource(), params);
    base::test::TestFuture<int> result;
    int error = request->Start(result.GetCallback());
    if (error == net::ERR_IO_PENDING) error = result.Get();
    return error;
  }
  std::vector<base::DictValue> Denials() {
    tasks_.RunUntilIdle();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    std::string text;
    EXPECT_TRUE(base::ReadFileToString(log_path_, &text));
    std::vector<base::DictValue> rows;
    for (const auto& line : base::SplitString(text, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      auto row = base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
      EXPECT_TRUE(row);
      if (row) rows.push_back(std::move(*row));
    }
    return rows;
#else
    EXPECT_FALSE(base::PathExists(log_path_));
    return {};
#endif
  }
  base::test::ScopedFeatureList features_;
  base::test::TaskEnvironment tasks_{base::test::TaskEnvironment::MainThreadType::IO};
  bool page_completed_ = false;
  net::MutableNetworkTrafficAnnotationTag page_origin_;
  test::AlloyTestDnsServer dns_;
  std::unique_ptr<net::NetworkChangeNotifier> notifier_;
  std::unique_ptr<NetworkService> service_;
  base::ScopedTempDir dir_;
  base::FilePath log_path_;
  mojo::Remote<mojom::NetworkContext> remote_;
  std::unique_ptr<NetworkContext> context_;
};

TEST_F(AlloyGuardDnsTest, OwnedPageDnsReachesReceiverAndOwnerlessDoesNot) {
  CreateContext(false, net::SecureDnsMode::kOff);
  ASSERT_TRUE(context_);
  FetchOwnedPage();
  EXPECT_GT(dns_.Queries("owned.alloy.test"), 0);
  EXPECT_EQ(ResolveOwnerless("ownerless.alloy.test"), net::ERR_NETWORK_ACCESS_DENIED);
  EXPECT_EQ(dns_.Queries("ownerless.alloy.test"), 0);
  auto rows = Denials();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(*rows[0].FindString("annotation_id"), "alloy_native_dns");
  EXPECT_EQ(*rows[0].FindString("class"), "unannotated");
#else
  EXPECT_TRUE(rows.empty());
#endif
}

TEST_F(AlloyGuardDnsTest, HappyEyeballsPageRetainsDnsOrigin) {
  CreateContext(true, net::SecureDnsMode::kOff);
  ASSERT_TRUE(context_);
  FetchOwnedPage();
  EXPECT_GT(dns_.Queries("owned.alloy.test"), 0);
  EXPECT_TRUE(Denials().empty());
}

TEST_F(AlloyGuardDnsTest, ProbeOwnedByAdmittedPageReachesDohReceiver) {
  net::TestDohServer doh;
  doh.AddAddressRecord(net::kDohProbeHostname, net::IPAddress::IPv4Localhost());
  ASSERT_TRUE(doh.Start());
  auto config = net::DnsOverHttpsConfig::FromString(doh.GetTemplate());
  ASSERT_TRUE(config);
  CreateContext(true, net::SecureDnsMode::kAutomatic, *config);
  ASSERT_TRUE(context_);
  // An actual admitted request supplies the label; no synthetic page identity
  // is assigned to a browser startup probe. IP literal needs no DoH bootstrap.
  FetchOwnedPage("127.0.0.1");
  ASSERT_TRUE(page_completed_);
  EXPECT_EQ(doh.QueriesServed(), 0);
  auto probe = context_->url_request_context()->host_resolver()->CreateDohProbeRequest();
  probe->SetOriginatingAnnotation(page_origin_);
  EXPECT_EQ(probe->Start(), net::ERR_IO_PENDING);
  ASSERT_TRUE(base::test::RunUntil([&] { return doh.QueriesServedForSubdomains(net::kDohProbeHostname) > 0; }));
  probe.reset();
  EXPECT_TRUE(Denials().empty());
  RecordProperty("owned_probe_requests_received", doh.QueriesServedForSubdomains(net::kDohProbeHostname));
}

TEST_F(AlloyGuardDnsTest, OwnedAutomaticDohFailureKeepsPlaintextFallback) {
  net::TestDohServer doh;
  doh.SetFailRequests(true);
  ASSERT_TRUE(doh.Start());
  auto config = net::DnsOverHttpsConfig::FromString(doh.GetTemplate());
  ASSERT_TRUE(config);
  CreateContext(true, net::SecureDnsMode::kAutomatic, *config);
  ASSERT_TRUE(context_);
  FetchOwnedPage();
  EXPECT_GT(doh.QueriesServed(), 0);
  EXPECT_GT(dns_.Queries("owned.alloy.test"), 0);
  EXPECT_TRUE(Denials().empty());
  RecordProperty("failed_owned_doh_received", doh.QueriesServed());
  RecordProperty("plaintext_fallback_received", dns_.Queries("owned.alloy.test"));
}

class AlloyGuardDohTest : public AlloyGuardDnsTest,
                         public testing::WithParamInterface<net::SecureDnsMode> {};
TEST_P(AlloyGuardDohTest, PageDohHasRealOriginWithoutOwnerlessProbeException) {
  net::TestDohServer doh;
  doh.AddAddressRecord("owned.alloy.test", net::IPAddress::IPv4Localhost());
  ASSERT_TRUE(doh.Start());
  auto config = net::DnsOverHttpsConfig::FromString(doh.GetTemplate());
  ASSERT_TRUE(config);
  CreateContext(true, GetParam(), *config);
  ASSERT_TRUE(context_);
  net::RecordingNetLogObserver events;
  FetchOwnedPage();
  EXPECT_GT(doh.QueriesServedForSubdomains("owned.alloy.test"), 0);
  EXPECT_EQ(dns_.Queries("owned.alloy.test"), 0);
  bool saw_pair = false;
  for (const auto& event : events.GetEntriesWithType(net::NetLogEventType::REQUEST_ALIVE)) {
    if (event.params.FindInt("traffic_annotation") == AlloyGuard::Hash("dns_over_https") &&
        event.params.FindInt("originating_traffic_annotation") == AlloyGuard::Hash("blink_resource_loader"))
      saw_pair = true;
  }
  EXPECT_TRUE(saw_pair);
  const int before = doh.QueriesServed();
  auto probe = context_->url_request_context()->host_resolver()->CreateDohProbeRequest();
  EXPECT_EQ(probe->Start(), net::ERR_NETWORK_ACCESS_DENIED);
  EXPECT_EQ(ResolveOwnerless("ownerless.alloy.test"), net::ERR_NETWORK_ACCESS_DENIED);
  tasks_.RunUntilIdle();
  EXPECT_EQ(doh.QueriesServed(), before);
  EXPECT_EQ(dns_.Queries("ownerless.alloy.test"), 0);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  EXPECT_EQ(Denials().size(), 2u);
#else
  EXPECT_TRUE(Denials().empty());
#endif
  RecordProperty("secure_dns_mode", GetParam() == net::SecureDnsMode::kAutomatic ? "automatic" : "secure");
  RecordProperty("owned_doh_queries", doh.QueriesServedForSubdomains("owned.alloy.test"));
  RecordProperty("ownerless_probe_requests_received", doh.QueriesServed() - before);
}
INSTANTIATE_TEST_SUITE_P(Modes, AlloyGuardDohTest,
    testing::Values(net::SecureDnsMode::kAutomatic, net::SecureDnsMode::kSecure));

TEST_F(AlloyGuardDnsTest, WpadQuickCheckDeniedBeforeDnsServer) {
  CreateContext(false, net::SecureDnsMode::kOff);
  ASSERT_TRUE(context_);
  auto fetcher = net::PacFileFetcherImpl::Create(context_->url_request_context());
  net::DoNothingDhcpPacFileFetcher dhcp;
  net::PacFileDecider decider(fetcher.get(), &dhcp, nullptr);
  net::ProxyConfig proxy;
  proxy.set_auto_detect(true);
  net::ProxyConfigWithAnnotation config(proxy,
      static_cast<net::NetworkTrafficAnnotationTag>(DnsTag("proxy_config")));
  base::test::TestFuture<int> done;
  int result = decider.Start(config, base::TimeDelta(), true, done.GetCallback());
  if (result == net::ERR_IO_PENDING) result = done.Get();
  EXPECT_NE(result, net::OK);
  EXPECT_EQ(dns_.Queries("wpad"), 0);
  auto rows = Denials();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  int wpad_denials = 0;
  for (const auto& row : rows) {
    if (row.FindString("endpoint") && *row.FindString("endpoint") == "wpad") {
      ++wpad_denials;
      EXPECT_EQ(*row.FindString("annotation_id"), "alloy_native_dns");
      EXPECT_EQ(*row.FindString("class"), "unannotated");
    }
  }
  EXPECT_EQ(wpad_denials, 1);
#else
  EXPECT_TRUE(rows.empty());
#endif
}
}  // namespace
}  // namespace network
