// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/test_future.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "content/browser/alloy_network_generation.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/alloy_network_retirement.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/common/content_client.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_content_browser_client.h"
#include "content/public/test/unittest_test_suite.h"
#include "mojo/public/cpp/bindings/message.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "services/network/public/mojom/network_context.mojom-shared-message-ids.h"
#include "services/network/public/mojom/network_context.mojom-test-utils.h"
#include "services/network/test/test_network_context.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
class FanoutBrowserClient final : public content::TestContentBrowserClient {
 public:
  FanoutBrowserClient() { CHECK(logs_.CreateUniqueTempDir()); }
  bool ConfigureNetworkContextParams(content::BrowserContext*,bool,
      const base::FilePath&,network::mojom::NetworkContextParams* params,
      cert_verifier::mojom::CertVerifierCreationParams*) override {
    params->user_agent="alloy-fanout-test";params->accept_language="en";
    params->alloy_stateless=true;
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    params->alloy_guard_log=base::File(logs_.GetPath().AppendASCII(
        "guard-"+std::to_string(++next_log_)+".log"),
        base::File::FLAG_CREATE_ALWAYS|base::File::FLAG_APPEND);
#endif
    return true;
  }
 private:
  base::ScopedTempDir logs_;
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  int next_log_=0;
#endif
};

class HeldNetworkOperation final : public network::mojom::AlloyNetworkRetirement {
 public:
  explicit HeldNetworkOperation(mojo::PendingReceiver<network::mojom::AlloyNetworkRetirement> receiver)
      : receiver_(this,std::move(receiver)) {}
  void Commit(CommitCallback callback) override {++commits;reply_=std::move(callback);}
  void Finish() {if (reply_) std::move(reply_).Run(true);}
  void Drop() {receiver_.reset();}
  int commits=0;
 private:
  mojo::Receiver<network::mojom::AlloyNetworkRetirement> receiver_;
  CommitCallback reply_;
};

// Keep the dispatch identity through nested calls without maintaining a second
// method allowlist. These receipts exist only in this test executable.
struct FanoutDispatch {
  uint32_t ordinal;
  std::string method;
};
class FanoutDispatchReceipt final : public mojo::MessageFilter {
 public:
  FanoutDispatchReceipt(unsigned actor, std::vector<FanoutDispatch>& dispatches)
      : actor_(actor), dispatches_(dispatches) {}
  bool WillDispatch(mojo::Message* message) override {
    const char* method =
        network::mojom::NetworkContext::MessageToMethodName_(*message);
    dispatches_.push_back({message->name(),method ? method : "unknown"});
    Record("ALLOY_FANOUT_DISPATCH", actor_, dispatches_.back());
    return true;
  }
  void DidDispatchOrReject(mojo::Message* message, bool accepted) override {
    CHECK(!dispatches_.empty());
    CHECK_EQ(dispatches_.back().ordinal, message->name());
    if (!accepted) Record("ALLOY_FANOUT_REJECT", actor_, dispatches_.back());
    dispatches_.pop_back();
  }
  static void Record(const char* marker, unsigned actor, const FanoutDispatch& message) {
    std::fprintf(stderr,
        "%s {\"actor\":%u,\"ordinal\":%u,\"method\":\"%s\"}\n",
        marker, actor, static_cast<unsigned>(message.ordinal), message.method.c_str());
    std::fflush(stderr);
  }
 private:
  const unsigned actor_;
  std::vector<FanoutDispatch>& dispatches_;
};

// The generated surface forwards all ordinary IPC to a real native context.
// ONLY retirement is controlled here; no transport outcome is claimed by this
// browser-discovery/barrier fixture.
class FanoutNetworkActor final : public network::mojom::NetworkContextInterceptorForTesting {
 public:
  FanoutNetworkActor() : actor_id_(++next_actor_id_) {
    auto params = network::mojom::NetworkContextParams::New();
    auto verifier = cert_verifier::mojom::CertVerifierCreationParams::New();
    CHECK(content::GetContentClientForTesting()->browser()->ConfigureNetworkContextParams(
        nullptr, true, base::FilePath(), params.get(), verifier.get()));
    params->cert_verifier_params =
        content::GetCertVerifierParams(std::move(verifier));
    content::CreateNetworkContextInNetworkService(
        native_.BindNewPipeAndPassReceiver(), std::move(params));
    native_.set_disconnect_handler(base::BindOnce([] {
      CHECK(false) << "Unexpected native fanout forwarding backend disconnect";
    }));
  }
  mojo::PendingRemote<network::mojom::NetworkContext> Bind() {
    mojo::PendingRemote<network::mojom::NetworkContext> remote;
    receiver_.Bind(remote.InitWithNewPipeAndPassReceiver());
    receiver_.set_disconnect_handler(base::BindOnce(
        [](FanoutNetworkActor* actor) { actor->network_disconnected = true; },
        base::Unretained(this)));
    receiver_.SetFilter(std::make_unique<FanoutDispatchReceipt>(actor_id_,dispatches_));
    return remote;
  }
  network::mojom::NetworkContext* GetForwardingInterface() override {
    CHECK(native_.is_bound());
    CHECK(native_.is_connected());
    CHECK(!dispatches_.empty());
    const auto& message = dispatches_.back();
    CHECK_NE(message.ordinal, static_cast<uint32_t>(network::mojom::messages::
        NetworkContext::kPrepareAlloyNetworkServiceRetirement));
    FanoutDispatchReceipt::Record("ALLOY_FANOUT_FORWARD",actor_id_,message);
    return native_.get();
  }
  void PrepareAlloyNetworkServiceRetirement(const std::string& domain,
      mojo::PendingReceiver<network::mojom::AlloyNetworkRetirement> receiver,
      PrepareAlloyNetworkServiceRetirementCallback callback) override {
    EXPECT_EQ("alpha.test",domain);++prepares;
    operations.push_back(std::make_unique<HeldNetworkOperation>(std::move(receiver)));
    ready_.push_back(std::move(callback));
  }
  void Ready() {
    auto original=std::move(ready_);ready_.clear();
    for (auto& callback:original) std::move(callback).Run(true);
  }
  void Finish() {for (auto& operation:operations) operation->Finish();}
  int Commits() const {int count=0;for(const auto& op:operations)count+=op->commits;return count;}
  void Drop() {receiver_.reset();for(auto& op:operations)op->Drop();}
  int prepares=0;
  bool network_disconnected=false;
  std::vector<std::unique_ptr<HeldNetworkOperation>> operations;
 private:
  inline static unsigned next_actor_id_=0;
  const unsigned actor_id_;
  mojo::Remote<network::mojom::NetworkContext> native_;
  std::vector<FanoutDispatch> dispatches_;
  mojo::Receiver<network::mojom::NetworkContext> receiver_{this};
  std::vector<PrepareAlloyNetworkServiceRetirementCallback> ready_;
};

class BrowserNetworkFanoutTest : public testing::Test {
 protected:
  BrowserNetworkFanoutTest() {
    context_=std::make_unique<content::TestBrowserContext>();
    context_->set_is_off_the_record(true);
    RequestContextPolicy::AttachStateless(*context_,NetworkState::kCut);
  }
  content::StoragePartitionImpl& Partition() {
    return *static_cast<content::StoragePartitionImpl*>(context_->GetDefaultStoragePartition());
  }
  void Shared(FanoutNetworkActor& actor) {
    // Establish the canonical owner/verifier through production initialization
    // before using its existing test endpoint override.
    ASSERT_TRUE(Partition().GetNetworkContext());
    Partition().SetNetworkContextForTesting(actor.Bind());
  }
  scoped_refptr<content::AlloyNetworkGeneration> Cut(FanoutNetworkActor& actor) {
    auto generation=Partition().CreateAlloyNetworkGeneration();
    mojo::PendingRemote<cert_verifier::mojom::CertVerifierServiceUpdater> verifier;
    auto disconnected_verifier=verifier.InitWithNewPipeAndPassReceiver();
    generation->Bind(actor.Bind(),std::move(verifier),base::DoNothing(),base::DoNothing());
    return generation;
  }
  void Prepare(base::test::TestFuture<base::OnceClosure>& prepared,
                base::test::TestFuture<bool>& drained) {
    auto target=net::AlloyNetworkRecordTarget::FromCanonicalDomain("alpha.test");
    ASSERT_TRUE(target);
    content::PrepareAlloyNetworkServiceRetirement(context_.get(),*target,
        prepared.GetCallback(),drained.GetCallback());
  }
  content::BrowserTaskEnvironment tasks_;
  std::unique_ptr<content::TestBrowserContext> context_;
};

TEST_F(BrowserNetworkFanoutTest, SharedAndCutPrepareBeforeReadyAndAllRepliesDrain) {
  FanoutNetworkActor shared,first,second,retired;
  Shared(shared);auto a=Cut(first),b=Cut(second),closed=Cut(retired);closed->Shutdown();
  base::test::TestFuture<base::OnceClosure> prepared; base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();
  EXPECT_EQ(1,shared.prepares);EXPECT_EQ(1,first.prepares);EXPECT_EQ(1,second.prepares);
  EXPECT_EQ(0,retired.prepares);EXPECT_FALSE(prepared.IsReady());
  shared.Ready();first.Ready();tasks_.RunUntilIdle();EXPECT_FALSE(prepared.IsReady());
  second.Ready();ASSERT_TRUE(prepared.Wait());
  auto commit=prepared.Take();std::move(commit).Run();tasks_.RunUntilIdle();
  EXPECT_EQ(1,shared.Commits());EXPECT_EQ(1,first.Commits());EXPECT_EQ(1,second.Commits());
  EXPECT_FALSE(drained.IsReady());shared.Finish();first.Finish();tasks_.RunUntilIdle();
  EXPECT_FALSE(drained.IsReady());second.Finish();EXPECT_TRUE(drained.Get());
}

TEST_F(BrowserNetworkFanoutTest, FreshCutLeaseIsExcludedUntilNextSnapshot) {
  FanoutNetworkActor shared,old,fresh;
  Shared(shared);auto a=Cut(old);
  base::test::TestFuture<base::OnceClosure> prepared; base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();shared.Ready();old.Ready();ASSERT_TRUE(prepared.Wait());
  auto b=Cut(fresh);auto commit=prepared.Take();std::move(commit).Run();tasks_.RunUntilIdle();
  EXPECT_EQ(0,fresh.prepares);shared.Finish();old.Finish();EXPECT_TRUE(drained.Get());
  base::test::TestFuture<base::OnceClosure> again; base::test::TestFuture<bool> done;
  Prepare(again,done);tasks_.RunUntilIdle();EXPECT_EQ(1,fresh.prepares);
  shared.Ready();old.Ready();fresh.Ready();auto next=again.Take();std::move(next).Run();
  tasks_.RunUntilIdle();shared.Finish();old.Finish();fresh.Finish();EXPECT_TRUE(done.Get());
}

TEST_F(BrowserNetworkFanoutTest, DroppedPreparedActionCommitsOriginalEndpoints) {
  FanoutNetworkActor shared,cut;
  Shared(shared);auto lease=Cut(cut);
  base::test::TestFuture<base::OnceClosure> prepared; base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();shared.Ready();cut.Ready();
  {auto abandoned=prepared.Take();}
  tasks_.RunUntilIdle();EXPECT_EQ(1,shared.Commits());EXPECT_EQ(1,cut.Commits());
  EXPECT_FALSE(drained.IsReady());shared.Finish();cut.Finish();EXPECT_TRUE(drained.Get());
}

TEST_F(BrowserNetworkFanoutTest, LostEndpointReportsFailureWithoutHangingBarrier) {
  FanoutNetworkActor shared,cut;
  Shared(shared);auto lease=Cut(cut);
  base::test::TestFuture<base::OnceClosure> prepared; base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();shared.Ready();cut.Drop();
  auto commit=prepared.Take();std::move(commit).Run();tasks_.RunUntilIdle();
  shared.Finish();EXPECT_FALSE(drained.Get());
}

TEST_F(BrowserNetworkFanoutTest, EmptyLoadedPartitionSnapshotDoesNotCreateBackend) {
  int before=0;context_->ForEachLoadedStoragePartition([&](content::StoragePartition*){++before;});
  ASSERT_EQ(0,before);
  base::test::TestFuture<base::OnceClosure> prepared; base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);auto commit=prepared.Take();std::move(commit).Run();EXPECT_TRUE(drained.Get());
  int after=0;context_->ForEachLoadedStoragePartition([&](content::StoragePartition*){++after;});
  EXPECT_EQ(0,after);
}

TEST_F(BrowserNetworkFanoutTest, UnsupportedProviderCannotReportSuccessfulBrowserDrain) {
  FanoutNetworkActor initial;
  Shared(initial);
  network::TestNetworkContext unsupported;
  mojo::PendingRemote<network::mojom::NetworkContext> remote;
  mojo::Receiver<network::mojom::NetworkContext> receiver(
      &unsupported,remote.InitWithNewPipeAndPassReceiver());
  Partition().SetNetworkContextForTesting(std::move(remote));
  base::test::TestFuture<base::OnceClosure> prepared;
  base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);
  auto commit=prepared.Take();std::move(commit).Run();
  EXPECT_FALSE(drained.Get());
}

TEST_F(BrowserNetworkFanoutTest, ShutdownClosesAdmissionButRetainsOriginalEndpointUntilDrain) {
  FanoutNetworkActor shared,cut;
  Shared(shared);auto lease=Cut(cut);
  base::test::TestFuture<base::OnceClosure> prepared;
  base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();
  ASSERT_EQ(1,cut.prepares);
  lease->Shutdown();tasks_.RunUntilIdle();
  EXPECT_FALSE(lease->GetWeakPtr());
  EXPECT_FALSE(cut.network_disconnected);
  mojo::Remote<network::mojom::AlloyNetworkRetirement> late;
  base::test::TestFuture<bool> refused;
  lease->GetNetworkContext()->PrepareAlloyNetworkServiceRetirement(
      "alpha.test",late.BindNewPipeAndPassReceiver(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(refused.GetCallback(),false));
  EXPECT_FALSE(refused.Get());EXPECT_EQ(1,cut.prepares);
  shared.Ready();cut.Ready();auto commit=prepared.Take();std::move(commit).Run();
  tasks_.RunUntilIdle();EXPECT_EQ(1,cut.Commits());EXPECT_FALSE(drained.IsReady());
  shared.Finish();tasks_.RunUntilIdle();EXPECT_FALSE(drained.IsReady());
  EXPECT_FALSE(cut.network_disconnected);
  cut.Finish();EXPECT_TRUE(drained.Get());tasks_.RunUntilIdle();
  EXPECT_TRUE(cut.network_disconnected);
}

TEST_F(BrowserNetworkFanoutTest, RebindCannotReplaceOriginalRetirementEndpoint) {
  FanoutNetworkActor shared,original,fresh;
  Shared(shared);auto lease=Cut(original);
  base::test::TestFuture<base::OnceClosure> prepared;
  base::test::TestFuture<bool> drained;
  Prepare(prepared,drained);tasks_.RunUntilIdle();
  mojo::PendingRemote<cert_verifier::mojom::CertVerifierServiceUpdater> verifier;
  auto disconnected_verifier=verifier.InitWithNewPipeAndPassReceiver();
  lease->Bind(fresh.Bind(),std::move(verifier),base::DoNothing(),base::DoNothing());
  tasks_.RunUntilIdle();EXPECT_FALSE(original.network_disconnected);
  shared.Ready();original.Ready();auto commit=prepared.Take();std::move(commit).Run();
  tasks_.RunUntilIdle();EXPECT_EQ(1,original.Commits());
  EXPECT_EQ(0,fresh.prepares);EXPECT_EQ(0,fresh.Commits());
  shared.Finish();original.Finish();EXPECT_TRUE(drained.Get());tasks_.RunUntilIdle();
  EXPECT_TRUE(original.network_disconnected);EXPECT_FALSE(fresh.network_disconnected);
  base::test::TestFuture<base::OnceClosure> again;
  base::test::TestFuture<bool> done;
  Prepare(again,done);tasks_.RunUntilIdle();EXPECT_EQ(1,fresh.prepares);
  shared.Ready();fresh.Ready();auto next=again.Take();std::move(next).Run();
  tasks_.RunUntilIdle();shared.Finish();fresh.Finish();EXPECT_TRUE(done.Get());
}
}  // namespace

std::unique_ptr<content::UnitTestTestSuite::ContentClients> CreateFanoutClients() {
  auto clients=content::UnitTestTestSuite::CreateTestContentClients();
  clients->content_browser_client=std::make_unique<FanoutBrowserClient>();return clients;
}
}  // namespace alloy::site_records
