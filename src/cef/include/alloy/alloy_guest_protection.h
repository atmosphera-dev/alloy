// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_PROTECTION_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_PROTECTION_H_
#pragma once
#include "include/cef_base.h"

#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Copied achieved protection, separate from feature availability.
/// The foundation reports "unavailable" enforcement, never write containment.
/// Linux enforcement fields and backends are extended by the protection owner.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestProtection : public virtual CefBaseRefCounted {
 public:
  ///
  /// "unavailable", "named-writers-only", "data-writes-denied" or "read-only-view".
  ///
  /*--cef()--*/
  virtual CefString GetFilesystem() = 0;
  ///
  /// Actual pre-engine/bootstrap and registered runtime coverage, without URLs.
  ///
  /*--cef()--*/
  virtual CefString GetProcessCoverage() = 0;
  ///
  /// "unavailable", "disk-possible" or verified RAM backing. RAM may swap.
  ///
  /*--cef()--*/
  virtual CefString GetScratchBacking() = 0;
  ///
  /// Actual Chromium launch route, not a blanket renderer enforcement claim.
  ///
  /*--cef()--*/
  virtual CefString GetSandbox() = 0;
  ///
  /// Static limits and unavailable boundaries, never browsing activity.
  ///
  /*--cef()--*/
  virtual CefString GetLimits() = 0;
};
#endif
#endif  // ALLOY_INCLUDE_ALLOY_GUEST_PROTECTION_H_
