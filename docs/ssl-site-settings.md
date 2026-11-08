<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native SSL site settings and original admission

Default-partition learned certificate decisions and HTTP/HTTPS decisions use the
native HostContentSettingsMap projection backed by each site's encrypted settings.
The native SSL delegate additionally owns nondefault-partition certificate grants,
mixed-content/certificate-error host state, and the HTTPS lists' volatile copies.
Its registration in the same native retirement traversal removes selected live
copies during preparation, using trusted site matching supplied by the engine.
Nested list owners prune their own volatile collections without preference writes
or external notifications. Delegate shutdown removes its registration before the
settings-map dependency is destroyed.

Shared profile persistence does not grant a retired document fresh admission.
For every managed Alloy context, native SSL entry points and certificate handlers
resolve the original navigation/document owner. A retired or mismatched owner
cannot resolve the shared delegate. Stateless contexts retain their private
document working stores; legacy contexts retain their native profile behaviour.
The request reply owner checks admission again immediately before continuing,
including after an embedder decision or reentrant native preference publication.
An unrelated site's original request remains usable.

Native SSLManager intentionally revokes learned exceptions when a clean TLS
response is observed for that host. Cold-read tests of an accepted certificate
error therefore supply that actual error in the simulated SSLInfo. A separate
clean-response consumer observes selective revocation while another site's grant
survives. The security algorithm is the same for encrypted settings.

Delegate metadata and request-reply admission are distinct from cached SSLConfig
and socket/session authority in the network layer. Clearing the delegate is not
a transport completion fence; that layer needs its own retirement consumer and
actual completion proof in the shared barrier.
