// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_STREAM_H_
#pragma once
#include "include/cef_base.h"
#include <vector>
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
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
};
#endif
#endif
