#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Actual Chromium newborn stopped before registration, then custodian loss."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools'))
from guest_lifetime_sdk_probe import POLICIES, sandbox_receipt
from guest_test_support.display import environment
from guest_test_support.fixtures import Fixtures
from guest_test_support.session import Client, OwnedProcesses
from workspace import workspace_path, validate_output


def line(process):
    ready, _, _ = select.select([process.stdout], [], [], 40)
    assert ready, 'No zygote tracer receipt'
    value = process.stdout.readline().decode('ascii').strip()
    assert value, ('Tracer exited', process.poll())
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--policy', choices=('all', *POLICIES), default='all')
    args = parser.parse_args()
    work = workspace_path(REPO)
    out = args.out.resolve()
    validate_output(work, out, REPO)
    evidence = args.evidence.resolve()
    assert evidence.is_relative_to(work / 'evidence')
    evidence.mkdir(parents=True, exist_ok=False)
    tracer = evidence / 'zygote-fork'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(Path(__file__).with_name('zygote_fork.c')), '-o', str(tracer)], check=True)
    rows = []
    try:
        with Fixtures(REPO) as fixtures:
            for role in ('wait-owner', 'supervisor'):
                for policy in POLICIES if args.policy == 'all' else (args.policy,):
                    root = evidence / role / policy
                    env = environment(root / 'environment', out)
                    env['ALLOY_FIXTURE_SIGCHLD'] = policy
                    with Client(out, root / 'client', launch_environment=env) as client:
                        client.start()
                        client.active()
                        client.page(fixtures.base + '/title/BEFORE_FORK', 'BEFORE_FORK')
                        processes = OwnedProcesses(client.process.pid, out)
                        observer = None
                        try:
                            sandbox = sandbox_receipt(processes)
                            renderer_parents = {int(line.split(':', 1)[1])
                                                for row in processes.members if row['role'] == 'renderer'
                                                for line in (Path('/proc') / str(row['pid']) / 'status').read_text().splitlines()
                                                if line.startswith('PPid:')}
                            candidates = [row for row in processes.members if row['role'] == 'zygote' and
                                          row['pid'] in renderer_parents and
                                          b'--no-zygote-sandbox' not in
                                          (Path('/proc') / str(row['pid']) / 'cmdline').read_bytes()]
                            assert len(candidates) == 1, candidates
                            with (root / 'tracer.log').open('x') as log:
                                observer = subprocess.Popen(['sudo', '-n', str(tracer), str(candidates[0]['pid'])],
                                                            stdout=subprocess.PIPE, stderr=log)
                                seized = line(observer)
                                assert seized == 'SEIZED ' + str(candidates[0]['pid']), seized
                                # A separate view requests a fresh renderer from the actual zygote.
                                client.send('create guest ' + fixtures.base + '/title/NEWBORN')
                                stopped = line(observer)
                                assert stopped.startswith('STOPPED_UNREGISTERED '), stopped
                                newborn = int(stopped.split()[1])
                                assert stopped.endswith('STATE T TRACER 0'), stopped
                                status = (Path('/proc') / str(newborn) / 'status').read_text()
                                (root / 'newborn-status.txt').write_text(status)
                                processes.kill(role)
                                exited = line(observer)
                                assert exited == 'ORIGINAL_NEWBORN_EXITED ' + str(newborn), exited
                                assert observer.wait(timeout=10) == 0
                                client.closed(reason='failed')
                                processes.exited()
                                assert client.wait('driver', 'signal-policy-preserved')['ok']
                                client.finish()
                                rows.append(dict(role=role, policy=policy, passed=True, seized=seized,
                                                 stopped=stopped, exited=exited, sandbox=sandbox))
                                (evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
                        finally:
                            processes.close()
                            if observer is not None:
                                if observer.poll() is None:
                                    client.abort()
                                    observer.wait(timeout=65)
                                observer.stdout.close()
    finally:
        (evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
    print('LIFETIME_REAL_ZYGOTE_FORK_PASS', flush=True)


if __name__ == '__main__':
    main()
