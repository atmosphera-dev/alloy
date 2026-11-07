// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include <atomic>
#include <memory>
#include "base/functional/callback_helpers.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/test_timeouts.h"
#include "base/threading/thread_restrictions.h"
#include "base/json/json_writer.h"
#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "brave/browser/net/brave_ad_block_tp_network_delegate_helper.h"
#include "brave/browser/net/brave_ad_block_csp_network_delegate_helper.h"
#include "brave/browser/net/url_context.h"
#include "brave/browser/ui/webui/brave_adblock_ui.h"
#include "brave/browser/cosmetic_filters/profile_cosmetic_filters_resources.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/filter_list_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "brave/components/brave_shields/core/browser/ad_block_custom_resource_provider.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/test_web_ui.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/web_contents_tester.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace alloy::site_records {
namespace {
// Broker inputs use a real background FIFO. Own a held task's synchronization
// independently of the test stack, including assertion-failure cleanup.
struct EngineTaskGate {
  base::WaitableEvent entered,resume,finished;
  std::atomic<bool> ran{false};
  static void Run(std::shared_ptr<EngineTaskGate> gate,brave_shields::AdBlockEngineWrapper*) {
    base::ScopedAllowBaseSyncPrimitivesForTesting allow;
    gate->entered.Signal();gate->resume.Wait();gate->ran.store(true);gate->finished.Signal();
  }
};
class AdBlockConsumersTest:public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {ChromeRenderViewHostTestHarness::SetUp();features.InitAndDisableFeature(brave_shields::features::kBraveAdblockCnameUncloaking);RequestContextPolicy::AttachMemory(*profile());}
  brave_shields::AdBlockService* Prepare(content::BrowserContext& context) {
    auto* service=brave_shields::AdBlockServiceForContext(context);EXPECT_TRUE(service);if(!service)return nullptr;
    service->custom_resource_provider()->OverrideResourcesForTesting(adblock::new_empty_resource_storage());
    service->component_service_manager()->SetFilterListCatalog({});task_environment()->RunUntilIdle();return service;
  }
  std::shared_ptr<brave::BraveRequestInfo> Request(content::BrowserContext& context,const char* target,blink::mojom::ResourceType type) {
    auto info=std::make_shared<brave::BraveRequestInfo>();info->set_browser_context(&context);
    info->set_request_url(GURL(target));info->set_request_initiator(url::Origin::Create(GURL("https://page.invalid")));
    info->set_method("GET");info->set_resource_type(type);info->set_request_identifier(1);info->set_aggressive_blocking(true);return info;
  }
  void TearDown() override {task_environment()->RunUntilIdle();ChromeRenderViewHostTestHarness::TearDown();}
  base::test::ScopedFeatureList features;
};
TEST_F(AdBlockConsumersTest, NativeNetworkHelperConsumesOnlyOriginalProfilesEngine) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*peer);
  auto* alpha=Prepare(*profile());auto* beta=Prepare(*peer);ASSERT_TRUE(alpha&&beta);
  ASSERT_TRUE(alpha->custom_filters_provider()->UpdateCustomFilters("||network-private.invalid^$important"));task_environment()->RunUntilIdle();
  auto a=Request(*profile(),"https://network-private.invalid/image.png",blink::mojom::ResourceType::kImage);
  auto b=Request(*peer,"https://network-private.invalid/image.png",blink::mojom::ResourceType::kImage);bool a_done=false,b_done=false;
  EXPECT_EQ(net::ERR_IO_PENDING,brave::OnBeforeURLRequest_AdBlockTPPreWork(base::BindLambdaForTesting([&]{a_done=true;}),a));
  EXPECT_EQ(net::ERR_IO_PENDING,brave::OnBeforeURLRequest_AdBlockTPPreWork(base::BindLambdaForTesting([&]{b_done=true;}),b));
  task_environment()->RunUntilIdle();EXPECT_TRUE(a_done&&b_done);EXPECT_EQ(brave::kAdBlocked,a->blocked_by());EXPECT_EQ(brave::kNotBlocked,b->blocked_by());
}
TEST_F(AdBlockConsumersTest, LateNetworkReplyCancelsInsteadOfApplyingRetiredResult) {
  auto* service=Prepare(*profile());ASSERT_TRUE(service);
  ASSERT_TRUE(service->custom_filters_provider()->UpdateCustomFilters("||retired-private.invalid^$important"));task_environment()->RunUntilIdle();
  auto info=Request(*profile(),"https://retired-private.invalid/image.png",blink::mojom::ResourceType::kImage);bool done=false;
  EXPECT_EQ(net::ERR_IO_PENDING,brave::OnBeforeURLRequest_AdBlockTPPreWork(base::BindLambdaForTesting([&]{done=true;}),info));
  RequestContextPolicy::Get(*profile())->Revoke();task_environment()->RunUntilIdle();
  EXPECT_TRUE(done);ASSERT_TRUE(info->pending_error());EXPECT_EQ(net::ERR_ABORTED,*info->pending_error());EXPECT_EQ(brave::kNotBlocked,info->blocked_by());
}
TEST_F(AdBlockConsumersTest, NativeCspHelperKeepsPeerDirectivesAndMergesPrivateDirectives) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*peer);
  auto* alpha=Prepare(*profile());ASSERT_TRUE(alpha&&Prepare(*peer));
  ASSERT_TRUE(alpha->custom_filters_provider()->UpdateCustomFilters("||csp-private.invalid^$csp=script-src 'none'"));task_environment()->RunUntilIdle();
  std::string raw="HTTP/1.1 200 OK";raw.push_back('\0');raw+="Content-Security-Policy: default-src 'self'";raw.push_back('\0');raw.push_back('\0');
  auto headers=base::MakeRefCounted<net::HttpResponseHeaders>(raw);scoped_refptr<net::HttpResponseHeaders> a_headers,b_headers;
  auto a=Request(*profile(),"https://csp-private.invalid/page",blink::mojom::ResourceType::kMainFrame);
  auto b=Request(*peer,"https://csp-private.invalid/page",blink::mojom::ResourceType::kMainFrame);bool a_done=false,b_done=false;GURL redirect;
  EXPECT_EQ(net::ERR_IO_PENDING,brave::OnHeadersReceived_AdBlockCspWork(headers.get(),&a_headers,&redirect,base::BindLambdaForTesting([&]{a_done=true;}),a));
  EXPECT_EQ(net::ERR_IO_PENDING,brave::OnHeadersReceived_AdBlockCspWork(headers.get(),&b_headers,&redirect,base::BindLambdaForTesting([&]{b_done=true;}),b));
  task_environment()->RunUntilIdle();EXPECT_TRUE(a_done&&b_done);
  auto a_csp=a_headers->GetNormalizedHeader("Content-Security-Policy");auto b_csp=b_headers->GetNormalizedHeader("Content-Security-Policy");
  ASSERT_TRUE(a_csp&&b_csp);EXPECT_NE(std::string::npos,a_csp->find("script-src 'none'"));EXPECT_NE(std::string::npos,a_csp->find("default-src 'self'"));
  EXPECT_EQ("default-src 'self'",*b_csp);
}
TEST_F(AdBlockConsumersTest, DestroyedRequestContextAndHeldMojoConsumerCannotReacquireAuthority) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*peer);ASSERT_TRUE(Prepare(*peer));
  brave_shields::AdBlockServiceLease lease(*peer);brave_shields::FilterListService consumer(lease.Getter());
  mojo::Remote<brave_shields::mojom::FilterListAndroidHandler> remote(consumer.MakeRemote());
  auto info=Request(*peer,"https://unrelated.invalid/image.png",blink::mojom::ResourceType::kImage);peer.reset();task_environment()->RunUntilIdle();
  EXPECT_EQ(nullptr,info->browser_context());EXPECT_EQ(nullptr,lease.Get());bool done=false;
  EXPECT_EQ(net::ERR_ABORTED,brave::OnBeforeURLRequest_AdBlockTPPreWork(base::BindLambdaForTesting([&]{done=true;}),info));EXPECT_FALSE(done);
  bool disconnected=false;remote.set_disconnect_handler(base::BindLambdaForTesting([&]{disconnected=true;}));
  remote->GetCustomFilters(base::BindOnce([](const std::string&){ADD_FAILURE()<<"retired consumer returned data";}));
  task_environment()->RunUntilIdle();EXPECT_TRUE(disconnected);
}
TEST_F(AdBlockConsumersTest, MainAdBlockPageReadsOriginalContextInsteadOfProcessService) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*peer);
  auto* alpha=Prepare(*profile());ASSERT_TRUE(alpha&&Prepare(*peer));
  ASSERT_TRUE(alpha->custom_filters_provider()->UpdateCustomFilters("||main-page-private.invalid^"));
  auto peer_contents=content::WebContentsTester::CreateTestWebContents(peer.get(),nullptr);
  content::TestWebUI a,b;a.set_web_contents(web_contents());b.set_web_contents(peer_contents.get());
  BraveAdblockUI a_controller(&a),b_controller(&b);base::ListValue args;
  a.HandleReceivedMessage("brave_adblock.getCustomFilters",args);b.HandleReceivedMessage("brave_adblock.getCustomFilters",args);
  auto read=[](content::TestWebUI& ui)->std::string {
    for(const auto& call:ui.call_data())if(call->function_name()=="brave_adblock.onGetCustomFilters"&&call->arg1()&&call->arg1()->is_string())return call->arg1()->GetString();
    ADD_FAILURE()<<"main AdBlock page did not return its filter text";return "missing";
  };
  EXPECT_EQ("||main-page-private.invalid^",read(a));EXPECT_EQ("",read(b));
}
TEST_F(AdBlockConsumersTest, CosmeticMojoReadsOnlyItsOriginalProfilesNativeResourcesAndSelectors) {
  auto peer=TestingProfile::Builder().Build();RequestContextPolicy::AttachMemory(*peer);
  auto* alpha=Prepare(*profile());ASSERT_TRUE(alpha&&Prepare(*peer));
  ASSERT_TRUE(alpha->custom_filters_provider()->UpdateCustomFilters("cosmetics.invalid##.profile-banner\n##.generic-private"));
  task_environment()->RunUntilIdle();NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  auto peer_contents=content::WebContentsTester::CreateTestWebContents(peer.get(),nullptr);
  content::WebContentsTester::For(peer_contents.get())->NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> a,b;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),a.BindNewPipeAndPassReceiver());
  cosmetic_filters::BindProfileCosmeticFiltersResources(*peer_contents->GetPrimaryMainFrame(),b.BindNewPipeAndPassReceiver());
  base::test::TestFuture<base::Value> a_result,b_result;
  a->UrlCosmeticResources("https://cosmetics.invalid/page",true,a_result.GetCallback());
  b->UrlCosmeticResources("https://cosmetics.invalid/page",true,b_result.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(a_result.IsReady()&&b_result.IsReady());auto a_json=base::WriteJson(a_result.Get());auto b_json=base::WriteJson(b_result.Get());
  ASSERT_TRUE(a_json&&b_json);EXPECT_NE(std::string::npos,a_json->find(".profile-banner"));EXPECT_EQ(std::string::npos,b_json->find(".profile-banner"));
  base::test::TestFuture<base::DictValue> selectors;
  a->HiddenClassIdSelectors(R"({"classes":["generic-private"],"ids":[]})",{},selectors.GetCallback());task_environment()->RunUntilIdle();
  ASSERT_TRUE(selectors.IsReady());auto json=base::WriteJson(selectors.Get());ASSERT_TRUE(json);EXPECT_NE(std::string::npos,json->find(".generic-private"));
}
TEST_F(AdBlockConsumersTest, CosmeticMojoDisconnectsOnCrossDocumentNavigation) {
  ASSERT_TRUE(Prepare(*profile()));NavigateAndCommit(GURL("https://cosmetics.invalid/first"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> remote;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),remote.BindNewPipeAndPassReceiver());
  bool disconnected=false;remote.set_disconnect_handler(base::BindLambdaForTesting([&]{disconnected=true;}));
  NavigateAndCommit(GURL("https://cosmetics.invalid/second"));task_environment()->RunUntilIdle();EXPECT_TRUE(disconnected);
}
TEST_F(AdBlockConsumersTest, CosmeticMojoRefusesRetiredCanonicalDocumentInsteadOfReturningPrivateData) {
  ASSERT_TRUE(Prepare(*profile()));NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> remote;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),remote.BindNewPipeAndPassReceiver());
  bool disconnected=false;remote.set_disconnect_handler(base::BindLambdaForTesting([&]{disconnected=true;}));
  content::RetireAlloyDocumentState(main_rfh());
  remote->UrlCosmeticResources("https://cosmetics.invalid/page",true,base::BindOnce([](base::Value){ADD_FAILURE()<<"retired document received resources";}));
  task_environment()->RunUntilIdle();EXPECT_TRUE(disconnected);
}
TEST_F(AdBlockConsumersTest, ProfileCloseRetiresHeldCosmeticPipeBeforeEngineBackingAcknowledgement) {
  auto* service=Prepare(*profile());ASSERT_TRUE(service);NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> remote;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),remote.BindNewPipeAndPassReceiver());
  base::test::TestFuture<void> disconnected;remote.set_disconnect_handler(disconnected.GetCallback());
  auto gate=std::make_shared<EngineTaskGate>();
  base::ScopedClosureRunner release(base::BindOnce([](std::shared_ptr<EngineTaskGate> gate){gate->resume.Signal();},gate));
  ASSERT_TRUE(service->AsyncCall(base::BindOnce(&EngineTaskGate::Run,gate)));
  {base::ScopedAllowBaseSyncPrimitivesForTesting allow;ASSERT_TRUE(gate->entered.TimedWait(TestTimeouts::action_timeout()));}
  base::test::TestFuture<FileStatus> closed;bool closing_notice=false;
  auto notice=service->ObserveClose(base::BindLambdaForTesting([&]{closing_notice=true;EXPECT_FALSE(gate->ran.load());EXPECT_FALSE(closed.IsReady());}));
  auto input=ProfileFilterPreferences::Get(*profile());ASSERT_TRUE(input);input->Close(closed.GetCallback());
  EXPECT_TRUE(closing_notice);EXPECT_TRUE(service->IsClosed());EXPECT_FALSE(closed.IsReady());
  ASSERT_TRUE(disconnected.Wait());EXPECT_FALSE(gate->ran.load());EXPECT_FALSE(closed.IsReady());
  release.RunAndReset();task_environment()->RunUntilIdle();ASSERT_TRUE(closed.IsReady());
  EXPECT_EQ(FileStatus::kOk,closed.Get());EXPECT_TRUE(gate->ran.load()&&disconnected.IsReady());
}
TEST_F(AdBlockConsumersTest, CompletedEngineTaskStillRetiresCosmeticPipeAndAcknowledgesBackingClose) {
  auto* service=Prepare(*profile());ASSERT_TRUE(service);NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> remote;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),remote.BindNewPipeAndPassReceiver());
  base::test::TestFuture<void> disconnected;remote.set_disconnect_handler(disconnected.GetCallback());
  auto gate=std::make_shared<EngineTaskGate>();gate->resume.Signal();
  ASSERT_TRUE(service->AsyncCall(base::BindOnce(&EngineTaskGate::Run,gate)));
  {base::ScopedAllowBaseSyncPrimitivesForTesting allow;ASSERT_TRUE(gate->finished.TimedWait(TestTimeouts::action_timeout()));}
  ASSERT_TRUE(gate->ran.load());base::test::TestFuture<FileStatus> closed;
  auto input=ProfileFilterPreferences::Get(*profile());ASSERT_TRUE(input);input->Close(closed.GetCallback());
  EXPECT_TRUE(service->IsClosed());EXPECT_FALSE(closed.IsReady());task_environment()->RunUntilIdle();
  ASSERT_TRUE(closed.IsReady());EXPECT_EQ(FileStatus::kOk,closed.Get());EXPECT_TRUE(disconnected.IsReady());
}
TEST_F(AdBlockConsumersTest, SameDocumentNavigationKeepsTheOriginalCosmeticPipeAdmitted) {
  ASSERT_TRUE(Prepare(*profile()));NavigateAndCommit(GURL("https://cosmetics.invalid/page"));
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> remote;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),remote.BindNewPipeAndPassReceiver());
  bool disconnected=false;remote.set_disconnect_handler(base::BindLambdaForTesting([&]{disconnected=true;}));
  auto navigation=content::NavigationSimulator::CreateRendererInitiated(GURL("https://cosmetics.invalid/page#fragment"),main_rfh());
  navigation->CommitSameDocument();base::test::TestFuture<base::Value> result;
  remote->UrlCosmeticResources("https://cosmetics.invalid/page#fragment",true,result.GetCallback());task_environment()->RunUntilIdle();
  EXPECT_FALSE(disconnected);ASSERT_TRUE(result.IsReady());EXPECT_TRUE(result.Get().is_dict());
}
TEST_F(AdBlockConsumersTest, BackForwardCachedDocumentRestoresThroughFreshOriginalOwnerBinding) {
  auto* service=Prepare(*profile());ASSERT_TRUE(service);
  ASSERT_TRUE(service->custom_filters_provider()->UpdateCustomFilters("cosmetics.invalid##.restored-original"));task_environment()->RunUntilIdle();
  NavigateAndCommit(GURL("https://cosmetics.invalid/first"));auto original_id=main_rfh()->GetGlobalId();
  auto original_document=content::AlloyDocumentStateOwner(main_rfh());
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> old;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),old.BindNewPipeAndPassReceiver());
  bool disconnected=false;old.set_disconnect_handler(base::BindLambdaForTesting([&]{disconnected=true;}));
  NavigateAndCommit(GURL("https://cosmetics.invalid/second"));task_environment()->RunUntilIdle();
  EXPECT_TRUE(disconnected);ASSERT_TRUE(original_document);
  content::NavigationSimulator::GoBack(web_contents());task_environment()->RunUntilIdle();
  ASSERT_EQ(original_id,main_rfh()->GetGlobalId());
  mojo::Remote<cosmetic_filters::mojom::CosmeticFiltersResources> restored;
  cosmetic_filters::BindProfileCosmeticFiltersResources(*main_rfh(),restored.BindNewPipeAndPassReceiver());
  base::test::TestFuture<base::Value> result;restored->UrlCosmeticResources("https://cosmetics.invalid/first",true,result.GetCallback());
  task_environment()->RunUntilIdle();ASSERT_TRUE(result.IsReady());auto json=base::WriteJson(result.Get());ASSERT_TRUE(json);
  EXPECT_NE(std::string::npos,json->find(".restored-original"));
}
}  // namespace
}  // namespace alloy::site_records
