// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_ACCESSIBILITY_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_ACCESSIBILITY_H_
#pragma once
#include "include/cef_base.h"
#include "include/cef_values.h"
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
///
/// EXPERIMENTAL: Portable accessibility updates on a dedicated serialized client
/// sequence. The app owns native platform objects and its presentation transform.
/// Values are independent copies; Alloy retains no event or interaction history.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestAccessibilityHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Ordered semantic update. Snapshot replaces all trees; delta replaces only
  /// listed frame trees and removes listed retired tree IDs. See
  /// docs/guest-accessibility.md for the portable dictionary schema. Bounds are
  /// root-view DIP, never app screen coordinates. A sequence gap automatically
  /// requests a full snapshot before another delta is delivered.
  /// While a handler is slow, pending updates coalesce into a complete current
  /// snapshot with an explicit coalesced_updates count; intermediate semantic
  /// events may be omitted. The separate interaction feed does not coalesce.
  ///
  /*--cef()--*/
  virtual void OnUpdate(CefRefPtr<CefDictionaryValue> update) = 0;
  ///
  /// Explicit AX connector unavailability on the serialized client sequence,
  /// never native accessibility objects in the guest.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
  ///
  /// Final callback after queued old-owner updates have been retired. A retained
  /// endpoint cannot admit further actions. Session OnClosed follows this callback.
  ///
  /*--cef()--*/
  virtual void OnClosed() = 0;
};
///
/// EXPERIMENTAL: App-owned screen-reader connector. No guest-native accessible
/// window, system bus, toolkit dependency or JavaScript action emulation.
/// Methods are thread-safe/asynchronous; action IDs share the stream interaction
/// feed and allocator with pointer, key, IME and other extension producers.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestAccessibility : public virtual CefBaseRefCounted {
 public:
  ///
  /// Attach the app's handler once to this view and enable native TreeOnly AX.
  /// False means unqueued. The existing session must already be ACTIVE.
  ///
  /*--cef()--*/
  virtual bool Start(CefRefPtr<AlloyGuestAccessibilityHandler> handler) = 0;
  ///
  /// Request a complete current snapshot, for example after an app bridge reset.
  /// No replacement document or native platform object is created.
  ///
  /*--cef()--*/
  virtual bool Resync() = 0;
  ///
  /// Queue a typed native AX action against the exact original document,
  /// geometry, frame-tree incarnation and node incarnation supplied by OnUpdate.
  /// Action is "default", "focus", "blur", "expand", "collapse", "increment",
  /// "decrement", "scroll-visible", "scroll-up", "scroll-down", "scroll-left",
  /// "scroll-right", "scroll-forward", "scroll-backward", "set-value" or "selection".
  /// Value is used only by set-value; anchor/focus are UTF-16 code-unit offsets
  /// used only by selection. Return a shared interaction ID, or zero if unqueued.
  /// The stream OnInteraction reports native acceptance/refusal and timestamps;
  /// acceptance is not proof that the page changed. Retired nodes cannot act on
  /// replacements, even when Chromium reuses a numeric node ID.
  ///
  /*--cef(optional_param=value)--*/
  virtual uint64_t SendAction(const CefString& source, uint64_t document,
      uint64_t geometry, const CefString& tree, uint64_t tree_generation,
      int node, uint64_t node_generation, const CefString& action,
      const CefString& value, int anchor, int focus, uint64_t capture_time_us) = 0;
};
#endif
#endif
