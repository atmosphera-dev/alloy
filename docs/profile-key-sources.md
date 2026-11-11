<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Profile key sources

Choose the source at creation. Open existing profiles with `AlloyOpenProfile`
while locked, then unlock with their original source. Failure never repairs,
replaces or opens the profile through a different source.

| # | Source | Create / unlock | Requirements |
|---|---|---|---|
| 1 | Password | `AlloyCreateProfile` / `UnlockWithPassword` | Argon2id over UTF-8 password bytes, random 32-byte salt; malformed Unicode refused |
| 2 | Linux Secret Service | `AlloyCreateProfileWithKeyring` / `UnlockWithKeyring` | Already-running provider and unlocked default collection; no prompt, activation or fallback |
| 3 | Embedder provider | `AlloyCreateProfileWithKeySource` / `UnlockWithKeySource` | `AlloyKeySource::DeriveKey` fills a 32-byte key from a borrowed 32-byte public salt |

All these calls are blocking background-thread operations. External derivation
runs synchronously on the calling thread, outside the lifecycle mutex. The
profile does not retain the provider. Derive the same key for the same stored
salt; return false on failure/cancellation. Never retain borrowed buffers.

The source yields a key-encryption key (KEK), not a direct file key. It wraps
a random profile key; independent site-generation keys are wrapped in the
encrypted catalogue. Domain identities and metadata are encrypted too, with
keyed opaque disk names. Lock wipes engine-owned keys, not the embedder's
credential strings or copies. The embedder must clear those itself.

Keyring operations distinguish `ALLOY_KEYRING_OK`, `UNAVAILABLE`, `LOCKED`,
`MISSING` and `FAILED`. Creation provisions one random per-profile KEK; unlock
only looks it up. Removing that entry makes the profile inaccessible. Locking
Alloy neither locks the whole keyring nor deletes the entry. Keyring backup
and access policy belong to the provider. There is no password recovery route.

See [encrypted profiles](encrypted-profiles.md) for exact format/KDF and
Secret Service details, and [deletion limits](site-records.md#honest-deletion-boundary)
before describing deletion to users.
