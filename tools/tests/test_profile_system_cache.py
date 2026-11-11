# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import profile_system_cache as cache


class SystemCacheBoundary(unittest.TestCase):
    def test_dconf_is_the_only_allowed_runtime_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = cache.create_runtime(Path(directory))
            cache.check_runtime(root)
            (root/'dconf').mkdir()
            (root/'dconf/user').write_bytes(b'\0\0')
            cache.check_runtime(root)
            (root/'dconf/browser-state').write_bytes(b'private')
            with self.assertRaisesRegex(AssertionError, 'unexpected system runtime'):
                cache.check_runtime(root)

    def test_runtime_unknown_directories_and_symlinks_fail(self):
        for kind in ('directory', 'symlink'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = cache.create_runtime(Path(directory))
                if kind == 'directory':
                    (root/'browser-runtime').mkdir()
                else:
                    (root/'dconf').symlink_to(Path(directory), target_is_directory=True)
                with self.assertRaises(AssertionError):
                    cache.check_runtime(root)

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
