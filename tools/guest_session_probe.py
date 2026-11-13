#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Guest-session lifecycle and headless smoke consumers."""
import argparse
import json
from pathlib import Path
import struct

from workspace import validate_output, workspace_path
from guest_test_support.fixtures import Fixtures
from guest_test_support.session import Client, OwnedProcesses

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
    with Client(out, evidence / 'lifecycle') as client:
        report = client.start(policy='manual')
        assert report['filesystem'] == 'unavailable' and report['extensions_unavailable']
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
        return [{'case': 'sdk-prepared-active-page-close-stale-view', 'page': page,
                 'processes_exited': processes, 'callback_threads': list(callback_threads)}]


def smoke(out, evidence, fixtures):
    with Client(out, evidence / 'client') as client:
        client.start()
        client.active()
        page = client.page(fixtures.base + '/smoke', 'Internet - Alloy smoke fixture')
        load = client.wait('guest', 'load', lambda v: v['view'] == page['view'] and v['status'] == 200)
        processes = close_and_observe(client, out)
        return [{'case': 'loopback-internet-title-through-guest', 'page': page,
                 'load': load, 'owned_processes_exited': processes}]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('foundation', 'smoke'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    if not evidence.is_relative_to(WORK):
        parser.error('Evidence must stay in the assigned disk-backed guest workspace')
    evidence.mkdir(parents=True, mode=0o700, exist_ok=False)
    result = {'check': args.check, 'out': str(out), 'status': 'running'}
    try:
        with Fixtures(REPO) as fixtures:
            rows = (foundation if args.check == 'foundation' else smoke)(out, evidence, fixtures)
            (evidence / 'fixture-requests.json').write_text(json.dumps(fixtures.requests, indent=2) + '\n')
        result.update(status='passed', cases=rows)
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('GUEST_FOUNDATION_PASS' if args.check == 'foundation' else 'GUEST_SMOKE_PASS', flush=True)
        return 0
    except BaseException as error:
        result.update(status='failed', error=repr(error))
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        raise


if __name__ == '__main__':
    raise SystemExit(main())
