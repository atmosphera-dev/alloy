#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Consume native guest integration outcomes through the translated SDK."""
import argparse
import base64
from functools import partial
import http.server
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import threading
import time
import zlib

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
                                               'integration-unavailable'), event
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

    def title(self, kind, after=0, predicate=lambda _: True, timeout=30):
        def match(value):
            try:
                state = json.loads(value['title'])
                return state.get('kind') == kind and predicate(state)
            except (ValueError, TypeError):
                return False
        value = self.wait('title', match, after, timeout)
        return json.loads(value['title'])

    def page(self, base):
        after = len(self.events)
        assert self.command(op='navigate', url=base + '/ui.html')['ok']
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
        if self.process.poll() is None:
            try:
                self.send(op='terminate')
                self.process.stdin.close()
                self.process.wait(timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=30)
        if self.members:
            self.members.exited()
            self.members.close()
        self.log.close()
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


def drags(client, base):
    page = client.page(base)
    client.action(op='focus', focused=True)
    rectangle = page['rectangles']['drop']
    x, y = round(rectangle['x'] + 20), round(rectangle['y'] + 20)
    after = len(client.events)
    client.action(op='drag-enter', x=x, y=y, allowed=1, modifiers=0, text='app-drop', html='<b>app-drop</b>')
    client.action(op='drag-over', x=x+2, y=y+2, allowed=1, modifiers=0)
    client.wait('drag-effect-consumed', lambda value: value['operation'] == 1, after)
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('native-grants-unit', 'ui', 'all'), required=True)
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
            for name, check in [('clipboard-edit-keyboard', lambda client: clipboard_ui(client, base)),
                                ('async-clipboard-formats-sinks', lambda client: async_clipboard(client, base)),
                                ('menus-original-owner', lambda client: menus(client, base, server)),
                                ('quick-menu-native-clipboard', lambda client: quick_menu(client, base)),
                                ('clipboard-permission-denied', lambda client: denied_permission(client, base)),
                                ('native-drag-data-effects', lambda client: drags(client, base)),
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
                raise RuntimeError('Native file/upload/save/download acceptance is not implemented yet')
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
