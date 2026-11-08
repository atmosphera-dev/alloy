# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The explicit fontconfig system-state boundary shared by browser probes."""
from fnmatch import fnmatchcase


def create(root):
    """Keep user-level font indexes separate from measured browser roots."""
    path = root/'system-cache'
    path.mkdir(mode=0o700)
    return path


def check(path):
    """No browser or arbitrary system state may hide in the separate cache."""
    assert path.is_dir() and not path.is_symlink(), 'invalid system cache root'
    for entry in path.rglob('*'):
        relative = entry.relative_to(path)
        assert not entry.is_symlink(), 'system cache symlink: '+str(relative)
        if entry.is_dir():
            allowed = relative.parts == ('fontconfig',)
        else:
            allowed = entry.is_file() and (
                relative.parts in (('CACHEDIR.TAG',), ('fontconfig', 'CACHEDIR.TAG')) or
                len(relative.parts) == 2 and relative.parts[0] == 'fontconfig' and
                fnmatchcase(relative.name, '*.cache-*'))
        assert allowed, 'unexpected system cache entry: '+str(relative)
