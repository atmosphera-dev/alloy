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
Read completion rechecks the app sequence before supplying data to native paste
policy. A mismatch returns no content, never new bytes labeled with the earlier
policy identity. Offered text aliases use the same mapping for listing and reads.

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
Captured selection offsets use exactly the native restoration scope, including
selections across inline nodes.

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

## Files, uploads and transactional sinks

`OnFileSelection` supplies a typed request for native open/save/directory,
upload, clipboard/drop files or a download destination. Reply with app-owned
`AlloyGuestFile` objects, never paths or native destination descriptors. File
getters and `OnOperation` run on the registering integration callback sequence.
Completion methods may be called from app I/O threads; metadata getters are
queued back to that sequence before sending the reply. A true return means queue
admission, not consumer completion. Invalid results retire the request.

Each app entry has an opaque `GetIdentifier()` identity (use
`AlloyCreateGuestFileIdentity()`) and canonical opaque ancestor identities, root
first. Preserve these when returning the same entry from a different picker or
directory. Logical names are basenames, not paths. Native validation rejects
cycles and contradictory ancestry. Repeated selections and directory-derived
handles share Chromium's ordinary file/directory lock hierarchy; app identities
do not create filesystem paths that the guest may open.

Native permission and grant objects still decide access. Selected capabilities
and issuing/receiving binding leases only add lifetime: navigation, provider loss,
close and revocation cancel pending work and cannot grant a replacement page.
Transfer tokens retain those native grants and leases. Serialized external
handles refer to the exact live native token, not persisted paths from which
new grants could be reconstructed. Retirement invalidates restored handles too.
Experimental platform filesystem observation is unsupported for these entries;
native external-file move/rename and OPFS-only sync handles keep their existing
native refusals. OPFS stays on its ordinary storage path.

File operations use bounded acknowledged byte chunks. `open-write` returns an
app transaction, with keep-existing/exclusive information. Reads use checked
offset/length and expected modification time. Writes and truncate affect staging;
`close` acknowledges actual app commit, and `abort` discards it. Release discarded
transaction resources when their app object is released, including after provider
loss. Failed saves/writes must preserve the app's prior committed data.

Path-only upload, paste and drop consumers receive native-created aliases to
sealed RAM snapshots. Native proof checks the exact immutable inode, private
RAM ancestry and original document before file grant delivery. App display names
and directory components stay separate from backing aliases, including repeated
or long basenames. Arbitrary chooser paths and native clipboard/portal services
are not fallbacks. Clipboard file materialization shares the pinned ten-second
clipboard read bound; ordinary file operations have no clipboard deadline.
`DragEnterFiles` requests typed drop files; wait for its normal interaction receipt
before over/drop. Clipboard writes and source drags expose selected app objects
through `GetFileCount`/`GetFile`, never OS path authority.
After handoff, native Blob file references retain snapshot backing through File
clones, slices, reads and multipart requests. The last consumer releases RAM
without waiting for navigation; original-document retirement still revokes it
immediately. Guest multipart bodies use the existing Blob-backed path rather
than detaching ownership into raw filename elements.
Unredeemed paste/drop tokens retain the selected app capability, not snapshot
backing. Their last clone releases it; redemption still uses native process,
document and permission checks.

Downloads consume the normal guarded native network stream. App chunk
acknowledgements precede native progress, and actual sink commit precedes native
completion. The native item retains uncommitted staging across interruption and
resume, verifies partial bytes/hash and overlapping ranges, and aborts on cancel,
destruction or original-document retirement. The app owns destination presentation
and OS open/reveal actions; guest mode does not initialize Chrome's download UI.
`GetDownloadId()` identifies the native item independently of its naming metadata.
`DownloadControl` pauses/resumes/cancels through the existing ordered interaction
route. `OnDownloadProgress` reports native preparing/active/paused/complete/
cancelled/interrupted state; sink queue admission alone never means complete.

## Verification surface

The synthetic `alloy_guest_integration_consumer` uses the translated SDK without
app-side CEF initialization. `tools/guest_integration_probe.py --check ui`
compares final app clipboard data and native page state for editing/keyboard/
Async Clipboard, menu ownership, drag data/effects, fullscreen transitions and
pending-read retirement. `--check native-grants-unit` invokes the existing
native-grants binary. Run these only in the assigned build guest; evidence is
explicit synthetic fixture output, not production logging.

`--check files` exercises native file reads/writes/directories/locks, upload bytes
at page and HTTP receiver, clipboard/drop handles, shared/serialized retirement,
large streams, asynchronous metadata replies and app download sinks/controls.
`--check all` combines these with the original integration checks. UI-only or
startup results cannot stand in for file acceptance. Protected acceptance also
requires the merged Linux protection milestone.
