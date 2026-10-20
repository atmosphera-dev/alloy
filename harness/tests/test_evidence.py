# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The final seal binds nested analysis seals and records symlinks without following."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('harness_run',Path(__file__).resolve().parents[1]/'run.py')
runner=importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class EvidenceSeal(unittest.TestCase):
    def test_nested_seal_and_symlink_are_preserved(self):
        with tempfile.TemporaryDirectory() as scratch:
            root=Path(scratch)/'run';root.mkdir()
            nested=root/'analysis';nested.mkdir()
            (nested/'SHA256SUMS').write_text('retained analysis seal\n')
            (root/'receipt.json').write_text('{}\n')
            outside=Path(scratch)/'outside';outside.write_text('not a payload\n')
            (root/'SingletonLock').symlink_to(outside)
            runner.seal(root)
            seal=(root/'SHA256SUMS').read_text()
            self.assertIn(runner.digest(nested/'SHA256SUMS')+'  analysis/SHA256SUMS\n',seal)
            self.assertNotIn('  SingletonLock\n',seal)
            self.assertEqual(json.loads((root/'SYMLINKS.json').read_text()),{'SingletonLock':str(outside)})
            self.assertIn(runner.digest(root/'SYMLINKS.json')+'  SYMLINKS.json\n',seal)


if __name__=='__main__':
    unittest.main()
