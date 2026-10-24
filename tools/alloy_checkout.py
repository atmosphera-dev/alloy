#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Create the pinned Linux checkout through Brave's own shallow init route."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess

from workspace import temp_path, validate_source, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
ROOT = WORK / 'src'


def run(args, cwd, capture=False, timeout=32400):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), cwd=cwd, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None,
                          timeout=timeout).stdout


def pinned_clone(path, pin):
    if not path.exists():
        path.mkdir(parents=True)
        run(['git', 'init', '--quiet'], path)
        run(['git', 'remote', 'add', 'origin', pin['url']], path)
    if not (path / '.git').is_dir():
        raise RuntimeError(f'Not an owned repository: {path}')
    head = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=path,
                          text=True, capture_output=True, timeout=30)
    if head.returncode:
        # A clone interrupted before checkout has no work to overwrite.
        if any(p.name != '.git' for p in path.iterdir()):
            raise RuntimeError(f'Unborn repository contains files: {path}')
        run(['git', 'fetch', '--depth=1', 'origin', pin['revision']], path)
        run(['git', 'checkout', '--detach', 'FETCH_HEAD'], path)
    verify(path, pin['revision'])


def verify(path, revision):
    head = run(['git', 'rev-parse', 'HEAD'], path, capture=True, timeout=30).strip()
    if head != revision:
        raise RuntimeError(f'{path}: expected {revision}, found {head}; refusing sync')
    print('PIN', path, head, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    try:
        validate_source(WORK, args.root, REPO)
    except RuntimeError as error:
        parser.error(str(error))
    WORK.mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(temp_path(WORK))
    os.environ['DEPOT_TOOLS_UPDATE'] = '0'
    pins = json.loads((REPO / 'pins.json').read_text())
    with (WORK / 'checkout.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for tool in ('node', 'pnpm'):
            version = run([tool, '--version'], WORK, capture=True, timeout=30).strip()
            if version != pins['tooling'][tool]:
                raise RuntimeError(f'{tool}: expected {pins["tooling"][tool]}, found {version}')
        state_path = WORK / 'checkout.json'
        state = json.loads(state_path.read_text()) if state_path.exists() else {}
        if state and state['pins'] != pins:
            raise RuntimeError('Pins changed; use a fresh owned workspace')
        state['pins'] = pins
        core = ROOT / 'brave'
        pinned_clone(core, pins['brave-core'])
        env_path = core / '.env'
        env_text = 'projects_depot_tools_revision=' + pins['tooling']['brave_depot_tools'] + '\n'
        if env_path.exists() and env_path.read_text() != env_text:
            raise RuntimeError('Existing Brave .env differs; refusing to overwrite')
        if not env_path.exists():
            env_path.write_text(env_text)
        if not state.get('initialized'):
            if (WORK / 'apply-state').exists():
                raise RuntimeError('Never init/sync an already CEF-composed tree')
            package = json.loads((core / 'package.json').read_text())
            if package['config']['projects']['chrome']['tag'] != pins['chromium']['tag']:
                raise RuntimeError('Brave pin does not select the Chromium pin')
            run(['pnpm', 'run', 'init', '--no-history', '--target_os', 'linux',
                 '--target_arch', 'x64'], core)
            verify(ROOT, pins['chromium']['revision'])
            verify(core / 'vendor/depot_tools', pins['tooling']['brave_depot_tools'])
            # Normal Brave preparation supplies branding/Siso files, as in the spike.
            run(['pnpm', 'run', 'build', 'Static', '--prepare_only'], core, timeout=600)
            state['initialized'] = True
            state_path.write_text(json.dumps(state, indent=2) + '\n')
        pinned_clone(ROOT / 'cef', pins['cef'])
        for path, revision in [(ROOT, pins['chromium']['revision']),
                               (core, pins['brave-core']['revision']),
                               (ROOT / 'cef', pins['cef']['revision']),
                               (core / 'vendor/depot_tools', pins['tooling']['brave_depot_tools'])]:
            verify(path, revision)
        print('ALLOY_CHECKOUT_PINS_OK', flush=True)


if __name__ == '__main__':
    main()
