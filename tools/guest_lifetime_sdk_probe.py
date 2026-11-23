#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Real SDK/CEF enclosure and forced-retirement consumer proofs."""
import argparse
import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from guest_test_support.display import environment
from guest_test_support.fixtures import Fixtures
from guest_test_support.launch_trace import RuntimeLaunchTrace
from guest_test_support.session import Client, OwnedProcesses
from workspace import validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
POLICIES = ('default', 'ignored', 'nochildwait', 'reaper')
CHECKS = ('close', 'terminate', 'stopped-init', 'stopped-inner', 'inner-loss',
          'init-loss', 'app-loss', 'blocked-callback', 'clone-refusal', 'wait-refusal',
          'kill-refusal', 'starting-close', 'starting-terminate',
          'ready-close', 'ready-terminate', 'probe-close', 'probe-terminate',
          'ready-close-denied', 'ready-terminate-denied',
          'stopped-inner-caller-denied', 'blocked-callback-caller-denied')


def process_status(pid):
    return dict(line.split(':', 1) for line in
                (Path('/proc') / str(pid) / 'status').read_text().splitlines())


def stop_original(processes, role):
    index = next(i for i, row in enumerate(processes.members) if row['role'] == role)
    signal.pidfd_send_signal(processes.handles[index], signal.SIGSTOP)
    pid = processes.members[index]['pid']
    deadline = time.monotonic() + 10
    while process_status(pid)['State'].strip()[0] not in ('T', 't'):
        assert time.monotonic() < deadline, ('SIGSTOP not observed', pid)
        time.sleep(.01)
    return pid


def release_worker(path):
    deadline = time.monotonic() + 10
    while True:
        try:
            descriptor = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as error:
            if error.errno != errno.ENXIO or time.monotonic() >= deadline:
                raise
            time.sleep(.01)
    try:
        assert os.write(descriptor, b'G') == 1
    finally:
        os.close(descriptor)


def wait_probe_group_stop(trace, observation):
    assert observation['state'] in ('T', 't'), observation
    path = trace.path.parent / (trace.path.name + '.' + str(observation['pid']))
    deadline = time.monotonic() + 10
    while True:
        try:
            text = path.read_text(encoding='ascii')
        except FileNotFoundError:
            text = ''
        # A generic ptrace stop is not enough. This exact pidfd-derived probe
        # must have reached its kernel SIGSTOP group-stop before cancellation.
        if '--- stopped by SIGSTOP ---' in text:
            return
        assert time.monotonic() < deadline, ('No confirmed SIGSTOP group-stop', observation, path)
        time.sleep(.01)


def sandbox_receipt(processes):
    owner = next(row for row in processes.members if row['role'] == 'wait-owner')
    renderers = [row for row in processes.members if row['role'] == 'renderer']
    assert renderers, processes.members
    outer = Path('/proc') / str(owner['pid'])
    assert process_status(owner['pid'])['NSpid'].split()[-1] == '1'
    rows = []
    for row in renderers:
        proc = Path('/proc') / str(row['pid'])
        status = process_status(row['pid'])
        namespaces = {name: str((proc / 'ns' / name).readlink()) for name in ('user', 'pid')}
        assert all(namespaces[name] != str((outer / 'ns' / name).readlink())
                   for name in namespaces), (row, namespaces)
        assert status['Seccomp'].strip() == '2' and status['NoNewPrivs'].strip() == '1', status
        rows.append(dict(process=row, namespaces=namespaces, nspid=status['NSpid'].split(),
                         seccomp=status['Seccomp'].strip(), no_new_privs=status['NoNewPrivs'].strip()))
    return rows


def case(out, evidence, fixtures, policy, check, denied, barrier):
    root = evidence / check / policy
    root.mkdir(parents=True)
    env = environment(root / 'environment', out)
    env['ALLOY_FIXTURE_SIGCHLD'] = policy
    prefix = []
    trace = None
    refusal = check.endswith('-refusal')
    starting = check.startswith('starting-')
    gated = check.startswith(('ready-', 'probe-'))
    grant = root / 'custody-grant'
    if gated:
        os.mkfifo(grant, 0o600)
        env.update(LD_PRELOAD=str(barrier), ALLOY_FIXTURE_CUSTODY_BARRIER=check.split('-')[0],
                   ALLOY_FIXTURE_CUSTODY_GRANT=str(grant))
    if refusal or starting or gated:
        trace = RuntimeLaunchTrace(out, root / 'launches')
        prefix = trace.command()
        if refusal:
            prefix += [str(denied), check.split('-')[0]]
    with Client(out, root / 'client', launch_environment=env, launch_prefix=prefix) as client:
        if gated:
            client.send('start guest auto memory unavailable 1 -')
            observation = client.wait('driver', 'probe-stopped' if check.startswith('probe-') else 'ready-barrier')
            if check.startswith('probe-'):
                wait_probe_group_stop(trace, observation)
            command = check.split('-')[1]
            if check.endswith('-denied'):
                processes = OwnedProcesses(client.process.pid, out)
                try:
                    stopped = stop_original(processes, 'wait-owner')
                    wait_probe_group_stop(trace, dict(pid=stopped,
                                          state=process_status(stopped)['State'].strip()[0]))
                finally:
                    processes.close()
            client.send(command + ('-denied' if check.endswith('-denied') else '') + ' guest')
            if check.endswith('-denied'):
                client.wait('driver', 'caller-kill-denied')
            client.wait('guest', command + '-called')
            # The API call has returned, so cancellation won the admission
            # mutex. Only now permit the worker to process its receipt.
            release_worker(grant)
            client.closed()
            assert client.wait('driver', 'signal-policy-preserved')['ok']
            client.finish()
            launches = trace.launches()
            assert all(row['executable'] == str(out / 'alloy_guest_host') and
                       row['arguments'][1:] == ['--alloy-supervisor', '--alloy-wait-gate']
                       for row in launches), launches
            assert len([row for row in client.events if row['event'] == 'closed']) == 1
            return dict(policy=policy, check=check, passed=True, allowed_pre_admission=launches)
        if starting:
            request = 'close-start' if check == 'starting-close' else 'terminate-start'
            client.send(f'start guest {request} memory unavailable 1 -')
            client.closed()
            assert client.wait('driver', 'signal-policy-preserved')['ok']
            client.finish()
            assert not trace.launches(), 'Cancelled startup admitted a supervisor or engine'
            assert len([row for row in client.events if row['event'] == 'closed']) == 1
            return dict(policy=policy, check=check, passed=True)
        if refusal:
            # Exercise both required and developmental request modes. Neither
            # may admit a runtime when the custody prerequisite was refused.
            for name, required in (('required', 'data-writes-denied'), ('developmental', 'unavailable')):
                report = client.start(name=name, required=required)
                expected = {'clone-refusal': 'clone3(', 'wait-refusal': 'waitid(P_PIDFD)',
                            'kill-refusal': 'pidfd_send_signal(SIGKILL)'}[check]
                assert report['filesystem'] == 'unavailable' and expected in report['limits'], report
                assert report['extensions_unavailable'], report
                client.closed(name, reason='failed')
                client.send('create ' + name + ' ' + fixtures.base + '/title/NEVER_ADMITTED')
                assert not client.wait(name, 'create-queued')['ok']
            client.finish()
            assert not trace.launches(), 'Refused startup admitted a supervisor or engine'
            return dict(policy=policy, check=check, passed=True)
        blocked = check.startswith('blocked-callback')
        client.start(policy='stall-title' if blocked else 'auto')
        client.active()
        title = 'STALL_CONSUMER' if blocked else 'LIFETIME_PAGE_LOADED'
        page = client.page(fixtures.base + '/title/' + title, title)
        if blocked:
            client.wait('guest', 'callback-blocked')
        else:
            client.wait('guest', 'load', lambda value: value['document'] == page['document'] and
                        value['status'] == 200)
        processes = OwnedProcesses(client.process.pid, out)
        try:
            sandbox = sandbox_receipt(processes)
            if check in ('stopped-init', 'stopped-inner', 'stopped-inner-caller-denied',
                         'blocked-callback-caller-denied'):
                stop_original(processes, 'wait-owner' if check == 'stopped-init' else 'supervisor')
            if check == 'app-loss':
                client.abort()
                processes.exited()
                return dict(policy=policy, check=check, passed=True, sandbox=sandbox)
            reason = 'closed'
            if check in ('inner-loss', 'init-loss'):
                processes.kill('supervisor' if check == 'inner-loss' else 'wait-owner')
                reason = 'failed'
            else:
                command = 'close' if check == 'close' else 'terminate'
                client.send(command + ('-denied' if check.endswith('-denied') else '') + ' guest')
                if check.endswith('-denied'):
                    client.wait('driver', 'caller-kill-denied')
            if blocked:
                client.wait('guest', 'terminate-called')
                # Observe every original exit BEFORE releasing the callback.
                processes.exited()
                assert not any(row['event'] in ('callback-resumed', 'closed') for row in client.events)
                client.send('resume guest')
                client.wait('guest', 'callback-resumed')
            client.closed(reason=reason)
            if reason == 'closed':
                assert any(row['event'] == 'state' and row['state'] == 'CLOSING'
                           for row in client.events), client.events
            processes.exited()
            assert client.wait('driver', 'signal-policy-preserved')['ok']
            client.finish()
            closed = [row for row in client.events if row['name'] == 'guest' and row['event'] == 'closed']
            assert len(closed) == 1, closed
            if blocked:
                assert_callback_order(client.events)
            return dict(policy=policy, check=check, passed=True, sandbox=sandbox,
                        originals=processes.members)
        finally:
            processes.close()


def assert_callback_order(events, name='guest'):
    resumed = [index for index, row in enumerate(events)
               if row['name'] == name and row['event'] == 'callback-resumed']
    closed = [index for index, row in enumerate(events)
              if row['name'] == name and row['event'] == 'closed']
    assert len(resumed) == len(closed) == 1 and resumed[0] < closed[0], events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--check', choices=('all', *CHECKS), default='all')
    parser.add_argument('--policy', choices=('all', *POLICIES), default='all')
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    assert evidence.is_relative_to(WORK), evidence
    evidence.mkdir(parents=True, exist_ok=False)
    denied = evidence / 'denied-sdk'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(REPO / 'tools/testdata/guest-lifetime/denied_sdk.cc'),
                    '-o', str(denied)], check=True)
    barrier = evidence / 'sdk-barrier.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-I', str(REPO / 'src'), str(REPO / 'tools/testdata/guest-lifetime/sdk_barrier.c'),
                    '-ldl', '-o', str(barrier)], check=True)
    rows = []
    check = policy = None
    try:
        with Fixtures(REPO) as fixtures:
            for check in CHECKS if args.check == 'all' else (args.check,):
                for policy in POLICIES if args.policy == 'all' else (args.policy,):
                    rows.append(case(out, evidence, fixtures, policy, check, denied, barrier))
                    (evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
    except BaseException as error:
        rows.append(dict(check=check, policy=policy, passed=False, error=repr(error)))
        raise
    finally:
        (evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
    print('GUEST_LIFETIME_SDK_PASS', flush=True)


if __name__ == '__main__':
    main()
