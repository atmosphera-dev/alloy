<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental guest native integration

`AlloyGuestView::GetIntegration()` supplies the integration endpoint under the
existing guest session. Call `Start(handler)` once per view after OnReady.
Availability is separate from the filesystem-protection report. The app does
not call CefInitialize or expose its OS service handles to the guest.

## Ownership and callback lifetime

Native adapters capture the original browser-owned frame/document and session
owner before offering a request. They recheck canonical document admission on
completion. Navigation, document replacement, close, provider loss and native
revocation retire pending requests; an old reply cannot authorize the replacement.
Geometry-sensitive menu/drag actions also retain their geometry generation.
Subframe navigation retires operation captures before commit, including a
synchronously pending paste that would otherwise delay the old renderer's
replacement. A canceled navigation permits fresh captures, never old replies.
Dropping the last reference to an unanswered typed request cancels it. Retain
the request while deciding or reading its data asynchronously.

Integration callbacks run on a serialized client sequence independent of guest
browser UI, session control and stream callbacks. Data reads complete there as
well. Already queued reads report failure after retirement; OnClosed follows
their completion. An app that blocks its own callback can delay its final
notification, not prevent native process teardown. Release handler/session
reference cycles when finished.

Data is copied in checked, acknowledged chunks over the existing ordered
extension route. A data object is scoped to its request, unlike a picture lease.
GetSize describes its extent; Read validates the requested range and returns
copied bytes asynchronously. Empty data is a successful zero-byte read with a
null binary value, not failure. No file path, native pointer or bare FD number
becomes authority across the session.

## Clipboard

Keyboard copy/paste, native menu editing, explicit Edit and Async Clipboard use
one document-scoped Chromium clipboard backend. Unowned clipboard access and
missing providers do not fall back to Ozone, a portal or a private guest clipboard.
The app owns OS access, clipboard contents, sequence tokens and permissions.
Clipboard permission decisions use the existing native document settings owner,
not another site-policy cache or a profile-wide replacement-document grant.
Existing temporary embedded scopes keep their canonical native settings and
service owner, including the app-selected sharing/retirement scope. The pending
request itself still belongs to its original document.
Sanitized-write decisions do not create persisted settings, matching Chromium's
native distinction from read/write clipboard grants.

Requests distinguish format enumeration, data reads, writes, sequence queries
and native permission queries. Buffers distinguish copy/paste from primary
selection. Sequence replies are nonzero 128-bit tokens encoded as 32 hexadecimal
digits; change the token when replacing that buffer. ClipboardChanged notifies
native listeners, which refresh through the same provider.

Standard text/HTML/SVG/RTF/PNG and opaque platform/custom formats preserve their
native encodings. Text and HTML bytes are UTF-8; HTML fragment offsets are UTF-16
code-unit offsets, checked against the decoded markup. CEF's native custom-format
map and DataTransfer serialization are transferred as opaque native formats.
The app never supplies a renderer frame token as clipboard authority. The
write request includes source URL and privacy flags for the app's handling.

Browser UI never waits synchronously on the app. Native renderer-main paste
keeps Chromium's synchronous mojom contract. Each app read/sequence request
expires empty after 10 seconds, using the pin's Linux incremental clipboard
transfer bound; retirement cancels it sooner. The pinned X11 requester and
Wayland reader do not themselves have a requester deadline, so this is an
explicit bound at the guest connector, not a claim about their read implementation.

Async Clipboard write promises keep Chromium's native fire-and-forget CommitWrite
semantics. They do not acknowledge OS clipboard storage. CompleteWrite reports
the app sink outcome through the shared live interaction feed; consumers that
need storage proof observe the actual app clipboard. A failed sink does not
retroactively rewrite the already-resolved native page promise.

## Menus, editing, drag and fullscreen

Menu requests copy CEF's model before callback return. Items carry native command
IDs, parent indexes, enabled/checked state and whether guest execution is
supported. The app presents the menu and either selects an enabled command,
marks an action handled on the app side, or cancels. Context-menu IDs are CEF
MENU_ID constants; quick-menu IDs are QM_EDITFLAG constants. Quick-menu Paste
availability reads the same captured document's app clipboard, not the denied
global backend; an old query cannot revive a closed or replaced menu. Native editing
retains the captured frame/document through both CEF queues and renderer IPC,
even if focus moves before the reply. Native UI-interaction bookkeeping does
not fabricate pointer/key input or install permission grants.
Pending menus also retain a renderer-owned editor/selection context behind an
internal opaque token. Native command execution consumes it in the original
document, rechecking after focus handlers; dismissal and retirement release it.

Drag target entry carries copied text, link and HTML, not native file paths.
Over/drop/leave use the same original document and view-DIP geometry. Native
effect feedback is not a new user action. Source requests copy text/link/HTML,
file content and a scale-one PNG drag image/hotspot before CEF releases its
callback data. Finish selects one permitted effect or none and completes both
native source-ended paths once. Navigation/geometry retirement cancels the
original drag rather than completing a new one.

Fullscreen requests are offered after native gesture and Permissions Policy
checks but before admission and renderer success. The app accepts/refuses its
presentation change, then replies; native completion rechecks original-document
admission and policy. FullscreenChanged reports the actual transition, including
page/user exit. ExitFullscreen is a live typed action. The app owns presentation
size and updates its stream viewport explicitly.

Edit, menu/drag/fullscreen decisions and clipboard sink outcomes use M3's shared
view-local IDs, source/capture time and session-clock dispatch/outcome receipts.
An accepted dispatch is not proof of a DOM effect. No interaction history or
clipboard content is written to product logs.

## Verification surface

The synthetic `alloy_guest_integration_consumer` uses the translated SDK without
app-side CEF initialization. `tools/guest_integration_probe.py --check ui`
compares final app clipboard data and native page state for editing/keyboard/
Async Clipboard, menu ownership, drag data/effects, fullscreen transitions and
pending-read retirement. `--check native-grants-unit` invokes the existing
native-grants binary. Run these only in the assigned build guest; evidence is
explicit synthetic fixture output, not production logging.

The file/upload/save/download adapter slice is not implemented in this draft.
Clipboard file capabilities are not yet materialized. `--check all` deliberately
fails until that slice and its byte/sink/revocation proofs are present; UI-only
results cannot stand in for complete M5 acceptance. Protected acceptance also
requires the merged Linux protection milestone.
