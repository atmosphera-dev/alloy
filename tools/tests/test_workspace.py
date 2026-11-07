# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Build workspace admission contract; no original-workspace writes."""
import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import workspace as w
import shared_proto_inventory as proto


class Admission(unittest.TestCase):
    def setUp(self):
        for mock in (patch.object(w.socket, 'gethostname', return_value='wakaba-build'),
                     patch.object(w.pwd, 'getpwnam', return_value=SimpleNamespace(pw_uid=1000)),
                     patch.object(w.os, 'geteuid', return_value=1000),
                     patch.object(Path, 'resolve', lambda p: p),
                     patch.object(Path, 'exists', return_value=True),
                     patch.object(Path, 'is_dir', return_value=True),
                     patch.object(Path, 'stat', return_value=SimpleNamespace(st_uid=1000))):
            mock.start()
            self.addCleanup(mock.stop)

    def test_both_roots_and_outputs(self):
        for root in w.APPROVED:
            repo = root / 'repo'
            self.assertEqual(w.workspace_path(repo), root)
            w.validate_source(root, root / 'src', repo)
            for config in ('Release_Alloy', 'Verify_Alloy'):
                w.validate_output(root, root / 'src/out' / config, repo)
        self.assertEqual(str(w.temp_path(w.PRIMARY)), '/home/builder/work/tmp')
        self.assertEqual(w.temp_path(w.SECONDARY), w.SECONDARY / 'tmp')

    def test_foreign_and_spike_roots(self):
        for root in (Path('/home/builder/work/brave'), Path('/home/builder/work/foreign')):
            with self.assertRaises(RuntimeError):
                w.validate_workspace(root)

    def test_approved_host_root_matrix(self):
        for host in w.HOSTS:
            with patch.object(w.socket, 'gethostname', return_value=host):
                for root in w.APPROVED:
                    w.validate_source(root, root/'src', root/'repo')
                    w.validate_output(root, root/'src/out/Verify_Alloy', root/'repo')
                    with patch.object(Path, 'stat', return_value=SimpleNamespace(st_uid=1234)):
                        with self.assertRaises(RuntimeError):
                            w.validate_workspace(root)
                    with patch.object(Path, 'resolve', lambda p: Path('/foreign') if p == root else p):
                        with self.assertRaises(RuntimeError):
                            w.validate_workspace(root)
                with self.assertRaises(RuntimeError):
                    w.validate_workspace(Path('/home/builder/work/foreign'))

    def test_wrong_host_and_account(self):
        with patch.object(w.socket, 'gethostname', return_value='laptop'):
            with self.assertRaises(RuntimeError):
                w.validate_workspace(w.PRIMARY)
        with patch.object(w.os, 'geteuid', return_value=1234):
            with self.assertRaises(RuntimeError):
                w.validate_workspace(w.SECONDARY)

    def test_ownership_and_symlink_escape(self):
        with patch.object(Path, 'stat', return_value=SimpleNamespace(st_uid=1234)):
            with self.assertRaises(RuntimeError):
                w.validate_workspace(w.SECONDARY)
        with patch.object(Path, 'resolve', lambda p: w.PRIMARY if p == w.SECONDARY else p):
            with self.assertRaises(RuntimeError):
                w.validate_workspace(w.SECONDARY)
        for name in ('src/out', 'src/out/Release_Alloy', 'apply-state'):
            with patch.object(Path, 'resolve',
                    lambda p: w.PRIMARY/name if p == w.SECONDARY/name else p):
                with self.assertRaises(RuntimeError):
                    w.validate_workspace(w.SECONDARY)

    def test_cross_lane_and_misdelivered_repo(self):
        with self.assertRaises(RuntimeError):
            w.validate_source(w.SECONDARY, w.PRIMARY / 'src')
        with self.assertRaises(RuntimeError):
            w.validate_output(w.SECONDARY, w.PRIMARY / 'src/out/Release_Alloy')
        with self.assertRaises(RuntimeError):
            w.validate_workspace(w.SECONDARY, w.PRIMARY / 'repo')

    def test_shared_namespace_lock_order(self):
        self.assertEqual(w.namespace_prefix(w.PRIMARY), ['sudo', '-n'])
        self.assertEqual(w.namespace_prefix(w.SECONDARY), ['sudo', '-n',
            'flock', '--wait', '120', str(w.PRIMARY/'harness-run.lock'),
            'flock', '--wait', '120', str(w.PRIMARY/'controller.lock')])

    def test_native_inventory_helper_workspace_admission(self):
        for root in w.APPROVED:
            with patch.object(proto, 'WORK', root), patch.object(proto, 'REPO', root/'repo'), \
                 patch.object(Path, 'rglob', return_value=[]):
                for config in ('Release_Alloy', 'Verify_Alloy'):
                    utility = root/'src/out'/config/'leveldb_leveldbutil'
                    result = proto.inspect_shared_proto(root/'evidence/profile/shared_proto_db', utility)
                    self.assertEqual(result['forbidden'], [])
                other = w.SECONDARY if root == w.PRIMARY else w.PRIMARY
                with self.assertRaises(ValueError):
                    proto.inspect_shared_proto(other/'evidence/profile/shared_proto_db', utility)
                with self.assertRaises(RuntimeError):
                    proto.inspect_shared_proto(root/'evidence/profile/shared_proto_db',
                        other/'src/out/Release_Alloy/leveldb_leveldbutil')


class EntryPoints(unittest.TestCase):
    def test_harness_direct_and_snapshot_locations(self):
        code = (TOOLS.parent/'harness/common.py').read_bytes()
        for root in w.APPROVED:
            for relative in ('repo/harness/common.py',
                             'harness-runs/campaign/source/harness/common.py'):
                values = dict(__name__='harness_workspace_contract', __file__=str(root/relative))
                with patch.dict(os.environ, {'ALLOY_HARNESS_CONFIG': 'config-fixture'}), \
                     patch.object(Path, 'read_text', return_value=json.dumps(dict(workspace=str(root)))):
                    exec(compile(code, 'common.py', 'exec'), values)
                self.assertEqual(values['ROOT'], root)
                self.assertEqual(values['TMP'], str(w.temp_path(root)))
                self.assertEqual(values['NETWORK_LOCK_ROOT'], w.PRIMARY)

    def test_harness_configuration_cannot_switch_lane(self):
        code = (TOOLS.parent/'harness/common.py').read_bytes()
        for source, selected in ((w.SECONDARY, w.PRIMARY), (w.PRIMARY, w.SECONDARY)):
            values = dict(__name__='harness_workspace_contract',
                __file__=str(source/'repo/harness/common.py'))
            with patch.dict(os.environ, {'ALLOY_HARNESS_CONFIG': 'config-fixture'}), \
                 patch.object(Path, 'read_text', return_value=json.dumps(dict(workspace=str(selected)))):
                with self.assertRaises(RuntimeError):
                    exec(compile(code, 'common.py', 'exec'), values)

    def test_original_location_derivation(self):
        # Execute definitions, not main: no lock, build, apply or probe in A.
        for name, variable in [('alloy_checkout', 'WORK'), ('alloy_apply', 'WORK'),
                               ('alloy_build', 'WORK'), ('smoke', 'WORK'),
                               ('probe_blocking', 'WORK'), ('store_inventory_probe', 'WORK'),
                               ('worker_probe', 'W'), ('shared_proto_inventory', 'WORK')]:
            values = dict(__name__='workspace_contract',
                          __file__=str(w.PRIMARY / 'repo/tools' / (name+'.py')))
            exec(compile((TOOLS / (name+'.py')).read_bytes(), name, 'exec'), values)
            self.assertEqual(values[variable], w.PRIMARY, name)

    def test_foreign_delivery_refused_before_side_effects(self):
        self.assertIn(w.socket.gethostname(), w.HOSTS)
        self.assertEqual(os.environ['TMPDIR'], str(w.temp_path(TOOLS.parents[1])))
        # Foreign delivery lives on B's scratch disk, never the spike or A.
        with tempfile.TemporaryDirectory(prefix='workspace-contract-') as scratch:
            foreign = Path(scratch) / 'repo/tools'
            foreign.mkdir(parents=True)
            for source in TOOLS.glob('*.py'):
                (foreign / source.name).write_bytes(source.read_bytes())
            for name, args in [
                ('alloy_checkout', ['--root', str(w.SECONDARY/'src')]),
                ('alloy_apply', ['--root', str(w.SECONDARY/'src')]),
                ('alloy_build', ['--prepare-only']),
                ('prune_origin', ['--root', str(w.SECONDARY/'src'), '--audit']),
                ('smoke', ['--out', str(w.SECONDARY/'src/out/Release_Alloy')]),
                ('probe_blocking', ['--out', str(w.SECONDARY/'src/out/Release_Alloy')]),
                ('store_inventory_probe', ['--out', str(w.SECONDARY/'src/out/Release_Alloy')]),
            ]:
                result = subprocess.run([sys.executable, str(foreign/(name+'.py')), *args],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, (name, result.stderr))
                self.assertIn('Only the approved build workspaces', result.stderr)
            self.assertEqual(sorted(p.name for p in Path(scratch).iterdir()), ['repo'])


if __name__ == '__main__':
    unittest.main()
