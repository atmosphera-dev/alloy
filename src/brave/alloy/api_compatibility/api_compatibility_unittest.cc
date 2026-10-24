// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "build/build_config.h"
#include "cef/libcef/features/features.h"
#include "chrome/browser/renderer_context_menu/render_view_context_menu_test_util.h"
#include "chrome/browser/ui/browser_window/test/mock_browser_window_interface.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/viz/test/fake_display_client.h"
#include "components/viz/test/mock_display_client.h"
#include "content/public/browser/context_menu_params.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/cookie_store_test_helpers.h"
#include "services/viz/privileged/mojom/compositing/layered_window_updater.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_LINUX) && BUILDFLAG(ENABLE_CEF)
#include "chrome/browser/policy/browser_dm_token_storage_linux.h"
#include "chrome/browser/policy/chrome_browser_policy_connector.h"
#endif

namespace {

class ApiCompatibilityTest : public ChromeRenderViewHostTestHarness {};

TEST_F(ApiCompatibilityTest, NonShowingMenuFixtureNeverReportsNativePopup) {
  TestRenderViewContextMenu menu(*main_rfh(), content::ContextMenuParams());
  RenderViewContextMenuBase& consumer = menu;
  EXPECT_FALSE(consumer.IsRunning());
  consumer.Show();
  EXPECT_FALSE(consumer.IsRunning());
}

TEST(ApiCompatibilityMockTest, NormalBrowserQueryTracksTypeAndCanBeOverridden) {
  testing::NiceMock<MockBrowserWindowInterface> window;
  const BrowserWindowInterface& consumer = window;
  ON_CALL(window, GetType())
      .WillByDefault(testing::Return(BrowserWindowInterface::Type::TYPE_NORMAL));
  EXPECT_TRUE(consumer.IsNormalBrowser());
  ON_CALL(window, GetType())
      .WillByDefault(testing::Return(BrowserWindowInterface::Type::TYPE_POPUP));
  EXPECT_FALSE(consumer.IsNormalBrowser());
  EXPECT_CALL(window, IsNormalBrowser()).WillOnce(testing::Return(true));
  EXPECT_TRUE(consumer.IsNormalBrowser());
}

TEST(ApiCompatibilityCookieTest, DelayedStorePreservesNativeSchemeFirstUseGate) {
  base::test::TaskEnvironment tasks;
  net::DelayedCookieMonster store;
  net::CookieStore& consumer = store;
  base::test::TestFuture<bool> added;
  consumer.AddCookieableSchemes({"alloy-fixture"}, added.GetCallback());
  ASSERT_TRUE(added.Get());
  base::test::TestFuture<bool> removed;
  consumer.RemoveCookieableSchemes({"alloy-fixture"}, removed.GetCallback());
  ASSERT_TRUE(removed.Get());
  base::test::TestFuture<net::CookieList> used;
  consumer.GetAllCookiesAsync(used.GetCallback<const net::CookieList&>());
  ASSERT_TRUE(used.Wait());
  base::test::TestFuture<bool> late_add;
  consumer.AddCookieableSchemes({"late-fixture"}, late_add.GetCallback());
  EXPECT_FALSE(late_add.Get());
  base::test::TestFuture<bool> late_remove;
  consumer.RemoveCookieableSchemes({"http"}, late_remove.GetCallback());
  EXPECT_FALSE(late_remove.Get());
}

TEST(ApiCompatibilityDisplayTest, UpdaterDispatchWorksOnNonWindowsPlatforms) {
  base::test::TaskEnvironment tasks;
  viz::FakeDisplayClient fake;
  mojo::Remote<viz::mojom::DisplayClient> fake_remote(fake.BindRemote());
  mojo::PendingRemote<viz::mojom::LayeredWindowUpdater> fake_updater;
  fake_remote->CreateLayeredWindowUpdater(
      fake_updater.InitWithNewPipeAndPassReceiver());
  fake_remote.FlushForTesting();

  testing::NiceMock<viz::MockDisplayClient> mock;
  mojo::Remote<viz::mojom::DisplayClient> consumer(mock.BindRemote());
  bool received = false;
  EXPECT_CALL(mock, CreateLayeredWindowUpdater(testing::_))
      .WillOnce([&received](
                    mojo::PendingReceiver<viz::mojom::LayeredWindowUpdater>
                        receiver) {
        EXPECT_TRUE(receiver.is_valid());
        received = true;
      });
  mojo::PendingRemote<viz::mojom::LayeredWindowUpdater> updater;
  consumer->CreateLayeredWindowUpdater(
      updater.InitWithNewPipeAndPassReceiver());
  consumer.FlushForTesting();
  EXPECT_TRUE(received);
}

#if BUILDFLAG(IS_LINUX) && BUILDFLAG(ENABLE_CEF)
TEST(ApiCompatibilityPolicyTest, CefPolicyDirectoryUsesNativePlatformHook) {
  base::test::TaskEnvironment tasks;
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath previous;
  const bool had_previous =
      policy::ChromeBrowserPolicyConnector::GetDirPolicyFilesPath(&previous);
  base::ScopedClosureRunner restore(base::BindOnce(
      [](base::FilePath previous, bool enabled) {
        policy::ChromeBrowserPolicyConnector::EnablePlatformPolicySupport(
            enabled ? previous.AsUTF8Unsafe() : std::string());
      },
      previous, had_previous));
  policy::ChromeBrowserPolicyConnector::EnablePlatformPolicySupport(
      directory.GetPath().AsUTF8Unsafe());
  base::FilePath observed;
  ASSERT_TRUE(policy::ChromeBrowserPolicyConnector::GetDirPolicyFilesPath(
      &observed));
  EXPECT_EQ(observed, directory.GetPath());
  const auto enrollment =
      directory.GetPath().AppendASCII("enrollment/CloudManagementEnrollmentToken");
  ASSERT_TRUE(base::CreateDirectory(enrollment.DirName()));
  ASSERT_TRUE(base::WriteFile(enrollment, "alloy-policy-fixture"));
  policy::BrowserDMTokenStorageLinux storage;
  policy::BrowserDMTokenStorage::Delegate& consumer = storage;
  EXPECT_EQ(consumer.InitEnrollmentToken(), "alloy-policy-fixture");
}
#endif

}  // namespace
