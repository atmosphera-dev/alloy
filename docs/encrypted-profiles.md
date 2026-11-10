<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental encrypted profiles

`include/alloy/alloy_profile.h` exposes `AlloyProfile` and `AlloyKeySource` through
CEF's native translator. C clients use the generated
`include/capi/alloy/alloy_profile_capi.h`; C++ clients use the normal generated
CEF wrapper. Select the experimental API and perform the usual CEF API-hash
negotiation. The new types keep the Alloy prefix in wrapper identities and do
not change existing Cef wrapper names. These APIs may evolve while experimental.

## Lifecycle

`AlloyCreateProfile(path, password)` creates a new profile and unlocks it.
Existing paths are refused. `AlloyOpenProfile(path)` opens an existing profile
while locked, reading only public bootstrap parameters. `UnlockWithPassword`
authenticates/decrypts the profile and returns false on failure; it never selects
another source or substitutes a fresh profile. `Lock` flushes completed writes,
revokes admission and wipes keys. Releasing the last profile reference locks it.

Operations are blocking and run on an embedder background thread. They can run
before `CefInitialize`, so profile bootstrap does not require constructing an
ordinary Chromium Profile or opening private preferences first. Browser-context
and native-store binding use this foundation in their own integration layers.

The states are LOCKED, UNLOCKED, TRANSITION and ERROR. LOCKED acknowledges that
engine-owned keys and admitted file access are gone. An in-flight unlock keeps
TRANSITION until its temporary keys are cleared. `Lock` during that operation
requests cancellation and returns false rather than acknowledging destruction
prematurely. A later completion cannot publish a cancelled unlock. State and
source callbacks can reenter safely; a concurrent unlock is refused while an
operation is still running. ERROR indicates a quarantined file-layer failure.
An explicit lock still wipes on a flush failure, while returning that failure.

The embedder owns clearing its input password strings and any credentials/key
copies in a custom provider. Alloy's password conversion writes into a single
owned buffer, rejects malformed UTF-16 and wipes that buffer; it does not use
generic conversions that leave extra freed password strings behind. The
one-shot password source wipes its own input on success, error and destruction.

## Key sources and hierarchy

The engine `KeySource` interface yields a transient 256-bit KEK from public
source parameters. A random profile key is authenticated-wrapped by that KEK.
The existing encrypted catalogue wraps independent random per-generation site
keys; the file layer derives distinct lookup, metadata, naming and file keys.
See [authenticated record files](encrypted-record-files.md) for the shared
publication, deletion and integrity contracts.

The password source uses the **official Argon2 reference implementation** pinned
by Brave's DEPS at `62358ba2123abd17fccf2a108a301d4b52c01a7c`, through
`brave/third_party/argon2`. Its original source/license is retained; Apache-2.0
is selected from its Apache-2.0/CC0 grant. Format 1 uses Argon2id v1.3, 64 MiB,
three passes, four lanes and a random 256-bit salt. The existing no-threads
reference target schedules lanes serially without changing the result. Arena
deallocation always wipes memory, independently of the reference library's
internal-clear flag. No weaker KDF or plaintext fallback is selected on failure.

`AlloyCreateProfileWithKeySource` and `UnlockWithKeySource` use an explicit
embedder implementation of `AlloyKeySource`. Its synchronous `DeriveKey` gets a
borrowed 32-byte public salt and fills a borrowed 32-byte KEK buffer. It runs
outside the profile lifecycle mutex and is not retained by the profile. The
embedder owns its provider's credential policy and storage. Built-in password
profiles cannot be unlocked by substituting this external-source mechanism.
Native C callers transfer one counted source reference per refptr argument;
the generated C++ wrapper does this automatically. This is CEF's existing
reference-transfer contract, not a retained key-source lifetime in the profile.
The separate OS-keyring source uses the same engine contract. Provisioning is
separate from lookup: `KeySource::Create` defaults to `Derive` for password and
external sources, while Secret Service provisions only on creation. Unlock is
always lookup-only.

### Linux Secret Service

`AlloyCreateProfileWithKeyring(path, result)` selects the built-in OS-keyring
source. `UnlockWithKeyring()` returns `alloy_keyring_result_t`: OK, UNAVAILABLE,
LOCKED, MISSING or FAILED. MISSING includes an absent default collection or
profile entry; FAILED covers source mismatch, protocol, integrity and I/O errors.
Every failure refuses profile admission. Password and external APIs are still
explicitly selectable and cannot substitute for a stored keyring source.

This follows pinned Chromium's `FreedesktopSecretKeyProvider` Secret Service
protocol and `//dbus` access library (the upstream `gnome-libsecret` option name
does not mean that revision uses libsecret). It does not call OSCrypt or reuse
its global Safe Storage secret, KDF, fixed/basic fallback, KWallet selection,
automatic service activation or metrics. It requires an already-running provider
and unlocked default collection on the local Unix session bus. Locked keyrings
are errors: the embedder/user can unlock the keyring separately and retry.
Alloy never starts a daemon, creates a collection or launches an unlock prompt.

Each profile has a fresh random 256-bit KEK. The existing random source salt is
its opaque lookup ID; attributes contain only `application=alloy`,
`xdg:schema=org.alloy.ProfileKey.v1` and `profile=<64 hex digits>`. The label is
`Alloy profile key`; no path, domain, profile name, history or password is exposed
to keyring labels/search attributes. Creation refuses duplicate matches and
reads back the committed secret. Lookup refuses missing/ambiguous entries,
malformed secrets, incorrect session binding or unexpected protocol responses.
Deleting the entry makes the profile inaccessible; unlock never regenerates it.
An interrupted/failed profile creation can leave an unreachable opaque keyring
entry; it cannot create a working plaintext/fixed-key profile or replace an
existing profile's key.

Each blocking operation uses a private D-Bus connection, resolves the existing
unique service owner once, disables activation on requests, bounds each call to
three seconds, closes its Secret Service session and shuts down the connection.
The local `plain` Secret Service session is the same transport used by Chromium;
the session bus and provider are trusted with the KEK. Engine-owned key buffers
and serialized secret byte arrays are cleansed before release. No source key or
connection is retained across calls; normal profile lock revocation/wiping is
shared with the password source. Locking Alloy does not lock the user's entire
keyring or erase its persistent entry. Keyring backup/OS access policy remains
the provider's responsibility. Wakaba uses password profiles, not this source.

## Bootstrap and profile-private state

The root's public bootstrap is 56 source-descriptor bytes followed by an 80-byte
authenticated profile-key envelope. The descriptor contains big-endian format,
source kind, Argon version/memory/pass/lane fields and 32 salt bytes. Format-1
password parameters must match exactly; other source kinds have zero KDF fields.
Unknown versions, changed parameters and oversized descriptors are rejected
before KDF work or provider callbacks. Both the wrapped key and authoritative
root authenticate this descriptor. A changing bootstrap cannot bypass source
authentication during an unlock.

Profile-private files live in a separate encrypted namespace with profile-derived
keys. It has a reserved non-domain catalogue kind and opaque HMAC directory;
it never appears in the site-record list and cannot be site-deleted. Deleting
all site records leaves profile-private settings intact. This is the same
coordinator/catalogue/root protocol, not another authoritative index or a
plaintext installation envelope. Site record domains and all private metadata
remain encrypted; there is no shared empty-domain record for opaque principals.
Private capabilities are limited to explicit profile settings, certificate,
custom-filter, dictionary and asset purposes. Site activity purposes (including
cookies, history and site settings) cannot open that tree, and site records
cannot issue profile-private purposes. This scope check is enforced at file
admission as well as capability issuance.

Wrong credentials fail without admitting reads or writes. A corrupted
authoritative root fails without repair, raze, old-root selection or native
plaintext fallback. Record HMAC names cannot be tested by plain domain hashes.
All engine-owned profile, record and file keys are wiped on lock, including
retained old handles. Forgotten password data has no recovery mechanism.

## Native contexts and site settings

After `CefInitialize`, call `AlloyCreateEncryptedRequestContext` on browser UI
with the explicit unlocked profile. Its boolean return means queued; the
callback supplies a context only with `ALLOY_SETTINGS_OK`. Native preferences
use that profile's original settings projection before Profile construction,
without an ordinary Preferences file or parent-profile fallback. CEF sharing
preserves the same native owner.

Permissions/content settings, host zoom, Shields settings/seeds, preferred link
handlers and managed-origin configuration use their site's record. Explicit
non-site configuration, including global search choice and profile attributes,
uses the encrypted profile-level namespace. Process-wide Local State is always
memory-only; install-wide startup settings are embedder inputs.

`AlloyProfile::DeleteSiteRecord` queues key-first deletion on UI and acknowledges
only after native documents, workers, grants and generated references retire.
Unrelated records and user-selected external files are preserved. `LockAsync`
revokes the original context's keys, native owners and preference caches before
completion. Unlocking does not reauthorize an old context.

### Native first-party working state

Record deletion also retires the native working state of every attached request
context, not just its encrypted files. A captured record lease closes admission
before teardown. Replacement admission opens only after the native owners and
the record coordinator finish; a context created during deletion observes the
same shared admission block. Old documents, pending navigations and retained
storage endpoints cannot adopt the replacement lease.

| # | Native state | Retirement owner |
|---|---|---|
| 1 | Cookies, including partitioned cookies and bound script-cookie endpoints | Native network context and cookie store |
| 2 | localStorage and sessionStorage, including namespace clones | DOM storage service; old area endpoints are disconnected |
| 3 | IndexedDB, Cache Storage, OPFS, managed filesystems and quota metadata | Native quota clients, with deletion statuses joined |
| 4 | Service workers, script resources and dependent background registrations | Existing worker teardown and native storage deletion |
| 5 | HTTP and No-Vary-Search caches | Native resource and isolation-key ownership |
| 6 | Generated code, media salts and persistent origin-trial working state | Record-owned native service instances |
| 7 | HSTS, server properties, Reporting/NEL configuration, auth, DNS and client-certificate decisions | Typed native network owners; unrelated sites and proxy authentication are preserved |
| 8 | History, visited-link projections, favicon mappings and download metadata | Native history, favicon and download owners; mappings without a history visit are included |
| 9 | Notifications and CDM working files | Native endpoint and data retirement; this does not enable persistent CDM licenses |

The existing targeted transport barrier drains old network writers before
native data is removed. It also retires CORS preflight, shared-dictionary and
connection state. Native failure or a dropped storage reply is not reported as
successful deletion. Another site's own data remains intact, including shared
favicon content still referenced by that site's pages. User-selected external
files are never deleted by this operation.

Generated-code caches currently use a bounded-admission fallback, not a global
entry LRU. One process-wide pool reserves at most 80 MiB of native cache
capacities, matching Chromium's default disk code-cache size. Each cache bundle
reserves 16 MiB: a 4 MiB limit for each of JS, Wasm, the JS memory front-cache
and the persistent/source-keyed collection. Thus at most five bundles are
admitted across records, contexts and partitions. Capacity returns only after
native cache shutdown runs. Additional caches are refused while full; pages
still execute normally, without those optional cached compilations. This is
cache-capacity accounting, not a total-process RSS bound. Other record state
is neither evicted nor recreated to make room. Entry-level size-triggered LRU
is tracked in [issue 13](https://github.com/atmosphera-dev/alloy/issues/13).

The profile and context embed-list accessors share the same encrypted owner;
guest context accessors use memory only, and stateless access is refused. See
[embed-list settings](embed-list-settings.md) for callback, validation and
publication semantics, and [the system-state boundary](stateless-contexts.md#user-level-system-state)
for fontconfig indexes outside Alloy profiles.

## Verification consumers

`alloy_site_records_unittests` covers the hierarchy, production password source,
official Argon2id known-answer vector, wrong-password/locked behavior, encrypted
metadata/names, private-state persistence, real key clearing, provider reentry
and cancellation. The inherited file-layer crash/integrity tests still run.

`tools/profile_probe.py --check core` is the native API proof on the assigned
build/test machine. It drives the generated C ABI and a C++ wrapper consumer
before `CefInitialize`, including custom provider transport/refcounts, Unicode
passwords, reopen, wrong credentials, cancellation, encrypted marker scans and
a corrupted-root refusal that preserves the on-disk files. Store/browser probes
are added with the corresponding integrations.

`tools/profile_probe.py --check keyring` runs generated C ABI/C++ wrapper and
cold-process native consumers in an explicitly owned throwaway gnome-keyring on
a fresh private D-Bus session, private HOME/XDG directories and disk TMPDIR.
It proves five records with real SQLite, LevelDB and Simple Cache stores, record
files and profile-private settings survive create, lock, process exit and keyring
unlock; encrypted names/data contain no fixture markers; locked/missing/unavailable
providers fail without changing profile bytes; removed entries are not recreated;
an absent provider is not activated. The storage unit suite can run through
`tools/test_keyring.py --root <new-B-evidence-path> -- <suite argv...>` for actual
provider, per-profile isolation, missing-key and key-wipe tests. Without that
explicit private-session opt-in, tests requiring a provider skip rather than
access a real user keyring. Item 6 adds browser store routing: this foundational
probe does not yet prove cookie/localStorage/IndexedDB/browser-cache persistence.
