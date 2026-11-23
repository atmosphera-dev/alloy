# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import io

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from guest_test_support.launch_trace import RuntimeLaunchTrace
from guest_test_support.session import process_role
from guest_lock_probe import role_of
from guest_lifetime_sdk_probe import wait_probe_group_stop, assert_callback_order
from guest_lifetime_probe import cleanup_driver
import guest_lifetime_probe as lifetime
from guest_test_support.native_consumer import native_consumer_executable
from guest_test_support.scratch import private_directory
from guest_test_support.certificate import stage_nss_certificate


def quoted(value):
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92)
                         else '\\%03o' % b for b in os.fsencode(value)) + '"'


class LifetimeReceipts(unittest.TestCase):
    def test_late_child_deadline_cannot_pass_with_only_the_named_creator(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(lifetime.shutil, 'copy2'), \
             patch.object(lifetime.subprocess, 'Popen') as popen, \
             patch.object(lifetime, 'line', side_effect=['LAUNCHED 1 OWNER 123',
                                                       'REAPED APP_SIGNALS_UNCHANGED']), \
             patch.object(lifetime, 'pinned', return_value=11), \
             patch.object(lifetime, 'descendants', return_value=[124]), \
             patch.object(lifetime, 'status', return_value={'Name': 'alloy-lifetime', 'State': 'R'}), \
             patch.object(lifetime, 'exited', return_value=True), \
             patch.object(lifetime.time, 'monotonic', side_effect=[0, 0, 11]), \
             patch.object(lifetime.time, 'sleep'), \
             patch.object(lifetime, 'cleanup_driver'):
            popen.return_value.wait.return_value = 0
            with self.assertRaisesRegex(AssertionError, 'No confirmed stopped unregistered child'):
                lifetime.case(Path(directory), Path(directory), 'default', 'late')

    def test_blocked_callback_order_uses_the_drained_log(self):
        resumed = dict(name='guest', event='callback-resumed')
        closed = dict(name='guest', event='closed')
        assert_callback_order([resumed, closed])
        for events in ([closed, resumed], [resumed, closed, closed], [closed]):
            with self.subTest(events=events), self.assertRaises(AssertionError):
                assert_callback_order(events)

    def test_cleanup_collects_pin_errors_but_still_closes_handles_and_driver(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch('guest_lifetime_probe.descendants', return_value=[123]), \
             patch('guest_lifetime_probe.pinned', side_effect=OSError(24, 'FD pressure')), \
             patch('guest_lifetime_probe.signal.pidfd_send_signal') as signal_fd, \
             patch('guest_lifetime_probe.os.close') as close:
            process = Mock(pid=100)
            process.poll.return_value = None
            with self.assertRaises(ExceptionGroup):
                cleanup_driver(process, [11, 12], Path(directory))
            self.assertEqual(signal_fd.call_count, 2)
            self.assertEqual([call.args[0] for call in close.call_args_list], [11, 12])
            process.kill.assert_called_once()
            process.wait.assert_called_once_with(timeout=10)

    def test_cleanup_signal_errors_do_not_skip_later_descriptors(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch('guest_lifetime_probe.descendants', return_value=[]), \
             patch('guest_lifetime_probe.signal.pidfd_send_signal',
                   side_effect=[PermissionError(1, 'denied'), None]), \
             patch('guest_lifetime_probe.os.close') as close:
            process = Mock(pid=100)
            process.poll.return_value = None
            with self.assertRaises(ExceptionGroup):
                cleanup_driver(process, [11, 12], Path(directory))
            self.assertEqual([call.args[0] for call in close.call_args_list], [11, 12])
            process.kill.assert_called_once()
            process.wait.assert_called_once_with(timeout=10)

    def test_certificate_input_uses_supplied_tmpdir_without_ambient_tmpdir(self):
        with patch.dict(os.environ, {}, clear=True), \
             patch('guest_test_support.certificate.subprocess.run',
                   side_effect=[Mock(returncode=0), Mock(returncode=0), Mock(returncode=0)]) as run:
            env = {'TMPDIR': '/home/builder/work/client-tmp'}
            stage_nss_certificate(123, 'home/.pki/nssdb', Path('/fixture/ca.pem'),
                                  'builder', io.StringIO(), env)
            for call in run.call_args_list:
                self.assertIn('TMPDIR=' + env['TMPDIR'], call.args[0])
                self.assertIs(call.kwargs['env'], env)

    def test_certificate_input_uses_one_namespace_for_database_operations(self):
        with patch.dict(os.environ, TMPDIR='/home/builder/work/tmp'), \
             patch('guest_test_support.certificate.subprocess.run',
                   side_effect=[Mock(returncode=0), Mock(returncode=1),
                                Mock(returncode=0), Mock(returncode=0)]) as run:
            stage_nss_certificate(123, 'home/.pki/nssdb', Path('/fixture/ca.pem'),
                                  'builder', io.StringIO())
            commands = [call.args[0] for call in run.call_args_list]
            self.assertEqual(len(commands), 4)
            for command in commands:
                self.assertEqual(command[:5], ['sudo', '-n', 'nsenter',
                                              '--mount=/proc/123/ns/mnt', 'sudo'])
            self.assertIn('sql:/dev/shm/home/.pki/nssdb', commands[-1])
            self.assertIn('/fixture/ca.pem', commands[-1])
            self.assertNotIn('sql:/proc/123/root/dev/shm/home/.pki/nssdb', commands[-1])

    def test_certificate_input_rejects_path_escape_before_execution(self):
        with patch('guest_test_support.certificate.subprocess.run') as run:
            for name in ('../outside', '/outside'):
                with self.subTest(name=name), self.assertRaises(AssertionError):
                    stage_nss_certificate(123, name, Path('/fixture/ca.pem'),
                                          'builder', io.StringIO())
            run.assert_not_called()

    def test_private_directory_preserves_the_observed_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'home').mkdir(mode=0o700)
            (root / 'home/nss').mkdir(mode=0o700)
            self.assertEqual(private_directory(root, 'home/nss'), root / 'home/nss')

    def test_private_directory_rejects_symlink_and_parent_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'actual').mkdir(mode=0o700)
            (root / 'alias').symlink_to(root / 'actual', target_is_directory=True)
            for name in ('alias', '../actual', '/absolute'):
                with self.subTest(name=name), self.assertRaises(AssertionError):
                    private_directory(root, name)

    def test_native_consumers_share_the_app_allowance_and_cleanup_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with native_consumer_executable(root) as first, native_consumer_executable(root) as second:
                self.assertNotEqual(first, second)
                for executable in (first, second):
                    self.assertEqual(executable.parent, root)
                    self.assertTrue(executable.name.startswith('alloy_guest_fixture-'))
                    self.assertEqual(executable.stat().st_mode & 0o777, 0o700)
            self.assertFalse(first.exists())
            self.assertFalse(second.exists())

    def test_native_consumer_cleanup_preserves_a_compile_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'compile failure'):
                with native_consumer_executable(Path(directory)) as executable:
                    raise RuntimeError('compile failure')
            self.assertFalse(executable.exists())

    def test_probe_accepts_only_an_explicit_sigstop_group_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = SimpleNamespace(path=Path(directory) / 'trace')
            (Path(directory) / 'trace.100').write_text('--- stopped by SIGSTOP ---\n')
            for state in ('T', 't'):
                wait_probe_group_stop(trace, dict(pid=100, state=state))

    def test_probe_rejects_signal_delivery_without_group_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = SimpleNamespace(path=Path(directory) / 'trace')
            (Path(directory) / 'trace.100').write_text('--- SIGSTOP {si_signo=SIGSTOP} ---\n')
            with patch('guest_lifetime_sdk_probe.time.monotonic', side_effect=[0, 11]):
                with self.assertRaisesRegex(AssertionError, 'No confirmed SIGSTOP group-stop'):
                    wait_probe_group_stop(trace, dict(pid=100, state='t'))

    def test_probe_rejects_another_process_group_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = SimpleNamespace(path=Path(directory) / 'trace')
            (Path(directory) / 'trace.99').write_text('--- stopped by SIGSTOP ---\n')
            with patch('guest_lifetime_sdk_probe.time.monotonic', side_effect=[0, 11]):
                with self.assertRaisesRegex(AssertionError, 'No confirmed SIGSTOP group-stop'):
                    wait_probe_group_stop(trace, dict(pid=100, state='t'))

    def test_probe_rejects_a_running_process(self):
        with self.assertRaises(AssertionError):
            wait_probe_group_stop(SimpleNamespace(path=Path('/unused')), dict(pid=100, state='R'))

    def test_trace_paths_preserve_utf8_and_non_utf8_bytes(self):
        for suffix in ('caf\u00e9', os.fsdecode(b'raw-\xff')):
            with self.subTest(suffix=repr(suffix)), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                out = root / suffix
                lines = []
                for name in ('alloy_guest_sdk_consumer', 'alloy_guest_host', 'alloy_guest_engine'):
                    path = str(out / name)
                    token = quoted(path)
                    lines.append(f'execve({token}, [{token}], 0x0) = 0\n')
                (root / 'trace.1').write_text(''.join(lines))
                rows = RuntimeLaunchTrace(out, root / 'trace').launches()
                self.assertEqual([os.fsencode(row['executable']) for row in rows],
                                 [os.fsencode(out / name) for name in ('alloy_guest_host', 'alloy_guest_engine')])

    def test_role_ignores_executable_path_markers(self):
        executable = '/root/a --alloy-supervisor b/engine'
        self.assertEqual(process_role([os.fsencode(executable), b'--type=renderer']), 'renderer')
        row = dict(exe='/root/--alloy-wait-owner/engine',
                   argv='/root/--alloy-wait-owner/engine --type=utility',
                   arguments=['/root/--alloy-wait-owner/engine', '--type=utility'])
        self.assertEqual(role_of(row), 'utility')

    def test_real_role_tokens_remain_distinct(self):
        self.assertEqual(process_role([b'host', b'--alloy-supervisor']), 'supervisor')
        self.assertEqual(process_role([b'host', b'--alloy-wait-owner']), 'wait-owner')
        self.assertEqual(process_role([b'engine', b'--type=renderer']), 'renderer')
        self.assertEqual(process_role([b'engine']), 'browser')


if __name__ == '__main__':
    unittest.main()
