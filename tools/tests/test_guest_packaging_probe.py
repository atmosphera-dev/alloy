# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import guest_packaging_probe as probe
from guest_packaging_support import LocalFlatpak, PackageClient, PackageMembers


def report():
    return dict(filesystem='data-writes-denied', scratch='verified-tmpfs',
        coverage=json.dumps(dict(before_engine=True,
            flatpak_external_entries='fixed-bootstrap-per-entry',
            snap_external_entries='unverified-blocked:strict-Snap-denies-pidfd_send_signal')),
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
             patch('guest_packaging_support.os.close') as closed:
            with PackageMembers(rows) as members:
                self.assertEqual([rows[1]], members.rows)
                opened.assert_called_once_with(200)
            closed.assert_called_once_with(9)

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
