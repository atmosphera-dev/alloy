<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Linux guest protection

Guest sessions require Landlock ABI 3 or newer, verified tmpfs scratch and
Chromium's user-namespace sandbox. They never use `chrome-sandbox`, a setuid
relay or `--no-sandbox`. Normal, non-guest Alloy retains its existing sandbox
selection. The app inspects `OnPrepared` and explicitly accepts protection;
missing requirements never cause a weaker retry.

The static bootstrap closes ungranted inherited descriptors, replaces stdio
with `/dev/null`, disables core output and establishes Landlock before loading
libcef. Exec discards the app's mappings. Fork/exec and zygote children inherit
the domain; native runtime entries register kernel identities and liveness
channels with the unprivileged supervisor. Writable app output handles are not
part of this protocol. M3's one ordered extension route remains unchanged.

## What the report means

| # | Field | Meaning |
|---|---|---|
| 1 | Filesystem | `data-writes-denied`: Landlock handles writes, truncate, creation, removal, rename and hard links outside its explicit exceptions. This is not metadata refusal. |
| 2 | Process coverage | Pre-engine enforcement and inherited children, with registered runtime supervision. External Flatpak/Snap entries remain unavailable until their adapters are proven. |
| 3 | Scratch | `verified-tmpfs`: fresh descriptor-pinned 0700 RAM scratch; no durable fallback. RAM, memfds and GPU memory can swap or hibernate. |
| 4 | Sandbox | A private blank renderer completes before PREPARED. The report includes observed renderer seccomp, NNP, capabilities and native userns status, not a blanket claim about other roles. |
| 5 | Limits | Actual proc/device exceptions, metadata, existing capabilities, IPC, external services and cleanup limits. These values contain no browsing activity. |

Known writers use private RAM HOME, XDG config/cache/data/state/runtime, native
temporary paths, an explicit CEF root, both NSS destinations and Fontconfig
caches. GSettings is memory-backed, GIO local, ambient session-bus activation
disabled and graphics disk caches disabled. Existing memory/stateless store
owners remain authoritative. Guest logs and crash files are suppressed.

The lifetime enclosure has its own PID namespace, matching private procfs and
POSIX session. Real CEF needs namespace-local process IDs and `/proc/<pid>`
lookups to agree. This is not a distinct host UID, an ambient IPC firewall or a
read-only filesystem view; non-process procfs controls are not virtualized.
Landlock still grants procfs WRITE_FILE for Chromium namespace map/setgroups
setup without truncate/create/remove rights, now on that private instance.
Native Chromium OOM adjustment still uses its original truncating write, which
fails under this policy; renderer OOM scores remain inherited. No extra procfs
grant or alternate non-truncating OOM path is installed.

Sandbox state is role-specific: Chromium's userns zygote retains only
namespace-local CAP_SYS_ADMIN to create child PID namespaces. Renderers drop
capabilities and use seccomp. This is not host-root privilege or a claim that
every role has a renderer's seccomp/capability state.

The only writable device exceptions are `/dev/null`, `/dev/random` and
`/dev/urandom`; creating block/character devices is refused even in RAM.
Landlock does not refuse chmod, utime, xattrs or atime updates, revoke existing
writable FDs/mappings, provide read secrecy or enforce an ambient IPC firewall.
Admission closes inherited capabilities; it does not change those kernel
limitations. Connector routing is separate from OS IPC/device enforcement.
Kernel audit/core policy, drivers, external services and app-authorized saves
are outside the physical-storage guarantee. This backend is not a no-disk claim.

Scratch is a private, size-limited 1 GiB tmpfs in the enclosure mount namespace,
with noexec, nosuid and nodev. Its `/dev/shm` is not the host's shared location;
no named `alloy-guest-*` scratch is created outside. The supervisor checks its
identity/backing/permissions/capacity while the session runs. Loss ends the
session; there is no disk fallback. The kernel reclaims scratch after the last
reference ends, even on app/init or supervisor SIGKILL. Same-user inspection is
not isolated: deliberately opening `/proc/<pid>/root` can read private scratch,
and an observer's retained handle keeps that memory alive after guest exit.

## Ubuntu packaging

Ubuntu 24.04 restricts unprofiled user namespaces by default. Instantiate
`packaging/linux/alloy-guest.apparmor.in` with unique profile names and the
**exact absolute installed app, bootstrap and engine paths**. The app needs
the allowance because it creates the outer user/PID enclosure before the
bootstrap exec; the engine still needs Chromium's nested namespace operations.
Install the resulting
root-owned 0644 file under `/etc/apparmor.d/<app>-alloy-guest`. A .deb postinst
loads it with `apparmor_parser -r /etc/apparmor.d/<app>-alloy-guest` when AppArmor
is active; a prerm removes the loaded profiles with `apparmor_parser -R`
before removing the policy file. Depend on AppArmor tooling where required.
Use package-specific paths, not wildcard home/build attachments. Never disable
Ubuntu's global restriction or install a guest setuid helper.

Unprofiled/denied execution reports `unavailable` before CEF initialization or
navigation. Lifetime refusals name the failed clone3, ID-map, mount, session,
pidfd or wait operation and errno. Native sandbox limits also include readable
kernel namespace controls. EPERM alone is not claimed to identify AppArmor.
Missing kernel support, disabled user namespaces, exhaustion and other LSM or
seccomp policy can also make the route unavailable. A package allowance cannot
override those kernel/admin settings. Even developmental acceptance cannot
activate a session without its native sandbox.

## Verification

Run only on the assigned Alloy cloud guest, task `gm-m2`, workspace
`/home/builder/work/alloy`, with `TMPDIR=/home/builder/work/tmp`:

```sh
python3 repo/tools/guest_lock_probe.py --expected-hostname alloy-gm-m2 --out src/out/Verify_Alloy --check all --evidence evidence/guest-mode/m2/lock-01
```

The driver records real-default before/live/close/crash inventories without
traversal exclusions, kernel boundary fixtures, role status and teardown,
memory/stateless outcomes and an Ubuntu package install proof. Require
`GUEST_LOCK_PASS`, complete receipts and zero failed tests. Repeat in Release;
startup alone is not containment acceptance.
