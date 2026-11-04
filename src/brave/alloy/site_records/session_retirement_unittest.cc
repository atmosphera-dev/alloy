// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "net/base/features.h"
#include "net/http/http_network_session.h"
#include "net/http/http_stream_pool.h"
#include "net/http/http_stream_pool_group.h"
#include "net/http/http_stream_pool_handle.h"
#include "net/http/http_stream_pool_request_info.h"
#include "net/http/http_stream_pool_test_util.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/spdy/spdy_stream.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/test/test_with_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class NetworkRetirementModernTest : public net::WithTaskEnvironment,
                                    public testing::Test {
 protected:
  NetworkRetirementModernTest() {
    features_.InitAndEnableFeature(net::features::kHappyEyeballsV3);
    dependencies_.alternate_host_resolver =
        std::make_unique<net::FakeServiceEndpointResolver>();
    session_ = net::SpdySessionDependencies::SpdyCreateSession(&dependencies_);
  }
  net::HttpStreamPool& pool() { return *session_->http_stream_pool(); }
  static net::HttpStreamKey Key(const char* url) {
    return net::StreamKeyBuilder(url).Build();
  }
  std::unique_ptr<net::HttpStreamPoolHandle> Handle(const char* url) {
    auto socket = std::make_unique<net::FakeStreamSocket>();
    socket->set_is_connected(true);
    return pool().GetOrCreateGroupForTesting(Key(url)).CreateHandle(
        std::move(socket), net::StreamSocketHandle::SocketReuseType::kUnused,
        net::LoadTimingInfo::ConnectTiming());
  }
  void Retire(const char* host, base::OnceClosure done) {
    pool().RetireMatchingStreams(base::BindRepeating(
        [](std::string host, const net::HttpStreamKey& key) {
          return key.destination().host() == host;
        }, std::string(host)), std::move(done));
  }
  net::HttpStreamPoolRequestInfo Info(const char* url) {
    return net::HttpStreamPoolRequestInfo(url::SchemeHostPort(GURL(url)),
        net::PRIVACY_MODE_DISABLED, net::SocketTag(), net::NetworkAnonymizationKey(),
        net::SecureDnsPolicy::kAllow, false, net::AlternativeServiceInfo(),
        net::AdvertisedAltSvcState::kUnknown, net::HttpStreamPool::kAllProtocols,
        0, net::ProxyInfo::Direct(), net::handles::kInvalidNetworkHandle,
        net::NetLogWithSource());
  }
  base::test::ScopedFeatureList features_;
  net::SpdySessionDependencies dependencies_;
  std::unique_ptr<net::HttpNetworkSession> session_;
};

TEST_F(NetworkRetirementModernTest, OriginalHandleBlocksButFreshHandleDoesNot) {
  auto alpha = Handle("http://alpha.test");
  auto alpha_idle = Handle("http://alpha.test");
  auto beta = Handle("http://beta.test");
  auto beta_idle = Handle("http://beta.test");
  auto* original_beta_idle = beta_idle->socket();
  alpha_idle.reset();
  beta_idle.reset();
  base::test::TestFuture<void> drained;

  Retire("alpha.test", drained.GetCallback());

  EXPECT_FALSE(alpha->socket()->IsConnected());
  EXPECT_TRUE(beta->socket()->IsConnected());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());
  auto fresh = Handle("http://alpha.test");
  auto& beta_group = pool().GetOrCreateGroupForTesting(Key("http://beta.test"));
  auto reused = beta_group.GetIdleStreamSocket();
  EXPECT_EQ(original_beta_idle, reused.get());
  alpha.reset();
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(fresh->socket()->IsConnected());
  EXPECT_TRUE(beta->socket()->IsConnected());
}

TEST_F(NetworkRetirementModernTest, OverlappingOriginalHandoutsDrainSeparately) {
  auto original = Handle("http://alpha.test");
  auto beta = Handle("http://beta.test");
  base::test::TestFuture<void> first, second;
  Retire("alpha.test", first.GetCallback());
  auto fresh = Handle("http://alpha.test");
  Retire("alpha.test", second.GetCallback());
  original.reset();
  ASSERT_TRUE(first.Wait());
  EXPECT_FALSE(second.IsReady());
  fresh.reset();
  ASSERT_TRUE(second.Wait());
  EXPECT_TRUE(beta->socket()->IsConnected());
}

TEST_F(NetworkRetirementModernTest, EmptySelectionPreservesUnrelatedHandle) {
  auto beta = Handle("http://beta.test");
  base::test::TestFuture<void> drained;
  Retire("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(beta->socket()->IsConnected());
}

TEST_F(NetworkRetirementModernTest, NativePreconnectCallbackAndDnsRetireSelectively) {
  auto* resolver = static_cast<net::FakeServiceEndpointResolver*>(
      dependencies_.alternate_host_resolver.get());
  auto alpha_dns = resolver->AddFakeRequest();
  auto beta_dns = resolver->AddFakeRequest();
  base::test::TestFuture<int> alpha, beta;
  base::test::TestFuture<void> drained, beta_drained;
  ASSERT_EQ(net::ERR_IO_PENDING, pool().Preconnect(Info("http://alpha.test"), 1,
      base::BindOnce([](base::test::TestFuture<int>* result,
                       base::test::TestFuture<void>* drain, int status) {
        EXPECT_FALSE(drain->IsReady());
        result->SetValue(status);
      }, &alpha, &drained)));
  ASSERT_EQ(net::ERR_IO_PENDING, pool().Preconnect(Info("http://beta.test"), 1,
      beta.GetCallback()));
  ASSERT_TRUE(alpha_dns && beta_dns);

  Retire("alpha.test", drained.GetCallback());

  EXPECT_EQ(net::ERR_ABORTED, alpha.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha_dns);
  EXPECT_TRUE(beta_dns);
  EXPECT_FALSE(beta.IsReady());
  Retire("beta.test", beta_drained.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, beta.Get());
  ASSERT_TRUE(beta_drained.Wait());
}

TEST_F(NetworkRetirementModernTest, PreparedPreconnectKeepsFreshSameKeyDns) {
  auto* resolver=static_cast<net::FakeServiceEndpointResolver*>(
      dependencies_.alternate_host_resolver.get());
  auto alpha_dns=resolver->AddFakeRequest(),beta_dns=resolver->AddFakeRequest();
  base::test::TestFuture<int> alpha,beta,fresh;
  ASSERT_EQ(net::ERR_IO_PENDING,pool().Preconnect(Info("http://alpha.test"),1,alpha.GetCallback()));
  ASSERT_EQ(net::ERR_IO_PENDING,pool().Preconnect(Info("http://beta.test"),1,beta.GetCallback()));
  ASSERT_TRUE(alpha_dns && beta_dns);
  base::test::TestFuture<void> drained;
  auto commit=pool().PrepareMatchingStreams(base::BindRepeating(
      [](const net::HttpStreamKey& key){return key.destination().host()=="alpha.test";}),
      drained.GetCallback());
  EXPECT_FALSE(alpha_dns);
  auto fresh_dns=resolver->AddFakeRequest();
  ASSERT_EQ(net::ERR_IO_PENDING,pool().Preconnect(Info("http://alpha.test"),1,fresh.GetCallback()));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha.IsReady());EXPECT_FALSE(drained.IsReady());
  EXPECT_FALSE(beta.IsReady());EXPECT_FALSE(fresh.IsReady());
  EXPECT_TRUE(fresh_dns && beta_dns);
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED,alpha.Get());ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(fresh.IsReady());EXPECT_TRUE(fresh_dns && beta_dns);
  base::test::TestFuture<void> fresh_done,beta_done;
  Retire("alpha.test",fresh_done.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED,fresh.Get());ASSERT_TRUE(fresh_done.Wait());
  Retire("beta.test",beta_done.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED,beta.Get());ASSERT_TRUE(beta_done.Wait());
}

class H2CloseDelegate final : public net::SpdyStream::Delegate {
 public:
  void OnHeadersSent() override { sent = true; }
  void OnEarlyHintsReceived(const quiche::HttpHeaderBlock&) override {}
  void OnHeadersReceived(const quiche::HttpHeaderBlock&) override {}
  void OnDataReceived(std::unique_ptr<net::SpdyBuffer>) override {}
  void OnDataSent() override {}
  void OnTrailers(const quiche::HttpHeaderBlock&) override {}
  void OnClose(int status) override {
    closed = true;
    error = status;
    if (on_close) on_close.Run();
  }
  bool CanGreaseFrameType() const override { return false; }
  net::NetLogSource source_dependency() const override { return {}; }
  bool sent = false, closed = false;
  int error = net::OK;
  base::RepeatingClosure on_close;
};

class NetworkRetirementH2Test : public net::WithTaskEnvironment,
                                public testing::Test {
 protected:
  struct Endpoint {
    explicit Endpoint(const char* url, bool write_headers) : ssl(net::SYNCHRONOUS, net::OK) {
      if (write_headers) {
        frame = std::make_unique<spdy::SpdySerializedFrame>(
            framing.ConstructSpdyGet(url, 1, net::MEDIUM));
        writes.push_back(net::CreateMockWrite(*frame, 1));
      }
      data = std::make_unique<net::StaticSocketDataProvider>(reads, writes);
      data->set_connect_data(net::MockConnect(net::SYNCHRONOUS, net::OK));
    }
    net::SpdyTestUtil framing;
    std::unique_ptr<spdy::SpdySerializedFrame> frame;
    net::MockRead reads[1] = {net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING)};
    std::vector<net::MockWrite> writes;
    std::unique_ptr<net::StaticSocketDataProvider> data;
    net::SSLSocketDataProvider ssl;
  };
  NetworkRetirementH2Test() {
    features_.InitAndDisableFeature(net::features::kHappyEyeballsV3);
    session_ = net::SpdySessionDependencies::SpdyCreateSession(&dependencies_);
    net::SpdySessionPoolPeer(session_->spdy_session_pool())
        .SetEnableSendingInitialData(false);
  }
  net::SpdySessionKey Key(const char* url) {
    return net::SpdySessionKey(net::HostPortPair::FromURL(GURL(url)),
        net::PRIVACY_MODE_DISABLED, net::ProxyChain::Direct(),
        net::SessionUsage::kDestination, net::SocketTag(),
        net::NetworkAnonymizationKey(), net::SecureDnsPolicy::kAllow, false,
        net::handles::kInvalidNetworkHandle);
  }
  base::WeakPtr<net::SpdySession> Create(const char* url, bool headers = false) {
    auto endpoint = std::make_unique<Endpoint>(url, headers);
    dependencies_.socket_factory->AddSocketDataProvider(endpoint->data.get());
    dependencies_.socket_factory->AddSSLSocketDataProvider(&endpoint->ssl);
    endpoints_.push_back(std::move(endpoint));
    return net::CreateSpdySessionWithIpBasedPoolingDisabled(
        session_.get(), Key(url), net::NetLogWithSource());
  }
  void RetireAlpha(base::OnceClosure done) {
    session_->spdy_session_pool()->RetireMatchingSessions(base::BindRepeating(
        [](const net::SpdySessionKey& key) {
          return key.host_port_pair().host() == "alpha.test";
        }), std::move(done));
  }
 public:
  void CreateFreshAlpha() { fresh_ = Create("https://alpha.test"); }
  void CloseAll() {
    session_->spdy_session_pool()->CloseAllSessions();
    base::RunLoop().RunUntilIdle();
  }
 protected:
  base::test::ScopedFeatureList features_;
  std::vector<std::unique_ptr<Endpoint>> endpoints_;
  net::SpdySessionDependencies dependencies_;
  base::WeakPtr<net::SpdySession> fresh_;
  std::unique_ptr<net::HttpNetworkSession> session_;
};

TEST_F(NetworkRetirementH2Test, ActiveNativeStreamClosesAndBetaSessionSurvives) {
  H2CloseDelegate alpha_delegate, beta_delegate;
  base::ScopedClosureRunner cleanup(base::BindOnce(
      &NetworkRetirementH2Test::CloseAll, base::Unretained(this)));
  auto alpha = Create("https://alpha.test", true);
  auto beta = Create("https://beta.test", true);
  ASSERT_TRUE(alpha && beta);
  auto alpha_stream = net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      alpha, GURL("https://alpha.test"), net::MEDIUM, net::NetLogWithSource());
  auto beta_stream = net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      beta, GURL("https://beta.test"), net::MEDIUM, net::NetLogWithSource());
  ASSERT_TRUE(alpha_stream && beta_stream);
  alpha_stream->SetDelegate(&alpha_delegate);
  beta_stream->SetDelegate(&beta_delegate);
  net::SpdyTestUtil framing;
  alpha_stream->SendRequestHeaders(framing.ConstructGetHeaderBlock("https://alpha.test"),
                                   net::NO_MORE_DATA_TO_SEND);
  beta_stream->SendRequestHeaders(framing.ConstructGetHeaderBlock("https://beta.test"),
                                  net::NO_MORE_DATA_TO_SEND);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(alpha_delegate.sent && beta_delegate.sent);
  ASSERT_NE(0u, alpha_stream->stream_id());
  ASSERT_NE(0u, beta_stream->stream_id());
  auto* original_beta = beta.get();
  base::test::TestFuture<void> drained;

  RetireAlpha(drained.GetCallback());

  EXPECT_FALSE(alpha);
  EXPECT_TRUE(alpha_delegate.closed);
  EXPECT_EQ(net::ERR_ABORTED, alpha_delegate.error);
  EXPECT_FALSE(drained.IsReady());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(original_beta, beta.get());
  EXPECT_TRUE(beta->IsAvailable());
  EXPECT_FALSE(beta_delegate.closed);
  EXPECT_TRUE(net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      beta, GURL("https://beta.test/next"), net::MEDIUM, net::NetLogWithSource()));
}

TEST_F(NetworkRetirementH2Test, NativeCloseCallbackCanCreateFreshSession) {
  H2CloseDelegate delegate;
  base::ScopedClosureRunner cleanup(base::BindOnce(
      &NetworkRetirementH2Test::CloseAll, base::Unretained(this)));
  auto alpha = Create("https://alpha.test");
  ASSERT_TRUE(alpha);
  auto stream = net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      alpha, GURL("https://alpha.test"), net::MEDIUM, net::NetLogWithSource());
  ASSERT_TRUE(stream);
  stream->SetDelegate(&delegate);
  delegate.on_close = base::BindRepeating(
      &NetworkRetirementH2Test::CreateFreshAlpha, base::Unretained(this));
  base::test::TestFuture<void> drained;

  RetireAlpha(drained.GetCallback());

  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha);
  ASSERT_TRUE(fresh_);
  EXPECT_TRUE(fresh_->IsAvailable());
  EXPECT_TRUE(net::HasSpdySession(session_->spdy_session_pool(), Key("https://alpha.test")));
}

TEST_F(NetworkRetirementH2Test, PreparedSessionDefersCloseAndPreservesFreshSameKey) {
  H2CloseDelegate alpha_delegate,beta_delegate;
  base::ScopedClosureRunner cleanup(base::BindOnce(
      &NetworkRetirementH2Test::CloseAll,base::Unretained(this)));
  auto alpha=Create("https://alpha.test"),beta=Create("https://beta.test");
  ASSERT_TRUE(alpha && beta);
  auto alpha_stream=net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      alpha,GURL("https://alpha.test"),net::MEDIUM,net::NetLogWithSource());
  auto beta_stream=net::CreateStreamSynchronously(net::SPDY_BIDIRECTIONAL_STREAM,
      beta,GURL("https://beta.test"),net::MEDIUM,net::NetLogWithSource());
  ASSERT_TRUE(alpha_stream && beta_stream);
  alpha_stream->SetDelegate(&alpha_delegate);beta_stream->SetDelegate(&beta_delegate);
  base::test::TestFuture<void> drained;
  auto commit=session_->spdy_session_pool()->PrepareMatchingSessions(base::BindRepeating(
      [](const net::SpdySessionKey& key){return key.host_port_pair().host()=="alpha.test";}),
      drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha_delegate.closed);EXPECT_FALSE(beta_delegate.closed);
  EXPECT_FALSE(drained.IsReady());ASSERT_TRUE(alpha);EXPECT_FALSE(alpha->IsAvailable());
  auto fresh=Create("https://alpha.test");ASSERT_TRUE(fresh);
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha);EXPECT_TRUE(alpha_delegate.closed);
  EXPECT_EQ(net::ERR_ABORTED,alpha_delegate.error);
  ASSERT_TRUE(fresh && beta);EXPECT_TRUE(fresh->IsAvailable());
  EXPECT_TRUE(beta->IsAvailable());EXPECT_FALSE(beta_delegate.closed);
}
}  // namespace
}  // namespace alloy::site_records
