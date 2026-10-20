#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""One-pass phone-home extraction from the retained soak02 data (read-only).

Reads per lane: dnsmasq.log (query names) and proxy.jsonl (observe lanes:
SNI, requests, responses). Writes JSON + a markdown summary to OUT.
Excludes Kiwix (nomad.home) and the dry-run canary (example.com) only by
labelling, never by dropping.
"""
import argparse, collections, json, os, re, sys, datetime
from hosts import label

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--campaign', required=True)
parser.add_argument('--lane', action='append', required=True)
parser.add_argument('--origin', type=float, required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
C, OUT, LANES = args.campaign, args.out, args.lane
T0 = datetime.datetime.fromtimestamp(args.origin, datetime.timezone.utc)
QRE = re.compile(r"^(\S+) dnsmasq\[\d+\]: \d+ \S+ query\[(\w+)\] (\S+) from")


def hour(ts):
    return (datetime.datetime.fromisoformat(ts) - T0).total_seconds() / 3600


res = {}
for lane in LANES:
    d = os.path.join(C, lane)
    dns = collections.defaultdict(lambda: {"n": 0, "first_h": None, "last_h": None, "types": set()})
    with open(os.path.join(d, "dnsmasq.log"), errors="replace") as f:
        for line in f:
            m = QRE.match(line)
            if not m:
                continue
            h = hour(m.group(1)); name = m.group(3).lower().rstrip(".")
            e = dns[name]; e["n"] += 1; e["types"].add(m.group(2))
            e["first_h"] = h if e["first_h"] is None else min(e["first_h"], h)
            e["last_h"] = h if e["last_h"] is None else max(e["last_h"], h)
    proxy = collections.defaultdict(lambda: {"sni": 0, "req": collections.Counter(), "status": collections.Counter(), "req_bytes": 0, "resp_bytes": 0, "first_h": None})
    pj = os.path.join(d, "proxy.jsonl")
    if os.path.exists(pj):
        with open(pj, errors="replace") as f:
            for line in f:
                try:
                    r = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError('Malformed complete proxy record') from exc
                host = (r.get("sni") or r.get("host") or "").lower()
                if not host:
                    continue
                e = proxy[host]; h = hour(r["time"])
                e["first_h"] = h if e["first_h"] is None else min(e["first_h"], h)
                ev = r.get("event")
                if ev == "tls_clienthello":
                    e["sni"] += 1
                elif ev == "request":
                    e["req"][f'{r.get("method")} {r.get("path")}'] += 1
                    e["req_bytes"] += r.get("request_bytes") or 0
                elif ev == "response":
                    e["status"][str(r.get("status"))] += 1
                    e["resp_bytes"] += r.get("response_bytes") or 0
    res[lane] = {
        "dns": {k: {**v, "types": sorted(v["types"]), "label": label(k, v["types"])} for k, v in dns.items()},
        "proxy": {k: {**v, "req": dict(v["req"].most_common(15)), "status": dict(v["status"]), "label": label(k)} for k, v in proxy.items()},
    }

os.makedirs(OUT, exist_ok=True)
json.dump(res, open(os.path.join(OUT, "quick-extract.json"), "w"), indent=1, default=list)

md = ["# Quick extraction — soak02 (read-only, one pass)\n"]
for lane in LANES:
    dns = res[lane]["dns"]; px = res[lane]["proxy"]
    out = {k: v for k, v in dns.items() if v["label"] == "outside"}
    md.append(f"\n## {lane}\n\nOutside DNS names: {len(out)}\n")
    md.append("| # | Name | Queries | First seen (h) | Last seen (h) |\n|---|---|---|---|---|")
    for i, (k, v) in enumerate(sorted(out.items(), key=lambda kv: kv[1]["first_h"]), 1):
        md.append(f"| {i} | {k} | {v['n']} | {v['first_h']:.1f} | {v['last_h']:.1f} |")
    pout = {k: v for k, v in px.items() if v["label"] == "outside"}
    if pout:
        md.append("\nProxy (observe) — requests per host:\n")
        md.append("| # | Host | TLS hellos | Requests (method path: count) | Status | Req bytes | Resp bytes |\n|---|---|---|---|---|---|---|")
        for i, (k, v) in enumerate(sorted(pout.items(), key=lambda kv: kv[1]["first_h"]), 1):
            reqs = "; ".join(f"{a}: {b}" for a, b in list(v["req"].items())[:6])
            md.append(f"| {i} | {k} | {v['sni']} | {reqs} | {v['status']} | {v['req_bytes']} | {v['resp_bytes']} |")
open(os.path.join(OUT, "quick-extract.md"), "w").write("\n".join(md) + "\n")
print("OK", OUT)
