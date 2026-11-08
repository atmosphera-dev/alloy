# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import profile_system_cache as cache


class SystemCacheBoundary(unittest.TestCase):
    def test_font_indexes_and_tags_are_the_only_allowed_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = cache.create(Path(directory))
            cache.check(root)
            (root/'fontconfig').mkdir()
            for name in ('a.cache-9', 'b.cache-12', 'CACHEDIR.TAG'):
                (root/'fontconfig'/name).write_bytes(b'fixture')
            cache.check(root)
            (root/'fontconfig'/'browser-state').write_bytes(b'private')
            with self.assertRaisesRegex(AssertionError, 'unexpected system cache'):
                cache.check(root)

    def test_unknown_directories_and_symlinks_fail(self):
        for kind in ('directory', 'symlink'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = cache.create(Path(directory))
                if kind == 'directory':
                    (root/'browser-cache').mkdir()
                else:
                    (root/'fontconfig').symlink_to(Path(directory), target_is_directory=True)
                with self.assertRaises(AssertionError):
                    cache.check(root)
