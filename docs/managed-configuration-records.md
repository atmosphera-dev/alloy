<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native managed-configuration cache ownership

The native `ManagedConfigurationAPI` owns one binding per admitted security
origin. Its browser/Profile factory supplies a `PrefService`, a store binding
factory and a network-factory getter. The component does not query its Profile
parent or choose a storage mode from a pathname. An encrypted binding supplies
the existing capability-bound native ValueStore/LevelDB Env, with admission
checked before cached reads and I/O. The binding's factory must select an
already browser-admitted record generation; background completion cannot create
a record or reacquire authority.

## One snapshot, one publication

The native ValueStore contains one `managed-configuration-snapshot-v1` entry:
format version, exact serialized origin (including scheme/port), version hash
and complete configuration. A single native Set batch publishes it. A separate
last-hash profile preference no longer acknowledges work before the async cache
write. Hash-only updates persist without reporting a data change; failed writes
cannot advance the cached version or suppress restart retry. Empty cache,
unchanged data and errors are distinct native results.

Configuration keys are literal strings. Each value is serialized for the web
API using the existing JSON-string representation; dots in a key do not create
nested preference paths. Malformed downloads leave the last valid snapshot
intact. Malformed snapshots or mismatched origins are errors, never an empty
cache, native repair/raze or a successful version acknowledgement.

## Asynchronous authority

Each binding owns its SequenceBound store, current revision and downloader.
Version queries, network replies, writes, reads and observer delivery retain
the original binding revision and admission. Old completions cannot cancel a
new downloader, publish over a new policy, return data from a replacement
generation or create a store. RevokeOrigin removes authority before child
teardown. Explicit refresh after fresh browser admission may bind a new
generation. Configuration policy is external input; deletion does not rewrite
that input, and it may reassert only under the embedder's agreed fresh-admission
contract.

## Verification scope

`ManagedConfigurationTest` exercises the real native API, SimpleURLLoader,
TestURLLoaderFactory, encrypted ValueStore and consumer callbacks. It covers
cold process readback, version/data publication failure, original-generation
revocation, late network/policy callbacks, namespace isolation, reentrant
notifications and malformed input. These native component checks do not prove
the pending encrypted CEF request-context/factory binding or the final encrypted
browser consumer probe.
