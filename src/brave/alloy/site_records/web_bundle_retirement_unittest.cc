// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/web_bundle_retirement_test_context.h"

namespace alloy::site_records {
namespace {
TEST_F(WebBundleRetirementTest,
       FactoryDestructionStopsOriginalActiveBodyProducer) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  ASSERT_TRUE(factory);
  std::string payload(1024 * 1024, 'a');
  Fill(*factory, payload);
  network::TestURLLoaderClient client;
  auto loader = Request(token, client);
  client.RunUntilResponseReceived();
  ASSERT_FALSE(client.has_received_completion());
  handle.Close();
  tasks_.RunUntilIdle();
  ASSERT_FALSE(factory);
  std::string received = Read(client.response_body_release());
  EXPECT_LT(received.size(), payload.size());
}

TEST_F(WebBundleRetirementTest, SmallBodyControlActuallyReachesConsumer) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  ASSERT_TRUE(factory);
  Fill(*factory, "native-bundle-body");
  network::TestURLLoaderClient client;
  auto loader = Request(token, client);
  client.RunUntilComplete();
  EXPECT_EQ(net::OK, client.completion_status().error_code);
  std::string received;
  ASSERT_TRUE(
      mojo::BlockingCopyToString(client.response_body_release(), &received));
  EXPECT_EQ("native-bundle-body", received);
}

TEST_F(WebBundleRetirementTest,
       PendingMetadataControlFailsOnFactoryDestruction) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  ASSERT_TRUE(factory);
  network::TestURLLoaderClient client;
  auto loader = Request(token, client);
  tasks_.RunUntilIdle();
  ASSERT_FALSE(client.has_received_completion());
  handle.Close();
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_FAILED, client.completion_status().error_code);
}

TEST_F(WebBundleRetirementTest,
       SelectiveActiveBodyStopsWhileSharedFactoryBetaReachesConsumer) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token, "bundle.test");
  ASSERT_TRUE(factory);
  std::string payload(1024 * 1024, 's');
  Fill(*factory, payload, "text/css");
  network::TestURLLoaderClient alpha, beta;
  auto a = Request(token, alpha);
  auto b = Request(token, beta, "beta.test");
  alpha.RunUntilResponseReceived();
  beta.RunUntilResponseReceived();
  ASSERT_FALSE(alpha.has_received_completion());
  ASSERT_FALSE(beta.has_received_completion());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  std::move(commit).Run();
  alpha.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  ASSERT_TRUE(drained.Wait());
  EXPECT_TRUE(factory);
  EXPECT_LT(Read(alpha.response_body_release()).size(), payload.size());
  EXPECT_EQ(payload, Read(beta.response_body_release()));
  beta.RunUntilComplete();
  EXPECT_EQ(net::OK, beta.completion_status().error_code);
}

TEST_F(WebBundleRetirementTest,
       DrainWaitsForOriginalWriterCancellationOnNativeSequence) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  ASSERT_TRUE(factory);
  std::string payload(1024 * 1024, 'h');
  Fill(*factory, payload);
  network::TestURLLoaderClient alpha;
  auto a = Request(token, alpha);
  alpha.RunUntilResponseReceived();
  tasks_.RunUntilIdle();
  ASSERT_FALSE(alpha.has_received_completion());
  base::test::TestFuture<void> drained;
  {
    // Pause the actual DataPipeProducer sequence, not an artificial completion
    // callback. Main-thread Mojo messages and nested loops can still run.
    base::test::TaskEnvironment::ParallelExecutionFence hold_native_writer;
    auto commit = Prepare("alpha.test", drained.GetCallback());
    std::move(commit).Run();
    alpha.RunUntilComplete();
    EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
    EXPECT_FALSE(factory);
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(drained.IsReady());
  }
  ASSERT_TRUE(drained.Wait());
  EXPECT_LT(Read(alpha.response_body_release()).size(), payload.size());
}

TEST_F(WebBundleRetirementTest,
       AwaitingFactoryRetiresOnlyOriginalAlphaAndFreshSameKeyLoads) {
  auto token = base::UnguessableToken::Create();
  network::TestURLLoaderClient alpha, beta, fresh;
  auto a = Request(token, alpha);
  auto b = Request(token, beta, "beta.test");
  tasks_.RunUntilIdle();
  ASSERT_FALSE(alpha.has_received_completion());
  ASSERT_FALSE(beta.has_received_completion());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  auto f = Request(token, fresh);
  std::move(commit).Run();
  alpha.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(beta.has_received_completion());
  EXPECT_FALSE(fresh.has_received_completion());
  BundleHandle handle;
  auto factory = Create(handle, token, "bundle.test");
  ASSERT_TRUE(factory);
  Fill(*factory, "new-owner-body", "text/css");
  beta.RunUntilComplete();
  fresh.RunUntilComplete();
  EXPECT_EQ(net::OK, beta.completion_status().error_code);
  EXPECT_EQ(net::OK, fresh.completion_status().error_code);
  EXPECT_EQ("new-owner-body", Read(beta.response_body_release()));
  EXPECT_EQ("new-owner-body", Read(fresh.response_body_release()));
}

TEST_F(WebBundleRetirementTest,
       LateTrustedHeaderReplyCannotPublishRetiredResponse) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token, "bundle.test");
  ASSERT_TRUE(factory);
  Fill(*factory, "must-not-publish");
  HeldBundleHeaders headers;
  network::TestURLLoaderClient alpha;
  auto a = Request(token, alpha, "alpha.test", headers.Bind());
  ASSERT_TRUE(headers.entered.Wait());
  ASSERT_FALSE(alpha.has_received_response());
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  alpha.RunUntilComplete();
  ASSERT_TRUE(drained.Wait());
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  std::move(headers.reply).Run(net::OK, std::nullopt, std::nullopt);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(alpha.has_received_response());
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
  EXPECT_TRUE(factory);
}

TEST_F(WebBundleRetirementTest,
       OverlappingSnapshotsAndOldDisconnectPreserveFreshSameKeyFactory) {
  BundleHandle old_handle, second_handle, fresh_handle;
  auto token = base::UnguessableToken::Create();
  auto old = Create(old_handle, token);
  network::TestURLLoaderClient original, second_client, fresh_client;
  auto a = Request(token, original);
  tasks_.RunUntilIdle();
  base::test::TestFuture<void> first_drained, second_drained;
  auto first = Prepare("alpha.test", first_drained.GetCallback());
  EXPECT_FALSE(old);
  auto second = Create(second_handle, token);
  ASSERT_TRUE(second);
  auto b = Request(token, second_client);
  tasks_.RunUntilIdle();
  auto later = Prepare("alpha.test", second_drained.GetCallback());
  EXPECT_FALSE(second);
  auto fresh = Create(fresh_handle, token);
  ASSERT_TRUE(fresh);
  auto f = Request(token, fresh_client);
  old_handle.Close();
  second_handle.Close();
  tasks_.RunUntilIdle();
  EXPECT_TRUE(fresh);
  EXPECT_FALSE(first_drained.IsReady());
  EXPECT_FALSE(second_drained.IsReady());
  std::move(first).Run();
  original.RunUntilComplete();
  ASSERT_TRUE(first_drained.Wait());
  EXPECT_FALSE(second_client.has_received_completion());
  std::move(later).Run();
  second_client.RunUntilComplete();
  ASSERT_TRUE(second_drained.Wait());
  EXPECT_EQ(net::ERR_ABORTED, original.completion_status().error_code);
  EXPECT_EQ(net::ERR_ABORTED, second_client.completion_status().error_code);
  ASSERT_TRUE(fresh);
  Fill(*fresh, "after-two-snapshots");
  fresh_client.RunUntilComplete();
  EXPECT_EQ(net::OK, fresh_client.completion_status().error_code);
  EXPECT_EQ("after-two-snapshots", Read(fresh_client.response_body_release()));
}

TEST_F(WebBundleRetirementTest, RetiredFetchWrapperClosesBeforeLateSuccess) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  ASSERT_TRUE(factory);
  network::TestURLLoaderClient fetch;
  mojo::Remote<network::mojom::URLLoaderClient> wrapper(
      factory->MaybeWrapURLLoaderClient(fetch.CreateRemote()));
  base::test::TestFuture<void> drained, wrapper_closed;
  wrapper.set_disconnect_handler(wrapper_closed.GetCallback());
  auto commit = Prepare("alpha.test", drained.GetCallback());
  EXPECT_FALSE(drained.IsReady());
  wrapper->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  std::move(commit).Run();
  fetch.RunUntilComplete();
  ASSERT_TRUE(drained.Wait());
  ASSERT_TRUE(wrapper_closed.Wait());
  EXPECT_EQ(net::ERR_ABORTED, fetch.completion_status().error_code);
  EXPECT_FALSE(fetch.has_received_response());
}

TEST_F(WebBundleRetirementTest, EmptySelectionPreservesPendingBeta) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token, "beta.test");
  network::TestURLLoaderClient beta;
  auto b = Request(token, beta, "beta.test");
  tasks_.RunUntilIdle();
  base::test::TestFuture<void> drained;
  auto commit = Prepare("alpha.test", drained.GetCallback());
  std::move(commit).Run();
  ASSERT_TRUE(drained.Wait());
  ASSERT_TRUE(factory);
  EXPECT_FALSE(beta.has_received_completion());
  Fill(*factory, "beta-after-empty", "text/css");
  beta.RunUntilComplete();
  EXPECT_EQ(net::OK, beta.completion_status().error_code);
  EXPECT_EQ("beta-after-empty", Read(beta.response_body_release()));
}

TEST_F(WebBundleRetirementTest, AbandonedPreparationRevokesOriginalEndpoints) {
  BundleHandle handle;
  auto token = base::UnguessableToken::Create();
  auto factory = Create(handle, token);
  network::TestURLLoaderClient alpha;
  auto a = Request(token, alpha);
  tasks_.RunUntilIdle();
  base::test::TestFuture<void> drained;
  {
    auto abandoned = Prepare("alpha.test", drained.GetCallback());
    EXPECT_FALSE(drained.IsReady());
  }
  alpha.RunUntilComplete();
  ASSERT_TRUE(drained.Wait());
  EXPECT_FALSE(factory);
  EXPECT_EQ(net::ERR_ABORTED, alpha.completion_status().error_code);
}
}  // namespace
}  // namespace alloy::site_records
