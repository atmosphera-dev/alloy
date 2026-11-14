<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental guest accessibility

After ACTIVE and view OnReady, obtain `view->GetAccessibility()` and call
`Start(handler)`. Availability is a separate `accessibility` capability, not a
filesystem-protection claim. No app-side CefInitialize or UI toolkit is needed.
The guest enables CEF's off-screen TreeOnly mode; it does not expose native
accessible windows or connect to a screen reader's desktop bus.

The app provides AT-SPI, UIA or NSAccessibility objects. Alloy provides native
tree data and native actions, not an OS integration service. The Linux reference
under `tools/guest_test_support/accessibility/` is an app-side test consumer;
neither it nor its dependencies are linked into Alloy.

## Ordered data and recovery

OnUpdate runs on a dedicated serialized AX client sequence. Dictionaries are
independent copies. A snapshot replaces the app's entire view forest. A delta
replaces the listed frame subtrees and removes the listed retired tree IDs.
Replacement subtrees avoid making apps implement Chromium's internal mutation
algorithm. Derived bounds can change across frame boundaries, so a delta may
replace several or all current frame subtrees. This is functional transport,
not a minimal-bandwidth claim.

| # | Update field | Meaning |
|---|---|---|
| 1 | `sequence`, `document`, `geometry`, `capture_us` | Decimal uint64 strings, without floating-point precision loss. Capture time uses the shared session clock. |
| 2 | `snapshot`, `scale` | Complete-forest replacement flag and M3's native device scale. |
| 3 | `trees`, `removed` | Replacement frame-tree dictionaries and retired tree-ID strings. |
| 4 | `events` | Native semantic/focus events: tree, node, kind and native action-request ID. Events are live, not retained history. |
| 5 | `root_tree` | The primary native frame's AX tree ID. Only that tree's focused-frame selection owns global focus; other frames can retain an old local focus. |
| 6 | `view_focused` | Actual native accessibility-view focus. A remembered local DOM focus is not an active platform focus while the view is blurred. |

Each tree contains `id`, decimal-string `generation`, `parent`, `focused_tree`,
`root`, `focus`, `loaded`, native selection object IDs/UTF-16 offsets and
`nodes`. Each node contains its numeric `id`, decimal-string incarnation
`generation`, `role`, `name`, `value`, `description`, `ignored`, `states`,
`actions`, `children`, `child_tree`, `attributes`, `bounds_available` and
`bounds` (`[x,y,width,height]`). Role/state/action and attribute names retain
Chromium's portable semantic enum names, not toolkit objects or C++ layouts.
Node IDs retain their signed 32-bit representation: generated renderer nodes
use negative IDs on this pin. Zero alone denotes an invalid node.

The client validates counters, values, finite rectangles, ownership and tree
relationships before delivery. Semantic updates larger than one control
envelope travel in checked parts on the existing ordered extension route and
are delivered atomically. A missing part or sequence gap invalidates the cached
model and requests a complete snapshot; deltas cannot leave it permanently
stale. Apps that reset their own bridge can explicitly call `Resync()`.

A slow AX handler does not build an unbounded history queue. Pending updates
coalesce into one complete current snapshot with decimal-string
`coalesced_updates` describing skipped pending updates. Intermediate semantic
events may be omitted during coalescing; the latest native tree/focus remains
complete. This follows the foundation's current-title projection pattern.
M3's separate actual-interaction receipts are not coalesced or dropped here.

Tree and node incarnations are independent of geometry and document IDs.
Deleting a node retires its incarnation even if Chromium reuses its numeric
ID. Navigation, frame retirement, renderer recovery and closure retire the
original owners before new actions can be admitted. Reentrant handlers may
close/release endpoints; closing never authorizes a callback to resurrect one.

## Coordinates and focus

Bounds are root-view-relative DIP after native offset-container, scroll and
frame transforms. An unavailable parent transform is explicit through
`bounds_available=false`, not a fabricated screen position. Apply the same
app presentation transform used for M3 pictures/input: device scale followed
by the app's own view-to-screen placement. Do not apply scroll twice or invent
an independent AX scale/origin. Native popup geometry changes share M3's
geometry generation; actions against an older generation are refused.
The Linux pin's native AX layout bounds are pixels. Alloy applies M3's inverse
scale once before publishing DIP. Geometry changes retire the AX baseline;
bounds remain unavailable until the native root viewport matches M3's actual
pixel dimensions, rather than relabeling old-scale data as new geometry.

Tree focus follows the actual native page/frame. A bridge focus request sends
a native AX action; it does not immediately manufacture a focused state.
AT-SPI text offsets count Unicode code points, while native selection actions
count UTF-16 code units. The app bridge owns that platform conversion.

## Actions and interactions

`SendAction` accepts `default`, `focus`, `blur`, `expand`, `collapse`,
`increment`, `decrement`, `scroll-visible`, the six `scroll-*` directions,
`set-value` and `selection`, with the exact original document, geometry, tree incarnation and
node incarnation from an update. No JavaScript click/focus/value emulation is
used. Chromium's owning RenderFrameHost performs the action after native
membership, live-frame and current-node checks. Retired trees, other views and
replacement documents cannot be reached through an old node identity.

The return value is the session's shared per-view interaction ID, or zero when
unqueued. Start the view's stream handler to receive its existing OnInteraction
feed. AX actions have kind `ax`, original source/capture time, actual session
dispatch/refusal time, typed target details and accepted/rejected outcome.
The same allocator/feed serves pointer, key, IME and other extensions. A local
retirement with no observed native receipt remains `cancelled-unobserved`.
Native dispatch acceptance alone is not proof of a page effect.

The app chooses retention and text/password redaction. Production Alloy stores
no completed interaction, accessibility-event or browsing history in this
connector. Synthetic acceptance fixtures explicitly capture app-side results.

## Verification boundary

`tools/guest_accessibility_probe.py --check tree-actions` exercises the public
SDK against native fixture trees/actions. `--check all` also starts an app-owned
private AT-SPI registry, reference bridge and the installed, unmodified Orca
core. Its modifier initialization uses a private authenticated app-test display
with filesystem sockets/lock files disabled and non-loopback access blocked;
the guest browser receives no display capability. Orca's script
selection/traversal, focus/action utilities and speech
generator consume the actual app objects; a speech-server extension captures
the synthetic utterances on the app side. The browser child receives no
accessibility-bus capability. The windowless driver does not claim desktop
keyboard/device integration, Windows UIA or macOS NSAccessibility support.

Run both Verify and Release in the milestone's assigned cloud guest, together
with native AX/stream tests and guest smoke/blocking. Protected acceptance
remains pending until M2 is merged; developmental checks explicitly accept
main's unavailable filesystem enforcement and make no stronger claim.
