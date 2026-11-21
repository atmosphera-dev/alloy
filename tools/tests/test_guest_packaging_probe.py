# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import copy
import ctypes
import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import guest_packaging_probe as probe
from guest_packaging_support import LocalFlatpak, PackageClient, PackageMembers
from guest_test_support.process_identity import identity
from guest_test_support.process_fixture import pipe_child, pin_after_exit
from guest_test_support.launch_trace import RuntimeLaunchTrace


def report():
    return dict(filesystem='data-writes-denied', scratch='verified-tmpfs',
        coverage=json.dumps(dict(before_engine=True,
            trusted_setup=['fixed-static-bootstrap-teardown-probe; no-engine-payload',
                           'fixed-static-wait-owner; private-kernel-exit-status; no-engine-payload'],
            flatpak_external_entries='fixed-bootstrap-per-entry',
            snap_external_entries='unsupported:strict-Snap-policy-denies-pidfd_send_signal')),
        sandbox=json.dumps(dict(route='flatpak-brokered-userns', browser=dict(no_new_privs=True),
            renderer=dict(observed_after_blank_load=True, no_new_privs=True, seccomp=2,
                          effective_capabilities_decimal='0'))),
        limits=json.dumps(dict(namespace_filesystem_and_packages='native-namespace-view-absent',
                               metadata_and_atime='not-refused-by-Landlock')))


class GuestPackagingTests(unittest.TestCase):
    def test_report_requires_actual_brokered_controls(self):
        good = report()
        self.assertEqual(good, probe.protected(good))
        for group, field, bad in (
                ('sandbox', 'route', 'userns'),
                ('coverage', 'before_engine', False),
                ('coverage', 'flatpak_external_entries', 'unavailable'),
                ('coverage', 'snap_external_entries', 'verified'),
                ('limits', 'namespace_filesystem_and_packages', 'read-only-view')):
            changed = copy.deepcopy(good)
            value = json.loads(changed[group])
            value[field] = bad
            changed[group] = json.dumps(value)
            with self.subTest(group=group, field=field), self.assertRaises(AssertionError):
                probe.protected(changed)
        for field, bad in (('observed_after_blank_load', False), ('no_new_privs', False),
                           ('seccomp', 0), ('effective_capabilities_decimal', '1')):
            changed = copy.deepcopy(good)
            value = json.loads(changed['sandbox'])
            value['renderer'][field] = bad
            changed['sandbox'] = json.dumps(value)
            with self.subTest(renderer=field), self.assertRaises(AssertionError):
                probe.protected(changed)

    def test_brokered_process_roles_require_sandbox_but_not_different_userns(self):
        rows = [dict(exe='/app/lib/alloy/alloy_guest_engine', role=role,
            status=dict(NoNewPrivs='1', CapEff='0', Seccomp='2'),
            fds={str(fd): '/dev/null' for fd in (0, 1, 2)},
            namespaces=dict(user='shared-package-userns',
                pid='renderer' if role == 'renderer' else 'browser',
                mnt='renderer' if role == 'renderer' else 'browser'))
            for role in ('browser', 'zygote', 'renderer', 'gpu-process', 'utility')]
        self.assertEqual(rows, probe.roles(rows))
        for change in ('missing-role', 'setup-fd', 'shared-pidns', 'capability'):
            altered = copy.deepcopy(rows)
            if change == 'missing-role':
                altered.pop()
            elif change == 'setup-fd':
                altered[0]['fds']['256'] = '/run/user/1000/alloy-guest/alloy-guest-1000-unique'
            elif change == 'shared-pidns':
                altered[2]['namespaces']['pid'] = 'browser'
            else:
                altered[2]['status']['CapEff'] = '1'
            with self.subTest(change=change), self.assertRaises(AssertionError):
                probe.roles(altered)

    def test_external_members_are_not_limited_to_app_descendants(self):
        rows = [dict(pid=100, role='app'), dict(pid=200, role='renderer')]
        with patch('guest_packaging_support.os.pidfd_open', return_value=9) as opened, \
             patch('guest_packaging_support.os.close') as closed, \
             patch.object(PackageMembers, '_validate_identities') as validate:
            with PackageMembers(rows) as members:
                self.assertEqual([rows[1]], members.rows)
                opened.assert_called_once_with(200)
                validate.assert_called_once()
            closed.assert_called_once_with(9)

    def test_snapshot_validation_accepts_only_the_pinned_birth_and_executable(self):
        handle = os.pidfd_open(os.getpid())
        try:
            original = identity(os.getpid(), handle)
        finally:
            os.close(handle)
        row = dict(pid=os.getpid(), role='app', identity=original)
        with PackageMembers([row], include_app=True) as members:
            self.assertEqual([row], members.rows)
        for field in ('start_ticks', 'exe_inode', 'exe_device'):
            stale = copy.deepcopy(row)
            stale['identity'][field] = (str(int(original[field]) + 1) if field == 'start_ticks'
                                        else original[field] + 1)
            with self.subTest(field=field), \
                 patch('guest_packaging_support.signal.pidfd_send_signal') as signalled:
                with self.assertRaisesRegex(AssertionError, 'process identity changed'):
                    with PackageMembers([stale], include_app=True):
                        pass
                signalled.assert_not_called()

    def test_snapshot_exit_after_pin_is_retired_without_signaling(self):
        with pipe_child() as process:
            opened = os.pidfd_open(process.pid)
            try:
                row = dict(pid=process.pid, role='app', identity=identity(process.pid, opened))
            finally:
                os.close(opened)
            real_open = os.pidfd_open
            pinned = []
            def pin_then_exit(pid):
                if pid != process.pid:
                    return real_open(pid)
                pinned.append(pin_after_exit(process, real_open))
                return pinned[-1]
            with patch('guest_packaging_support.os.pidfd_open', side_effect=pin_then_exit), \
                 patch('guest_packaging_support.signal.pidfd_send_signal') as signalled, \
                 patch('guest_packaging_support.os.close', wraps=os.close) as closed:
                with PackageMembers([row], include_app=True) as members:
                    self.assertFalse(members.rows)
                    self.assertFalse(members.handles)
                    self.assertEqual(1, len(pinned))
                    closed.assert_any_call(pinned[0])
                signalled.assert_not_called()

    def test_non_utf8_process_name_does_not_break_birth_identity(self):
        libc = ctypes.CDLL(None, use_errno=True)
        original = Path('/proc/self/comm').read_bytes().removesuffix(b'\n')
        handle = os.pidfd_open(os.getpid())
        try:
            before = identity(os.getpid(), handle)
            self.assertEqual(0, libc.prctl(15, ctypes.c_char_p(b'\xff)fixture'), 0, 0, 0))
            self.assertEqual(before, identity(os.getpid(), handle))
            self.assertIsInstance(before['start_ticks'], str)
        finally:
            libc.prctl(15, ctypes.c_char_p(original), 0, 0, 0)
            os.close(handle)

    def test_validator_deadline_kills_and_reaps_the_pinned_root_helper(self):
        real_popen = subprocess.Popen
        stall = TOOLS / 'testdata/guest-packaging/validator_stall.py'
        handle = os.pidfd_open(os.getpid())
        try:
            row = dict(pid=os.getpid(), role='app', identity=identity(os.getpid(), handle))
        finally:
            os.close(handle)
        for before_identity in (False, True):
            helpers = []
            def spawn(command, **kwargs):
                command = list(command)
                if str(command[-1]).endswith('/process_identity.py'):
                    command[-1] = str(stall)
                    if before_identity:
                        command.append('before-identity')
                result = real_popen(command, **kwargs)
                helpers.append(result)
                return result
            started = time.monotonic()
            with self.subTest(before_identity=before_identity), \
                 patch('guest_packaging_support.subprocess.Popen', side_effect=spawn), \
                 patch('guest_packaging_support.IDENTITY_EXCHANGE_TIMEOUT', 0.5):
                with self.assertRaises(TimeoutError):
                    PackageMembers([row], include_app=True)
            self.assertLess(time.monotonic() - started, 5)
            self.assertGreaterEqual(len(helpers), 1 if before_identity else 2)
            self.assertTrue(all(helper.poll() is not None for helper in helpers))

    def test_pipe_child_setup_failure_still_reaps_the_child(self):
        real_popen = subprocess.Popen
        children = []
        def spawn(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            return child
        with patch('guest_test_support.process_fixture.subprocess.Popen', side_effect=spawn), \
             patch('guest_test_support.process_fixture.os.pidfd_open', side_effect=OSError('fixture pin failure')):
            with self.assertRaisesRegex(OSError, 'fixture pin failure'):
                with pipe_child():
                    self.fail('Fixture unexpectedly entered')
        self.assertEqual(0, children[0].poll())

    def test_failed_pin_handoff_closes_its_descriptor(self):
        with pipe_child() as process:
            handle = os.pidfd_open(process.pid)
            with patch.object(process, 'communicate', side_effect=RuntimeError('fixture communication failure')):
                with self.assertRaisesRegex(RuntimeError, 'fixture communication failure'):
                    pin_after_exit(process, lambda pid: handle)
            with self.assertRaises(OSError):
                os.fstat(handle)

    def test_pipe_child_cleanup_timeout_terminates_and_reaps_original_child(self):
        scope = pipe_child()
        process = scope.__enter__()
        original = process.communicate
        count = 0
        def communicate(*args, **kwargs):
            nonlocal count
            count += 1
            if count == 1:
                raise subprocess.TimeoutExpired('fixture', 15)
            return original(*args, **kwargs)
        with patch.object(process, 'communicate', side_effect=communicate):
            with self.assertRaises(subprocess.TimeoutExpired):
                scope.__exit__(None, None, None)
        self.assertEqual(-9, process.returncode)

    def test_launch_trace_records_an_engine_that_exits_before_final_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk, engine = root / 'alloy_guest_sdk_consumer', root / 'alloy_guest_engine'
            shutil.copyfile('/bin/sh', sdk)
            shutil.copyfile('/bin/true', engine)
            sdk.chmod(0o700)
            engine.chmod(0o700)
            trace = RuntimeLaunchTrace(root, root / 'exec')
            subprocess.run(trace.command() + [sdk, '-c', '"' + str(engine) + '"'], check=True)
            launches = trace.launches()
            self.assertEqual([str(engine)], [entry['executable'] for entry in launches])

    def test_failed_package_command_preserves_its_diagnostic(self):
        package = LocalFlatpak(TOOLS.parent, Path('/out'), Path('/evidence'))
        package.log = io.StringIO()
        failure = subprocess.CalledProcessError(1, ['flatpak'], output='fixture error\n')
        with patch('guest_packaging_support.subprocess.run', side_effect=failure):
            with self.assertRaises(subprocess.CalledProcessError):
                package.run(['flatpak'])
        self.assertIn('fixture error', package.log.getvalue())

    def test_package_client_launches_inside_package_not_host_deployment_path(self):
        package = Mock(library=Path('/deployment/files/lib/alloy'), app_id='org.test.Alloy')
        with patch('guest_packaging_support.Client.__init__', return_value=None) as initialize:
            PackageClient(package, Path('/evidence'), encrypted_app=True)
        args, kwargs = initialize.call_args
        self.assertEqual('/app/lib/alloy/alloy_guest_sdk_consumer', kwargs['executable'])
        self.assertEqual(package.app_id, kwargs['launch_prefix'][-1])
        self.assertIn('--command=/app/lib/alloy/zypak/bin/zypak-wrapper', kwargs['launch_prefix'])
        self.assertEqual(Path.home() / '.var/app/org.test.Alloy/cache/app-profile', kwargs['app_profile'])
        with patch('guest_packaging_support.Client.__init__', return_value=None) as initialize:
            PackageClient(package, Path('/evidence'), session_bus=False)
        self.assertIn('--no-session-bus', initialize.call_args.kwargs['launch_prefix'])

    def test_negative_permissions_get_a_fresh_identity_without_mutating_the_original(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = LocalFlatpak(TOOLS.parent, Path('/out'), Path(temporary))
            package.installed = True
            package.snapshot = Mock(return_value=[])
            metadata = package.stage / 'package/metadata'
            metadata.parent.mkdir(parents=True)
            original = (b'[Application]\nname=org.test.Alloy\n[Context]\n'
                        b'filesystems=xdg-run/alloy-guest:create;xdg-documents:ro;\n'
                        b'[Session Bus Policy]\norg.freedesktop.portal.Flatpak=talk\n')
            metadata.write_bytes(original)
            with patch.object(LocalFlatpak, '_install_package') as install:
                with self.assertRaisesRegex(RuntimeError, 'fixture startup'):
                    with package.without_permission('Context', 'filesystems', 'xdg-run/alloy-guest:create') as denied:
                        self.assertNotEqual(package.app_id, denied.app_id)
                        altered = (denied.stage / 'package/metadata').read_bytes()
                        self.assertNotIn(b'alloy-guest', altered)
                        self.assertIn(b'xdg-documents:ro;', altered)
                        self.assertIn(denied.app_id.encode(), altered)
                        self.assertEqual(original, metadata.read_bytes())
                        raise RuntimeError('fixture startup')
                install.assert_called_once()
                with package.without_permission('Session Bus Policy', 'org.freedesktop.portal.Flatpak') as denied:
                    altered = (denied.stage / 'package/metadata').read_bytes()
                    self.assertNotIn(b'org.freedesktop.portal.Flatpak=talk', altered)
            self.assertEqual(original, metadata.read_bytes())

    def test_package_abort_owns_external_entries_before_the_outer_wrapper(self):
        package = LocalFlatpak(TOOLS.parent, Path('/out'), Path('/evidence'))
        package.snapshot = Mock(return_value=[dict(pid=200, role='renderer')])
        process = Mock()
        process.poll.return_value = None
        with patch('guest_packaging_support.PackageMembers') as members, \
             patch('guest_packaging_support.os.pidfd_open', return_value=9), \
             patch('guest_packaging_support.os.close'), \
             patch('guest_packaging_support.signal.pidfd_send_signal') as signal:
            package.abort(process)
        members.assert_called_once_with(package.snapshot.return_value, include_app=True)
        members.return_value.__enter__.return_value.terminate.assert_called_once()
        signal.assert_called_once()


if __name__ == '__main__':
    unittest.main()
