<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Encrypted LevelDB record files

`RecordLevelDbEnv` implements the pinned native `leveldb::Env` interface for one
trusted record capability and store purpose. `Options(existing)` sets that Env
on the normal Chromium `leveldb_env::Options`; it never changes the global Env
or chooses keys from a pathname. The Env outlives its DB/compaction workers.

Sequential/random files, writable/appendable files, directories, enumeration,
rename/replace, sizes, deletion and LOCK all use the shared encrypted filesystem.
CURRENT and its temp/manifest protocol, numeric functional WAL, SST and rewrite/
recovery files therefore share the authenticated namespace and commit ordering.
The default Env supplies only clock, sleep and thread scheduling. There is no
native mmap, filesystem, test-directory or logger fallback.

Small append fragments coalesce in a bounded 32 KiB buffer owned by the commit
coordinator, not a plaintext cache in the adapter. Reads/random writes/truncate
observe pending bytes, sizes include them, Flush seals them, and Sync/Close
publish through the authoritative catalogue. Lock/revoke and last close cleanse
pending bytes under the same barrier as keys. Even detached open files cannot
reinsert a removed name. This keeps append buffering compatible with revocation
and with other handles, instead of relying on a caller to flush an external
buffer before locking. Flush does not claim the root durability fence; Sync does.

Integrity and uncertain-publication errors are sticky native IOErrors, with
`last_status()` retaining the underlying integrity cause. They are not reported
as absence or ignorable log corruption. Even Env methods whose native signature
is only bool cannot enable empty-DB recreation: the quarantined coordinator
rejects every subsequent create/write/delete attempt. All data files remain
encrypted, and there is no parent-profile seed.

Optional diagnostic `LOG` obeys the existing
`ALLOY_ENABLE_DIAGNOSTIC_LOGGING` build gate: product returns no logger and creates
no diagnostic file; verification uses an encrypted logger. Numeric `000001.log`
files are functional WAL and remain available in both configurations. No
diagnostic formatting/writing implementation is compiled into product builds.

Native tests open a real tracked Chromium LevelDB, write synchronously, force
compaction, execute RewriteDB, close/reopen the profile and read every value.
They cover sequential/random/appendable I/O, CURRENT rename, conflicting locks,
pending-buffer visibility/wipe, the diagnostic distinction, physical tampering
without empty recreation, and synced-WAL recovery after a multiprocess child
exits without close or profile Lock. Actual `rg -a` marker scans must find no
plaintext under the profile. All checks run on the assigned authorized test
machine alongside the cumulative both-config gate.
