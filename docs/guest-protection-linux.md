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

Landlock grants procfs WRITE_FILE for Chromium namespace map/setgroups setup,
without truncate/create/remove rights. **Guest processes are not isolated from
the user's other processes.** Non-truncating procfs writes can change some
same-user settings: OOM score, coredump filter, clear_refs, sched and autogroup.
A private process view is planned immediately after the first release. Native
Chromium OOM adjustment still uses its original truncating write, which fails
under this policy; renderer OOM scores remain inherited. No extra procfs grant
or alternate non-truncating OOM path is installed.

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

The supervisor checks scratch identity/backing/permissions/capacity while the
session runs. Loss ends the session; there is no disk fallback. Cleanup removes
owned scratch where possible. Abrupt supervisor death, including joint
app/supervisor death, can leave named RAM until logout or an identity-checked
sweep; it is never reused and is not promised to be securely erased immediately.

## Ubuntu packaging

Ubuntu 24.04 restricts unprofiled user namespaces by default. Instantiate
`packaging/linux/alloy-guest.apparmor.in` with unique profile names and the
**exact absolute installed bootstrap and engine paths**. Install the resulting
root-owned 0644 file under `/etc/apparmor.d/<app>-alloy-guest`. A .deb postinst
loads it with `apparmor_parser -r /etc/apparmor.d/<app>-alloy-guest` when AppArmor
is active; a prerm removes the loaded profiles with `apparmor_parser -R`
before removing the policy file. Depend on AppArmor tooling where required.
Use package-specific paths, not wildcard home/build attachments. Never disable
Ubuntu's global restriction or install a guest setuid helper.

Unprofiled/denied execution reports `unavailable` before CEF initialization or
navigation. Limits name the observed `unshare` errno and readable kernel
namespace controls. EPERM alone is not claimed to uniquely identify AppArmor.
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
