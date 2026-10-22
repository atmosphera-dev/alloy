<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Engine storage capabilities

`brave/alloy/site_records/record_capability.h` defines the engine-only storage
admission contract. It is groundwork for the experimental request-context and
profile API, not a public CEF API or a claim that existing request contexts
already enforce the three modes.

The browser-side lifecycle owner issues a `RecordCapability` with an opaque
profile identity, record identity, generation, mode and store purpose. These
identities come from engine ownership, not a renderer message, URL parameter,
request header or native filename. Frame/navigation/worker adapters must resolve
that ownership before issuing a handle; a cached resource's URL alone does not
identify its record. Record lookup uses `RecordDomainForPrincipal` on a validated
browser-owned document/hop URL and origin: Chromium's SchemefulSite domain/host without
the scheme. This grouping never replaces origin or StorageKey security checks.
Opaque, file, data and about principals have no record, including about documents
that inherit a normal web origin. An origin alone cannot express that boundary.

Every owner checks `Allows(its_purpose, access)` before constructing, opening or
reading a backend. The captured purpose cannot be changed into another store's
authority. Stateless rejects cookie, DOM/IndexedDB/Cache Storage, cache, history,
worker and other storage admission, including in-memory backends. Live document
and network working state have separate purposes; neither permits record files
or background jobs. Their lifecycle owners must bound their lifetime. Memory
mode admits memory stores but never record files. Encrypted mode admits its own
record's files. None permits consulting a parent profile.

The issuer owns a shared, monotonically revocable generation. Revocation or
issuer destruction invalidates all copies, including pending operations on
other sequences. An asynchronous owner must recheck admission at its write/
callback boundary and implement the actual teardown and write barrier; a
boolean check alone is not an atomic deletion protocol. Recreating a record
uses a new generation. Keys, encryption, the durable catalogue, deletion
publication and owner adapters are separate layers, not responsibilities of
this value type. Embedded-memory/document issuers use transient identities;
they do not authorize persistent records or decide embedded-storage exceptions.

NUC-only unit verification, in `/home/builder/work/alloy` with
`TMPDIR=/home/builder/work/tmp`:

```sh
python3 repo/tools/alloy_build.py --config release --target alloy_site_records_unittests
src/out/Release_Alloy/alloy_site_records_unittests --test-launcher-retry-limit=0
python3 repo/tools/alloy_build.py --config verify --target alloy_site_records_unittests
src/out/Verify_Alloy/alloy_site_records_unittests --test-launcher-retry-limit=0
```

Run builds under the documented durable user-unit supervisor. The suite checks
admission, purpose/owner isolation, cross-sequence revocation, owner teardown,
generation recreation and record-domain grouping. Browser/probe verification
must additionally establish enforcement at actual store consumers.
