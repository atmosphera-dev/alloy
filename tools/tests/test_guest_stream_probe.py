# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import guest_stream_probe as probe


class WorkerRendererTests(unittest.TestCase):
    def processes(self):
        return Mock(members=[dict(pid=11, role='renderer'),
                             dict(pid=12, role='renderer'),
                             dict(pid=13, role='browser')], handles=[21, 22, 23])

    def test_worker_identifies_primary_not_spare_or_browser(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for pid, name in ((11, 'DedicatedWorker'), (12, 'CrRendererMain'),
                              (13, 'DedicatedWorker')):
                task = root / str(pid) / 'task' / str(pid)
                task.mkdir(parents=True)
                (task / 'comm').write_text(name + '\n')
            poll = Mock()
            poll.poll.return_value = []
            processes = self.processes()
            with patch.object(probe, 'Path', return_value=root), \
                 patch.object(probe.select, 'poll', return_value=poll):
                self.assertEqual([(processes.members[0], 21)], probe.worker_renderers(processes))
            self.assertEqual(2, poll.register.call_count)

    def test_exited_original_is_not_a_live_worker(self):
        poll = Mock()
        poll.poll.return_value = [(21, 1)]
        with patch.object(probe.select, 'poll', return_value=poll), \
             patch.object(probe, 'Path', side_effect=AssertionError('Exited pid must not be looked up')):
            self.assertEqual([], probe.worker_renderers(self.processes()))

    def test_process_without_worker_is_not_admitted(self):
        # Exercise the real proc task path, not only the fixture's fake tree.
        # No renderer identity is inferred from ordering or the command line.
        handle = os.pidfd_open(os.getpid())
        try:
            processes = Mock(members=[dict(pid=os.getpid(), role='renderer')], handles=[handle])
            self.assertEqual([], probe.worker_renderers(processes))
        finally:
            os.close(handle)
