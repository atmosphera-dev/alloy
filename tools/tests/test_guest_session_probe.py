# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import importlib.util
import json
import os
import io
import queue
import socket
import ssl
from pathlib import Path
import struct
import subprocess
import sys
import threading
import tempfile
import time
from unittest.mock import Mock
import unittest
from urllib.request import urlopen

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import guest_session_probe as probe
from guest_test_support.fixtures import Fixtures
from guest_test_support.session import Client, descendants, process_role


class GuestProbeTests(unittest.TestCase):
    def test_idle_tls_peer_does_not_block_requests_or_shutdown(self):
        with tempfile.TemporaryDirectory() as temporary:
            with Fixtures(TOOLS.parent, tls_root=Path(temporary) / 'tls') as fixtures:
                idle = socket.create_connection(('127.0.0.1', fixtures.port), timeout=3)
                try:
                    context = ssl.create_default_context(cafile=fixtures.certificate)
                    self.assertIn(b'trackerExecuted=true', urlopen(
                        fixtures.base + '/gtag/js', context=context, timeout=3).read())
                    started = time.monotonic()
                    fixtures.server.shutdown()
                    self.assertLess(time.monotonic() - started, 2)
                finally:
                    idle.close()

    def exited_client(self, root, code, events):
        client = Client.__new__(Client)
        client.root = root
        client.finished = client.aborted = False
        client.process = Mock()
        client.process.poll.return_value = code
        client.process.wait.return_value = code
        client.process.stdin = io.StringIO()
        client.process.stdout = io.StringIO()
        client.reader = Mock()
        client.reader.is_alive.return_value = False
        client.incoming = queue.Queue()
        client.events = events
        client.log = io.StringIO()
        client.stderr = io.StringIO()
        return client

    def test_already_exited_consumer_is_still_validated_and_collected(self):
        with tempfile.TemporaryDirectory() as temporary:
            client = self.exited_client(Path(temporary), 0, [{'event': 'finished'}])
            client.__exit__(None, None, None)
            self.assertTrue(client.finished)
            self.assertTrue(client.log.closed)
            self.assertTrue((client.root / 'events.json').exists())

    def test_already_exited_failure_cannot_pass_context_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            client = self.exited_client(Path(temporary), 1, [])
            with self.assertRaises(AssertionError):
                client.__exit__(None, None, None)
            self.assertTrue(client.aborted)
            self.assertTrue(client.log.closed)

    def test_finish_failure_aborts_and_preserves_original_exception(self):
        client = Client.__new__(Client)
        client.aborted = False
        client.process = Mock()
        client.process.poll.return_value = None
        original = RuntimeError('shutdown failed')
        client.finish = Mock(side_effect=original)
        client.abort = Mock()
        with self.assertRaises(RuntimeError) as caught:
            client.__exit__(None, None, None)
        self.assertIs(original, caught.exception)
        client.abort.assert_called_once()

    def test_zero_exit_without_finished_event_is_not_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            client = self.exited_client(Path(temporary), 0, [])
            with self.assertRaises(AssertionError):
                client.__exit__(None, None, None)
            self.assertTrue(client.aborted)
            self.assertTrue(client.log.closed)

    def test_role_survives_chromium_process_title_rewrite(self):
        for role in ('zygote', 'renderer', 'gpu-process', 'utility'):
            self.assertEqual(role, process_role([b'/engine', b'--type=' + role.encode(), b'']))
            self.assertEqual(role, process_role([b'/engine --type=' + role.encode() + b' --flag', b'']))
        self.assertEqual('browser', process_role([b'/engine', b'']))
        self.assertEqual('supervisor', process_role([b'/host', b'--alloy-supervisor', b'']))

    def test_process_walk_includes_worker_thread_children(self):
        ready, release = threading.Event(), threading.Event()
        children = []

        def spawn():
            child = subprocess.Popen([sys.executable, '-c', 'import sys;sys.stdin.read()'],
                                     stdin=subprocess.PIPE)
            children.append(child)
            ready.set()
            release.wait()
            child.stdin.close()
            child.wait(timeout=10)

        worker = threading.Thread(target=spawn)
        worker.start()
        try:
            self.assertTrue(ready.wait(10))
            self.assertIn(children[0].pid, descendants(os.getpid()))
        finally:
            release.set()
            worker.join(timeout=15)

    def test_wire_header_has_no_native_layout_or_fd_identity(self):
        value = probe.frame(1, probe.fields(['a', 'b']))
        header = struct.unpack('!4sHHII16sQQQQQ', value[:72])
        self.assertEqual((b'AGS1', 1, 1, 10, 0), header[:5])
        self.assertEqual((1, 0, 0, 0, 0), header[6:])
        self.assertEqual(b'\0\0\0\x01a\0\0\0\x01b', value[72:])

    def test_mismatch_fixture_can_encode_an_incompatible_version(self):
        value = probe.frame(1, version=2)
        self.assertEqual(2, struct.unpack('!H', value[4:6])[0])

    def test_loopback_smoke_reuses_the_committed_internet_fixture(self):
        with Fixtures(TOOLS.parent) as fixtures:
            self.assertEqual((TOOLS / 'testdata/smoke/index.html').read_bytes(),
                             urlopen(fixtures.base + '/smoke').read())

    def test_unblocked_tracker_counterpart_really_supplies_a_script(self):
        with Fixtures(TOOLS.parent) as fixtures:
            body = urlopen(fixtures.base + '/gtag/js').read()
            self.assertIn(b'trackerExecuted=true', body)
            self.assertEqual('/gtag/js', fixtures.requests[-1]['path'])


if __name__ == '__main__':
    unittest.main()
