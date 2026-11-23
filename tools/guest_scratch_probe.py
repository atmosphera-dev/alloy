#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Real CEF private scratch lifetime and disclosed observer-limit proofs."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess

from guest_lifetime_sdk_probe import POLICIES, sandbox_receipt
from guest_test_support.display import environment
from guest_test_support.fixtures import Fixtures
from guest_test_support.session import Client, OwnedProcesses
from guest_test_support.scratch import scratch_view, scratch_receipt, scratch_reclaimed, scratch_references
from workspace import workspace_path, validate_output

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    assert evidence.is_relative_to(WORK / 'evidence')
    evidence.mkdir(parents=True, exist_ok=False)
    rows = []
    # A reaped-or-exiting task has no mount view. Keep a confirmed fixture
    # zombie throughout the matrix so unrelated build exits cannot break scans.
    zombie = subprocess.Popen(['/bin/true'])
    zombie_handle = os.pidfd_open(zombie.pid)
    event = select.poll()
    event.register(zombie_handle, select.POLLIN)
    assert event.poll(10000)
    os.close(zombie_handle)
    try:
        with Fixtures(REPO) as fixtures:
            for check in ('close', 'app-loss', 'init-loss', 'inner-loss', 'observer'):
                for policy in POLICIES:
                    root = evidence / check / policy
                    env = environment(root / 'environment', out)
                    env['ALLOY_FIXTURE_SIGCHLD'] = policy
                    with Client(out, root / 'client', launch_environment=env) as client:
                        client.start()
                        client.active()
                        client.page(fixtures.base + '/title/PRIVATE_SCRATCH', 'PRIVATE_SCRATCH')
                        processes = OwnedProcesses(client.process.pid, out)
                        borrowed = None
                        try:
                            pid = next(row['pid'] for row in processes.members if row['role'] == 'supervisor')
                            mount = scratch_receipt(pid)
                            sandbox = sandbox_receipt(processes)
                            canary = scratch_view(pid) / 'private-scratch-canary'
                            canary.write_bytes(b'PRIVATE_CANARY')
                            assert not (Path('/dev/shm') / canary.name).exists()
                            if check == 'observer':
                                borrowed = os.open(canary, os.O_RDONLY | os.O_CLOEXEC)
                            if check in ('app-loss', 'observer'):
                                client.abort()
                            elif check == 'close':
                                client.send('close guest')
                                client.closed()
                            else:
                                processes.kill('wait-owner' if check == 'init-loss' else 'supervisor')
                                client.closed(reason='failed')
                            processes.exited()
                            observer = None
                            if borrowed is not None:
                                assert os.pread(borrowed, 14, 0) == b'PRIVATE_CANARY'
                                observer = scratch_references(mount)
                                assert any(row.get('pid') == os.getpid() and
                                           row.get('fd') == str(borrowed) for row in observer), observer
                                os.close(borrowed)
                                borrowed = None
                            scratch_reclaimed(mount)
                            if check not in ('app-loss', 'observer'):
                                assert client.wait('driver', 'signal-policy-preserved')['ok']
                                client.finish()
                            rows.append(dict(check=check, policy=policy, passed=True, mount=mount,
                                             sandbox=sandbox, observer_limit=observer))
                            (evidence / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
                        finally:
                            if borrowed is not None:
                                os.close(borrowed)
                            processes.close()
    finally:
        assert zombie.wait(timeout=10) == 0
        (evidence / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print('GUEST_PRIVATE_SCRATCH_PASS', flush=True)


if __name__ == '__main__':
    main()
