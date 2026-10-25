// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <atomic>
#include <memory>
#include <string>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/cookies/site_for_cookies.h"
#include "net/net_buildflags.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/url_request/url_request_context.h"
#include "services/network/alloy_guard.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {

class StatelessNetworkTest : public testing::Test {
 protected:
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};

  void CheckConsumers(bool stateless) {
    auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
    std::atomic<int> cache_requests = 0;
    net::EmbeddedTestServer server;
    server.RegisterRequestHandler(base::BindRepeating(
        [](std::atomic<int>* requests, const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response = std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_code(net::HTTP_OK);
          response->set_content_type("text/plain");
          if (request.relative_url == "/cacheable") {
            response->AddCustomHeader("Cache-Control", "public, max-age=3600");
            response->AddCustomHeader("Set-Cookie", "alloy_state=retained; Path=/");
            response->set_content("body-"+std::to_string(++*requests));
          } else {
            response->AddCustomHeader("Cache-Control", "no-store");
            response->set_content(request.headers.contains("Cookie")
                                      ? "cookie-seen" : "cookie-empty");
          }
          return response;
        }, &cache_requests));
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
    params->alloy_guard_log = base::File(log_dir.GetPath().AppendASCII("guard.log"),
        base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
    mojo::Remote<network::mojom::NetworkContext> remote;
    auto context = network::NetworkContext::CreateForTesting(service.get(),
        remote.BindNewPipeAndPassReceiver(), std::move(params), {});
    if (stateless) {
#if BUILDFLAG(ENABLE_REPORTING)
      EXPECT_EQ(nullptr, context->url_request_context()->reporting_service());
      EXPECT_EQ(nullptr, context->url_request_context()->network_error_logging_service());
#endif
    }
    const auto origin = url::Origin::Create(server.base_url());
    auto factory_params = network::mojom::URLLoaderFactoryParams::New();
    factory_params->process_id = network::OriginatingProcessId::browser();
    factory_params->is_orb_enabled = false;
    factory_params->isolation_info = net::IsolationInfo::CreateForInternalRequest(origin);
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                   std::move(factory_params));
    int request_id = 0;
    auto fetch = [&](const char* path) {
      network::ResourceRequest request;
      request.method = "GET";
      request.url = server.GetURL(path);
      request.request_initiator = origin;
      request.site_for_cookies = net::SiteForCookies::FromUrl(server.base_url());
      request.credentials_mode = network::mojom::CredentialsMode::kInclude;
      network::TestURLLoaderClient client;
      mojo::Remote<network::mojom::URLLoader> loader;
      net::MutableNetworkTrafficAnnotationTag annotation;
      annotation.unique_id_hash_code = network::AlloyGuard::Hash("blink_resource_loader");
      factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(),
          ++request_id, 0, request, client.CreateRemote(), annotation);
      client.RunUntilComplete();
      EXPECT_EQ(net::OK, client.completion_status().error_code);
      std::string body;
      EXPECT_TRUE(mojo::BlockingCopyToString(client.response_body_release(), &body));
      return body;
    };
    EXPECT_EQ("body-1", fetch("/cacheable"));
    EXPECT_EQ(stateless ? "body-2" : "body-1", fetch("/cacheable"));
    EXPECT_EQ(stateless ? 2 : 1, cache_requests.load());
    EXPECT_EQ(stateless ? "cookie-empty" : "cookie-seen", fetch("/cookie-check"));
    tasks_.RunUntilIdle();
    if (stateless) EXPECT_TRUE(base::IsDirectoryEmpty(data_dir.GetPath()));
    factory.reset(); remote.reset(); context.reset(); service.reset();
    tasks_.RunUntilIdle();
    if (stateless) EXPECT_TRUE(base::IsDirectoryEmpty(data_dir.GetPath()));
  }
};

TEST_F(StatelessNetworkTest, NativeControlActuallyCachesAndKeepsCookies) {
  CheckConsumers(false);
}

TEST_F(StatelessNetworkTest, RejectsCookiesCachesAndPersistentBackendsAtAdmission) {
  CheckConsumers(true);
}

}  // namespace
}  // namespace alloy::site_records
