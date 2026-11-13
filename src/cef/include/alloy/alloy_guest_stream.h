// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#pragma once
#include "include/cef_base.h"
// M3 owns resource transport, CPU leases, input and its translated declarations.
// The foundation's control session remains the lifecycle/identity authority.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Stream extension callback slot. Foundation reports this feature
/// unavailable; M3 adds typed resources/input without replacing control.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestStreamHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit stream unavailability on the serialized client sequence, never a
  /// native-window or synchronous display fallback.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
