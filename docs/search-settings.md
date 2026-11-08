<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Explicit search configuration

An explicitly chosen global search engine is non-browsing profile configuration.
It is encrypted in the profile-private settings shard. It does not create a
record for the provider, and deleting a visit record for that provider does not
erase the user's global choice. Discovered engines and their browsing metadata
are not reintroduced through this exemption.

The native configuration codec projects `TemplateURLData` dictionaries through
`CopySearchEngineConfiguration`. Last-visit time, usage count and the originating
OpenSearch description URL are learned activity/discovery facts, not engine
configuration. The shared projection excludes those fields at serialization,
deserialization, native pref-map production and every generated default-engine
mirror. Mirror comparisons use the same configuration projection. Policy-disable
flags and configuration fields outside TemplateURLData are preserved, and an
external policy/input dictionary is not rewritten by the mirror owner.

Functional engine definitions keep their native URLs, query/POST parameters,
encodings, alternate URLs, explicit options and user modification metadata.
The native DefaultSearchManager remains the producer and consumer; Alloy does
not substitute a copied settings implementation or infer ownership from URL-like
strings. Its native preferences flow through SitePrefStore into the existing
encrypted profile namespace.

`SearchSettingsTest` exercises the actual native manager, resolver, pref-map
producer and dictionary reader against the encrypted store: configuration and
its mirror cold-reopen, learned facts are excluded, raw domain scans find no
plaintext, and the explicit global choice survives deletion of the provider's
separate site record. This is the native settings boundary. Browser Profile/
request-context selection, complete lock teardown and the final encrypted CEF
consumer remain separate integration requirements.
