// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/test/metrics/histogram_tester.h"
#include "brave/browser/misc_metrics/process_misc_metrics.h"
#include "brave/browser/misc_metrics/profile_misc_metrics_service.h"
#include "brave/browser/misc_metrics/profile_misc_metrics_service_factory.h"
#include "brave/browser/p3a/p3a_core_metrics.h"
#include "brave/components/misc_metrics/menu_metrics.h"
#include "brave/components/misc_metrics/new_tab_metrics.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/prefs/testing_pref_service.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
TEST_F(ChromeRenderViewHostTestHarness,
       NativeActivityCompatibilityMojoEndpointsKeepNoValuesOrHistograms) {
  TestingPrefServiceSimple local_state;
  RegisterLocalState(local_state.registry());
  local_state.SetString("dns_over_https.mode", "secure");
  const auto before = local_state.GetPreferenceValues(PrefService::EXCLUDE_DEFAULTS);
  base::HistogramTester histograms;
  {
    misc_metrics::ProcessMiscMetrics owner(&local_state);
    ASSERT_NE(nullptr, owner.menu_metrics());
    ASSERT_NE(nullptr, owner.new_tab_metrics());
    owner.menu_metrics()->RecordMenuShown();
    owner.menu_metrics()->RecordMenuDismiss();
    owner.menu_metrics()->RecordMenuGroupAction(misc_metrics::MenuGroup::kBraveFeatures);
    mojo::Remote<brave_new_tab_page::mojom::NewTabMetrics> ntp;
    owner.new_tab_metrics()->Bind(ntp.BindNewPipeAndPassReceiver());
    ntp->ReportNTPSearchDefaultEngine(std::nullopt);
    ntp->ReportNTPSearchUsage(1);
    ntp.FlushForTesting();
    EXPECT_TRUE(ntp.is_connected());
    mojo::Remote<misc_metrics::mojom::Web3Metrics> web3;
    owner.web3_metrics().BindReceiver(web3.BindNewPipeAndPassReceiver());
    web3->RecordDappVisit();
    web3.FlushForTesting();
    EXPECT_TRUE(web3.is_connected());
    EXPECT_EQ(nullptr, owner.default_browser_monitor());
    EXPECT_EQ(nullptr, owner.vertical_tab_metrics());
    EXPECT_EQ(nullptr, owner.split_view_metrics());
    EXPECT_EQ(local_state.GetPreferenceValues(PrefService::EXCLUDE_DEFAULTS), before);
  }
  brave::BraveWindowTracker::CreateInstance(&local_state);
  EXPECT_FALSE(brave::BraveWindowTracker::HasInstance());
  brave::BraveWindowTracker::ClearInstance();
  EXPECT_EQ(local_state.GetPreferenceValues(PrefService::EXCLUDE_DEFAULTS), before);
  for (const char* name : {misc_metrics::kFrequentMenuGroupHistogramName,
                           misc_metrics::kMenuDismissRateHistogramName,
                           misc_metrics::kMenuOpensHistogramName,
                           misc_metrics::kNTPSearchEngineHistogramName,
                           misc_metrics::kNTPSearchUsageHistogramName,
                           misc_metrics::kNTPGoogleWidgetUsageHistogramName,
                           misc_metrics::kWeb3DappVisitHistogramName,
                           misc_metrics::kWidevineEnabledHistogramName,
                           "Brave.Core.LastTimeIncognitoUsed",
                           "Brave.Core.TorEverUsed"}) {
    histograms.ExpectTotalCount(name, 0);
  }
}

TEST_F(ChromeRenderViewHostTestHarness,
       NativeProfileMetricsFactoryKeepsNoCollectorOrPreferenceProjection) {
  const auto before = profile()->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS);
  auto* service = misc_metrics::ProfileMiscMetricsServiceFactory::GetServiceForContext(profile());
  ASSERT_NE(nullptr, service);
  EXPECT_EQ(nullptr, service->GetPageMetrics());
#if BUILDFLAG(ENABLE_AI_CHAT)
  EXPECT_EQ(nullptr, service->GetAIChatMetrics());
#endif
  service->Shutdown();
  EXPECT_EQ(profile()->GetPrefs()->GetPreferenceValues(PrefService::INCLUDE_DEFAULTS), before);
}
}  // namespace
}  // namespace alloy::site_records
