// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/namespace_lock/packaging_protection.h"
#include "base/json/json_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest {
namespace {
LinuxSandboxFacts Observed(const std::string& route) {
  LinuxSandboxFacts facts;
  facts.observed = true;
  facts.browser_nnp = true;
  facts.renderer_nnp = true;
  facts.renderer_seccomp = 2;
  facts.route = route;
  return facts;
}

TEST(GuestPackaging, NativeDoesNotClaimExternalFlatpakEntries) {
  auto fields = PackagingProtectionFields(4, Observed("userns"), false);
  ASSERT_TRUE(fields);
  auto coverage = base::JSONReader::ReadDict((*fields)[1], base::JSON_PARSE_RFC);
  ASSERT_TRUE(coverage);
  EXPECT_EQ(*coverage->FindString("flatpak_external_entries"), "unavailable");
  EXPECT_NE(coverage->FindString("snap_external_entries")->find("unverified-blocked"), std::string::npos);
  auto native = base::JSONReader::ReadDict((*fields)[3], base::JSON_PARSE_RFC);
  ASSERT_TRUE(native);
  EXPECT_EQ(*native->FindString("route"), "userns");
}

TEST(GuestPackaging, FlatpakReportsActualBrokeredRouteAndNoNamespaceView) {
  auto fields = PackagingProtectionFields(3, Observed("flatpak-brokered-userns"), true);
  ASSERT_TRUE(fields);
  EXPECT_EQ((*fields)[0], "data-writes-denied");
  EXPECT_EQ((*fields)[2], "verified-tmpfs");
  auto coverage = base::JSONReader::ReadDict((*fields)[1], base::JSON_PARSE_RFC);
  auto native = base::JSONReader::ReadDict((*fields)[3], base::JSON_PARSE_RFC);
  auto limits = base::JSONReader::ReadDict((*fields)[4], base::JSON_PARSE_RFC);
  ASSERT_TRUE(coverage && native && limits);
  EXPECT_NE(coverage->FindString("flatpak_external_entries")->find("fixed-bootstrap-per-entry"), std::string::npos);
  EXPECT_EQ(*native->FindString("route"), "flatpak-brokered-userns");
  EXPECT_NE(native->FindString("setuid_helper")->find("never-used"), std::string::npos);
  EXPECT_NE(limits->FindString("namespace_filesystem_and_packages")->find("native-namespace-view-absent"), std::string::npos);
  EXPECT_EQ(*limits->FindString("metadata_and_atime"), "not-refused-by-Landlock");
}

TEST(GuestPackaging, PackagingSelectionCannotRelabelAnotherRoute) {
  EXPECT_FALSE(PackagingProtectionFields(4, Observed("userns"), true));
  EXPECT_FALSE(PackagingProtectionFields(4, Observed("setuid"), true));
  EXPECT_FALSE(PackagingProtectionFields(4, Observed("flatpak-brokered-userns"), false));
}

TEST(GuestPackaging, MissingControlsNeverProduceProtectedReport) {
  for (int failure = 0; failure < 6; ++failure) {
    auto facts = Observed("flatpak-brokered-userns");
    int abi = 4;
    if (failure == 0) abi = 2;
    if (failure == 1) facts.observed = false;
    if (failure == 2) facts.browser_nnp = false;
    if (failure == 3) facts.renderer_nnp = false;
    if (failure == 4) facts.renderer_seccomp = 0;
    if (failure == 5) facts.renderer_capabilities = 1;
    EXPECT_FALSE(PackagingProtectionFields(abi, facts, true));
  }
}
}
}
