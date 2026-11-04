// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <atomic>
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/network_retirement_test_context.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/alloy_traffic_class.h"
#include "net/base/auth.h"
#include "net/base/network_change_notifier.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/controllable_http_response.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/network/cors/preflight_controller.h"
#include "services/network/cors/preflight_result.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/test_url_loader_network_observer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
class HeldLoaderAuth final : public network::TestURLLoaderNetworkObserver {
 public:
  void OnAuthRequired(
      const std::optional<base::UnguessableToken>&,
      int32_t,
      const GURL&,
      bool,
      const net::AuthChallengeInfo&,
      const scoped_refptr<net::HttpResponseHeaders>&,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder> responder)
      override {
    reply.Bind(std::move(responder));
    requested.SetValue();
  }
  mojo::Remote<network::mojom::AuthChallengeResponder> reply;
  base::test::TestFuture<void> requested;
};

class LoaderRetirementTest : public testing::Test {
 public:
  LoaderRetirementTest()
      : alpha_stream_(&server_, "/alpha-stream"),
        beta_stream_(&server_, "/beta-stream"),
        options_(&server_, "/preflight") {
    server_.RegisterRequestHandler(base::BindRepeating(
        &LoaderRetirementTest::Handle, base::Unretained(this)));
    CHECK(server_.Start());
  }
  std::unique_ptr<net::test_server::HttpResponse> Handle(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    response->AddCustomHeader("Cache-Control", "no-store");
    if (request.relative_url == "/auth") {
      ++auth_requests_;
      response->set_code(net::HTTP_UNAUTHORIZED);
      response->AddCustomHeader("WWW-Authenticate",
                                "Basic realm=\"loader-retirement\"");
    } else if (request.relative_url == "/redirect") {
      response->set_code(net::HTTP_FOUND);
      response->AddCustomHeader(
          "Location", server_.GetURL("transport.test", "/after").spec());
    } else {
      if (request.relative_url == "/after")
        ++after_requests_;
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/plain");
      response->set_content("beta-data");
    }
    return response;
  }
  static url::Origin Origin(const char* owner) {
    return url::Origin::Create(GURL(std::string("https://") + owner));
  }
  static net::IsolationInfo Isolation(const char* owner) {
    return net::IsolationInfo::CreateForInternalRequest(Origin(owner));
  }
  mojo::Remote<network::mojom::URLLoaderFactory> Factory(
      const char* scoped_owner = nullptr,
      mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver>
          observer = {}) {
    auto params = network::mojom::URLLoaderFactoryParams::New();
    params->is_orb_enabled = false;
    if (scoped_owner) {
      params->process_id = network::OriginatingProcessId::renderer(
          network::RendererProcessId(41));
      params->request_initiator_origin_lock = Origin(scoped_owner);
      params->isolation_info = Isolation(scoped_owner);
    } else {
      params->process_id = network::OriginatingProcessId::browser();
      params->is_trusted = true;
    }
    params->url_loader_network_observer = std::move(observer);
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    network_.context().CreateURLLoaderFactory(
        factory.BindNewPipeAndPassReceiver(), std::move(params));
    return factory;
  }
  network::ResourceRequest Request(const char* owner,
                                   const GURL& url,
                                   bool trusted) {
    network::ResourceRequest request;
    request.url = url;
    request.method = "GET";
    request.request_initiator = Origin(owner);
    request.site_for_cookies =
        net::SiteForCookies::FromUrl(Origin(owner).GetURL());
    request.credentials_mode = network::mojom::CredentialsMode::kOmit;
    request.mode = network::mojom::RequestMode::kNoCors;
    if (trusted) {
      request.trusted_params = network::ResourceRequest::TrustedParams();
      request.trusted_params->isolation_info = Isolation(owner);
    }
    return request;
  }
  mojo::Remote<network::mojom::URLLoader> Send(
      network::mojom::URLLoaderFactory* factory,
      const network::ResourceRequest& request,
      network::TestURLLoaderClient& client) {
    mojo::Remote<network::mojom::URLLoader> loader;
    net::MutableNetworkTrafficAnnotationTag annotation;
    annotation.unique_id_hash_code =
        net::AlloyAnnotationHash("blink_resource_loader");
    factory->CreateLoaderAndStart(loader.BindNewPipeAndPassReceiver(),
                                  ++request_id_, 0, request,
                                  client.CreateRemote(), annotation);
    return loader;
  }
  base::OnceClosure Prepare(const char* owner, base::OnceClosure done) {
    return network_.context().PrepareURLLoaderServiceRetirement(
        base::BindRepeating(
            [](net::NetworkAnonymizationKey key,
               const network::NativeNetworkOwner& candidate) {
              return key == candidate.network_key;
            },
            Isolation(owner).network_anonymization_key()),
        std::move(done));
  }

 protected:
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
  std::unique_ptr<net::NetworkChangeNotifier> notifier_ =
      net::NetworkChangeNotifier::CreateMockIfNeeded();
  std::atomic<int> auth_requests_{0}, after_requests_{0};
  net::EmbeddedTestServer server_;
  net::test_server::ControllableHttpResponse alpha_stream_, beta_stream_;
  net::test_server::ControllableHttpResponseManager options_;
  NetworkRetirementTestContext network_;
  int request_id_ = 0;
};

TEST_F(LoaderRetirementTest, ScopedFactoryClonesCloseAndQueuedCreateIsAborted) {
  network_.resolver().set_ondemand_mode(true);
  auto alpha = Factory("alpha.test"), beta = Factory("beta.test");
  mojo::Remote<network::mojom::URLLoaderFactory> clone;
  alpha->Clone(clone.BindNewPipeAndPassReceiver());
  alpha.FlushForTesting();
  network::TestURLLoaderClient original, unrelated, queued;
  auto a =
      Send(alpha.get(),
           Request("alpha.test", GURL("https://alpha.test/pending"), false),
           original);
  auto b = Send(beta.get(),
                Request("beta.test", GURL("https://beta.test/pending"), false),
                unrelated);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(original.has_received_completion());
  ASSERT_FALSE(unrelated.has_received_completion());
  base::test::TestFuture<void> drained, clone_closed;
  clone.set_disconnect_handler(clone_closed.GetCallback());
  auto commit = Prepare("alpha.test", drained.GetCallback());
  auto late = Send(
      clone.get(),
      Request("alpha.test", GURL("https://alpha.test/late"), false), queued);
  queued.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, queued.completion_status().error_code);
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  original.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, original.completion_status().error_code);
  ASSERT_TRUE(clone_closed.Wait());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(unrelated.has_received_completion());
  EXPECT_TRUE(beta.is_connected());
  base::test::TestFuture<void> cleanup;
  auto close = Prepare("beta.test", cleanup.GetCallback());
  std::move(close).Run();
  unrelated.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, unrelated.completion_status().error_code);
  ASSERT_TRUE(cleanup.Wait());
}

TEST_F(LoaderRetirementTest, SharedBrowserFactoryKeepsBetaAndAdmitsFreshAlpha) {
  network_.resolver().set_ondemand_mode(true);
  auto factory = Factory();
  network::TestURLLoaderClient alpha, beta, fresh;
  auto a = Send(
      factory.get(),
      Request("alpha.test", GURL("https://transport.test/alpha"), true), alpha);
  auto b = Send(factory.get(),
                Request("beta.test", GURL("https://transport.test/beta"), true),
                beta);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(alpha.has_received_completion());
  ASSERT_FALSE(beta.has_received_completion());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  alpha.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(factory.is_connected());
  EXPECT_FALSE(beta.has_received_completion());
  auto f = Send(
      factory.get(),
      Request("alpha.test", GURL("https://transport.test/fresh"), true), fresh);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(fresh.has_received_completion());
  base::test::TestFuture<void> close_alpha, close_beta;
  auto ca = Prepare("alpha.test", close_alpha.GetCallback());
  std::move(ca).Run();
  fresh.RunUntilComplete();
  ASSERT_TRUE(close_alpha.Wait());
  auto cb = Prepare("beta.test", close_beta.GetCallback());
  std::move(cb).Run();
  beta.RunUntilComplete();
  ASSERT_TRUE(close_beta.Wait());
}

TEST_F(LoaderRetirementTest, ActiveBodyAbortsWhileBetaContinuesToConsumer) {
  auto factory = Factory();
  network::TestURLLoaderClient alpha, beta;
  auto a =
      Send(factory.get(),
           Request("alpha.test",
                   server_.GetURL("transport.test", "/alpha-stream"), true),
           alpha);
  auto b = Send(factory.get(),
                Request("beta.test",
                        server_.GetURL("transport.test", "/beta-stream"), true),
                beta);
  alpha_stream_.WaitForRequest();
  beta_stream_.WaitForRequest();
  alpha_stream_.Send(
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Content-Type-Options: "
      "nosniff\r\nContent-Length: 64\r\n\r\nalpha-");
  beta_stream_.Send(
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Content-Type-Options: "
      "nosniff\r\nContent-Length: 12\r\n\r\nbefore-");
  alpha.RunUntilResponseReceived();
  beta.RunUntilResponseReceived();
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  alpha.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.has_received_completion());
  beta_stream_.Send("after");
  beta_stream_.Done();
  beta.RunUntilComplete();
  EXPECT_EQ(net::OK, beta.completion_status().error_code);
  std::string body;
  EXPECT_TRUE(mojo::BlockingCopyToString(beta.response_body_release(), &body));
  EXPECT_EQ("before-after", body);
}

TEST_F(LoaderRetirementTest,
       HeldAuthAndDeferredRedirectCannotResumeAfterRetirement) {
  HeldLoaderAuth auth;
  auto factory = Factory(nullptr, auth.Bind());
  network::TestURLLoaderClient alpha, redirect;
  auto auth_request =
      Request("alpha.test", server_.GetURL("transport.test", "/auth"), true);
  auth_request.credentials_mode = network::mojom::CredentialsMode::kInclude;
  auto a = Send(factory.get(), auth_request, alpha);
  ASSERT_TRUE(auth.requested.Wait());
  EXPECT_EQ(1, auth_requests_.load());
  auto r = Send(factory.get(),
                Request("alpha.test",
                        server_.GetURL("transport.test", "/redirect"), true),
                redirect);
  redirect.RunUntilRedirectReceived();
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  alpha.RunUntilComplete();
  redirect.RunUntilComplete();
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  EXPECT_EQ(net::ERR_ABORTED, redirect.completion_status().error_code);
  auth.reply->OnAuthCredentials(net::AuthCredentials(u"user", u"password"));
  r->FollowRedirect(network::HttpRequestHeadersUpdateParams(), std::nullopt);
  network_.remote().FlushForTesting();
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, auth_requests_.load());
  EXPECT_EQ(0, after_requests_.load());
  network::TestURLLoaderClient beta;
  auto b = Send(
      factory.get(),
      Request("beta.test", server_.GetURL("transport.test", "/echo"), true),
      beta);
  beta.RunUntilComplete();
  EXPECT_EQ(net::OK, beta.completion_status().error_code);
  std::string body;
  EXPECT_TRUE(mojo::BlockingCopyToString(beta.response_body_release(), &body));
  EXPECT_EQ("beta-data", body);
}

TEST_F(LoaderRetirementTest,
       PendingPreflightCannotPublishOrStartActualRequest) {
  auto factory = Factory();
  network::TestURLLoaderClient original, fresh;
  auto request = Request("alpha.test",
                         server_.GetURL("transport.test", "/preflight"), true);
  request.mode = network::mojom::RequestMode::kCors;
  request.method = "PUT";
  request.headers.SetHeader("X-Retire", "yes");
  auto a = Send(factory.get(), request, original);
  auto old_options = options_.WaitForRequest();
  ASSERT_EQ(net::test_server::METHOD_OPTIONS,
            old_options->http_request()->method);
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  old_options->Send(
      "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: "
      "https://alpha.test\r\nAccess-Control-Allow-Methods: "
      "PUT\r\nAccess-Control-Allow-Headers: "
      "x-retire\r\nAccess-Control-Max-Age: 600\r\nContent-Length: 0\r\n\r\n");
  old_options->Done();
  std::move(commit).Run();
  original.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, original.completion_status().error_code);
  ASSERT_TRUE(drained.Wait());
  auto b = Send(factory.get(), request, fresh);
  auto new_options = options_.WaitForRequest();
  ASSERT_EQ(net::test_server::METHOD_OPTIONS,
            new_options->http_request()->method);
  new_options->Send(
      "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: "
      "https://alpha.test\r\nAccess-Control-Allow-Methods: "
      "PUT\r\nAccess-Control-Allow-Headers: "
      "x-retire\r\nAccess-Control-Max-Age: 600\r\nContent-Length: 0\r\n\r\n");
  new_options->Done();
  auto actual = options_.WaitForRequest();
  ASSERT_EQ(net::test_server::METHOD_PUT, actual->http_request()->method);
  actual->Send(
      "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: "
      "https://alpha.test\r\nContent-Type: text/plain\r\nContent-Length: "
      "5\r\n\r\nfresh");
  actual->Done();
  fresh.RunUntilComplete();
  EXPECT_EQ(net::OK, fresh.completion_status().error_code);
  std::string body;
  EXPECT_TRUE(mojo::BlockingCopyToString(fresh.response_body_release(), &body));
  EXPECT_EQ("fresh", body);
}

TEST_F(LoaderRetirementTest, CachedPreflightGrantRetiresSelectively) {
  auto& cache = network_.context()
                    .cors_preflight_controller()
                    ->GetPreflightCacheForTesting();
  auto url = server_.GetURL("transport.test", "/cached-preflight");
  net::HttpRequestHeaders headers;
  headers.SetHeader("X-Retire", "yes");
  for (const char* owner : {"alpha.test", "beta.test"}) {
    std::optional<network::mojom::CorsError> error;
    auto result = network::cors::PreflightResult::Create(
        network::mojom::CredentialsMode::kOmit, "PUT", "x-retire", "600",
        &error);
    ASSERT_TRUE(result);
    EXPECT_FALSE(error);
    cache.AppendEntry(Origin(owner), url,
                      Isolation(owner).network_isolation_key(),
                      std::move(result));
  }
  auto allowed = [&](const char* owner) {
    return cache.CheckIfRequestCanSkipPreflight(
        Origin(owner), url, Isolation(owner).network_isolation_key(),
        network::mojom::CredentialsMode::kOmit, "PUT", headers, false,
        net::NetLogWithSource(), true, false);
  };
  ASSERT_TRUE(allowed("alpha.test"));
  ASSERT_TRUE(allowed("beta.test"));
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(allowed("alpha.test"));
  EXPECT_TRUE(allowed("beta.test"));
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
}
}  // namespace
}  // namespace alloy::site_records
