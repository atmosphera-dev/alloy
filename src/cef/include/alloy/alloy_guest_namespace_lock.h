// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_NAMESPACE_LOCK_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_NAMESPACE_LOCK_H_
#pragma once
#include "include/cef_base.h"
// M7 owns stronger namespace/package-entry protection and its declarations.
// Syscall availability alone must not be reported as package launch coverage.
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Namespace/package protection extension slot. Foundation reports
/// unavailable; M7 owns its typed backend and package-entry declarations.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestNamespaceLockHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Explicit backend unavailability on the serialized client sequence, never
  /// permission to silently weaken an accepted protection report.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
};
#endif
#endif
