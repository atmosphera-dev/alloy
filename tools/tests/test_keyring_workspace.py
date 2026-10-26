# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Workspace and private-session refusals happen before any keyring IPC."""
import contextlib
import io
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import workspace as w
import test_keyring as k


class PrivateSessionAdmission(unittest.TestCase):
    def setUp(self):
        for mock in (
            patch.object(w.socket, 'gethostname', return_value='wakaba-build'),
            patch.object(w.pwd, 'getpwnam', return_value=SimpleNamespace(pw_uid=1000)),
            patch.object(w.os, 'geteuid', return_value=1000),
            patch.object(Path, 'resolve', lambda p: p),
            patch.object(Path, 'exists', return_value=True),
            patch.object(Path, 'is_dir', return_value=True),
            patch.object(Path, 'stat', return_value=SimpleNamespace(st_uid=1000)),
        ):
            mock.start()
            self.addCleanup(mock.stop)

    def environment(self, root, work):
        return dict(HOME=str(root/'home'), ALLOY_TEST_KEYRING=str(root/'home'),
            ALLOY_TEST_KEYRING_WORKSPACE=str(work), TMPDIR=str(w.temp_path(work)),
            DBUS_SESSION_BUS_ADDRESS='unix:path=/private-fixture/bus',
            ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS='unix:path=/run/user/1000/bus')

    def test_both_shared_workspace_definitions(self):
        for work in w.APPROVED:
            root = work/'evidence/keyring-admission'
            with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'):
                env = self.environment(root, work)
                self.assertEqual(k.validate_private_session(root, env),
                                 env['DBUS_SESSION_BUS_ADDRESS'])

    def test_foreign_workspace_and_foreign_root_refused(self):
        work = w.PRIMARY
        with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'):
            for root in (Path('/home/builder/work/foreign/evidence/keyring'),
                         w.SECONDARY/'evidence/keyring'):
                with self.assertRaises(RuntimeError):
                    k.validate_test_root(root)
        work = Path('/home/builder/work/foreign')
        with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'):
            with self.assertRaises(RuntimeError):
                k.validate_test_root(work/'evidence/keyring')

    def test_inherited_real_session_refused_before_provider_or_ipc(self):
        work = w.PRIMARY
        root = work/'evidence/keyring-admission'
        env = self.environment(root, work)
        env['DBUS_SESSION_BUS_ADDRESS'] = env['ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS']
        with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'), \
             patch.dict(os.environ, env, clear=True), \
             patch.object(sys, 'argv', ['test_keyring.py', '--root', str(root),
                                      '--inside', '--', 'unused-child']), \
             patch.object(k.subprocess, 'run') as run, \
             patch.object(k.subprocess, 'Popen') as provider, \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                k.main()
            self.assertEqual(error.exception.code, 2)
            run.assert_not_called()
            provider.assert_not_called()

    def test_outer_launcher_removes_inherited_bus_and_control(self):
        work = w.PRIMARY
        root = work/'evidence/keyring-admission'
        real = 'unix:path=/run/user/1000/bus'
        with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'), \
             patch.dict(os.environ, dict(DBUS_SESSION_BUS_ADDRESS=real,
                 GNOME_KEYRING_CONTROL='/real-user/control',
                 ALLOY_TEST_KEYRING_BUS_ADDRESS=real), clear=True), \
             patch.object(sys, 'argv', ['test_keyring.py', '--root', str(root),
                                      '--', 'unused-child']), \
             patch.object(Path, 'mkdir'), \
             patch.object(k.subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as run:
            self.assertEqual(k.main(), 0)
            command = run.call_args.args[0]
            env = run.call_args.kwargs['env']
            self.assertIn('dbus-run-session', command)
            for name in ('DBUS_SESSION_BUS_ADDRESS', 'GNOME_KEYRING_CONTROL',
                         'ALLOY_TEST_KEYRING_BUS_ADDRESS'):
                self.assertNotIn(name, env)
            self.assertEqual(env['ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS'], real)
            self.assertEqual(env['TMPDIR'], str(w.temp_path(work)))

    def test_session_home_workspace_and_scratch_must_match(self):
        work = w.PRIMARY
        root = work/'evidence/keyring-admission'
        with patch.object(k, 'WORK', work), patch.object(k, 'REPO', work/'repo'):
            for key, value in (('HOME', '/home/builder'),
                               ('ALLOY_TEST_KEYRING_WORKSPACE', str(w.SECONDARY)),
                               ('TMPDIR', str(w.SECONDARY/'tmp')),
                               ('ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS', None)):
                env = self.environment(root, work)
                if value is None:
                    env.pop(key)
                else:
                    env[key] = value
                with self.assertRaises(RuntimeError):
                    k.validate_private_session(root, env)


if __name__ == '__main__':
    unittest.main()
