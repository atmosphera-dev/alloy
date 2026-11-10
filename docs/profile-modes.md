<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Three request-context modes

Choose a mode explicitly per context. The choice is immutable; CEF sharing
retains the same mode/owner, not an independent session. No factory silently
falls back to the global context or another profile.

| # | Mode | C++ factory | State lifetime |
|---|---|---|---|
| 1 | Stateless | `AlloyCreateStatelessRequestContext(network_state, handler)` | Storage refused, including memory cookies, storage and caches. Each tab keeps only its URL-only Back/Forward stack until close |
| 2 | Memory-only | `AlloyCreateMemoryRequestContext(handler)` | Storage in RAM only, never read from a persistent parent or written to disk. Release every context/sharing reference to end the session |
| 3 | Encrypted | `AlloyCreateEncryptedRequestContext(profile, handler, callback)` | The explicit unlocked profile's encrypted records; only callback OK supplies a context |

The stateless/memory factories follow `CefRequestContext::CreateContext`
calling semantics and return null on refusal. Encrypted creation is asynchronous
on browser UI after `CefInitialize`. Its callback can report cancellation even
when the queueing call returned true. `handler` is optional in each factory.
Native C uses the generated snake_case equivalents.

Stateless `KEEP` permits context-lifetime managed network working state.
`CUT` uses a fresh managed DNS/socket/TLS/HSTS/auth generation for every new
top-level document, including reload and Back/Forward, not fragment/history-API
changes. Neither option enables storage. See [stateless contexts](stateless-contexts.md)
for the working-state and system-cache boundaries.

Encrypted lock retires original browsers, workers, grants and native caches.
Unlock with the chosen key source and create a new context; retaining an old
context cannot restore admission. A locked profile does not automatically
become a guest or stateless context. The embedder chooses that UI policy.

Embedded state is memory-only unless an explicit considered-first-party entry
selects the embedded site's own real record. `AlloySetEmbeddedStateScope` must
run on browser UI before the context's first network document. Choose
`per-outer-site` (default, 30-second keepalive) or `per-tab` (tab plus outer site,
retired immediately on close or moving to a different outer site).

Files deliberately selected through a native file picker remain external user
files, not site-record data. Fontconfig/dconf system-state boundaries are
documented separately; they never permit moving site state outside a record.
