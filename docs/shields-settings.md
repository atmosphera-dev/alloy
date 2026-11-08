<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Native Shields settings and seeds

Functional Shields controls remain native HostContentSettingsMap settings.
SitePrefStore classifies them using the native registered setting identities and
routes each site's preference fragments through that site's encrypted record.
It does not treat all `brave.*` preferences as a global exemption.

The native Shields service keeps a random base `farbling_token` in the site's
Shields metadata. Its native additional-container entropy and optional random
profile-session entropy affect the returned value without becoming a persistent
profile-wide seed from which a deleted identity can be reconstructed. Deleting
a record removes its base metadata; a subsequent explicitly fresh acquisition
gets a new random base. Other sites retain their own base values.

Shields usage, manual-action, domain-count and reporting-revision projections
are not functional settings. Alloy's native P3A compatibility entry points do
not inspect sites, update preferences, retain metrics or schedule reports. Their
zero/default preference registrations remain only for native reader/schema
compatibility; they do not create retained user values.

The browser blocked-event producer no longer increments global blocked counters
or retains their URL-dedup set. Its separate immediate event dispatch and native
allow-scripts-once behavior remain. The auxiliary bandwidth-savings predictor/
P3A tracker is not attached to WebContents and is not fed blocked events.
Legacy Android counter-import entry points do not import those counters; Linux
verification does not establish Android runtime behavior.

`ShieldsSettingsTest` uses the real native service, HostContentSettingsMap and
encrypted preference adapters to exercise two-site control/base-seed cold
readback, selective metadata deletion and fresh identity, native session-entropy
behavior, and functional changes without retained P3A state. The Strict-mode
assertion explicitly enables the native Strict feature, as its upstream test
does; production feature defaults are not changed.

These tests cover native settings/seed persistence and counter non-retention.
Renderer/worker copies of returned values still require actual context lifecycle
teardown. Native test stores model encrypted profile/local preferences; they do
not establish the production pre-profile Local State selector or the complete
encrypted CEF request-context/embedded-state behavior.

## Derived native cookie settings

The preference provider owns both base content-setting rules and its derived
cookie rules, including Shields-down and explicit sign-in exceptions. Base-rule
preparation calls the provider's injected dependent-projection callback before
any ordinary notification. The callback installs the derived maps and returns
their deferred publication. Ordinary native edits, clearing and preference
replacement use this same path; no observer event is needed to refresh a cache.
The provider does not subscribe to its own ordinary events to rebuild its maps.

During record retirement, the shared settings owner holds base and dependent
publications until the native drain completes. Native queries see the pruned
configuration while old-directory admission remains closed; ordinary preference
and content-setting observers receive no early event. Other sites' rules and
explicit profile-wide defaults remain separate. The native held-drain cookie
consumers also reconstruct the encrypted store and native services for readback.
