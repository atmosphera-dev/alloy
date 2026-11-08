<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native considered-first-party settings

The explicit list maps an outer site's record domain to embedded sites that
the user considers first-party there. Entries belong to the outer site's
encrypted settings shard. An entry does not itself create the embedded site's
record. R16 domain grouping uses the existing browser principal/site machinery,
including IP and intranet hosts, without merging origin security identities.

`SitePrefStore` owns the single native preference projection. Its owning-sequence
`GetFirstPartyEmbeds` copies canonical domain strings; `SetFirstPartyEmbeds`
validates the complete input before atomically replacing one outer site's list.
Canonical duplicates are removed while preserving first occurrence order. An
empty replacement removes that outer entry, and an absent read or empty write
does not create a record. `SetConsideredFirstParty` uses the same replacement
owner for single-entry changes. Invalid input preserves the previous setting;
failed reads do not modify the caller's output.

## Publication and callbacks

The native projection and its observers stay on the owning UI sequence. A single
serialized `MayBlock` runner constructs, loads, writes and destroys its private
I/O backend. Native preference flushes, explicit list and preferred-app
publication, and record deletion all use that same backend. Browser UI uses
`ReadPrefsAsync`, callback-based typed operations and native asynchronous flush
hooks. Blocking compatibility methods assert that blocking is allowed and are
for pre-initialization/blocking-capable callers, not browser UI.

`CommitPendingWrite` preserves both native completion forms: the thread-safe
`synchronous_done_callback` runs on the I/O sequence without requiring the owner
to pump messages; the ordinary reply returns to the owner. Native dirty-key
checkpoints merge into the backend's current values, preserving typed writes
whose replies have not yet reached UI. Pending deletion references are pruned
before a native checkpoint can write. No worker waits for UI teardown: an opaque
coordinator-owned retirement ticket keeps directory admission closed between key
retirement, UI teardown and the later finish/unlink task.

Explicit setting publishers share one candidate-persistence and installation
path with native preferred-app settings. The candidate's encrypted shard roots
are published before preference observers see the new projection. Cross-site
permission persistence is classified against that candidate's embed list, not
the older live list.

The installation owner captures the setting revision and admission lifetime
before invoking observers. A synchronous replacement, deletion, invalidation or
lock cannot make an old setter acknowledge the replacement as its own success.
Silent native mutations and prefix removals also advance the tracked setting's
revision, including change-and-restore sequences. An unrelated setting change
does not change the preferred-app or embed-list value revision. Record deletion
retires admission even when a tracked setting's value is unchanged.

`kBusy` after observer reentry means the original publication is no longer the
current admitted version. It does not roll back the observer's newer explicit
operation or authorize an automatic replay. Publication errors retain the real
failure and close admission through the shared encrypted-file coordinator;
later close cannot replay the refused candidate. Reopen reads the authoritative
encrypted root.

R5/R13 deletion removes outer entries and generated references to a deleted
embedded site in the existing single settings/key-removal transaction. No
persistent aggregate list or second API-owned pref snapshot is introduced.

## Verification boundary

The experimental `AlloyProfile::GetFirstPartyEmbeds` and `SetFirstPartyEmbeds`
methods accept requests on browser UI after initialization. Their boolean return
means queued only; `AlloySiteSettingsCallback::OnComplete` reports the actual
result asynchronously on UI. Reads return copied canonical domains; setters
accept absolute URLs and return no entries. Empty replacement clears one outer
list. A lost/locked admission reports `ALLOY_SETTINGS_CANCELLED`, never success
under a newly unlocked generation. Invalid input and publication conflicts are
distinct from I/O/integrity failure. Wrong-thread or missing-callback calls are
not queued and do not call the callback.

The profile caches one projection for its current coordinator generation and
reuses one I/O runner across unlocks. Internal native Profile composition obtains
that same projection through `alloy::GetProfilePrefStore`, with an explicit
profile and no global fallback. API callback-only profile references are released
on the I/O runner, preserving the existing background-only locking lifecycle.

`alloy_site_settings_consumer` is an SDK-side consumer of generated C and C++
bindings with real CEF initialization/UI callbacks. Its driver uses isolated
legacy CEF runtime fixture roots separately from the encrypted settings root;
those fixtures are not encrypted browser-context routing proof.

`EmbedListTest` exercises native callback deletion/replacement, durable visibility
before callback-time lock, silent change-and-restore, unrelated setting writes,
failed publication, candidate permission admission, whole-list validation and
canonicalization, independent-process enumeration and denied invalidated access.
Existing native site-settings tests additionally cover cross-record dependency
cleanup and ciphertext domain scans.

The native Profile binding installs the embedded-state read-only policy hook before
the first document. It reads this projection directly. The encrypted browser
probe additionally seeds real first-party data and observes it from a listed
iframe; after outer-record deletion, a fresh iframe instead gets temporary
backing. Native composition tests check the actual frame StorageKey selection
for both encrypted and guest settings. Record retirement also revokes matching
temporary embedded scopes through the same embedded-state lifecycle hook.

## Native consumer contract (item 6E)

The following member names/signatures and the canonical-domain dictionary schema
are the native integration surface. Consumers receive the explicit owning
projection; browser UI predicates do no filesystem work and do not select another
profile. The native context binding supplies the same generation-bound projection
used by the public API. A locked, invalidated or retired owner returns false from
the predicate, rather than consulting a previous or global profile.

| # | Member | Consumer use |
|---|---|---|
|1|`bool SitePrefStore::IsConsideredFirstParty(const GURL& outer, const GURL& embedded) const`|Read-only decision on the owning UI sequence, using browser-trusted outer and embedded URLs|
|2|`GetFirstPartyEmbedsAsync(GURL, OnceCallback<void(FileStatus, vector<string>)>)`|Copy the canonical list; completion on UI|
|3|`SetFirstPartyEmbedsAsync(GURL, vector<GURL>, Completion)`|Atomic whole-list replacement; OK means durable publication|
|4|`SitePrefStore::kEmbedList`|`alloy.considered_first_party`; flat pref value is dictionary `outer record domain -> list of embedded record domains`|

Every listed domain is canonical under the existing R16 resolver; HTTP/HTTPS and
subdomains of the same registrable domain share the setting, IP/intranet hosts
remain their own identity, and opaque/non-network principals receive no entry.
The predicate is directional: an entry under X does not grant the reverse pair.
Only an explicit entry controls this exception; the consumer supplies its own
temporary-jar routing/lifetime decisions. A list entry changes admission, not
the embedded site's record ownership. R5 cleanup removes references to a deleted
embedded site, and R13 removes the outer list with its outer record.

`AlloyGetFirstPartyEmbeds(context, outer, callback)` and
`AlloySetFirstPartyEmbeds(context, outer, entries, callback)` expose that same
setting through an explicit initialized request context. Encrypted contexts use
their existing `SitePrefStore`; memory contexts use their own native in-memory
preferences. Both use `embed_list_settings` for the canonical list schema and
R16 validation. CEF sharing aliases the same owner; independent contexts do not.
Stateless, retired and locked contexts refuse access without parent fallback.
Callbacks are asynchronous on UI. OK acknowledges a completed memory publication
for guest and a durable record publication for encrypted profiles. Invalid whole
lists preserve the prior value; empty lists clear the outer entry.

`alloy_guest_settings_consumer` checks the public memory/sharing/independent and
stateless paths. `profile_probe.py --check encrypted` also runs the generated
C/C++ profile-list API and native Local State bootstrap consumers, then checks
actual browser permissions, zoom, profile metadata, list ownership, cold reopen,
lock, deletion and worker retirement. The keyring check runs the same encrypted
browser/settings path with a throwaway Secret Service source.
