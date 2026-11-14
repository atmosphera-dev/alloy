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

The full `--check all` driver also exercises native input, popup placement/hit
testing, cursor and IME through the same translated SDK. It compares receipts
with actual page text/state and rendered response pixels, not dispatch alone.
GPU, sound, clock synchronization, native clipboard/files/capture and AX remain
later milestones. Nothing here changes memory/stateless behavior or production
request-logging policy; the consumer retains only explicit synthetic fixtures.

## Native input and one live interaction feed

SendPointer, SendWheel, SendKey, SendTouch, SendFocus and SendCaptureLost use
the existing native browser-host methods. All require the original document
and geometry generation. Coordinates are DIP local to surface zero (view) or
one (PET_POPUP); the connector performs popup hit testing and translates its
local point to view DIP once. Stale generations/owners and invalid native
values/order are refused, never applied to replacement geometry. While a
navigation has no committed native document, input cannot reach the old page.
When a navigation ends without committing, its surviving native document is
admitted under the fresh session owner and streaming resumes; old owners stay
retired. Captured mouse/touch continuation and termination may leave the view
bounds, while new presses still require containment and matching owner/geometry.
Capture-loss notification follows pinned CEF's legacy widget/plugin-capture
semantics; it does not mean JavaScript Element.releasePointerCapture. DOM
pointer capture still follows the native pointer-event lifecycle. The connector
retires its pressed-button containment allowance when capture loss is dispatched.

Methods return an ordered view-local event ID, or zero when unqueued.
OnInteraction reports actual native accepted/rejected dispatch, original app
capture time/source and session-clock dispatch/refusal time. App and agent
actions use identical typed methods and native dispatch. The app chooses its
own source labels, redaction and retention; Alloy retains only pending work.
Acceptance is not proof of DOM effect or an exact first responding frame.
`cancelled-unobserved` makes an interrupted/unobserved dispatch explicit,
rather than claiming it succeeded or certainly did nothing.

The session's shared Interactions tracker allocates IDs atomically with command
admission for every feature, validates exact original-owner/feature receipts
and removes completion state before delivery. Native and generic completion
paths cannot produce duplicate echoes. Later menu and AX producers use
ExtensionEndpoint.interact, the same metadata codec and reserved @interaction
event, not a new injection path, ID allocator or history store. The existing
logical control-envelope bound is checked with receipt space reserved before
an action is admitted. Other providers remain unavailable until implemented.

## Popups and cursor

PET_POPUP has separate full CPU pictures, DIP geometry and hit testing.
Show/hide/geometry changes retire the geometry generation, so an old popup
point cannot act on a replacement popup. The app owns placement/compositing.

OnWindowOpen offers the actual CEF-created window.open WebContents. Its original
WindowProxy/opener, initial blank document and document.write state are kept;
there is no cancellation followed by URL recreation. A pending child remains
off-screen/unpresented, but may run page script while awaiting placement. This
is an app attachment decision, not a promise that script waits for approval.
The admitted session's existing native context and permission owners still
govern it; there is no second site-state authority.

AlloyGuestPopup.Attach selects DIP size/scale and adopts a view handler. OnReady
supplies the same child view; the app starts its stream independently. Native
offered views have a distinct ID namespace under the same Session so concurrent
app CreateView calls cannot collide. Deny closes that exact pending child.
Original opener retirement or child closure emits OnWindowRetired for an
undecided offer; stale attachment returns false. Final stream close retires all
pending offers. There is no engine-selected decision timeout.

OnCursor supplies a cursor enum or owned BGRA pixels, pixel hotspot and image
scale. Custom bytes are copied before the native callback returns; no native
cursor pointer crosses the session. The app owns the system cursor/UI.

## App-owned IME

The app handles its platform IME/dead keys and calls SetComposition, CommitText,
FinishComposition or CancelComposition. They reach native CefBrowserHost IME
methods, not JavaScript text injection. UTF-16 text, directed selection and
underline ranges/styles/colors are copied and validated without scalar/index
conversion; an unpaired surrogate is rejected before queuing. (-1,-1) denotes
an absent replacement range. Replacement/relative-cursor fields retain CEF's
platform semantics (macOS-only uses are not Linux parity claims).

OnImeBounds supplies the native UTF-16 composition range and per-code-unit
bounds in view DIP. A trailing surrogate or combining unit can have zero-width
bounds; it is not a missing glyph slot or a reason to discard the range.
Apply the same view-to-app/screen transform used by presentation and input.
Composition inputs and their accepted/rejected receipts join the common live
interaction feed, with no engine-retained typing/password history.
