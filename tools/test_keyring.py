#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only throwaway Secret Service, never a login/user keyring.

Run: python3 repo/tools/test_keyring.py --root <new-workspace-evidence-dir> -- <argv...>
The child and daemon share a fresh dbus-run-session, private HOME and XDG paths.
No activation is needed; the already-running provider is explicitly owned here.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from workspace import workspace_path, temp_path, validate_workspace

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def validate_test_root(root):
    validate_workspace(WORK, REPO)
    if root.resolve() != root or not root.is_relative_to(WORK / 'evidence'):
        raise RuntimeError('Private keyring tests require owned workspace evidence')


def validate_private_session(root, environ):
    validate_test_root(root)
    home = str(root / 'home')
    if environ.get('HOME') != home or environ.get('ALLOY_TEST_KEYRING') != home:
        raise RuntimeError('Private keyring HOME does not match its fixture')
    if environ.get('ALLOY_TEST_KEYRING_WORKSPACE') != str(WORK):
        raise RuntimeError('Private keyring workspace does not match its fixture')
    if environ.get('TMPDIR') != str(temp_path(WORK)):
        raise RuntimeError('Private keyring scratch is outside its workspace')
    address = environ.get('DBUS_SESSION_BUS_ADDRESS', '')
    if not address.startswith('unix:'):
        raise RuntimeError('Private keyring requires its own Unix session bus')
    if 'ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS' not in environ:
        raise RuntimeError('Private keyring session has no launcher identity')
    if address == environ.get('ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS'):
        raise RuntimeError('Inherited session bus is not a private test session')
    return address


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--absent', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        validate_test_root(root)
    except RuntimeError as error:
        parser.error(str(error))
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('A child command is required')
    if not args.inside:
        root.mkdir(mode=0o700)
        for name in ('home', 'data', 'config', 'cache', 'runtime', 'control'):
            (root / name).mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(root / 'home'),
            XDG_DATA_HOME=str(root / 'data'), XDG_CONFIG_HOME=str(root / 'config'),
            XDG_CACHE_HOME=str(root / 'cache'), XDG_RUNTIME_DIR=str(root / 'runtime'),
            TMPDIR=str(temp_path(WORK)), PYTHONDONTWRITEBYTECODE='1',
            ALLOY_TEST_KEYRING=str(root / 'home'),
            ALLOY_TEST_KEYRING_WORKSPACE=str(WORK),
            ALLOY_TEST_KEYRING_PARENT_BUS_ADDRESS=os.environ.get('DBUS_SESSION_BUS_ADDRESS', ''))
        for name in ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_STARTER_ADDRESS',
                     'DBUS_STARTER_BUS_TYPE', 'GNOME_KEYRING_CONTROL', 'DISPLAY', 'WAYLAND_DISPLAY',
                     'ALLOY_TEST_KEYRING_BUS_ADDRESS'):
            env.pop(name, None)
        return subprocess.run(['timeout', '--kill-after=20', '420',
            'dbus-run-session', '--', sys.executable, __file__, '--root', str(root),
            '--inside', *(['--absent'] if args.absent else []), '--', *command],
            env=env, cwd=WORK, timeout=450).returncode
    try:
        address = validate_private_session(root, os.environ)
    except RuntimeError as error:
        parser.error(str(error))
    # Only the validated private-session launcher binds this metadata for native
    # children. Workspace admission remains owned by tools/workspace.py.
    os.environ['ALLOY_TEST_KEYRING_BUS_ADDRESS'] = address

    def owner():
        result = subprocess.run(['timeout', '5', 'gdbus', 'call', '--session',
            '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
            '--method', 'org.freedesktop.DBus.GetNameOwner', 'org.freedesktop.secrets'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=8)
        return result.stdout.decode() if result.returncode == 0 else None

    assert owner() is None, 'Fresh private session unexpectedly has a provider'
    daemon = None
    receipt = dict(private_home=os.environ['HOME'], bus=os.environ['DBUS_SESSION_BUS_ADDRESS'],
                   initial_owner=None, absent=args.absent, command=command)
    with (root / 'daemon.log').open('xb') as log:
        try:
            if not args.absent:
                daemon = subprocess.Popen(['gnome-keyring-daemon', '--foreground',
                    '--components=secrets', '--control-directory=' + str(root / 'control'),
                    '--unlock'], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
                daemon.stdin.write(b'ALLOY_THROWAWAY_TEST_KEYRING\n')
                daemon.stdin.close()
                for _ in range(100):
                    current = owner()
                    if current:
                        break
                    if daemon.poll() is not None:
                        raise RuntimeError('Owned provider exited before ready')
                    time.sleep(0.05)
                else:
                    raise RuntimeError('Owned provider did not become ready')
                receipt.update(provider_pid=daemon.pid, provider_owner=current)
            result = subprocess.run(['timeout', '--kill-after=20', '360', *command],
                                    cwd=WORK, timeout=390)
            receipt['exit'] = result.returncode
            if args.absent:
                assert owner() is None, 'Consumer activated a missing provider'
            return result.returncode
        finally:
            if daemon is not None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=5)
                receipt['provider_exit'] = daemon.returncode
            (root / 'session.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
