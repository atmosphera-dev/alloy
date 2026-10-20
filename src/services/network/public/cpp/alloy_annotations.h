// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef SERVICES_NETWORK_PUBLIC_CPP_ALLOY_ANNOTATIONS_H_
#define SERVICES_NETWORK_PUBLIC_CPP_ALLOY_ANNOTATIONS_H_
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace network::alloy {
inline constexpr auto kNativeDns = net::DefineNetworkTrafficAnnotation(
    "alloy_native_dns", R"(
      semantics {
        sender: "Native hostname resolver"
        description: "A native hostname lookup or resolver probe retains the actual originating request annotation separately from the DNS transport annotation. Ownerless native lookups are denied."
        trigger: "A labelled connection, page transport or explicit resolver asks for hostname resolution."
        data: "The hostname and DNS query type selected by the originating request."
        destination: OTHER
        destination_other: "The configured resolver."
      }
      policy {
        cookies_allowed: NO
        setting: "Only allowed for an allowed originating request; explicit standalone DNS additionally requires alloy.dns_prefetch_enabled."
        policy_exception_justification: "Transport permission follows the actual originating request, not a global DNS-service exception."
      })");
inline constexpr auto kWebTransport = net::DefineNetworkTrafficAnnotation(
    "alloy_webtransport_page", R"(
      semantics {
        sender: "Page WebTransport"
        description: "A document opens an HTTP/3 WebTransport session."
        trigger: "A page invokes the WebTransport API."
        data: "The URL, handshake headers and application data supplied by the page."
        destination: OTHER
        destination_other: "The endpoint chosen by the page."
      }
      policy {
        cookies_allowed: NO
        setting: "Page networking is available by default. Alloy classifies this as webtransport for parental policy."
        policy_exception_justification: "A user-requested web platform transport."
      })");
inline constexpr auto kWebRtcManager = net::DefineNetworkTrafficAnnotation(
    "alloy_webrtc_manager", R"(
      semantics {
        sender: "Page WebRTC socket manager"
        description: "Owns labelled page WebRTC sockets and their prerequisite hostname lookups."
        trigger: "A document requests the peer connection or RTC transport API."
        data: "Page-selected hostnames, endpoints and WebRTC connection data."
        destination: OTHER
        destination_other: "Page-selected peers, STUN and TURN servers."
      }
      policy {
        cookies_allowed: NO
        setting: "Page WebRTC is allowed by default; the webrtc class is exposed for later parental policy."
        policy_exception_justification: "A user-requested web platform transport."
      })");
inline constexpr auto kBrowserSocket = net::DefineNetworkTrafficAnnotation(
    "alloy_browser_socket", R"(
      semantics {
        sender: "Browser-owned socket capability"
        description: "Labels creation of a built-in service or extension socket factory which formerly carried no annotation. Child operations retain their individual annotations. Alloy denies this browser-service capability."
        trigger: "A browser service or extension requests a UDP, proxy or P2P capability."
        data: "No data is sent: the capability is denied before socket creation."
        destination: OTHER
        destination_other: "The service-selected endpoint, if networking were permitted."
      }
      policy {
        cookies_allowed: NO
        setting: "Permanently denied by the Alloy network guard."
        policy_exception_justification: "This built-in service has no allowed browser-traffic toggle."
      })");
inline constexpr auto kDnsPrefetch = net::DefineNetworkTrafficAnnotation(
    "alloy_dns_prefetch", R"(
      semantics {
        sender: "Standalone hostname resolver"
        description: "Labels hostname resolution not owned by an already authorized request."
        trigger: "A caller requests standalone ResolveHost or HostResolver creation."
        data: "The requested hostname and query type."
        destination: OTHER
        destination_other: "The configured DNS resolver."
      }
      policy {
        cookies_allowed: NO
        setting: "Disabled unless alloy.dns_prefetch_enabled is enabled."
        chrome_policy { NetworkPredictionOptions { NetworkPredictionOptions: 2 } }
      })");
}  // namespace network::alloy
#endif
