// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NET_BASE_ALLOY_TRAFFIC_CLASS_H_
#define NET_BASE_ALLOY_TRAFFIC_CLASS_H_

#include <cstdint>
#include <string_view>

namespace net {
enum class AlloyTrafficClass {
#define ALLOY_TRAFFIC_CLASS(symbol, name, dscp) symbol,
#define ALLOY_NO_CLASS(name, dscp)
#include "net/base/alloy_traffic_classes.inc"
#undef ALLOY_NO_CLASS
#undef ALLOY_TRAFFIC_CLASS
};

constexpr int AlloyAnnotationHash(std::string_view id) {
  int hash = 0;
  for (unsigned char c : id) {
    hash = (static_cast<int64_t>(hash) * 31 + c) % 138003713;
  }
  return hash;
}

// Classification is shared by policy and instrumentation; this is never an
// admission decision. A socket always carries its actual originating
// annotation.
constexpr AlloyTrafficClass ClassifyAlloyAnnotation(int hash) {
  using C = AlloyTrafficClass;
  if (hash == 0 || hash == -1 || hash == AlloyAnnotationHash("missing") ||
      hash == AlloyAnnotationHash("undefined") ||
      hash == AlloyAnnotationHash("test")) {
    return C::kUnannotated;
  }
  if (hash == AlloyAnnotationHash("blink_resource_loader") ||
      hash == AlloyAnnotationHash("navigation_url_loader")) {
    return C::kPageLoad;
  }
  if (hash == AlloyAnnotationHash("renderer_initiated_download")) {
    return C::kPageDownload;
  }
  if (hash == AlloyAnnotationHash("service_worker_script_load") ||
      hash == AlloyAnnotationHash("service_worker_update_checker")) {
    return C::kServiceWorker;
  }
  if (hash == AlloyAnnotationHash("websocket_stream") ||
      hash == AlloyAnnotationHash("websocket_basic_stream")) {
    return C::kWebSocket;
  }
  if (hash == AlloyAnnotationHash("alloy_webtransport_page")) {
    return C::kWebTransport;
  }
  if (hash == AlloyAnnotationHash("webrtc_peer_connection") ||
      hash == AlloyAnnotationHash("webrtc_rtc_transport") ||
      hash == AlloyAnnotationHash("alloy_webrtc_manager")) {
    return C::kWebRtc;
  }
  if (hash == AlloyAnnotationHash("favicon_loader") ||
      hash == AlloyAnnotationHash("open_search") ||
      hash == AlloyAnnotationHash("alloy_dns_prefetch") ||
      hash == AlloyAnnotationHash("predictive_preconnect") ||
      hash == AlloyAnnotationHash("search_engine_preconnect") ||
      hash == AlloyAnnotationHash("network_hints_preconnect")) {
    return C::kOnBehalfOfPage;
  }
  return C::kBrowserService;
}

constexpr std::string_view AlloyTrafficClassName(AlloyTrafficClass category) {
  switch (category) {
#define ALLOY_TRAFFIC_CLASS(symbol, name, dscp) \
  case AlloyTrafficClass::symbol:               \
    return name;
#define ALLOY_NO_CLASS(name, dscp)
#include "net/base/alloy_traffic_classes.inc"
#undef ALLOY_NO_CLASS
#undef ALLOY_TRAFFIC_CLASS
  }
  return "unannotated";
}
}  // namespace net
#endif  // NET_BASE_ALLOY_TRAFFIC_CLASS_H_
