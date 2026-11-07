// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_subscription_source.h"

#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted_memory.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service_factory.h"
#include "content/public/browser/url_data_source.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
using Manager=brave_shields::AdBlockSubscriptionServiceManager;
using Provider=brave_shields::AdBlockSubscriptionFiltersProvider;
using Response=scoped_refptr<base::RefCountedMemory>;
struct Owner {
  explicit Owner(StorageMode mode):issuer(base::UnguessableToken::Create(),
      base::UnguessableToken::Create(),base::UnguessableToken::Create(),mode) {}
  RecordCapabilityIssuer issuer;
  std::shared_ptr<ProfileFilterStorage> storage;
  scoped_refptr<ProfileFilterPreferences> input;
  std::unique_ptr<PrefService> prefs;
  brave_shields::AdBlockFiltersProviderManager providers;
  std::string text;
  std::unique_ptr<Manager> manager;
};
class ProfileSubscriptionSourceTest:public ChromeRenderViewHostTestHarness {
 protected:
  std::unique_ptr<Owner> MakeOwner(std::string text,StorageMode mode=StorageMode::kMemory) {
    auto owner=std::make_unique<Owner>(mode);owner->text=std::move(text);
    EXPECT_EQ(FileStatus::kOk,ProfileFilterStorage::Create(nullptr,
        owner->issuer.Issue(StorePurpose::kProfileCustomFilters),&owner->storage));
    base::test::TestFuture<FileStatus,scoped_refptr<ProfileFilterPreferences>> loaded;
    ProfileFilterPreferences::Load(owner->storage,base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindRepeating([](FileStatus){ADD_FAILURE();}),loaded.GetCallback());
    task_environment()->RunUntilIdle();
    if(!loaded.IsReady()){ADD_FAILURE();return nullptr;}
    EXPECT_EQ(FileStatus::kOk,loaded.Get<0>());owner->input=loaded.Get<1>();
    auto registry=base::MakeRefCounted<PrefRegistrySimple>();
    brave_shields::RegisterPrefsForAdBlockService(registry.get());
    PrefServiceFactory factory;factory.set_user_prefs(owner->input);owner->prefs=factory.Create(registry);
    auto backend=owner->input->SubscriptionBackend(base::BindRepeating(
        [](Owner* owner,const GURL&,bool,Provider::SourceReply done)->base::OnceClosure {
          std::move(done).Run(Provider::SourceResult::kOk,DATFileDataBuffer(owner->text.begin(),owner->text.end()));
          return base::DoNothing();
        },base::Unretained(owner.get())));
    owner->manager=std::make_unique<Manager>(owner->prefs.get(),&owner->providers,std::move(backend),nullptr);
    return owner;
  }
  Response Read(scoped_refptr<ProfileFilterPreferences> original,const GURL& url) {
    base::test::TestFuture<Response> done;
    ProfileSubscriptionSourceHandler(original).Run(content::URLDataSource::URLToRequestPath(url),done.GetCallback());
    task_environment()->RunUntilIdle();
    if(!done.IsReady()){ADD_FAILURE();return nullptr;}
    return done.Get();
  }
  std::string Body(const Response& response) {
    return response ? std::string(response->begin(),response->end()) : std::string();
  }
};

TEST_F(ProfileSubscriptionSourceTest, NativeSourceResponseIsOpaqueEscapedAndOriginalProfileOnly) {
  auto alpha=MakeOwner("! <script>alpha&private</script>\n||alpha.invalid^\n");
  auto beta=MakeOwner("! beta private\n||beta.invalid^\n");ASSERT_TRUE(alpha&&beta);
  const GURL subscription("https://private-subscription.invalid/list?private=secret");
  ASSERT_TRUE(alpha->manager->CreateSubscription(subscription));
  ASSERT_TRUE(beta->manager->CreateSubscription(subscription));
  task_environment()->RunUntilIdle();
  auto a=alpha->manager->GetListTextFileUrl(subscription);
  auto b=beta->manager->GetListTextFileUrl(subscription);
  ASSERT_TRUE(a.is_valid()&&b.is_valid());EXPECT_NE(a,b);EXPECT_EQ("chrome",a.scheme());
  EXPECT_EQ("adblock",a.host());EXPECT_TRUE(IsProfileSubscriptionSourcePath(content::URLDataSource::URLToRequestPath(a)));
  EXPECT_EQ(std::string::npos,a.spec().find("private-subscription"));
  EXPECT_EQ(std::string::npos,a.spec().find("secret"));EXPECT_EQ(std::string::npos,a.spec().find("alpha"));
  auto response=Read(alpha->input,a);ASSERT_TRUE(response);
  EXPECT_EQ("<!doctype html><meta charset=utf-8><title>Filter list source</title><pre>! &lt;script&gt;alpha&amp;private&lt;/script&gt;\n||alpha.invalid^\n</pre>",Body(response));
  EXPECT_FALSE(Read(beta->input,a));EXPECT_FALSE(Read(alpha->input,b));
  EXPECT_NE(std::string::npos,Body(Read(beta->input,b)).find("beta private"));
}

TEST_F(ProfileSubscriptionSourceTest, CloseCancelsHeldNativeDataSourceReplyAndKeepsOtherProfileReadable) {
  auto alpha=MakeOwner("! alpha held\n");auto beta=MakeOwner("! beta live\n");ASSERT_TRUE(alpha&&beta);
  const GURL subscription("https://source.invalid/list");
  ASSERT_TRUE(alpha->manager->CreateSubscription(subscription));
  ASSERT_TRUE(beta->manager->CreateSubscription(subscription));task_environment()->RunUntilIdle();
  auto a=alpha->manager->GetListTextFileUrl(subscription);auto b=beta->manager->GetListTextFileUrl(subscription);
  base::test::TestFuture<Response> held;
  auto handler=ProfileSubscriptionSourceHandler(alpha->input);
  handler.Run(content::URLDataSource::URLToRequestPath(a),held.GetCallback());
  base::test::TestFuture<FileStatus> closed;alpha->input->Close(closed.GetCallback());
  task_environment()->RunUntilIdle();ASSERT_TRUE(closed.IsReady()&&held.IsReady());
  EXPECT_EQ(FileStatus::kOk,closed.Get());EXPECT_FALSE(held.Get());EXPECT_FALSE(Read(alpha->input,a));
  EXPECT_FALSE(alpha->manager->GetListTextFileUrl(subscription).is_valid());
  EXPECT_NE(std::string::npos,Body(Read(beta->input,b)).find("beta live"));
}

TEST_F(ProfileSubscriptionSourceTest, DeleteAndReaddCannotReuseOldSourceTokenForReplacementBytes) {
  auto owner=MakeOwner("! old bytes\n");ASSERT_TRUE(owner);
  const GURL subscription("https://source.invalid/list");
  ASSERT_TRUE(owner->manager->CreateSubscription(subscription));task_environment()->RunUntilIdle();
  auto old=owner->manager->GetListTextFileUrl(subscription);ASSERT_TRUE(Read(owner->input,old));
  ASSERT_TRUE(owner->manager->DeleteSubscription(subscription));
  owner->text="! replacement bytes\n";
  ASSERT_TRUE(owner->manager->CreateSubscription(subscription));task_environment()->RunUntilIdle();
  auto replacement=owner->manager->GetListTextFileUrl(subscription);EXPECT_NE(old,replacement);
  EXPECT_FALSE(Read(owner->input,old));
  EXPECT_NE(std::string::npos,Body(Read(owner->input,replacement)).find("replacement bytes"));
  base::test::TestFuture<Response> unknown;
  ProfileSubscriptionSourceHandler(owner->input).Run("subscription-source/unknown.html",unknown.GetCallback());
  ASSERT_TRUE(unknown.IsReady());EXPECT_FALSE(unknown.Get());
}

TEST_F(ProfileSubscriptionSourceTest, StatelessAndUnboundNativeDataSourceCannotReadPrivateSource) {
  auto owner=MakeOwner("! must not store\n",StorageMode::kStateless);ASSERT_TRUE(owner);
  const GURL subscription("https://source.invalid/list");EXPECT_FALSE(owner->manager->CreateSubscription(subscription));
  EXPECT_FALSE(owner->manager->GetListTextFileUrl(subscription).is_valid());
  base::test::TestFuture<Response> denied,unbound;
  ProfileSubscriptionSourceHandler(owner->input).Run("subscription-source/unknown.html",denied.GetCallback());
  ProfileSubscriptionSourceHandler(nullptr).Run("subscription-source/unknown.html",unbound.GetCallback());
  task_environment()->RunUntilIdle();ASSERT_TRUE(denied.IsReady()&&unbound.IsReady());
  EXPECT_FALSE(denied.Get());EXPECT_FALSE(unbound.Get());
}
}  // namespace
}  // namespace alloy::site_records
