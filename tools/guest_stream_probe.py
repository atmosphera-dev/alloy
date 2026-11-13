#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Observe CPU stream/lease and native interaction results through the SDK."""
import argparse
from functools import partial
import http.server
import json
import os
from pathlib import Path
import select
import subprocess
import threading
import time

from guest_test_support.display import environment
from guest_test_support.session import OwnedProcesses
from workspace import validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


class Client:
    def __init__(self, out, evidence):
        env = environment(evidence / 'runtime', out)
        assert 'DISPLAY' not in env and 'WAYLAND_DISPLAY' not in env
        self.log = (evidence / 'consumer.log').open('xb')
        self.process = subprocess.Popen([str(out / 'alloy_guest_stream_consumer')],
            cwd=out, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.buffer = b''
        self.events = []

    def send(self, text):
        self.process.stdin.write((text + '\n').encode())
        self.process.stdin.flush()

    def pump(self, timeout):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            return
        data = os.read(self.process.stdout.fileno(), 65536)
        assert data, f'consumer exited {self.process.poll()}'
        self.log.write(data)
        self.log.flush()
        self.buffer += data
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            if line.startswith(b'GUEST_STREAM_EVENT '):
                event = json.loads(line[len(b'GUEST_STREAM_EVENT '):])
                assert event['event'] not in ('unavailable', 'invalid-command'), event
                self.events.append(event)

    def wait(self, event, predicate=lambda _: True, after=0, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            for value in self.events[after:]:
                if value['event'] == event and predicate(value):
                    return value
            remaining = deadline - time.monotonic()
            assert remaining > 0, f'timed out waiting for {event}: {self.events[-12:]}'
            self.pump(min(remaining, 1))

    def command(self, text, event, predicate=lambda _: True):
        after = len(self.events)
        self.send(text)
        return self.wait(event, predicate, after=after)

    def titles(self, count, after):
        deadline = time.monotonic() + 30
        while len([v for v in self.events[after:]
                   if v['event'] == 'title' and v['title'].startswith(('FRAME_', 'DAMAGE_'))]) < count:
            assert time.monotonic() < deadline, 'page/source stopped while leases were held'
            self.pump(1)

    def held(self):
        after = len(self.events)
        result = self.command('held', 'held-complete')
        rows = [v for v in self.events[after:] if v['event'] == 'held-frame']
        assert len(rows) == result['references'], (rows, result)
        assert all(v['expected'] and v['actual'] == v['expected'] for v in rows), rows
        return rows

    def finish(self):
        self.send('quit')
        self.wait('finished')
        assert self.process.wait(timeout=30) == 0

    def dispose(self):
        if self.process.poll() is None:
            try:
                self.send('quit')
                self.process.wait(timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=30)
        self.log.close()


def frames(out, evidence, base):
    client = Client(out, evidence)
    processes = None
    try:
        assert client.wait('prepared')['stream'] is True
        client.wait('configured', lambda v: v['width'] == 128 and v['height'] == 96)
        client.command('navigate ' + base + '/frames.html', 'navigate-queued', lambda v: v['ok'])
        client.wait('load', lambda v: v['status'] == 200)
        first = client.wait('frame', lambda v: v['document'] and v['pattern'] and v['source_frame'])
        after = len(client.events)
        client.titles(8, after)
        unique = {v['source_frame'] for v in client.events
                  if v['event'] == 'frame' and v['document'] and v['pattern']}
        assert len(unique) >= 5, unique
        after = len(client.events)
        client.command('navigate ' + base + '/damage.html', 'navigate-queued', lambda v: v['ok'])
        client.wait('load', lambda v: v['status'] == 200 and v['document'] > first['document'], after=after)
        client.wait('frame', lambda v: v['document'] > first['document'] and v['split_pattern'] and
                    v['left_frame'] != v['right_frame'], after=after)
        client.command('retain on', 'retaining', lambda v: v['on'])
        after = len(client.events)
        client.titles(15, after)
        held = client.held()
        assert len(held) == 6, held
        latest = max(v['sequence'] for v in held)
        client.command('release presentation', 'released')
        after = len(client.events)
        client.titles(10, after)
        assert not any(v['event'] == 'frame' for v in client.events[after:]), 'recorder lease released too early'
        assert len(client.held()) == 3
        client.command('retain off', 'retaining', lambda v: not v['on'])
        after = len(client.events)
        client.command('release recorder', 'released')
        recovered = client.wait('frame', lambda v: v['split_pattern'] and v['dropped'] > 0,
                                after=after)
        assert recovered['sequence'] > latest + 1, (latest, recovered)
        assert (recovered['left_frame'] - recovered['right_frame']) % 250 in (1, 249), recovered
        assert recovered['bytes'] == recovered['stride'] * recovered['height']
        client.command('retain on', 'retaining', lambda v: v['on'])
        client.titles(10, len(client.events))
        old = client.held()
        assert len(old) == 6
        generation = max(v['generation'] for v in old)
        after = len(client.events)
        client.command('configure 192 100 1.5', 'configure-queued', lambda v: v['ok'])
        geometry = client.wait('configured', lambda v: v['generation'] > generation and v['scale'] == 1.5,
                               after=after)
        assert len(client.held()) == 6
        client.command('retain off', 'retaining', lambda v: not v['on'])
        client.command('release presentation', 'released')
        assert len(client.held()) == 3
        after = len(client.events)
        client.command('release recorder', 'released')
        resized = client.wait('frame', lambda v: v['generation'] == geometry['generation'] and v['split_pattern'], after=after)
        assert (resized['left_frame'] - resized['right_frame']) % 250 in (1, 249), resized
        assert (resized['width'], resized['height'], resized['stride'], resized['bytes']) == (288, 150, 1152, 172800), resized
        after = len(client.events)
        client.command('configure 240 120 1.2', 'configure-queued', lambda v: v['ok'])
        fractional_geometry = client.wait('configured', lambda v: abs(v['scale'] - 1.2) < 1e-6, after=after)
        fractional = client.wait('frame', lambda v: v['generation'] == fractional_geometry['generation'] and v['split_pattern'], after=after)
        assert (fractional['width'], fractional['height'], fractional['stride']) == (288, 144, 1152), fractional
        client.command('retain on', 'retaining', lambda v: v['on'])
        client.titles(8, len(client.events))
        assert client.held()
        processes = OwnedProcesses(client.process.pid, out)
        client.send('close')
        client.wait('closed', lambda v: v['reason'] == 'closed')
        processes.exited()
        assert client.held(), 'retained frames vanished at session close'
        names = [v['event'] for v in client.events]
        assert names.index('stream-closed') < names.index('closed'), names
        control_threads = {v['thread'] for v in client.events if v['event'] in ('state', 'title', 'load', 'closed')}
        stream_threads = {v['thread'] for v in client.events if v['event'] in ('configured', 'frame', 'stream-closed')}
        assert len(control_threads) == len(stream_threads) == 1 and control_threads != stream_threads
        client.finish()
        return [{'case': 'windowless-unique-numbered-cpu-frames', 'first': first, 'unique_source_frames': len(unique)},
                {'case': 'independent-presentation-recorder-leases-and-full-recovery', 'recovered': recovered},
                {'case': 'resize-scale-with-retired-leases', 'configured': geometry, 'frame': resized},
                {'case': 'fractional-native-float-geometry', 'configured': fractional_geometry, 'frame': fractional},
                {'case': 'retained-pixels-after-close-and-separate-serialized-stream-sequence',
                 'control_threads': list(control_threads), 'stream_threads': list(stream_threads)}]
    finally:
        client.dispose()
        if processes is not None:
            processes.close()
        (evidence / 'events.json').write_text(json.dumps(client.events, indent=2) + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('frames', 'all'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    if not evidence.is_relative_to(WORK):
        parser.error('Evidence must stay in the assigned disk-backed guest workspace')
    evidence.mkdir(parents=True, mode=0o700, exist_ok=False)
    handler = partial(QuietHandler, directory=str(REPO / 'tools/testdata/guest-stream'))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    result = {'check': args.check, 'status': 'running'}
    start = time.monotonic()
    try:
        rows = frames(out, evidence, f'http://127.0.0.1:{server.server_port}')
        if args.check == 'all':
            raise RuntimeError('Input/popup/IME acceptance is not implemented in the CPU-only commit')
        result.update(status='passed', cases=rows, seconds=time.monotonic() - start)
        print('GUEST_STREAM_PASS', flush=True)
    except BaseException as error:
        result.update(status='failed', error=repr(error), seconds=time.monotonic() - start)
        raise
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
