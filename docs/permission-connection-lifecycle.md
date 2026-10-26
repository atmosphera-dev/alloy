# Permission service connection lifetime

Blink's execution-context, document and WebRTC permission bindings share the
connection in `core/dom/permission_service_connection`. The native browser
remains the permission authority; the renderer connection owns completion of
forwarded replies.

An initial same-origin document transition can preserve the renderer global
object while replacing the browser's document-associated permission receiver.
Previously, a query queued on that receiver could disappear with its pipe and
leave the live page's promise pending. A connection now completes a dropped
reply using the browser's existing fail-closed cancellation representation:

| # | Operation | Receiver-retirement result |
|---|---|---|
|1|Query, single request or revoke|`DENIED`, with no permission details|
|2|Bulk request|One `DENIED` result per input descriptor|
|3|Embedded permission-control request|`kDenied`|

Successful native replies pass through unchanged. Observer and registration
endpoints retain their native pipe lifetimes. No permission decision is cached
by the connection. A subsequent independent operation binds the current native
receiver; an interrupted operation is never replayed. A connection-generation
check prevents an older disconnect notification from clearing a newer binding.

The connection's browser binder is supplied by its parent and weakly bound to
the execution context. Client pipe closure destroys the connection. All its
dispatch, cancellation and cleanup run on the permission task sequence. The
content media-permission dispatcher retains its existing separate pending-call
owner, which completes callbacks with `false` on navigation or disconnection.

The native `alloy_permission_lifecycle_unittests` suite deliberately closes an
actual Mojo receiver before dispatch, checks completion of every reply-bearing
operation and its bulk cardinality, and checks that a fresh operation uses a new
browser connection. Browser-level verification exercises the real permission
policy, grant and document-retirement consumers in both build configurations.
