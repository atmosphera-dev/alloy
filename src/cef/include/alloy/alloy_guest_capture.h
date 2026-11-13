// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_CAPTURE_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_CAPTURE_H_
#pragma once
#include "include/cef_base.h"
// M8 owns reverse microphone/camera/display providers and their declarations.
// No ambient device discovery or opening is supplied by the foundation slot.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: External capture extension slot. Foundation is unavailable;
/// M8 owns typed app-supplied device/sample declarations.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestCaptureHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit capture unavailability on the serialized client sequence, never
  /// native microphone/camera/display opening.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
