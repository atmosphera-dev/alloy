# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Explicit fontconfig and dconf system-state boundaries for browser probes."""
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


def create_runtime(root):
    """Keep desktop runtime state separate from measured browser roots."""
    path = root/'system-runtime'
    path.mkdir(mode=0o700)
    return path


def check_runtime(path):
    """Only dconf's native runtime file may occupy this separate directory."""
    assert path.is_dir() and not path.is_symlink(), 'invalid system runtime root'
    for entry in path.rglob('*'):
        relative = entry.relative_to(path)
        assert not entry.is_symlink(), 'system runtime symlink: '+str(relative)
        allowed = (entry.is_dir() and relative.parts == ('dconf',) or
                   entry.is_file() and relative.parts == ('dconf', 'user'))
        assert allowed, 'unexpected system runtime entry: '+str(relative)
