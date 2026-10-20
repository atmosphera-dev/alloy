# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Answer03-2 admits only flow-proven zero-data RSTs, across capture boundaries."""
import importlib.util
from pathlib import Path
import socket
import struct
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('tcp_dscp',Path(__file__).resolve().parents[1]/'analysis/dscp.py')
wire=importlib.util.module_from_spec(spec);spec.loader.exec_module(wire)
LANE=dict(guest='10.77.1.2',host='10.77.1.1')
MAPPING=wire.classes()


def frame(code=3,flags=0x10,reverse=False,payload=b'',port=45000):
    src,dst=('10.4.20.237',LANE['guest']) if reverse else (LANE['guest'],'10.4.20.237')
    sport,dport=(8090,port) if reverse else (port,8090)
    tcp=struct.pack('!HHIIHHHH',sport,dport,100,200,0x5000|flags,100,0,0)+payload
    ip=bytearray(20);ip[0]=0x45;ip[1]=code<<2;ip[9]=6
    ip[2:4]=struct.pack('!H',20+len(tcp));ip[12:16]=socket.inet_aton(src);ip[16:20]=socket.inet_aton(dst)
    return bytes(12)+b'\x08\x00'+ip+tcp


class TcpControl(unittest.TestCase):
    def classifier(self,marked=3,incoming=True):
        classifier=wire.WireClassifier(LANE,MAPPING)
        classifier.classify(wire.packet(frame(code=marked,flags=2),10))
        if incoming:classifier.classify(wire.packet(frame(code=0,reverse=True,payload=b'reply'),11))
        return classifier

    def test_rst_and_rst_ack_report_parent_allowed_class(self):
        for flags in (4,20):
            decision,proof=self.classifier().classify(wire.packet(frame(code=0,flags=flags),12))
            self.assertEqual(decision,'kernel-tcp-control')
            self.assertEqual(proof['parent_class'],'page-load')
            self.assertEqual(proof['incoming_epoch'],11)
            self.assertEqual(proof['last_marked_epoch'],10)

    def test_unknown_unallowed_no_incoming_and_stale_are_rejected(self):
        for classifier,epoch,port in [(wire.WireClassifier(LANE,MAPPING),12,45000),
                                      (self.classifier(marked=31),12,45000),
                                      (self.classifier(incoming=False),12,45000),
                                      (self.classifier(),131,45000),
                                      (self.classifier(),12,45001)]:
            decision,proof=classifier.classify(wire.packet(frame(code=0,flags=4,port=port),epoch))
            self.assertEqual(decision,'unmarked-elsewhere');self.assertIsNone(proof)

    def test_data_syn_syn_ack_fin_and_other_flags_never_inherit_class(self):
        for flags,payload in [(4,b'x'),(20,b'x'),(2,b''),(18,b''),(17,b''),(5,b''),(6,b''),(16,b'')]:
            decision,proof=self.classifier().classify(wire.packet(frame(code=0,flags=flags,payload=payload),12))
            self.assertEqual(decision,'unmarked-elsewhere');self.assertIsNone(proof)
        classifier=self.classifier()
        classifier.classify(wire.packet(frame(code=3,flags=2),12))
        self.assertEqual(classifier.classify(wire.packet(frame(code=0,flags=4),13))[0],'unmarked-elsewhere')

    def test_capture_rotation_preserves_flow_proof_and_raw_codepoint_count(self):
        header=b'\xd4\xc3\xb2\xa1'+struct.pack('<HHIIII',2,4,0,0,262144,1)
        def record(epoch,data):return struct.pack('<IIII',epoch,0,len(data),len(data))+data
        with tempfile.TemporaryDirectory() as scratch:
            first=Path(scratch)/'first.pcap';second=Path(scratch)/'second.pcap'
            first.write_bytes(header+record(10,frame(flags=2))+record(11,frame(code=0,reverse=True,payload=b'reply')))
            second.write_bytes(header+record(12,frame(code=0,flags=4)))
            result=wire.check([first,second],LANE,0)
            self.assertEqual(result['invalid_packets'],0)
            self.assertEqual(result['kernel_tcp_control'],1)
            self.assertEqual(result['kernel_tcp_control_by_parent_class'],{'page-load':1})
            self.assertEqual(result['codepoints'],{3:1,0:1})


if __name__=='__main__':unittest.main()
