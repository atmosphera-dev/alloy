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
The separate OS-keyring source is added through the same engine contract.

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
