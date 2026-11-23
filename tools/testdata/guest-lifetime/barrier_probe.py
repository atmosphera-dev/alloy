#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Exercise the SDK interposer on actual custody before using it with CEF."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools'))
from guest_lifetime_probe import POLICIES, line
from guest_lifetime_sdk_probe import release_worker
from guest_test_support.display import environment
from workspace import validate_workspace, workspace_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    work = workspace_path(REPO)
    validate_workspace(work, REPO)
    fixture, evidence = args.fixture.resolve(), args.evidence.resolve()
    assert fixture.is_relative_to(work / 'evidence') and evidence.is_relative_to(work / 'evidence')
    evidence.mkdir(parents=True, exist_ok=False)
    library = evidence / 'sdk-barrier.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-I', str(REPO / 'src'), str(Path(__file__).with_name('sdk_barrier.c')),
                    '-ldl', '-o', str(library)], check=True)
    rows = []
    try:
        for mode in ('ready', 'probe'):
            for policy in POLICIES:
                root = evidence / mode / policy
                root.mkdir(parents=True)
                env = environment(root / 'environment', fixture)
                grant = root / 'grant'
                os.mkfifo(grant, 0o600)
                env.update(LD_PRELOAD=str(library), ALLOY_FIXTURE_CUSTODY_BARRIER=mode,
                           ALLOY_FIXTURE_CUSTODY_GRANT=str(grant))
                with (root / 'stderr.log').open('x') as log:
                    process = subprocess.Popen([str(fixture / 'driver'), policy, 'signals'],
                                               env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                               stderr=log)
                    try:
                        event = line(process)
                        expected = 'probe-stopped' if mode == 'probe' else 'ready-barrier'
                        assert event.startswith('GUEST_EVENT ') and json.loads(event[12:])['event'] == expected, event
                        release_worker(grant)
                        launched = line(process)
                        assert launched.startswith('LAUNCHED 1 OWNER '), launched
                        process.stdin.write(b'K')
                        process.stdin.flush()
                        assert line(process) == 'REAPED APP_SIGNALS_UNCHANGED'
                        assert process.wait(timeout=10) == 0
                        rows.append(dict(mode=mode, policy=policy, event=expected, passed=True))
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait(timeout=10)
                        process.stdin.close()
                        process.stdout.close()
    finally:
        (evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
    print('LIFETIME_SDK_BARRIER_SELFCHECK_PASS', flush=True)


if __name__ == '__main__':
    main()
