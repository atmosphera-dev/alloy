<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental guest sessions

M1 is the process foundation, not completed guest mode or write containment.
Its Linux backend reports filesystem enforcement **unavailable** and scratch
**disk-possible**. M3 adds the experimental CPU stream capability described in
`guest-streaming.md`; later media/native connectors remain unavailable. An app
must explicitly request and accept developmental unavailable enforcement to
activate this foundation. Required protection that is unavailable refuses
admission; it never selects an in-process or weaker-session fallback.

The app keeps control of policy. Existing `Cef*` APIs and in-process contexts
keep their behavior. Starting a session does not call `CefInitialize`, alter
the app environment, or apply restrictions to the app process.

## SDK and startup

Include `include/alloy/alloy_guest_session.h`. Define
`CEF_API_VERSION=CEF_EXPERIMENTAL` and use the generated C headers and C++
wrapper from the same output as `libcef`. New declarations use `Alloy*`;
CEF's translator generates the C API and counted-reference wrappers.
Before using a standalone session client, select and verify the native API hash
with `cef_api_hash(CEF_API_VERSION, 0)` and `CEF_API_HASH_PLATFORM`. Normal CEF
startup also does this, but is not required by the guest-session client.

Bundle `alloy_guest_host` (static Linux entry), `alloy_guest_engine`, `libcef`
and CEF resources together. The library resolves only its sibling bootstrap;
there is no caller-supplied executable, shell command, public CDP listener or
discoverable session socket. The translated `AlloyRunGuestHost` entry is for
the bundled engine, not app initialization.

Create immutable settings with `AlloyCreateGuestSettings`: explicit memory or
stateless mode, KEEP/CUT working network state, minimum protection, and optional
read-only list data. CUT is stateless-only. No encrypted-profile root or key is
accepted. Unsupported platforms return unavailable through null factories;
macOS/Windows backend parity is not claimed.

For the developmental foundation, settings can be created as follows:

```cpp
auto settings = AlloyCreateGuestSettings("memory", "keep", "unavailable",
                                          true, "");
auto session = AlloyStartGuestSession(settings, handler);
```

The handler's `OnPrepared(session, protection, capabilities)` inspects the
copied report and explicitly calls `session->Accept()` or `session->Close()`.
Only the `ACTIVE` state callback acknowledges acceptance. No requested URL
is loaded before acceptance. From ACTIVE, `CreateView(width, height, handler)`
queues a headless viewport; its `OnReady` callback supplies an `AlloyGuestView`
whose `Navigate` method uses CEF's URL/scheme admission. This is not a remote `CefBrowser`
facade. Multiple views use one context; different sessions use different
contexts and browser crash groups.

Invalid URLs return false from Navigate before admission; they do not change
the live document or emit a load-completion callback for it. Native URL
normalization is shared with CEF, including bare-hostname scheme insertion.

## Sequences, ownership and shutdown

| # | Contract | Behavior |
|---|---|---|
| 1 | Lifecycle | STARTING, PREPARED, ACTIVE, CLOSING, CLOSED; failures are explicit |
| 2 | Callbacks | One dedicated serialized client sequence per session, independent of app-side CEF UI |
| 3 | Queued methods | True means queued, not completed; callbacks observe actual creation, page title/load or terminal retirement |
| 4 | Reentrancy | Callbacks may close/release owners; closing synchronously stops new client admission |
| 5 | Identity | Session, view, native document, request and stream generation remain distinct; IDs are not authority |
| 6 | Close | Graceful: cancel view owners, run native browser close (page handlers may delay), then wait for supervisor teardown/reaping |
| 7 | Failure | Browser death ends the session; renderer/GPU recovery uses fresh generations under unchanged protection |
| 8 | Final callback | View cancellation/retirement precedes session OnClosed; no callback resurrects a retired owner |
| 9 | Terminate | Forced and idempotent in any state, including pending Close; skip page handlers, reap owned children, and deliver the same single final completion; no-op after CLOSED |

Alloy selects no close timeout. An app that wants timed escalation calls
`Terminate()` itself. Termination disconnects supervision from the calling thread,
so a stalled native page or control consumer cannot prevent process teardown.

Retain objects needed beyond a callback. In native C, each refptr callback
argument transfers one counted reference; release it, including session,
protection, capabilities and view arguments. Release returned settings/session
references and free userfree strings with `cef_string_userfree_free`.
Avoid app-created handler/session cycles; release app references on completion.

Titles and completion values are live copied events. Alloy adds no request,
blocked-attempt or interaction history. Test consumers retain only synthetic
observations under their explicit evidence directories.

## Process and extension boundary

The app uses `posix_spawn`. Its fixed static entry closes ungranted inherited
handles and stdio, sanitizes loader/plugin/desktop environment, registers the
runtime with the supervisor, and seals actual pre-engine facts before the fixed
engine exec. This is a startup fact, not proof of write denial.

The supervisor obtains kernel sender credentials and pidfds before acknowledging
each entry. Native exec and zygote-fork routes preserve registration capability
and obtain fresh liveness endpoints. Peer endpoints do not leak into descendants.
Threadless Linux liveness notification preserves Chromium's single-threaded
sandbox setup; parent-death signals are supplementary, not the cleanup authority.
Close and runtime admission share one supervisor sequence.

The control envelope negotiates protocol, build, experimental API hash and
feature availability. It uses explicit byte order, exact lengths and ordered
sequence numbers, not native C++ layout. Logical control messages are chunked
independently of socket packet size and accommodate Chromium's URL limit.
The host yields its native producer batch when control output is queued and
admits another batch or command only after that output drains. Title state
coalesces per original owner; reliable control remains ordered. A stalled
consumer neither grows an unbounded event queue nor triggers a quota failure.
Unexpected control handles are closed and rejected. The inherited resource lane
belongs to the same session; later resource transport does not replace control.

Separate GN/provider owners exist for `linux_lock`, `stream`, `media`,
`integration`, `accessibility`, `namespace_lock` and `capture`. Empty providers
return unavailable. Injected provider completions return to the owner sequence,
validate original ownership, and complete once or are explicitly cancelled.
No default provider calls desktop services. These extension slots do not claim
that M1 already contains arbitrary OS IPC/device effects or implements native
clipboard, audio, file, capture or accessibility adapters.

The normal Chromium sandbox remains enabled. There is no `--no-sandbox`
fallback. Display-server-free headless Ozone is used by the foundation consumers;
CPU startup is not GPU transport or 1080p60 evidence. Scratch, swap, external
services, metadata and persistence limits remain in the copied protection report.

## Verification

The M1 execution plan names DigitalOcean `alloy-gm-m1`, task `gm-m1`, allocated
in sfo2 under addendum 01. All commands run there, from
`/home/builder/work/alloy`, with `TMPDIR=/home/builder/work/tmp`:

```sh
python3 repo/tools/alloy_build.py --config verify --target cefclient --target alloy_guest_host --target alloy_guest_session_unittests
src/out/Verify_Alloy/alloy_guest_session_unittests --test-launcher-retry-limit=0
python3 -m unittest discover -s repo/tools/tests -p test_guest_session_probe.py -v
python3 repo/tools/guest_session_probe.py --out src/out/Verify_Alloy --check foundation --evidence evidence/guest-mode/m1/foundation-01
python3 repo/tools/guest_session_probe.py --out src/out/Verify_Alloy --check smoke --evidence evidence/guest-mode/m1/smoke-01
python3 repo/tools/guest_session_probe.py --out src/out/Verify_Alloy --check blocking --alloy-data-dir repo/tools/testdata/alloy-data --evidence evidence/guest-mode/m1/blocking-01
```

Use new evidence paths on each attempt. The checks require their respective
`GUEST_FOUNDATION_PASS`, `GUEST_SMOKE_PASS` and `GUEST_BLOCKING_PASS` markers and
complete result files, not a running process. The PR repeats the complete checks
against Release. The blocking fixture reuses committed rules/query data, observes
actual guest page outcomes, and uses a locally served unblocked counterpart so
network failure cannot masquerade as tracker blocking. It uses the original
HTTPS tracker URLs on loopback port 443 and a generated certificate trusted only
in the fixture's private NSS input. A short guest-sudo helper binds that port
and transfers the socket to the unprivileged test server; no root browser or
network configuration change is involved. It restores its exact
temporary guest `/etc/hosts` fixture block and never invokes legacy `/tmp`-writing
display helpers or changes legacy verification gates.
