// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_ACCESSIBILITY_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_ACCESSIBILITY_H_
#pragma once
#include "include/cef_base.h"
// M6 owns AX snapshots/deltas, original-node actions and translated declarations.
// The app, not the guest, provides native accessibility objects.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Accessibility extension slot. Foundation reports unavailable;
/// M6 owns typed AX updates and original-node actions.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestAccessibilityHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit AX connector unavailability on the serialized client sequence,
  /// never native accessibility objects in the guest.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
