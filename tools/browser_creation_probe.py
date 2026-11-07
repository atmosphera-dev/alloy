#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Native CEF creation callback lifetime regression, on the approved NUC."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from smoke import session_processes, terminate_session, wait_for_session_exit
from workspace import temp_path, validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--inside', action='store_true')
    args = parser.parse_args()
    assert socket.gethostname() == 'wakaba-build'
    assert os.environ['TMPDIR'] == str(temp_path(W))
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        primary = Path('/home/builder/work/alloy')
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(primary/'harness-run.lock'),
            'flock', '--wait', '1800', str(primary/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"',
            'alloy-browser-creation', 'env', 'TMPDIR='+str(temp_path(W)),
            'PYTHONDONTWRITEBYTECODE=1', 'python3', str(Path(__file__).resolve()),
            '--out', str(out), '--inside']
        return subprocess.run(['timeout', '--kill-after=30', '4200', *command], timeout=4240).returncode
    assert os.geteuid() == 1000
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    routes = subprocess.check_output(['ip', 'route'], text=True, timeout=10)
    assert [v['ifname'] for v in interfaces] == ['lo'] and not routes.strip()
    evidence = W/'evidence'/('browser-creation-'+out.name+'-'+str(time.time_ns()))
    evidence.mkdir()
    print('BROWSER_CREATION_EVIDENCE', evidence, flush=True)
    save(evidence/'isolation.json', dict(interfaces=interfaces, routes=routes,
        uid=os.geteuid(), netns=os.readlink('/proc/self/ns/net')))
    outcomes = []
    for mode in ('async-close', 'sync-close', 'ordinary'):
        root = evidence/mode
        root.mkdir()
        owned = {n: root/n for n in ('home', 'cache', 'config', 'data', 'runtime')}
        for path in owned.values():
            path.mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(owned['home']), XDG_CACHE_HOME=str(owned['cache']),
            XDG_CONFIG_HOME=str(owned['config']), XDG_DATA_HOME=str(owned['data']),
            XDG_RUNTIME_DIR=str(owned['runtime']), ALLOY_BROWSER_CREATION_MODE=mode,
            ALLOY_BROWSER_CREATION_ROOT=str(root/'browser-root'))
        argv = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24',
            str(out/'alloy_browser_creation_consumer'), '--alloy-no-disk',
            '--ozone-platform=x11', '--disable-gpu']
        save(root/'argv.json', argv)
        process = None
        try:
            with (root/'stdio.log').open('x') as log:
                process = subprocess.Popen(argv, cwd=out, env=env, stdout=log,
                    stderr=subprocess.STDOUT, start_new_session=True)
                code = process.wait(timeout=90)
            remaining = wait_for_session_exit(process.pid, 10)
            text = (root/'stdio.log').read_text(errors='replace')
            events = [line.removeprefix('BROWSER_CREATION_EVENT ')
                for line in text.splitlines() if line.startswith('BROWSER_CREATION_EVENT ')]
            expected = ['created', 'closed', 'callback-returned', 'creation-returned']
            if mode == 'async-close':
                expected = ['creation-returned', 'created', 'closed', 'callback-returned']
            elif mode == 'ordinary':
                expected = ['creation-returned', 'created', 'callback-returned', 'closed']
            passed = (code == 0 and not remaining and events == expected and
                'PASS browser-creation-'+mode in text and 'FAIL browser-creation-' not in text)
            row = dict(mode=mode, exit=code, events=events, expected_events=expected,
                remaining=remaining, pass_=passed)
        except Exception as error:
            row = dict(mode=mode, pass_=False, error=str(error), type=type(error).__name__)
        finally:
            if process is not None and session_processes(process.pid):
                terminate_session(process.pid)
                process.wait(timeout=10)
        outcomes.append(row)
        save(root/'result.json', row)
        save(evidence/'outcomes.json', outcomes)
    passed = len(outcomes) == 3 and all(row['pass_'] for row in outcomes)
    save(evidence/'result.json', dict(pass_=passed, outcomes=outcomes))
    print(('PASS' if passed else 'FAIL')+' browser-creation-native-lifetimes', flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
