# Experimental stateless request contexts

`AlloyCreateStatelessRequestContext(network_state, handler)` creates an isolated
request context whose storage mode is immutable. The C++ declaration is in
`include/alloy/alloy_request_context.h`; CEF's translator generates the C entry
point `alloy_create_stateless_request_context` and its enum declarations.
This Alloy API is **experimental** and follows CEF's initialization, threading
and reference-counting conventions. An invalid network option or invalid CEF
initialization state returns a null context.

## Storage and document state

Stateless storage is refused before admission, including in-memory storage:
cookies (including Brave's temporary third-party jars), DOM/session storage,
IndexedDB, Cache Storage, managed origin-private filesystems, service workers,
HTTP/code/renderer caches and retained page-restore payloads. A temporary store
that accepts a value and clears it later does not satisfy this mode. Callers
must handle the web APIs' normal unavailable-storage errors.

Each tab has its own volatile Back/Forward stack of URLs. A traversal loads
the URL afresh: it does not restore a cached document, form values, scroll
position, history state or a completed POST body. Going back and following a
new link discards the forward part. Closing the tab discards its stack, even
when another tab keeps the shared request context alive. The open document and
its in-flight operations retain only their required working state.

Sharing the context through CEF's sharing-context API preserves its mode and
network option. It does not merge tabs' navigation stacks. Mutable parent
preferences are not inherited; registered defaults and explicitly supplied
process-wide policy/command-line inputs remain configuration inputs.

### User-level system state

Fontconfig's font index is user-level system state, outside Alloy profiles in
all three modes. Browser-owned profile and cache roots retain their zero-new-files
boundary in stateless and memory-only operation. The probes give the browser a
separate `XDG_CACHE_HOME` outside those measured roots and assert that it contains
only `fontconfig/*.cache-*` files and `CACHEDIR.TAG`; any other state fails.
`HOME` remains separate and measured. This boundary does not authorize site data,
browser caches or preferences in the system-cache directory.

The probes likewise isolate `XDG_RUNTIME_DIR` as user-level desktop state,
outside measured browser roots, and permit only `dconf/user` there. Unknown
files and symlinks fail the boundary. This does not relax the browser-root
assertions. GPU, Dawn WebGPU and Graphite disk caches are disabled by Alloy's
storage authority in every context mode, including encrypted profiles; these
caches are not encrypted or relocated into a system-state directory.

Native permission, chooser, external-file and certificate decisions belong to
their original document or pending navigation. Their working state is retired
before teardown callbacks; a held callback or live handle cannot authorize a
replacement document. Explicitly selected external user files remain usable
through the native file API while their owner is live; they are not managed
origin-private storage.

The native extension configuration/runtime graph has a private, in-memory
namespace for this context. It does not read the persistent parent's registry,
preferences, state stores or runtime. Only the component/built-in extensions
already supplied by the product may run; extension installation and persistent
extension storage are unavailable. This does not enable retained activity
services such as MostVisited or InstantService.

## Managed network state

| Option | Lifetime |
|---|---|
| `ALLOY_NETWORK_STATE_KEEP` | Managed DNS, sockets, TLS-session, dynamic HSTS, Alt-Svc and authentication working state may be reused within the request context |
| `ALLOY_NETWORK_STATE_CUT` | Each **new top-level document** gets a fresh managed network generation, including reload and Back/Forward. Redirect hops within that navigation use the same generation. Subframes and dedicated workers use their owning document's generation |

Fragment changes and `history.pushState`/`history.replaceState` are
**same-document navigations** and do not rotate the network generation. The
page's existing JavaScript memory and in-flight work continue. The next
cross-document navigation gets a fresh generation.

Generations coexist for independently live documents. Navigating one tab does
not reset another tab's context or change its StoragePartition. Ordinary opener
relationships and native redirect/POST handling remain intact. Live policy and
certificate configuration updates apply to all live generations.

The network option covers Alloy-managed state. It cannot erase OS resolver
caches, server-side observations or state outside the browser. Neither option
permits cookie, site-storage or content-cache admission.
