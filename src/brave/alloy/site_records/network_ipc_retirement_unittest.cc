// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/network_retirement_test_context.h"
#include "brave/alloy/site_records/web_bundle_retirement_test_context.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/alloy_network_record_target.h"
#include <ctime>
#include "base/test/scoped_feature_list.h"
#include "net/base/features.h"
#include "net/base/test_completion_callback.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_info.h"
#include "net/http/http_transaction_factory.h"
#include "net/log/net_log.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/transport_client_socket_pool_test_util.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace alloy::site_records {
namespace {
class NetworkRecordTargetTest : public testing::Test {};
TEST_F(NetworkRecordTargetTest, CanonicalPrivateRegistryIpAndIntranet) {
  auto target=net::AlloyNetworkRecordTarget::FromPrincipal(
      GURL("https://en.family.blogspot.com/page"),
      url::Origin::Create(GURL("https://en.family.blogspot.com")));
  ASSERT_TRUE(target);
  EXPECT_EQ("family.blogspot.com",target->domain());
  EXPECT_TRUE(target->Owns(url::Origin::Create(GURL("http://ru.family.blogspot.com"))));
  EXPECT_FALSE(target->Owns(url::Origin::Create(GURL("https://other.blogspot.com"))));
  EXPECT_TRUE(net::AlloyNetworkRecordTarget::FromCanonicalDomain("127.0.0.1"));
  EXPECT_TRUE(net::AlloyNetworkRecordTarget::FromCanonicalDomain("intranet"));
}
TEST_F(NetworkRecordTargetTest, RejectsNonRecordPrincipalsAndNonCanonicalWire) {
  const auto inherited=url::Origin::Create(GURL("https://alpha.test"));
  for (const char* principal:{"about:blank","file:///notes","data:text/plain,body"})
    EXPECT_FALSE(net::AlloyNetworkRecordTarget::FromPrincipal(GURL(principal),inherited));
  EXPECT_FALSE(net::AlloyNetworkRecordTarget::FromPrincipal(GURL("https://alpha.test"),url::Origin()));
  for (const char* domain:{"","ALPHA.test","sub.alpha.test","alpha.test:443","https://alpha.test","alpha.test/path"})
    EXPECT_FALSE(net::AlloyNetworkRecordTarget::FromCanonicalDomain(domain));
}
TEST_F(NetworkRecordTargetTest, SeparatesNativeOwnershipServerDependencyAndProxy) {
  auto target=net::AlloyNetworkRecordTarget::FromCanonicalDomain("alpha.test");
  ASSERT_TRUE(target);
  const auto alpha=net::NetworkAnonymizationKey::CreateSameSite(net::SchemefulSite(GURL("https://alpha.test")));
  const auto beta=net::NetworkAnonymizationKey::CreateSameSite(net::SchemefulSite(GURL("https://beta.test")));
  EXPECT_TRUE(target->DependsOnTransport(alpha,GURL("https://cdn.test")));
  EXPECT_TRUE(target->DependsOnTransport(beta,GURL("https://alpha.test")));
  EXPECT_FALSE(target->DependsOnTransport(beta,GURL("https://beta.test")));
  EXPECT_FALSE(target->DependsOnTransport(beta,GURL("https://alpha.test"),net::SessionUsage::kProxy));
  EXPECT_TRUE(target->DependsOnTransport(alpha,GURL("https://beta.test"),net::SessionUsage::kProxy));
}

class NetworkIPCRetirementTest : public WebBundleRetirementTest {
 protected:
  NetworkIPCRetirementTest() {
    features_.InitWithFeatures({net::features::kPartitionConnectionsByNetworkIsolationKey},
                              {net::features::kHappyEyeballsV3});
    network_=std::make_unique<NetworkRetirementTestContext>(base::BindOnce(
        [](net::ClientSocketFactory* sockets,net::URLRequestContextBuilder* builder) {
          builder->set_client_socket_factory_for_testing(sockets);
        },base::Unretained(&sockets_)));
    UseManager(network_->context().GetWebBundleManager());
  }
  mojo::Remote<network::mojom::AlloyNetworkRetirement> PrepareIPC() {
    mojo::Remote<network::mojom::AlloyNetworkRetirement> retirement;
    base::test::TestFuture<bool> prepared;
    network_->remote()->PrepareAlloyNetworkServiceRetirement(
        "alpha.test",retirement.BindNewPipeAndPassReceiver(),prepared.GetCallback());
    EXPECT_TRUE(prepared.Get());
    return retirement;
  }
  net::HttpNetworkSession& session() {
    return *network_->context().url_request_context()->http_transaction_factory()->GetSession();
  }
  int ConnectNative(net::ClientSocketHandle& handle,const char* owner) {
    const net::ClientSocketPool::GroupId key(url::SchemeHostPort("http","transport.test",80),
        net::PRIVACY_MODE_DISABLED,Isolation(owner).network_anonymization_key(),
        net::SecureDnsPolicy::kAllow,false,net::handles::kInvalidNetworkHandle);
    return handle.Init(key,net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),
        std::nullopt,net::LOW,net::SocketTag(),net::ClientSocketPool::RespectLimits::ENABLED,
        base::DoNothing(),net::ClientSocketPool::ProxyAuthCallback(),
        session().GetSocketPool(net::HttpNetworkSession::SocketPoolType::kNormal,
                                net::ProxyChain::Direct()),net::NetLogWithSource());
  }
  bssl::UniquePtr<SSL_SESSION> Ticket() {
    bssl::UniquePtr<SSL_SESSION> ticket(SSL_SESSION_new(ssl_context_.get()));
    EXPECT_TRUE(SSL_SESSION_set_protocol_version(ticket.get(),TLS1_2_VERSION));
    SSL_SESSION_set_time(ticket.get(),static_cast<uint64_t>(std::time(nullptr)));
    SSL_SESSION_set_timeout(ticket.get(),3600);
    return ticket;
  }
 public:
  void PreconnectReentry(net::ClientSocketHandle* fresh,
                         base::test::TestFuture<void>* callback,
                         base::test::TestFuture<bool>* drained) {
    EXPECT_FALSE(drained->IsReady());
    network_->resolver().set_ondemand_mode(false);
    network_->resolver().set_synchronous_mode(true);
    EXPECT_EQ(net::OK,ConnectNative(*fresh,"alpha.test"));
    EXPECT_TRUE(fresh->is_initialized());
    callback->SetValue();
  }
 protected:
  base::test::ScopedFeatureList features_;
  net::MockTransportClientSocketFactory sockets_{net::NetLog::Get()};
  bssl::UniquePtr<SSL_CTX> ssl_context_{SSL_CTX_new(TLS_method())};
  std::unique_ptr<NetworkRetirementTestContext> network_;
};

TEST_F(NetworkIPCRetirementTest, PrepareIsSilentAndCommitPreservesBetaBody) {
  BundleHandle alpha_handle,beta_handle;
  const auto alpha_token=base::UnguessableToken::Create(),beta_token=base::UnguessableToken::Create();
  auto alpha=Create(alpha_handle,alpha_token);
  const GURL beta_bundle("https://beta.test/bundle.wbn"),beta_resource("https://beta.test/data");
  auto beta=Create(beta_handle,beta_token,"beta.test",beta_bundle);
  network::TestURLLoaderClient a_client,b_client;
  auto a=Request(alpha_token,a_client);
  auto b=Request(beta_token,b_client,"beta.test",{},beta_bundle,beta_resource);
  tasks_.RunUntilIdle();
  auto retirement=PrepareIPC();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(alpha);
  EXPECT_TRUE(beta);
  EXPECT_FALSE(a_client.has_received_completion());
  EXPECT_FALSE(b_client.has_received_completion());
  base::test::TestFuture<bool> drained;
  retirement->Commit(drained.GetCallback());
  a_client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED,a_client.completion_status().error_code);
  ASSERT_TRUE(drained.Get());
  ASSERT_TRUE(beta);
  Fill(*beta,"beta-after-native-ipc","text/plain",beta_resource);
  b_client.RunUntilComplete();
  EXPECT_EQ(net::OK,b_client.completion_status().error_code);
  EXPECT_EQ("beta-after-native-ipc",Read(b_client.response_body_release()));
}

TEST_F(NetworkIPCRetirementTest, CommitReplyWaitsForActualHeldNetworkWriter) {
  BundleHandle handle;
  auto token=base::UnguessableToken::Create();
  auto factory=Create(handle,token);
  ASSERT_TRUE(factory);
  std::string payload(1024*1024,'n');
  Fill(*factory,payload);
  network::TestURLLoaderClient client;
  auto loader=Request(token,client);
  client.RunUntilResponseReceived();
  tasks_.RunUntilIdle();
  auto retirement=PrepareIPC();
  base::test::TestFuture<bool> drained;
  {
    base::test::TaskEnvironment::ParallelExecutionFence hold_native;
    retirement->Commit(drained.GetCallback());
    client.RunUntilComplete();
    EXPECT_EQ(net::ERR_ABORTED,client.completion_status().error_code);
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(drained.IsReady());
  }
  ASSERT_TRUE(drained.Get());
  EXPECT_LT(Read(client.response_body_release()).size(),payload.size());
}

TEST_F(NetworkIPCRetirementTest, OverlapAndSameKeyFreshFactoryAreOriginalSnapshots) {
  BundleHandle first_handle,second_handle,fresh_handle;
  auto token=base::UnguessableToken::Create();
  auto first=Create(first_handle,token);
  network::TestURLLoaderClient first_client,second_client,fresh_client;
  auto a=Request(token,first_client);
  auto first_batch=PrepareIPC();
  auto second=Create(second_handle,token);
  auto b=Request(token,second_client);
  auto second_batch=PrepareIPC();
  auto fresh=Create(fresh_handle,token);
  auto f=Request(token,fresh_client);
  base::test::TestFuture<bool> first_done,second_done;
  first_batch->Commit(first_done.GetCallback());
  first_client.RunUntilComplete();
  ASSERT_TRUE(first_done.Get());
  EXPECT_FALSE(second_client.has_received_completion());
  second_batch->Commit(second_done.GetCallback());
  second_client.RunUntilComplete();
  ASSERT_TRUE(second_done.Get());
  ASSERT_TRUE(fresh);
  Fill(*fresh,"fresh-after-ipc-snapshots");
  fresh_client.RunUntilComplete();
  EXPECT_EQ(net::OK,fresh_client.completion_status().error_code);
  EXPECT_EQ("fresh-after-ipc-snapshots",Read(fresh_client.response_body_release()));
}

TEST_F(NetworkIPCRetirementTest, DroppedPreparedEndpointRevokesOriginal) {
  BundleHandle handle;
  auto token=base::UnguessableToken::Create();
  auto factory=Create(handle,token);
  network::TestURLLoaderClient client;
  auto loader=Request(token,client);
  auto retirement=PrepareIPC();
  retirement.reset();
  client.RunUntilComplete();
  EXPECT_FALSE(factory);
  EXPECT_EQ(net::ERR_ABORTED,client.completion_status().error_code);
}

TEST_F(NetworkIPCRetirementTest, InvalidWireTargetCannotRetireNativeOwners) {
  BundleHandle handle;
  auto token=base::UnguessableToken::Create();
  auto factory=Create(handle,token);
  network::TestURLLoaderClient client;
  auto loader=Request(token,client);
  mojo::Remote<network::mojom::AlloyNetworkRetirement> retirement;
  base::test::TestFuture<bool> prepared;
  network_->remote()->PrepareAlloyNetworkServiceRetirement("sub.alpha.test",
      retirement.BindNewPipeAndPassReceiver(),prepared.GetCallback());
  EXPECT_FALSE(prepared.Get());
  ASSERT_TRUE(factory);
  EXPECT_FALSE(client.has_received_completion());
  Fill(*factory,"not-retired-by-invalid-target");
  client.RunUntilComplete();
  EXPECT_EQ(net::OK,client.completion_status().error_code);
  EXPECT_EQ("not-retired-by-invalid-target",Read(client.response_body_release()));
}

TEST_F(NetworkIPCRetirementTest, ContextDestructionDropsPendingCommitAsFailure) {
  BundleHandle handle;
  auto token=base::UnguessableToken::Create();
  auto factory=Create(handle,token);
  network::TestURLLoaderClient client;
  auto loader=Request(token,client);
  auto retirement=PrepareIPC();
  base::test::TestFuture<bool> drained;
  retirement->Commit(mojo::WrapCallbackWithDefaultInvokeIfNotRun(drained.GetCallback(),false));
  network_.reset();
  EXPECT_FALSE(drained.Get());
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED,client.completion_status().error_code);
  EXPECT_FALSE(factory);
}

TEST_F(NetworkIPCRetirementTest, SecondCommitIsRejectedWhileFirstNativeDrainIsHeld) {
  BundleHandle handle;
  auto token=base::UnguessableToken::Create();
  auto factory=Create(handle,token);
  ASSERT_TRUE(factory);
  Fill(*factory,std::string(1024*1024,'d'));
  network::TestURLLoaderClient client;
  auto loader=Request(token,client);
  client.RunUntilResponseReceived();
  tasks_.RunUntilIdle();
  auto retirement=PrepareIPC();
  base::test::TestFuture<bool> first,second;
  {
    base::test::TaskEnvironment::ParallelExecutionFence hold_native;
    retirement->Commit(first.GetCallback());
    retirement->Commit(second.GetCallback());
    EXPECT_FALSE(second.Get());
    EXPECT_FALSE(first.IsReady());
  }
  EXPECT_TRUE(first.Get());
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED,client.completion_status().error_code);
}
TEST_F(NetworkIPCRetirementTest, TransportHandoutHoldsReplyAndFreshSameKeySurvives) {
  network_->resolver().set_synchronous_mode(true);
  net::ClientSocketHandle alpha,beta,fresh;
  ASSERT_EQ(net::OK,ConnectNative(alpha,"alpha.test"));
  ASSERT_EQ(net::OK,ConnectNative(beta,"beta.test"));
  ASSERT_TRUE(alpha.socket() && beta.socket());
  auto* original_beta=beta.socket();
  auto retirement=PrepareIPC();
  EXPECT_FALSE(alpha.socket()->IsConnected());
  EXPECT_TRUE(beta.socket()->IsConnected());
  ASSERT_EQ(net::OK,ConnectNative(fresh,"alpha.test"));
  base::test::TestFuture<bool> drained;
  retirement->Commit(drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());
  EXPECT_TRUE(fresh.socket()->IsConnected());
  alpha.Reset();ASSERT_TRUE(drained.Get());
  EXPECT_TRUE(fresh.socket()->IsConnected());
  EXPECT_EQ(original_beta,beta.socket());EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkIPCRetirementTest, CrossOwnerCallbackCreatesFreshWorkBeforeOldPoolCommit) {
  network_->resolver().set_synchronous_mode(true);
  net::ClientSocketHandle original,beta,fresh;
  ASSERT_EQ(net::OK,ConnectNative(original,"alpha.test"));
  ASSERT_EQ(net::OK,ConnectNative(beta,"beta.test"));
  network_->resolver().set_ondemand_mode(true);
  network_->resolver().set_synchronous_mode(false);
  net::HttpRequestInfo info;
  info.url=GURL("http://preconnect.test/");info.method="GET";
  const net::SchemefulSite site(GURL("https://alpha.test"));
  info.network_isolation_key=net::NetworkIsolationKey(site,site);
  info.network_anonymization_key=Isolation("alpha.test").network_anonymization_key();
  info.traffic_annotation=net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
  base::test::TestFuture<void> callback;
  base::test::TestFuture<bool> drained;
  session().http_stream_factory()->PreconnectStreams(1,info,
      base::BindOnce(&NetworkIPCRetirementTest::PreconnectReentry,
                     base::Unretained(this),&fresh,&callback,&drained));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(callback.IsReady());
  auto retirement=PrepareIPC();
  EXPECT_FALSE(callback.IsReady());
  retirement->Commit(drained.GetCallback());
  ASSERT_TRUE(callback.Wait());
  ASSERT_TRUE(fresh.socket());EXPECT_TRUE(fresh.socket()->IsConnected());
  EXPECT_FALSE(original.socket()->IsConnected());
  EXPECT_TRUE(beta.socket()->IsConnected());
  EXPECT_FALSE(drained.IsReady());
  original.Reset();ASSERT_TRUE(drained.Get());
  EXPECT_TRUE(fresh.socket()->IsConnected());EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkIPCRetirementTest, TlsInsertionFenceRejectsOldAndPreservesFreshAndBeta) {
  auto& cache=*session().ssl_client_context()->ssl_client_session_cache();
  net::SSLClientSessionCache::Key alpha,beta;
  alpha.server=net::HostPortPair("transport.test",443);
  alpha.network_anonymization_key=Isolation("alpha.test").network_anonymization_key();
  beta=alpha;beta.network_anonymization_key=Isolation("beta.test").network_anonymization_key();
  auto old=cache.CaptureInsertion(alpha),unrelated=cache.CaptureInsertion(beta);
  cache.Insert(*old,alpha,Ticket());cache.Insert(*unrelated,beta,Ticket());
  ASSERT_TRUE(cache.Lookup(alpha));ASSERT_TRUE(cache.Lookup(beta));
  auto retirement=PrepareIPC();
  EXPECT_FALSE(cache.Lookup(alpha));EXPECT_TRUE(cache.Lookup(beta));
  cache.Insert(*old,alpha,Ticket());EXPECT_FALSE(cache.Lookup(alpha));
  auto fresh=cache.CaptureInsertion(alpha);
  cache.Insert(*fresh,alpha,Ticket());ASSERT_TRUE(cache.Lookup(alpha));
  base::test::TestFuture<bool> drained;
  retirement->Commit(drained.GetCallback());ASSERT_TRUE(drained.Get());
  EXPECT_TRUE(cache.Lookup(alpha));EXPECT_TRUE(cache.Lookup(beta));
}
}  // namespace
}  // namespace alloy::site_records
