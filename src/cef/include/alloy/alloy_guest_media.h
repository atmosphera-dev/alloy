// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_MEDIA_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_MEDIA_H_
#pragma once
#include "include/cef_base.h"
// M4 owns GPU leases, PCM, clock mapping and their translated declarations.
// No native speaker or desktop-service fallback is provided by this slot.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Media extension callback slot. Foundation reports unavailable;
/// M4 owns the typed GPU/PCM/time declarations.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestMediaHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit media unavailability on the serialized client sequence, never a
  /// native speaker fallback.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
