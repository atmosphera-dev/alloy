# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Focused reference-bridge regressions; run inside the assigned test guest."""
import io
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from . import bridge, display


class TextTests(unittest.TestCase):
    def node(self, role, value, name='Field label', states=()):
        node = object.__new__(bridge.Accessible)
        node.data = dict(role=role, value=value, name=name, states=list(states))
        node.caret, node.selection = -1, None
        return node

    def test_empty_editable_value_is_not_the_label(self):
        for role in ('textField', 'textFieldWithComboBox', 'searchBox', 'spinButton'):
            with self.subTest(role=role):
                node = self.node(role, '')
                self.assertEqual(node.text(), '')
                self.assertEqual(node.GetText(0, -1), '')
                self.assertEqual(node.props(bridge.TEXT)['CharacterCount'], 0)

    def test_name_backed_text_and_contenteditable_stay_distinct(self):
        self.assertEqual(self.node('staticText', '', 'Visible text').text(), 'Visible text')
        self.assertEqual(self.node('genericContainer', '', states=('editable',)).text(), '')
        self.assertEqual(self.node('textField', 'actual value').text(), 'actual value')

    def test_unicode_offsets_reject_half_surrogates(self):
        self.assertEqual(bridge.codepoint_offset('a\U0001f600b', 1), 1)
        self.assertIsNone(bridge.codepoint_offset('a\U0001f600b', 2))
        self.assertEqual(bridge.codepoint_offset('a\U0001f600b', 3), 2)
        self.assertIsNone(bridge.codepoint_offset('a', 2))


class FocusTests(unittest.TestCase):
    def test_gap_emits_focus_loss_before_retiring_focus(self):
        events = []
        old = SimpleNamespace(data={'name': 'Focused object'}, target={}, children=[], stale=False,
                              StateChanged=lambda *args: events.append(args))
        app = object.__new__(bridge.Bridge)
        app.awaiting_snapshot = False
        app.update = {'sequence': '1'}
        app.trees = {}
        app.focus = old
        app.frame = SimpleNamespace(children=[old])
        app.objects = {('old',): old}
        app.send = lambda command: events.append(command)
        app.apply({'sequence': '3', 'snapshot': False})
        self.assertIn(('focused', 0, 0, 0, {}), events)
        self.assertIsNone(app.focus)
        self.assertTrue(app.awaiting_snapshot)


class CleanupTests(unittest.TestCase):
    def fixture(self):
        resource = object.__new__(display.PrivateDisplay)
        resource.child = Mock()
        resource.child.poll.return_value = None
        resource.policy = resource.cache_mounted = True
        resource.table = 'alloy_m6_display_test'
        resource.env = {}
        resource.log = io.BytesIO()
        resource.cache = Mock(spec=Path)
        resource.cache.samefile.return_value = True
        return resource

    @patch.object(display.subprocess, 'run')
    def test_timeout_still_kills_reaps_and_restores_every_resource(self, run):
        resource = self.fixture()
        resource.child.wait.side_effect = [subprocess.TimeoutExpired('Xvfb', 30), 0]
        try:
            resource.close()
        except BaseException:
            pass
        resource.child.kill.assert_called_once()
        self.assertEqual(resource.child.wait.call_count, 2)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertIn(['sudo', '-n', 'nft', 'delete', 'table', 'inet', resource.table], commands)
        self.assertIn(['sudo', '-n', 'umount', '/var/lib/xkb'], commands)
        self.assertTrue(resource.log.closed)

    @patch.object(display.subprocess, 'run')
    def test_failed_firewall_delete_does_not_skip_unmount_or_log_close(self, run):
        resource = self.fixture()
        run.side_effect = [subprocess.CalledProcessError(1, 'nft'), None]
        with self.assertRaises(BaseException):
            resource.close()
        self.assertEqual(run.call_count, 2)
        self.assertFalse(resource.cache_mounted)
        self.assertTrue(resource.log.closed)

    @patch.object(display.subprocess, 'run')
    def test_failed_ownership_check_never_unmounts_someone_elses_cache(self, run):
        resource = self.fixture()
        resource.cache.samefile.return_value = False
        with self.assertRaises(BaseException):
            resource.close()
        self.assertEqual(run.call_count, 1)
        self.assertTrue(resource.cache_mounted)
        self.assertTrue(resource.log.closed)


if __name__ == '__main__':
    unittest.main()
