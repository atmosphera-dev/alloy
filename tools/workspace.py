# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The two explicitly approved, builder-owned NUC workspaces.

Selection follows the delivered repository, not cwd or an arbitrary environment
variable. Validation happens at entry points, before any locks or writes.
"""
import os
from pathlib import Path
import pwd
import socket

PRIMARY = Path('/home/builder/work/alloy')
SECONDARY = Path('/home/builder/work/alloy-b')
APPROVED = (PRIMARY, SECONDARY)


def workspace_path(repo):
    return Path(repo).parent


def temp_path(work):
    return work / 'tmp' if work == SECONDARY else Path('/home/builder/work/tmp')


def namespace_prefix(work):
    """Serialize B's privileged namespace lifetime with A's network owners."""
    command = ['sudo', '-n']
    if work == SECONDARY:
        for name in ('harness-run.lock', 'controller.lock'):
            command += ['flock', '--wait', '120', str(PRIMARY / name)]
    return command


def validate_workspace(work, repo=None):
    """Reject foreign roots, symlink escapes and roots not owned by builder."""
    if socket.gethostname() != 'wakaba-build' or work not in APPROVED:
        raise RuntimeError(f'Only the approved NUC workspaces {APPROVED} are supported')
    owner = pwd.getpwnam('builder').pw_uid
    if os.geteuid() not in (0, owner):
        raise RuntimeError('Workspace tools must run as builder (or the network supervisor)')
    if repo is not None and repo != work / 'repo':
        raise RuntimeError('Tools must come from the selected workspace repo/')
    for path in (work, work / 'repo', work / 'src', work / 'src/out',
                 work / 'src/out/Release_Alloy', work / 'src/out/Verify_Alloy', work / 'apply-state',
                 work / 'logs', work / 'evidence', work / 'support',
                 temp_path(work)):
        if path.resolve() != path or (path.exists() and
                (not path.is_dir() or path.stat().st_uid != owner)):
            raise RuntimeError(f'Not an owned, non-symlink workspace directory: {path}')
    if not work.is_dir() or not (work / 'repo').is_dir():
        raise RuntimeError('Deliver repo/ into an existing owned workspace first')


def validate_source(work, root, repo=None):
    validate_workspace(work, repo)
    if root.resolve() != work / 'src':
        raise RuntimeError(f'Only the selected source tree {work / "src"} is supported')


def validate_output(work, out, repo=None):
    validate_workspace(work, repo)
    if out.resolve() not in {work / 'src/out' / name
                             for name in ('Release_Alloy', 'Verify_Alloy')}:
        raise RuntimeError('Only the selected workspace release/verify outputs are supported')
