# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Lock-driver receipts must reject missing paths and weaker protection."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from guest_lock_probe import bus_calls, inventory, inventory_equal, protected, role_of, wait_bus_output


class LockReceipts(unittest.TestCase):
    def test_inventory_errors_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                inventory([Path(directory) / 'missing'])

    def test_inventory_does_not_follow_or_exclude_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'alias').symlink_to(root / 'missing')
            result = inventory([root])
            self.assertEqual(result[str(root / 'alias')][-1], str(root / 'missing'))
            (root / 'new').write_text('new durable content')
            with self.assertRaises(AssertionError):
                inventory_equal(result, inventory([root]))

    def test_helper_or_missing_process_limit_is_not_accepted(self):
        report = dict(filesystem='data-writes-denied', scratch='verified-tmpfs',
            sandbox=json.dumps(dict(route='setuid')), limits='{}')
        with self.assertRaises(AssertionError):
            protected(report)

    def test_role_does_not_confuse_browser_with_subprocess(self):
        self.assertEqual(role_of(dict(argv='engine --type=renderer --x=y')), 'renderer')
        self.assertEqual(role_of(dict(argv='host --alloy-supervisor')), 'supervisor')
        self.assertEqual(role_of(dict(argv='engine')), 'browser')

    def test_bus_count_excludes_only_explicit_control_senders(self):
        text = ('method call sender=:1.1 member=ListNames\n'
                'method call sender=:1.10 member=Unexpected\n')
        self.assertEqual(bus_calls(text, {':1.1'}), ['method call sender=:1.10 member=Unexpected'])

    def test_bus_observation_waits_for_marker_after_delayed_guest_call(self):
        path = Mock()
        path.read_text.side_effect = ['', 'method call sender=:1.9 member=Unexpected\n',
                                     'method call sender=:1.9 member=Unexpected\nBARRIER\n']
        monitor = Mock()
        monitor.poll.return_value = None
        with patch('guest_lock_probe.time.sleep'):
            text = wait_bus_output(path, monitor, lambda value: 'BARRIER\n' in value)
        self.assertEqual(len(bus_calls(text, set())), 1)
        self.assertEqual(path.read_text.call_count, 3)

    def test_bus_observation_rejects_monitor_exit_before_barrier(self):
        path, monitor = Mock(), Mock()
        path.read_text.return_value = ''
        monitor.poll.return_value = 1
        with self.assertRaisesRegex(AssertionError, 'monitor exited'):
            wait_bus_output(path, monitor, lambda value: 'BARRIER\n' in value)


if __name__ == '__main__':
    unittest.main()
