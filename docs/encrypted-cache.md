<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Encrypted native cache files

`RecordCacheFileOperationsFactory` binds Chromium's native
`BackendFileOperationsFactory` and `CacheFile` interfaces to one
`RecordFileSystem`, record generation and store purpose. It holds no raw keys.
The `cache_files` GN target implements the Simple Cache file contract; browser
owner routing remains the responsibility of each store integration.

## Selection and lifetime

Resolve the engine-owned capability before looking up an entry. Pass the
factory to `CreateCacheBackend` with the Simple backend, a distinct absolute
virtual path and the store's chosen maximum size. The absolute path is an
encrypted logical namespace, never a physical cache directory. No global
factory or mutable global encryption key is installed. The ordinary memory,
blockfile and SQL backend choices are not changed by this adapter; a retained
SQL cache requires the encrypted SQLite adapter at its native SQL owner.

Factories can create sequence-bound operations or unbound operations to be
bound on a worker. Enumeration returns logical file paths, authenticated
lengths and timestamps, excluding directories. Index, index-dir temporary
files, entry keys, headers, footers, all streams, sparse ranges and doom/rename
files share the same encrypted namespace. Replacing or unlinking a filename
does not redirect an existing open handle to its replacement.

`CacheFile` has no error-returning close or flush method. Writes and length
changes therefore publish through the coordinator before returning success,
so publication errors reach the native caller. Reads return logical bytes and
offsets; no ciphertext descriptor, native mapping or plaintext staging file is
exposed. This durability choice has a per-write publication cost; it is not a
performance claim.

## Recovery, cleanup and revocation

The pinned Simple Cache startup recovery branch used native directory
enumeration/deletion and emptiness checks even with injected operations.
`0021-encrypted-cache-recovery.patch` excludes encrypted backends from that
physical-path recovery branch. A bad encrypted index structure fails startup.
Healthy version checks/upgrades, index reconstruction, ordinary entry lookup,
doom and replacement continue through the injected operations. The unrelated
physical filesystem cannot become an alternate cache namespace.

Authentication or uncertain-publication errors remain sticky on the factory.
The shared coordinator quarantines damaged state; even `kResetOnError` cannot
turn it into a successful empty cache. `last_status()` distinguishes integrity
failure from a normal native cache miss. The adapter never chooses another key,
parent profile or plaintext backend to make startup succeed.

`CleanupDirectory` posts work to the bound runner and acknowledges only after
encrypted recursive namespace removal has completed. Pending work rechecks
capability generation, including when deletion races the posted operation.
Record-wide R13 key retirement remains `RecordFileSystem::DeleteRecord`;
removing one cache subtree does not claim destruction of the whole record key.
Owners must quiesce native entries/backends and their already-decoded buffers
on record/profile teardown. Revocation prevents new encrypted I/O; it does not
erase plaintext already returned to a consumer.

## Verification

`RecordCacheFilesTest` exercises a real native Simple backend: streams 0/1/2,
sparse holes and range data, cold filesystem/backend reopen, actual `rg -a`
marker absence, doom/recreate, index temporary replacement, enumeration and
bind/unbind. It also checks tampering with reset-on-error, an unrelated physical
index sentinel during failed startup consistency, deferred cleanup completion,
and record deletion racing that cleanup. Both product and diagnostic configs
run these tests alongside the cumulative profile/file/SQLite/LevelDB proofs.
