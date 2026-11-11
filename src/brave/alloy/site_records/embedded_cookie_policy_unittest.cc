// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/alloy/storage_policy.h"
#include "base/command_line.h"
#include "base/test/multiprocess_test.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "gpu/config/gpu_finch_features.h"
#include "net/cookies/alloy_cookie_settings.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

constexpr auto kListed = net::CookieSettingOverride::kAlloyConsideredFirstParty;
constexpr auto kExplicitBlock =
    net::CookieSettingOverride::kForceDisableThirdPartyCookies;

TEST(AlloyCookieOriginTest, OnlyExactNonOpaqueOriginCanIssueAuthority) {
  const auto captured = url::Origin::Create(GURL("https://beta.test:8443/"));
  for (const auto& destination : {
           GURL("https://gamma.test:8443/"), GURL("http://beta.test:8443/"),
           GURL("https://beta.test/"), GURL("https://sub.beta.test:8443/"),
           GURL("data:text/plain,opaque"), GURL()}) {
    SCOPED_TRACE(destination.spec());
    net::CookieSettingOverrides overrides{kListed, kExplicitBlock};
    net::ApplyAlloyCookieOrigin(captured, destination, overrides);
    EXPECT_FALSE(overrides.Has(kListed));
    EXPECT_TRUE(overrides.Has(kExplicitBlock));
  }
  net::CookieSettingOverrides overrides{kListed, kExplicitBlock};
  const GURL matching("https://beta.test:8443/another-path");
  net::ApplyAlloyCookieOrigin(std::nullopt, matching, overrides);
  EXPECT_FALSE(overrides.Has(kListed));
  net::ApplyAlloyCookieOrigin(url::Origin(), matching, overrides);
  EXPECT_FALSE(overrides.Has(kListed));
  net::ApplyAlloyCookieOrigin(captured, matching, overrides);
  EXPECT_TRUE(overrides.Has(kListed));
  EXPECT_TRUE(overrides.Has(kExplicitBlock));
}

TEST(AlloyCookieOriginTest, RedirectHopCannotRetainAnotherOriginsAuthority) {
  const GURL listed("https://beta.test/");
  const auto captured = url::Origin::Create(listed);
  net::CookieSettingOverrides overrides{kExplicitBlock};
  net::ApplyAlloyCookieOrigin(captured, listed, overrides);
  ASSERT_TRUE(overrides.Has(kListed));
  net::ApplyAlloyCookieOrigin(captured, GURL("https://gamma.test/"), overrides);
  EXPECT_FALSE(overrides.Has(kListed));
  net::ApplyAlloyCookieOrigin(captured, listed, overrides);
  EXPECT_TRUE(overrides.Has(kListed));
  EXPECT_TRUE(overrides.Has(kExplicitBlock));
}

TEST(AlloyGpuCacheAdmissionTest, BootstrapAndEnabledFeaturesCannotAdmitDisk) {
  for (const char* mode : {"legacy", "no-disk"}) {
    SCOPED_TRACE(mode);
    auto command = base::GetMultiProcessTestChildBaseCommandLine();
    command.AppendSwitchASCII("alloy-gpu-bootstrap", mode);
    auto child = base::SpawnMultiProcessTestChild(
        "AlloyGpuCacheAdmissionChild", command, {});
    ASSERT_TRUE(child.IsValid());
    int exit = -1;
    const bool stopped = base::WaitForMultiprocessTestChildExit(
        child, base::Seconds(30), &exit);
    if (!stopped)
      base::TerminateMultiProcessTestChild(child, 99, true);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(0, exit);
  }
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloyGpuCacheAdmissionChild) {
  const auto* command = base::CommandLine::ForCurrentProcess();
  const bool no_disk =
      command->GetSwitchValueASCII("alloy-gpu-bootstrap") == "no-disk";
  base::alloy::InitializeStoragePolicy(no_disk);
  base::test::ScopedFeatureList enabled;
  enabled.InitWithFeatures({features::kGpuShaderDiskCache,
                            features::kGpuPersistentCache,
                            features::kSkiaGraphiteUsePersistentCache}, {});
  // No disable-GPU or disable-shader-cache switches: native admission must win
  // over enabled cache features even for a legacy-disk installation bootstrap.
  base::CommandLine unrestricted(base::CommandLine::NO_PROGRAM);
  return base::alloy::IsNoDiskProcess() == no_disk &&
                 !base::alloy::AllowsGpuDiskCaches() &&
                 !features::IsShaderDiskCacheEnabled(&unrestricted)
             ? 0 : 1;
}

}  // namespace alloy::site_records
