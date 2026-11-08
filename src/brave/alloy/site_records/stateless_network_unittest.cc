// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/alloy/buildflags.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/features.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/base/network_isolation_key.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/dns/mock_host_resolver.h"
#include "net/extras/shared_dictionary/shared_dictionary_usage_info.h"
#include "net/http/http_cache.h"
#include "net/http/http_server_properties.h"
#include "net/http/http_status_code.h"
#include "net/http/transport_security_state.h"
#include "net/net_buildflags.h"
#include "net/network_error_logging/network_error_logging_service.h"
#include "net/reporting/reporting_cache.h"
#include "net/reporting/reporting_context.h"
#include "net/reporting/reporting_endpoint.h"
#include "net/reporting/reporting_service.h"
#include "net/shared_dictionary/shared_dictionary.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/url_request/url_request_context.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/alloy_guard.h"
#include "services/network/cookie_manager.h"
#include "services/network/cookie_settings.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/shared_dictionary/shared_dictionary_access_checker.h"
#include "services/network/shared_dictionary/shared_dictionary_manager.h"
#include "services/network/shared_dictionary/shared_dictionary_storage.h"
#include "services/network/test/client_security_state_builder.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

namespace alloy::site_records {
namespace {

class StatelessNetworkTest : public testing::Test {
 protected:
  void CheckTransientCache(bool memory) {
    base::test::ScopedFeatureList features;
    features.InitWithFeatures({net::features::kSplitCacheByNetworkIsolationKey,
                               net::features::kHttpCacheNoVarySearch},
                              {});
    auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
    std::atomic<int> requests = 0;
    net::EmbeddedTestServer server;
    server.RegisterRequestHandler(base::BindRepeating(
        [](std::atomic<int>* count, const net::test_server::HttpRequest&)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_code(net::HTTP_OK);
          response->set_content_type("text/plain");
          response->AddCustomHeader("Cache-Control", "public, max-age=3600");
          response->AddCustomHeader("No-Vary-Search", "params=(\"ignored\")");
          response->set_content("transient-body-" + std::to_string(++*count));
          return response;
        },
        &requests));
    ASSERT_TRUE(server.Start());
    auto service = network::NetworkService::CreateForTesting();
    base::ScopedTempDir data;
    base::ScopedTempDir logs;
    ASSERT_TRUE(data.CreateUniqueTempDir());
    ASSERT_TRUE(logs.CreateUniqueTempDir());
    auto params = network::CreateNetworkContextParamsForTesting();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->http_cache_enabled = true;
    params->alloy_stateless = false;
    if (!memory) {
      params->file_paths = network::mojom::NetworkContextFilePaths::New();
      params->file_paths->http_cache_directory = data.GetPath();
    } else {
      params->file_paths.reset();
    }
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log =
        base::File(logs.GetPath().AppendASCII("guard.log"),
                   base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
    mojo::Remote<network::mojom::NetworkContext> remote;
    auto context = network::NetworkContext::CreateForTesting(
        service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
        {});
    const auto origin = url::Origin::Create(server.base_url());
    int id = 0;
    auto fetch = [&](base::UnguessableToken nonce, const char* query) {
      auto factory_params = network::mojom::URLLoaderFactoryParams::New();
      factory_params->process_id = network::OriginatingProcessId::browser();
      factory_params->is_orb_enabled = false;
      factory_params->isolation_info = net::IsolationInfo::Create(
          net::IsolationInfo::RequestType::kOther, origin, origin,
          net::SiteForCookies::FromUrl(server.base_url()), nonce);
      mojo::Remote<network::mojom::URLLoaderFactory> factory;
      remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                     std::move(factory_params));
      network::ResourceRequest request;
      request.url = server.GetURL(std::string("/cacheable?ignored=") + query);
      request.method = "GET";
      request.request_initiator = origin;
      request.site_for_cookies =
          net::SiteForCookies::FromUrl(server.base_url());
      request.credentials_mode = network::mojom::CredentialsMode::kInclude;
      network::TestURLLoaderClient client;
      mojo::Remote<network::mojom::URLLoader> loader;
      net::MutableNetworkTrafficAnnotationTag annotation;
      annotation.unique_id_hash_code =
          network::AlloyGuard::Hash("blink_resource_loader");
      factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(), ++id,
                                    0, request, client.CreateRemote(),
                                    annotation);
      client.RunUntilComplete();
      EXPECT_EQ(net::OK, client.completion_status().error_code);
      std::string body;
      EXPECT_TRUE(
          mojo::BlockingCopyToString(client.response_body_release(), &body));
      return body;
    };
    const auto first = base::UnguessableToken::Create();
    const auto second = base::UnguessableToken::Create();
    EXPECT_EQ("transient-body-1", fetch(first, "one"));
    EXPECT_EQ(memory ? "transient-body-1" : "transient-body-2",
              fetch(first, "one"));
    EXPECT_EQ(memory ? "transient-body-1" : "transient-body-3",
              fetch(first, "two"));
    EXPECT_EQ(memory ? "transient-body-2" : "transient-body-4",
              fetch(second, "one"));
    EXPECT_EQ(memory ? "transient-body-2" : "transient-body-5",
              fetch(second, "one"));
    EXPECT_EQ(memory ? "transient-body-1" : "transient-body-6",
              fetch(first, "two"));
    EXPECT_EQ(memory ? 2 : 6, requests.load());
    auto* backend = context->url_request_context()
                        ->http_transaction_factory()
                        ->GetCache()
                        ->GetCurrentBackend();
    ASSERT_TRUE(backend);
    EXPECT_EQ(memory ? net::MEMORY_CACHE : net::DISK_CACHE,
              backend->GetCacheType());
    base::test::TestFuture<int32_t> count;
    auto result = backend->GetEntryCount(count.GetCallback());
    if (result.has_value()) {
      EXPECT_EQ(memory ? 2 : 0, result.value());
    } else {
      ASSERT_EQ(net::ERR_IO_PENDING, result.error());
      EXPECT_EQ(memory ? 2 : 0, count.Get());
    }
    remote.reset();
    context.reset();
    service.reset();
    tasks_.RunUntilIdle();
  }

  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};

  void CheckConsumers(bool stateless) {
    auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
    std::atomic<int> cache_requests = 0;
    net::EmbeddedTestServer server;
    server.RegisterRequestHandler(base::BindRepeating(
        [](std::atomic<int>* requests,
           const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_code(net::HTTP_OK);
          response->set_content_type("text/plain");
          if (request.relative_url == "/cacheable") {
            response->AddCustomHeader("Cache-Control", "public, max-age=3600");
            response->AddCustomHeader("Set-Cookie",
                                      "alloy_state=retained; Path=/");
            response->set_content("body-" + std::to_string(++*requests));
          } else {
            response->AddCustomHeader("Cache-Control", "no-store");
            response->set_content(request.headers.contains("Cookie")
                                      ? "cookie-seen"
                                      : "cookie-empty");
          }
          return response;
        },
        &cache_requests));
    ASSERT_TRUE(server.Start());
    auto service = network::NetworkService::CreateForTesting();
    base::ScopedTempDir data_dir;
    base::ScopedTempDir log_dir;
    ASSERT_TRUE(data_dir.CreateUniqueTempDir());
    ASSERT_TRUE(log_dir.CreateUniqueTempDir());
    auto params = network::CreateNetworkContextParamsForTesting();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->alloy_stateless = stateless;
    // Deliberately supply normal persistence/cache inputs. The trusted mode
    // must refuse them before a backend can open, including in verification.
    params->http_cache_enabled = true;
    params->shared_dictionary_enabled = true;
    params->enable_encrypted_cookies = false;
    params->file_paths = network::mojom::NetworkContextFilePaths::New();
    params->file_paths->data_directory = data_dir.GetPath();
    params->file_paths->cookie_database_name =
        base::FilePath(FILE_PATH_LITERAL("Cookies"));
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log =
        base::File(log_dir.GetPath().AppendASCII("guard.log"),
                   base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
    mojo::Remote<network::mojom::NetworkContext> remote;
    auto context = network::NetworkContext::CreateForTesting(
        service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
        {});
    if (stateless) {
#if BUILDFLAG(ENABLE_REPORTING)
      EXPECT_EQ(nullptr, context->url_request_context()->reporting_service());
      EXPECT_EQ(
          nullptr,
          context->url_request_context()->network_error_logging_service());
#endif
    }
    const auto origin = url::Origin::Create(server.base_url());
    auto factory_params = network::mojom::URLLoaderFactoryParams::New();
    factory_params->process_id = network::OriginatingProcessId::browser();
    factory_params->is_orb_enabled = false;
    factory_params->isolation_info =
        net::IsolationInfo::CreateForInternalRequest(origin);
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                   std::move(factory_params));
    int request_id = 0;
    auto fetch = [&](const char* path) {
      network::ResourceRequest request;
      request.method = "GET";
      request.url = server.GetURL(path);
      request.request_initiator = origin;
      request.site_for_cookies =
          net::SiteForCookies::FromUrl(server.base_url());
      request.credentials_mode = network::mojom::CredentialsMode::kInclude;
      network::TestURLLoaderClient client;
      mojo::Remote<network::mojom::URLLoader> loader;
      net::MutableNetworkTrafficAnnotationTag annotation;
      annotation.unique_id_hash_code =
          network::AlloyGuard::Hash("blink_resource_loader");
      factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(),
                                    ++request_id, 0, request,
                                    client.CreateRemote(), annotation);
      client.RunUntilComplete();
      EXPECT_EQ(net::OK, client.completion_status().error_code);
      std::string body;
      EXPECT_TRUE(
          mojo::BlockingCopyToString(client.response_body_release(), &body));
      return body;
    };
    EXPECT_EQ("body-1", fetch("/cacheable"));
    EXPECT_EQ(stateless ? "body-2" : "body-1", fetch("/cacheable"));
    EXPECT_EQ(stateless ? 2 : 1, cache_requests.load());
    EXPECT_EQ(stateless ? "cookie-empty" : "cookie-seen",
              fetch("/cookie-check"));
    tasks_.RunUntilIdle();
    if (stateless)
      EXPECT_TRUE(base::IsDirectoryEmpty(data_dir.GetPath()));
    factory.reset();
    remote.reset();
    context.reset();
    service.reset();
    tasks_.RunUntilIdle();
    if (stateless)
      EXPECT_TRUE(base::IsDirectoryEmpty(data_dir.GetPath()));
  }
};

TEST_F(StatelessNetworkTest, NativeControlActuallyCachesAndKeepsCookies) {
  CheckConsumers(false);
}

TEST_F(StatelessNetworkTest,
       RejectsCookiesCachesAndPersistentBackendsAtAdmission) {
  CheckConsumers(true);
}

TEST_F(StatelessNetworkTest,
       NativeMemoryCacheRetainsNonceIsolationAndNoVarySearch) {
  CheckTransientCache(true);
}

TEST_F(StatelessNetworkTest, NativeDiskCacheRejectsTransientKeysAndMetadata) {
  CheckTransientCache(false);
}

TEST(TransientCacheIdentityTest, MemoryRepresentationPreservesNativeIdentity) {
  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(
      net::features::kSplitCacheByNetworkIsolationKey);
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  base::ScopedTempDir logs;
  ASSERT_TRUE(logs.CreateUniqueTempDir());
  auto service = network::NetworkService::CreateForTesting();
  auto params = network::CreateNetworkContextParamsForTesting();
  params->cert_verifier_params =
      network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
  params->http_cache_enabled = true;
  params->file_paths.reset();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log =
      base::File(logs.GetPath().AppendASCII("guard.log"),
                 base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<network::mojom::NetworkContext> remote;
  auto context = network::NetworkContext::CreateForTesting(
      service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
      {});
  auto* cache =
      context->url_request_context()->http_transaction_factory()->GetCache();
  ASSERT_TRUE(cache);
  auto memory_key = [&](const net::NetworkIsolationKey& identity) {
    net::HttpRequestInfo request;
    request.url = GURL("https://cache.test/resource");
    request.method = "GET";
    request.network_isolation_key = identity;
    return cache->GenerateCacheKeyForBackend(&request);
  };
  const net::SchemefulSite top(GURL("https://outer.test/"));
  const net::SchemefulSite frame(GURL("https://embedded.test/"));
  const auto nonce = base::UnguessableToken::Create();
  const net::NetworkIsolationKey first(top, frame, nonce);
  EXPECT_FALSE(first.ToCacheKeyString());
  ASSERT_TRUE(memory_key(first));
  EXPECT_EQ(memory_key(first), memory_key(net::NetworkIsolationKey(first)));
  EXPECT_NE(memory_key(first),
            memory_key(net::NetworkIsolationKey(
                top, frame, base::UnguessableToken::Create())));
  EXPECT_NE(memory_key(first),
            memory_key(net::NetworkIsolationKey(frame, top, nonce)));
  const auto opaque = net::NetworkIsolationKey::CreateTransientForTesting();
  ASSERT_TRUE(memory_key(opaque));
  EXPECT_EQ(memory_key(opaque), memory_key(net::NetworkIsolationKey(opaque)));
  EXPECT_NE(memory_key(opaque),
            memory_key(net::NetworkIsolationKey::CreateTransientForTesting()));
  EXPECT_FALSE(opaque.ToCacheKeyString());
  EXPECT_FALSE(memory_key(net::NetworkIsolationKey()));
  remote.reset();
  context.reset();
  service.reset();
  tasks.RunUntilIdle();
}

TEST_F(StatelessNetworkTest,
       ScopedMemoryKeepsRealCrossSiteHstsHeaderAndRetiresIt) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(net::features::kBravePartitionHSTS);
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = network::NetworkService::CreateForTesting();
  base::ScopedTempDir logs;
  ASSERT_TRUE(logs.CreateUniqueTempDir());
  int next_log = 0;
  auto create = [&](bool scoped,
                    mojo::Remote<network::mojom::NetworkContext>& remote) {
    auto params = network::CreateNetworkContextParamsForTesting();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->alloy_stateless = false;
    params->alloy_scoped_memory = scoped;
    params->file_paths.reset();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log =
        base::File(logs.GetPath().AppendASCII(
                       "guard-" + std::to_string(++next_log) + ".log"),
                   base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#else
    static_cast<void>(next_log);
#endif
    return network::NetworkContext::CreateForTesting(
        service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
        {});
  };
  mojo::Remote<network::mojom::NetworkContext> scoped_remote, peer_remote;
  auto scoped = create(true, scoped_remote);
  auto peer = create(false, peer_remote);
  auto* scoped_state =
      scoped->url_request_context()->transport_security_state();
  auto* peer_state = peer->url_request_context()->transport_security_state();
  ASSERT_TRUE(scoped_state && peer_state);
  const auto outer = url::Origin::Create(GURL("https://outer.test/"));
  const auto embedded = url::Origin::Create(GURL("https://embedded.test/"));
  const auto isolation = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, outer, embedded,
      net::SiteForCookies(), base::UnguessableToken::Create());
  const auto nak = isolation.network_anonymization_key();
  ASSERT_TRUE(isolation.network_isolation_key().IsTransient());
  ASSERT_FALSE(scoped_state->ShouldUpgradeToSSL(nak, "embedded.test", true));
  ASSERT_FALSE(peer_state->AddHSTSHeader(isolation, "embedded.test",
                                         "max-age=86400; includeSubDomains"));
  ASSERT_TRUE(scoped_state->AddHSTSHeader(isolation, "embedded.test",
                                          "max-age=86400; includeSubDomains"));
  ASSERT_TRUE(scoped_state->ShouldUpgradeToSSL(nak, "embedded.test", true));
  ASSERT_TRUE(
      scoped_state->ShouldUpgradeToSSL(nak, "child.embedded.test", true));
  ASSERT_FALSE(peer_state->ShouldUpgradeToSSL(nak, "embedded.test", true));
  const auto first_party = net::IsolationInfo::CreateForInternalRequest(outer);
  ASSERT_TRUE(
      peer_state->AddHSTSHeader(first_party, "outer.test", "max-age=86400"));
  ASSERT_TRUE(peer_state->ShouldUpgradeToSSL(
      first_party.network_anonymization_key(), "outer.test", true));
  RecordProperty(
      "positive_scoped_hsts_header",
      "real-parser-nonce-bearing-cross-site-policy-live-peer-isolated");
  scoped_remote.reset();
  scoped.reset();
  tasks_.RunUntilIdle();
  mojo::Remote<network::mojom::NetworkContext> fresh_remote;
  auto fresh = create(true, fresh_remote);
  EXPECT_FALSE(fresh->url_request_context()
                   ->transport_security_state()
                   ->ShouldUpgradeToSSL(nak, "embedded.test", true));
  EXPECT_TRUE(peer_state->ShouldUpgradeToSSL(
      first_party.network_anonymization_key(), "outer.test", true));
  if (!HasFailure())
    RecordProperty("retired_scoped_hsts_header",
                   "fresh-context-empty-live-peer-control-retained");
  fresh_remote.reset();
  peer_remote.reset();
  fresh.reset();
  peer.reset();
  service.reset();
  tasks_.RunUntilIdle();
}

TEST_F(StatelessNetworkTest,
       ScopedMemoryLearnsRealHttpsHstsResponseAndRetiresIt) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {net::features::kBravePartitionHSTS,
       net::features::kPartitionConnectionsByNetworkIsolationKey,
       network::features::kReporting, network::features::kNetworkErrorLogging},
      {});
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  std::atomic<int> requests = 0;
  net::EmbeddedTestServer server(net::EmbeddedTestServer::TYPE_HTTPS);
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* count, const net::test_server::HttpRequest&)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*count;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content_type("text/plain");
        response->AddCustomHeader("Cache-Control", "no-store");
        response->AddCustomHeader("Strict-Transport-Security",
                                  "max-age=86400; includeSubDomains");
        response->AddCustomHeader(
            "Alt-Svc", "h2=\"alternative.embedded.test:8443\"; ma=3600");
        response->AddCustomHeader(
            "Report-To",
            R"({"group":"scope","max_age":3600,"endpoints":[{"url":"https://collector.test/upload","priority":7,"weight":9}]})");
        response->AddCustomHeader(
            "NEL",
            R"({"report_to":"scope","max_age":3600,"include_subdomains":false,"success_fraction":0.0,"failure_fraction":1.0})");
        response->set_content("scoped-secure-header-body");
        return response;
      },
      &requests));
  ASSERT_TRUE(server.Start());
  auto service = network::NetworkService::CreateForTesting();
  net::MockHostResolverBase::RuleResolver rules;
  rules.AddRule("embedded.test", "127.0.0.1");
  service->set_host_resolver_factory_for_testing(
      std::make_unique<net::MockHostResolverFactory>(std::move(rules)));
  base::ScopedTempDir logs;
  ASSERT_TRUE(logs.CreateUniqueTempDir());
  int next_log = 0;
  auto create = [&](bool scoped,
                    mojo::Remote<network::mojom::NetworkContext>& remote) {
    auto params = network::CreateNetworkContextParamsForTesting();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->alloy_stateless = false;
    params->alloy_scoped_memory = scoped;
    params->file_paths.reset();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log =
        base::File(logs.GetPath().AppendASCII(
                       "response-guard-" + std::to_string(++next_log) + ".log"),
                   base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#else
    static_cast<void>(next_log);
#endif
    return network::NetworkContext::CreateForTesting(
        service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
        {});
  };
  const auto url = server.GetURL("embedded.test", "/learn-hsts");
  const auto outer = url::Origin::Create(GURL("https://outer.test/"));
  const auto embedded = url::Origin::Create(url);
  const auto isolation = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, outer, embedded,
      net::SiteForCookies(), base::UnguessableToken::Create());
  const auto nak = isolation.network_anonymization_key();
  ASSERT_TRUE(isolation.network_isolation_key().IsTransient());
  auto fetch = [&](mojo::Remote<network::mojom::NetworkContext>& remote) {
    auto params = network::mojom::URLLoaderFactoryParams::New();
    params->process_id = network::OriginatingProcessId::browser();
    params->is_orb_enabled = false;
    params->isolation_info = isolation;
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                   std::move(params));
    network::ResourceRequest request;
    request.method = "GET";
    request.url = url;
    request.request_initiator = embedded;
    request.site_for_cookies = net::SiteForCookies();
    request.credentials_mode = network::mojom::CredentialsMode::kInclude;
    network::TestURLLoaderClient client;
    mojo::Remote<network::mojom::URLLoader> loader;
    net::MutableNetworkTrafficAnnotationTag annotation;
    annotation.unique_id_hash_code =
        network::AlloyGuard::Hash("blink_resource_loader");
    factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(), 1, 0,
                                  request, client.CreateRemote(), annotation);
    client.RunUntilComplete();
    const auto error = client.completion_status().error_code;
    std::string body;
    if (error == net::OK)
      EXPECT_TRUE(
          mojo::BlockingCopyToString(client.response_body_release(), &body));
    return std::make_pair(error, body);
  };
  const auto expected =
      std::make_pair(net::OK, std::string("scoped-secure-header-body"));
  mojo::Remote<network::mojom::NetworkContext> scoped_remote, peer_remote,
      ordinary_remote;
  auto scoped = create(true, scoped_remote);
  auto peer = create(true, peer_remote);
  auto ordinary = create(false, ordinary_remote);
  auto upgrade = [&](const std::unique_ptr<network::NetworkContext>& context) {
    return context->url_request_context()
        ->transport_security_state()
        ->ShouldUpgradeToSSL(nak, "embedded.test", true);
  };
  for (auto* context : {scoped.get(), peer.get(), ordinary.get()}) {
    auto* reporting = context->url_request_context()->reporting_service();
    auto* nel = context->url_request_context()->network_error_logging_service();
    ASSERT_TRUE(reporting && nel);
    ASSERT_FALSE(reporting->GetContextForTesting()->store());
    ASSERT_FALSE(nel->GetPersistentNelStoreForTesting());
  }
  const net::ReportingEndpointGroupKey group_key(
      nak, embedded, "scope", net::ReportingTargetType::kDeveloper);
  auto endpoints =
      [&](const std::unique_ptr<network::NetworkContext>& context) {
        return context->url_request_context()
            ->reporting_service()
            ->GetContextForTesting()
            ->cache()
            ->GetCandidateEndpointsForDelivery(group_key);
      };
  auto nel_keys = [&](const std::unique_ptr<network::NetworkContext>& context) {
    return context->url_request_context()
        ->network_error_logging_service()
        ->GetPolicyKeysForTesting();
  };
  auto check_config = [&](const std::unique_ptr<network::NetworkContext>&
                              context) {
    const auto candidates = endpoints(context);
    ASSERT_EQ(1u, candidates.size());
    ASSERT_EQ(group_key, candidates[0].group_key);
    ASSERT_EQ(GURL("https://collector.test/upload"), candidates[0].info.url);
    ASSERT_EQ(7, candidates[0].info.priority);
    ASSERT_EQ(9, candidates[0].info.weight);
    const auto keys = nel_keys(context);
    ASSERT_EQ(1u, keys.size());
    ASSERT_EQ(nak, keys.begin()->network_anonymization_key);
    ASSERT_EQ(embedded, keys.begin()->origin);
    const auto status = context->url_request_context()
                            ->network_error_logging_service()
                            ->StatusAsValue();
    const auto* policies = status.GetDict().FindList("originPolicies");
    ASSERT_TRUE(policies);
    ASSERT_EQ(1u, policies->size());
    const auto& policy = (*policies)[0].GetDict();
    const auto* origin = policy.FindString("origin");
    const auto* report_to = policy.FindString("reportTo");
    ASSERT_TRUE(origin && report_to);
    ASSERT_EQ(embedded.Serialize(), *origin);
    ASSERT_EQ("scope", *report_to);
    ASSERT_EQ(std::optional<bool>(false), policy.FindBool("includeSubdomains"));
    ASSERT_EQ(std::optional<double>(0.0), policy.FindDouble("successFraction"));
    ASSERT_EQ(std::optional<double>(1.0), policy.FindDouble("failureFraction"));
  };
  ASSERT_TRUE(endpoints(scoped).empty());
  ASSERT_TRUE(endpoints(peer).empty());
  ASSERT_TRUE(nel_keys(scoped).empty());
  ASSERT_TRUE(nel_keys(peer).empty());
  auto alternatives =
      [&](const std::unique_ptr<network::NetworkContext>& context) {
        return context->url_request_context()
            ->http_server_properties()
            ->GetAlternativeServiceInfos(url::SchemeHostPort(url), nak);
      };
  ASSERT_TRUE(alternatives(scoped).empty());
  ASSERT_TRUE(alternatives(peer).empty());
  ASSERT_FALSE(upgrade(scoped));
  ASSERT_FALSE(upgrade(peer));
  ASSERT_EQ(expected, fetch(ordinary_remote));
  ASSERT_FALSE(upgrade(ordinary));
  ASSERT_EQ(1u, alternatives(ordinary).size());
  ASSERT_NO_FATAL_FAILURE(check_config(ordinary));
  ASSERT_EQ(expected, fetch(scoped_remote));
  ASSERT_NO_FATAL_FAILURE(check_config(scoped));
  ASSERT_TRUE(upgrade(scoped));
  ASSERT_TRUE(scoped->url_request_context()
                  ->transport_security_state()
                  ->ShouldUpgradeToSSL(nak, "child.embedded.test", true));
  const auto learned = alternatives(scoped);
  ASSERT_EQ(1u, learned.size());
  ASSERT_EQ(net::NextProto::kProtoHTTP2, learned[0].protocol());
  ASSERT_EQ("alternative.embedded.test", learned[0].GetHostPortPair().host());
  ASSERT_EQ(8443, learned[0].GetHostPortPair().port());
  ASSERT_TRUE(alternatives(peer).empty());
  ASSERT_TRUE(endpoints(peer).empty());
  ASSERT_TRUE(nel_keys(peer).empty());
  ASSERT_FALSE(upgrade(peer));
  ASSERT_EQ(expected, fetch(peer_remote));
  ASSERT_NO_FATAL_FAILURE(check_config(peer));
  ASSERT_TRUE(upgrade(peer));
  const auto peer_learned = alternatives(peer);
  ASSERT_EQ(1u, peer_learned.size());
  ASSERT_EQ(learned[0].alternative_service(),
            peer_learned[0].alternative_service());
  auto* scoped_properties =
      scoped->url_request_context()->http_server_properties();
  auto* peer_properties = peer->url_request_context()->http_server_properties();
  ASSERT_FALSE(scoped_properties->IsAlternativeServiceBroken(
      learned[0].alternative_service(), nak));
  scoped_properties->MarkAlternativeServiceBroken(
      learned[0].alternative_service(), nak);
  ASSERT_TRUE(scoped_properties->IsAlternativeServiceBroken(
      learned[0].alternative_service(), nak));
  ASSERT_FALSE(peer_properties->IsAlternativeServiceBroken(
      learned[0].alternative_service(), nak));
#if !BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  // Configuration remains usable; the product never retains an activity queue.
  auto* reporting = scoped->url_request_context()->reporting_service();
  base::DictValue queue_probe;
  queue_probe.Set("probe", "forbidden-product-queue");
  reporting->QueueReport(url, std::nullopt, nak, "fixture", "scope",
                         "scope-test", std::move(queue_probe), 0,
                         net::ReportingTargetType::kDeveloper);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(reporting->GetReports().empty());
  const auto reports =
      reporting->GetContextForTesting()->cache()->GetReportsAsValue();
  ASSERT_TRUE(reports.is_list());
  ASSERT_TRUE(reports.GetList().empty());
#endif
  ASSERT_EQ(3, requests.load());
  RecordProperty(
      "positive_scoped_reporting_nel_config",
      "real-https-endpoint-and-nel-values-memory-owner-peer-isolated");
  RecordProperty(
      "positive_scoped_server_properties",
      "real-https-alt-svc-h2-host-port-and-native-brokenness-peer-isolated");
  RecordProperty(
      "positive_scoped_hsts_response",
      "real-https-url-loader-header-learned-cross-site-nonce-peer-isolated");
  scoped_remote.reset();
  scoped.reset();
  tasks_.RunUntilIdle();
  mojo::Remote<network::mojom::NetworkContext> fresh_remote;
  auto fresh = create(true, fresh_remote);
  EXPECT_FALSE(upgrade(fresh));
  ASSERT_TRUE(fresh->url_request_context()->reporting_service());
  ASSERT_TRUE(fresh->url_request_context()->network_error_logging_service());
  EXPECT_TRUE(endpoints(fresh).empty());
  EXPECT_TRUE(nel_keys(fresh).empty());
  ASSERT_NO_FATAL_FAILURE(check_config(peer));
  EXPECT_TRUE(alternatives(fresh).empty());
  EXPECT_FALSE(
      fresh->url_request_context()
          ->http_server_properties()
          ->IsAlternativeServiceBroken(learned[0].alternative_service(), nak));
  const auto retained = alternatives(peer);
  ASSERT_EQ(1u, retained.size());
  EXPECT_EQ(learned[0].alternative_service(),
            retained[0].alternative_service());
  EXPECT_FALSE(peer_properties->IsAlternativeServiceBroken(
      learned[0].alternative_service(), nak));
  EXPECT_TRUE(upgrade(peer));
  EXPECT_FALSE(upgrade(ordinary));
  EXPECT_EQ(3, requests.load());
  if (!HasFailure()) {
    RecordProperty("retired_scoped_reporting_nel_config",
                   "fresh-endpoints-policy-empty-live-peer-config-retained");
    RecordProperty("retired_scoped_hsts_response",
                   "fresh-context-empty-peer-transport-policy-live");
    RecordProperty(
        "retired_scoped_server_properties",
        "fresh-alt-svc-and-brokenness-empty-live-peer-value-retained");
  }
  fresh_remote.reset();
  peer_remote.reset();
  ordinary_remote.reset();
  fresh.reset();
  peer.reset();
  ordinary.reset();
  service.reset();
  tasks_.RunUntilIdle();
}
TEST_F(StatelessNetworkTest,
       ScopedMemoryKeepsRealSharedDictionaryBytesAndRetiresGetter) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  std::atomic<int> requests = 0;
  const std::string payload = "ALLOY_SHARED_DICTIONARY_REAL_BODY_83";
  net::EmbeddedTestServer server(net::EmbeddedTestServer::TYPE_HTTPS);
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* count, const std::string& bytes,
         const net::test_server::HttpRequest&)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*count;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content_type("application/octet-stream");
        response->AddCustomHeader("Cache-Control", "max-age=3600");
        response->AddCustomHeader(
            "Use-As-Dictionary",
            "match=\"/resource/*\", id=\"scope-dictionary\"");
        response->set_content(bytes);
        return response;
      },
      &requests, payload));
  ASSERT_TRUE(server.Start());
  auto service = network::NetworkService::CreateForTesting();
  net::MockHostResolverBase::RuleResolver rules;
  rules.AddRule("embedded.test", "127.0.0.1");
  service->set_host_resolver_factory_for_testing(
      std::make_unique<net::MockHostResolverFactory>(std::move(rules)));
  base::ScopedTempDir logs;
  ASSERT_TRUE(logs.CreateUniqueTempDir());
  int next_log = 0;
  auto create = [&](bool scoped,
                    mojo::Remote<network::mojom::NetworkContext>& remote) {
    auto params = network::CreateNetworkContextParamsForTesting();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    params->alloy_stateless = false;
    params->alloy_scoped_memory = scoped;
    params->shared_dictionary_enabled = true;
    params->file_paths.reset();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log = base::File(
        logs.GetPath().AppendASCII("dictionary-guard-" +
                                   std::to_string(++next_log) + ".log"),
        base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#else
    static_cast<void>(next_log);
#endif
    return network::NetworkContext::CreateForTesting(
        service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
        {});
  };
  const GURL dictionary_url = server.GetURL("embedded.test", "/dictionary");
  const GURL resource_url = server.GetURL("embedded.test", "/resource/value");
  const auto embedded = url::Origin::Create(dictionary_url);
  const auto outer = url::Origin::Create(GURL("https://outer.test/"));
  const auto nonce = base::UnguessableToken::Create();
  auto fetch = [&](mojo::Remote<network::mojom::NetworkContext>& remote,
                   bool scoped) {
    auto params = network::mojom::URLLoaderFactoryParams::New();
    params->process_id = network::OriginatingProcessId::browser();
    params->is_orb_enabled = false;
    params->client_security_state =
        network::ClientSecurityStateBuilder().WithIsSecureContext(true).Build();
    params->isolation_info = net::IsolationInfo::Create(
        net::IsolationInfo::RequestType::kOther, outer, embedded,
        net::SiteForCookies(),
        scoped ? std::make_optional(nonce) : std::nullopt);
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                   std::move(params));
    network::ResourceRequest request;
    request.method = "GET";
    request.url = dictionary_url;
    request.mode = network::mojom::RequestMode::kSameOrigin;
    request.request_initiator = embedded;
    request.site_for_cookies = net::SiteForCookies();
    request.credentials_mode = network::mojom::CredentialsMode::kInclude;
    request.shared_dictionary_writer_enabled = true;
    network::TestURLLoaderClient client;
    mojo::Remote<network::mojom::URLLoader> loader;
    net::MutableNetworkTrafficAnnotationTag annotation;
    annotation.unique_id_hash_code =
        network::AlloyGuard::Hash("blink_resource_loader");
    factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(), 1, 0,
                                  request, client.CreateRemote(), annotation);
    client.RunUntilComplete();
    const int error = client.completion_status().error_code;
    std::string body;
    if (error == net::OK)
      EXPECT_TRUE(
          mojo::BlockingCopyToString(client.response_body_release(), &body));
    tasks_.RunUntilIdle();
    return std::make_pair(error, body);
  };
  auto usage = [&](const std::unique_ptr<network::NetworkContext>& context) {
    base::test::TestFuture<std::vector<net::SharedDictionaryUsageInfo>> future;
    context->GetSharedDictionaryManager()->GetUsageInfo(
        future
            .GetCallback<const std::vector<net::SharedDictionaryUsageInfo>&>());
    return future.Take();
  };
  auto check_bytes =
      [&](const std::unique_ptr<network::NetworkContext>& context) {
        const auto entries = usage(context);
        ASSERT_EQ(1u, entries.size());
        ASSERT_EQ(payload.size(), entries[0].total_size_bytes);
        const auto storage = context->GetSharedDictionaryManager()->GetStorage(
            entries[0].isolation_key);
        auto dictionary = storage->GetDictionarySync(
            resource_url, network::mojom::RequestDestination::kEmpty);
        ASSERT_TRUE(dictionary);
        base::test::TestFuture<int> read;
        int result = dictionary->ReadAll(read.GetCallback());
        if (result == net::ERR_IO_PENDING)
          result = read.Get();
        ASSERT_EQ(net::OK, result);
        ASSERT_EQ(payload.size(), dictionary->size());
        ASSERT_EQ("scope-dictionary", dictionary->id());
        ASSERT_EQ(payload,
                  std::string(dictionary->data()->data(), dictionary->size()));
      };
  mojo::Remote<network::mojom::NetworkContext> control_remote, scoped_remote,
      peer_remote;
  auto control = create(false, control_remote);
  auto scoped = create(true, scoped_remote);
  auto peer = create(true, peer_remote);
  ASSERT_TRUE(control->GetSharedDictionaryManager());
  ASSERT_TRUE(scoped->GetSharedDictionaryManager());
  ASSERT_TRUE(peer->GetSharedDictionaryManager());
  {
    const auto policy_isolation = net::IsolationInfo::Create(
        net::IsolationInfo::RequestType::kOther, outer, embedded,
        net::SiteForCookies(), nonce);
    const auto policy_partition_key =
        net::CookiePartitionKey::FromNetworkIsolationKey(
            policy_isolation.network_isolation_key(), net::SiteForCookies(),
            net::SchemefulSite(dictionary_url), false);
    ASSERT_TRUE(policy_partition_key);
    ASSERT_FALSE(
        scoped->cookie_manager()->cookie_settings().IsFullCookieAccessAllowed(
            dictionary_url, net::SiteForCookies(), outer,
            net::CookieSettingOverrides(), policy_partition_key));
    network::SharedDictionaryAccessChecker scope_access(
        *scoped,
        static_cast<network::mojom::SharedDictionaryAccessObserver*>(nullptr));
    network::SharedDictionaryAccessChecker ordinary_access(
        *control,
        static_cast<network::mojom::SharedDictionaryAccessObserver*>(nullptr));
    ASSERT_TRUE(scope_access.CheckAllowedToWriteAndReport(
        dictionary_url, net::SiteForCookies(), policy_isolation));
    ASSERT_TRUE(scope_access.CheckAllowedToReadAndReport(
        resource_url, net::SiteForCookies(), policy_isolation,
        policy_partition_key));
    ASSERT_FALSE(ordinary_access.CheckAllowedToWriteAndReport(
        dictionary_url, net::SiteForCookies(), policy_isolation));
    ASSERT_FALSE(ordinary_access.CheckAllowedToReadAndReport(
        resource_url, net::SiteForCookies(), policy_isolation,
        policy_partition_key));
    mojo::Remote<network::mojom::NetworkContext> blocked_remote;
    auto blocked = create(true, blocked_remote);
    base::RunLoop settings_done;
    blocked->cookie_manager()->SetContentSettings(
        ContentSettingsType::COOKIES,
        {ContentSettingPatternSource(
            ContentSettingsPattern::FromURL(dictionary_url),
            ContentSettingsPattern::Wildcard(),
            base::Value(CONTENT_SETTING_BLOCK),
            content_settings::mojom::ProviderType::kNone, false)},
        settings_done.QuitClosure());
    settings_done.Run();
    network::SharedDictionaryAccessChecker blocked_access(
        *blocked,
        static_cast<network::mojom::SharedDictionaryAccessObserver*>(nullptr));
    ASSERT_FALSE(blocked_access.CheckAllowedToWriteAndReport(
        dictionary_url, net::SiteForCookies(), policy_isolation));
    ASSERT_FALSE(blocked_access.CheckAllowedToReadAndReport(
        resource_url, net::SiteForCookies(), policy_isolation,
        policy_partition_key));
  }
  tasks_.RunUntilIdle();
  const auto expected = std::make_pair(int(net::OK), payload);
  ASSERT_EQ(expected, fetch(control_remote, false));
  ASSERT_NO_FATAL_FAILURE(check_bytes(control));
  RecordProperty("positive_shared_dictionary_control",
                 "nonce-free-real-https-dictionary-body-registered-and-read");
  ASSERT_TRUE(usage(scoped).empty());
  ASSERT_TRUE(usage(peer).empty());
  ASSERT_EQ(expected, fetch(scoped_remote, true));
  ASSERT_NO_FATAL_FAILURE(check_bytes(scoped));
  ASSERT_TRUE(usage(peer).empty());
  ASSERT_EQ(expected, fetch(peer_remote, true));
  ASSERT_NO_FATAL_FAILURE(check_bytes(peer));
  const auto scoped_usage = usage(scoped);
  ASSERT_EQ(1u, scoped_usage.size());
  const auto held_key = scoped_usage[0].isolation_key;
  ASSERT_EQ(std::make_optional(nonce), held_key.nonce());
  const auto isolation =
      net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther, outer,
                                 embedded, net::SiteForCookies(), nonce);
  ASSERT_EQ(std::make_optional(held_key),
            net::SharedDictionaryIsolationKey::MaybeCreate(isolation));
  ASSERT_EQ(std::make_optional(held_key),
            net::SharedDictionaryIsolationKey::MaybeCreate(
                isolation.network_isolation_key(), embedded));
  base::test::TestFuture<std::vector<net::SharedDictionaryUsageInfo>> ipc_usage;
  scoped_remote->GetSharedDictionaryUsageInfo(
      ipc_usage
          .GetCallback<const std::vector<net::SharedDictionaryUsageInfo>&>());
  ASSERT_EQ(scoped_usage, ipc_usage.Get());
  ASSERT_FALSE(control->GetSharedDictionaryManager()->GetStorage(held_key));
  base::ScopedTempDir disk_directory;
  ASSERT_TRUE(disk_directory.CreateUniqueTempDir());
  auto disk_manager = network::SharedDictionaryManager::CreateOnDisk(
      disk_directory.GetPath().AppendASCII("db"),
      disk_directory.GetPath().AppendASCII("cache"), std::nullopt, 1000,
      nullptr);
  ASSERT_FALSE(disk_manager->GetStorage(held_key));
  base::test::TestFuture<std::vector<network::mojom::SharedDictionaryInfoPtr>>
      disk_info;
  disk_manager->GetSharedDictionaryInfo(held_key, disk_info.GetCallback());
  ASSERT_TRUE(disk_info.Get().empty());
  disk_manager.reset();
  tasks_.RunUntilIdle();
  auto held_getter =
      scoped->GetSharedDictionaryManager()->MaybeCreateSharedDictionaryGetter(
          net::LOAD_CAN_USE_SHARED_DICTIONARY,
          network::mojom::RequestDestination::kEmpty);
  ASSERT_TRUE(held_getter);
  const net::SharedDictionaryIsolationKey other_nonce_key(
      held_key.frame_origin(), held_key.top_frame_site(),
      base::UnguessableToken::Create());
  ASSERT_NE(held_key, other_nonce_key);
  ASSERT_FALSE(held_getter.Run(other_nonce_key, resource_url));
  ASSERT_TRUE(held_getter.Run(held_key, resource_url));
  ASSERT_EQ(3, requests.load());
  RecordProperty(
      "positive_scoped_shared_dictionary",
      "native-nonce-real-https-registration-readall-bytes-peer-isolated");
  scoped_remote.reset();
  scoped.reset();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(held_getter.Run(held_key, resource_url));
  mojo::Remote<network::mojom::NetworkContext> fresh_remote;
  auto fresh = create(true, fresh_remote);
  EXPECT_TRUE(usage(fresh).empty());
  ASSERT_NO_FATAL_FAILURE(check_bytes(peer));
  EXPECT_EQ(3, requests.load());
  if (!HasFailure())
    RecordProperty(
        "retired_scoped_shared_dictionary",
        "old-native-getter-denied-fresh-empty-live-peer-bytes-retained");
  fresh_remote.reset();
  peer_remote.reset();
  control_remote.reset();
  fresh.reset();
  peer.reset();
  control.reset();
  service.reset();
  tasks_.RunUntilIdle();
}
}  // namespace
}  // namespace alloy::site_records
