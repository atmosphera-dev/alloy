#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Guest-session SDK, lifecycle, smoke and blocking consumer checks."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import time

from workspace import validate_output, workspace_path
from guest_test_support.display import environment
from guest_test_support.fixtures import Fixtures, tracker_loopback_aliases
from guest_test_support.session import Client, OwnedProcesses, descendants

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def fields(values):
    result = b''
    for value in values:
        encoded = value.encode()
        result += struct.pack('!I', len(encoded)) + encoded
    return result


def frame(kind, payload=b'', version=1, session=b'\x01' + b'\0' * 15):
    return struct.pack('!4sHHII16sQQQQQ', b'AGS1', version, kind,
                       len(payload), 0, session, 1, 0, 0, 0, 0) + payload


def incompatible(out, root, kind):
    env = environment(root, out)
    env.update(ALLOY_GUEST_RUNTIME='1', ALLOY_GUEST_BOOTSTRAP=str(out / 'alloy_guest_host'))
    control, peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    resources, resource_peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    process = os.posix_spawn(str(out / 'alloy_guest_host'),
        [str(out / 'alloy_guest_host'), '--alloy-supervisor'], env,
        file_actions=[(os.POSIX_SPAWN_DUP2, peer.fileno(), 198),
                      (os.POSIX_SPAWN_DUP2, resource_peer.fileno(), 200)])
    peer.close()
    resource_peer.close()
    handle = os.pidfd_open(process)
    control.settimeout(120)
    try:
        build = 'wrong-build'
        if kind == 'api':
            identity = (out / 'gen/brave/alloy/guest/build_identity.h').read_text()
            version = (out / 'gen/cef/include/cef_version.h').read_text()
            build = re.search(r'ALLOY_GUEST_BUILD_ID "([^"]+)"', identity)[1]
            build = re.search(r'#define CEF_VERSION "([^"]+)"', version)[1] + ':' + build
        payload = fields([build, 'wrong-api', 'memory', 'keep', 'unavailable', '1', '', '127'])
        control.sendall(frame(1, payload, version=2 if kind == 'version' else 1))
        response = control.recv(16384)
        if kind != 'version':
            assert len(response) >= 72 and struct.unpack('!H', response[6:8])[0] == 15, response
            assert response[72:] == b'incompatible', response
        else:
            assert not response, response
        poll = select.poll()
        poll.register(handle, select.POLLIN)
        assert poll.poll(30000), 'incompatible peer did not exit/reap'
        got, status = os.waitpid(process, 0)
        assert got == process
        return {'case': kind + '-mismatch', 'supervisor': process, 'status': status,
                'response': response.hex(), 'admitted': False}
    finally:
        control.close()
        resources.close()
        os.close(handle)


def compile_c(out, evidence):
    executable = evidence / 'guest_c_sdk_consumer'
    command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-DUSING_CEF_SHARED', '-I', str(WORK / 'src'),
                '-I', str(WORK / 'src/cef'), '-I', str(out / 'gen/cef'),
                str(REPO / 'tools/guest_test_support/sdk_consumer.c'),
                '-L', str(out), '-Wl,-rpath,' + str(out), '-lcef', '-o', str(executable)]
    with (evidence / 'compile-c.log').open('x') as log:
        subprocess.run(command, cwd=WORK, stdout=log, stderr=subprocess.STDOUT, check=True)
    env = environment(evidence / 'c-sdk', out)
    with (evidence / 'c-sdk/run.log').open('x') as log:
        subprocess.run([str(executable)], cwd=out, env=env,
                        stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    assert 'GUEST_C_SDK_PASS' in (evidence / 'c-sdk/run.log').read_text()
    return {'case': 'generated-native-c-sdk', 'passed': True}


def close_and_observe(client, out, name='guest'):
    processes = OwnedProcesses(client.process.pid, out)
    try:
        assert any(v['role'] == 'supervisor' for v in processes.members), processes.members
        client.send('close ' + name)
        client.closed(name)
        processes.exited()
        return processes.members
    finally:
        processes.close()


def foundation(out, evidence, fixtures):
    rows = [compile_c(out, evidence)]
    registration = evidence / 'registration-exit'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(REPO / 'src'), '-I', str(WORK / 'src'),
                    str(REPO / 'tools/guest_test_support/registration_exit.c'),
                    '-o', str(registration)], check=True)
    with (evidence / 'registration-exit.log').open('x') as log:
        subprocess.run([str(registration)], stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=30)
    rows.append({'case': 'original-child-exits-before-registration-or-ack', 'passed': True})
    for kind in ('version', 'build', 'api'):
        rows.append(incompatible(out, evidence / ('incompatible-' + kind), kind))
    with Client(out, evidence / 'terminate-start') as client:
        client.send('start guest terminate-start memory unavailable 1 -')
        client.closed()
        client.send('terminate guest')
        client.wait('guest', 'terminate-called', lambda v: v['state'] == 'CLOSED')
        client.finish()
        assert sum(v['event'] == 'closed' for v in client.events) == 1
        assert not descendants(client.process.pid)
        rows.append({'case': 'terminate-before-launch-and-after-closed', 'completions': 1})
    with Client(out, evidence / 'terminate-admission') as client:
        client.send('start guest manual memory unavailable 1 -')
        deadline = time.monotonic() + 30
        while True:
            processes = OwnedProcesses(client.process.pid, out)
            if any(v['role'] == 'supervisor' for v in processes.members):
                break
            processes.close()
            assert time.monotonic() < deadline, 'No in-flight launch observed'
            time.sleep(0.001)
        try:
            client.send('terminate guest\nterminate guest')
            client.closed()
            processes.exited()
            client.finish()
            assert sum(v['event'] == 'closed' for v in client.events) == 1
            rows.append({'case': 'terminate-in-flight-launch-admission',
                         'owned_processes_exited': processes.members, 'completions': 1})
        finally:
            processes.close()
    with Client(out, evidence / 'terminate-pending-close') as client:
        client.start()
        client.active()
        client.page(fixtures.base + '/hang-close', 'CLOSE_HANDLER_READY')
        processes = OwnedProcesses(client.process.pid, out)
        try:
            client.send('close guest')
            client.wait('guest', 'state', lambda v: v['state'] == 'CLOSING')
            deadline = time.monotonic() + 15
            while not any(v['path'] == '/close-entered' for v in fixtures.requests):
                assert time.monotonic() < deadline, 'Graceful beforeunload did not execute'
                time.sleep(0.01)
            assert not any(v['event'] == 'closed' for v in client.events)
            client.send('terminate guest\nterminate guest')
            client.closed()
            processes.exited()
            client.send('terminate guest')
            client.wait('guest', 'terminate-called', lambda v: v['state'] == 'CLOSED')
            client.finish()
            assert sum(v['event'] == 'closed' for v in client.events) == 1
            rows.append({'case': 'terminate-pending-page-delayed-graceful-close',
                         'owned_processes_exited': processes.members, 'completions': 1})
        finally:
            processes.close()
    with Client(out, evidence / 'terminate-stalled-consumer') as client:
        client.start(policy='stall-title')
        client.active()
        client.page(fixtures.base + '/title/STALL_CONSUMER', 'STALL_CONSUMER')
        processes = OwnedProcesses(client.process.pid, out)
        try:
            client.send('terminate guest')
            # Teardown must not need the blocked callback thread to return.
            processes.exited()
            client.send('resume guest')
            client.closed()
            client.finish()
            assert sum(v['event'] == 'closed' for v in client.events) == 1
            rows.append({'case': 'terminate-tears-down-before-stalled-callback-resumes',
                         'owned_processes_exited': processes.members, 'completions': 1})
        finally:
            processes.close()
    with Client(out, evidence / 'navigation-normalization') as client:
        client.start()
        client.active()
        # A raw host:port is an invalid scheme in pinned CEF. A bare loopback
        # host exercises its actual scheme insertion without requiring port 80
        # to be free or coupling this check to a pre-existing HTTP service.
        client.send('create guest 127.0.0.1/alloy-schemeless-navigation')
        ready = client.wait('guest', 'view-ready')
        page = client.wait('guest', 'load', lambda v: v['view'] == ready['view'])
        assert page['document'] > 0 and page['status'] != -300, page
        after = len(client.events)
        client.send(f'navigate guest {page["view"]} http://[')
        rejected = client.wait('guest', 'navigate-queued', after=after)
        assert rejected['ok'] is False, rejected
        client.send(f'navigate guest {page["view"]} {fixtures.base}/title/VALID_AFTER_INVALID')
        resumed = client.wait('guest', 'title', lambda v: v['title'] == 'VALID_AFTER_INVALID', after=after)
        assert resumed['view'] == page['view'] and resumed['document'] > page['document']
        assert not any(v['event'] == 'load' and v['document'] == page['document'] and v['status'] == -300
                       for v in client.events[after:])
        after = len(client.events)
        client.send('create guest -')
        blank = client.wait('guest', 'view-ready', after=after)
        after = len(client.events)
        client.send(f'navigate guest {blank["view"]} http://[')
        assert client.wait('guest', 'navigate-queued', after=after)['ok'] is False
        client.send(f'navigate guest {blank["view"]} {fixtures.base}/title/VALID_BLANK_OWNER')
        client.wait('guest', 'title', lambda v: v['view'] == blank['view'] and v['title'] == 'VALID_BLANK_OWNER',
                    after=after)
        assert not any(v['event'] == 'load' and v['document'] == 0 for v in client.events[after:])
        close_and_observe(client, out)
        rows.append({'case': 'schemeless-and-invalid-navigation-keeps-session-usable',
                     'page': page, 'invalid_result': rejected})
    with Client(out, evidence / 'commands-during-output') as client:
        client.start()
        client.active()
        client.send('create guest ' + fixtures.base + '/title-flood')
        flooded = client.wait('guest', 'title', lambda v:
                              v['title'].startswith('FLOOD_') and v['title'][6:].isdigit())
        after = len(client.events)
        client.send(f'navigate guest {flooded["view"]} {fixtures.base}/title/COMMAND_PROGRESS')
        client.wait('guest', 'title', lambda v: v['title'] == 'COMMAND_PROGRESS', after=after)
        after = len(client.events)
        client.send(f'navigate guest {flooded["view"]} {fixtures.base}/title-flood')
        client.wait('guest', 'title', lambda v:
                    v['title'].startswith('FLOOD_') and v['title'][6:].isdigit(), after=after)
        processes = close_and_observe(client, out)
        rows.append({'case': 'navigate-and-graceful-close-progress-during-native-output',
                     'owned_processes_exited': processes})
    with Client(out, evidence / 'spawn-descriptor-collision', reserve_descriptors=193) as client:
        client.wait('driver', 'descriptor-ceiling', lambda v: v['last'] == 193)
        client.start()
        client.active()
        client.page(fixtures.base + '/title/DESCRIPTOR_ALIAS_SAFE', 'DESCRIPTOR_ALIAS_SAFE')
        processes = OwnedProcesses(client.process.pid, out)
        try:
            browser = next(v for v in processes.members if v['role'] == 'browser')
            supervisor = next(v for v in processes.members if v['role'] == 'supervisor')
            handles = Path('/proc') / str(browser['pid']) / 'fd'
            assert (handles / '198').readlink() != (handles / '200').readlink(), 'Resource lane aliases control'
            supervisor_control = Path('/proc') / str(supervisor['pid']) / 'fd/198'
            assert supervisor_control.readlink() != (handles / '200').readlink(), 'Spawn source was overwritten'
        finally:
            processes.close()
        close_and_observe(client, out)
        rows.append({'case': 'spawn-sources-alias-reserved-targets', 'ceiling': 193})
    with Client(out, evidence / 'gpu-pending-navigation') as client:
        client.start()
        client.active()
        page = client.page(fixtures.base + '/title/GPU_BEFORE_PENDING', 'GPU_BEFORE_PENDING')
        after = len(client.events)
        client.send(f'navigate guest {page["view"]} {fixtures.base}/pending-navigation')
        assert fixtures.navigation_waiting.wait(30), 'Navigation did not reach server'
        processes = OwnedProcesses(client.process.pid, out)
        try:
            processes.kill('gpu-process')
            time.sleep(1)
            fixtures.navigation_release.set()
            resumed = client.wait('guest', 'title', lambda v: v['title'] == 'GPU_PENDING_COMPLETED', after=after)
            client.wait('guest', 'load', lambda v: v['document'] == resumed['document'] and v['status'] == 200,
                        after=after)
            close_and_observe(client, out)
            processes.exited()
            rows.append({'case': 'gpu-loss-preserves-pending-navigation', 'page': resumed})
        finally:
            fixtures.navigation_release.set()
            processes.close()
    with Client(out, evidence / 'lifecycle') as client:
        report = client.start(policy='manual')
        assert report['filesystem'] == 'unavailable' and report['stream_available']
        assert not report['extensions_unavailable']
        client.send('create guest ' + fixtures.base + '/title/NOT_ADMITTED')
        assert not client.wait('guest', 'create-queued')['ok']
        assert not any('/title/NOT_ADMITTED' in v['path'] for v in fixtures.requests)
        client.send('accept guest')
        client.active()
        page = client.page(fixtures.base + '/title/GUEST_FOUNDATION_DOM', 'GUEST_FOUNDATION_DOM')
        client.wait('guest', 'load', lambda v: v['view'] == page['view'] and v['status'] == 200)
        assert page['document'] > 0
        processes = close_and_observe(client, out)
        after = len(client.events)
        client.send(f'navigate guest {page["view"]} {fixtures.base}/title/STALE_VIEW')
        assert not client.wait('guest', 'navigate-queued', after=after)['ok']
        callback_names = {'prepared', 'state', 'title', 'load', 'view-ready', 'view-closed', 'closed'}
        callback_threads = {v['thread'] for v in client.events
                            if v['name'] == 'guest' and v['event'] in callback_names}
        assert len(callback_threads) == 1, callback_threads
        rows.append({'case': 'sdk-prepared-active-page-close-stale-view',
                     'page': page, 'processes_exited': processes,
                     'callback_threads': list(callback_threads)})
    for policy, required, reason in (('refuse', 'unavailable', 'closed'),
                                     ('auto', 'data-writes-denied', 'refused'),
                                     ('queue-close', 'unavailable', 'closed')):
        with Client(out, evidence / ('admission-' + policy + '-' + required)) as client:
            client.start(policy=policy, required=required)
            client.closed(reason=reason)
            assert not any(v['event'] == 'view-ready' for v in client.events)
            rows.append({'case': 'admission-' + policy + '-' + required, 'reason': reason})
    with Client(out, evidence / 'stateless') as client:
        client.start(mode='stateless')
        client.active()
        page = client.page(fixtures.base + '/set-cookie', 'COOKIE_REFUSED')
        close_and_observe(client, out)
        rows.append({'case': 'stateless-context-native-cookie-refusal', 'page': page})
    with Client(out, evidence / 'sharing-and-isolation') as client:
        first = client.start(name='first')
        client.active('first')
        client.page(fixtures.base + '/set-cookie', 'COOKIE_SET', 'first')
        shared = client.page(fixtures.base + '/cookie', 'COOKIE_PRESENT', 'first')
        before = OwnedProcesses(client.process.pid, out)
        second = client.start(name='second')
        client.active('second')
        assert first['id'] != second['id']
        isolated = client.page(fixtures.base + '/cookie', 'COOKIE_ABSENT', 'second')
        both = OwnedProcesses(client.process.pid, out)
        old_pids = {v['pid'] for v in before.members}
        new = [v for v in both.members if v['role'] == 'supervisor' and v['pid'] not in old_pids]
        assert len(new) == 1, both.members
        second_processes = OwnedProcesses(new[0]['pid'], out)
        try:
            second_processes.kill('browser')
            client.closed('second', 'failed')
            second_processes.exited()
            client.page(fixtures.base + '/cookie', 'COOKIE_PRESENT', 'first')
            rows.append({'case': 'shared-context-distinct-sessions-and-crash-groups',
                         'shared': shared, 'isolated': isolated})
            client.send('close first')
            client.closed('first')
            both.exited()
        finally:
            before.close()
            both.close()
            second_processes.close()
    for role in ('browser', 'supervisor', 'renderer', 'gpu-process', 'app'):
        with Client(out, evidence / ('death-' + role)) as client:
            client.start()
            client.active()
            page = client.page(fixtures.base + '/title/BEFORE_CRASH', 'BEFORE_CRASH')
            processes = OwnedProcesses(client.process.pid, out)
            try:
                if role == 'app':
                    client.abort()
                    processes.exited()
                elif role in ('browser', 'supervisor'):
                    processes.kill(role)
                    client.closed(reason='failed')
                    processes.exited()
                else:
                    # Chromium keeps a spare renderer. End every captured
                    # renderer so the test cannot accidentally kill only the
                    # spare and claim that the page's renderer recovered.
                    killed = processes.kill_all(role) if role == 'renderer' else [processes.kill(role)]
                    killed_ids = {entry['pid'] for entry in killed}
                    # Observe completion from the original kernel identity,
                    # not an unrelated replacement PID or process-group count.
                    for item, fd in zip(processes.members, processes.handles):
                        if item['pid'] in killed_ids:
                            poll = select.poll()
                            poll.register(fd, select.POLLIN)
                            assert poll.poll(30000), killed
                    time.sleep(1)
                    after = len(client.events)
                    client.send(f'navigate guest {page["view"]} {fixtures.base}/title/AFTER_CRASH')
                    resumed = client.wait('guest', 'title',
                        lambda v: v['title'] == 'AFTER_CRASH', after=after)
                    assert resumed['document'] > page['document']
                    close_and_observe(client, out)
                    processes.exited()
                rows.append({'case': 'death-' + role, 'owned_processes': processes.members})
            finally:
                processes.close()
    with Client(out, evidence / 'flexible-app', flexible=True) as client:
        client.wait('app', 'initialized')
        client.send('app ' + fixtures.base + '/title/FLEXIBLE_BEFORE')
        client.wait('app', 'title', lambda v: v['title'] == 'FLEXIBLE_BEFORE')
        client.start()
        client.active()
        client.page(fixtures.base + '/title/GUEST_IN_FLEXIBLE_APP', 'GUEST_IN_FLEXIBLE_APP')
        close_and_observe(client, out)
        client.send('app ' + fixtures.base + '/title/FLEXIBLE_AFTER')
        client.wait('app', 'title', lambda v: v['title'] == 'FLEXIBLE_AFTER')
        rows.append({'case': 'pre-existing-in-process-cef-context-still-usable'})
    return rows


def smoke(out, evidence, fixtures):
    with Client(out, evidence / 'client') as client:
        client.start()
        client.active()
        page = client.page(fixtures.base + '/smoke', 'Internet - Alloy smoke fixture')
        load = client.wait('guest', 'load', lambda v: v['view'] == page['view'] and v['status'] == 200)
        processes = close_and_observe(client, out)
        return [{'case': 'loopback-internet-title-through-guest', 'page': page,
                 'load': load, 'owned_processes_exited': processes}]


def blocking(out, evidence, fixtures, data):
    rows = []
    with tracker_loopback_aliases():
        for with_data in (False, True):
            label = 'with-data' if with_data else 'without-data'
            with Client(out, evidence / label, trusted_certificate=fixtures.certificate) as client:
                client.start(data=data if with_data else None)
                client.active()
                deadline = time.monotonic() + 40
                query = None
                expected = '?keep=1' if with_data else '?fbclid=alloy-query-probe&keep=1'
                while time.monotonic() < deadline:
                    # Match the legacy readiness condition without invoking its
                    # Xvfb/CDP helpers: fresh blank origin plus a startup-idle fence.
                    after = len(client.events)
                    client.send('create guest -')
                    view = client.wait('guest', 'view-ready', after=after)
                    time.sleep(4)
                    client.send(f'navigate guest {view["view"]} {fixtures.base}/query?fbclid=alloy-query-probe&keep=1')
                    query = client.wait('guest', 'title',
                        lambda v: v['view'] == view['view'] and v['title'].startswith('QUERY:'), after=after)
                    if query['title'] == 'QUERY:' + expected:
                        break
                assert query and query['title'] == 'QUERY:' + expected, query
                after = len(client.events)
                client.send('create guest ' + fixtures.base + '/blocking')
                page = client.wait('guest', 'title', lambda v: v['title'].startswith('BLOCKING:'), after=after)
                observed = json.loads(page['title'].removeprefix('BLOCKING:'))
                assert observed['neutral'] is True, observed
                assert observed['outcomes'] == ['error' if with_data else 'load'] * 3, observed
                assert observed['trackerExecuted'] is (not with_data), observed
                close_and_observe(client, out)
                rows.append({'case': label, 'query_consumer': query, 'script_consumer': observed})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('foundation', 'smoke', 'blocking'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--alloy-data-dir', type=Path)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    if not evidence.is_relative_to(WORK):
        parser.error('Evidence must stay in the assigned disk-backed guest workspace')
    if args.check == 'blocking' and not args.alloy_data_dir:
        parser.error('blocking requires the explicit synthetic --alloy-data-dir')
    evidence.mkdir(parents=True, mode=0o700, exist_ok=False)
    started = time.monotonic()
    result = {'check': args.check, 'out': str(out), 'status': 'running'}
    fixtures = None
    try:
        with Fixtures(REPO, tls_root=evidence / 'tls' if args.check == 'blocking' else None,
                      tracker_https=args.check == 'blocking') as fixtures:
            if args.check == 'foundation':
                rows = foundation(out, evidence, fixtures)
            elif args.check == 'smoke':
                rows = smoke(out, evidence, fixtures)
            else:
                rows = blocking(out, evidence, fixtures, args.alloy_data_dir.resolve())
            (evidence / 'fixture-requests.json').write_text(json.dumps(fixtures.requests, indent=2) + '\n')
        result.update(status='passed', cases=rows, seconds=time.monotonic() - started)
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        marker = {'foundation': 'GUEST_FOUNDATION_PASS', 'smoke': 'GUEST_SMOKE_PASS',
                  'blocking': 'GUEST_BLOCKING_PASS'}[args.check]
        print(marker, flush=True)
        return 0
    except BaseException as error:
        result.update(status='failed', error=repr(error), seconds=time.monotonic() - started)
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        raise
    finally:
        if fixtures is not None:
            (evidence / 'fixture-requests.json').write_text(json.dumps(fixtures.requests, indent=2) + '\n')
            (evidence / 'fixture-protocol-errors.json').write_text(json.dumps(fixtures.protocol_errors, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
