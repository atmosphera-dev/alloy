// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#pragma once
#include "include/cef_base.h"
#include "include/cef_values.h"
#include <vector>
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
class AlloyGuestViewHandler;
///
/// EXPERIMENTAL: One pending attachment of the actual window.open WebContents.
/// Its opener and WindowProxy are preserved, never cancelled and recreated by
/// URL. Until attached it stays windowless/unpresented; it may run page script while
/// awaiting app placement. Navigation/close revokes the original opener owner.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestPopup : public virtual CefBaseRefCounted {
 public:
  ///
  /// Return the session-local pending child view ID, not a native handle.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Attach once with explicit DIP geometry/scale and the new view's handler.
  /// OnReady supplies the preserved child view, whose stream is independently
  /// started by the app. False means unqueued/stale, never URL recreation.
  ///
  /*--cef()--*/
  virtual bool Attach(int width, int height, double scale,
                       CefRefPtr<AlloyGuestViewHandler> handler) = 0;
  ///
  /// Refuse attachment and close that exact pending child. Idempotent. Dropping
  /// this object alone is not an implicit decision or a timeout policy.
  ///
  /*--cef()--*/
  virtual void Deny() = 0;
};
///
/// EXPERIMENTAL: Copied live dispatch receipt, not retained engine history and
/// not proof of a DOM effect. App and agent actions use this same native path.
/// The app selects text/password redaction and retention.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestInteraction : public virtual CefBaseRefCounted {
 public:
  ///
  /// Return the ordered view-local event ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Return the original caller source, for example "app" or "agent".
  ///
  /*--cef()--*/
  virtual CefString GetSource() = 0;
  ///
  /// Return the typed operation name, including pointer, wheel, key, touch,
  /// focus, capture-lost and the ime-* operations.
  ///
  /*--cef()--*/
  virtual CefString GetKind() = 0;
  ///
  /// Return the original target view ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetViewId() = 0;
  ///
  /// Return the original target document ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Return the original target geometry generation.
  ///
  /*--cef()--*/
  virtual uint64_t GetGeneration() = 0;
  ///
  /// Return the original target surface, zero for view or one for popup.
  ///
  /*--cef()--*/
  virtual uint64_t GetSurface() = 0;
  ///
  /// Return the unchanged app capture timestamp, on the app's own clock.
  ///
  /*--cef()--*/
  virtual uint64_t GetAppCaptureTimeUs() = 0;
  ///
  /// Return session-monotonic microseconds at actual native dispatch/refusal.
  /// Zero is used when a locally cancelled call has no guest timestamp.
  ///
  /*--cef()--*/
  virtual uint64_t GetDispatchTimeUs() = 0;
  ///
  /// Return accepted, rejected:<reason>, unavailable or cancelled-unobserved.
  /// Dispatch acceptance does not promise the page used or responded to it.
  ///
  /*--cef()--*/
  virtual CefString GetOutcome() = 0;
  ///
  /// Return an independent copied dictionary of the actual typed fields: DIP
  /// coordinates, deltas, modifiers, key/touch values or UTF-16 text/ranges.
  /// This is semantic app data, not the private transport representation.
  ///
  /*--cef()--*/
  virtual CefRefPtr<CefDictionaryValue> GetDetails() = 0;
};
///
/// EXPERIMENTAL: One immutable full BGRA8, premultiplied-alpha, sRGB CPU picture.
/// Its upper-left origin, stride and dimensions are in physical pixels. Retain
/// independent references for presentation and recording; the producer cannot
/// reuse its shared-memory slot until the last reference is released. ReadPixels
/// is thread-safe, including after view/session close. No native FD is identity.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestFrame : public virtual CefBaseRefCounted {
 public:
  ///
  /// Return the session-local view ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetViewId() = 0;
  ///
  /// Return the original native document ID (zero for the initial blank page).
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Return the stream geometry generation. Resize, navigation and recovery
  /// retire it; input against a different generation is rejected.
  ///
  /*--cef()--*/
  virtual uint64_t GetGeneration() = 0;
  ///
  /// Return zero for the view or one for its separate PET_POPUP surface.
  ///
  /*--cef()--*/
  virtual uint64_t GetSurface() = 0;
  ///
  /// Return the capture sequence for this surface, including skipped paints.
  ///
  /*--cef()--*/
  virtual uint64_t GetSequence() = 0;
  ///
  /// Return the cumulative dropped new frames on this surface.
  ///
  /*--cef()--*/
  virtual uint64_t GetDroppedCount() = 0;
  ///
  /// Session-monotonic microseconds at callback capture. CPU paint has no source
  /// video timestamp. This is not app/guest clock synchronization or A/V sync.
  ///
  /*--cef()--*/
  virtual uint64_t GetCaptureTimeUs() = 0;
  ///
  /// Return physical pixel width.
  ///
  /*--cef()--*/
  virtual int GetWidth() = 0;
  ///
  /// Return physical pixel height.
  ///
  /*--cef()--*/
  virtual int GetHeight() = 0;
  ///
  /// Return bytes per row.
  ///
  /*--cef()--*/
  virtual size_t GetStride() = 0;
  ///
  /// Return the full picture size in bytes.
  ///
  /*--cef()--*/
  virtual size_t GetByteSize() = 0;
  ///
  /// Copy up to |buffer_size| bytes starting at |offset| into |buffer|. Return
  /// bytes copied, or zero for an invalid buffer/offset. Shared pixels remain
  /// immutable for the entire frame lifetime, not just the callback.
  ///
  /*--cef()--*/
  virtual size_t ReadPixels(void* buffer, size_t buffer_size, size_t offset) = 0;
};

///
/// EXPERIMENTAL: Stream callbacks on a dedicated serialized stream sequence,
/// separate from the session control sequence. Callbacks may reenter methods
/// and release leases. A slow consumer drops new pictures, never blocks paint.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestStreamHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// A geometry generation was configured. Width/height are DIP; scale maps
  /// view DIP to physical pixels. The app owns its presentation transform.
  ///
  /*--cef()--*/
  virtual void OnConfigured(uint64_t generation, int width, int height,
                            double scale) = 0;
  ///
  /// A full independently usable picture, including recovery after any gap.
  /// Retain the frame to use it after this callback; no history is retained.
  ///
  /*--cef()--*/
  virtual void OnFrame(CefRefPtr<AlloyGuestFrame> frame) = 0;
  ///
  /// One live receipt for an input operation admitted by the stream endpoint.
  /// No duplicate local/native echo and no retained engine interaction history.
  ///
  /*--cef()--*/
  virtual void OnInteraction(CefRefPtr<AlloyGuestInteraction> interaction) = 0;
  ///
  /// PET_POPUP visibility/geometry in view DIP. Its separate pixels arrive as
  /// surface one; each show/hide/geometry change retires the geometry generation.
  ///
  /*--cef()--*/
  virtual void OnPopup(bool visible, const CefRect& bounds, uint64_t generation) = 0;
  ///
  /// App placement decision for a real window.open child. Metadata is copied
  /// from the original native request. No native popup window is created.
  ///
  /*--cef(optional_param=target_url,optional_param=frame_name)--*/
  virtual void OnWindowOpen(CefRefPtr<AlloyGuestPopup> popup,
      const CefString& target_url, const CefString& frame_name, int disposition,
      bool user_gesture, const CefRect& suggested_bounds) = 0;
  ///
  /// An undecided child or its original opener was retired. The pending popup
  /// object cannot attach after this callback. Final OnClosed retires all.
  ///
  /*--cef()--*/
  virtual void OnWindowRetired(uint64_t id, const CefString& reason) = 0;
  ///
  /// Copied cursor shape. Custom pixels are full BGRA, never a native pointer.
  /// Hotspot is in image pixels; image_scale maps those pixels to DIP.
  ///
  /*--cef(optional_param=pixels)--*/
  virtual void OnCursor(int type, CefRefPtr<CefBinaryValue> pixels, int width,
                        int height, int hotspot_x, int hotspot_y, double image_scale,
                        uint64_t generation) = 0;
  ///
  /// UTF-16 composition/selection and per-code-unit caret bounds in view DIP.
  /// A trailing surrogate or combining unit may have zero-width bounds.
  /// Apply the same app presentation transform used for pictures and input.
  ///
  /*--cef(optional_param=bounds)--*/
  virtual void OnImeBounds(const CefRange& selection, const std::vector<CefRect>& bounds,
                           uint64_t generation) = 0;
  ///
  /// Explicit unavailable/invalid-operation result, never a native fallback.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
  ///
  /// Final stream callback. Leased frames may outlive it. Session OnClosed
  /// follows retirement and completion of stream callbacks.
  ///
  /*--cef()--*/
  virtual void OnClosed() = 0;
};

///
/// EXPERIMENTAL: Typed view-stream endpoint, obtained from AlloyGuestView.
/// Methods are thread-safe and asynchronous; true means queued, not successful.
/// Geometry settings are mutable. Session security/storage settings are not.
/// No native window, codec or app-side CefInitialize is required.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestStream : public virtual CefBaseRefCounted {
 public:
  ///
  /// Attach once per view. The app selects |frame_slots| and |byte_budget| for
  /// all live CPU extents, including retired resize generations. Exhaustion
  /// drops new frames with explicit gaps. No disk spill or engine-chosen wait.
  ///
  /*--cef()--*/
  virtual bool Start(CefRefPtr<AlloyGuestStreamHandler> handler,
                     uint32_t frame_slots, uint64_t byte_budget) = 0;
  ///
  /// Queue new DIP geometry and explicit maximum paint cadence. Positive finite
  /// scale and physical dimensions must fit native integer/byte bounds. Cadence
  /// uses CEF's supported 1-60 range; CPU transport makes no 1080p60 promise.
  /// Old geometry input is rejected after OnConfigured for the new generation.
  ///
  /*--cef()--*/
  virtual bool Configure(int width, int height, double scale,
                         int frames_per_second) = 0;
  ///
  /// Queue a pointer action (move, leave, down or up). Coordinates are DIP local
  /// to the target surface. Document and geometry must still match. All Send*
  /// methods return an ordered event ID, or zero when unqueued; OnInteraction
  /// supplies the actual dispatch outcome. capture_time_us is kept unchanged.
  ///
  /*--cef()--*/
  virtual uint64_t SendPointer(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t surface, int x, int y, const CefString& action,
      int button, int click_count, uint32_t modifiers, uint64_t capture_time_us) = 0;
  ///
  /// Queue a wheel action, with CEF-native DIP delta values and surface-local
  /// coordinates. The app owns presentation transforms, not Alloy.
  ///
  /*--cef()--*/
  virtual uint64_t SendWheel(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t surface, int x, int y, int delta_x, int delta_y,
      uint32_t modifiers, uint64_t capture_time_us) = 0;
  ///
  /// Queue the native key fields; character values are UTF-16 code units.
  ///
  /*--cef()--*/
  virtual uint64_t SendKey(const CefString& source, uint64_t document,
      uint64_t generation, const CefKeyEvent& event, uint64_t capture_time_us) = 0;
  ///
  /// Queue native touch fields. CEF supports 16 simultaneous contacts; invalid
  /// IDs, pressure/radii, ordering and stale geometry are explicitly rejected.
  ///
  /*--cef()--*/
  virtual uint64_t SendTouch(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t surface, const CefTouchEvent& event,
      uint64_t capture_time_us) = 0;
  ///
  /// Queue native focus activation/deactivation.
  ///
  /*--cef()--*/
  virtual uint64_t SendFocus(const CefString& source, uint64_t document,
      uint64_t generation, bool focused, uint64_t capture_time_us) = 0;
  ///
  /// Queue native capture loss.
  ///
  /*--cef()--*/
  virtual uint64_t SendCaptureLost(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t capture_time_us) = 0;
  ///
  /// Begin/update app-owned native IME composition. Ranges count UTF-16 code
  /// units; (-1,-1) is the absent replacement range. Underlines and selection
  /// are copied/validated, not replaced by a JavaScript text shim.
  ///
  /*--cef(optional_param=text,optional_param=underlines)--*/
  virtual uint64_t SetComposition(const CefString& source, uint64_t document,
      uint64_t generation, const CefString& text,
      const std::vector<CefCompositionUnderline>& underlines,
      const CefRange& replacement, const CefRange& selection,
      uint64_t capture_time_us) = 0;
  ///
  /// Commit text through the native IME owner, preserving UTF-16 text/ranges.
  /// Relative cursor position follows CEF semantics (used on macOS).
  ///
  /*--cef(optional_param=text)--*/
  virtual uint64_t CommitText(const CefString& source, uint64_t document,
      uint64_t generation, const CefString& text, const CefRange& replacement,
      int relative_cursor_position, uint64_t capture_time_us) = 0;
  ///
  /// Finish the native composition, optionally retaining selection.
  ///
  /*--cef()--*/
  virtual uint64_t FinishComposition(const CefString& source, uint64_t document,
      uint64_t generation, bool keep_selection, uint64_t capture_time_us) = 0;
  ///
  /// Cancel/discard the native composition.
  ///
  /*--cef()--*/
  virtual uint64_t CancelComposition(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t capture_time_us) = 0;
};
#endif
#endif
