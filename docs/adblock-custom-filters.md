<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental custom-filter API

`include/alloy/alloy_request_context.h` exposes `AlloyGetAdBlockCustomFilters`
and `AlloySetAdBlockCustomFilters`. C clients use the generated
`include/capi/alloy/alloy_request_context_capi.h`; C++ clients use the generated
CEF wrapper. Select the experimental API and perform CEF API-hash negotiation.

Call on browser UI after the explicit Alloy request context has initialized.
Both functions return false for missing, stateless, lock-pending, locked, closing or
unavailable contexts. Get clears its output on refusal; true with an empty
string means an admitted empty filter list. No process/global or parent-profile
fallback is selected. Independent contexts cannot read or overwrite each other's
filters. CEF sharing retains the same original owner.

Memory-only contexts retain filters in RAM for their context lifetime. Encrypted
contexts use their original profile's encrypted custom-filter storage. Calls
resolve the existing native AdBlock broker/provider and retain its admission,
generation and teardown behavior. The caller owns input/output plaintext strings.

Set replaces the complete native custom-filter list; an empty string clears it.
True acknowledges native
mutation admission, not completed encrypted I/O or an asynchronous engine rebuild.
Observe the native request result to synchronize on matching; profile lock
completion remains the durable flush and native-retirement fence. Locked or
retired context references cannot reopen the provider or select another owner.
Once `LockAsync` admits a lock, a subsequent Set explicitly returns false, even
before the lock callback runs. Earlier accepted edits drain before key revocation;
the refused replacement is not stored and does not turn a successful lock into
a storage failure. Actual storage errors still propagate through lock completion.

The test-only `alloy_encrypted_adblock_consumer` uses OSR/Alloy-style browsers and
the public API, with an owned loopback page and positive fetch controls. It does
not depend on SettingsUI or a Chrome-style/Views window.
