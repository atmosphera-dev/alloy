<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Linux guest package entries

The app remains flexible within its package permissions. Guest sessions still
require pre-engine Landlock ABI 3+, verified RAM and a working Chromium sandbox.
There is no setuid helper, sandbox-disable flag or weaker retry in guest mode.
The native private namespace/read-only filesystem view is absent. Landlock's
metadata/atime, same-user procfs, existing-capability, IPC/service and swap limits
remain; see [Linux protection](guest-protection-linux.md).

## Native packages

Bundle the fixed static bootstrap, engine, libcef and resources together. Keep
the exact-path Ubuntu AppArmor template from `packaging/linux/` and its package
installation/removal behavior. An unprofiled Ubuntu install reports userns
unavailability before navigation; it does not switch to a helper. Native scratch
continues to use a fresh descriptor-pinned directory in verified /dev/shm.

## Experimental Flatpak layout

Bundle pinned zypak v2025.09 (`693a71c5ffa80ec9c9ce2ae03b1ccc493c698e53`) with
`tools/testdata/guest-packaging/zypak-alloy.patch`. Its bin/ and lib/ directories
go under `zypak/` beside the Alloy bootstrap and libcef. The static bootstrap
checks this fixed layout and supplies its own preload list only after write
enforcement. Arbitrary app loader/plugin environment is not copied.

The package declares `--filesystem=xdg-run/alloy-guest:create` and access to the
filtered `org.freedesktop.portal.Flatpak` spawn portal. Native guest integrations
still have no automatic desktop-service fallback. Access to this launch portal
is not a claim of an OS-enforced IPC firewall.

The supervisor creates fresh 0700 scratch under the shared user-runtime grant,
verifies tmpfs and pins its inode. Flatpak's private /dev/shm is not visible by
pathname in another sandbox; transferring its FD alone is insufficient. The
adapter explicitly transfers the shared RAM directory and Chromium launch
capabilities into each external fixed bootstrap. That entry establishes a new
Landlock domain before libcef, rather than trusting an inherited marker from a
different kernel lineage. Setup directory capabilities are closed before engine
entry. Durable app XDG directories and /var/tmp are not scratch exceptions.

The pinned patch puts dynamic launch values into the owning spawn request,
not zypak's borrowed string-view map, and retires attached message handles with
their message owner. Fixture builds check both lifetimes before packaging.

Flatpak's broker constructs the sandbox. The pinned zypak adapter preserves
Chromium's native helper handshake without invoking a setuid helper. The report
names `flatpak-brokered-userns`, not native in-process unshare. PREPARED still
waits for a real private blank renderer and observed NNP/seccomp/capabilities.
The kernel-credential/pidfd registration and per-process liveness binding apply
to external entries too; a namespace-init parent is not mistaken for supervisor
death. Scratch or control loss ends the session without a durable fallback.

Flatpak/bubblewrap and pinned unprivileged zypak setup helpers are trusted setup
exceptions without engine payload. Ordinary exec/zygote descendants inherit
Landlock. The report distinguishes those paths and does not advertise the
deferred namespace/read-only view.

## Snap guest mode is unsupported

A real strict local package with connected browser-support/allow-sandbox on
snapd 2.78 allowed pidfd_open but denied pidfd_send_signal to its own child with
EPERM. Alloy's identity-bound forced termination requires the latter. No Snap
coverage is claimed. Before launching any supervisor or engine, Alloy checks
the actual operation with signal 0 against its own pinned process identity.
A policy denial reports `unavailable` with the reason `process policy denies
pidfd_send_signal: identity-bound process teardown unavailable`. No features,
scratch or supervised runtime are claimed, and acceptance is closed even for
developmental settings. The check is not selected by a Snap environment marker:
the same refusal applies to a denial imposed by any inherited process policy.

There is no weaker teardown, numeric-PID fallback or devmode/classic substitute.
Snap support would require a separately verified identity-bound teardown
mechanism or a snapd policy change admitting the required syscall. This work
does neither and does not modify snapd policy.

## Verification status

Flatpak's static-bootstrap prerequisite has demonstrated external registration,
pidfd signaling, verified shared RAM writes and durable-sentinel write refusal.
Real installed Flatpak CEF feasibility also passed: the expected page/title and
HTTP 200, observed renderer sandbox controls, normal-close process exit and no
retained setup-directory capabilities. Verify and Release acceptance passed
actual Flatpak and native packages, cold defaults/death, crash outcomes and recovery,
smoke/blocking, flexible encrypted apps and missing RAM/helper/launch-IPC cases.
Navigation racing an external kill has one load or named crash-abort outcome;
the next app-requested navigation recovers, without an engine retry or sleep.

Negative RAM tests use a fresh package identity and assert actual carrier
absence: revoking a grant on a previously used app can leave usable private RAM.
Flatpak 1.14.6's default filter permits portal calls, so removing a talk policy does
not prove IPC loss; that test verifies absence of the session-bus socket.
Both configurations passed 20 Flatpak and 18 native/package cases, plus shared
foundation, smoke and blocking checks. Native recovery also passed on Release.
The startup-refusal regression inherits a real seccomp filter denying
`pidfd_send_signal` and checks the SDK's unavailable report, rejected acceptance,
zero runtime entries and no navigation. An unfiltered control loads normally,
including with a synthetic Snap marker. This is refusal proof, not Snap support.
It runs as `--check teardown` and is included in native package acceptance.
Local test packages are never published to a store.
