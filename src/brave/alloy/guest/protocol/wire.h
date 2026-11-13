// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_PROTOCOL_WIRE_H_
#define BRAVE_ALLOY_GUEST_PROTOCOL_WIRE_H_
// Shared with the static C transport relay. No native layouts cross the wire.
#define ALLOY_GUEST_PROTOCOL_VERSION 1
#define ALLOY_GUEST_ENVELOPE_SIZE 72
// url/url_constants.h on the pin: Chromium accepts URLs up to 2 MiB. One
// length-prefixed URL is the largest control field; resources use their lane.
#define ALLOY_GUEST_MAX_CONTROL_PAYLOAD (2u * 1024u * 1024u + 4u)
#define ALLOY_GUEST_CONTROL_CHUNK_SIZE 16384u
#define ALLOY_GUEST_CLOSE_MESSAGE 13
#define ALLOY_GUEST_CLOSED_MESSAGE 14
#define ALLOY_GUEST_FAILURE_MESSAGE 15
#endif
