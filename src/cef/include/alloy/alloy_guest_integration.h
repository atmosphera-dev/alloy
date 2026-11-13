// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_INTEGRATION_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_INTEGRATION_H_
#pragma once
#include "include/cef_base.h"
// M5 owns typed native-integration requests and their translated declarations.
// Missing providers are unavailable, not permission to use desktop services.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Native-integration extension slot. Foundation is unavailable;
/// M5 owns typed requests and original-document replies.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestIntegrationHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit integration unavailability on the serialized client sequence,
  /// never permission to invoke a desktop service.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
