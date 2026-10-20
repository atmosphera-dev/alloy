#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Port of the soak quick-ip-map: one pass, with outer-protocol-aware decoding.

Keep every protocol, unlike tcpdump text parsing of quoted ICMP payloads.
DNS labels are evidence, never a reason to discard a packet.
"""
import argparse
import collections
import json
from pathlib import Path
import re
from dscp import packets, egress, local_control


def ip_map(campaign, lane):
    directory = campaign / lane['name']
    names = collections.defaultdict(set)
    for line in (directory / 'dnsmasq.log').open():
        match = re.search(r' reply (\S+) is ([0-9a-f.:]+)$', line)
        if match:
            names[match[2]].add(match[1])
    by = {}
    for path in sorted((directory / 'captures').glob('all-*.pcap')):
        for _, p in packets(path):
            if not egress(p, lane):
                continue
            if local_control(p, lane):
                continue  # Retained and separately counted by the DSCP cross-check.
            if p['dst'] == lane['host'] and p['protocol'] in (6, 17) and p['dport'] == 53:
                continue
            if p['dst'] == '10.4.20.237' and p['protocol'] == 6 and p['dport'] == 8090:
                continue
            host = ','.join(sorted(names[p['dst']])) or '<no DNS name: ' + p['dst'] + '>'
            key = (host, p['dst'], p['protocol'], p['dport'])
            value = by.setdefault(key, dict(host=host, ip=p['dst'], protocol=p['protocol'],
                                            port=p['dport'], packets=0, syn=0))
            value['packets'] += 1
            value['syn'] += int(p['syn'])
    return list(by.values())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--campaign', required=True, type=Path)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--lane', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    lane = next(l for l in json.loads(args.config.read_text())['lanes'] if l['name'] == args.lane)
    ready=args.config.parent/'network-ready.json'
    if ready.exists():
        lane=next(l for l in json.loads(ready.read_text())['lanes'] if l['name']==args.lane)
    args.out.write_text(json.dumps(ip_map(args.campaign, lane), indent=2) + '\n')
