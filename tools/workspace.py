# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The approved build-machine and GitHub Actions workspaces.

Selection follows the delivered repository, not cwd or an arbitrary environment
variable. Validation happens at entry points, before any locks or writes.
"""
import os
from pathlib import Path
import pwd
import re
import socket
import stat

PRIMARY = Path('/home/builder/work/alloy')
SECONDARY = Path('/home/builder/work/alloy-b')
APPROVED = (PRIMARY, SECONDARY)
CI = Path('/home/runner/alloy')
HOSTS = ('wakaba-build', 'alloy-do-build')
DO_MARKER = Path('/etc/alloy-build-prerequisites-ready')


def is_build_host():
    """Admit named guests only with the provisioned image's protected marker."""
    host = socket.gethostname()
    if host in HOSTS:
        return True
    if not re.fullmatch(r'alloy-[a-z](?:[a-z0-9-]{0,46}[a-z0-9])?', host):
        return False
    try:
        marker = DO_MARKER.lstat()
    except OSError:
        return False
    return (stat.S_ISREG(marker.st_mode) and marker.st_uid == 0 and
            not marker.st_mode & 0o022)


def workspace_path(repo):
    return Path(repo).parent


def temp_path(work):
    """Keep scratch workspace-local, preserving the original NUC temp path."""
    return work / 'tmp' if work in (SECONDARY, CI) else Path('/home/builder/work/tmp')


def workspace_owner(work):
    """Resolve the account used for validation and unprivileged consumers."""
    if work == CI and os.environ.get('GITHUB_ACTIONS') == 'true':
        return pwd.getpwuid(os.geteuid())
    return pwd.getpwnam('builder')


def namespace_prefix(work):
    """Serialize B's privileged namespace lifetime with A's network owners."""
    command = ['sudo', '-n']
    if work == SECONDARY:
        for name in ('harness-run.lock', 'controller.lock'):
            command += ['flock', '--wait', '120', str(PRIMARY / name)]
    return command


def validate_workspace(work, repo=None):
    """Reject foreign roots, symlink escapes and directories not owned by the account."""
    if work == CI:
        if os.environ.get('GITHUB_ACTIONS') != 'true':
            raise RuntimeError('The CI workspace requires GITHUB_ACTIONS=true')
    elif not is_build_host() or work not in APPROVED:
        raise RuntimeError(f'Only the approved build workspaces {APPROVED} on {HOSTS} '
                           'or provisioned alloy-<task> hosts are supported')
    owner = workspace_owner(work).pw_uid
    if os.geteuid() not in (0, owner):
        raise RuntimeError('Workspace tools must run as the owner (or the network supervisor)')
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
