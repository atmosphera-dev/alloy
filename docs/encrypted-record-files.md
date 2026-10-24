<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Authenticated record files

`brave/alloy/site_records/record_file_system.h` is the engine-owned encrypted
virtual filesystem used by record-store adapters. It is a persistence mechanism,
not browser store routing: browser-owned frame/navigation/worker ownership must
select a record and purpose before opening a native store. A renderer pathname,
resource URL or request header is never authority to choose a key.

## Ownership and keys

One `RecordFileSystem` owns an unlocked profile, its commit mutex and directory
lock. All adapters and handles in that profile share it. An independent owner,
including one in the same process, is refused by a lifetime directory-inode
`flock`. A directory descriptor remains stable across authoritative root
replacement. Relative physical operations use `openat`, reject symlinks, and
admit only regular ciphertext files. Nonblocking open prevents substituted FIFOs
from blocking before type validation. Profile/record directories are private
0700 directories and newly written objects have mode 0600.

The lifecycle owner supplies a random 256-bit profile key and a public bootstrap
envelope. Password/key-source derivation belongs above this layer. Profile HKDF
subkeys are separate for directory HMAC, catalogue encryption and record-key
wrapping. Each record generation has an independent random 256-bit key and
128-bit generation. The catalogue contains its authenticated wrapped-key
envelope, never a plaintext on-disk key.

Record directories use full HMAC-SHA-256 over the canonical R16 record domain.
`RecordDomainForPrincipal` resolves the browser-owned principal using Chromium's
scheme-independent SchemefulSite grouping. It does not merge security origins
or StorageKeys inside a record. Nonpersistent principals receive no persistent
record. A separately domain-separated profile-private HMAC namespace holds
catalogue objects.

A file gets a new random 256-bit file ID on creation, retained across rename.
Its HKDF key is derived from the record key, file ID, stable on-disk store tag
and record generation (`alloy-file-v1`). Chunk-map, namespace and native-name
HMAC subkeys are separately derived. Store tags are explicitly assigned format
values, independent of the in-memory C++ enum's ordinal ordering.

`SecretKey` is noncopyable and non-elidably clears its old value on move,
replacement, explicit revocation and destruction. Decrypted temporary pages and
decoded metadata nodes are wiped. There is no plaintext page/chunk cache in
this implementation. Lock revokes capabilities and clears every live helper;
closing the last handle also clears its helper before evicting it, including
when an application retains the closed handle object.

## Format v1

All integer encodings are big-endian. Physical names contain only opaque
lowercase hexadecimal IDs, apart from the nonsemantic fixed authoritative name
`root`. Native filenames, directory relationships, domains, creation/modification
times, logical lengths, revisions and all private metadata are encrypted.

| # | Structure | Encoding and authentication |
|---|---|---|
|1|Immutable object|A 4096-byte plaintext page/chunk plus the 16-byte AES-256-GCM-SIV tag. Its random 256-bit ID is its physical name; its parent stores the expected ID and SHA-256 ciphertext digest|
|2|Extent subkey/nonce|HKDF-SHA-256 derives a fresh per-object key from its encryption key, random object ID and `alloy-data-extent-v1` plus the fixed object-type tag. Nonce is the 32-bit type plus 64-bit chunk index. Each object seals once; every retry allocates a new ID and exclusive physical file|
|3|AAD|Format magic/version/algorithm, object type, length-delimited coordinator binding, random object ID, logical index and plaintext length. File binding includes profile/record/generation, stable purpose tag and file ID. Metadata/root/key-envelope types have distinct domains|
|4|COW maps|4096-byte encrypted path-compressed binary Merkle-trie pages. Leaves contain a 256-bit key and at most 3896 value bytes; branches contain a prefix/branch bit and two expected child ID/digest references. Prefix bounds and strictly increasing branch bits are validated. Updates copy only a search path, at most 256 branches|
|5|Encrypted namespace|Name lookup is HMAC over purpose, parent directory ID and length-delimited component name. The encrypted leaf stores those fields, type, modification time, file identity, logical length, revision and expected chunk-map root. Directory moves preserve descendant identities without rewriting their paths|
|6|Key envelope|Fresh 32-byte random wrapping ID followed by 32 encrypted key bytes and a 16-byte tag. Identity/generation binding and the separately domain-separated envelope type are authenticated|
|7|Authoritative root|Eight-byte magic/version/algorithm, fresh random 32-byte root ID, a 32-bit bootstrap length, public bootstrap, and a 4096-byte encrypted root plus tag. The entire public bootstrap is AAD-bound. The encrypted body holds the profile identity, publication revision and expected catalogue directory/root ID/digest. No recovery history or named backup is retained|

The public bootstrap is only key-source parameters, salts and the wrapped random
profile key. `ReadBootstrap` is explicitly unauthenticated; its caller must unwrap
the key and successfully open the authenticated root before trusting profile
contents. Root and key-envelope failures are errors, never an empty-profile
fallback.

Format-v1 arithmetic limits are 1 TiB logical length per file, at most
2^32−1 mutations per file ID and at most 2^32−1 root publications per profile.
Limit exhaustion is an explicit error, not counter wrap or nonce reuse. Random
IDs and exclusive creation detect physical collisions; failed/torn objects are
never reused. BoringSSL's operating-system RNG fails closed instead of selecting
a weak source. Whole-profile rollback does not restore an on-disk nonce counter:
new seals always obtain new OS randomness.

## Publication, handles and errors

The coordinator serializes admission, mutations, namespace changes, commit and
revocation. All handles to one file ID share a logical revision. Partial writes
authenticate old mapped chunks even on a full overwrite, then write fresh COW
extents/maps. A failed multi-chunk mutation leaves the prior in-process logical
root intact. Sparse absence is an authenticated map decision; a missing
referenced object is an integrity error. Shrink removes tail mappings and
reseals a zeroed partial tail. Extension cannot reveal old bytes.

`Sync` fsyncs new data/metadata and their directories, publishes the new file/
namespace/record roots in the encrypted catalogue, fsyncs a fresh opaque root
sibling, atomically renames it over `root`, then fsyncs the root directory.
Only the catalogue-selected roots are authoritative. Completed non-fsync writes
also publish on native Close; native rename commits its referenced data before
returning. Explicit profile Lock flushes completed changes before wiping and
returns any failure to its lifecycle owner.

Rename preserves file identity. Replacing or unlinking a name detaches existing
handles while preserving their ordinary open-file behavior. A detached handle
can never reinsert the name. Shared/reserved/pending/exclusive logical file
locks are coordinated across handles; pending writers block new readers. Native
adapters still implement their own higher-level ownership/transaction contracts.

After publication, authenticated reachability determines garbage collection.
Superseded catalogue pages are individually unlinkable, never append-packed
with live keys. Record GC retains current namespace/map/data objects and all
still-open detached file roots, so ordinary rewrites do not accumulate an
unbounded ciphertext history. Closing detached handles releases those roots for
the next collection. Unlock completes orphan/key-envelope cleanup before
returning any record admission.

Authentication/coverage failures latch an integrity error and clear keys. A
failure after root rename, or while retiring obsolete key-bearing pages, latches
`kCommitUncertain`, clears keys and requires reopen. There is no old-root choice,
silent raze, native plaintext fallback, parent-profile read or normal cache-miss
translation for these failures. A native adapter must preserve this distinction.

## Record deletion and the honest boundary

Deletion closes admission under the commit barrier, durably removes the wrapped
record key and namespace reference, unlinks/fsyncs superseded key-bearing
catalogue objects, then revokes/wipes all handles and calls engine teardown.
The callback runs outside the mutex, with admission for the old directory still
closed. Only then are record data objects unlinked. The result is `deleted` or
`deleted-files-pending`; orphan cleanup retries at unlock. Failure to retire key
envelopes never acknowledges key destruction or invokes teardown as though it
had succeeded. A later record for the same domain has a new random key and
generation even though its HMAC directory name can be the same.

Encryption does not hide record counts, ciphertext sizes or physical timestamps.
Someone without the key cannot identify domains by hashing guesses. A password
holder recovering SSD residual blocks may recover old traces; no secure-block
overwrite or profile-wide rekey is claimed. Partial old chunk/map/record replay
does not match the current authenticated parent. Restoring a complete,
internally consistent old profile snapshot, including its authoritative root
and key envelopes, cannot be detected without an independent freshness anchor.
No TPM, remote service or monotonic keyring counter is introduced.

## Verification

Run only on the assigned, explicitly authorized build/test machine with its
disk-backed TMPDIR. `alloy_site_records_unittests` exercises the object, map,
chunked-file and coordinator layers, native namespace/locking contracts, key
clearing, concurrent handles, pending writes/deletion and post-publication GC.
Multiprocess children deliberately `_exit` at every observed publication-fence
occurrence, including data/metadata writes/fsyncs, directory syncs, root
write/fsync/rename, and obsolete-object unlink/fsync. Record deletion additionally
exercises record-directory unlink and its parent-directory fsync. The nonfaulted
final child exits after the native success acknowledgement without cleanup, then
the parent observes the persisted result after reopen. These simulate process
crashes, not hardware power loss or a drive that lies about fsync.

Native SQLite/LevelDB/cache adapters and browser-store routing are separate
consumers of this mechanism; their own real-store round trips, tamper handling,
marker scans and both-config product gates are required when added.
