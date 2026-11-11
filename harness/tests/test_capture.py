# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Every packet provider requests the same bounded receive-buffer headroom."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))
spec = importlib.util.spec_from_file_location('capture_network', HARNESS / 'network.py')
network = importlib.util.module_from_spec(spec)
spec.loader.exec_module(network)


class CaptureProviders(unittest.TestCase):
    def test_all_providers_keep_full_immediate_capture_with_buffer_headroom(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            lanes = [dict(name='observe', mode='observe', host='10.77.1.1', veth='alh1', port=18081),
                     dict(name='enforce', mode='enforce', host='10.77.2.1', veth='alh2', port=18082)]
            for lane in lanes:
                (root / lane['name'] / 'home').mkdir(parents=True)
            (root / 'nat-source.json').write_text(json.dumps(dict(ip='10.4.20.10')))
            calls = []

            def unit(name, argv, *args, **kwargs):
                calls.append((name, argv, kwargs))
                if name.endswith('-proxy'):
                    (root / 'observe/home/.mitmproxy/mitmproxy-ca-cert.pem').touch()

            with patch.object(network, 'LANES', lanes), patch.object(network, 'unit', unit), \
                    patch.object(network, 'run'), patch.object(network.time, 'sleep'):
                network.services(root, 9600)
            captures = [(name, argv, kwargs) for name, argv, kwargs in calls if argv[0] == 'tcpdump']
            self.assertEqual(len(captures), 4)
            self.assertEqual({argv[argv.index('-i') + 1] for _, argv, _ in captures},
                             {'alh1', 'alh2', 'eno1', 'nflog:77'})
            for name, argv, kwargs in captures:
                with self.subTest(provider=name):
                    self.assertEqual(argv.count('-B'), 1)
                    self.assertEqual(argv[argv.index('-B') + 1], '65536')
                    self.assertIn('--immediate-mode', argv)
                    self.assertIn('-U', argv)
                    self.assertEqual(argv[argv.index('-s') + 1], '0')
                    self.assertEqual(argv[argv.index('-G') + 1], '300')
                    self.assertEqual(kwargs['properties']['KillSignal'], 'SIGINT')
            uplink = next(argv for _, argv, _ in captures if argv[argv.index('-i') + 1] == 'eno1')
            self.assertEqual(uplink[-1], 'ip and src host 10.4.20.10 and (tcp or udp) and src portrange 62000-62999')
            receipt = json.loads((root / 'capture-settings.json').read_text())
            self.assertEqual(receipt['buffer_kib'], 65536)
            self.assertEqual(receipt['buffer_bytes'], 64 * 1024 * 1024)


if __name__ == '__main__':
    unittest.main()
