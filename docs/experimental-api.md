<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental SDK API

Alloy's C++ declarations are in `include/alloy/alloy_profile.h` and
`include/alloy/alloy_request_context.h`. CEF generates the corresponding
`include/capi/alloy/*_capi.h` headers and C/C++ wrappers. Do not hand-write the
bindings. These interfaces are experimental, not a stable ABI promise.

Define `CEF_API_VERSION=CEF_EXPERIMENTAL` for an SDK consumer. Before using
the API, call `cef_api_hash(CEF_API_VERSION, 0)` and compare the non-null result
with `CEF_API_HASH_PLATFORM`. Build with the headers and wrapper from that same
Alloy output; never substitute a stock CEF SDK.

| # | Topic | Entry points and contract |
|---|---|---|
| 1 | [Modes](profile-modes.md) | Stateless, memory-only and explicit encrypted request contexts |
| 2 | [Key sources](profile-key-sources.md) | Password, Linux Secret Service or an explicit embedder provider |
| 3 | [Profiles](encrypted-profiles.md) | Create/open, unlock, state and completed lock |
| 4 | [Site records](site-records.md) | Copied domain/last-visit snapshots and key-first deletion |
| 5 | [Decay](profile-decay.md) | Persistent `SetDecaySettings` / `GetDecaySettings` period/timebase policy |
| 6 | [Auto-lock](profile-auto-lock.md) | Runtime idle policy, native trigger availability and lock events |
| 7 | [Embedded exceptions](embed-list-settings.md) | Explicit directional lists, owned by the outer site's record |
| 8 | [Guest sessions](guest-sessions.md) | Separate developmental browser sessions, explicit report acceptance and asynchronous closure |

## Threads and completion

Create/open/unlock and synchronous `Lock` perform blocking crypto/filesystem
work on an embedder background thread; they can run before `CefInitialize`.
After a profile has native owners, `Lock` waits for UI teardown, so the embedder
must keep browser UI pumping. Use `LockAsync` on browser UI instead.

Record listing/deletion, embed lists, encrypted-context creation and auto-lock
configuration run on browser UI after `CefInitialize`. A queued method returning
true is not a successful result. Its callback reports completion asynchronously
on UI. False means unqueued and no callback. Only `ALLOY_SETTINGS_OK` is success;
`CANCELLED` is a lost/locked generation, not an empty successful read. Other
results distinguish invalid input, conflict, integrity and I/O failure.

Decay get/set run on a background thread with the other blocking profile calls.
Idle policy is separate: `SetAutoLock(milliseconds, handler)` runs on UI and is
runtime-only. Zero disables the respective decay/idle timer, not other lock
triggers. Alloy supplies no one-week or idle-time browser-policy default.

Callbacks can reenter and release owners. They retain the original profile
generation, never switch to a newly unlocked profile or a global context.
Unlocking requires a fresh encrypted context; old contexts stay revoked.

## Ownership

C++ uses normal `CefRefPtr` ownership. Native C passes one counted reference
for each refptr argument, including callbacks, and releases returned references.
Each record reference delivered in a C callback's array is transferred to the
caller; the array itself is borrowed. Copy or retain the records you need and
release each reference. Free userfree string results with
`cef_string_userfree_free`. Input strings and collection buffers remain owned
by the caller and can be released after a queued call copies them.

Returned domains and UI copies are plaintext owned by the embedder. Retaining
a snapshot does not retain a key or live record access. Clear displays and
copies when appropriate on lock/deletion; Alloy cannot erase application-owned
copies. Avoid profile/handler reference cycles, or break them when releasing UI.

## SDK verification

On the assigned `alloy-do-build` guest, from `/home/builder/work/alloy-b`, with
`TMPDIR=/home/builder/work/alloy-b/tmp` and `PYTHONDONTWRITEBYTECODE=1`:

```sh
python3 src/cef/tools/translator.py --root-dir src/cef
python3 src/cef/tools/version_manager.py -c --fast-check
python3 repo/tools/profile_probe.py --out src/out/Verify_Alloy --check core
```

The normal build updates the untracked experimental hash before compiling.
The explicit check then verifies it. Generated output belongs in the prepared
CEF checkout; this repository tracks the declarations and translator patches.
Repeat SDK acceptance against Release output before publishing a PR.
