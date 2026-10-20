// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Canonical class/name/DSCP table for the guard and wire cross-check.
// DSCP is the six-bit codepoint, NOT the shifted IP_TOS/IPV6_TCLASS byte.
// All codepoints are local/experimental pool 3 (xxxx11). Never send them off
// the test machine: count first, then reset DSCP to zero before egress.
// libc getaddrinfo traffic carries no class; permitted only to the configured
// resolver (UDP/TCP port 53). Unmarked traffic anywhere else is a failure.
ALLOY_TRAFFIC_CLASS(kPageLoad, "page-load", 3)
ALLOY_TRAFFIC_CLASS(kPageDownload, "page-download", 7)
ALLOY_TRAFFIC_CLASS(kServiceWorker, "service-worker", 11)
ALLOY_TRAFFIC_CLASS(kWebSocket, "websocket", 15)
ALLOY_TRAFFIC_CLASS(kWebTransport, "webtransport", 19)
ALLOY_TRAFFIC_CLASS(kWebRtc, "webrtc", 23)
ALLOY_TRAFFIC_CLASS(kOnBehalfOfPage, "on-behalf-of-page", 27)
ALLOY_TRAFFIC_CLASS(kBrowserService, "browser-service", 31)
ALLOY_TRAFFIC_CLASS(kUnannotated, "unannotated", 35)
ALLOY_NO_CLASS("no-class-known", 39)
