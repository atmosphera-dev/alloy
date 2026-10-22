// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "services/network/alloy_capability.h"

#include "base/alloy/buildflags.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/bind.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/base/network_delegate_impl.h"
#include "net/base/isolation_info.h"
#include "net/dns/mdns_client.h"
#include "net/socket/datagram_server_socket.h"
#include "net/http/http_status_code.h"
#include "net/http/http_request_info.h"
#include "net/http/http_transaction.h"
#include "net/http/http_transaction_factory.h"
#include "net/reporting/reporting_uploader.h"
#include "net/reporting/reporting_cache_observer.h"
#include "net/reporting/reporting_policy.h"
#include "net/reporting/reporting_service.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "services/network/p2p/socket_manager.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/mojom/host_resolver.mojom.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/mdns_responder.mojom.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/trust_tokens.mojom.h"
#include "services/network/sct_auditing/sct_auditing_handler.h"
#include "services/network/socket_factory.h"
#include "services/network/test/test_utils.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

#include <atomic>

namespace network {
namespace {
net::MutableNetworkTrafficAnnotationTag Tag(std::string_view id) {
  net::MutableNetworkTrafficAnnotationTag tag;
  tag.unique_id_hash_code = AlloyGuard::Hash(id);
  return tag;
}

class AlloyGuardNetworkTest : public testing::Test {
 protected:
  base::test::TaskEnvironment tasks_{base::test::TaskEnvironment::MainThreadType::IO};
};

class TransientReportingObserver : public net::ReportingCacheObserver {
 public:
  explicit TransientReportingObserver(net::ReportingService* service)
      : service_(service) {
    service_->AddReportingCacheObserver(this);
  }
  ~TransientReportingObserver() {
    service_->RemoveReportingCacheObserver(this);
  }
  void OnReportAdded(const net::ReportingReport* report) override {
    ++added;
    EXPECT_EQ(report->url, GURL("https://alloy.invalid/report?keep=1"));
    const auto* value = report->body.FindString("sentinel");
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, "transient");
  }
  void OnReportUpdated(const net::ReportingReport* report) override {
    ++updated;
    last_status = report->status;
  }
  int added = 0;
  int updated = 0;
  net::ReportingReport::Status last_status = net::ReportingReport::Status::QUEUED;

 private:
  net::ReportingService* service_;
};

TEST_F(AlloyGuardNetworkTest, ReportingNotifiesWithoutKeepingProductReports) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto builder = net::CreateTestURLRequestContextBuilder();
  builder->set_network_delegate(std::make_unique<net::NetworkDelegateImpl>());
  builder->set_reporting_policy(std::make_unique<net::ReportingPolicy>());
  auto context = builder->Build();
  auto* service = context->reporting_service();
  ASSERT_TRUE(service);
  TransientReportingObserver observer(service);
  base::DictValue body;
  body.Set("sentinel", "transient");
  service->QueueReport(GURL("https://alloy.invalid/report?keep=1#strip"),
      std::nullopt, net::NetworkAnonymizationKey(), "Alloy", "default",
      "network-error", std::move(body), 0, net::ReportingTargetType::kDeveloper);
  tasks_.RunUntilIdle();
  EXPECT_EQ(observer.added, 1);
  auto status = service->StatusAsValue();
  const auto* rows = status.GetDict().FindList("reports");
  ASSERT_TRUE(rows);
#if BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  EXPECT_EQ(service->GetReports().size(), 1u);
  EXPECT_EQ(rows->size(), 1u);
#else
  EXPECT_TRUE(service->GetReports().empty());
  EXPECT_TRUE(rows->empty());
  EXPECT_EQ(observer.updated, 1);
  EXPECT_EQ(observer.last_status, net::ReportingReport::Status::DOOMED);
#endif
}

TEST_F(AlloyGuardNetworkTest, SctDiagnosticStorageCannotBeReenabledInProduct) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto path = dir.GetPath().AppendASCII("pending-reports.json");
  SCTAuditingHandler handler(nullptr, path);
  handler.SetMode(mojom::SCTAuditingMode::kEnhancedSafeBrowsingReporting);
  base::test::TestFuture<void> cleared;
  handler.ClearPendingReports(cleared.GetCallback());
  ASSERT_TRUE(cleared.Wait());
  tasks_.RunUntilIdle();
#if BUILDFLAG(ALLOY_ENABLE_DIAGNOSTIC_LOGGING)
  EXPECT_TRUE(handler.is_enabled());
  ASSERT_TRUE(handler.GetFileWriterForTesting());
  std::string serialized;
  ASSERT_TRUE(base::ReadFileToString(path, &serialized));
  EXPECT_EQ(serialized, "[]");
#else
  EXPECT_FALSE(handler.is_enabled());
  EXPECT_FALSE(handler.GetFileWriterForTesting());
  EXPECT_FALSE(handler.SerializeData());
  EXPECT_FALSE(base::PathExists(path));
#endif
  EXPECT_TRUE(handler.GetPendingReportersForTesting()->empty());
}

TEST_F(AlloyGuardNetworkTest, RemovedTrustTokenStoreCompletesConsumersWithoutDiskState) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto token_path = dir.GetPath().AppendASCII("Trust Tokens");
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
  params->file_paths = mojom::NetworkContextFilePaths::New();
  params->file_paths->data_directory = dir.GetPath();
  params->file_paths->trust_token_database_name = token_path.BaseName();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  base::ScopedTempDir log_dir;
  ASSERT_TRUE(log_dir.CreateUniqueTempDir());
  params->alloy_guard_log = base::File(log_dir.GetPath().AppendASCII("alloy-guard.log"),
      base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(service.get(),
      remote.BindNewPipeAndPassReceiver(), std::move(params), {});
  ASSERT_FALSE(context->trust_token_store());
  context->SetBlockTrustTokens(false);
  EXPECT_TRUE(context->are_trust_tokens_blocked());

  base::test::TestFuture<std::vector<mojom::StoredTrustTokensForIssuerPtr>> counts;
  remote->GetStoredTrustTokenCounts(counts.GetCallback());
  EXPECT_TRUE(counts.Get().empty());
  base::test::TestFuture<bool> session_cleared;
  remote->ClearTrustTokenSessionOnlyData(session_cleared.GetCallback());
  EXPECT_FALSE(session_cleared.Get());
  base::test::TestFuture<void> cleared;
  remote->ClearTrustTokenData(nullptr, cleared.GetCallback());
  ASSERT_TRUE(cleared.Wait());
  mojo::Remote<mojom::TrustTokenQueryAnswerer> query;
  auto query_receiver = query.BindNewPipeAndPassReceiver();
  base::test::TestFuture<void> query_closed;
  query.set_disconnect_handler(query_closed.GetCallback());
  remote->GetTrustTokenQueryAnswerer(std::move(query_receiver),
      url::Origin::Create(GURL("https://alloy.invalid")));
  ASSERT_TRUE(query_closed.Wait());

  std::atomic<int> received = 0;
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* received, const net::test_server::HttpRequest&)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*received;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content("ordinary-page-consumer");
        response->set_content_type("text/plain");
        return response;
      }, &received));
  ASSERT_TRUE(server.Start());
  auto factory_params = mojom::URLLoaderFactoryParams::New();
  factory_params->process_id = OriginatingProcessId::browser();
  factory_params->is_orb_enabled = false;
  factory_params->isolation_info = net::IsolationInfo::CreateForInternalRequest(
      url::Origin::Create(server.base_url()));
  mojo::Remote<mojom::URLLoaderFactory> factory;
  remote->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
      std::move(factory_params));
  ResourceRequest request;
  request.method = "GET";
  request.url = server.GetURL("/page");
  request.request_initiator = url::Origin::Create(server.base_url());
  request.trust_token_params = OptionalTrustTokenParams(mojom::TrustTokenParams::New());
  mojo::Remote<mojom::URLLoader> token_loader;
  TestURLLoaderClient token_client;
  factory->CreateLoaderAndStart(token_loader.BindNewPipeAndPassReceiver(), 1, 0,
      request, token_client.CreateRemote(), Tag("blink_resource_loader"));
  token_client.RunUntilComplete();
  // The native CORS factory rejects token parameters when no store exists,
  // before forwarding to the inner loader's operation-failure path.
  EXPECT_EQ(token_client.completion_status().error_code, net::ERR_INVALID_ARGUMENT);
  EXPECT_EQ(received.load(), 0);

  request.trust_token_params = std::nullopt;
  mojo::Remote<mojom::URLLoader> page_loader;
  TestURLLoaderClient page_client;
  factory->CreateLoaderAndStart(page_loader.BindNewPipeAndPassReceiver(), 2, 0,
      request, page_client.CreateRemote(), Tag("blink_resource_loader"));
  page_client.RunUntilComplete();
  EXPECT_EQ(page_client.completion_status().error_code, net::OK);
  std::string body;
  ASSERT_TRUE(mojo::BlockingCopyToString(page_client.response_body_release(), &body));
  EXPECT_EQ(body, "ordinary-page-consumer");
  EXPECT_EQ(received.load(), 1);
  page_loader.reset();
  token_loader.reset();
  factory.reset();
  query.reset();
  remote.reset();
  context.reset();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(token_path));
  EXPECT_TRUE(base::IsDirectoryEmpty(dir.GetPath()));
}

TEST_F(AlloyGuardNetworkTest, SocketCreationDeniesBeforeBinding) {
  auto context = net::CreateTestURLRequestContextBuilder()->Build();
  auto guard = base::MakeRefCounted<AlloyGuard>();
  SocketFactory factory(nullptr, context.get(), guard);
  mojo::Remote<mojom::UDPSocket> socket;
  auto receiver = socket.BindNewPipeAndPassReceiver();
  base::test::TestFuture<uint32_t, const std::string&> closed;
  socket.set_disconnect_with_reason_handler(closed.GetCallback());
  factory.CreateUDPSocket(Tag("alloy_browser_socket"), std::move(receiver), {});
  ASSERT_TRUE(closed.Wait());
  EXPECT_EQ(closed.Get<0>(), static_cast<uint32_t>(net::ERR_NETWORK_ACCESS_DENIED));
  EXPECT_EQ(closed.Get<1>(), "net::ERR_NETWORK_ACCESS_DENIED");
}

TEST_F(AlloyGuardNetworkTest, ExistingUdpCapabilityRechecksPolicy) {
  auto context = net::CreateTestURLRequestContextBuilder()->Build();
  auto guard = base::MakeRefCounted<AlloyGuard>();
  AlloyGuard::Options on;
  on.open_search = true;
  guard->SetOptions(on);
  SocketFactory factory(nullptr, context.get(), guard);
  mojo::Remote<mojom::UDPSocket> socket;
  factory.CreateUDPSocket(Tag("open_search"), socket.BindNewPipeAndPassReceiver(), {});
  base::test::TestFuture<int32_t, const std::optional<net::IPEndPoint>&> bound;
  socket->Bind(net::IPEndPoint(net::IPAddress::IPv4Localhost(), 0), nullptr, bound.GetCallback());
  ASSERT_EQ(bound.Get<0>(), net::OK);
  ASSERT_TRUE(bound.Get<1>());
  EXPECT_GT(bound.Get<1>()->port(), 0);

  // The capability and socket still exist. Turning the preference off must
  // deny at the child operation, before SendTo reaches the kernel.
  guard->SetOptions({});
  const uint8_t bytes[] = {1, 2, 3};
  base::test::TestFuture<int32_t> sent;
  socket->SendTo(net::IPEndPoint(net::IPAddress::IPv4Localhost(), 9), bytes,
                 Tag("open_search"), sent.GetCallback());
  EXPECT_EQ(sent.Get(), net::ERR_NETWORK_ACCESS_DENIED);
}

TEST_F(AlloyGuardNetworkTest, P2pDnsDenialPrecedesResolverAccess) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  mojo::PendingRemote<mojom::P2PTrustedSocketManagerClient> client;
  auto client_receiver = client.InitWithNewPipeAndPassReceiver();
  mojo::Remote<mojom::P2PTrustedSocketManager> trusted;
  mojo::Remote<mojom::P2PSocketManager> remote;
  // No URLRequestContext exists: calling its resolver would fail this test.
  P2PSocketManager manager({}, std::move(client),
      trusted.BindNewPipeAndPassReceiver(), remote.BindNewPipeAndPassReceiver(),
      base::DoNothing(), nullptr, guard, AlloyGuard::Hash("alloy_browser_socket"));
  base::test::TestFuture<const net::IPAddressList&> addresses;
  base::test::TestFuture<uint32_t, const std::string&> closed;
  remote.set_disconnect_with_reason_handler(closed.GetCallback());
  remote->GetHostAddress("must-not-resolve.invalid", std::nullopt, false,
                         addresses.GetCallback());
  EXPECT_TRUE(addresses.Get().empty());
  ASSERT_TRUE(closed.Wait());
  EXPECT_EQ(closed.Get<0>(), static_cast<uint32_t>(net::ERR_NETWORK_ACCESS_DENIED));
}

TEST_F(AlloyGuardNetworkTest, LocalDiscoveryDeniedEvenWhenStandaloneDnsIsEnabled) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto log_path = dir.GetPath().AppendASCII("alloy-guard.log");
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log = base::File(log_path, base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  params->alloy_dns_prefetch_enabled = true;
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params), {});
  mojo::Remote<mojom::MdnsResponder> responder;
  auto responder_receiver = responder.BindNewPipeAndPassReceiver();
  base::test::TestFuture<uint32_t, const std::string&> closed;
  responder.set_disconnect_with_reason_handler(closed.GetCallback());
  remote->CreateMdnsResponder(std::move(responder_receiver));
  EXPECT_EQ(closed.Get<0>(), static_cast<uint32_t>(net::ERR_NETWORK_ACCESS_DENIED));
  mojo::Remote<mojom::HostResolver> resolver;
  remote->CreateHostResolver(std::nullopt, resolver.BindNewPipeAndPassReceiver());
  remote.FlushForTesting();
  mojo::PendingRemote<mojom::MdnsListenClient> listener;
  auto listener_receiver = listener.InitWithNewPipeAndPassReceiver();
  base::test::TestFuture<int32_t> result;
  resolver->MdnsListen(net::HostPortPair("discovery.local", 5353), net::DnsQueryType::A,
                       std::move(listener), result.GetCallback());
  EXPECT_EQ(result.Get(), net::ERR_NETWORK_ACCESS_DENIED);
  tasks_.RunUntilIdle();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  std::string log;
  ASSERT_TRUE(base::ReadFileToString(log_path, &log));
  auto lines = base::SplitString(log, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(lines.size(), 2u) << log;
  EXPECT_NE(log.find("NetworkContext.CreateMdnsResponder"), std::string::npos);
  EXPECT_NE(log.find("HostResolver.MdnsListen"), std::string::npos);
#else
  EXPECT_FALSE(base::PathExists(log_path));
#endif
}

TEST_F(AlloyGuardNetworkTest, NativeMdnsCannotAllocateOrJoinMulticastSockets) {
  class CountingFactory : public net::MDnsSocketFactory {
   public:
    void CreateSockets(std::vector<std::unique_ptr<net::DatagramServerSocket>>*) override { ++calls; }
    int calls = 0;
  } factory;
  auto client = net::MDnsClient::CreateDefault();
  EXPECT_EQ(client->StartListening(&factory), net::ERR_NETWORK_ACCESS_DENIED);
  EXPECT_EQ(factory.calls, 0);
  EXPECT_FALSE(client->IsListening());
  EXPECT_FALSE(net::CreateAndBindMDnsSocket(net::ADDRESS_FAMILY_IPV4, 0, nullptr));
  EXPECT_FALSE(net::CreateAndBindMDnsSocket(net::ADDRESS_FAMILY_IPV6, 0, nullptr));
}

TEST_F(AlloyGuardNetworkTest, DownloadContinuationsReachOnlyTheirPageOwnedConsumer) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto log_path = dir.GetPath().AppendASCII("alloy-guard.log");
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log = base::File(log_path,
      base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(service.get(),
      remote.BindNewPipeAndPassReceiver(), std::move(params), {});
  std::atomic<int> received = 0;
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* received, const net::test_server::HttpRequest& request)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*received;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content("download-continuation-consumer");
        response->set_content_type("application/octet-stream");
        return response;
      }, &received));
  ASSERT_TRUE(server.Start());
  int allowed_count = 0;
  int denied_count = 0;
  for (auto continuation : {"download_manager_resume", "parallel_download_job"}) {
    for (auto owner : {"renderer_initiated_download", "navigation_url_loader",
                       "missing", "reporting", "blink_resource_loader",
                       "download_manager_resume", "unknown_download_owner"}) {
      net::TestDelegate consumer;
      auto request = context->url_request_context()->CreateRequest(
          server.GetURL("/file"), net::DEFAULT_PRIORITY, &consumer,
          static_cast<net::NetworkTrafficAnnotationTag>(Tag(continuation)),
          net::handles::kInvalidNetworkHandle, false, std::nullopt, Tag(owner));
      request->Start();
      consumer.RunUntilComplete();
      if (net::IsAlloyPageDownloadOwner(AlloyGuard::Hash(owner))) {
        EXPECT_EQ(consumer.request_status(), net::OK);
        EXPECT_EQ(consumer.data_received(), "download-continuation-consumer");
        ++allowed_count;
      } else {
        EXPECT_EQ(consumer.request_status(), net::ERR_NETWORK_ACCESS_DENIED);
        EXPECT_TRUE(consumer.data_received().empty());
        ++denied_count;
      }
    }
  }
  EXPECT_EQ(allowed_count, 4);
  EXPECT_EQ(denied_count, 10);
  EXPECT_EQ(received.load(), allowed_count);
  tasks_.RunUntilIdle();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  std::string log;
  ASSERT_TRUE(base::ReadFileToString(log_path, &log));
  const auto lines = base::SplitString(log, "\n", base::KEEP_WHITESPACE,
                                       base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(lines.size(), static_cast<size_t>(denied_count));
  for (const auto& line : lines) {
    auto row = base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
    ASSERT_TRUE(row);
    ASSERT_TRUE(row->FindString("originating_annotation_id"));
    EXPECT_EQ(*row->FindString("entry_point"),
              "NetworkServiceNetworkDelegate.OnBeforeURLRequest");
  }
#else
  EXPECT_FALSE(base::PathExists(log_path));
#endif
  context.reset();
  tasks_.RunUntilIdle();
}

TEST_F(AlloyGuardNetworkTest, NativeRequestsAndReportingCannotBypassFactories) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto log_path = dir.GetPath().AppendASCII("alloy-guard.log");
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log = base::File(log_path, base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(service.get(),
      remote.BindNewPipeAndPassReceiver(), std::move(params), {});

  std::atomic<int> pages = 0;
  std::atomic<int> collectors = 0;
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* pages, std::atomic<int>* collectors,
         const net::test_server::HttpRequest& request)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        if (request.relative_url == "/page") ++*pages;
        else ++*collectors;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content("page-consumer-ok");
        response->set_content_type("text/plain");
        return response;
      }, &pages, &collectors));
  ASSERT_TRUE(server.Start());

  net::TestDelegate page_consumer;
  auto page = context->url_request_context()->CreateRequest(server.GetURL("/page"),
      net::DEFAULT_PRIORITY, &page_consumer,
      static_cast<net::NetworkTrafficAnnotationTag>(Tag("blink_resource_loader")),
      net::handles::kInvalidNetworkHandle);
  page->Start();
  page_consumer.RunUntilComplete();
  EXPECT_EQ(page_consumer.request_status(), net::OK);
  EXPECT_EQ(page_consumer.data_received(), "page-consumer-ok");
  EXPECT_EQ(pages.load(), 1);
  page.reset();

  net::TestDelegate blocked_consumer;
  auto blocked = context->url_request_context()->CreateRequest(server.GetURL("/denied-direct"),
      net::DEFAULT_PRIORITY, &blocked_consumer,
      static_cast<net::NetworkTrafficAnnotationTag>(Tag("reporting")),
      net::handles::kInvalidNetworkHandle);
  blocked->Start();
  blocked_consumer.RunUntilComplete();
  EXPECT_EQ(blocked_consumer.request_status(), net::ERR_NETWORK_ACCESS_DENIED);
  blocked.reset();

  auto uploader = net::ReportingUploader::Create(context->url_request_context());
  base::test::TestFuture<net::ReportingUploader::Outcome> upload_consumer;
  auto upload_url = server.GetURL("/denied-upload");
  uploader->StartUpload(url::Origin::Create(upload_url), upload_url,
      net::IsolationInfo::CreateTransient(/*nonce=*/std::nullopt), "{}", 0,
      false, upload_consumer.GetCallback());
  EXPECT_EQ(upload_consumer.Get(), net::ReportingUploader::Outcome::FAILURE);
  EXPECT_EQ(uploader->GetPendingUploadCountForTesting(), 0);
  tasks_.RunUntilIdle();
  EXPECT_EQ(collectors.load(), 0);

#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  std::string log;
  ASSERT_TRUE(base::ReadFileToString(log_path, &log));
  const auto lines = base::SplitString(log, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(lines.size(), 2u) << log;
  for (size_t i = 0; i < lines.size(); ++i) {
    auto row = base::JSONReader::ReadDict(lines[i], base::JSON_PARSE_RFC);
    ASSERT_TRUE(row);
    EXPECT_EQ(*row->FindString("annotation_id"), "reporting");
    EXPECT_EQ(*row->FindString("class"), "browser-service");
    EXPECT_EQ(*row->FindString("entry_point"), "NetworkServiceNetworkDelegate.OnBeforeURLRequest");
    EXPECT_EQ(*row->FindString("endpoint"), server.GetURL(i == 0 ? "/denied-direct" : "/denied-upload").spec());
  }
#else
  EXPECT_FALSE(base::PathExists(log_path));
#endif
  uploader.reset();
  context.reset();
  tasks_.RunUntilIdle();
}

TEST_F(AlloyGuardNetworkTest, NativeHttpTransactionCannotBypassRequestFactories) {
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto path = dir.GetPath().AppendASCII("alloy-guard.log");
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params = FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log = base::File(path, base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params), {});
  std::atomic<int> received = 0;
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* received, const net::test_server::HttpRequest&) -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*received;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_code(net::HTTP_OK);
        response->set_content("warm-page");
        return response;
      }, &received));
  ASSERT_TRUE(server.Start());
  net::TestDelegate page;
  auto request = context->url_request_context()->CreateRequest(server.GetURL("/warm"), net::DEFAULT_PRIORITY,
      &page, static_cast<net::NetworkTrafficAnnotationTag>(Tag("blink_resource_loader")), net::handles::kInvalidNetworkHandle);
  request->Start();
  page.RunUntilComplete();
  ASSERT_EQ(page.request_status(), net::OK);
  EXPECT_EQ(received.load(), 1);
  request.reset();
  net::HttpRequestInfo info;
  info.url = server.GetURL("/direct-native");
  info.method = "GET";
  info.traffic_annotation = Tag("reporting");
  auto transaction = context->url_request_context()->http_transaction_factory()->CreateTransaction(net::DEFAULT_PRIORITY);
  base::test::TestFuture<int> done;
  int result = transaction->Start(&info, done.GetCallback(), {});
  if (result == net::ERR_IO_PENDING) result = done.Get();
  EXPECT_EQ(result, net::ERR_NETWORK_ACCESS_DENIED);
  tasks_.RunUntilIdle();
  EXPECT_EQ(received.load(), 1);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  std::string log;
  ASSERT_TRUE(base::ReadFileToString(path, &log));
  auto lines = base::SplitString(log, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(lines.size(), 1u) << log;
  auto denial = base::JSONReader::ReadDict(lines[0], base::JSON_PARSE_RFC);
  ASSERT_TRUE(denial);
  EXPECT_EQ(*denial->FindString("annotation_id"), "reporting");
  EXPECT_EQ(*denial->FindString("entry_point"), "HttpTransaction.Start");
#else
  EXPECT_FALSE(base::PathExists(path));
#endif
}
}  // namespace
}  // namespace network
