// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "net/base/features.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_request_info.h"
#include "net/http/http_server_properties.h"
#include "net/quic/mock_quic_data.h"
#include "net/quic/quic_crypto_client_config_handle.h"
#include "net/quic/quic_session_attempt_manager.h"
#include "net/quic/quic_session_attempt_request.h"
#include "net/quic/quic_session_pool_peer.h"
#include "net/quic/quic_session_pool_test_base.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net::test {
namespace {
class RecordQuicOwnerTest
    : public QuicSessionPoolTestBase,
      public testing::TestWithParam<quic::ParsedQuicVersion> {
 public:
  explicit RecordQuicOwnerTest(bool async_creation = false)
      : QuicSessionPoolTestBase(
            GetParam(),
            async_creation
                ? std::vector<
                      base::test::
                          FeatureRef>{features::
                                          kPartitionConnectionsByNetworkIsolationKey,
                                      features::kAsyncQuicSession}
                : std::vector<
                      base::test::
                          FeatureRef>{features::
                                          kPartitionConnectionsByNetworkIsolationKey},
            async_creation ? std::vector<base::test::FeatureRef>{}
                           : std::vector<base::test::FeatureRef>{
                                 features::kAsyncQuicSession}) {}

 protected:
  NetworkAnonymizationKey Alpha() const {
    return NetworkAnonymizationKey::CreateSameSite(
        SchemefulSite(GURL("https://alpha.test")));
  }
  NetworkAnonymizationKey Beta() const {
    return NetworkAnonymizationKey::CreateSameSite(
        SchemefulSite(GURL("https://beta.test")));
  }
  void Retire(const NetworkAnonymizationKey& owner, base::OnceClosure done) {
    pool_->RetireNetworkState(
        base::BindRepeating(
            [](NetworkAnonymizationKey wanted,
               const NetworkAnonymizationKey& actual, const quic::QuicServerId&,
               SessionUsage) { return wanted == actual; },
            owner),
        std::move(done));
  }
  base::OnceClosure Prepare(const NetworkAnonymizationKey& owner,base::OnceClosure done) {
    return pool_->PrepareNetworkRetirement(base::BindRepeating(
        [](NetworkAnonymizationKey wanted,const NetworkAnonymizationKey& actual,
           const quic::QuicServerId&,SessionUsage){return wanted==actual;},owner),std::move(done));
  }
  auto Config(const NetworkAnonymizationKey& owner) {
    QuicSessionPool::QuicCryptoClientConfigKey key;
    key.network_anonymization_key = owner;
    return QuicSessionPoolPeer::GetCryptoConfig(pool_.get(), key);
  }
  bssl::UniquePtr<SSL_SESSION> Ticket(SSL_CTX* context) {
    bssl::UniquePtr<SSL_SESSION> ticket(SSL_SESSION_new(context));
    EXPECT_TRUE(SSL_SESSION_set_protocol_version(ticket.get(), TLS1_3_VERSION));
    SSL_SESSION_set_time(ticket.get(), 1000000000);
    SSL_SESSION_set_timeout(ticket.get(), 3600);
    return ticket;
  }
};
INSTANTIATE_TEST_SUITE_P(
    H3OwnerVersions,
    RecordQuicOwnerTest,
    testing::ValuesIn(AllSupportedQuicVersions()),
    [](const testing::TestParamInfo<quic::ParsedQuicVersion>& info) {
      return ParsedQuicVersionToString(info.param);
    });

TEST_P(RecordQuicOwnerTest, ActiveStreamCacheAndMetadataRetireSelectively) {
  Initialize();
  auto details = DefaultProofVerifyDetails();
  crypto_client_stream_factory_.AddProofVerifyDetails(&details);
  crypto_client_stream_factory_.AddProofVerifyDetails(&details);
  MockQuicData alpha_data(version_);
  alpha_data.AddReadPauseForever();
  alpha_data.AddWrite(SYNCHRONOUS, ConstructInitialSettingsPacket());
  alpha_data.AddSocketDataToFactory(socket_factory_.get());
  client_maker_.Reset();
  MockQuicData beta_data(version_);
  beta_data.AddReadPauseForever();
  beta_data.AddWrite(SYNCHRONOUS, ConstructInitialSettingsPacket());
  beta_data.AddSocketDataToFactory(socket_factory_.get());
  RequestBuilder alpha(this), beta(this);
  alpha.network_anonymization_key = Alpha();
  beta.network_anonymization_key = Beta();
  ASSERT_EQ(ERR_IO_PENDING, alpha.CallRequest());
  ASSERT_EQ(OK, callback_.WaitForResult());
  ASSERT_EQ(ERR_IO_PENDING, beta.CallRequest());
  ASSERT_EQ(OK, callback_.WaitForResult());
  HttpRequestInfo request_info;
  request_info.traffic_annotation =
      MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
  auto alpha_stream = CreateStream(&alpha.request),
       beta_stream = CreateStream(&beta.request);
  ASSERT_TRUE(alpha_stream && beta_stream);
  for (auto* stream : {alpha_stream.get(), beta_stream.get()}) {
    stream->RegisterRequest(&request_info);
    ASSERT_EQ(OK, stream->InitializeStream(false, DEFAULT_PRIORITY, net_log_,
                                           CompletionOnceCallback()));
  }
  auto* original_beta =
      GetActiveSession(kDefaultDestination, PRIVACY_MODE_DISABLED, Beta());
  ASSERT_TRUE(original_beta);
  auto alpha_config = Config(Alpha()), beta_config = Config(Beta());
  quic::QuicServerId server(kDefaultServerHostName, kDefaultServerPort);
  auto alpha_cache =
      alpha_config->GetConfig()->session_cache()->CreateConnectionState(server);
  auto beta_cache =
      beta_config->GetConfig()->session_cache()->CreateConnectionState(server);
  bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_method()));
  quic::TransportParameters params;
  alpha_cache->Insert(Ticket(context.get()), params);
  beta_cache->Insert(Ticket(context.get()), params);
  alpha_config->GetConfig()->LookupOrCreate(server)->set_source_address_token(
      "old-alpha");
  beta_config->GetConfig()->LookupOrCreate(server)->set_source_address_token(
      "keep-beta");
  http_server_properties_->ClearServerNetworkStats(kDefaultDestination,
                                                   Alpha());
  base::test::TestFuture<void> drained;
  host_resolver_->set_ondemand_mode(true);
  RequestBuilder pending(this);
  pending.destination =
      url::SchemeHostPort("https", kServer2HostName, kDefaultServerPort);
  pending.url = GURL(kServer2Url);
  pending.network_anonymization_key = Alpha();
  base::test::TestFuture<int> pending_failed;
  pending.callback = base::BindOnce(
      [](base::WeakPtr<QuicChromiumClientSession> original,
         base::test::TestFuture<void>* drained,
         base::test::TestFuture<int>* failed, int status) {
        EXPECT_TRUE(!original || !original->connection()->connected());
        EXPECT_FALSE(drained->IsReady());
        failed->SetValue(status);
      },
      GetActiveSession(kDefaultDestination, PRIVACY_MODE_DISABLED, Alpha())
          ->NetworkRetirementView(),
      base::Unretained(&drained), base::Unretained(&pending_failed));
  ASSERT_EQ(ERR_IO_PENDING, pending.CallRequest());
  Retire(Alpha(), drained.GetCallback());
  EXPECT_EQ(ERR_ABORTED, pending_failed.Get());
  EXPECT_FALSE(drained.IsReady());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(
      HasActiveSession(kDefaultDestination, PRIVACY_MODE_DISABLED, Alpha()));
  EXPECT_EQ(ERR_ABORTED,
            alpha_stream->ReadResponseHeaders(callback_.callback()));
  EXPECT_EQ(original_beta, GetActiveSession(kDefaultDestination,
                                            PRIVACY_MODE_DISABLED, Beta()));
  EXPECT_EQ(nullptr, http_server_properties_->GetServerNetworkStats(
                         kDefaultDestination, Alpha()));
  EXPECT_TRUE(alpha_config->GetConfig()
                  ->LookupOrCreate(server)
                  ->source_address_token()
                  .empty());
  EXPECT_EQ(
      "keep-beta",
      beta_config->GetConfig()->LookupOrCreate(server)->source_address_token());
  alpha_cache->Insert(Ticket(context.get()), params);
  EXPECT_FALSE(alpha_cache->Lookup(
      quic::QuicWallTime::FromUNIXSeconds(1000000000), context.get()));
  EXPECT_TRUE(beta_cache->Lookup(
      quic::QuicWallTime::FromUNIXSeconds(1000000000), context.get()));
  base::test::TestFuture<void> cleanup;
  Retire(Beta(), cleanup.GetCallback());
  ASSERT_TRUE(cleanup.Wait());
  alpha_data.ExpectAllReadDataConsumed();
  alpha_data.ExpectAllWriteDataConsumed();
  beta_data.ExpectAllReadDataConsumed();
  beta_data.ExpectAllWriteDataConsumed();
}

TEST_P(RecordQuicOwnerTest, PendingDnsCallbacksCanDeleteSiblingRequest) {
  Initialize();
  host_resolver_->set_ondemand_mode(true);
  RequestBuilder beta(this);
  auto first = std::make_unique<RequestBuilder>(this);
  auto second = std::make_unique<RequestBuilder>(this);
  first->network_anonymization_key = Alpha();
  second->network_anonymization_key = Alpha();
  beta.network_anonymization_key = Beta();
  base::test::TestFuture<int> failed, other;
  int hosts = 0, completions = 0;
  auto callback = [](int* hosts, int* completions,
                     base::test::TestFuture<int>* failed,
                     std::unique_ptr<RequestBuilder>* sibling, int status) {
    EXPECT_EQ(1, *hosts);
    ++*completions;
    sibling->reset();
    failed->SetValue(status);
  };
  first->callback = base::BindOnce(
      callback, base::Unretained(&hosts), base::Unretained(&completions),
      base::Unretained(&failed), base::Unretained(&second));
  second->callback = base::BindOnce(
      callback, base::Unretained(&hosts), base::Unretained(&completions),
      base::Unretained(&failed), base::Unretained(&first));
  beta.callback = other.GetCallback();
  ASSERT_EQ(ERR_IO_PENDING, first->CallRequest());
  ASSERT_EQ(ERR_IO_PENDING, second->CallRequest());
  ASSERT_EQ(ERR_IO_PENDING, beta.CallRequest());
  for (auto* request : {first.get(), second.get()}) {
    ASSERT_TRUE(request->request.WaitForHostResolution(base::BindOnce(
        [](int* count, int status) {
          EXPECT_EQ(ERR_ABORTED, status);
          ++*count;
        },
        base::Unretained(&hosts))));
  }
  base::test::TestFuture<void> drained;
  Retire(Alpha(), drained.GetCallback());
  EXPECT_EQ(ERR_ABORTED, failed.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(1, completions);
  EXPECT_EQ(1, hosts);
  EXPECT_NE(static_cast<bool>(first), static_cast<bool>(second));
  EXPECT_FALSE(other.IsReady());
  base::test::TestFuture<void> cleanup;
  Retire(Beta(), cleanup.GetCallback());
  EXPECT_EQ(ERR_ABORTED, other.Get());
  ASSERT_TRUE(cleanup.Wait());
}

TEST_P(RecordQuicOwnerTest, HalfHandshakeSessionAndLegacyJobDrain) {
  Initialize();
  auto details = DefaultProofVerifyDetails();
  crypto_client_stream_factory_.AddProofVerifyDetails(&details);
  crypto_client_stream_factory_.set_handshake_mode(
      MockCryptoClientStream::COLD_START);
  MockQuicData data(version_);
  data.AddReadPauseForever();
  data.AddSocketDataToFactory(socket_factory_.get());
  RequestBuilder request(this);
  request.network_anonymization_key = Alpha();
  base::test::TestFuture<int> failed;
  request.callback = failed.GetCallback();
  ASSERT_EQ(ERR_IO_PENDING, request.CallRequest());
  ASSERT_TRUE(base::test::RunUntil([&] {
    return QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()) == 1;
  }));
  EXPECT_EQ(0u, pool_->CountActiveSessions());
  base::test::TestFuture<void> drained;
  Retire(Alpha(), drained.GetCallback());
  EXPECT_EQ(ERR_ABORTED, failed.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(0u, QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()));
}

TEST_P(RecordQuicOwnerTest, ModernAttemptGraphDrainsPendingHandshake) {
  Initialize();
  auto details = DefaultProofVerifyDetails();
  crypto_client_stream_factory_.AddProofVerifyDetails(&details);
  crypto_client_stream_factory_.set_handshake_mode(
      MockCryptoClientStream::COLD_START);
  MockQuicData data(version_);
  data.AddReadPauseForever();
  data.AddSocketDataToFactory(socket_factory_.get());
  QuicSessionKey key(
      kDefaultServerHostName, kDefaultServerPort, PRIVACY_MODE_DISABLED,
      ProxyChain::Direct(), SessionUsage::kDestination, SocketTag(), Alpha(),
      SecureDnsPolicy::kAllow, false, false, handles::kInvalidNetworkHandle);
  auto request = pool_->session_attempt_manager()->CreateRequest(
      QuicSessionAliasKey(kDefaultDestination, key));
  base::test::TestFuture<int> failed;
  ASSERT_EQ(ERR_IO_PENDING,
            request->RequestSession(
                QuicEndpoint(
                    version_,
                    IPEndPoint(IPAddress::IPv4Localhost(), kDefaultServerPort),
                    ConnectionEndpointMetadata()),
                0, base::TimeTicks(), base::TimeTicks(), std::nullopt, true, {},
                MultiplexedSessionCreationInitiator::kUnknown, std::nullopt,
                net_log_, failed.GetCallback()));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()) == 1;
  }));
  base::test::TestFuture<void> drained;
  Retire(Alpha(), drained.GetCallback());
  EXPECT_EQ(ERR_ABORTED, failed.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(nullptr, request->session());
  EXPECT_EQ(0u, QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()));
}

TEST_P(RecordQuicOwnerTest, PreparedDnsWaitsForCommitAndFreshSameKeySurvives) {
  Initialize();host_resolver_->set_ondemand_mode(true);
  RequestBuilder alpha(this),beta(this),fresh(this);
  alpha.network_anonymization_key=Alpha();beta.network_anonymization_key=Beta();
  fresh.network_anonymization_key=Alpha();
  base::test::TestFuture<int> alpha_failed,beta_failed,fresh_failed;
  alpha.callback=alpha_failed.GetCallback();beta.callback=beta_failed.GetCallback();
  fresh.callback=fresh_failed.GetCallback();
  ASSERT_EQ(ERR_IO_PENDING,alpha.CallRequest());ASSERT_EQ(ERR_IO_PENDING,beta.CallRequest());
  base::test::TestFuture<void> drained;
  auto commit=Prepare(Alpha(),drained.GetCallback());
  ASSERT_EQ(ERR_IO_PENDING,fresh.CallRequest());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha_failed.IsReady());EXPECT_FALSE(drained.IsReady());
  EXPECT_FALSE(beta_failed.IsReady());EXPECT_FALSE(fresh_failed.IsReady());
  std::move(commit).Run();
  EXPECT_EQ(ERR_ABORTED,alpha_failed.Get());ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta_failed.IsReady());EXPECT_FALSE(fresh_failed.IsReady());
  base::test::TestFuture<void> fresh_done,beta_done;
  Retire(Alpha(),fresh_done.GetCallback());
  EXPECT_EQ(ERR_ABORTED,fresh_failed.Get());ASSERT_TRUE(fresh_done.Wait());
  Retire(Beta(),beta_done.GetCallback());
  EXPECT_EQ(ERR_ABORTED,beta_failed.Get());ASSERT_TRUE(beta_done.Wait());
}

class RecordQuicAsyncOwnerTest : public RecordQuicOwnerTest {
 public:
  RecordQuicAsyncOwnerTest() : RecordQuicOwnerTest(true) {}
};
INSTANTIATE_TEST_SUITE_P(
    H3AsyncOwnerVersions,
    RecordQuicAsyncOwnerTest,
    testing::ValuesIn(AllSupportedQuicVersions()),
    [](const testing::TestParamInfo<quic::ParsedQuicVersion>& info) {
      return ParsedQuicVersionToString(info.param);
    });

TEST_P(RecordQuicAsyncOwnerTest,
       QueuedSocketCompletionCannotCreateRetiredSession) {
  Initialize();
  host_resolver_->set_synchronous_mode(true);
  MockQuicData data(version_);
  data.AddSocketDataToFactory(socket_factory_.get());
  RequestBuilder request(this);
  request.network_anonymization_key = Alpha();
  base::test::TestFuture<int> failed;
  request.callback = failed.GetCallback();
  ASSERT_EQ(ERR_IO_PENDING, request.CallRequest());
  // Native socket configuration completed, but FinishCreateSession is queued.
  ASSERT_EQ(0u, QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()));
  base::test::TestFuture<int> created;
  ASSERT_TRUE(
      request.request.WaitForQuicSessionCreation(created.GetCallback()));
  EXPECT_FALSE(created.IsReady());
  base::test::TestFuture<void> drained;
  Retire(Alpha(), drained.GetCallback());
  EXPECT_EQ(ERR_ABORTED, failed.Get());
  EXPECT_EQ(ERR_ABORTED, created.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(0u, QuicSessionPoolPeer::GetNumLiveSessions(pool_.get()));
  EXPECT_FALSE(
      HasActiveSession(kDefaultDestination, PRIVACY_MODE_DISABLED, Alpha()));
  EXPECT_FALSE(request.request.WaitForQuicSessionCreation(base::BindOnce(
      [](int) { ADD_FAILURE() << "Retired creation callback ran twice"; })));
  // The native request's own admission above includes Alpha's NAK. The generic
  // HasActiveJob fixture helper checks only the empty NAK, so it cannot observe
  // this job. A fresh real request proves that no retired job captures new
  // work.
  auto details = DefaultProofVerifyDetails();
  crypto_client_stream_factory_.AddProofVerifyDetails(&details);
  MockQuicData fresh_data(version_);
  fresh_data.AddReadPauseForever();
  fresh_data.AddWrite(SYNCHRONOUS, ConstructInitialSettingsPacket());
  fresh_data.AddSocketDataToFactory(socket_factory_.get());
  RequestBuilder fresh(this);
  fresh.network_anonymization_key = Alpha();
  ASSERT_EQ(ERR_IO_PENDING, fresh.CallRequest());
  ASSERT_EQ(OK, callback_.WaitForResult());
  EXPECT_TRUE(
      HasActiveSession(kDefaultDestination, PRIVACY_MODE_DISABLED, Alpha()));
  base::test::TestFuture<void> cleanup;
  Retire(Alpha(), cleanup.GetCallback());
  ASSERT_TRUE(cleanup.Wait());
  data.ExpectAllReadDataConsumed();
  data.ExpectAllWriteDataConsumed();
  fresh_data.ExpectAllReadDataConsumed();
  fresh_data.ExpectAllWriteDataConsumed();
}
}  // namespace
}  // namespace net::test
