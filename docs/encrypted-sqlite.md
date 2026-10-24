<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Encrypted SQLite record files

`RecordSqliteVfs` is an explicitly selected SQLite VFS bound to one engine-issued
record capability and store purpose. It stores every SQLite logical file in the
existing encrypted namespace and uses the same authoritative profile catalogue,
commit coordinator and generation revocation. Filenames never choose keys.

Create the registration after resolving trusted browser-side ownership. Keep it
alive until all of its SQLite connections have closed; its destructor checks
that lifetime contract. Each registration has a random nonsemantic VFS name.
`Options(existing_options)` sets that name and disables mmap while preserving
the caller's rollback/WAL and locking strategy. Callers pass virtual relative
database names, not native OS locations. Nested logical directories must already
exist in the encrypted namespace. Ordinary native copy/delete/backup helpers are
not virtual operations and must not be used by encrypted-store owners.

## Deliberate composition with //sql

The pinned `sql::EnsureVfsWrapper()` wraps only the global default VFS and checks
its OS/storage-service allowlist. This adapter calls `EnsureSqliteInitialized`
first, leaving that ordinary wrapper installed, then registers its own VFS with
`makeDflt=0`. `DatabaseOptions::set_vfs_name_discouraged` selects the record VFS
explicitly in `sqlite3_open_v2`. It never feeds a new default into the wrapper or
changes that allowlist. The record VFS implements the complete required I/O
surface itself; `sql::Database` obtains its file via SQLite's standard
FILE_POINTER control, without assuming it is a `sql::VfsFile` wrapper.

Tests use real `sql::Database` instances in rollback and WAL modes and confirm
the default VFS remains unchanged. This is production option plumbing, not a
testing override or process-global mutable key.

## Native contracts

All xOpen kinds use encrypted files: main DB, rollback/temp/sub/super journals,
temporary/transient DBs and WAL. A NULL temporary filename gets a fresh opaque
virtual name, and DELETEONCLOSE removes it from the encrypted namespace. Related
journal/WAL names retain the same capability with independent logical file IDs.
Exclusive creation and read-only flags preserve their native meanings. There
is no native temporary directory, plaintext working copy, or raw FD escape.

Read supports SQLite's short-read/zero-fill contract. Write/truncate/sync/close
use the shared logical file layer and return native errors. File controls do
not pre-extend logical EOF for allocation hints. xFetch never maps data, and
the mmap control reports zero even if a raw SQLite caller requests a mapping.
No device atomicity or powersafe-overwrite feature is claimed beyond the native
ordering implemented by the coordinator.

Logical shared/reserved/pending/exclusive locks are coordinated by file identity.
The VFS reads the actual lock state to implement SQLite's no-op upgrade/downgrade
rules, including a pending lock retained after a busy exclusive upgrade. The
profile directory's lifetime OS lock rejects accidental second process owners.

WAL shared memory is an in-process, aligned memory index shared by file identity
across registrations for the same capability. Slot locks and barriers preserve
SQLite's protocol. No `-shm` file is persisted. Last unmap/close destroys and
cleanses the index regions; after an ungraceful exit, SQLite reconstructs the
index from the authenticated encrypted WAL. Store lifecycle owners must quiesce
and close SQLite before teardown completes, just as they must retire SQLite's
own decoded page/statement caches. Revocation immediately rejects new file I/O;
the crypto layer does not pretend to erase plaintext already returned to callers.

## Integrity and deletion

Integrity and uncertain-publication failures surface as `SQLITE_IOERR_DATA`,
not a missing file, empty DB or cache miss. They remain latched in the adapter
even if SQLite later makes cleanup calls on revoked handles. The underlying
filesystem also quarantines the profile, so recovery attempts cannot truncate,
raze, recreate plaintext or seed from another profile. Locked/revoked admission
is an I/O access error. Ordinary absence is reported only for genuinely absent
authenticated namespace entries.

The shared namespace decoder validates into a separate local value before
replacing caller output. This matters when a lookup's input name aliases its
destination: journal/temp names are heap-sized, and replacing them prematurely
would invalidate the borrowed input. Stat/list/remove/rename share this
ownership boundary and the long-name regression.

Record deletion retains the existing R13 key-removal/revocation/teardown/unlink
order. An open DB cannot use a deleted generation to recreate its data. Its
native-store owner still closes the database and releases the VFS registration
as part of teardown.

## Proof

`alloy_site_records_unittests` exercises real sql::Database rollback/WAL reads
after profile reopen, normal-mode concurrent WAL through separate registrations,
all eight xOpen file types, temp cleanup, short reads, mmap refusal, revocation
and physical-tamper error propagation. A multiprocess child acknowledges a FULL
WAL commit then exits without close/checkpoint/Lock; its parent observes the
committed row after cold reopen. Read-only `rg -a` scans of the actual profile
must return no marker matches. These checks run on the assigned authorized test
machine, with its disk-backed TMPDIR, alongside the cumulative full gate.
