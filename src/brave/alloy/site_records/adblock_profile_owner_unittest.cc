// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <memory>
#include <optional>
#include <string>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "brave/browser/ui/webui/settings/brave_adblock_handler.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "brave/test/base/testing_brave_browser_process.h"
#include "chrome/browser/browser_process.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/test_web_ui.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class NativeAdBlockProfileTest : public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    ASSERT_TRUE(TestingBraveBrowserProcess::GetGlobal());
    ASSERT_TRUE(service_root_.CreateUniqueTempDir());
    auto* prefs = g_browser_process->local_state();
    ASSERT_TRUE(prefs->FindPreference(brave_shields::prefs::kAdBlockCustomFilters));
    if (const auto* value =
            prefs->GetUserPrefValue(brave_shields::prefs::kAdBlockCustomFilters)) {
      original_filters_ = value->Clone();
    }
    prefs->ClearPref(brave_shields::prefs::kAdBlockCustomFilters);
    // Supply the normal process owner's real service to the native settings
    // handler. No private getter or test-only replacement of editor routing.
    TestingBraveBrowserProcess::GetGlobal()->SetAdBlockService(
        std::make_unique<brave_shields::AdBlockService>(
            prefs, "en", nullptr,
            base::SequencedTaskRunner::GetCurrentDefault(), base::DoNothing(),
            service_root_.GetPath()));
  }

  void TearDown() override {
    TestingBraveBrowserProcess::GetGlobal()->SetAdBlockService(nullptr);
    auto* prefs = g_browser_process->local_state();
    if (original_filters_) {
      prefs->Set(brave_shields::prefs::kAdBlockCustomFilters,
                 std::move(*original_filters_));
    } else {
      prefs->ClearPref(brave_shields::prefs::kAdBlockCustomFilters);
    }
    task_environment()->RunUntilIdle();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  static void WriteFilters(content::TestWebUI& ui, const std::string& filters) {
    base::ListValue args;
    args.Append(filters);
    ui.HandleReceivedMessage("brave_adblock.updateCustomFilters", args);
  }

  static std::optional<std::string> ReadFilters(content::TestWebUI& ui) {
    ui.ClearTrackedCalls();
    base::ListValue args;
    args.Append("profile-owner-read");
    ui.HandleReceivedMessage("brave_adblock.getCustomFilters", args);
    for (const auto& call : ui.call_data()) {
      if (call->function_name() == "cr.webUIResponse" && call->arg1() &&
          call->arg1()->is_string() &&
          call->arg1()->GetString() == "profile-owner-read" && call->arg2() &&
          call->arg2()->is_bool() && call->arg2()->GetBool() && call->arg3() &&
          call->arg3()->is_string()) {
        return call->arg3()->GetString();
      }
    }
    ADD_FAILURE() << "native custom-filter editor did not return a string";
    return std::nullopt;
  }

 private:
  base::ScopedTempDir service_root_;
  std::optional<base::Value> original_filters_;
};

TEST_F(NativeAdBlockProfileTest,
       CustomFilterEditorDoesNotReadOrOverwriteAnotherMemoryContext) {
  constexpr char kAlpha[] = "||adblock-owner-alpha.invalid^$important";
  constexpr char kBeta[] = "||adblock-owner-beta.invalid^$important";
  auto peer = TestingProfile::Builder().Build();
  RequestContextPolicy::AttachMemory(*profile());
  RequestContextPolicy::AttachMemory(*peer);
  ASSERT_NE(RequestContextPolicy::Get(*profile())->profile(),
            RequestContextPolicy::Get(*peer)->profile());
  auto peer_contents =
      content::WebContentsTester::CreateTestWebContents(peer.get(), nullptr);
  content::TestWebUI alpha, beta;
  alpha.set_web_contents(web_contents());
  beta.set_web_contents(peer_contents.get());
  alpha.AddMessageHandler(std::make_unique<BraveAdBlockHandler>());
  beta.AddMessageHandler(std::make_unique<BraveAdBlockHandler>());

  ASSERT_EQ(std::optional<std::string>(""), ReadFilters(alpha));
  ASSERT_EQ(std::optional<std::string>(""), ReadFilters(beta));
  WriteFilters(alpha, kAlpha);
  ASSERT_EQ(std::optional<std::string>(kAlpha), ReadFilters(alpha));
  EXPECT_EQ(std::optional<std::string>(""), ReadFilters(beta))
      << "another context must not read alpha's private filter";
  WriteFilters(beta, kBeta);
  ASSERT_EQ(std::optional<std::string>(kBeta), ReadFilters(beta));
  EXPECT_EQ(std::optional<std::string>(kAlpha), ReadFilters(alpha))
      << "beta's edit must not overwrite alpha's private filter";
  EXPECT_EQ(nullptr,
            g_browser_process->local_state()->GetUserPrefValue(
                brave_shields::prefs::kAdBlockCustomFilters))
      << "private filter text must not enter process Local State";
}

}  // namespace
}  // namespace alloy::site_records
