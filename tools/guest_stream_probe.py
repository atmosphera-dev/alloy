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
import struct
import zlib

from guest_test_support.display import environment
from guest_test_support.session import OwnedProcesses
from workspace import validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/cursor.png':
            def chunk(kind, data):
                return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
            pixels = (b'\0' + bytes((240, 30, 80, 255)) * 16) * 16
            body = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 16, 16, 8, 6, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))
            self.send_response(200)
            self.send_header('Content-Type', 'image/png')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == '/clicked':
            with self.server.click_lock:
                self.server.clicks += 1
            self.send_response(204)
            self.end_headers()
            return
        if path == '/pending-input.html':
            self.server.navigation_waiting.set()
            assert self.server.navigation_release.wait(60), 'pending navigation fixture was not released'
            self.path = '/input.html'
        super().do_GET()


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


def input_checks(out, evidence, base, server):
    evidence.mkdir()
    client = Client(out, evidence)
    processes = None
    capture = 9876543000
    receipts = []

    def state(predicate=lambda _: True, after=0, view=1):
        event = client.wait('title', lambda v: v.get('view') == view and v['title'].startswith('INPUT:') and
                            predicate(json.loads(v['title'][6:])), after=after)
        return event, json.loads(event['title'][6:])

    def geometry(view=1):
        return next(v for v in reversed(client.events) if v['event'] == 'configured' and v.get('view') == view)

    def send(kind, arguments='', source='app', document=None, generation=None, view=1):
        nonlocal capture
        capture += 17
        if document is None:
            document = next(v['document'] for v in reversed(client.events)
                            if v['event'] == 'title' and v.get('view') == view)
        if generation is None:
            generation = geometry(view)['generation']
        after = len(client.events)
        queued = client.command(f'input {view} {source} {document} {generation} {capture} {kind} {arguments}', 'queued-input')
        assert queued['id'] > 0, queued
        receipt = client.wait('interaction', lambda v: v['id'] == queued['id'] and v['view'] == view, after=after)
        assert receipt['capture_us'] == capture and receipt['source'] == source and receipt['kind'] == kind, receipt
        assert receipt['document'] == document and receipt['generation'] == generation, receipt
        assert receipt['dispatch_us'] > 0, receipt
        assert sum(v['event'] == 'interaction' and v['view'] == view and v['id'] == queued['id']
                   for v in client.events) == 1
        receipts.append(receipt)
        return receipt

    def center(snapshot, element):
        rectangle = snapshot['rectangles'][element]
        # Synthetic app presentation transform, inverted before native DIP input.
        sx, sy, scale = 37, 23, 1.75
        presented_x = (rectangle['x'] + rectangle['width'] / 2) * scale + sx
        presented_y = (rectangle['y'] + rectangle['height'] / 2) * scale + sy
        return round((presented_x - sx) / scale), round((presented_y - sy) / scale)

    def click(snapshot, element, source='app'):
        x, y = center(snapshot, element)
        down = send('pointer', f'down {x} {y} 0 0 1 16', source)
        up = send('pointer', f'up {x} {y} 0 0 1 0', source)
        assert down['outcome'] == up['outcome'] == 'accepted', (down, up)
        return up

    def pixel(snapshot, receipt, color):
        rectangle = snapshot['rectangles']['click']
        x = int((rectangle['x'] + 5) * snapshot['dpr'])
        y = int((rectangle['y'] + 5) * snapshot['dpr'])
        deadline = time.monotonic() + 10
        while True:
            value = client.command(f'pixel 1 {x} {y}', 'pixel')
            if (value['ok'] and value['capture_us'] >= receipt['dispatch_us'] and
                    (value['r'], value['g'], value['b'], value['a']) == (*color, 255)):
                return value
            assert time.monotonic() < deadline, (receipt, value, color)
            client.pump(0.02)

    try:
        assert client.wait('prepared')['stream']
        client.wait('configured')
        client.command('configure 480 320 1', 'configure-queued', lambda v: v['ok'])
        client.wait('configured', lambda v: v['width'] == 480 and v['height'] == 320)
        after = len(client.events)
        client.command('navigate ' + base + '/input.html', 'navigate-queued', lambda v: v['ok'])
        page, snapshot = state(after=after)
        client.wait('frame', lambda v: v['document'] == page['document'] and v['width'] == 480, after=after)
        assert send('focus', '1')['outcome'] == 'accepted'
        before = len(client.events)
        action = click(snapshot, 'click')
        _, snapshot = state(lambda v: v['clicks'] == 1, after=before)
        first_pixel = pixel(snapshot, action, (32, 180, 96))
        old_generation = geometry()['generation']
        after = len(client.events)
        client.command('configure 520 360 1.5', 'configure-queued', lambda v: v['ok'])
        client.wait('configured', lambda v: v['generation'] > old_generation and v['scale'] == 1.5, after=after)
        _, snapshot = state(lambda v: v['dpr'] == 1.5, after=after)
        x, y = center(snapshot, 'click')
        stale = send('pointer', f'down {x} {y} 0 0 1 16', generation=old_generation)
        assert stale['outcome'] == 'rejected:stale-generation', stale
        before = len(client.events)
        action = click(snapshot, 'click', 'agent')
        _, snapshot = state(lambda v: v['clicks'] == 2, after=before)
        scaled_pixel = pixel(snapshot, action, (96, 64, 192))
        before = len(client.events)
        x, y = center(snapshot, 'cursor')
        assert send('pointer', f'move {x} {y} 0 0 0 0')['outcome'] == 'accepted'
        cursor = client.wait('cursor', lambda v: v['view'] == 1 and v['bytes'] > 0, after=before)
        assert cursor['bytes'] == cursor['width'] * cursor['height'] * 4
        assert (cursor['b'], cursor['g'], cursor['r'], cursor['a']) == (80, 30, 240, 255), cursor
        assert 0 <= cursor['hotspot_x'] < cursor['width'] and 0 <= cursor['hotspot_y'] < cursor['height']
        click(snapshot, 'edit')
        assert send('key', '0 65 38 0 0 2 0 1')['outcome'] == 'accepted'
        assert send('key', '3 65 38 65 65 2 0 1')['outcome'] == 'accepted'
        assert send('key', '2 65 38 0 0 0 0 1')['outcome'] == 'accepted'
        _, snapshot = state(lambda v: v['text'] == 'A')
        before = len(client.events)
        cjk = '漢🙂'
        assert send('ime-set', f'{json.dumps(cjk, ensure_ascii=False)} 3 3 -1 -1')['outcome'] == 'accepted'
        bounds = client.wait('ime-bounds', lambda v: v['view'] == 1 and v['bounds'], after=before)
        edit = snapshot['rectangles']['edit']
        units = len(cjk.encode('utf-16-le')) // 2
        assert bounds['to'] - bounds['from'] == units and len(bounds['bounds']) == units, bounds
        assert any(r[2] > 0 for r in bounds['bounds']), bounds
        assert all(edit['x'] <= r[0] <= edit['x'] + edit['width'] and
                   edit['y'] <= r[1] <= edit['y'] + edit['height'] and r[2] >= 0 and r[3] > 0
                   for r in bounds['bounds']), (edit, bounds)
        assert send('ime-commit', f'{json.dumps(cjk, ensure_ascii=False)} 0 -1 -1')['outcome'] == 'accepted'
        _, snapshot = state(lambda v: v['text'] == 'A' + cjk)
        assert send('ime-set', '"´" 1 1 -1 -1')['outcome'] == 'accepted'
        assert send('ime-set', '"é" 1 1 -1 -1')['outcome'] == 'accepted'
        assert send('ime-commit', '"é" 0 -1 -1')['outcome'] == 'accepted'
        _, snapshot = state(lambda v: v['text'] == 'A' + cjk + 'é')
        assert any(v[0] == 'compositionend' and v[1] == cjk for v in snapshot['composed']), snapshot
        assert send('ime-set', '"X" 1 1 -1 -1')['outcome'] == 'accepted'
        assert send('ime-cancel')['outcome'] == 'accepted'
        state(lambda v: v['text'] == 'A' + cjk + 'é')
        assert send('ime-set', '"Z" 1 1 -1 -1')['outcome'] == 'accepted'
        assert send('ime-finish', '0')['outcome'] == 'accepted'
        _, snapshot = state(lambda v: v['text'] == 'A' + cjk + 'éZ')
        composed_text = snapshot['text']
        before = len(client.events)
        x, y = center(snapshot, 'touch')
        assert send('touch', f'7 1 {x} {y} 0 0.5 1 1 0 0 0')['outcome'] == 'accepted'
        assert send('touch', f'7 0 {x} {y} 0 0 1 1 0 0 0')['outcome'] == 'accepted'
        _, snapshot = state(lambda v: v['touches'] == 1, after=before)
        assert send('touch', f'7 2 {x} {y} 0 0.5 1 1 0 0 0')['outcome'] == 'rejected:touch-order'
        before = len(client.events)
        x, y = center(snapshot, 'capture')
        assert send('pointer', f'down {x} {y} 0 0 1 16')['outcome'] == 'accepted'
        assert send('pointer', f'move {x + 1} {y} 0 0 0 16')['outcome'] == 'accepted'
        state(lambda v: v['captureGot'] > 0, after=before)
        # Pinned CEF's MouseCaptureLost clears legacy widget/plugin capture;
        # it is not Element.releasePointerCapture. Keep that native dispatch
        # receipt distinct from the DOM capture release caused by pointer-up.
        capture_notification = send('capture-lost')
        assert capture_notification['outcome'] == 'accepted'
        send('pointer', f'up {x} {y} 0 0 1 0')
        _, snapshot = state(lambda v: v['captureLost'] > 0, after=before)
        before = len(client.events)
        click(snapshot, 'select')
        popup = client.wait('popup', lambda v: v['view'] == 1 and v['visible'] and v['width'] > 0 and v['height'] > 0, after=before)
        popup_frame = client.wait('frame', lambda v: v['view'] == 1 and v['surface'] == 1 and
                                  v['generation'] == popup['generation'], after=before)
        px, py = popup['width'] // 2, int(popup['height'] * 5 / 6)
        before = len(client.events)
        down = send('pointer', f'down {px} {py} 1 0 1 16')
        up = send('pointer', f'up {px} {py} 1 0 1 0')
        assert down['outcome'] == 'accepted', down
        assert up['outcome'] in ('accepted', 'rejected:stale-generation'), up
        _, snapshot = state(lambda v: v['selected'] == 'c', after=before)
        client.wait('popup', lambda v: v['view'] == 1 and not v['visible'], after=before)
        before = len(client.events)
        click(snapshot, 'window')
        window = client.wait('window-open', lambda v: v['view'] == 1, after=before)
        _, snapshot = state(lambda v: v['originalProxy'] and v['openerToken'], after=before)
        assert window['url'] == base + '/child.html' and window['gesture'], window
        client.command(f'window {window["id"]} attach 256 160 1.5', 'window-attach-queued', lambda v: v['ok'])
        client.wait('view-ready', lambda v: v['view'] == window['id'], after=before)
        child = client.wait('title', lambda v: v['view'] == window['id'] and v['title'].startswith('CHILD:'), after=before)
        assert json.loads(child['title'][6:])['token'] == 'original-opener-token', child
        child_frame = client.wait('frame', lambda v: v['view'] == window['id'] and v['width'] == 384 and v['height'] == 240, after=before)
        client.send(f'close-view {window["id"]}')
        state(lambda v: v['childClosed'])
        before = len(client.events)
        click(snapshot, 'deny')
        denied = client.wait('window-open', lambda v: v['view'] == 1, after=before)
        client.command(f'window {denied["id"]} deny', 'window-deny-called')
        _, snapshot = state(lambda v: v['childClosed'] and v['windows'] == 2, after=before)
        before = len(client.events)
        click(snapshot, 'blank')
        blank = client.wait('window-open', lambda v: v['view'] == 1, after=before)
        state(lambda v: v['blankOpener'] and v['windows'] == 3, after=before)
        assert blank['url'] in ('', 'about:blank'), blank
        client.command(f'window {blank["id"]} attach 160 100 1', 'window-attach-queued', lambda v: v['ok'])
        client.wait('title', lambda v: v['view'] == blank['id'] and v['title'] == 'BLANK_WRITTEN', after=before)
        client.wait('frame', lambda v: v['view'] == blank['id'] and v['width'] == 160 and v['height'] == 100, after=before)
        written = client.command(f'pixel {blank["id"]} 5 80', 'pixel')
        assert (written['r'], written['g'], written['b'], written['a']) == (120, 40, 200, 255), written
        client.send(f'close-view {blank["id"]}')
        _, snapshot = state(lambda v: v['childClosed'] and v['windows'] == 3, after=before)
        before = len(client.events)
        click(snapshot, 'window')
        retired = client.wait('window-open', lambda v: v['view'] == 1, after=before)
        client.command('navigate ' + base + '/input.html?retired=1', 'navigate-queued', lambda v: v['ok'])
        retirement = client.wait('window-retired', lambda v: v['id'] == retired['id'], after=before)
        active_page, snapshot = state(lambda v: v['windows'] == 0, after=before)
        assert not client.command(f'window {retired["id"]} attach 160 100 1', 'window-attach-queued')['ok']
        before = len(client.events)
        old_generation = geometry()['generation']
        with server.click_lock:
            previous_clicks = server.clicks
        client.command('navigate ' + base + '/pending-input.html', 'navigate-queued', lambda v: v['ok'])
        assert server.navigation_waiting.wait(30), 'navigation did not reach the pending fixture'
        pending_geometry = client.wait('configured', lambda v: v['generation'] > old_generation, after=before)
        x, y = center(snapshot, 'click')
        pending_refusals = []
        # The old visible document and a speculative replacement ID must neither
        # dispatch into the still-visible old page while navigation is pending.
        for document in (active_page['document'], active_page['document'] + 1):
            for action, modifiers in (('down', 16), ('up', 0)):
                refused = send('pointer', f'{action} {x} {y} 0 0 1 {modifiers}',
                               document=document, generation=pending_geometry['generation'])
                assert refused['outcome'] in ('rejected:document-not-ready', 'cancelled-unobserved'), refused
                pending_refusals.append(refused)
        with server.click_lock:
            assert server.clicks == previous_clicks, 'pending navigation dispatched a click to its old page'
        server.navigation_release.set()
        replacement = client.wait('title', lambda v: v.get('view') == 1 and
                                  v['document'] != active_page['document'] and v['title'].startswith('INPUT:'), after=before)
        snapshot = json.loads(replacement['title'][6:])
        # A script title can arrive before the replacement's compositor hit-test
        # data. Exercise scrolling on a presented document, not during its load.
        client.wait('frame', lambda v: v['view'] == 1 and v['document'] == replacement['document'] and
                    v['generation'] == pending_geometry['generation'], after=before)
        before = len(client.events)
        assert send('wheel', '260 300 0 0 -180 0', source='agent')['outcome'] == 'accepted'
        _, scrolled = state(lambda v: v['scrollY'] > 0, after=before)
        before = len(client.events)
        assert send('focus', '0')['outcome'] == 'accepted'
        state(lambda v: not v['focused'], after=before)
        assert send('focus', '1')['outcome'] == 'accepted'
        processes = OwnedProcesses(client.process.pid, out)
        client.send('close'); client.wait('closed', lambda v: v['reason'] == 'closed'); processes.exited()
        client.finish()
        for view in {v['view'] for v in receipts}:
            ordered = [v for v in receipts if v['view'] == view]
            assert [v['id'] for v in ordered] == sorted({v['id'] for v in ordered})
            assert [v['dispatch_us'] for v in ordered] == sorted(v['dispatch_us'] for v in ordered)
        return [{'case': 'native-app-agent-click-key-focus-capture-touch-wheel', 'receipts': len(receipts),
                 'first_response_pixel': first_pixel, 'scaled_response_pixel': scaled_pixel,
                 'native_legacy_capture_notification': capture_notification, 'scrollY': scrolled['scrollY']},
                {'case': 'utf16-cjk-emoji-dead-key-cancel-finish-and-caret-bounds', 'text': composed_text, 'bounds': bounds},
                {'case': 'copied-custom-cursor-and-separate-select-popup-hit-testing', 'cursor': cursor, 'popup': popup, 'frame': popup_frame},
                {'case': 'preserved-window-open-opener-proxy-placement-and-denial', 'window': window, 'child': child, 'frame': child_frame},
                {'case': 'blank-document-write-survives-attachment', 'window': blank, 'pixel': written},
                {'case': 'original-opener-retirement-refuses-pending-attachment', 'retirement': retirement},
                {'case': 'pending-navigation-cannot-dispatch-into-old-document', 'receipts': pending_refusals}]
    finally:
        server.navigation_release.set()
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
    server.navigation_waiting = threading.Event()
    server.navigation_release = threading.Event()
    server.click_lock = threading.Lock()
    server.clicks = 0
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    result = {'check': args.check, 'status': 'running'}
    start = time.monotonic()
    try:
        rows = frames(out, evidence, f'http://127.0.0.1:{server.server_port}')
        if args.check == 'all':
            rows.extend(input_checks(out, evidence / 'input', f'http://127.0.0.1:{server.server_port}', server))
        result.update(status='passed', cases=rows, seconds=time.monotonic() - start)
        print('GUEST_STREAM_PASS', flush=True)
    except BaseException as error:
        result.update(status='failed', error=repr(error), seconds=time.monotonic() - start)
        raise
    finally:
        server.navigation_release.set()
        server.shutdown()
        server.server_close()
        worker.join()
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
