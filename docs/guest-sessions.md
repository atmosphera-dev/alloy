<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental guest sessions

Linux guest sessions apply pre-engine Landlock write refusal and verified RAM
scratch, with Chromium's user-namespace sandbox. See `guest-protection-linux.md`
for the actual limits, including deliberate same-user inspection and metadata.
M3 adds the experimental CPU stream capability described in `guest-streaming.md`;
native integration and accessibility have separate capability reports. Required
protection that is unavailable refuses admission; it never selects an in-process
or weaker-session fallback. No user namespaces means no guest activation.

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

For Landlock data-write refusal, settings can be created as follows:

```cpp
auto settings = AlloyCreateGuestSettings("memory", "keep", "data-writes-denied",
                                          false, "");
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

`Navigate` returns a nonzero, increasing request ID local to that view, or zero
if rejected. Invalid URLs are rejected before admission; they do not change the
live document or emit a completion. Native URL normalization is shared with CEF,
including bare-hostname scheme insertion.

A queued navigation belongs to its view, not a URL or stream generation. Each
accepted request gets exactly one `OnNavigationComplete(view, request, document,
status)`, including failure, cancellation by a newer request, or retirement on
close. A request cancelled before native start reports `ERR_ABORTED` (-3) and
document 0; it is not attributed to the current document. Native-initiated
loads retain their document reports with request 0. Title and frame events
remain keyed by document.
A navigation racing an external kill either loads or completes with
`ALLOY_GUEST_NAVIGATION_RENDERER_CRASHED` (-10000), distinguishable from native
app/user cancellation (`ERR_ABORTED`, -3). The constant is declared in
`include/internal/alloy_guest_navigation_types.h` for C and C++ consumers.
The app receives an actual outcome, not just the earlier queue acknowledgement.
There is no automatic retry: after a crash abort the next app-requested navigation
loads in a new renderer. A replacement document or closed view still retires
the original intent explicitly. No process-readiness check can serialize a
navigation with a kill sent outside the browser before that process exits.
Native document adoption is independent of that single-use outcome: a pending
cross-site renderer can still commit after the old primary crashes. Its matching
document, title and stream become live without a second completion callback.

## Sequences, ownership and shutdown

| # | Contract | Behavior |
|---|---|---|
| 1 | Lifecycle | STARTING, PREPARED, ACTIVE, CLOSING, CLOSED; failures are explicit |
| 2 | Callbacks | One dedicated serialized client sequence per session, independent of app-side CEF UI |
| 3 | Queued methods | True (or a nonzero navigation request ID) means queued, not completed; callbacks observe actual results |
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

The app uses a C-only
`clone3(CLONE_PIDFD|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNS)` adapter,
not a shell, a spawn-then-pin sequence or post-fork C++. The top static entry is
PID 1 in the guest enclosure. Its original pidfd exists atomically with the
process; its death makes Linux kill and reap every enclosure member, including
stopped processes and native forks that have not registered yet. The app's
signal mask, dispositions, child reaper and namespace membership stay unchanged.

Before its fixed exec, the enclosure child makes mount propagation private,
mounts procfs for its PID namespace and starts its own POSIX session. Real CEF
needs matching `/proc/<pid>` lookups for native renderer status; the inherited
outer procfs resolves those local numbers to unrelated processes. This does not
add a read-only filesystem view or IPC isolation, or expand proc write grants.

The enclosure also mounts its own 1 GiB tmpfs at its private `/dev/shm`, with
noexec, nosuid and nodev. HOME, XDG, CEF and temporary writer paths use that
mount; no `alloy-guest-*` directory is created in shared RAM or on disk. The
kernel reclaims the mount after the last reference ends, including app/init or
supervisor SIGKILL. Cleanup does not depend on a supervisor walking directories.
This is not a same-user tenant boundary: a program deliberately inspecting
`/proc/<pid>/root` can read scratch and hold a reference that retains its memory
after the guest ends. The report discloses this observer limit; no dumpable or
extra isolation mechanism is selected.

The static init owns a real SIGKILL/waitid probe and a gated supervisor. READY
and the app's Close/Terminate share one locked admission decision. One dedicated
app-side custody thread performs capability checks, signals and final waits.
Close/Terminate post requests; the calling thread's later seccomp or signal
policy does not supply teardown authority. The custody thread remains responsive
while the callback worker is blocked, including when init or its supervisor is
stopped. Failed force attempts remain pending and retry. A refused final wait
retains the exact pidfd and retries without publishing completion or dropping
the reaping obligation. Failures remain recorded even after eventual cleanup.
Final completion waits for the original init's exit barrier, not control EOF or
the FINISHED receipt alone. App forks close inherited session handles without
shutting down the parent's endpoints. No product close timeout is selected.

Alloy's static children use the same clone3 pinned creation primitive. Native
Chromium namespace/zygote forks remain inside the kernel enclosure. The fixed
static entry closes ungranted inherited
handles and stdio, sanitizes loader/plugin/desktop environment, registers the
runtime with the supervisor, and seals actual pre-engine facts before the fixed
engine exec. The Linux lock adds actual write refusal and managed RAM paths;
bootstrap startup facts alone are not the enforcement proof.

The supervisor obtains kernel sender credentials, pins the process, then proves
the private registration peer is still live before acknowledging
each entry. Native exec and zygote-fork routes preserve registration capability
and obtain fresh liveness endpoints. Peer endpoints do not leak into descendants.
Threadless Linux liveness notification preserves Chromium's single-threaded
sandbox setup; parent-death signals are supplementary, not the cleanup authority.
Close and runtime admission share one supervisor sequence.

Before admission, refused clone3 flags, user-namespace ID mapping,
`pidfd_send_signal(SIGKILL)` or `waitid(P_PIDFD)` produce an unavailable report
naming the operation and errno. Resource exhaustion or a corrupt/failed launch
is a failure, not a successful cleanup or a weaker retry. No clone fallback or
newer `SO_PEERPIDFD` baseline is used. Flatpak, strict Snap and default Docker
seccomp do not currently support this route when their policies deny those
operations. Flatpak's parked adapter is not enabled by an environment marker.

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

Lifetime custody verification uses cloudbuild task `gm-lifetime`, guest
`alloy-gm-lifetime`, workspace `/home/builder/work/alloy`, with
`TMPDIR=/home/builder/work/tmp`. On that guest, after the matching build:

```sh
python3 repo/tools/guest_lifetime_probe.py --evidence evidence/lifetime-fixtures-new
python3 repo/tools/guest_lifetime_sdk_probe.py --out src/out/Verify_Alloy --evidence evidence/lifetime-sdk-new
python3 repo/tools/testdata/guest-lifetime/zygote_probe.py --out src/out/Verify_Alloy --evidence evidence/lifetime-zygote-new
python3 repo/tools/guest_scratch_probe.py --out src/out/Verify_Alloy --evidence evidence/lifetime-scratch-new
python3 repo/tools/guest_lock_probe.py --expected-hostname alloy-gm-lifetime --out src/out/Verify_Alloy --check all --evidence evidence/lifetime-lock-new
```

The lifetime matrices exercise default SIGCHLD, SIG_IGN, SA_NOCLDWAIT and an
app-owned reaper. Fixture-only PID reuse, seccomp filters and interposition do
not change the product launch path. The receive/fork test holds a newly received
pidfd before publication and verifies the app's fork closes it. The root-only
zygote tracer holds an actual Chromium newborn before registration, detaches it
in confirmed SIGSTOP, and retains its original pidfd across custodian loss.
Each invocation needs a new evidence path; repeat the SDK, zygote and existing
gates against Release as well.
The scratch matrix verifies ordinary mount invisibility and no remaining mount,
open-file or mapped-file reference after death, except its explicit same-user
observer cases. Those verify retention until the observer closes its handle or
unmaps the file, including a mapping whose descriptor was already closed.

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
