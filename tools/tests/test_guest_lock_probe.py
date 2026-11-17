# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Lock-driver receipts must reject missing paths and weaker protection."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from guest_lock_probe import inventory, inventory_equal, protected, role_of


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


if __name__ == '__main__':
    unittest.main()
