# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Wire contract regressions: exact resolver exception, outer headers, live tails."""
import importlib.util
from pathlib import Path
import socket
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('dscp', Path(__file__).resolve().parents[1] / 'analysis/dscp.py')
dscp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dscp)
LANE = dict(guest='10.77.1.2', host='10.77.1.1')
MAPPING = dscp.classes()
hostspec = importlib.util.spec_from_file_location('hosts', Path(__file__).resolve().parents[1] / 'analysis/hosts.py')
hosts = importlib.util.module_from_spec(hostspec)
hostspec.loader.exec_module(hosts)


def ipv4(code, dst='10.77.1.1', proto=17, port=53, payload=None):
    transport = payload if payload is not None else struct.pack('!HHHH', 45000, port, 8, 0)
    ip = bytearray(20)
    ip[0], ip[1], ip[9] = 0x45, code << 2, proto
    ip[2:4] = struct.pack('!H', 20 + len(transport))
    ip[12:16] = socket.inet_aton(LANE['guest'])
    ip[16:20] = socket.inet_aton(dst)
    return bytes(12) + b'\x08\x00' + ip + transport


class WireContract(unittest.TestCase):
    def test_kiwix_service_binding_is_not_arbitrary_subdomain_exemption(self):
        self.assertEqual(hosts.label('_8090._https.nomad.home',['HTTPS']),'kiwix')
        for host,types in [('_8090._https.nomad.home',['A']),('_8090._https.nomad.home',['HTTPS','A']),
                           ('outside.nomad.home',['A']),('_443._https.nomad.home',['HTTPS']),
                           ('evilnomad.home',['HTTPS'])]:
            self.assertEqual(hosts.label(host,types),'outside')

    def test_exact_unmarked_exception(self):
        for proto in (6, 17):
            payload = struct.pack('!HHIIHHHH',45000,53,1,0,0x5010,100,0,0) if proto == 6 else None
            p = dscp.packet(ipv4(0, proto=proto, payload=payload), 0)
            self.assertEqual(dscp.disposition(p, LANE, MAPPING), 'unmarked-resolver')
        for dst, port in [('8.8.8.8', 53), ('10.77.1.1', 853), ('10.4.20.237', 8090)]:
            p = dscp.packet(ipv4(0, dst=dst, port=port), 0)
            self.assertEqual(dscp.disposition(p, LANE, MAPPING), 'unmarked-elsewhere')

    def test_every_disallowed_class_fails_even_at_resolver(self):
        for code, name in MAPPING.items():
            result = dscp.disposition(dscp.packet(ipv4(code), 0), LANE, MAPPING)
            self.assertEqual(result, name if name in ('browser-service', 'unannotated', 'no-class-known') else 'allowed')
        self.assertEqual(dscp.disposition(dscp.packet(ipv4(63), 0), LANE, MAPPING), 'unknown-codepoint')

    def test_icmp_dns_quote_is_not_outer_dns(self):
        quoted = ipv4(0)[14:]
        frame = ipv4(0, proto=1, payload=b'\x03\x03' + bytes(6) + quoted)
        p = dscp.packet(frame, 0)
        self.assertIsNone(p['dport'])
        self.assertEqual(p['protocol'], 1)
        self.assertEqual(dscp.disposition(p, LANE, MAPPING), 'local-kernel-control')

    def test_local_icmp_exception_is_protocol_and_destination_limited(self):
        quoted = ipv4(0)[14:]
        good = dscp.packet(ipv4(48, proto=1, payload=b'\x03\x03' + bytes(6) + quoted), 0)
        self.assertTrue(dscp.local_control(good, LANE))
        for outer in [ipv4(48, dst='8.8.8.8', proto=1, payload=b'\x03\x03' + bytes(6) + quoted),
                      ipv4(48, proto=1, payload=b'\x08\x00' + bytes(6) + quoted),
                      ipv4(48, proto=1, payload=b'\x03\x03' + bytes(6) + ipv4(0, port=853)[14:]),
                      ipv4(48, proto=1, payload=b'\x03\x03' + bytes(6) + ipv4(0, dst='8.8.8.8')[14:]),
                      ipv4(48, proto=1, payload=b'\x03\x03' + bytes(6) + quoted[:22])]:
            p=dscp.packet(outer,0)
            self.assertFalse(dscp.local_control(p, LANE))
            self.assertEqual(dscp.disposition(p, LANE, MAPPING), 'unknown-codepoint')
        # The historical case quotes a resolver RESPONSE, not an outer DNS request.
        response=bytearray(quoted)
        response[12:16],response[16:20]=quoted[16:20],quoted[12:16]
        response[20:24]=struct.pack('!HH',53,11464)
        p=dscp.packet(ipv4(48,proto=1,payload=b'\x03\x03'+bytes(6)+response),0)
        self.assertTrue(dscp.local_control(p,LANE))
        self.assertIsNone(p['dport'])

    def test_ipv6_local_control_scope_and_quoted_dns(self):
        lane=dict(guest='fd00::2',host='fd00::1')
        def frame(dst,kind,quote=b''):
            payload=bytes([kind,0])+bytes(6)+quote
            header=struct.pack('!IHBB',6<<28,len(payload),58,255)
            header+=socket.inet_pton(socket.AF_INET6,lane['guest'])+socket.inet_pton(socket.AF_INET6,dst)
            return dscp.packet(bytes(12)+b'\x86\xdd'+header+payload,0)
        for kind in range(133,138):
            for dst in ('fe80::1','ff02::2'):
                self.assertEqual(dscp.disposition(frame(dst,kind),lane,MAPPING),'local-kernel-control')
            self.assertFalse(dscp.local_control(frame('2001:4860::1',kind),lane))
            self.assertFalse(dscp.local_control(frame('ff05::2',kind),lane))
        quote=struct.pack('!IHBB',6<<28,8,17,64)
        quote+=socket.inet_pton(socket.AF_INET6,lane['host'])+socket.inet_pton(socket.AF_INET6,lane['guest'])
        quote+=struct.pack('!HHHH',53,40000,8,0)
        self.assertTrue(dscp.local_control(frame(lane['host'],1,quote),lane))
        self.assertFalse(dscp.local_control(frame('2001:4860::1',1,quote),lane))
        self.assertFalse(dscp.local_control(frame(lane['host'],128,quote),lane))

    def test_ipv6_ecn_and_extension(self):
        for code in MAPPING:
            tclass = (code << 2) | 3
            header = struct.pack('!IHBB', (6 << 28) | (tclass << 20), 16, 0, 64)
            header += socket.inet_pton(socket.AF_INET6, 'fd00::2') + socket.inet_pton(socket.AF_INET6, 'fd00::1')
            hop = bytes([17, 0]) + bytes(6)
            p = dscp.packet(bytes(12) + b'\x86\xdd' + header + hop + struct.pack('!HHHH', 40000, 53, 8, 0), 0)
            self.assertEqual(p['dscp'], code)
            self.assertEqual(p['dport'], 53)
            self.assertEqual(p['protocol'], 17)

    def test_partial_live_record_is_revisited(self):
        frame = ipv4(0)
        header = b'\xd4\xc3\xb2\xa1' + struct.pack('<HHIIII', 2, 4, 0, 0, 65535, 1)
        record = struct.pack('<IIII', 123, 0, len(frame), len(frame)) + frame
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.pcap'
            path.write_bytes(header + record[:20])
            self.assertEqual(list(dscp.packets(path, live=True)), [])
            with self.assertRaisesRegex(ValueError, 'Partial'):
                list(dscp.packets(path))
            with path.open('ab') as stream:
                stream.write(record[20:])
            values = list(dscp.packets(path, live=True))
            self.assertEqual(len(values), 1)
            self.assertEqual(values[0][0], len(header) + len(record))

    def test_snaplen_loss_is_never_success(self):
        frame = ipv4(0)
        header = b'\xd4\xc3\xb2\xa1' + struct.pack('<HHIIII', 2, 4, 0, 0, 65535, 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.pcap'
            path.write_bytes(header + struct.pack('<IIII', 1, 0, len(frame), len(frame) + 20) + frame)
            with self.assertRaisesRegex(ValueError, 'Snaplen'):
                list(dscp.packets(path))


if __name__ == '__main__':
    unittest.main()
