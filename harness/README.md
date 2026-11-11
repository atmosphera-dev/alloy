<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Alloy phone-home regression harness

Run the verification build after an engine update (**gate: 2 hours**) and before
a release (**release: 24 hours**). Durations are fixed per lane; no convergence,
automatic extension or browser-recovery replay is used. This is test-only
instrumentation. Production Alloy never records requests.

## Execution and inputs

This port retains the approved dedicated **physical NUC `wakaba-build.home`**, account
`builder`, workspace `/home/builder/work/alloy`, disk-backed
`TMPDIR=/home/builder/work/tmp`. It is not a VM. The laptop edits/delivers source
and collects evidence only. If this NUC is unavailable, stop rather than fall
back. Other execution targets need their own authorization and topology review.

The second approved workspace `/home/builder/work/alloy-b` is selected by its
delivered `repo/` location and uses its own disk-backed `tmp/`. See
[`../tools/README.md`](../tools/README.md#second-owned-build-workspace).
Use `alloy-b-…` unit/run names there. Its routed lanes use B-only names,
`10.77.3.0/30` / `10.77.4.0/30`, proxy ports 18083/18084 and nft table
`alloy_b_harness`; scheduler and controller still acquire the original
workspace's shared `harness-run.lock` / `controller.lock`. A config's `workspace`
records that selection for immutable run snapshots and provider subprocesses.
The selection must contain the harness source location: a config cannot switch
code delivered in B into A (or vice versa). This check also covers run snapshots.

| # | Input | Contract |
|---|---|---|
| 1 | `--build` | Existing CEF output directory with `cefclient`, `libcef.so`, `args.gn`; guard log, canary and class-marking flags must all be true. Build with `tools/alloy_build.py --config verify` |
| 2 | `--mode gate\|release` | Required; 7200 / 86400 measured seconds per lane |
| 3 | `--lanes observe,enforce` | Default both; a single lane is also supported, without changing its policy |
| 4 | `--out` | New exclusive run directory beneath the NUC workspace; default `harness-runs/alloy-<mode>-<UTC>` |
| 5 | Kiwix | `http://nomad.home:8090`, currently `10.4.20.237`; Atom catalog, at least two books including `wikipedia_en_all_mini_2025-12`; real article/search/book consumers |
| 6 | Network | Uplink `eno1`; host DNS `10.4.10.53`; free `10.77.1.0/30` and `10.77.2.0/30`; initially empty namespace/firewall state; free NAT ports 62000–62999 above host ephemeral range |
| 7 | Tools | Python 3.11+ and `websocket-client`; systemd user manager; passwordless `sudo` for owned namespace/system-unit operations; nft, iproute2, tcpdump, dnsmasq, mitmdump, certutil, curl, Xvfb, nvme-cli, rg. No package installation is performed by the harness |
| 8 | Analysis source | Composed Chromium/Brave tree inferred from `<build>/../..`; retained annotation definitions and XML catalog |

Alloy ships no lists or dictionaries. The launcher snapshots
`tools/testdata/alloy-data/` into the run-owned source directory and records its
path as `data_directory` in `config.json`; lanes pass that path through
`--alloy-data-dir`. These small fixtures test the engine's offline consumers,
not Wakaba's production policy. See
[`../docs/data-directory.md`](../docs/data-directory.md) for the embedder contract.

The original seeded walker, human-paced active/idle rhythm, local trigger suite,
daily renderer-crash attempt, page canary and startup guard canary are retained
from the phase-1 harness. Optional API failures remain explicit outcomes; an
attempt is not a claim that an unsupported API worked. Sandbox and browser
background-network behavior are not disabled to obtain a clean result.

## Running

On the **NUC**, cwd `/home/builder/work/alloy` (the delivered repository is `repo/`):

```sh
systemd-run --user --unit=alloy-03-gate-01 --service-type=exec \
  --property=WorkingDirectory=/home/builder/work/alloy \
  --property=RuntimeMaxSec=9600 --property=TimeoutStopSec=360 \
  --property=StandardOutput=append:/home/builder/work/alloy/logs/item03-gate-01.log \
  --property=StandardError=append:/home/builder/work/alloy/logs/item03-gate-01.log \
  --setenv=TMPDIR=/home/builder/work/tmp --setenv=PYTHONDONTWRITEBYTECODE=1 \
  python3 repo/harness/run.py --build src/out/Verify_Alloy --mode gate
```

For release, use a new unit name, `--mode release`, and `RuntimeMaxSec=88800`.
The runner elevates through `sudo -n`, retaining its supervising unit, and owns
separate transient system units for collection, lanes and providers. Run one
campaign at a time; workspace locks prevent duplicates. Query the user unit
with bounded `systemctl --user show <unit>` requests; `supervisor.json` records
the collector's exact invocation. A dropped SSH connection does not justify a
second launch. Request a controlled stop by touching `<run>/STOP` or stopping
the user unit; this is a failed/incomplete run, never a shorter passing gate.

## Measurement and class → DSCP source

The **single source** is
[`../src/net/base/alloy_traffic_classes.inc`](../src/net/base/alloy_traffic_classes.inc).
Both nft generation and the independent packet checker parse it directly;
there is no second numerical mapping here. The six-bit DSCP codepoint is not
the whole TOS byte; ECN remains separate. See
[`../docs/class-marking.md`](../docs/class-marking.md) for socket ownership,
multiplexing, marking and product-build exclusions.

**libc `getaddrinfo` traffic carries no class; permitted only to the configured
resolver.** Unmarked packets pass only when their **outer** destination is that
lane's gateway and their outer protocol is UDP/TCP with destination port 53.
Unmarked traffic anywhere else fails, including DNS sent to any other resolver.
`browser-service`, `unannotated`, `no-class-known`, unknown codepoints and
fragmented/unaccountable packets fail. Ownerless UDP route-selection probes
have no invented class and no exception: they must emit no packets, as covered
by the native item-2 tests. Allowed page classes include on-behalf-of-page
traffic; a class mark is the creating socket owner's class, not proof of every
stream multiplexed on it.

**Local kernel control is reported separately**, following phase2 answer03-1.
It requires an IPv4 ICMP or ICMPv6 error addressed to the lane gateway/resolver,
whose quoted inner packet is UDP/TCP port53 between that resolver and the
namespace. IPv6 neighbour-discovery/router-solicitation (types133–137) must be
addressed to the gateway or link-local/`ff02::/16` ND scope. An echo, an error
quoting unrelated traffic, or any ICMP to an outside address receives no
exception. Outer ICMP headers and quoted DNS headers remain distinct. This
accounts for kernel-generated packets (including CS6) rather than inventing
an Alloy socket class; the exact unmarked-DNS restriction is unchanged.
All64 raw DSCP codepoints are independently counted by nft, so local controls
and unexpected values remain reconcilable to packet evidence.

**Kernel TCP control of a marked flow** follows phase2 answer03-2: only DSCP0
TCP RST (optionally ACK) with zero payload, on the same5-tuple as an earlier
namespace packet marked with an allowed class, following an incoming segment
of that flow, within120s of its last marked packet. Report the count separately
with its parent class and timestamp/tuple proof samples. SYN, SYN-ACK, FIN,
payload-bearing packets, stale flows and unknown/unallowed ownership still
fail. Flow history survives capture rotations and live-monitor polls. This
does not broaden the ICMP rule or the configured-resolver DNS restriction.
Any third kernel-originated category requires a new explicit ruling.

DNS HTTPS service-binding owner `_8090._https.nomad.home` denotes the same
Kiwix port8090 input; it is labelled Kiwix only for an HTTPS record query.
Other names/subdomains and ordinary A/AAAA queries receive no such exclusion.
The raw owner name and record type remain in the report.

All lane, uplink and NFLOG capture providers request a 64 MiB receive buffer
(`tcpdump -B 65536`) through the same launcher; `capture-settings.json` records
the request. Immediate mode, full snap length and packet-flushed output remain
enabled. Buffer headroom does not relax the zero-kernel-drop or exact final
nft/capture reconciliation requirements.

Each lane counts classes **before NAT**, captures all veth traffic (including
Kiwix), and resets IPv4/IPv6 DSCP to zero in postrouting. A reserved per-lane NAT
source-port range identifies namespace egress on an independent physical-uplink
capture. Positive Kiwix packets must be present there with **zero nonzero DSCP**;
an empty capture cannot establish stripping. Transparent observe proxy upstream
sockets originate on the host and are recorded by the per-lane proxy, not
mislabelled as browser sockets.

Observe permits/logs outside attempts through the proxy/all-protocol capture;
enforce drops/logs them. Both seal host services, DNS bypass, QUIC and routed
IPv6, as in phase 1. The workload's only outside page contact is
`https://example.com/wakaba-canary`; the separately denied service canary is
`https://example.com/alloy-guard-canary`. Their paths and consumers must not be
conflated. Calibration runs before the recorded browser-collection boundary;
its raw captures and nft baseline remain in evidence.

## Early results and stop conditions

Gate produces `analysis/30m/summary.{json,md}` at 30 measured minutes; release
produces `analysis/1h/` and `analysis/6h/`. Full analysis runs in a separate,
600-second-bounded process while collection continues. Snapshot inputs have
fixed byte lengths/hashes; complete events are retained in order, partial live
tails are recorded and revisited at the next checkpoint. The one-pass quick
DNS/proxy extractor, protocol-aware IP map, NetLog relabeler and DSCP checker
live in `analysis/`. No cyclic NetLog history is used.

An incremental independent monitor reads new DNS/proxy/packet records between
full analyses. An outside non-page contact or invalid/unclassified packet
causes immediate stop/report. A failed/timed-out interim analysis also stops
collection with `NEEDS_ATTENTION.json`; the fixed duration is never extended.
Storage/controller incidents stop without reboot. Every result reports counts,
analysis runtime and projected final cost, not only process liveness.

## Outputs and pass criteria

| # | Output | Required result |
|---|---|---|
| 1 | `config.json`, `supervisor.json` | Exact CLI mode, target, seeds, lanes, binary/source hashes and supervised ownership |
| 2 | Per-lane captures, DNS/proxy streams, full NetLog, CDP, walker, profiles/guard logs | Raw evidence retained; Kiwix/local/control labels explicit; 0 outside non-page contacts by host |
| 3 | `analysis/<checkpoint>/summary.json` and `.md` | Complete analysis ≤600 s; 0 invalid DSCP/unmarked-elsewhere packets; resolver-unmarked count separately reported; nft/capture class counts reconciled (interim is a bounded prefix; final exact) |
| 4 | Guard/canary results | One startup canary denial and native `-138` result per lane; no guard-canary proxy request; no unknown/browser-service URLRequest starts |
| 5 | Walker consumers | Same phase-1 definitions/ranges: pages ≥76/77 (98.70%), searches ≥75%, book switches 100%; positive attempts and at least two distinct books per lane; all trigger attempts retained |
| 6 | `uplink-*.pcap` | Positive Kiwix packets; 0 nonzero-DSCP packets; zero capture kernel drops |
| 7 | `run-complete.json` | Both selected lanes reach the exact mode's fixed exposure target; no recovery replay or extension |
| 8 | `browser-shutdown.json`, `producer-quiesced.json`, `restored.json` | Normal browser exit, then provider drain; no namespaces/veths/nft tables/owned processes; forwarding and task-changed IPv4 settings restored |
| 9 | `result.json`, `SHA256SUMS` | All gates true; manifest sealed only after collector/ExecStopPost and all analysis writers have exited; repeated cleanup is byte-preserving |

Graceful close uses its own cancellation-free CDP phase, then waits for normal
browser exit. Forced termination is retained explicitly as a failed shutdown,
not passed off as normal. Cleanup retains stripping until namespace/process/veth
teardown is proven; inactive ownership returns before touching any receipt.
The scheduler alone owns the final seal after all producers/providers and
collector cleanup have settled.

Run meaningful parser regressions on the **NUC**, never on the laptop:

```sh
timeout 60 env TMPDIR=/home/builder/work/tmp PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest discover -s repo/harness/tests -v
```

Inspect a completed run without re-running collection (use a new unique tag for
historical readbacks; `final` is reserved for the owned finalization):

```sh
timeout 610 sudo -n env TMPDIR=/home/builder/work/tmp PYTHONDONTWRITEBYTECODE=1 \
  python3 repo/harness/analysis/report.py --campaign <run> --source src --tag review-01 \
  --out /home/builder/work/alloy/evidence/review-01
```

`--out` leaves the sealed run untouched. Verify delivered evidence with
`sha256sum --check SHA256SUMS` from that evidence directory.
