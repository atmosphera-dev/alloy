#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Independent outer-header packet checker; never interpret ICMP quotes as ports."""
import collections
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import socket
import struct

TABLE = Path(__file__).resolve().parents[2] / 'src/net/base/alloy_traffic_classes.inc'


def classes(path=TABLE):
    text = path.read_text()
    rows = re.findall(r'ALLOY_(?:TRAFFIC_CLASS\(\w+,\s*|NO_CLASS\()"([^"]+)",\s*(\d+)\)', text)
    result = {int(value): name for name, value in rows}
    if len(result) != 10 or len(set(result.values())) != 10 or 0 in result:
        raise ValueError('Invalid canonical class table')
    return result


def packet(frame, epoch):
    if len(frame) < 14:
        raise ValueError('short Ethernet frame')
    ether = int.from_bytes(frame[12:14], 'big')
    offset = 14
    while ether in (0x8100, 0x88a8):
        if len(frame) < offset + 4:
            raise ValueError('short VLAN frame')
        ether = int.from_bytes(frame[offset + 2:offset + 4], 'big')
        offset += 4
    ip = frame[offset:]
    result = dict(epoch=epoch, ether=ether, src_mac=frame[6:12].hex(':'), dst_mac=frame[:6].hex(':'), src=None, dst=None, dscp=None,
                  protocol=None, sport=None, dport=None, fragmented=False, syn=False)
    if ether == 0x0800:
        if len(ip) < 20 or ip[0] >> 4 != 4:
            raise ValueError('short/invalid IPv4')
        ihl = (ip[0] & 15) * 4
        length = int.from_bytes(ip[2:4], 'big')
        if ihl < 20 or length < ihl or len(ip) < length:
            raise ValueError('truncated IPv4')
        result.update(src=socket.inet_ntop(socket.AF_INET, ip[12:16]),
                      dst=socket.inet_ntop(socket.AF_INET, ip[16:20]),
                      dscp=ip[1] >> 2, protocol=ip[9],
                      fragmented=bool(int.from_bytes(ip[6:8], 'big') & 0x3fff))
        transport = ip[ihl:length]
    elif ether == 0x86dd:
        if len(ip) < 40 or ip[0] >> 4 != 6 or len(ip) < 40 + int.from_bytes(ip[4:6], 'big'):
            raise ValueError('short/invalid IPv6')
        result.update(src=socket.inet_ntop(socket.AF_INET6, ip[8:24]),
                      dst=socket.inet_ntop(socket.AF_INET6, ip[24:40]),
                      dscp=(((ip[0] & 15) << 4) | (ip[1] >> 4)) >> 2,
                      protocol=ip[6])
        pos = 40
        while result['protocol'] in (0, 43, 60, 44, 51):
            proto = result['protocol']
            if len(ip) < pos + 8:
                raise ValueError('truncated IPv6 extension')
            size = 8 if proto == 44 else (ip[pos + 1] + (2 if proto == 51 else 1)) * (4 if proto == 51 else 8)
            result['protocol'] = ip[pos]
            if proto == 44:
                result['fragmented'] = True
            pos += size
            if pos > len(ip):
                raise ValueError('truncated IPv6 extension body')
        transport = ip[pos:]
    else:
        return result
    if result['protocol'] in (6, 17) and not result['fragmented']:
        minimum = 20 if result['protocol'] == 6 else 8
        if len(transport) < minimum:
            raise ValueError('truncated TCP/UDP')
        result['sport'], result['dport'] = struct.unpack('!HH', transport[:4])
        result['syn'] = result['protocol'] == 6 and transport[13] & 0x12 == 2
        if result['protocol']==6:
            header_bytes=(transport[12]>>4)*4
            if header_bytes<20 or header_bytes>len(transport):
                raise ValueError('invalid TCP header length')
            result.update(tcp_flags=transport[13],payload_bytes=len(transport)-header_bytes,
                          tcp_seq=int.from_bytes(transport[4:8],'big'),tcp_ack=int.from_bytes(transport[8:12],'big'))
    elif result['protocol'] in (1, 58) and not result['fragmented']:
        if len(transport) < 8:
            raise ValueError('truncated ICMP')
        result['icmp_type'], result['icmp_code'] = transport[:2]
        # A quote is evidence about a different packet, never outer ports.
        result['quoted'] = quoted_header(transport[8:])
    return result


def quoted_header(data):
    """Decode only the quoted IP/transport prefix; an ICMP quote may be short."""
    if not data:
        return None
    if data[0] >> 4 == 4:
        if len(data) < 20:
            return None
        pos = (data[0] & 15) * 4
        if pos < 20 or int.from_bytes(data[6:8], 'big') & 0x3fff:
            return None
        src = socket.inet_ntop(socket.AF_INET, data[12:16])
        dst = socket.inet_ntop(socket.AF_INET, data[16:20])
        protocol = data[9]
    elif data[0] >> 4 == 6:
        if len(data) < 40:
            return None
        src = socket.inet_ntop(socket.AF_INET6, data[8:24])
        dst = socket.inet_ntop(socket.AF_INET6, data[24:40])
        protocol, pos = data[6], 40
        while protocol in (0, 43, 60, 51):
            if len(data) < pos + 8:
                return None
            size = (data[pos + 1] + (2 if protocol == 51 else 1)) * (4 if protocol == 51 else 8)
            protocol = data[pos]
            pos += size
    else:
        return None
    if protocol not in (6, 17) or len(data) < pos + 4:
        return None
    sport, dport = struct.unpack('!HH', data[pos:pos + 4])
    return dict(src=src, dst=dst, protocol=protocol, sport=sport, dport=dport)


def local_control(p, lane):
    """Approved answer03-1: local ICMP errors quoting resolver DNS, or IPv6 ND/RS."""
    if p['fragmented'] or p['protocol'] not in (1, 58):
        return False
    if (p['protocol']==1 and p['ether']!=0x0800) or (p['protocol']==58 and p['ether']!=0x86dd):
        return False
    kind = p.get('icmp_type')
    if p['protocol'] == 58 and kind in (133, 134, 135, 136, 137):
        address = ipaddress.ip_address(p['dst'])
        return p.get('icmp_code') == 0 and (p['dst']==lane['host'] or address.is_link_local or address in ipaddress.ip_network('ff02::/16'))
    errors = (3, 4, 5, 11, 12) if p['protocol'] == 1 else (1, 2, 3, 4)
    if kind not in errors or p['dst'] != lane['host']:
        return False
    quote = p.get('quoted') or {}
    return bool((quote.get('src') == lane['host'] and quote.get('sport') == 53 and quote.get('dst') == lane['guest']) or
                (quote.get('dst') == lane['host'] and quote.get('dport') == 53 and quote.get('src') == lane['guest']))


def packets(path, *, live=False, start=24):
    """One bounded read of complete PCAP records; active tail is deferred, not lost."""
    with Path(path).open('rb') as source:
        size = Path(path).stat().st_size
        header = source.read(24)
        if len(header) != 24:
            if live:
                return
            raise ValueError(f'Missing PCAP header: {path}')
        endian, scale = {b'\xd4\xc3\xb2\xa1': ('<', 1e6), b'\xa1\xb2\xc3\xd4': ('>', 1e6),
                         b'\x4d\x3c\xb2\xa1': ('<', 1e9), b'\xa1\xb2\x3c\x4d': ('>', 1e9)}[header[:4]]
        if struct.unpack(endian + 'I', header[20:24])[0] != 1:
            raise ValueError('Expected Ethernet PCAP')
        source.seek(start)
        while source.tell() < size:
            pos = source.tell()
            record = source.read(min(16, size - pos))
            if len(record) != 16:
                if live:
                    return
                raise ValueError('Partial PCAP record header')
            sec, frac, count, original = struct.unpack(endian + 'IIII', record)
            if pos + 16 + count > size:
                if live:
                    return
                raise ValueError('Partial PCAP frame')
            if count != original:
                raise ValueError('Snaplen-truncated PCAP frame')
            frame = source.read(count)
            yield source.tell(), packet(frame, sec + frac / scale)


def egress(p, lane):
    return p['src'] == lane['guest'] or (p['ether'] == 0x86dd and p['src_mac'] == lane.get('guest_mac'))


def disposition(p, lane, mapping):
    if not egress(p, lane):
        return 'not-egress'
    if local_control(p, lane):
        return 'local-kernel-control'
    resolver = p['dst'] == lane['host'] and p['protocol'] in (6, 17) and p['dport'] == 53
    if p['dscp'] == 0:
        return 'unmarked-resolver' if resolver else 'unmarked-elsewhere'
    name = mapping.get(p['dscp'], 'unknown-codepoint')
    if name in ('browser-service', 'unannotated', 'no-class-known', 'unknown-codepoint'):
        return name
    return 'allowed'


class WireClassifier:
    """One chronological pass; answer03-2 never guesses ownership from RST flags."""
    def __init__(self,lane,mapping):
        self.lane=lane
        self.mapping=mapping
        self.flows={}
        self.records=0

    def classify(self,p):
        self.records+=1
        if self.records%1024==0:
            self.flows={key:value for key,value in self.flows.items() if p['epoch']-value['last_marked_epoch']<=120}
        outgoing=egress(p,self.lane)
        if p['protocol']!=6 or p['fragmented']:
            return disposition(p,self.lane,self.mapping),None
        incoming=p['dst']==self.lane['guest'] or (p['ether']==0x86dd and p['dst_mac']==self.lane.get('guest_mac'))
        if not outgoing and not incoming:
            return 'not-egress',None
        key=(6,p['src'],p['sport'],p['dst'],p['dport']) if outgoing else (6,p['dst'],p['dport'],p['src'],p['sport'])
        if incoming:
            if key in self.flows:
                self.flows[key]['incoming_epoch']=p['epoch']
                self.flows[key]['incoming_seq']=p['tcp_seq']
            return 'not-egress',None
        decision=disposition(p,self.lane,self.mapping)
        if decision=='allowed':
            prior=self.flows.get(key,{}) if not p['syn'] else {}
            self.flows[key]=dict(prior,parent_class=self.mapping[p['dscp']],last_marked_epoch=p['epoch'])
            return decision,None
        parent=self.flows.get(key)
        if (p['dscp']==0 and p['tcp_flags'] in (0x04,0x14) and p['payload_bytes']==0 and parent and
                0<=p['epoch']-parent['last_marked_epoch']<=120 and
                parent.get('incoming_epoch') is not None and parent['incoming_epoch']<=p['epoch']):
            proof=dict(parent,flow=key,reset_epoch=p['epoch'],reset_flags=p['tcp_flags'],reset_payload_bytes=0)
            return 'kernel-tcp-control',proof
        return decision,None


def check(paths, lane, since, *, live=False, table=TABLE):
    mapping = classes(table)
    counts = collections.Counter()
    classes_count = collections.Counter()
    bad = []
    outside = collections.Counter()
    codepoints = collections.Counter()
    classifier=WireClassifier(lane,mapping)
    tcp_control=collections.Counter()
    tcp_proofs=[]
    for path in paths:
        for _, p in packets(path, live=live):
            if p['epoch'] < since:
                continue
            decision,proof=classifier.classify(p)
            if not egress(p,lane):
                continue
            if proof:
                tcp_control[proof['parent_class']]+=1
                if len(tcp_proofs)<100:tcp_proofs.append(proof)
            counts[decision] += 1
            classes_count[decision if decision in ('local-kernel-control','kernel-tcp-control') else mapping.get(p['dscp'], 'unmarked' if p['dscp'] == 0 else 'unknown-codepoint')] += 1
            codepoints[p['dscp']] += 1
            if decision not in ('allowed', 'unmarked-resolver', 'local-kernel-control','kernel-tcp-control') or p['fragmented'] or (p['protocol'] not in (6, 17) and decision != 'local-kernel-control'):
                bad.append(dict(p, disposition=decision))
            local = p['dst'] == lane['host'] and p['dport'] == 53 and p['protocol'] in (6, 17)
            kiwix = p['dst'] == '10.4.20.237' and p['dport'] == 8090 and p['protocol'] == 6
            if not local and not kiwix and decision != 'local-kernel-control':
                outside[p['dst']] += 1
    return dict(dispositions=dict(counts), classes=dict(classes_count),
                codepoints=dict(codepoints), local_kernel_control=counts['local-kernel-control'],
                kernel_tcp_control=counts['kernel-tcp-control'],kernel_tcp_control_by_parent_class=dict(tcp_control),
                kernel_tcp_control_proof_samples=tcp_proofs,
                unmarked_resolver=counts['unmarked-resolver'],
                unmarked_elsewhere=counts['unmarked-elsewhere'],
                invalid_packets=len(bad), invalid_samples=bad[:100],
                outside_packets_by_ip=dict(outside),
                table_sha256=hashlib.sha256(table.read_bytes()).hexdigest())


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--lane', required=True)
    parser.add_argument('--since', required=True, type=float)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('pcap', nargs='+', type=Path)
    args = parser.parse_args()
    lane = next(l for l in json.loads(args.config.read_text())['lanes'] if l['name'] == args.lane)
    result = check(args.pcap, lane, args.since)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(bool(result['invalid_packets']))
