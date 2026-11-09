// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <array>
#include <atomic>
#include <memory>
#include <string>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/network_retirement_test_context.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/alloy_traffic_class.h"
#include "net/base/auth.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_change_notifier.h"
#include "net/base/schemeful_site.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/quic/web_transport_error.h"
#include "net/test/embedded_test_server/create_websocket_handler.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/embedded_test_server/websocket_echo_handler.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/native_network_retirement.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/cpp/alloy_annotations.h"
#include "services/network/public/cpp/constants.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/mojom/web_transport.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
class SocketClient final : public network::mojom::WebSocketHandshakeClient,
                           public network::mojom::WebSocketClient {
 public:
  SocketClient() : handshake_(this), client_(this) {}
  mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> CreateRemote() {
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> remote;
    handshake_.Bind(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr) override {}
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> socket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> receiver,
      network::mojom::WebSocketHandshakeResponsePtr,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override {
    socket_.Bind(std::move(socket));
    client_.Bind(std::move(receiver));
    client_.set_disconnect_handler(
        base::BindOnce(&SocketClient::Closed, base::Unretained(this)));
    readable_ = std::move(readable);
    writable_ = std::move(writable);
    opened.SetValue(true);
    socket_->StartReceiving();
  }
  void OnFailure(const std::string&, int32_t error, int32_t) override {
    EXPECT_FALSE(failed.IsReady());
    if (!failed.IsReady())
      failed.SetValue(error);
    if (!opened.IsReady())
      opened.SetValue(false);
  }
  void OnDataFrame(bool fin,
                   network::mojom::WebSocketMessageType type,
                   uint64_t length) override {
    EXPECT_TRUE(fin);
    EXPECT_EQ(network::mojom::WebSocketMessageType::TEXT, type);
    ASSERT_TRUE(frame_);
    std::move(frame_).Run(length);
  }
  void OnDropChannel(bool, uint16_t, const std::string&) override { Closed(); }
  void OnClosingHandshake() override {}
  void Closed() {
    if (!closed.IsReady())
      closed.SetValue();
  }
  void Echo(const std::string& text) {
    ASSERT_TRUE(socket_.is_bound());
    base::test::TestFuture<uint64_t> length;
    frame_ = length.GetCallback();
    socket_->SendMessage(network::mojom::WebSocketMessageType::TEXT,
                         text.size());
    size_t written = 0;
    ASSERT_EQ(MOJO_RESULT_OK,
              writable_->WriteData(base::as_byte_span(text),
                                   MOJO_WRITE_DATA_FLAG_NONE, written));
    ASSERT_EQ(text.size(), written);
    ASSERT_EQ(text.size(), length.Get());
    std::string received(text.size(), '\0');
    size_t read = 0;
    ASSERT_EQ(MOJO_RESULT_OK,
              readable_->ReadData(MOJO_READ_DATA_FLAG_NONE,
                                  base::as_writable_byte_span(received), read));
    EXPECT_EQ(text.size(), read);
    EXPECT_EQ(text, received);
  }
  void ExpectReadPipeClosed() {
    std::array<char, 8> buffer;
    size_t read = 0;
    EXPECT_EQ(MOJO_RESULT_FAILED_PRECONDITION,
              readable_->ReadData(MOJO_READ_DATA_FLAG_NONE,
                                  base::as_writable_byte_span(buffer), read));
  }
  base::test::TestFuture<bool> opened;
  base::test::TestFuture<int> failed;
  base::test::TestFuture<void> closed;

 private:
  mojo::Receiver<network::mojom::WebSocketHandshakeClient> handshake_;
  mojo::Receiver<network::mojom::WebSocketClient> client_;
  mojo::Remote<network::mojom::WebSocket> socket_;
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::ScopedDataPipeProducerHandle writable_;
  base::OnceCallback<void(uint64_t)> frame_;
};

class TransportClient final
    : public network::mojom::WebTransportHandshakeClient {
 public:
  TransportClient() : receiver_(this) {}
  mojo::PendingRemote<network::mojom::WebTransportHandshakeClient>
  CreateRemote() {
    mojo::PendingRemote<network::mojom::WebTransportHandshakeClient> remote;
    receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void OnBeforeConnect(const net::IPEndPoint&) override {}
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebTransport>,
      mojo::PendingReceiver<network::mojom::WebTransportClient>,
      const scoped_refptr<net::HttpResponseHeaders>&,
      const std::optional<std::string>&,
      network::mojom::WebTransportStatsPtr,
      std::optional<uint32_t>) override {
    ADD_FAILURE() << "Pending DNS transport unexpectedly connected";
    if (!failed.IsReady())
      failed.SetValue(net::ERR_UNEXPECTED);
  }
  void OnHandshakeFailed(
      const std::optional<net::WebTransportError>& error) override {
    EXPECT_TRUE(error);
    EXPECT_FALSE(failed.IsReady());
    if (!failed.IsReady())
      failed.SetValue(error ? error->net_error : net::ERR_FAILED);
  }
  base::test::TestFuture<int> failed;

 private:
  mojo::Receiver<network::mojom::WebTransportHandshakeClient> receiver_;
};

class HeldAuth final : public network::mojom::WebSocketAuthenticationHandler {
 public:
  HeldAuth() : receiver_(this) {}
  mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler>
  CreateRemote() {
    mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler> remote;
    receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void OnAuthRequired(const net::AuthChallengeInfo&,
                      const scoped_refptr<net::HttpResponseHeaders>&,
                      const net::IPEndPoint&,
                      OnAuthRequiredCallback callback) override {
    EXPECT_FALSE(reply);
    reply = std::move(callback);
    requested.SetValue();
  }
  base::test::TestFuture<void> requested;
  OnAuthRequiredCallback reply;

 private:
  mojo::Receiver<network::mojom::WebSocketAuthenticationHandler> receiver_;
};

class ServiceTransportRetirementTest : public testing::Test {
 public:
  ServiceTransportRetirementTest() {
    server_.RegisterRequestHandler(base::BindRepeating(
        [](std::atomic<int>* count,
           const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url != "/auth")
            return nullptr;
          ++*count;
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_code(net::HTTP_UNAUTHORIZED);
          response->AddCustomHeader("WWW-Authenticate",
                                    "Basic realm=\"retirement\"");
          return response;
        },
        base::Unretained(&auth_requests_)));
    net::test_server::RegisterWebSocketHandler<
        net::test_server::WebSocketEchoHandler>(&server_, "/echo");
    CHECK(server_.Start());
    network_ = std::make_unique<NetworkRetirementTestContext>();
    context_ = &network_->context();
    resolver_ = &network_->resolver();
  }
  static url::Origin Origin(const char* owner) {
    return url::Origin::Create(GURL(std::string("https://") + owner));
  }
  static net::IsolationInfo Isolation(const char* owner) {
    return net::IsolationInfo::CreateForInternalRequest(Origin(owner));
  }
  base::OnceClosure Prepare(const char* owner, base::OnceClosure done) {
    return context_->PrepareTransportServiceRetirement(
        base::BindRepeating(
            [](net::NetworkAnonymizationKey wanted,
               const network::NativeNetworkOwner& actual) {
              return wanted == actual.network_key;
            },
            Isolation(owner).network_anonymization_key()),
        std::move(done));
  }
  void Socket(
      const char* owner,
      SocketClient* client,
      const char* path = "/echo",
      mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler> auth =
          {}) {
    auto url = server_.GetURL("transport.test", path);
    GURL::Replacements changes;
    changes.SetSchemeStr("ws");
    url = url.ReplaceComponents(changes);
    net::MutableNetworkTrafficAnnotationTag annotation;
    annotation.unique_id_hash_code =
        net::AlloyAnnotationHash("websocket_stream");
    context_->CreateWebSocket(
        url, {}, net::StorageAccessApiStatus::kNone, Isolation(owner), {},
        network::OriginatingProcessId::browser(), Origin(owner), nullptr, 0,
        annotation, client->CreateRemote(), mojo::NullRemote(), std::move(auth),
        mojo::NullRemote(), std::nullopt,
        network::GetTestNetworkRestrictionsId(),
        network::mojom::IPAddressSpace::kUnknown,
        net::handles::kInvalidNetworkHandle);
  }
  void Transport(const char* owner, TransportClient* client) {
    context_->CreateWebTransport(
        net::MutableNetworkTrafficAnnotationTag(network::alloy::kWebTransport),
        GURL("https://pending-transport.test/"), Origin(owner),
        Isolation(owner).network_anonymization_key(), {}, {},
        network::mojom::WebTransportCongestionControl::kDefault, std::nullopt,
        std::nullopt, {}, client->CreateRemote(), mojo::NullRemote(), nullptr,
        network::GetTestNetworkRestrictionsId());
  }

 protected:
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
  std::unique_ptr<net::NetworkChangeNotifier> notifier_ =
      net::NetworkChangeNotifier::CreateMockIfNeeded();
  std::atomic<int> auth_requests_{0};
  net::EmbeddedTestServer server_;
  std::unique_ptr<NetworkRetirementTestContext> network_;
  raw_ptr<network::NetworkContext> context_ = nullptr;
  raw_ptr<net::MockHostResolverBase> resolver_ = nullptr;
};

TEST_F(ServiceTransportRetirementTest,
       ActiveSocketRetiresAndBetaEchoStillFlows) {
  SocketClient alpha, beta;
  Socket("alpha.test", &alpha);
  Socket("beta.test", &beta);
  ASSERT_TRUE(alpha.opened.Get());
  ASSERT_TRUE(beta.opened.Get());
  alpha.Echo("alpha-before");
  beta.Echo("beta-before");
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  ASSERT_TRUE(alpha.closed.Wait());
  ASSERT_TRUE(drained.Wait());
  alpha.ExpectReadPipeClosed();
  EXPECT_FALSE(beta.closed.IsReady());
  beta.Echo("beta-after-alpha-retirement");
}

TEST_F(ServiceTransportRetirementTest,
       PendingSocketAndTransportRetireSelectively) {
  resolver_->set_ondemand_mode(true);
  SocketClient alpha, beta;
  TransportClient alpha_transport, beta_transport;
  Socket("alpha.test", &alpha);
  Socket("beta.test", &beta);
  Transport("alpha.test", &alpha_transport);
  Transport("beta.test", &beta_transport);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(alpha.opened.IsReady());
  ASSERT_FALSE(beta.opened.IsReady());
  ASSERT_FALSE(alpha_transport.failed.IsReady());
  ASSERT_FALSE(beta_transport.failed.IsReady());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED, alpha.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, alpha_transport.failed.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.failed.IsReady());
  EXPECT_FALSE(beta_transport.failed.IsReady());
  base::test::TestFuture<void> cleanup;
  auto beta_commit = Prepare("beta.test", cleanup.GetCallback());
  std::move(beta_commit).Run();
  EXPECT_EQ(net::ERR_ABORTED, beta.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, beta_transport.failed.Get());
  ASSERT_TRUE(cleanup.Wait());
}

TEST_F(ServiceTransportRetirementTest,
       HeldAuthReplyCannotResumeOriginalSocket) {
  SocketClient alpha, beta;
  HeldAuth auth;
  Socket("beta.test", &beta);
  ASSERT_TRUE(beta.opened.Get());
  beta.Echo("beta-positive-control");
  Socket("alpha.test", &alpha, "/auth", auth.CreateRemote());
  ASSERT_TRUE(auth.requested.Wait());
  EXPECT_EQ(1, auth_requests_.load());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED, alpha.failed.Get());
  ASSERT_TRUE(drained.Wait());
  ASSERT_TRUE(auth.reply);
  std::move(auth.reply).Run(net::AuthCredentials(u"user", u"password"));
  network_->remote().FlushForTesting();
  tasks_.RunUntilIdle();
  beta.Echo("beta-after-late-auth-reply");
  EXPECT_EQ(1, auth_requests_.load());
  EXPECT_FALSE(beta.closed.IsReady());
}

TEST_F(ServiceTransportRetirementTest,
       OverlapIncludesFreshWorkOnlyInLaterSnapshot) {
  resolver_->set_ondemand_mode(true);
  SocketClient original, fresh;
  TransportClient original_transport, fresh_transport;
  Socket("alpha.test", &original);
  Transport("alpha.test", &original_transport);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(original.opened.IsReady());
  ASSERT_FALSE(original_transport.failed.IsReady());
  base::test::TestFuture<void> first, second;
  auto first_commit = Prepare("alpha.test", first.GetCallback());
  Socket("alpha.test", &fresh);
  Transport("alpha.test", &fresh_transport);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(fresh.opened.IsReady());
  ASSERT_FALSE(fresh_transport.failed.IsReady());
  auto second_commit = Prepare("alpha.test", second.GetCallback());
  std::move(first_commit).Run();
  EXPECT_EQ(net::ERR_ABORTED, original.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, original_transport.failed.Get());
  ASSERT_TRUE(first.Wait());
  EXPECT_FALSE(second.IsReady());
  EXPECT_FALSE(fresh.failed.IsReady());
  EXPECT_FALSE(fresh_transport.failed.IsReady());
  std::move(second_commit).Run();
  EXPECT_EQ(net::ERR_ABORTED, fresh.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, fresh_transport.failed.Get());
  ASSERT_TRUE(second.Wait());
}

TEST_F(ServiceTransportRetirementTest, EmptySelectionPreservesPendingBeta) {
  resolver_->set_ondemand_mode(true);
  SocketClient beta;
  TransportClient transport;
  Socket("beta.test", &beta);
  Transport("beta.test", &transport);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(beta.opened.IsReady());
  ASSERT_FALSE(transport.failed.IsReady());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.failed.IsReady());
  EXPECT_FALSE(transport.failed.IsReady());
  base::test::TestFuture<void> cleanup;
  auto close = Prepare("beta.test", cleanup.GetCallback());
  std::move(close).Run();
  EXPECT_EQ(net::ERR_ABORTED, beta.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, transport.failed.Get());
  ASSERT_TRUE(cleanup.Wait());
}
TEST_F(ServiceTransportRetirementTest,
       AbandonedPreparationRevokesOriginalEndpoints) {
  resolver_->set_ondemand_mode(true);
  SocketClient alpha;
  TransportClient transport;
  Socket("alpha.test", &alpha);
  Transport("alpha.test", &transport);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(alpha.opened.IsReady());
  ASSERT_FALSE(transport.failed.IsReady());
  base::test::TestFuture<void> drained;
  auto abandoned = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  abandoned.Reset();
  EXPECT_EQ(net::ERR_ABORTED, alpha.failed.Get());
  EXPECT_EQ(net::ERR_ABORTED, transport.failed.Get());
  ASSERT_TRUE(drained.Wait());
}
}  // namespace
}  // namespace alloy::site_records
