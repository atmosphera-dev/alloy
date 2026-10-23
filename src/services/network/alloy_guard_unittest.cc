// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "services/network/alloy_guard.h"
#include "services/network/alloy_capability.h"

#include <set>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "services/network/public/cpp/resource_request.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
using C = AlloyGuard::TrafficClass;
using Op = AlloyGuard::Operation;

class AlloyGuardTest : public testing::Test {
 protected:
  base::test::TaskEnvironment tasks_;
};

TEST_F(AlloyGuardTest, PinnedCatalogDefaultDeny) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  const std::set<std::string_view> allowed = {
      "blink_resource_loader", "navigation_url_loader",
      "renderer_initiated_download", "service_worker_script_load",
      "service_worker_update_checker", "websocket_stream",
      "websocket_basic_stream", "webrtc_peer_connection",
      "webrtc_rtc_transport", "alloy_webrtc_manager",
      "alloy_webtransport_page", "favicon_loader"};
  EXPECT_GE(guard->Catalog().size(), 599u);
  for (const auto& entry : guard->Catalog()) {
    SCOPED_TRACE(entry.id);
    EXPECT_EQ(guard->Decide(entry.hash, "https://test.invalid/", "catalog").allowed,
              allowed.contains(entry.id));
    EXPECT_EQ(guard->Classify(entry.hash).annotation_id, entry.id);
  }
}

TEST_F(AlloyGuardTest, ClassesAndMissingAnnotations) {
  const std::pair<std::string_view, C> rows[] = {
      {"blink_resource_loader", C::kPageLoad},
      {"renderer_initiated_download", C::kPageDownload},
      {"service_worker_update_checker", C::kServiceWorker},
      {"websocket_stream", C::kWebSocket},
      {"alloy_webtransport_page", C::kWebTransport},
      {"webrtc_peer_connection", C::kWebRtc},
      {"webrtc_rtc_transport", C::kWebRtc},
      {"favicon_loader", C::kOnBehalfOfPage},
      {"alloy_guard_canary", C::kBrowserService},
      {"missing", C::kUnannotated}, {"undefined", C::kUnannotated}};
  auto guard = base::MakeRefCounted<AlloyGuard>();
  for (const auto& [id, category] : rows)
    EXPECT_EQ(guard->Classify(guard->Hash(id)).traffic_class, category);
  for (int hash : {0, guard->Hash("missing"), guard->Hash("undefined"), -1})
    EXPECT_FALSE(guard->Decide(hash, "endpoint", "missing").allowed);
  EXPECT_TRUE(guard->DecideRequest(guard->Hash("blink_resource_loader"),
                                  "page", "boundary", -1).allowed);
  EXPECT_FALSE(guard->DecideRequest(guard->Hash("reporting"), "collector",
                                   "boundary", guard->Hash("navigation_url_loader")).allowed);
}

TEST_F(AlloyGuardTest, TogglesAndRetainedCapabilityRecheck) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  for (std::string_view id : {"favicon_loader", "open_search", "alloy_dns_prefetch"}) {
    AlloyGuard::Options off;
    off.favicons = false;
    guard->SetOptions(off);
    EXPECT_FALSE(guard->Decide(guard->Hash(id), "host", "creation").allowed);
    AlloyGuard::Options on;
    on.favicons = on.open_search = on.dns_prefetch = on.preconnect = true;
    guard->SetOptions(on);
    EXPECT_TRUE(guard->Decide(guard->Hash(id), "host", "creation").allowed);
    guard->SetOptions(off);
    EXPECT_FALSE(guard->Decide(guard->Hash(id), "host", "operation",
                               Op::kRequest, guard->Hash(id)).allowed);
  }
  guard->SetOptions({});
  const int rtc = guard->Hash("webrtc_peer_connection");
  EXPECT_TRUE(guard->Decide(rtc, "host:3478", "P2P.Create").allowed);
  EXPECT_TRUE(guard->Decide(rtc, "host", "P2P.GetHostAddress", Op::kRequest, rtc).allowed);
  EXPECT_FALSE(guard->Decide(guard->Hash("blink_resource_loader"), "host",
                            "P2P.CreateSocket", Op::kRequest, rtc).allowed);
  EXPECT_FALSE(guard->Decide(guard->Hash("alloy_dns_prefetch"), "host",
                            "HostResolver.ResolveHost", Op::kStandaloneDns).allowed);
}

TEST_F(AlloyGuardTest, RealPreconnectLabelsAndTheirDnsFollowThePreconnectToggle) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  for (auto id : {"predictive_preconnect", "search_engine_preconnect", "network_hints_preconnect"}) {
    SCOPED_TRACE(id);
    const int hash = guard->Hash(id);
    EXPECT_EQ(guard->Classify(hash).annotation_id, id);
    EXPECT_EQ(guard->Classify(hash).traffic_class, C::kOnBehalfOfPage);
    EXPECT_FALSE(guard->Decide(hash, "host", "preconnect", Op::kPreconnect).allowed);
    AlloyGuard::Options options;
    options.preconnect = true;
    guard->SetOptions(options);
    EXPECT_TRUE(guard->Decide(hash, "host", "preconnect", Op::kPreconnect).allowed);
    EXPECT_TRUE(guard->Decide(guard->Hash("alloy_native_dns"), "host", "dns", Op::kOwnedDns, hash).allowed);
    guard->SetOptions({});
    EXPECT_FALSE(guard->Decide(guard->Hash("alloy_native_dns"), "host", "dns", Op::kOwnedDns, hash).allowed);
  }
}

#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
TEST_F(AlloyGuardTest, OneJsonLinePerDenial) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto path = dir.GetPath().AppendASCII("alloy-guard.log");
  auto guard = base::MakeRefCounted<AlloyGuard>();
  guard->Configure(base::File(path, base::File::FLAG_CREATE_ALWAYS |
                                   base::File::FLAG_APPEND), {});
  EXPECT_FALSE(guard->Decide(guard->Hash("alloy_guard_canary"),
                            "https://example.com/\"\n", "canary").allowed);
  tasks_.RunUntilIdle();
  std::string text;
  ASSERT_TRUE(base::ReadFileToString(path, &text));
  ASSERT_EQ(text.find('\n'), text.size() - 1);
  auto row = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  ASSERT_TRUE(row);
  EXPECT_EQ(*row->FindString("annotation_id"), "alloy_guard_canary");
  EXPECT_EQ(*row->FindString("class"), "browser-service");
  EXPECT_EQ(*row->FindString("endpoint"), "https://example.com/\"\n");
}
#else
TEST_F(AlloyGuardTest, ProductDenialRetainsClassWithoutLogWriter) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  const auto decision = guard->Decide(guard->Hash("alloy_guard_canary"),
                                     "https://example.com/", "product");
  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.traffic_class, C::kBrowserService);
  EXPECT_EQ(decision.annotation_id, "alloy_guard_canary");
}
#endif

TEST_F(AlloyGuardTest, ChildCannotLaunderMissingOrUnknownOwner) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  const int page = guard->Hash("blink_resource_loader");
  for (int owner : {0, -1, 123456, guard->Hash("alloy_browser_socket")}) {
    AlloyCapability capability(guard, owner);
    EXPECT_FALSE(capability.Check("host:443", "child", page));
  }
  AlloyCapability rtc(guard, guard->Hash("alloy_webrtc_manager"));
  EXPECT_TRUE(rtc.Check("stun.invalid", "P2P.GetHostAddress"));
  EXPECT_TRUE(rtc.Check("peer:3478", "P2P.CreateSocket", guard->Hash("webrtc_peer_connection")));
  EXPECT_FALSE(rtc.Check("host:443", "child", page));
}

TEST_F(AlloyGuardTest, DownloadContinuationsRequireTheOriginalPageOwner) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  for (auto continuation : {"download_manager_resume", "parallel_download_job"}) {
    const int tag = guard->Hash(continuation);
    EXPECT_FALSE(guard->Decide(tag, "file", "unowned").allowed);
    EXPECT_FALSE(guard->DecideRequest(tag, "file", "unowned").allowed);
    for (auto owner : {"renderer_initiated_download", "navigation_url_loader"}) {
      const auto result = guard->Decide(tag, "file", "continuation",
          Op::kDownloadContinuation, guard->Hash(owner));
      EXPECT_TRUE(result.allowed);
      EXPECT_EQ(result.traffic_class, C::kPageDownload);
      EXPECT_EQ(result.annotation_id, continuation);
      EXPECT_EQ(result.originating_annotation_id, owner);
      const auto boundary = guard->DecideRequest(tag, "file", "boundary",
                                                guard->Hash(owner));
      EXPECT_TRUE(boundary.allowed);
      EXPECT_EQ(boundary.traffic_class, C::kPageDownload);
      EXPECT_EQ(boundary.originating_annotation_id, owner);
    }
    for (int owner : {0, -1, 123456, guard->Hash("missing"),
                     guard->Hash("reporting"), guard->Hash("blink_resource_loader"),
                     guard->Hash("service_worker_script_load"), tag}) {
      EXPECT_FALSE(guard->Decide(tag, "file", "continuation",
          Op::kDownloadContinuation, owner).allowed);
      EXPECT_FALSE(guard->DecideRequest(tag, "file", "boundary", owner).allowed);
    }
  }
  // An inherited page owner is not a general-purpose browser-service exemption.
  EXPECT_FALSE(guard->Decide(guard->Hash("reporting"), "file", "unrelated",
      Op::kDownloadContinuation, guard->Hash("navigation_url_loader")).allowed);
}

TEST_F(AlloyGuardTest, DnsTransportRequiresRealOriginAndRetainsClass) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  for (auto transport : {"alloy_native_dns", "dns_over_https"}) {
    const int hash = guard->Hash(transport);
    EXPECT_FALSE(guard->Decide(hash, "resolver", "native").allowed);
    for (auto owner : {"blink_resource_loader", "webrtc_peer_connection", "alloy_webtransport_page"}) {
      const auto decision = guard->Decide(hash, "resolver", "native", Op::kOwnedDns, guard->Hash(owner));
      EXPECT_TRUE(decision.allowed);
      EXPECT_EQ(decision.annotation_id, transport);
      EXPECT_EQ(decision.originating_annotation_id, owner);
      EXPECT_EQ(decision.traffic_class, guard->Classify(guard->Hash(owner)).traffic_class);
      if (hash == guard->Hash("dns_over_https")) {
        EXPECT_TRUE(guard->DecideRequest(hash, "resolver", "boundary",
                                        guard->Hash(owner)).allowed);
      }
    }
    EXPECT_FALSE(guard->Decide(hash, "resolver", "native", Op::kOwnedDns, -1).allowed);
    EXPECT_FALSE(guard->Decide(hash, "resolver", "native", Op::kOwnedDns, guard->Hash("reporting")).allowed);
    AlloyGuard::Options options;
    options.dns_prefetch = true;
    guard->SetOptions(options);
    EXPECT_FALSE(guard->Decide(hash, "resolver", "probe", Op::kOwnedDns, -1).allowed);
    EXPECT_TRUE(guard->Decide(hash, "resolver", "explicit", Op::kOwnedDns, guard->Hash("alloy_dns_prefetch")).allowed);
    guard->SetOptions({});
  }
  EXPECT_FALSE(guard->Decide(guard->Hash("reporting"), "collector", "native", Op::kOwnedDns,
                            guard->Hash("blink_resource_loader")).allowed);
}

TEST_F(AlloyGuardTest, WorkerLoadsRequireTheValidatedPageCreator) {
  auto guard = base::MakeRefCounted<AlloyGuard>();
  const int worker = guard->Hash("worker_script_load");
  EXPECT_FALSE(guard->DecideRequest(worker, "https://host/worker.js", "unowned").allowed);
  // It is not an annotation-level exemption: extension and browser documents
  // use the same native fetcher and may request an ordinary HTTPS worker URL.
  for (auto scheme : {"https", "http", "file", "data", "about", "custom-page"}) {
    const int owner = net::AlloyWorkerOwnerForPrincipal(scheme, false);
    const auto result = guard->DecideRequest(worker, "https://host/worker.js", "page", owner);
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.annotation_id, "worker_script_load");
    EXPECT_EQ(result.originating_annotation_id, "blink_resource_loader");
    EXPECT_EQ(result.traffic_class, C::kPageLoad);
  }
  for (int owner : {0, -1, 123456, worker, guard->Hash("reporting"),
                   guard->Hash("renderer_initiated_download"),
                   net::AlloyWorkerOwnerForPrincipal("chrome-extension", false),
                   net::AlloyWorkerOwnerForPrincipal("isolated-app", false),
                   net::AlloyWorkerOwnerForPrincipal("https", true),
                   net::AlloyWorkerOwnerForPrincipal("chrome", true),
                   net::AlloyWorkerOwnerForPrincipal("", false)}) {
    EXPECT_FALSE(guard->DecideRequest(worker, "https://host/worker.js", "non-page", owner).allowed);
  }
  const int page = guard->Hash("blink_resource_loader");
  EXPECT_FALSE(guard->DecideRequest(guard->Hash("reporting"), "collector", "service", page).allowed);
  EXPECT_FALSE(guard->Decide(guard->Hash("reporting"), "collector", "forged-operation", Op::kWorkerScript, page).allowed);
}

TEST_F(AlloyGuardTest, WorkerOwnershipStaysInItsTrustedOperation) {
  ResourceRequest::TrustedParams params;
  const int worker = AlloyGuard::Hash("worker_script_load");
  const int page = AlloyGuard::Hash("blink_resource_loader");
  params.alloy_download_owner = page;
  EXPECT_EQ(params.AlloyOriginatingOwner(worker, true), 0);
  params.alloy_worker_owner = page;
  EXPECT_EQ(params.AlloyOriginatingOwner(worker, true), page);
  EXPECT_EQ(params.AlloyOriginatingOwner(worker, false), 0);
  params.alloy_download_owner = 0;
  EXPECT_EQ(params.AlloyOriginatingOwner(AlloyGuard::Hash("reporting"), true), 0);
  ResourceRequest::TrustedParams copied(params);
  EXPECT_EQ(copied.alloy_worker_owner, page);
  EXPECT_TRUE(params.EqualsForTesting(copied));
  copied.alloy_worker_owner = 0;
  EXPECT_FALSE(params.EqualsForTesting(copied));
}
}  // namespace
}  // namespace network
