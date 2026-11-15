# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import guest_integration_probe as probe


class IntegrationCleanupTests(unittest.TestCase):
    def client(self, root):
        client = object.__new__(probe.Client)
        client.process = Mock(); client.process.poll.return_value = 0
        client.members = Mock(); client.members.exited.side_effect = RuntimeError('owned process did not exit')
        client.log = Mock(); client.root = Path(root); client.events = [dict(event='preserved')]
        return client

    def test_teardown_failure_still_closes_resources_and_preserves_events(self):
        with tempfile.TemporaryDirectory() as root:
            client = self.client(root)
            with self.assertRaisesRegex(RuntimeError, 'did not exit'):
                client.dispose()
            client.members.close.assert_called_once()
            client.log.close.assert_called_once()
            self.assertEqual(json.loads((Path(root) / 'events.json').read_text()), client.events)

    def test_scenario_failure_remains_in_teardown_exception_chain(self):
        with tempfile.TemporaryDirectory() as root:
            client = self.client(root)
            def scenario(_):
                raise ValueError('original scenario failure')
            with patch.object(probe, 'Client', return_value=client):
                with self.assertRaises(RuntimeError) as caught:
                    probe.case(None, Path(root), 'failure', scenario)
            self.assertIsInstance(caught.exception.__context__, ValueError)
            self.assertEqual(str(caught.exception.__context__), 'original scenario failure')
            client.members.close.assert_called_once()
            self.assertTrue((Path(root) / 'events.json').exists())
