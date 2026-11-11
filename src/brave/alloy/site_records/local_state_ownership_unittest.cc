// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/browser/misc_metrics/process_misc_metrics.h"
#include "brave/components/misc_metrics/menu_metrics.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

TEST_F(ChromeRenderViewHostTestHarness,
       ProcessActivityOwnerDoesNotRetainNativeMenuEventsOrCollectors) {
  TestingPrefServiceSimple local_state;
  RegisterLocalState(local_state.registry());
  local_state.SetString("dns_over_https.mode", "secure");
  constexpr const char* activity[] = {
      "brave.misc_metrics.menu_group_actions",
      "brave.misc_metrics.menu_shown_storage",
      "brave.misc_metrics.menu_dismiss_storage",
  };
  for (const char* key : activity) {
    ASSERT_NE(nullptr, local_state.FindPreference(key));
    ASSERT_EQ(nullptr, local_state.GetUserPrefValue(key)) << key;
  }
  {
    misc_metrics::ProcessMiscMetrics owner(&local_state);
    if (auto* menu = owner.menu_metrics()) {
      menu->RecordMenuShown();
      menu->RecordMenuGroupAction(misc_metrics::MenuGroup::kTabWindow);
      menu->RecordMenuDismiss();
    }
    for (const char* key : activity) {
      EXPECT_EQ(nullptr, local_state.GetUserPrefValue(key)) << key;
    }
    EXPECT_EQ(nullptr, owner.uptime_monitor());
    EXPECT_EQ(nullptr, owner.media_session_metrics());
    EXPECT_EQ(nullptr, owner.captcha_metrics());
    EXPECT_EQ("secure", local_state.GetString("dns_over_https.mode"));
  }
  for (const char* key : activity) {
    EXPECT_EQ(nullptr, local_state.GetUserPrefValue(key)) << key;
  }
}

}  // namespace
}  // namespace alloy::site_records
