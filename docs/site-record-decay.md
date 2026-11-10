<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental site-record decay

The embedder sets `AlloyProfile::SetDecaySettings(period_seconds, timebase)` on
an embedder background thread while the profile is unlocked. Only
`ALLOY_DECAY_SINCE_LAST_VISIT` is supported. A positive period enables decay;
zero disables it. New profiles have no decay policy. Alloy has no built-in
one-week default. Negative/overflowing periods and unknown clocks are refused
without changing the saved policy. `GetDecaySettings` requires an unlocked
profile and changes its outputs only on success. C clients use the generated
`set_decay_settings` / `get_decay_settings` functions on `alloy_profile_t`.

The setting lives in a separate virtual file in the profile-private encrypted
namespace, using the existing coordinator and atomic settings publication.
It contains a version, timebase and period, not a second record index. Reopening
a profile does not require resupplying the policy or running a settings UI.

## Visits and enforcement

A visit is a committed primary top-level document: navigation, reload or
Back/Forward, counting only the final document of a redirect chain. Embedded
frames, workers, background work, failed navigations and same-document fragment
changes do not count. Native commit ownership supplies the URL and origin;
renderer messages cannot name a record to refresh. A visit is queued on the
original settings owner and publishes encrypted last-visit metadata in the
record's catalogue entry. Old queued visits lose admission on deletion or lock.

A record that never received such a visit uses its creation time. Times use
wall-clock Unix microseconds. Future baselines are durably clamped to the
current time, not repeatedly treated as a new current time on each sweep.

Unlock loads the stored policy and finishes expiration before exposing the
coordinator or publishing UNLOCKED. Failure refuses unlock and wipes the opened
coordinator. Standalone expiration owns a scoped native settings-registry graph
built by the same registration constructors as the global graph, including Brave
settings. It validates exact registered pref names without constructing global
LazyInstances or changing CEF's AtExitManager lifetime. Unknown names remain
invalid; no prefix-based fallback is used. The graph dies with the sweep.
The engine also sweeps while unlocked, including before
`CefInitialize`: its timer checks at intervals no greater than the period or
60 seconds, whichever is shorter. Scheduling and native teardown can delay
completion; this is not a real-time deadline.

Once native owners exist, the same original settings queue handles expiration
through `DeleteSiteWithTeardownAsync`'s R13 lifecycle. Key/root removal and R5
reference pruning publish first, old capabilities and native documents/workers/
grants retire, then opaque objects are unlinked. A new visit that wins the shared
commit barrier prevents an earlier expiration snapshot from deleting it. Live
writers cannot recreate the retired generation. Later explicit navigation can
create a fresh record with a fresh key and generation. Unrelated sites and
profile-private settings survive; the expired outer site's embed list does not.

Expiration retires the selected record identity, not a record found by parsing
its domain as a content-setting pattern. Exact hosts, including trailing dots,
remain distinct during native teardown and reference cleanup. A stale selection
cannot borrow a replacement record. If an unlock sweep repeatedly selects an
unchanged record without retiring it, unlock fails rather than spinning or
publishing success with that record still expired.

## Honest boundary and verification

A locked profile retains expired ciphertext until its next successful unlock.
All existing [deletion limits](encrypted-record-files.md#record-deletion-and-the-honest-boundary)
apply: no secure SSD overwrite or protection against restoring a complete old
profile snapshot is claimed.

On the assigned build guest, `profile_probe.py --check decay` exercises a
six-second policy through the public SDK: cold-process unlock without reapplying
settings, expired settings/list absence, live pages and workers retiring despite
continued writes, visited-site survival, and fresh-navigation isolation. Live
verification uses the shared explicit-deletion fixture for cookies,
localStorage, sessionStorage, IndexedDB, Cache Storage, OPFS, service-worker
registration, and same-origin and top-site CDN HTTP caches. Fresh alpha values
must be absent while beta's values and cached responses remain unchanged. The
storage units additionally exercise unlock ordering, never-visited/future times,
pending writes and atomic visit/deletion races; native navigation units exercise
the commit classifier. Run in both Verify and Release configurations.
