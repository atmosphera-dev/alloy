// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "net/base/features.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_network_session.h"
#include "net/http/http_response_info.h"
#include "net/http/http_stream_factory.h"
#include "net/http/http_stream_factory_job_controller.h"
#include "net/http/http_stream_factory_test_util.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/test/test_with_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
class FactoryRequestDelegate : public net::MockHttpStreamRequestDelegate {
 public:
  void OnStreamFailed(int status,
                      const net::NetErrorDetails& details,
                      const net::ProxyInfo& proxy,
                      net::ResolveErrorInfo resolution) override {
    net::MockHttpStreamRequestDelegate::OnStreamFailed(status, details, proxy,
                                                       resolution);
    if (failed)
      std::move(failed).Run();
  }
  void OnNeedsProxyAuth(const net::HttpResponseInfo&,
                        const net::ProxyInfo&,
                        net::HttpAuthController*) override {
    auth_seen = true;
  }
  base::OnceClosure failed;
  bool auth_seen = false;
};

class FactoryRetirementTest : public net::WithTaskEnvironment,
                              public testing::Test {
 public:
  FactoryRetirementTest() {
    features_.InitWithFeatures(
        {net::features::kPartitionConnectionsByNetworkIsolationKey},
        {net::features::kHappyEyeballsV3});
    dependencies_.host_resolver->rules()->AddRule("*", "127.0.0.1");
    dependencies_.host_resolver->set_ondemand_mode(true);
    session_ = net::SpdySessionDependencies::SpdyCreateSession(&dependencies_);
  }
  net::HttpStreamFactory& factory() { return *session_->http_stream_factory(); }
  net::HttpRequestInfo Info(const char* owner) {
    net::HttpRequestInfo info;
    info.url = GURL("http://transport.test/");
    info.method = "GET";
    net::SchemefulSite site(GURL(std::string("http://") + owner));
    info.network_isolation_key = net::NetworkIsolationKey(site, site);
    info.network_anonymization_key =
        net::NetworkAnonymizationKey::CreateFromNetworkIsolationKey(
            info.network_isolation_key);
    info.traffic_annotation =
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
    EXPECT_TRUE(info.IsConsistent());
    return info;
  }
  std::unique_ptr<net::HttpStreamRequest> Request(
      const char* owner,
      FactoryRequestDelegate* delegate) {
    return factory().RequestStream(Info(owner), net::MEDIUM, {}, delegate, true,
                                   false, net::NetLogWithSource());
  }
  void Preconnect(const char* owner,
                  base::OnceClosure done,
                  const char* destination = nullptr) {
    auto info = Info(owner);
    if (destination)
      info.url = GURL(destination);
    factory().PreconnectStreams(1, info, std::move(done));
  }
  void Retire(const char* owner, base::OnceClosure done) {
    factory().RetireNetworkState(
        base::BindRepeating(
            [](net::NetworkAnonymizationKey wanted,
               const net::NetworkAnonymizationKey& actual,
               const url::SchemeHostPort&) { return wanted == actual; },
            Info(owner).network_anonymization_key),
        std::move(done));
  }

  base::OnceClosure Prepare(const char* owner,base::OnceClosure done) {
    return factory().PrepareNetworkRetirement(base::BindRepeating(
        [](net::NetworkAnonymizationKey wanted,const net::NetworkAnonymizationKey& actual,
           const url::SchemeHostPort&) {return wanted==actual;},
        Info(owner).network_anonymization_key),std::move(done));
  }

 protected:
  base::test::ScopedFeatureList features_;
  net::SpdySessionDependencies dependencies_;
  std::unique_ptr<net::HttpNetworkSession> session_;
};

TEST_F(FactoryRetirementTest,
       OriginalHelperDrainsAndSameDestinationBetaSurvives) {
  FactoryRequestDelegate alpha, beta;
  auto original = Request("alpha.test", &alpha);
  auto unrelated = Request("beta.test", &beta);
  base::RunLoop().RunUntilIdle();
  ASSERT_FALSE(alpha.IsDone());
  ASSERT_FALSE(beta.IsDone());
  base::test::TestFuture<void> drained;
  Retire("alpha.test", drained.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, alpha.WaitForError());
  EXPECT_EQ(net::ERR_ABORTED, original->RestartTunnelWithProxyAuth());
  EXPECT_EQ(net::LOAD_STATE_IDLE, original->GetLoadState());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());
  EXPECT_FALSE(beta.IsDone());
  original.reset();
  ASSERT_TRUE(drained.Wait());
  base::test::TestFuture<void> cleanup;
  Retire("beta.test", cleanup.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, beta.WaitForError());
  unrelated.reset();
  ASSERT_TRUE(cleanup.Wait());
}

TEST_F(FactoryRetirementTest, NativePreconnectCallbackCreatesFreshAdmission) {
  base::test::TestFuture<void> old, fresh, beta, drained;
  Preconnect(
      "alpha.test",
      base::BindOnce(
          [](FactoryRetirementTest* self, base::test::TestFuture<void>* old,
             base::test::TestFuture<void>* fresh,
             base::test::TestFuture<void>* drained) {
            EXPECT_FALSE(drained->IsReady());
            self->Preconnect("alpha.test", fresh->GetCallback(),
                             "http://fresh-transport.test/");
            old->SetValue();
          },
          base::Unretained(this), base::Unretained(&old),
          base::Unretained(&fresh), base::Unretained(&drained)));
  Preconnect("beta.test", beta.GetCallback());
  base::RunLoop().RunUntilIdle();
  Retire("alpha.test", drained.GetCallback());
  ASSERT_TRUE(old.Wait());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(fresh.IsReady());
  EXPECT_FALSE(beta.IsReady());
  base::test::TestFuture<void> fresh_drain, beta_drain;
  Retire("alpha.test", fresh_drain.GetCallback());
  ASSERT_TRUE(fresh.Wait());
  ASSERT_TRUE(fresh_drain.Wait());
  EXPECT_FALSE(beta.IsReady());
  Retire("beta.test", beta_drain.GetCallback());
  ASSERT_TRUE(beta.Wait());
  ASSERT_TRUE(beta_drain.Wait());
}

TEST_F(FactoryRetirementTest,
       FailureCallbackDeletesSiblingAndCreatesFreshRequest) {
  FactoryRequestDelegate first_delegate, second_delegate, fresh_delegate;
  std::unique_ptr<net::HttpStreamRequest> first, second, fresh;
  int callbacks = 0;
  auto on_failure = [](FactoryRetirementTest* self,
                       std::unique_ptr<net::HttpStreamRequest>* sibling,
                       std::unique_ptr<net::HttpStreamRequest>* fresh,
                       FactoryRequestDelegate* delegate, int* count) {
    ++*count;
    sibling->reset();
    *fresh = self->Request("alpha.test", delegate);
  };
  first_delegate.failed = base::BindOnce(
      on_failure, base::Unretained(this), base::Unretained(&second),
      base::Unretained(&fresh), base::Unretained(&fresh_delegate),
      base::Unretained(&callbacks));
  second_delegate.failed = base::BindOnce(
      on_failure, base::Unretained(this), base::Unretained(&first),
      base::Unretained(&fresh), base::Unretained(&fresh_delegate),
      base::Unretained(&callbacks));
  first = Request("alpha.test", &first_delegate);
  second = Request("alpha.test", &second_delegate);
  base::RunLoop().RunUntilIdle();
  base::test::TestFuture<void> drained;
  Retire("alpha.test", drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, callbacks);
  ASSERT_TRUE(fresh);
  EXPECT_NE(first_delegate.IsDone(), second_delegate.IsDone());
  EXPECT_FALSE(fresh_delegate.IsDone());
  EXPECT_FALSE(drained.IsReady());
  first.reset();
  second.reset();
  ASSERT_TRUE(drained.Wait());
  base::test::TestFuture<void> cleanup;
  Retire("alpha.test", cleanup.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, fresh_delegate.WaitForError());
  fresh.reset();
  ASSERT_TRUE(cleanup.Wait());
}

TEST_F(FactoryRetirementTest,
       QueuedNativeReadyCannotOverrideRetirementFailure) {
  dependencies_.host_resolver->set_ondemand_mode(false);
  dependencies_.host_resolver->set_synchronous_mode(true);
  net::StaticSocketDataProvider data;
  data.set_connect_data(net::MockConnect(net::SYNCHRONOUS, net::OK));
  dependencies_.socket_factory->AddSocketDataProvider(&data);
  FactoryRequestDelegate alpha;
  auto request = Request("alpha.test", &alpha);
  ASSERT_FALSE(alpha.IsDone());
  base::test::TestFuture<void> drained;
  Retire("alpha.test", drained.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, alpha.WaitForError());
  EXPECT_FALSE(request->completed());
  EXPECT_FALSE(drained.IsReady());
  request.reset();
  ASSERT_TRUE(drained.Wait());
}

TEST_F(FactoryRetirementTest,
       NativeBoundAuthCannotRestartAndHelperDestructionIsSafe) {
  net::TestJobFactory jobs;
  FactoryRequestDelegate alpha;
  auto info = Info("alpha.test");
  auto owner = std::make_unique<net::HttpStreamFactory::JobController>(
      &factory(), &alpha, session_.get(), &jobs, info, false, false, true,
      false, false, std::vector<net::SSLConfig::CertAndStatus>());
  auto* controller = owner.get();
  net::HttpStreamFactoryPeer::AddJobController(&factory(), std::move(owner));
  auto request =
      controller->Start(&alpha, nullptr, net::NetLogWithSource(),
                        net::HttpStreamRequest::HTTP_STREAM, net::MEDIUM);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(jobs.main_job());
  controller->OnNeedsProxyAuth(jobs.main_job(), net::HttpResponseInfo(),
                               net::ProxyInfo::Direct(), nullptr);
  ASSERT_TRUE(alpha.auth_seen);
  base::test::TestFuture<void> drained;
  Retire("alpha.test", drained.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, request->RestartTunnelWithProxyAuth());
  EXPECT_EQ(net::ERR_ABORTED, alpha.WaitForError());
  EXPECT_FALSE(drained.IsReady());
  request.reset();
  ASSERT_TRUE(drained.Wait());
}
TEST_F(FactoryRetirementTest, NativeCallbackOutlivesSelfDeletedHelper) {
  FactoryRequestDelegate alpha;
  std::unique_ptr<net::HttpStreamRequest> original;
  base::test::TestFuture<void> drained, callback_finished;
  alpha.failed = base::BindOnce(
      [](std::unique_ptr<net::HttpStreamRequest>* request,
         base::test::TestFuture<void>* drained,
         base::test::TestFuture<void>* finished) {
        request->reset();
        base::test::TestFuture<void> progressed;
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, progressed.GetCallback());
        base::RunLoop(base::RunLoop::Type::kNestableTasksAllowed)
            .RunUntilIdle();
        EXPECT_TRUE(progressed.IsReady());
        EXPECT_FALSE(drained->IsReady());
        finished->SetValue();
      },
      base::Unretained(&original), base::Unretained(&drained),
      base::Unretained(&callback_finished));
  original = Request("alpha.test", &alpha);
  base::RunLoop().RunUntilIdle();
  Retire("alpha.test", drained.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED, alpha.WaitForError());
  ASSERT_TRUE(callback_finished.Wait());
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(original);
}
TEST_F(FactoryRetirementTest, PreparedFailureIsSilentAndFreshSameKeyHelperSurvives) {
  FactoryRequestDelegate alpha,beta,fresh_delegate;
  auto original=Request("alpha.test",&alpha);
  auto unrelated=Request("beta.test",&beta);
  base::RunLoop().RunUntilIdle();
  base::test::TestFuture<void> drained;
  auto commit=Prepare("alpha.test",drained.GetCallback());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(alpha.IsDone());
  EXPECT_FALSE(drained.IsReady());
  EXPECT_EQ(net::ERR_ABORTED,original->RestartTunnelWithProxyAuth());
  auto fresh=Request("alpha.test",&fresh_delegate);
  base::RunLoop().RunUntilIdle();
  std::move(commit).Run();
  EXPECT_EQ(net::ERR_ABORTED,alpha.WaitForError());
  EXPECT_FALSE(beta.IsDone());
  EXPECT_FALSE(fresh_delegate.IsDone());
  original.reset();
  ASSERT_TRUE(drained.Wait());
  base::test::TestFuture<void> fresh_done,beta_done;
  Retire("alpha.test",fresh_done.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED,fresh_delegate.WaitForError());
  fresh.reset();ASSERT_TRUE(fresh_done.Wait());
  Retire("beta.test",beta_done.GetCallback());
  EXPECT_EQ(net::ERR_ABORTED,beta.WaitForError());
  unrelated.reset();ASSERT_TRUE(beta_done.Wait());
}

TEST_F(FactoryRetirementTest, DroppedPreparedPreconnectNotifiesOnlyOnCommit) {
  base::test::TestFuture<void> alpha,beta,drained;
  Preconnect("alpha.test",alpha.GetCallback());
  Preconnect("beta.test",beta.GetCallback());
  base::RunLoop().RunUntilIdle();
  {
    auto commit=Prepare("alpha.test",drained.GetCallback());
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(alpha.IsReady());
    EXPECT_FALSE(beta.IsReady());
    EXPECT_FALSE(drained.IsReady());
  }
  ASSERT_TRUE(alpha.Wait());ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.IsReady());
  base::test::TestFuture<void> cleanup;
  Retire("beta.test",cleanup.GetCallback());
  ASSERT_TRUE(beta.Wait());ASSERT_TRUE(cleanup.Wait());
}
}  // namespace
}  // namespace alloy::site_records
