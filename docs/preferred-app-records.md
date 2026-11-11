<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native preferred-app record fragments

Native preferred-app entries are ordered intent filters. Equal-rank matches use
native last-match precedence; authority conditions are ANDed, and values inside
each condition are ORed. Persisting independent split filters would change
identity, conflict handling and ordering after restart.

The record representation therefore stores fragments with opaque join IDs,
native entry ordinals and original authority-value positions. Each fragment
retains common native conditions and activity attributes, but only authority
values belonging to its owner. Native AuthorityView decodes host/port; R16
groups literal authorities, domain-boundary suffixes and proven single-host glob
authorities into records. Fixed-character globs are decoded and confirmed with
the pinned native matcher, preserving ports and escape syntax. Native wildcard
and repetition patterns remain global; in this pin even an escaped dot is still
a wildcard, so it cannot prove a single-host owner. Explicit
origin-free protocol/MIME choices and selectors spanning unrelated sites remain
profile-level dispatch configuration. Native matching syntax is not changed.

The volatile join validates schema, native enum ranges, owner consistency,
unique ordinals/positions and common native filter identity before reconstructing
the original ordered native entries. It never silently drops malformed
conditions or broadens a grant. Empty authority conditions remain present when
a deleted owner's values were their only values.

SitePrefStore routes fragments before serialization and publishes every shard
through the shared settings transaction. Site deletion removes that owner's
fragments in the same authoritative publication as its key. Remaining records
retain their grants without naming the deleted authority; no persistent
profile-wide URL or join index exists. A failed publication cannot replay a new
choice on later close.

`PreferredAppRecordsTest` exercises real native PreferredAppsList matching,
identity/attributes, ordering, multi-authority/AND semantics, record deletion,
global choices, malformed input, publication failure and independent-process
readback with ciphertext scans. These tests prove the representation and its
native-consumer semantics.

## Native model lifecycle

`PreferredAppsList::Replace` installs the complete projection before delivering
supported-link removals and changed/new choices. Admission is an injected,
side-effect-free callback checked by every public read and mutation view. The
read-only handle exposes no cached grant while admission is lost; its owning
persistence controller can inspect retained desired entries through the separate
`GetPendingValue` method. Retention is not permission to replay them under a new
record generation. The controller must bind the original generation and call
`SetAvailable(false)` during invalidation to deliver removals promptly.

Conflict decisions operate on an owned candidate and borrowed-input copies.
Only a still-live, unchanged, admitted operation installs its candidate; a
delegate cannot observe partial old removals or revive a revoked projection.
Matching preserves native tie ordering, longest-prefix selection and the
fallback priority of extended web-app scopes.

One notification drain owns observer delivery. Nested mutations enqueue their
events; obsolete positive events are cancelled or rederived from the current
admitted model per consumer, while current removals still reach every observer.
Readiness follows the admitted initialized model rather than a filter revision.
Observer/delegate callbacks may replace the model or retire its owner; weak
lifetime checks stop access after destruction, including base-handle teardown.

`PreferredAppModelTest` covers those outcomes with two real native observers,
reentrant conflict/matching callbacks, owner destruction and admission loss.
The owned test target also runs the unchanged upstream `PreferredAppListTest`
cases. Native model tests alone do not establish persistence or browser-context
binding.

## Native persistence and live publication

`PreferredAppsImpl` receives a native storage interface with typed read/write
results, versioned snapshots and a pure original-generation admission callback.
It retains one stable native list handle. The browser composition root selects
storage; the native component does not import Profile or its Alloy consumer.
The ordinary-file constructor retains asynchronous reads/writes on its native
sequenced runner. Missing files and real read/parse/write errors are distinct;
errors never initialize or rewrite a successful empty model. Native file writes
use atomic replacement, with ImportantFileWriter's documented crash limitations.

`MakePreferredAppStorage` binds that interface to SitePrefStore on the prefs'
owning sequence. A compare-and-publish token identifies both preferred-value
revision and admission lifetime. The candidate is encrypted and durably published
before preference observers run. Its acknowledgement is valid only for that
published version, not a replacement installed synchronously by an observer.
R13 retires admission even if the preferred value itself is unchanged. Each
value/admission publication emits one combined invalidation, so a duplicate old
event cannot cancel an explicit new action queued after that invalidation. An own
write's revision echo is consumed by its acknowledgement; deletion and errors
still invalidate immediately, including during publication. Invalidation carries
the real typed error so it cannot overtake and demote a failed acknowledgement.

The encrypted backend is asynchronous on the shared settings I/O sequence. Echo
suppression applies only inside the matching preferred-value installation, not
for the entire outstanding I/O interval; another native producer's change must
still invalidate the model while a write is pending. The native controller keeps
its original revision/admission and retained desired state until the real durable
acknowledgement. A completion after lock is denied/cancelled rather than rebound
to a fresh projection.

Multi-filter mutations run on an owned native candidate. Every external conflict
query must return to the same alive, admitted operation before anything installs.
Host notifications flow through the native model's shared notification drain;
the host or another observer can reenter or destroy the entire Impl. No host
pointer is used after that retirement. Deferred commands, reads, writes and their
acknowledgements are bound to their original controller epoch. An old completion
cannot acquire the authority of a freshly retained post-deletion projection.

The admitted model reflects current desired settings while ordinary asynchronous
writes are pending. Success chains only the owner's own newer desired version;
failure revokes the read view and notifies the host. Failed desired values and
rejected commands remain explicitly inspectable through `GetUnsavedValues` and
`GetRejectedChanges`. They are never silently replayed after refresh, deletion
or unlock; reapplying them requires a new owner action. Snapshot retention is
not authority. A failed read is not reported as initialized.

`PreferredAppPersistenceTest` exercises the real Impl, injected encrypted
backend, native host and independent-process cold consumer: two-site choices,
stable-handle/host revocation, selective deletion, lock, multi-filter reentry,
whole-owner retirement, queued reads/writes, stale acknowledgements, failure
attribution/unsaved state, ordinary async compatibility and actual ciphertext
domain scans. The shared settings-owner tests separately exercise callback-time
deletion, stale revision refusal and unchanged-value admission retirement.

These prove the native component/storage boundary. Encrypted Profile/request-
context factory selection and the final encrypted CEF consumer require their
own integration; those higher-level outcomes are not implied by native tests.
