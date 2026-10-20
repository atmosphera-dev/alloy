<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Verification socket classes

`alloy_enable_class_marking` defaults to false. Only `tools/args/verify.gn`
enables it. The product has no marking state, DSCP selection or socket-option
implementation. Verification requires no capabilities: it uses `IP_TOS` and
`IPV6_TCLASS`, never privileged socket marks.

The **single machine-readable class → name → six-bit DSCP table** is
[`src/net/base/alloy_traffic_classes.inc`](../src/net/base/alloy_traffic_classes.inc).
The guard and socket instrumentation include that table; harness readers must
consume those rows, not copy the numbers into a second mapping. All values use
the local/experimental `xxxx11` pool. The low two bits of the IP traffic-class
byte remain ECN; subsequent QUIC/WebRTC QoS changes cannot overwrite the class.
`no-class-known` is a distinct nonzero value, separate from a guard-classified
`unannotated` request. An unowned new IP socket starts with `no-class-known`.

Actual originating annotations travel through the existing socket-tag input:
legacy connect-job parameters, both HTTP stream-attempt paths (including nested
TLS), QUIC session keys (including migration/probing), dedicated WebTransport,
built-in DNS UDP/TCP, and retained raw-socket/P2P capabilities. Socket accounting
identity/equality and admission policy are unchanged. The marker describes the
socket's creating owner, not every multiplexed stream on a reused connection.
Native open/adoption applies the tag before connect/first send; retries and
accepted children retain it. Instrumentation failures are fatal in verification
builds rather than silently emitting an unmarked socket.

## System resolver and harness contract

**libc `getaddrinfo` traffic carries no class; permitted only to the configured
resolver.** The system resolver and its fallback behavior stay available in
both configurations. The harness must accept unmarked packets only for UDP/TCP
port 53 to that lane's configured resolver address, report the count separately,
and fail on unmarked traffic anywhere else. Host dnsmasq upstream queries belong
to the host and must retain their separate per-lane DNS evidence.

Count and capture classes inside the namespace boundary **before** resetting
DSCP to zero. Reset both IPv4 DSCP and IPv6 DSCP before egress. Independently
capture the namespace's NAT source on the physical uplink and require zero
nonzero-DSCP packets there. A namespace-only capture does not prove that marks
were stripped before leaving the machine. Product captures require DSCP zero.

Item 3's `harness/README.md` should link to this contract and consume the same
table. Item 2's source tests and namespace gate exercise the marking and the
system-resolver exception independently.

## Ownerless, non-sending route probes

Shared IPv6 reachability checks (`HostResolverManager::StartGloballyReachableCheck`)
and PAC `MyIpAddressImpl` public/private route selection have no request owner.
Their UDP `connect()` calls only select/query a local address and send no data.
They retain `no-class-known`; tests must reject any data-send operation and the
namespace capture must contain no packets from them. Address sorting for an
owned DNS task instead receives that task's actual owner. None of these probes
adds a wire exception: any outside `no-class-known` packet still fails.
