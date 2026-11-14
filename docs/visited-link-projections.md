<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native visited-link projections

Alloy memory contexts intentionally use a native off-the-record identity but
admit resident site state. Both partitioned and legacy visited-link writers
capture the original RequestContextPolicy capability for kVisitedLinks before
initialization. That capability, not the OTR boolean, controls initialization,
new links, origin salts and asynchronous rebuild publication.

Stateless, locked/revoked and ordinary unconfigured off-the-record contexts
refuse admission. A retained writer cannot acquire a replacement generation.
Native projections in every configured Alloy mode stay RAM-only; real encrypted
record persistence remains with the existing sharded record adapters. Both the
legacy writer and ContentVisitDelegate apply this rule even when their caller
requests native file persistence. Unconfigured regular Chromium contexts retain
their upstream persistence choice.

`alloy_visited_link_unittests` exercises both actual native writers, including
late adds and rebuild completion after revocation. This is admission/lifetime
coverage, not a claim that a previously transferred shared-memory mapping can
be physically revoked from another process.
