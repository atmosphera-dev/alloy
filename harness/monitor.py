# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Incremental safety observations, independent of the collector and full analysis."""
from common import *
from analysis.dscp import classes, WireClassifier, packets, egress
from analysis.hosts import label
import re


class Monitor:
    def __init__(self, directory):
        self.directory = directory
        self.offsets = {}
        self.mapping = classes()
        self.names = {l['name']: {} for l in LANES}
        self.since = json.loads((directory / 'collection-start.json').read_text())['epoch']
        self.lanes = json.loads((directory / 'network-ready.json').read_text())['lanes']
        self.classifiers={lane['name']:WireClassifier(lane,self.mapping) for lane in self.lanes}

    def lines(self, path):
        with path.open('rb') as source:
            source.seek(self.offsets.get(str(path), 0))
            size = path.stat().st_size
            while source.tell() < size:
                pos = source.tell()
                line = source.readline(size - pos)
                if not line.endswith(b'\n'):
                    break
                self.offsets[str(path)] = source.tell()
                yield line.decode()

    def sample(self):
        bad = []
        for lane in self.lanes:
            directory = self.directory / lane['name']
            names = self.names[lane['name']]
            for path in directory.glob('dnsmasq*.log'):
                for line in self.lines(path):
                    stamp = datetime.datetime.fromisoformat(line.split(' ', 1)[0]).timestamp()
                    if stamp < self.since:
                        continue
                    query = re.search(r'query\[([^]]+)\] (\S+) from', line)
                    if query and label(query[2], [query[1]])=='outside':
                        bad.append(dict(lane=lane['name'], source='dns', host=query[2], record=line))
                    answer = re.search(r'reply (\S+) is ([0-9a-f.:]+)$', line)
                    if answer:
                        names.setdefault(answer[2], set()).add(answer[1])
            for path in directory.glob('proxy*.jsonl'):
                for line in self.lines(path):
                    value = json.loads(line)
                    if value['epoch'] < self.since:
                        continue
                    host = (value.get('sni') or value.get('host') or '').lower()
                    if host and host not in ('nomad.home', 'example.com'):
                        bad.append(dict(lane=lane['name'], source='proxy', host=host, record=value))
                    if 'alloy-guard-canary' in value.get('url', ''):
                        bad.append(dict(lane=lane['name'], source='proxy', host=host, reason='guard canary escaped', record=value))
            for path in sorted((directory / 'captures').glob('all-*.pcap')):
                for offset, p in packets(path, live=True, start=self.offsets.get(str(path), 24)):
                    self.offsets[str(path)] = offset
                    if p['epoch'] < self.since:
                        continue
                    decision,proof=self.classifiers[lane['name']].classify(p)
                    if not egress(p,lane):
                        continue
                    if decision not in ('allowed', 'unmarked-resolver', 'local-kernel-control','kernel-tcp-control') or p['fragmented'] or (p['protocol'] not in (6, 17) and decision != 'local-kernel-control'):
                        bad.append(dict(lane=lane['name'], source='wire', disposition=decision, packet=p))
                    local = p['dst'] == lane['host'] and p['protocol'] in (6, 17) and p['dport'] == 53
                    kiwix = p['dst'] == '10.4.20.237' and p['protocol'] == 6 and p['dport'] == 8090
                    canary = names.get(p['dst']) == {'example.com'}
                    if not (local or kiwix or canary or decision == 'local-kernel-control'):
                        bad.append(dict(lane=lane['name'], source='wire', reason='outside destination', packet=p,
                                        names=sorted(names.get(p['dst'], set()))))
        return bad
