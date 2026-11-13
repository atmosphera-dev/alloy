<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental guest streaming

`AlloyGuestView::GetStream()` returns a typed adapter under the existing guest
session. After ACTIVE and view OnReady, call `Start(handler, frame_slots,
byte_budget)` once, then `Configure(width_DIP, height_DIP, scale, fps)`. The app
chooses viewport, finite positive scale, CEF's supported 1-60 cadence and live
frame capacity. No app window, toolkit, codec or app-side CefInitialize is needed.
The stream capability is independent of filesystem protection and later media
capabilities. CPU streaming is not a YouTube 1080p60 performance claim.
Standalone SDK consumers first select/verify CEF's API version using
`cef_api_hash(CEF_API_VERSION, 0)` against `CEF_API_HASH_PLATFORM`; this needs
neither CefExecuteProcess nor CefInitialize in the app.

## CPU pictures and leases

OnFrame supplies an immutable, complete BGRA8 image with premultiplied alpha,
sRGB color and an upper-left origin. Width/height/stride are physical pixels;
view geometry is DIP. Every frame is independently usable: conservative damage
is the whole picture, so skipped paint sequences always recover with a full
frame rather than applying damage to the wrong image. Surface zero is the view;
surface one is reserved for its separate PET_POPUP surface.

The producer copies CEF's whole callback-scoped buffer before OnPaint returns.
It writes only an unleased pool slot. Extents and layout are checked for byte
overflow, and received extent size and kernel seals must match metadata before
mapping. A received memfd cannot be resized or written by a new mapping/pwrite.
Only the producer's existing mapping is writable, and it is never used while
the app retains a lease.

The public lease is the counted AlloyGuestFrame reference. Presentation and
recording retain independent references to the same immutable lease. The final
release unmaps the client pixels and asynchronously returns the exact original
view/document/native generation/token to the pool. Wrong owners, duplicate
releases and stale tokens cannot free a replacement slot. ReadPixels copies a
requested range to app memory; it remains valid after resize, view close and
session shutdown while that reference is retained. Closing destroys the
producer's mappings but does not invalidate the app's retained mappings.
Memfd and app pixel memory may still be swapped by the OS; this is not a physical
no-disk guarantee.

Capacity counts all live slots and bytes, including old geometry generations.
If no slot/byte credit is available or the resource socket is full, the producer
drops the new display frame instead of waiting in CEF. Capture sequence counts
attempted paints, not unique source pictures. GetDroppedCount reports cumulative
pool/send losses on that surface. These counts are not presentation or recorder
success. After a capacity release, a static page is invalidated if recovery is
needed. No disk spill, codec or app output handle is involved.

## Coordinates, epochs and time

OnConfigured reports DIP viewport and device scale with a fresh stream geometry
generation. Geometry changes, native navigation and renderer/GPU recovery retire
the old generation. This geometry generation is distinct from M1's native
document/stream owner identity; it never authorizes work against another owner.
An old-size in-flight native paint is not relabelled as the replacement geometry.
The app owns its presentation transform; native geometry maps view DIP to screen
pixels consistently through CefScreenInfo and GetScreenPoint.

GetCaptureTimeUs is session-monotonic microseconds at CPU callback capture.
CEF CPU paint has no original source-video timestamp. This field is not clock
synchronization, A/V sync or input-to-DOM effect proof. M4 must add reference-time
bridging and measured synchronization before making those claims.

## Session and extension ownership

All extension features use the same ordered call/reply/event envelope. Calls
carry the original native owner, feature slot and unique call ID; Providers
serializes completions, checks original ownership and cancels exactly once.
Replies carry session-monotonic microsecond completion times, distinct from
actual native-action dispatch or DOM effects; locally unsent cancellations use
zero because no guest dispatch occurred.
Multiple calls may share one native owner without conflating its navigation
request ID with extension call IDs. Missing providers report unavailable.

The generic reserved @release operation routes only to a provider's resource
retirement callback. It may outlive its native document, but the pool must match
the complete original owner and lease token. It cannot dispatch new content
actions. Later media/capture/AX/native adapters use these same extension slots
and lease semantics rather than adding a second session or an injection bypass.

The inherited resource lane carries explicitly encoded metadata and SCM_RIGHTS
under the same session identity, with its own exact ordered sequence. Raw FD
numbers and C++ layouts are never wire identity. A matching reliable control
announcement is required before a resource can reach the consumer; descriptors
arriving before native document admission cannot overtake it. Negotiated slot
and byte credits bound outstanding descriptors, queued pictures and app leases.

Session/view control callbacks remain on M1's dedicated serialized sequence.
Stream callbacks use a separate serialized sequence, so a stalled stream
handler does not stall control or native teardown. Retirement stops new stream
admission, drops queued old callbacks and gives one final stream OnClosed.
Session OnClosed waits for stream completion after owned processes are reaped.
An app that stalls its own callback may delay that final notification, not the
forced process teardown. Release retained session/handler cycles explicitly.

## Current verification scope

The CPU-only commit tests actual display-server-free headless Ozone output,
numbered RGB page draws versus client pixels, independent presentation/recorder
retention, pool exhaustion/gaps, full-frame recovery, resize/scale, retained
pixels after shutdown and callback-sequence separation. New targets are
`alloy_guest_stream_unittests` and `alloy_guest_stream_consumer`; run
`tools/guest_stream_probe.py --check frames` in the assigned build guest.

Input, popup placement/hit testing, cursor and IME are added by the next M3
commit. GPU, sound, clocks, native clipboard/files/capture and AX remain later
milestones. Nothing here changes memory/stateless behavior or production
request-logging policy; the consumer retains only explicit synthetic fixtures.
