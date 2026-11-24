#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Consume native guest integration outcomes through the translated SDK."""
import argparse
import base64
from functools import partial
import http.server
import hashlib
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import threading
import time
import zlib
from email.parser import BytesParser
from email.policy import default as mail_policy

from guest_test_support.display import environment
from guest_test_support.session import OwnedProcesses
from workspace import validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def png():
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 4, 4, 8, 6, 0, 0, 0)) +
            chunk(b'sRGB', b'\0') +
            chunk(b'IDAT', zlib.compress((b'\0' + bytes((20, 40, 60, 255)) * 4) * 4)) + chunk(b'IEND', b''))


class Fixture(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/no-content':
            self.send_response(204)
            self.end_headers()
            return
        if path == '/file-download.bin':
            # Chromium intentionally rejects HTTP/1.0 strong validators.
            self.protocol_version = 'HTTP/1.1'
            body = getattr(self.server, 'download_bytes', b'app-native-download-bytes')
            if getattr(self.server, 'download_started', None):
                self.server.download_started.set()
                self.server.download_release.wait()
            selected = body
            range_value = self.headers.get('Range')
            if range_value:
                assert range_value.startswith('bytes=') and ',' not in range_value, range_value
                lower, upper = range_value.removeprefix('bytes=').split('-', 1)
                start = int(lower); end = min(len(body) - 1, int(upper) if upper else len(body) - 1)
                if start >= len(body):
                    self.send_response(416)
                    self.send_header('Content-Range', f'bytes */{len(body)}')
                    self.send_header('Content-Length', '0'); self.end_headers(); return
                selected = body[start:end + 1]
                self.send_response(206)
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(body)}')
            else:
                self.send_response(200)
            if 'case=resume' in self.path:
                self.server.download_ranges.append(range_value)
                if range_value:
                    self.server.download_resume_started.set()
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Disposition', 'attachment; filename="download.bin"')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('ETag', '"' + hashlib.sha256(body).hexdigest() + '"')
            self.send_header('Connection', 'close')
            self.send_header('Content-Length', str(len(selected)))
            self.end_headers()
            try:
                if 'case=resume' in self.path and range_value and not self.server.download_resume_release.wait(30):
                    self.close_connection = True
                    return
                if 'case=resume' in self.path and not range_value and not self.server.download_interrupted:
                    self.server.download_interrupted = True
                    self.wfile.write(selected[:len(selected) // 2]); self.wfile.flush(); self.close_connection = True
                    return
                self.wfile.write(selected)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if path in ('/phase', '/pixel.png'):
            body = str(self.server.phase).encode() if path == '/phase' else png()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json' if path == '/phase' else 'image/png')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        if self.path != '/file-upload':
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers['Content-Length']))
        message = BytesParser(policy=mail_policy).parsebytes(
            ('Content-Type: ' + self.headers['Content-Type'] + '\r\nMIME-Version: 1.0\r\n\r\n').encode() + body)
        files = []
        for part in message.iter_parts():
            data = part.get_payload(decode=True)
            files.append(dict(name=part.get_filename(), size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        response = json.dumps(dict(files=files)).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)


class Client:
    def __init__(self, out, root, mode='memory', attach=True):
        root.mkdir(parents=True, mode=0o700)
        self.root = root
        self.out = out
        self.mode = mode
        env = environment(root / 'runtime', out)
        assert 'DISPLAY' not in env and 'WAYLAND_DISPLAY' not in env
        self.log = (root / 'consumer.log').open('xb')
        self.process = subprocess.Popen([str(out / 'alloy_guest_integration_consumer'), mode,
                                         'attached' if attach else 'missing'], cwd=out, env=env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
        self.events = []
        self.buffer = b''
        self.policy = dict(permissions=True, hold_reads=False, deny_reads=False,
                           hold_writes=False, fail_writes=False)
        self.members = None
        try:
            assert self.wait('prepared')['integration']
            assert self.wait('view-ready')['attached'] == attach
            self.wait('configured', lambda value: value['width'] == 640)
            if attach:
                self.configure()
        except BaseException:
            self.dispose()
            raise

    def send(self, **command):
        self.process.stdin.write((json.dumps(command) + '\n').encode())
        self.process.stdin.flush()

    def pump(self, timeout=1):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            return
        data = os.read(self.process.stdout.fileno(), 65536)
        assert data, f'consumer exited {self.process.poll()}: {self.root}'
        self.log.write(data)
        self.log.flush()
        self.buffer += data
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            prefix = b'GUEST_INTEGRATION_EVENT '
            if line.startswith(prefix):
                event = json.loads(line[len(prefix):])
                assert event['event'] not in ('invalid-command', 'stream-unavailable',
                                               'integration-unavailable', 'file-wrong-callback-thread'), event
                self.events.append(event)

    def wait(self, event, predicate=lambda _: True, after=0, timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            for value in self.events[after:]:
                if value['event'] == event and predicate(value):
                    return value
            assert time.monotonic() < deadline, (event, self.root, self.events[-12:])
            self.pump(max(0, min(1, deadline - time.monotonic())))

    def command(self, **command):
        after = len(self.events)
        self.send(**command)
        return self.wait('queued', lambda value: value['op'] == command['op'], after)

    def action(self, outcome='accepted', **command):
        queued = self.command(**command)
        assert queued['ok'] and int(queued['id']), queued
        receipt = self.wait('interaction', lambda value: value['id'] == queued['id'])
        assert receipt['outcome'] == outcome and int(receipt['dispatch_us']) > 0, receipt
        return receipt

    def configure(self, **changes):
        self.policy.update(changes)
        assert self.command(op='configure', **self.policy)['ok']

    def title(self, kind, after=0, predicate=lambda _: True, timeout=30, allow_file_error=False):
        def match(value):
            try:
                state = json.loads(value['title'])
                if not allow_file_error and kind != 'file-error' and state.get('kind') == 'file-error':
                    raise AssertionError((kind, state))
                return state.get('kind') == kind and predicate(state)
            except (ValueError, TypeError):
                return False
        value = self.wait('title', match, after, timeout)
        return json.loads(value['title'])

    def page(self, base, name='ui.html'):
        after = len(self.events)
        assert self.command(op='navigate', url=base + '/' + name)['ok']
        self.wait('load', lambda value: value['status'] == 200, after)
        page = self.title('ready', after)
        self.wait('frame-consumed', lambda value: int(value['document']) > 0, after)
        if not self.members:
            self.members = OwnedProcesses(self.process.pid, self.out)
            assert self.members.members
        return page

    def click(self, rectangle, button=0, source='app'):
        x = round(rectangle['x'] + rectangle['width'] / 2)
        y = round(rectangle['y'] + min(rectangle['height'] / 2, 16))
        for action in ('down', 'up'):
            self.action(op='pointer', x=x, y=y, button=button, clicks=1, action=action,
                        modifiers=(16 if button == 0 else 64) if action == 'down' else 0,
                        source=source)

    def clipboard(self, **formats):
        assert self.command(op='clipboard-set', formats={name: base64.b64encode(value).decode()
                                                        for name, value in formats.items()})['ok']

    def drag_effect(self, operation, after):
        return self.wait('drag-effect-consumed', lambda value: value['operation'] == operation, after)

    def select_all(self):
        after = len(self.events)
        self.action(op='edit', command='select-all')
        self.title('selection', after, predicate=lambda state:
                   state['selection']['start'] == 0 and
                   state['selection']['end'] == state['selection']['length'] > 0)

    def content(self):
        after = len(self.events)
        self.send(op='clipboard-get', data=True)
        content = self.wait('clipboard-content', after=after)
        return {name: base64.b64decode(value) for name, value in content['formats'].items()}

    def finish(self):
        self.command(op='close')
        self.wait('closed')
        self.process.stdin.close()
        assert self.process.wait(timeout=30) == 0
        if self.members:
            self.members.exited()

    def dispose(self):
        try:
            if self.process.poll() is None:
                try:
                    self.send(op='terminate')
                    self.process.stdin.close()
                    self.process.wait(timeout=30)
                except (OSError, subprocess.TimeoutExpired):
                    self.process.kill()
                    self.process.wait(timeout=30)
        finally:
            try:
                if self.members:
                    try:
                        self.members.exited()
                    finally:
                        self.members.close()
            finally:
                try:
                    self.log.close()
                finally:
                    (self.root / 'events.json').write_text(json.dumps(self.events, indent=2) + '\n')


def case(out, evidence, name, check, **options):
    client = Client(out, evidence / name, **options)
    try:
        result = check(client)
        client.finish()
        return dict(case=name, status='passed', **(result or {}))
    finally:
        client.dispose()


def clipboard_ui(client, base):
    page = client.page(base)
    assert page['storage'] == ('memory-state' if client.mode == 'memory' else 'refused'), page
    client.action(op='focus', focused=True)
    client.click(page['rectangles']['edit'])
    client.select_all()
    after = len(client.events)
    client.action(op='edit', command='copy', source='app', capture_us=111)
    client.wait('clipboard-stored', after=after)
    copied = client.content()
    assert copied['text/plain'] == b'native-copy' and copied['text/html'] == b'<b>native-copy</b>', copied
    client.clipboard(**copied)
    after = len(client.events)
    client.action(op='edit', command='paste')
    native = client.title('paste', after)['paste']
    assert native['text'] == 'native-copy' and native['custom'] == 'native-custom', native
    external = 'external-\u4e2d\U0001f642'.encode()
    client.clipboard(**{'text/plain': external, 'text/html': b'<strong>external</strong>'})
    after = len(client.events)
    receipt = client.action(op='edit', command='paste', source='agent', capture_us=222)
    assert receipt['source'] == 'agent' and receipt['capture_us'] == '222', receipt
    pasted = client.title('paste', after)
    assert pasted['paste']['text'].encode() == external and 'external' in pasted['paste']['html'], pasted
    client.select_all()
    after = len(client.events)
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=67, native=54, modifiers=4, character=99)
    client.wait('clipboard-stored', after=after)
    assert client.content()['text/plain'] == external
    client.clipboard(**{'text/plain': b'keyboard-native'})
    after = len(client.events)
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=86, native=55, modifiers=4, character=118)
    assert client.title('paste', after)['paste']['text'] == 'keyboard-native'
    return {'routes': ['edit-copy', 'edit-paste', 'keyboard-copy', 'keyboard-paste'],
            'formats': list(copied)}


def async_clipboard(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.configure(hold_writes=True)
    before = client.content()
    after = len(client.events)
    client.click(page['rectangles']['write'])
    client.title('write-resolved', after)
    offered = client.wait('write-ready', after=after)
    assert client.content() == before, 'native write promise incorrectly waits for or changes the app sink'
    assert client.command(op='accept-write', request=offered['request'])['ok']
    client.wait('clipboard-stored', after=after)
    expected = 'async-native-\u4e2d\U0001f642'.encode()
    assert client.content()['text/plain'] == expected
    client.configure(hold_writes=False, fail_writes=True)
    after = len(client.events)
    client.click(page['rectangles']['write'])
    client.title('write-resolved', after)
    client.wait('sink-failed', after=after)
    client.wait('interaction', lambda value: value['kind'] == 'clipboard-write-outcome' and
                value['outcome'] == 'rejected:denied', after)
    assert client.content()['text/plain'] == expected
    client.configure(fail_writes=False)
    after = len(client.events)
    client.click(page['rectangles']['writeRich'])
    client.title('rich-write-resolved', after)
    client.wait('clipboard-stored', after=after)
    actual = client.content()
    assert actual['text/plain'] == b'rich-native' and b'rich-native' in actual['text/html']
    assert actual['image/png'].startswith(b'\x89PNG\r\n\x1a\n')
    after = len(client.events)
    client.click(page['rectangles']['readRich'])
    read = client.title('rich-read-resolved', after)['rich']
    values = {value['type']: value for value in read}
    assert values['text/plain']['text'] == 'rich-native', values
    assert bytes(values['image/png']['bytes']) == actual['image/png'], values
    assert values['image/png']['width'] == values['image/png']['height'] == 4, values
    assert values['web application/x-alloy-test']['bytes'] == [0, 1, 2, 255], values
    return {'native_write_promise': 'fire-and-forget', 'app_png_consumed': True,
            'opaque_custom_consumed': True, 'sink_failure_observed': True}


def menus(client, base, server):
    server.phase = 0
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.click(page['rectangles']['edit'])
    client.select_all()
    after = len(client.events)
    client.click(page['rectangles']['edit'], button=2)
    menu = client.wait('menu', after=after)
    assert any(item['enabled'] and item['native'] for item in menu['items']), menu
    # The page legitimately moves focus while the app holds the menu. Native
    # action must keep its original frame, not resolve the new focused frame.
    server.phase = 1
    client.title('frame-message', after, predicate=lambda state: state.get('frame', {}).get('focused'))
    assert client.command(op='menu-select', request=menu['request'], command='copy', source='agent')['ok']
    client.wait('clipboard-stored', after=after)
    assert client.content()['text/plain'] == b'native-copy'
    server.phase = 0
    client.click(page['rectangles']['edit'])
    client.select_all()
    after = len(client.events)
    client.click(page['rectangles']['edit'], button=2)
    oversized = client.wait('menu', after=after)
    assert not client.command(op='menu-select', request=oversized['request'], command='copy',
                              source='x' * (4 * 1024 * 1024))['ok'], 'unqueueable reply consumed the native request'
    assert client.command(op='menu-select', request=oversized['request'], command=-1)['ok']
    client.wait('request-ended', lambda value: value['request'] == oversized['request'], after)
    after = len(client.events)
    client.click(page['rectangles']['edit'], button=2)
    held = client.wait('menu', after=after)
    client.command(op='navigate', url=base + '/ui.html?replacement=1')
    client.wait('request-ended', lambda value: value['request'] == held['request'] and value['reason'] == 'retired', after)
    client.title('ready', after)
    assert not client.command(op='menu-select', request=held['request'], command='copy')['ok']
    return {'original_frame_after_focus_change': True, 'held_callback_revoked': True}


def clipboard_accept_before_ready(client, base):
    page = client.page(base); client.action(op='focus', focused=True)
    client.clipboard(**{'text/plain': b'unchanged-before-accumulation'})
    client.configure(hold_writes=True, hold_write_data=True)
    after = len(client.events); client.click(page['rectangles']['writeRich'])
    held = client.wait('write-data-held', after=after)
    client.command(op='accept-write', request=held['request'])
    before_ready = client.content()
    assert before_ready.get('text/plain') == b'unchanged-before-accumulation', before_ready
    assert not any(e['event'] == 'clipboard-stored' for e in client.events[after:])
    client.command(op='release-write-data', request=held['request'])
    client.wait('clipboard-stored', after=after)
    content = client.content()
    assert content['text/plain'] == b'rich-native' and b'<em>rich-native</em>' in content['text/html'], content
    assert content['image/png'].startswith(b'\x89PNG\r\n\x1a\n'), content
    return dict(early_accept_waited_for_data=True, atomic_complete_formats=True)


def clipboard_early_accept_retirement(client, base):
    page = client.page(base); client.action(op='focus', focused=True)
    client.clipboard(**{'text/plain': b'not-published-after-retirement'})
    client.configure(hold_writes=True, hold_write_data=True)
    after = len(client.events); client.click(page['rectangles']['writeRich'])
    held = client.wait('write-data-held', after=after)
    client.command(op='accept-write', request=held['request'])
    client.command(op='stop-provider')
    client.wait('request-ended', lambda value: value['request'] == held['request'], after)
    assert client.content().get('text/plain') == b'not-published-after-retirement'
    assert not client.command(op='release-write-data', request=held['request'])['ok']
    return dict(early_accept_cancelled_before_publication=True, old_clipboard_preserved=True)


def quick_menu(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.clipboard(**{'text/plain': b'quick-menu-paste'})
    rect = page['rectangles']['edit']
    x, y = rect['x'] + rect['width'] - 20, rect['y'] + rect['height'] - 15
    after = len(client.events)
    client.action(op='touch', type='press', contact=1, x=x, y=y)
    # Native long-press recognition, not a JavaScript selection/menu substitute.
    time.sleep(0.8)
    client.action(op='touch', type='release', contact=1, x=x, y=y)
    offered = client.wait('menu', lambda value: value['quick'], after=after)
    paste = next(item for item in offered['items'] if item['label'] == 'Paste')
    assert paste['enabled'] and paste['native'], offered
    assert client.command(op='menu-select', request=offered['request'], command='paste')['ok']
    assert client.title('paste', after)['paste']['text'] == 'quick-menu-paste'
    return {'app_formats_enable_native_paste': True, 'page_consumed_paste': True}


def inline_selection(client, base, cross=False):
    page = client.page(base, 'editor.html' + ('?cross=1' if cross else ''))
    client.action(op='focus', focused=True)
    after = len(client.events); client.click(page['rectangles']['select'])
    selected = client.title('editor-selected', after)
    client.click(selected['selection'], button=2)
    menu = client.wait('menu', after=after)
    assert client.command(op='menu-select', request=menu['request'], command='cut')['ok']
    client.wait('clipboard-stored', after=after)
    actual = client.title('editor-input', after)
    assert actual['text'] == ('abf' if cross else 'abcdf'), actual
    assert actual['focused'] == selected['focused'] == 'editor', (selected, actual)
    assert client.content()['text/plain'] == (b'cde' if cross else b'e')
    return dict(inline_nodes=True, cross_node=cross, native_cut=True, original_range=True)


def denied_permission(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.clipboard(**{'text/plain': b'permission-protected-data'})
    client.configure(permissions=False)
    after = len(client.events)
    client.click(page['rectangles']['read'])
    denied = client.title('read-denied', after)
    assert denied['error'] == 'NotAllowedError', denied
    requests = client.events[after:]
    assert any(value['event'] == 'clipboard-request' and value['permission'] for value in requests), requests
    assert not any(value['event'] == 'clipboard-request' and value['format'] == 'text/plain'
                   for value in requests), requests
    assert client.content()['text/plain'] == b'permission-protected-data'
    return {'native_app_permission_denial_observed': True, 'protected_data_not_read': True}


def popup_drag_retirement(client, base):
    page = client.page(base); client.action(op='focus', focused=True)
    drop = page['rectangles']['drop']; x, y = round(drop['x'] + 20), round(drop['y'] + 20)
    after = len(client.events)
    client.action(op='drag-enter', x=x, y=y, allowed=1, modifiers=0, text='old-popup-drag', html='')
    client.action(op='drag-over', x=x, y=y, allowed=1, modifiers=0); client.drag_effect(1, after)
    after = len(client.events); client.click(page['rectangles']['popup'])
    opened = client.wait('popup', lambda value: value['visible'] and value['width'] > 0 and value['height'] > 0,
                         after=after)
    after = len(client.events)
    client.action(op='key', type='rawdown', key=27, native=9, modifiers=0, character=0)
    # Size changes also emit configured. Only the typed visibility callback
    # proves closure before releasing the key and starting a replacement drag.
    closed = client.wait('popup', lambda value: not value['visible'] and
                         int(value['generation']) > int(opened['generation']), after=after)
    client.action(op='key', type='up', key=27, native=9, modifiers=0, character=0)
    stale = client.action(outcome='rejected:invalid', op='drag-drop', x=x, y=y, modifiers=0)
    assert stale['outcome'] != 'accepted', stale
    after = len(client.events)
    entered = client.action(op='drag-enter', x=x, y=y, allowed=1, modifiers=0, text='new-popup-drag', html='')
    assert entered['outcome'] == 'accepted', entered
    client.action(op='drag-over', x=x, y=y, allowed=1, modifiers=0); client.drag_effect(1, after)
    client.action(op='drag-drop', x=x, y=y, modifiers=0)
    assert client.title('drop', after)['dropText'] == 'new-popup-drag'
    return dict(popup_geometry_retires_drag=True, stale_drop_refused=True, fresh_drag_delivered=True,
                popup_open_generation=opened['generation'], popup_closed_generation=closed['generation'])


def drags(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    rectangle = page['rectangles']['drop']
    x, y = round(rectangle['x'] + 20), round(rectangle['y'] + 20)
    after = len(client.events)
    client.action(op='drag-enter', x=x, y=y, allowed=1, modifiers=0, text='app-drop', html='<b>app-drop</b>')
    client.action(op='drag-over', x=x+2, y=y+2, allowed=1, modifiers=0)
    client.drag_effect(1, after)
    client.action(op='drag-drop', x=x+2, y=y+2, modifiers=0, source='agent')
    dropped = client.title('drop', after)
    assert dropped['dropText'] == 'app-drop' and 'app-drop' in dropped['dropHtml'], dropped
    assert 'text/uri-list' not in dropped['dropTypes'], dropped
    start = page['rectangles']['drag']
    sx, sy = round(start['x'] + 20), round(start['y'] + 20)
    after = len(client.events)
    client.action(op='pointer', action='down', x=sx, y=sy, button=0, clicks=1, modifiers=16)
    for delta in (5, 15, 30):
        client.action(op='pointer', action='move', x=sx+delta, y=sy+delta, button=0, clicks=0, modifiers=16)
    source = client.wait('drag-source', after=after)
    assert source['text'] == 'drag-native', source
    assert client.command(op='drag-finish', request=source['request'], x=sx+30, y=sy+30, operation=1)['ok']
    client.action(op='pointer', action='up', x=sx+30, y=sy+30, button=0, clicks=1, modifiers=0)
    client.title('drag-end', after, predicate=lambda state: state['dragEnds'] == 1)
    return {'native_target_data_consumed': True, 'source_end_consumed': True}


def fullscreen(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    after = len(client.events)
    client.click(page['rectangles']['full'])
    denied = client.wait('fullscreen', after=after)
    assert client.command(op='fullscreen-decide', request=denied['request'], allow=False)['ok']
    assert not client.title('fullscreen-denied', after)['full']
    after = len(client.events)
    client.click(page['rectangles']['full'])
    accepted = client.wait('fullscreen', after=after)
    assert client.command(op='fullscreen-decide', request=accepted['request'], allow=True)['ok']
    assert client.title('fullscreen-resolved', after)['full']
    client.wait('frame-consumed', lambda value: value['width'] == 960 and value['height'] == 540, after)
    after = len(client.events)
    client.action(op='exit-fullscreen', source='agent')
    assert not client.title('fullscreen-change', after)['full']
    client.wait('frame-consumed', lambda value: value['width'] == 640 and value['height'] == 480, after)
    return {'native_entry_denied_before_transition': True, 'app_viewport_consumed': [960, 540],
            'native_exit_observed': True}


def pending_read(client, base, operation):
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.click(page['rectangles']['edit'])
    client.clipboard(**{'text/plain': b'never-delivered'})
    client.configure(hold_reads=True)
    after = len(client.events)
    client.action(op='edit', command='paste')
    held = client.wait('read-held', lambda value: value['format'] == 'text/plain', after)
    started = time.monotonic()
    if operation == 'deadline':
        page = client.title('paste', after, timeout=16)
        elapsed = time.monotonic() - started
        assert page['paste']['text'] == '' and 8.5 <= elapsed <= 13.5, (page, elapsed)
    elif operation == 'provider':
        client.command(op='stop-provider')
        client.wait('integration-closed', after=after, timeout=5)
        assert client.title('paste', after, timeout=5)['paste']['text'] == ''
        elapsed = time.monotonic() - started
    elif operation == 'close':
        client.command(op='close')
        client.wait('closed', after=after, timeout=5)
        elapsed = time.monotonic() - started
    else:
        client.command(op='navigate', url=base + '/ui.html?replacement=1')
        replacement = client.title('ready', after, timeout=5)
        assert replacement['pastes'] == 0, replacement
        elapsed = time.monotonic() - started
    client.wait('request-ended', lambda value: value['request'] == held['request'] and value['reason'] == 'retired', after)
    assert not client.command(op='reply-read', request=held['request'], text='late-clipboard')['ok']
    return {'retirement': operation, 'seconds': elapsed, 'late_real_sdk_reply_refused': True}


def child_pending_read(client, base, server):
    server.phase = 0
    client.page(base)
    client.action(op='focus', focused=True)
    after = len(client.events)
    server.phase = 1
    client.title('frame-message', after, predicate=lambda state: state.get('frame', {}).get('focused'))
    client.clipboard(**{'text/plain': b'original-child-only'})
    client.configure(hold_reads=True)
    after = len(client.events)
    client.action(op='edit', command='paste')
    held = client.wait('read-held', lambda value: value['format'] == 'text/plain', after)
    assert 'localhost' in held['origin'], held
    started = time.monotonic()
    # A different renderer initiates the subframe navigation while the original
    # renderer is synchronously pasting. It cannot rely on the blocked child
    # processing a postMessage to initiate its own replacement.
    server.phase = 2
    replacement = client.title('frame-message', after, timeout=5,
        predicate=lambda state: state.get('frame', {}).get('replacement'))
    assert replacement['frame']['pastes'] == 0, replacement
    client.wait('request-ended', lambda value: value['request'] == held['request'] and
                value['reason'] == 'retired', after, timeout=5)
    assert not client.command(op='reply-read', request=held['request'], text='late-child-data')['ok']
    return {'original_child_retired': True, 'replacement_pastes': 0,
            'seconds': time.monotonic() - started}


def unavailable(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    client.click(page['rectangles']['edit'])
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=65, native=38, modifiers=4, character=97)
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=67, native=54, modifiers=4, character=99)
    after = len(client.events)
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=86, native=55, modifiers=4, character=118)
    assert client.title('paste', after)['paste']['text'] == '', 'guest used a native/private fallback clipboard'
    assert client.content() == {}
    return {'missing_provider_native_clipboard_fallback': False}


def file_handles(client, base):
    page = client.page(base, 'files.html')
    client.action(op='focus', focused=True)
    after = len(client.events)
    client.click(page['rectangles']['open'])
    opened = client.title('file-read', after)
    assert opened['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest(), opened
    assert client.command(op='files-configure', **{'hold-close': True})['ok']
    after = len(client.events)
    client.click(page['rectangles']['write'])
    closing = client.wait('file-operation', lambda value: value['kind'] == 'close', after)
    client.command(op='files-get')
    stored = client.wait('files', after=after)
    assert bytes.fromhex(stored['source']) == b'app-selected-bytes', stored
    assert not any(event['event'] == 'file-committed' for event in client.events[after:]), closing
    assert client.command(op='files-release')['ok']
    finished = client.title('save-complete', after)
    assert finished['sha256'] == hashlib.sha256(b'native-save-bytes').hexdigest(), finished
    committed = client.wait('file-committed', after=after)
    assert bytes.fromhex(committed['hex']) == b'native-save-bytes', committed
    client.command(op='files-configure')
    after = len(client.events)
    client.click(page['rectangles']['save'])
    saved = client.title('save-complete', after)
    assert saved['name'] == 'saved.txt' and saved['sha256'] == hashlib.sha256(b'native-save-bytes').hexdigest(), saved
    after = len(client.events)
    client.click(page['rectangles']['directory'])
    directory = client.title('directory-complete', after)
    assert directory['names'] == ['nested', 'selected.txt'] and directory['path'] == ['nested', 'child.txt'], directory
    assert directory['child']['sha256'] == hashlib.sha256(b'nested-app-bytes').hexdigest(), directory
    assert directory['created']['sha256'] == hashlib.sha256(b'directory-created-bytes').hexdigest(), directory
    assert directory['missing'] == 'NotFoundError', directory
    return dict(read=True, native_writer_app_ack=True, save=True, directory=True)


def file_sink_failure(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    after = len(client.events); client.click(page['rectangles']['open']); client.title('file-read', after)
    client.command(op='files-configure', **{'fail-close': True})
    after = len(client.events); client.click(page['rectangles']['write'])
    failed = client.title('file-error', after)
    assert failed['action'] == 'write', failed
    client.command(op='files-get'); stored = client.wait('files', after=after)
    assert bytes.fromhex(stored['source']) == b'app-selected-bytes', stored
    assert not any(event['event'] == 'file-committed' for event in client.events[after:]), stored
    return dict(app_sink_failure=True, previous_bytes_preserved=True)


def file_uploads(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    for name in ('upload', 'uploadDirectory'):
        after = len(client.events); client.click(page['rectangles'][name])
        result = client.title('upload-complete', after)
        expected = {'selected.txt': b'app-selected-bytes'}
        if name == 'uploadDirectory':
            expected['child.txt'] = b'nested-app-bytes'
        receiver_expected = expected
        if name == 'uploadDirectory':
            receiver_expected = {'app-folder/selected.txt': expected['selected.txt'],
                                 'app-folder/nested/child.txt': expected['child.txt']}
        for location, contents in ((result['files'], expected), (result['uploaded']['files'], receiver_expected)):
            assert len(location) == len(contents), result
            assert {item['name']: item['sha256'] for item in location} == {
                key: hashlib.sha256(value).hexdigest() for key, value in contents.items()}, result
        if name == 'uploadDirectory':
            assert sorted(item['relative'] for item in result['files']) == [
                'app-folder/nested/child.txt', 'app-folder/selected.txt'], result
    return dict(native_input_and_http_receiver=True, directory_upload=True)


def file_clipboard_drop(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.clipboard(**{'text/uri-list': b''})
    after = len(client.events); client.click(page['rectangles']['filePaste'])
    for kind in ('rawdown', 'up'):
        client.action(op='key', type=kind, key=86, native=55, modifiers=4, character=118)
    pasted = client.title('file-paste', after)
    assert len(pasted['files']) == 1 and pasted['files'][0]['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest(), pasted
    rect = page['rectangles']['fileDrop']; x = round(rect['x'] + rect['width'] / 2); y = round(rect['y'] + 8)
    after = len(client.events)
    client.action(op='drag-file-enter', x=x, y=y, allowed=1, modifiers=0)
    client.action(op='drag-over', x=x, y=y, allowed=1, modifiers=0)
    client.drag_effect(1, after)
    client.action(op='drag-drop', x=x, y=y, modifiers=0)
    dropped = client.title('file-drop', after)
    assert len(dropped['files']) == 1 and dropped['files'][0]['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest(), dropped
    return dict(native_file_paste=True, native_file_drop=True)


def file_upload_names(client, base):
    name = 'app-' + 'x' * 400 + '.txt'
    expected = sorted(hashlib.sha256(value).hexdigest() for value in (b'app-selected-bytes', b'nested-app-bytes'))
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'logical-name': name, 'same-name': True, 'upload-pair': True})
    after = len(client.events); client.click(page['rectangles']['uploadMultiple'])
    result = client.title('upload-complete', after)
    for location in (result['files'], result['uploaded']['files']):
        assert len(location) == 2 and all(item['name'] == name for item in location), result
        assert sorted(item['sha256'] for item in location) == expected, result
    return dict(same_basename_distinct_entries=True, logical_name_length=len(name), receiver_bytes=True)


def file_large_stream(client, base):
    expected = bytes((index * 37 + 11) % 256 for index in range(2 * 1024 * 1024 + 13))
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'read-hex': expected.hex(), 'hold-read': True})
    after = len(client.events); client.click(page['rectangles']['open'])
    held = client.wait('file-operation', lambda value: value['kind'] == 'read', after)
    client.action(op='focus', focused=True)
    assert len({value['request'] for value in client.events[after:]
                if value['event'] == 'file-operation' and value['kind'] == 'read'}) == 1, held
    client.command(op='files-configure'); client.command(op='files-release')
    read = client.title('file-read', after)
    assert read['size'] == len(expected) and read['sha256'] == hashlib.sha256(expected).hexdigest(), read
    client.command(op='files-configure', **{'hold-write': True})
    after = len(client.events); client.click(page['rectangles']['largeWrite'])
    held = client.wait('file-operation', lambda value: value['kind'] == 'write', after)
    client.action(op='focus', focused=True)
    assert len({value['request'] for value in client.events[after:]
                if value['event'] == 'file-operation' and value['kind'] == 'write'}) == 1, held
    client.command(op='files-configure'); client.command(op='files-release')
    saved = client.title('save-complete', after)
    committed = client.wait('file-committed', after=after)
    assert bytes.fromhex(committed['hex']) == expected and saved['sha256'] == hashlib.sha256(expected).hexdigest(), saved
    return dict(bytes=len(expected), bounded_reads=True, acknowledged_writes=True, final_page_and_app=True)


def file_shared_retirement(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    child = client.title('file-child', predicate=lambda value: value['child']['kind'] == 'child-ready')['child']
    rect = dict(child['pick']); rect['x'] += page['rectangles']['peer']['x']; rect['y'] += page['rectangles']['peer']['y']
    after = len(client.events); client.click(rect)
    acquired = client.title('file-child', after, predicate=lambda value: value['child']['kind'] == 'child-picked')
    assert acquired['child']['serializedRead'] == 'app-selected-bytes', acquired
    after = len(client.events); client.click(page['rectangles']['read'])
    assert client.title('file-read', after)['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest()
    client.command(op='files-configure', **{'hold-read': True})
    after = len(client.events); client.click(page['rectangles']['read'])
    pending = client.wait('file-operation', lambda value: value['kind'] == 'read', after)
    client.click(page['rectangles']['change'])
    client.wait('request-ended', lambda value: value['request'] == pending['request'] and value['reason'] == 'retired', after)
    failed = client.title('file-error', after)
    assert failed['action'] == 'read', failed
    restored = client.title('file-child', after, predicate=lambda value: value['child']['kind'] == 'serialized-refused', allow_file_error=True)
    assert restored['child']['error'] in ('NotAllowedError', 'AbortError', 'InvalidStateError', 'DataCloneError', 'UnknownError'), restored
    client.command(op='files-configure'); client.command(op='files-release')
    after = len(client.events); client.click(page['rectangles']['read'])
    failed = client.title('file-error', after)
    assert failed['error'] in ('NotAllowedError', 'AbortError', 'InvalidStateError'), failed
    assert not any(value['event'] == 'file-operation' and value['kind'] == 'read' for value in client.events[after:])
    return dict(transferred_handle=True, live_serialized_read=True, retired_serialization_refused=True,
                issuer_child_navigation=True, pending_read_retired=True, late_read_refused=True)


def file_data_handle_retirement(client, base, route):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    child = client.title('file-child', predicate=lambda value: value['child']['kind'] == 'child-ready')['child']
    rect = dict(child[route]); rect['x'] += page['rectangles']['peer']['x']; rect['y'] += page['rectangles']['peer']['y']
    after = len(client.events)
    if route == 'paste':
        client.clipboard(**{'text/uri-list': b''}); client.click(rect)
        for kind in ('rawdown', 'up'):
            client.action(op='key', type=kind, key=86, native=55, modifiers=4, character=118)
    else:
        x = round(rect['x'] + rect['width'] / 2); y = round(rect['y'] + rect['height'] / 2)
        client.action(op='drag-file-enter', x=x, y=y, allowed=1, modifiers=0)
        client.action(op='drag-over', x=x, y=y, allowed=1, modifiers=0)
        client.drag_effect(1, after)
        client.action(op='drag-drop', x=x, y=y, modifiers=0)
    acquired = client.title('file-child', after, predicate=lambda value: value['child']['kind'] == 'child-picked')
    assert acquired['child']['route'] == route and acquired['child']['serializedRead'] == 'app-selected-bytes', acquired
    after = len(client.events); client.click(page['rectangles']['read'])
    assert client.title('file-read', after)['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest()
    client.command(op='files-configure', **{'hold-read': True})
    after = len(client.events); client.click(page['rectangles']['read'])
    pending = client.wait('file-operation', lambda value: value['kind'] == 'read', after)
    client.click(page['rectangles']['change'])
    client.wait('request-ended', lambda value: value['request'] == pending['request'] and value['reason'] == 'retired', after)
    assert client.title('file-error', after)['action'] == 'read'
    client.title('file-child', after, predicate=lambda value: value['child']['kind'] == 'serialized-refused', allow_file_error=True)
    client.command(op='files-configure'); client.command(op='files-release')
    after = len(client.events); client.click(page['rectangles']['read'])
    assert client.title('file-error', after)['action'] == 'read'
    assert not any(value['event'] == 'file-operation' and value['kind'] == 'read' for value in client.events[after:])
    return dict(route=route, native_handle=True, serialized_live_read=True, pending_read_retired=True,
                serialized_refused=True, replacement_cannot_reacquire=True)


def file_locks(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    after = len(client.events); client.click(page['rectangles']['open'])
    client.title('file-read', after)
    after = len(client.events); client.click(page['rectangles']['locks'])
    result = client.title('locks-complete', after)
    assert result['child'] == result['parent'] == 'NoModificationAllowedError' and result['siblingRemoved'], result
    return dict(same_entry_native_lock=True, ancestor_native_lock=True, unrelated_sibling_not_blocked=True)


def file_picker_denied(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'deny-selection': True})
    after = len(client.events); client.click(page['rectangles']['open'])
    result = client.title('file-error', after)
    assert result['action'] == 'open' and result['error'] in ('AbortError', 'NotAllowedError'), result
    assert not any(value['event'] == 'file-operation' for value in client.events[after:])
    return dict(cancelled=True, no_native_fallback=True)


def file_metadata_sequence(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'async-metadata': True})
    after = len(client.events); client.click(page['rectangles']['open'])
    assert client.title('file-read', after)['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest()
    after = len(client.events); client.click(page['rectangles']['write'])
    assert client.title('save-complete', after)['sha256'] == hashlib.sha256(b'native-save-bytes').hexdigest()
    return dict(worker_thread_completions=True, client_file_getters_serialized=True)


def file_provider_retirement(client, base, ending, operation):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    after = len(client.events); client.click(page['rectangles']['open']); client.title('file-read', after)
    client.command(op='files-configure', **{'hold-' + operation: True})
    after = len(client.events); client.click(page['rectangles']['read' if operation == 'read' else 'write'])
    pending = client.wait('file-operation', lambda value: value['kind'] == operation, after)
    assert client.command(op=ending)['ok']
    client.wait('request-ended', lambda value: value['request'] == pending['request'] and value['reason'] == 'retired', after)
    client.wait('integration-closed', after=after)
    client.command(op='files-configure'); client.command(op='files-release'); client.command(op='files-get')
    stored = client.wait('files', after=after)
    assert bytes.fromhex(stored['source']) == b'app-selected-bytes', stored
    assert not any(value['event'] == 'file-committed' for value in client.events[after:])
    if ending == 'stop-provider':
        client.title('file-error', after)
    else:
        client.wait('closed', after=after)
    return dict(ending=ending, pending=operation, retired=True, committed_bytes_preserved=True)


def file_download(client, base, server, failure=False):
    expected = bytes((index * 37 + 11) % 256 for index in range(2 * 1024 * 1024 + 13))
    server.download_bytes = expected
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'hold-close': not failure, 'fail-close': failure})
    after = len(client.events); client.click(page['rectangles']['download'])
    client.wait('file-operation', lambda value: value['kind'] == 'close', after)
    if not failure:
        client.command(op='files-get'); stored = client.wait('files', after=after)
        assert stored['downloaded'] == '', stored
        assert not any(value['event'] == 'download-state' and value['state'] == 'complete' for value in client.events[after:])
        client.command(op='files-configure'); client.command(op='files-release')
        committed = client.wait('file-committed', after=after)
        assert bytes.fromhex(committed['hex']) == expected
    finished = client.wait('download-state', lambda value: value['state'] in ('complete', 'interrupted', 'cancelled'), after)
    if failure:
        assert finished['state'] != 'complete', finished
        client.command(op='files-get'); assert client.wait('files', after=after)['downloaded'] == ''
        assert not any(value['event'] == 'file-committed' for value in client.events[after:])
    else:
        assert finished['state'] == 'complete' and int(finished['received']) == len(expected), finished
    return dict(bytes=len(expected), native_state=finished['state'], actual_app_sink=not failure)


def file_download_selection_retry(client, base, server):
    server.download_bytes = b'native-destination-retry-bytes'
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'deny-first-download': True})
    after = len(client.events); client.click(page['rectangles']['download'])
    first = client.wait('file-selection', lambda value: value['mode'] == 'download', after)
    client.wait('download-state', lambda value: value['state'] == 'interrupted', after)
    resumed = len(client.events)
    receipt = client.action(op='download-control', download=first['download'], command='resume')
    assert receipt['outcome'] == 'accepted', receipt
    second = client.wait('file-selection', lambda value: value['mode'] == 'download', resumed)
    assert second['download'] == first['download'] and second['request'] != first['request'], (first, second)
    assert bytes.fromhex(client.wait('file-committed', after=resumed)['hex']) == server.download_bytes
    client.wait('download-state', lambda value: value['state'] == 'complete', resumed)
    return dict(native_restart=True, retained_download_metadata=True, final_app_bytes=True)


def file_download_retirement(client, base, server):
    server.download_bytes = b'pending-app-download' * 65536
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'hold-write': True})
    after = len(client.events); client.click(page['rectangles']['download'])
    write = client.wait('file-operation', lambda value: value['kind'] == 'write', after)
    client.command(op='navigate', url=base + '/files.html?replacement=1')
    client.wait('request-ended', lambda value: value['request'] == write['request'] and value['reason'] == 'retired', after)
    finished = client.wait('download-state', lambda value: value['state'] in ('interrupted', 'cancelled'), after)
    client.command(op='files-configure'); client.command(op='files-release'); client.command(op='files-get')
    assert client.wait('files', after=after)['downloaded'] == ''
    assert not any(value['event'] == 'file-committed' for value in client.events[after:])
    return dict(original_navigation_revoked=True, native_state=finished['state'], no_late_commit=True)


def file_download_controls(client, base, server):
    server.download_bytes = b'native-pause-resume' * 65536
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'hold-write': True})
    after = len(client.events); client.click(page['rectangles']['download'])
    selected = client.wait('file-selection', lambda value: value['mode'] == 'download', after)
    write = client.wait('file-operation', lambda value: value['kind'] == 'write', after)
    identity = selected['download']
    client.wait('download-state', lambda value: value['download'] == identity and value['state'] == 'active', after)
    client.action(op='download-control', download=identity, command='pause')
    client.wait('download-state', lambda value: value['download'] == identity and value['state'] == 'paused', after)
    client.command(op='files-release')
    client.wait('request-ended', lambda value: value['request'] == write['request'], after)
    client.command(op='files-get')
    staged = client.wait('files', after=after)
    assert int(staged['download-staged-bytes']) == int(write['length']) and staged['downloaded'] == '', staged
    client.action(op='focus', focused=True)
    assert len({value['request'] for value in client.events[after:]
                if value['event'] == 'file-operation' and value['kind'] == 'write'}) == 1
    client.command(op='files-configure')
    client.action(op='download-control', download=identity, command='resume')
    committed = client.wait('file-committed', after=after)
    assert bytes.fromhex(committed['hex']) == server.download_bytes
    client.wait('download-state', lambda value: value['download'] == identity and value['state'] == 'complete', after)
    return dict(native_pause=True, paused_after_ack=True, native_resume=True, final_bytes=True)


def file_download_cancel_picker(client, base):
    page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
    client.command(op='files-configure', **{'hold-selection': True})
    after = len(client.events); client.click(page['rectangles']['download'])
    selected = client.wait('file-selection', lambda value: value['mode'] == 'download', after)
    client.action(op='download-control', download=selected['download'], command='cancel')
    client.wait('request-ended', lambda value: value['request'] == selected['request'] and value['reason'] == 'retired', after)
    client.wait('download-state', lambda value: value['download'] == selected['download'] and value['state'] == 'cancelled', after)
    assert not any(value['event'] == 'file-operation' for value in client.events[after:])
    return dict(native_cancel=True, pending_picker_retired=True, no_sink=True)


def file_download_resume(client, base, server):
    expected = bytes((index * 37 + 11) % 256 for index in range(2 * 1024 * 1024 + 13))
    server.download_bytes = expected; server.download_interrupted = False; server.download_ranges = []
    server.download_resume_release = threading.Event()
    server.download_resume_started = threading.Event()
    try:
        page = client.page(base, 'files.html?downloadCase=resume'); client.action(op='focus', focused=True)
        after = len(client.events); client.click(page['rectangles']['download'])
        client.wait('file-selection', lambda value: value['mode'] == 'download', after)
        assert server.download_resume_started.wait(30), server.download_ranges
        assert server.download_interrupted and len(server.download_ranges) == 2, server.download_ranges
        assert server.download_ranges[0] is None and not server.download_ranges[1].startswith('bytes=0-'), server.download_ranges
        client.command(op='files-get'); staged = client.wait('files', after=after)
        assert staged['downloaded'] == '' and 0 < int(staged['download-staged-bytes']) < len(expected), staged
        # Native network-error policy auto-continues this validated download.
        # Hold its range response while observing that staging is not committed.
        server.download_resume_release.set()
        committed = client.wait('file-committed', after=after)
        assert bytes.fromhex(committed['hex']) == expected
        client.wait('download-state', lambda value: value['state'] == 'complete', after)
        assert len(server.download_ranges) == 2, server.download_ranges
        assert len([value for value in client.events[after:] if value['event'] == 'file-selection' and value['mode'] == 'download']) == 1
        assert any(value['event'] == 'file-operation' and value['kind'] == 'read' and value['name'] == 'download.bin'
                   for value in client.events[after:]), 'Missing app-stage prefix validation'
        return dict(native_interruption=True, staged_not_committed=True, range_resume=True,
                    prefix_validated=True, final_bytes=True)
    finally:
        server.download_resume_release.set()
        (client.root / 'http-resume.json').write_text(json.dumps(dict(
            interrupted=server.download_interrupted, ranges=server.download_ranges), indent=2) + '\n')


def file_checks(base, server):
    return [('native-file-handles-and-sinks', lambda client: file_handles(client, base)),
            ('native-file-failed-sink', lambda client: file_sink_failure(client, base)),
            ('native-file-input-http-upload', lambda client: file_uploads(client, base)),
            ('native-file-upload-logical-names', lambda client: file_upload_names(client, base)),
            ('native-file-clipboard-drop', lambda client: file_clipboard_drop(client, base)),
            ('native-file-large-pressure', lambda client: file_large_stream(client, base)),
            ('native-file-native-locks', lambda client: file_locks(client, base)),
            ('native-file-shared-retirement', lambda client: file_shared_retirement(client, base)),
            ('native-file-drop-handle-retirement', lambda client: file_data_handle_retirement(client, base, 'drop')),
            ('native-file-paste-handle-retirement', lambda client: file_data_handle_retirement(client, base, 'paste')),
            ('native-file-picker-denied', lambda client: file_picker_denied(client, base)),
            ('native-file-metadata-sequence', lambda client: file_metadata_sequence(client, base)),
            ('native-file-close-pending-read', lambda client: file_provider_retirement(client, base, 'close', 'read')),
            ('native-file-stop-pending-commit', lambda client: file_provider_retirement(client, base, 'stop-provider', 'close')),
            ('native-file-download-sink', lambda client: file_download(client, base, server)),
            ('native-file-download-failed-sink', lambda client: file_download(client, base, server, True)),
            ('native-file-download-retirement', lambda client: file_download_retirement(client, base, server)),
            ('native-file-download-pause-resume', lambda client: file_download_controls(client, base, server)),
            ('native-file-download-cancel-picker', lambda client: file_download_cancel_picker(client, base)),
            ('native-file-download-range-resume', lambda client: file_download_resume(client, base, server))]


def native_grants(out, evidence):
    env = environment(evidence / 'runtime', out)
    command = [str(out / 'alloy_native_grants_unittests'), '--ozone-platform=headless',
               '--test-launcher-retry-limit=0', '--alloy-probe-tools-dir=' + str(REPO / 'tools'),
               '--alloy-test-log-dir=' + str(evidence / 'guard'),
               '--test-launcher-summary-output=' + str(evidence / 'native-grants-summary.json')]
    with (evidence / 'native-grants.log').open('xb') as log:
        subprocess.run(command, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=900)
    print('GUEST_NATIVE_GRANTS_PASS', flush=True)
    return [{'case': 'native-grants-unit', 'status': 'passed'}]


def file_input_drop_after_new_drag(client, base):
    page = client.page(base, 'files.html?retainDrop=1'); client.action(op='focus', focused=True)
    target = page['rectangles']['upload']; x, y = round(target['x'] + 20), round(target['y'] + 18)
    after = len(client.events)
    client.action(op='drag-file-enter', x=x, y=y, allowed=1, modifiers=0)
    client.action(op='drag-over', x=x, y=y, allowed=1, modifiers=0); client.drag_effect(1, after)
    client.action(op='drag-drop', x=x, y=y, modifiers=0)
    assert client.title('upload-retained', after)['count'] == 1
    target = page['rectangles']['fileDrop']; x, y = round(target['x'] + 20), round(target['y'] + 18)
    client.action(op='drag-enter', x=x, y=y, allowed=1, modifiers=0, text='later-native-drag', html='')
    client.action(op='drag-leave')
    after = len(client.events); client.click(page['rectangles']['read'])
    read = client.title('file-read', after)
    assert read['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest(), read
    return dict(native_file_input_drop=True, later_drag_did_not_revoke_file=True, exact_retained_bytes=True)


def renderer_file_source_handoff(client, base):
    page = client.page(base, 'files.html?retainDrop=1'); client.action(op='focus', focused=True)
    after = len(client.events); client.click(page['rectangles']['upload'])
    assert client.title('upload-retained', after)['count'] == 1
    source = page['rectangles']['fileDrop']
    sx, sy = round(source['x'] + source['width'] / 2), round(source['y'] + source['height'] / 2)
    after = len(client.events)
    client.action(op='pointer', action='down', x=sx, y=sy, button=0, clicks=1, modifiers=16)
    for delta in (5, 15, 30):
        client.action(op='pointer', action='move', x=sx+delta, y=sy+delta, button=0, clicks=0, modifiers=16)
    request = client.wait('drag-source', after=after)
    client.title('file-source-selection-cleared', after)
    assert request['files'] == 1 and request['file_name'] == 'selected.txt', request
    assert request['text'] == 'native-file-source', request
    assert client.command(op='drag-finish', request=request['request'], x=sx+30, y=sy+30, operation=1)['ok']
    client.action(op='pointer', action='up', x=sx+30, y=sy+30, button=0, clicks=1, modifiers=0)
    client.title('file-source-ended', after)
    return dict(real_renderer_file_drag=True, input_selection_cleared=True, typed_file_preserved=True, native_source_completed=True)


def file_outcomes(out, evidence, base, server, results):
    for name, check in file_checks(base, server):
        results.append(case(out, evidence, name, check))
    results.append(case(out, evidence, 'native-file-stateless-external',
                        lambda client: file_handles(client, base), mode='stateless'))
    results.append(case(out, evidence, 'native-file-input-drop-retained-across-drag',
                        lambda client: file_input_drop_after_new_drag(client, base)))
    results.append(case(out, evidence, 'native-renderer-file-source-handoff',
                        lambda client: renderer_file_source_handoff(client, base)))
    def removed_child(client):
        page = client.page(base, 'files.html'); client.action(op='focus', focused=True)
        after = len(client.events); client.click(page['rectangles']['recursive'])
        result = client.title('recursive-complete', after)
        assert result['failure'] == 'NotFoundError', result
        assert result['remaining']['sha256'] == hashlib.sha256(b'app-selected-bytes').hexdigest(), result
        return dict(retained_descendant_refused=True, sibling_preserved=True)
    results.append(case(out, evidence, 'native-file-recursive-retained-handle', removed_child))
    results.append(case(out, evidence, 'native-download-selection-retry',
                        lambda client: file_download_selection_retry(client, base, server)))


def document_after_abort(client, base):
    client.page(base)
    old_frame = client.wait('frame-consumed', lambda v: int(v['document']) > 0)
    after = len(client.events)
    queued = client.command(op='navigate', url=base + '/no-content')
    assert queued['ok'] and int(queued['id']), queued
    load = client.wait('load', lambda v: v['request'] == queued['id'], after)
    assert load['status'] == -3 and int(load['document']) > int(old_frame['document']), load
    client.wait('frame-consumed', lambda v: v['document'] == load['document'], after)
    # Deliberately omit document: use the reference consumer's admitted owner.
    action = client.action(op='focus', focused=True)
    assert action['document'] == load['document'], (load, action)
    return dict(load=load, action=action)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('native-grants-unit', 'ui', 'file-handles', 'files',
                                           'document-after-abort', 'all'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    evidence = args.evidence.resolve()
    if not evidence.is_relative_to(WORK):
        parser.error('Evidence must remain inside the assigned disk-backed guest workspace')
    evidence.mkdir(parents=True, mode=0o700, exist_ok=False)
    result = dict(check=args.check, status='running', cases=[])
    server = None
    started = time.monotonic()
    try:
        if args.check == 'native-grants-unit':
            result['cases'] = native_grants(out, evidence)
        else:
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
                partial(Fixture, directory=str(REPO / 'tools/testdata/guest-integration')))
            server.daemon_threads = True
            server.phase = 0
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            base = f'http://127.0.0.1:{server.server_port}'
            if args.check in ('document-after-abort', 'all'):
                result['cases'].append(case(out, evidence, 'document-after-abort',
                                            lambda client: document_after_abort(client, base)))
                if args.check == 'document-after-abort':
                    result.update(status='passed', seconds=time.monotonic() - started)
                    print('GUEST_DOCUMENT_AFTER_ABORT_PASS', flush=True)
                    return
            if args.check == 'file-handles':
                result['cases'].append(case(out, evidence, 'file-handles', lambda client: file_handles(client, base)))
                result['cases'].append(case(out, evidence, 'file-sink-failure', lambda client: file_sink_failure(client, base)))
                result.update(status='passed', seconds=time.monotonic() - started)
                print('GUEST_FILE_HANDLES_PASS', flush=True)
                return
            if args.check == 'files':
                file_outcomes(out, evidence, base, server, result['cases'])
                result.update(status='passed', seconds=time.monotonic() - started)
                print('GUEST_FILES_PASS', flush=True)
                return
            for name, check in [('clipboard-edit-keyboard', lambda client: clipboard_ui(client, base)),
                                 ('async-clipboard-formats-sinks', lambda client: async_clipboard(client, base)),
                                 ('clipboard-accept-before-ready', lambda client: clipboard_accept_before_ready(client, base)),
                                 ('clipboard-early-accept-retirement', lambda client: clipboard_early_accept_retirement(client, base)),
                                 ('menus-original-owner', lambda client: menus(client, base, server)),
                                 ('menu-inline-selection', lambda client: inline_selection(client, base)),
                                 ('menu-cross-node-selection', lambda client: inline_selection(client, base, True)),
                                ('quick-menu-native-clipboard', lambda client: quick_menu(client, base)),
                                ('clipboard-permission-denied', lambda client: denied_permission(client, base)),
                                 ('native-drag-data-effects', lambda client: drags(client, base)),
                                 ('popup-retires-native-drag', lambda client: popup_drag_retirement(client, base)),
                                ('fullscreen-admission-transition', lambda client: fullscreen(client, base))]:
                server.phase = 0
                result['cases'].append(case(out, evidence, name, check))
            for operation in ('deadline', 'provider', 'navigation', 'close'):
                server.phase = 0
                result['cases'].append(case(out, evidence, 'pending-read-' + operation,
                                            lambda client, op=operation: pending_read(client, base, op)))
            result['cases'].append(case(out, evidence, 'pending-read-child-navigation',
                                        lambda client: child_pending_read(client, base, server)))
            result['cases'].append(case(out, evidence, 'missing-provider',
                                        lambda client: unavailable(client, base), attach=False))
            result['cases'].append(case(out, evidence, 'stateless-clipboard',
                                        lambda client: clipboard_ui(client, base), mode='stateless'))
            if args.check == 'all':
                file_outcomes(out, evidence, base, server, result['cases'])
            print('GUEST_INTEGRATION_PASS', flush=True)
        result.update(status='passed', seconds=time.monotonic() - started)
    except BaseException as error:
        result.update(status='failed', error=repr(error), seconds=time.monotonic() - started)
        raise
    finally:
        if server:
            server.shutdown()
            server.server_close()
            worker.join()
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
