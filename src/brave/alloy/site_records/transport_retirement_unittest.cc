// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "net/base/features.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/http/http_network_session.h"
#include "net/log/net_log.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/transport_client_socket_pool.h"
#include "net/socket/transport_client_socket_pool_test_util.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/test/test_with_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/scheme_host_port.h"

namespace alloy::site_records {
namespace {

class NetworkRetirementTransportTest : public net::WithTaskEnvironment,
                                       public testing::Test {
 protected:
  NetworkRetirementTransportTest() : sockets_(net::NetLog::Get()) {
    features_.InitAndDisableFeature(net::features::kHappyEyeballsV2);
    auto verifier = std::make_unique<net::MockCertVerifier>();
    verifier->set_default_result(net::OK);
    dependencies_.cert_verifier = std::move(verifier);
    session_ = net::SpdySessionDependencies::SpdyCreateSession(&dependencies_);
    params_ = std::make_unique<net::CommonConnectJobParams>(
        session_->CreateCommonConnectJobParams());
    params_->client_socket_factory = &sockets_;
    pool_ = std::make_unique<net::TransportClientSocketPool>(
        32, 6, base::Seconds(10), net::ProxyChain::Direct(), false, params_.get());
  }

  static net::ClientSocketPool::GroupId Group(const char* host) {
    return net::ClientSocketPool::GroupId(
        url::SchemeHostPort("http", host, 80), net::PRIVACY_MODE_DISABLED,
        net::NetworkAnonymizationKey(), net::SecureDnsPolicy::kAllow, false,
        net::handles::kInvalidNetworkHandle);
  }

  void Connect(net::ClientSocketHandle& handle, const char* host) {
    net::TestCompletionCallback callback;
    const int result = handle.Init(Group(host),
        net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),
        std::nullopt, net::LOW, net::SocketTag(),
        net::ClientSocketPool::RespectLimits::ENABLED, callback.callback(),
        net::ClientSocketPool::ProxyAuthCallback(), pool_.get(), net::NetLogWithSource());
    ASSERT_EQ(net::OK, callback.GetResult(result));
    ASSERT_TRUE(handle.is_initialized());
  }

  void RetireAlpha(base::OnceClosure done) {
    pool_->RetireMatchingGroups(base::BindRepeating(
        [](const net::ClientSocketPool::GroupId& group) {
          return group.destination().host() == "alpha.test";
        }), std::move(done));
  }

  base::OnceClosure PrepareAlpha(base::OnceClosure done) {
    return pool_->PrepareMatchingGroups(base::BindRepeating(
        [](const net::ClientSocketPool::GroupId& group) {
          return group.destination().host() == "alpha.test";
        }), std::move(done));
  }

  void Stall(net::ClientSocketHandle& handle,
             net::TestCompletionCallback& callback) {
    sockets_.set_default_client_socket_type(
        net::MockTransportClientSocketFactory::Type::kStalled);
    ASSERT_EQ(net::ERR_IO_PENDING, handle.Init(Group("alpha.test"),
        net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),
        std::nullopt, net::LOW, net::SocketTag(),
        net::ClientSocketPool::RespectLimits::ENABLED, callback.callback(),
        net::ClientSocketPool::ProxyAuthCallback(), pool_.get(), net::NetLogWithSource()));
  }

 public:
  void PreconnectCallbackScope(base::test::TestFuture<void>* entered,
                              base::test::TestFuture<void>* drained,
                              bool success,
                              std::unique_ptr<net::ClientSocketHandle>) {
    EXPECT_TRUE(success);
    auto commit=PrepareAlpha(drained->GetCallback());
    std::move(commit).Run();
    base::RunLoop nested(base::RunLoop::Type::kNestableTasksAllowed);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,nested.QuitClosure());
    nested.Run();
    EXPECT_FALSE(drained->IsReady());
    entered->SetValue();
  }

  void FailedCallbackScope(base::test::TestFuture<void>* entered,
                           base::test::TestFuture<void>* drained,
                           net::ClientSocketHandle* handle,int status) {
    EXPECT_EQ(net::ERR_ABORTED,status);
    handle->Reset();
    auto commit=PrepareAlpha(drained->GetCallback());
    std::move(commit).Run();
    base::RunLoop nested(base::RunLoop::Type::kNestableTasksAllowed);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,nested.QuitClosure());
    nested.Run();
    EXPECT_FALSE(drained->IsReady());
    entered->SetValue();
  }

 protected:
  base::test::ScopedFeatureList features_;
  net::MockTransportClientSocketFactory sockets_;
  net::SpdySessionDependencies dependencies_;
  std::unique_ptr<net::HttpNetworkSession> session_;
  std::unique_ptr<net::CommonConnectJobParams> params_;
  std::unique_ptr<net::TransportClientSocketPool> pool_;
};

TEST_F(NetworkRetirementTransportTest, NativeHandleHoldsSelectiveCompletion) {
  net::ClientSocketHandle alpha, alpha_idle, beta, beta_idle;
  Connect(alpha, "alpha.test");
  Connect(alpha_idle, "alpha.test");
  Connect(beta, "beta.test");
  Connect(beta_idle, "beta.test");
  ASSERT_TRUE(alpha.socket() && alpha_idle.socket() && beta.socket() && beta_idle.socket());
  auto* original_alpha = alpha.socket();
  auto* original_beta = beta.socket();
  auto* original_beta_idle = beta_idle.socket();
  alpha_idle.Reset();
  beta_idle.Reset();
  ASSERT_EQ(2u, pool_->IdleSocketCount());

  base::test::TestFuture<void> drained;
  RetireAlpha(drained.GetCallback());
  EXPECT_FALSE(original_alpha->IsConnected());
  EXPECT_TRUE(original_beta->IsConnected());
  EXPECT_EQ(1u, pool_->IdleSocketCount());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());  // Actual original native handout is held.

  net::ClientSocketHandle fresh, reused_beta;
  Connect(fresh, "alpha.test");
  Connect(reused_beta, "beta.test");
  ASSERT_TRUE(fresh.socket() && reused_beta.socket());
  EXPECT_NE(original_alpha, fresh.socket());
  EXPECT_EQ(original_beta_idle, reused_beta.socket());
  alpha.Reset();
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(fresh.socket()->IsConnected());
  EXPECT_EQ(original_beta, beta.socket());
  EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkRetirementTransportTest, PendingNativeErrorPrecedesCompletion) {
  sockets_.set_default_client_socket_type(
      net::MockTransportClientSocketFactory::Type::kStalled);
  net::ClientSocketHandle alpha;
  net::TestCompletionCallback callback;
  const int result = alpha.Init(Group("alpha.test"),
      net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),
      std::nullopt, net::LOW, net::SocketTag(),
      net::ClientSocketPool::RespectLimits::ENABLED, callback.callback(),
      net::ClientSocketPool::ProxyAuthCallback(), pool_.get(), net::NetLogWithSource());
  ASSERT_EQ(net::ERR_IO_PENDING, result);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(callback.have_result());
  base::test::TestFuture<void> drained;

  RetireAlpha(drained.GetCallback());

  EXPECT_FALSE(drained.IsReady());
  EXPECT_EQ(net::ERR_ABORTED, callback.WaitForResult());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha.is_initialized());
}

TEST_F(NetworkRetirementTransportTest, OverlappingRetirementsKeepOriginalHandouts) {
  net::ClientSocketHandle original, fresh, beta;
  Connect(original, "alpha.test");
  Connect(beta, "beta.test");
  ASSERT_TRUE(original.socket() && beta.socket());
  base::test::TestFuture<void> first, second;
  RetireAlpha(first.GetCallback());
  Connect(fresh, "alpha.test");
  ASSERT_TRUE(fresh.socket());
  RetireAlpha(second.GetCallback());
  EXPECT_FALSE(fresh.socket()->IsConnected());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(second.IsReady());

  original.Reset();
  ASSERT_TRUE(first.Wait());
  EXPECT_FALSE(second.IsReady());
  fresh.Reset();
  ASSERT_TRUE(second.Wait());
  EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkRetirementTransportTest, EmptySelectionDoesNotWaitForBeta) {
  net::ClientSocketHandle beta;
  Connect(beta, "beta.test");
  ASSERT_TRUE(beta.socket());
  auto* original_beta = beta.socket();
  base::test::TestFuture<void> drained;

  RetireAlpha(drained.GetCallback());

  EXPECT_FALSE(drained.IsReady());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(original_beta, beta.socket());
  EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkRetirementTransportTest, PreparationDefersNativeErrorUntilCommit) {
  net::ClientSocketHandle alpha;
  net::TestCompletionCallback error;
  Stall(alpha,error);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(error.have_result());
  base::test::TestFuture<void> drained;
  auto commit=PrepareAlpha(drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(error.have_result());
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED,error.WaitForResult());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha.is_initialized());
}

TEST_F(NetworkRetirementTransportTest, PreparedOriginalPreservesFreshSameKeyAndBeta) {
  net::ClientSocketHandle alpha,beta,fresh;
  Connect(beta,"beta.test");
  ASSERT_TRUE(beta.socket());
  auto* original_beta=beta.socket();
  net::TestCompletionCallback error;
  Stall(alpha,error);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(error.have_result());
  base::test::TestFuture<void> drained;
  auto commit=PrepareAlpha(drained.GetCallback());
  sockets_.set_default_client_socket_type(
      net::MockTransportClientSocketFactory::Type::kSynchronous);
  Connect(fresh,"alpha.test");
  ASSERT_TRUE(fresh.socket());
  auto* original_fresh=fresh.socket();
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED,error.WaitForResult());
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(original_fresh,fresh.socket());
  EXPECT_TRUE(fresh.socket()->IsConnected());
  EXPECT_EQ(original_beta,beta.socket());
  EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkRetirementTransportTest, AbandonedPreparedCommitRevokesOriginalRequest) {
  net::ClientSocketHandle alpha;
  net::TestCompletionCallback error;
  Stall(alpha,error);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(error.have_result());
  base::test::TestFuture<void> drained;
  {
    auto abandoned=PrepareAlpha(drained.GetCallback());
  }
  EXPECT_EQ(net::ERR_ABORTED,error.WaitForResult());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha.is_initialized());
}

TEST_F(NetworkRetirementTransportTest, OldCommitCannotReleaseReusedHandlePreparation) {
  net::ClientSocketHandle reused;
  net::TestCompletionCallback original_error,replacement_error;
  Stall(reused,original_error);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(original_error.have_result());
  base::test::TestFuture<void> first_done,second_done;
  auto first=PrepareAlpha(first_done.GetCallback());
  reused.Reset();
  Stall(reused,replacement_error);
  auto second=PrepareAlpha(second_done.GetCallback());
  std::move(first).Run();
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(first_done.Wait());
  EXPECT_FALSE(original_error.have_result());
  EXPECT_FALSE(replacement_error.have_result());
  EXPECT_FALSE(second_done.IsReady());
  std::move(second).Run();
  EXPECT_EQ(net::ERR_ABORTED,replacement_error.WaitForResult());
  ASSERT_TRUE(second_done.Wait());
  EXPECT_FALSE(reused.is_initialized());
}

TEST_F(NetworkRetirementTransportTest, OverlappingPreparationsKeepFreshCallbackHeld) {
  net::ClientSocketHandle original,fresh;
  net::TestCompletionCallback first_error,second_error;
  Stall(original,first_error);
  base::RunLoop().RunUntilIdle();
  base::test::TestFuture<void> first_done,second_done;
  auto first=PrepareAlpha(first_done.GetCallback());
  Stall(fresh,second_error);
  auto second=PrepareAlpha(second_done.GetCallback());
  std::move(first).Run();
  EXPECT_EQ(net::ERR_ABORTED,first_error.WaitForResult());
  ASSERT_TRUE(first_done.Wait());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(second_error.have_result());
  EXPECT_FALSE(second_done.IsReady());
  std::move(second).Run();
  EXPECT_EQ(net::ERR_ABORTED,second_error.WaitForResult());
  ASSERT_TRUE(second_done.Wait());
}

TEST_F(NetworkRetirementTransportTest, EmptyPreparationStaysClosedUntilCommit) {
  net::ClientSocketHandle beta;
  Connect(beta,"beta.test");
  ASSERT_TRUE(beta.socket());
  base::test::TestFuture<void> drained;
  auto commit=PrepareAlpha(drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());
  EXPECT_TRUE(beta.socket()->IsConnected());
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(beta.socket()->IsConnected());
}

TEST_F(NetworkRetirementTransportTest, PendingPreconnectWaitsForCommitAndPreservesBeta) {
  sockets_.set_default_client_socket_type(
      net::MockTransportClientSocketFactory::Type::kStalled);
  base::test::TestFuture<bool> alpha,beta;
  auto callback=[](base::test::TestFuture<bool>* result,bool success,
                   std::unique_ptr<net::ClientSocketHandle> handle) {
    EXPECT_FALSE(handle);result->SetValue(success);
  };
  ASSERT_EQ(net::ERR_IO_PENDING,pool_->RequestSockets(Group("alpha.test"),
      net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),std::nullopt,1,
      base::BindOnce(callback,&alpha),net::NetLogWithSource()));
  ASSERT_EQ(net::ERR_IO_PENDING,pool_->RequestSockets(Group("beta.test"),
      net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),std::nullopt,1,
      base::BindOnce(callback,&beta),net::NetLogWithSource()));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha.IsReady());
  EXPECT_FALSE(beta.IsReady());
  base::test::TestFuture<void> drained,beta_drained;
  auto commit=PrepareAlpha(drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha.IsReady());
  EXPECT_FALSE(drained.IsReady());
  EXPECT_FALSE(beta.IsReady());
  std::move(commit).Run();
  EXPECT_FALSE(alpha.Get());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.IsReady());
  auto close_beta=pool_->PrepareMatchingGroups(base::BindRepeating(
      [](const net::ClientSocketPool::GroupId& group) {
        return group.destination().host()=="beta.test";
      }),beta_drained.GetCallback());
  std::move(close_beta).Run();
  EXPECT_FALSE(beta.Get());
  ASSERT_TRUE(beta_drained.Wait());
}

TEST_F(NetworkRetirementTransportTest, PreconnectCallbackReturnOwnsNativeDrain) {
  sockets_.set_default_client_socket_type(
      net::MockTransportClientSocketFactory::Type::kPending);
  base::test::TestFuture<void> entered,drained;
  ASSERT_EQ(net::ERR_IO_PENDING,pool_->RequestSockets(Group("alpha.test"),
      net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),std::nullopt,1,
      base::BindOnce(&NetworkRetirementTransportTest::PreconnectCallbackScope,
                     base::Unretained(this),&entered,&drained),net::NetLogWithSource()));
  ASSERT_TRUE(entered.Wait());
  ASSERT_TRUE(drained.Wait());
}

TEST_F(NetworkRetirementTransportTest, EnteredFailedCallbackOutlivesReleasedHandle) {
  sockets_.set_default_client_socket_type(
      net::MockTransportClientSocketFactory::Type::kStalled);
  net::ClientSocketHandle alpha;
  base::test::TestFuture<void> entered,drained;
  ASSERT_EQ(net::ERR_IO_PENDING,alpha.Init(Group("alpha.test"),
      net::ClientSocketPool::SocketParams::CreateForHttpForTesting(),
      std::nullopt,net::LOW,net::SocketTag(),net::ClientSocketPool::RespectLimits::ENABLED,
      base::BindOnce(&NetworkRetirementTransportTest::FailedCallbackScope,
                     base::Unretained(this),&entered,&drained,&alpha),
      net::ClientSocketPool::ProxyAuthCallback(),pool_.get(),net::NetLogWithSource()));
  base::RunLoop().RunUntilIdle();
  pool_->FlushWithError(net::ERR_ABORTED,"native callback scope test");
  ASSERT_TRUE(entered.Wait());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(alpha.is_initialized());
}

}  // namespace
}  // namespace alloy::site_records
