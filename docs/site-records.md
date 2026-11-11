<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental site records

One record groups a site's state by canonical registrable domain (exact host
for IPs/hosts without a registrable domain), independent of scheme and port.
Origins and StorageKeys inside that record retain their security boundaries.
Opaque, file, data and about principals get no persistent record.

## List

On browser UI after `CefInitialize`, call
`AlloyProfile::ListSiteRecords(callback)`. True means queued, not success.
`AlloySiteRecordsCallback::OnComplete(result, records)` supplies copied
`AlloySiteRecord` snapshots only on OK. Each has `GetDomain()` and
`GetLastVisit()` (Unix-epoch microseconds, zero if never visited). No order is
promised. Listing does not create a record or count as a visit. Profile-wide
settings are not sites and never appear here.

Only a committed top-level document refreshes last visit. Listing, embedded
loads, scripts and background writes do not. Zero is never replaced by a
creation timestamp in public output; decay uses creation as its separate
fallback baseline for a never-visited record.

Locked/superseded operations report CANCELLED with no entries, not a partial
or replacement-generation catalogue. A copied snapshot is owned plaintext,
not continued record access. The embedder owns clearing copies on lock/deletion.
Native C owns each delivered record reference and frees userfree domain strings.

## Delete one

`DeleteSiteRecord(site_url, callback)` accepts an absolute URL and deletes its
whole record, not just that origin/path. Browser UI only; true means queued.
Callback OK acknowledges durable wrapped-key removal, generated-reference
pruning and retirement of the site's live documents, workers and grants.
`files_pending=true` means unreadable opaque objects await unlink, not that
keys or old live access remain. Cleanup retries on unlock.

Other records and explicitly selected external user files survive. The outer
site's considered-first-party list belongs to its record; generated references
to a deleted embedded site are removed too. A later record for the same domain
has a fresh key/generation even if its opaque directory name is the same.

## Delete all

`DeleteAllSiteRecords(callback)` fences profile record creation and writes,
retires every record through the single-record lifecycle, completes with an
empty site set, then lifts the fence. Navigation admission itself is not blocked.
Record creation/writes during the fence are refused; the shared native settings
journal also refuses preference mutations until retirement finishes. Reentrant
listing from the callback is ordered before fence release. A later visit creates
a fresh record with no old data. Profile-wide settings and key-source selection
survive. This is not a loop over an initial snapshot while concurrent tabs keep
creating records. Callback result and opaque `files_pending` semantics match
single deletion; overlapping delete-all requests report CONFLICT.

## Honest deletion boundary

Deleted or decayed data, including which site it belonged to, is unreadable
without the password/key. **Someone who knows the password and recovers leftover
raw drive blocks may recover traces of a deleted site.** Alloy does not rekey
the entire profile on deletion, promise SSD secure erasure, erase remote-server
observations or remove copies made by the embedder/user.

Record counts, ciphertext sizes and physical timestamps are not hidden. Opaque
keyed names cannot be guessed by hashing domains without a key. A complete old
profile backup, including its old root and wrapped keys, can restore old data;
there is no independent rollback/freshness anchor. See
[authenticated record files](encrypted-record-files.md#record-deletion-and-the-honest-boundary).
