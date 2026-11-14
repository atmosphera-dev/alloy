#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Observe native AX outcomes and real Orca against an app-owned private bus."""
import argparse
from functools import partial
import http.server
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse, parse_qs

from guest_test_support.display import environment
from guest_test_support.accessibility.display import PrivateDisplay
from workspace import validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
SUPPORT = REPO / 'tools/guest_test_support/accessibility'


class Fixture(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlparse(self.path)
        if url.path in ('/clicked', '/frame-clicked', '/nested-clicked', '/replacement-clicked',
                        '/focused', '/frame-focused', '/frame-removed', '/text', '/range'):
            with self.server.lock:
                self.server.receipts.append({'path': url.path, 'query': parse_qs(url.query)})
            self.send_response(204)
            self.end_headers()
        else:
            super().do_GET()


class Process:
    def __init__(self, argv, env, evidence, prefix):
        self.log = (evidence / 'process.log').open('xb')
        self.child = subprocess.Popen(argv, cwd=WORK, env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.prefix, self.buffer, self.events = prefix.encode(), b'', []
        self.trees, self.update = {}, None
        self.peers = []

    def send(self, command):
        self.child.stdin.write((json.dumps(command, ensure_ascii=True) + '\n').encode())

    def pump(self, timeout=1):
        if not select.select([self.child.stdout], [], [], timeout)[0]:
            return
        chunk = os.read(self.child.stdout.fileno(), 65536)
        assert chunk, f'consumer exited {self.child.poll()}'
        self.log.write(chunk)
        self.log.flush()
        self.buffer += chunk
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            if line.startswith(self.prefix):
                event = json.loads(line[len(self.prefix):])
                assert event['event'] not in ('failed', 'unavailable', 'invalid-command'), event
                self.events.append(event)
                if event['event'] == 'update':
                    if event['snapshot']:
                        self.trees.clear()
                    for tree in event.get('removed', []):
                        self.trees.pop(tree, None)
                    for tree in event['trees']:
                        self.trees[tree['id']] = tree
                    self.update = event

    def wait(self, event, predicate=lambda _: True, after=0, timeout=90):
        deadline = time.monotonic() + timeout
        while True:
            for value in self.events[after:]:
                if value['event'] == event and predicate(value):
                    return value
            assert time.monotonic() < deadline, (event, self.events[-8:])
            for peer in self.peers:
                peer.pump(0)
            self.pump(min(1, deadline - time.monotonic()))

    def command(self, data, expected='queued'):
        after = len(self.events)
        self.send(data)
        return self.wait(expected, lambda v: expected != 'queued' or v.get('op') == data['op'], after=after)

    def named(self, name):
        for tree in self.trees.values():
            for node in tree['nodes']:
                if node['name'] == name and not node['ignored']:
                    return tree, node
        return None

    def node(self, name):
        deadline = time.monotonic() + 90
        while not self.named(name):
            assert time.monotonic() < deadline, ('missing native node', name)
            self.pump()
        tree, node = self.named(name)
        return {'document': self.update['document'], 'geometry': self.update['geometry'],
                'tree': tree['id'], 'tree_generation': tree['generation'],
                'node': node['id'], 'node_generation': node['generation']}, node

    def absent(self, name):
        deadline = time.monotonic() + 90
        while self.named(name):
            assert time.monotonic() < deadline, ('retired native node remains in current tree', name)
            self.pump()

    def action(self, target, action='default', **details):
        command = dict(target, op='action', action=action, source='agent', value='',
                       anchor=0, focus=0, capture_us='1234567')
        command.update(details)
        after = len(self.events)
        queued = self.command(command)
        assert queued['ok'] and queued['id'] != '0', queued
        receipt = self.wait('interaction', lambda v: v['id'] == queued['id'], after=after)
        assert receipt['source'] == 'agent' and receipt['kind'] == 'ax', receipt
        assert receipt['capture_us'] == '1234567', receipt
        return receipt

    def close(self):
        if self.child.poll() is None:
            self.send({'op': 'quit'})
        closed = self.wait('closed')
        assert closed['reason'] == 'closed', closed
        assert sum(v['event'] == 'endpoint-closed' for v in self.events) == 2, self.events[-8:]
        assert self.child.wait(timeout=60) == 0

    def dispose(self):
        if self.child.poll() is None:
            try:
                self.send({'op': 'quit'})
                self.child.wait(timeout=60)
            except (OSError, subprocess.TimeoutExpired):
                self.child.kill()
                self.child.wait(timeout=60)
        self.log.close()


def receipt(server, path, query=None):
    with server.lock:
        return any(v['path'] == path and (query is None or v['query'] == query) for v in server.receipts)


def wait_receipt(server, path, client, query=None):
    deadline = time.monotonic() + 60
    while not receipt(server, path, query):
        assert time.monotonic() < deadline, ('missing native page receipt', path, query)
        client.pump()


def native(out, evidence, base, server):
    evidence.mkdir()
    env = environment(evidence / 'runtime', out)
    client = Process([str(out / 'alloy_guest_accessibility_consumer')], env, evidence, 'GUEST_AX_EVENT ')
    results = {}
    try:
        assert client.wait('prepared')['accessibility']
        client.wait('ready')
        client.wait('configured')
        assert client.command({'op': 'navigate', 'url': base + '/index.html'})['ok']
        client.wait('load', lambda v: v['status'] == 200)
        button, button_data = client.node('Native Action')
        child, child_data = client.node('Frame Action')
        nested, _ = client.node('Nested Action')
        assert button['tree'] != child['tree'], 'cross-frame identity was flattened'
        assert button_data['bounds_available'] and button_data['bounds'] == [40, 80, 180, 40], button_data
        assert child_data['bounds_available'] and child_data['bounds'] == [50, 300, 200, 40], child_data
        results['initial'] = {'button': button, 'bounds': button_data['bounds'], 'frame': child}
        invalid = client.action(dict(button, tree='not-a-native-token'))
        assert invalid['outcome'] == 'rejected:invalid', invalid
        results['malformed_tree'] = invalid
        client.command(dict(button, op='focus'))
        result = client.action(button)
        assert result['outcome'] == 'accepted' and int(result['dispatch_us']) > 0, result
        client.wait('title', lambda v: v['title'] == 'AX_INVOKED')
        client.node('Native action completed')
        wait_receipt(server, '/clicked', client)
        results['default_action'] = result
        text, text_data = client.node('Native text')
        assert text_data['value'] == 'CJK 中文 and emoji 😀', text_data
        focused = client.action(text, 'focus')
        assert focused['outcome'] == 'accepted'
        wait_receipt(server, '/focused', client, {'name': ['Native text']})
        value = 'Native replacement 中文😀'
        assert client.action(text, 'set-value', value=value)['outcome'] == 'accepted'
        client.wait('update', lambda _: client.named('Native text')[1]['value'] == value)
        wait_receipt(server, '/text', client, {'value': [value]})
        results['text'] = {'value': value, 'focus': focused}
        slider, slider_data = client.node('Native range')
        assert slider_data['attributes']['valueForRange'] == 3, slider_data
        increment = client.action(slider, 'increment')
        assert increment['outcome'] == 'accepted', increment
        client.wait('update', lambda _: client.named('Native range')[1]['attributes']['valueForRange'] > 3)
        increased = client.named('Native range')[1]['attributes']['valueForRange']
        wait_receipt(server, '/range', client, {'value': [str(int(increased))]})
        decrement = client.action(slider, 'decrement')
        assert decrement['outcome'] == 'accepted', decrement
        client.wait('update', lambda _: client.named('Native range')[1]['attributes']['valueForRange'] == 3)
        wait_receipt(server, '/range', client, {'value': ['3']})
        results['range_actions'] = {'increment': increment, 'decrement': decrement, 'increased': increased}
        before = len(client.events)
        assert client.command({'op': 'configure', 'width': 800, 'height': 600, 'scale': 1.5})['ok']
        geometry = client.wait('configured', lambda v: v['scale'] == 1.5, after=before)
        client.wait('update', lambda v: v['geometry'] == geometry['geometry'] and
                    client.named('Native action completed') is not None and
                    client.named('Native action completed')[1]['bounds_available'], after=before)
        _, scaled_button = client.node('Native action completed')
        assert scaled_button['bounds_available'] and scaled_button['bounds'] == [40, 80, 180, 40], scaled_button
        stale = client.action(text, 'focus')
        assert stale['outcome'].startswith('rejected:'), stale
        results['stale_geometry'] = stale
        options, _ = client.node('Native options')
        before = len(client.events)
        assert client.action(options)['outcome'] == 'accepted'
        popup = client.wait('popup', lambda v: v['visible'], after=before)
        client.wait('update', lambda v: v['geometry'] == popup['geometry'] and
                    client.named('Native options') is not None and
                    client.named('Native options')[1]['bounds_available'], after=before)
        assert client.action(options, 'focus')['outcome'].startswith('rejected:')
        results['popup'] = popup
        current, _ = client.node('Native options')
        before = len(client.events)
        client.command(dict(current, op='pointer', action='down', x=0, y=0))
        hidden = client.wait('popup', lambda v: not v['visible'], after=before)
        client.wait('update', lambda v: v['geometry'] == hidden['geometry'] and
                    client.named('Native options') is not None and
                    client.named('Native options')[1]['bounds_available'], after=before)
        current, _ = client.node('Native options')
        client.command(dict(current, op='pointer', action='up', x=0, y=0))
        current, _ = client.node('Native text')
        before = len(client.events)
        client.command(dict(current, op='wheel', delta=-120))
        client.wait('update', lambda _: client.named('Native action completed')[1]['bounds'][1] < 80,
                    after=before)
        results['scroll_bounds'] = client.named('Native action completed')[1]['bounds']
        assert client.named('Frame Action')[1]['bounds'][1] < 300, client.named('Frame Action')
        retire, _ = client.node('Retire frame')
        assert client.action(retire)['outcome'] == 'accepted'
        retired_child, _ = client.node('Replacement Action')
        stale = client.action(dict(child, geometry=client.update['geometry']), 'default')
        assert stale['outcome'].startswith('rejected:'), stale
        assert not receipt(server, '/frame-clicked')
        results['stale_frame'] = stale
        nested_stale = client.action(dict(nested, geometry=client.update['geometry']), 'default')
        assert nested_stale['outcome'].startswith('rejected:'), nested_stale
        assert not receipt(server, '/nested-clicked')
        results['stale_descendant'] = nested_stale
        remove, _ = client.node('Remove frame')
        assert client.action(remove)['outcome'] == 'accepted'
        wait_receipt(server, '/frame-removed', client)
        client.absent('Replacement Action')
        removed = client.action(dict(retired_child, geometry=client.update['geometry']))
        assert removed['outcome'].startswith('rejected:'), removed
        assert not receipt(server, '/replacement-clicked')
        results['removed_frame'] = removed
        old, _ = client.node('Native action completed')
        before = len(client.events)
        assert client.command({'op': 'resync'})['ok']
        resync = client.wait('update', lambda v: v['snapshot'], after=before)
        results['resync'] = {'sequence': resync['sequence'], 'trees': len(resync['trees'])}
        before = len(client.events)
        assert client.command({'op': 'navigate', 'url': base + '/replacement.html'})['ok']
        loaded = client.wait('load', lambda v: v['status'] == 200 and v['document'] != old['document'], after=before)
        client.wait('update', lambda v: v['document'] == loaded['document'], after=before)
        replacement, _ = client.node('Replacement Action')
        refused = client.action(old)
        assert refused['outcome'] != 'accepted', refused
        assert not receipt(server, '/replacement-clicked')
        results['stale_document'] = refused
        assert replacement['document'] != old['document']
        client.close()
        return results
    finally:
        client.dispose()


class PrivateBus:
    def __init__(self, root, env):
        root.mkdir()
        self.address = 'unix:path=' + str(root / 'bus')
        self.log = (root / 'services.log').open('xb')
        self.daemon = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--nopidfile',
            '--address=' + self.address, '--print-address=1'], env=env,
            stdout=subprocess.PIPE, stderr=self.log)
        assert self.daemon.stdout.readline().strip().startswith(b'unix:'), 'private bus failed'
        self.env = dict(env, AT_SPI_BUS_ADDRESS=self.address, DBUS_SESSION_BUS_ADDRESS=self.address,
                        NO_AT_BRIDGE='0', PYTHONPATH=str(REPO / 'tools'))
        self.registry = subprocess.Popen(['/usr/libexec/at-spi2-registryd'], env=self.env,
                                          stdout=self.log, stderr=subprocess.STDOUT)
        # Popen is not service readiness. Embed may otherwise race the registry's
        # acquisition of its well-known name on this private, non-activating bus.
        import dbus
        connection = dbus.bus.BusConnection(self.address)
        deadline = time.monotonic() + 60
        try:
            while not connection.name_has_owner('org.a11y.atspi.Registry'):
                assert self.registry.poll() is None, 'private AT-SPI registry exited before readiness'
                assert time.monotonic() < deadline, 'private AT-SPI registry did not become ready'
                time.sleep(0.02)
        except BaseException:
            self.close()
            raise
        finally:
            connection.close()

    def close(self):
        for child in (self.registry, self.daemon):
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=30)
        self.log.close()


def orca(out, evidence, base, server):
    evidence.mkdir()
    env = environment(evidence / 'runtime', out)
    display = PrivateDisplay(evidence / 'display-service', env)
    bus = None
    client = reader = None
    sink = evidence / 'speech.jsonl'
    try:
        bus = PrivateBus(evidence / 'bus-service', display.env)
        client = Process(['/usr/bin/python3', str(SUPPORT / 'bridge.py'), '--bus', bus.address,
                          '--out', str(out)], bus.env, evidence, 'GUEST_AX_EVENT ')
        client.wait('bridge-ready')
        client.wait('ready')
        assert client.command({'op': 'navigate', 'url': base + '/index.html'})['ok']
        client.node('Frame Action')
        client.node('Nested Action')
        client.wait('load', lambda v: v['status'] == 200)
        initial, _ = client.node('Native Action')
        before = len(client.events)
        assert client.command(dict(initial, op='focus'))['ok']
        client.wait('update', lambda v: v['view_focused'], after=before)
        assert client.command({'op': 'drop-next-delta'})['ok']
        text, _ = client.node('Native text')
        assert client.action(text, 'focus')['outcome'] == 'accepted'
        dropped = client.wait('bridge-update-dropped')
        assert client.action(text, 'set-value', value='Gap recovery 中文😀')['outcome'] == 'accepted'
        # A full producer snapshot already is a resynchronization barrier; a
        # later delta must instead cause the bridge to request that snapshot.
        # The adapter unit separately proves the actual gap -> Resync call.
        recovered = client.wait('bridge-applied', lambda v: v['snapshot'] and
                                int(v['sequence']) > int(dropped['sequence']))
        requested = next((v for v in client.events if v['event'] == 'bridge-resync' and
                          int(v['observed_sequence']) > int(dropped['sequence'])), None)
        value_update = client.wait('update', lambda v: any(
            node['name'] == 'Native text' and node['value'] == 'Gap recovery 中文😀'
            for tree in v['trees'] for node in tree['nodes']))
        app_value = client.wait('bridge-applied', lambda v: int(v['sequence']) >= int(value_update['sequence']))
        before = len(client.events)
        assert client.command({'op': 'configure', 'width': 800, 'height': 600, 'scale': 1.5})['ok']
        scaled = client.wait('configured', lambda v: v['scale'] == 1.5, after=before)
        client.wait('update', lambda v: v['geometry'] == scaled['geometry'] and
                    client.named('Native Action') is not None and
                    client.named('Native Action')[1]['bounds_available'], after=before)
        reader_dir = evidence / 'orca'
        reader_dir.mkdir()
        reader = Process(['/usr/bin/python3', str(SUPPORT / 'orca_consumer.py')],
                         dict(bus.env, ALLOY_AX_SPEECH_SINK=str(sink)), reader_dir, 'GUEST_ORCA_EVENT ')
        reader.peers = [client]
        reader.wait('ready')
        found = reader.command({'op': 'find', 'name': 'Native Action'}, 'found')
        assert found['focus_queued'], found
        client.wait('atspi-focus', lambda v: v['name'] == 'Native Action')
        bounds = reader.command({'op': 'bounds'}, 'bounds')
        assert bounds['window'] == [60, 120, 270, 60] and bounds['screen'] == [100, 180, 270, 60], bounds
        assert reader.command({'op': 'activate'}, 'invoked')['queued']
        client.wait('title', lambda v: v['title'] == 'AX_INVOKED')
        interaction = client.wait('interaction', lambda v: v['source'] == 'screen-reader' and
                                  v['kind'] == 'ax' and v['action'] == 'default' and v['outcome'] == 'accepted')
        wait_receipt(server, '/clicked', client)
        entry = reader.command({'op': 'find', 'name': 'Native text'}, 'found')
        assert entry['focus_queued'], entry
        reader.command({'op': 'read'}, 'read')
        cross_frame = reader.command({'op': 'find', 'name': 'Frame Action'}, 'found')
        assert cross_frame['focus_queued'], cross_frame
        client.wait('atspi-focus', lambda v: v['name'] == 'Frame Action')
        frame_bounds = reader.command({'op': 'bounds'}, 'bounds')
        assert frame_bounds['window'] == [75, 450, 300, 60] and frame_bounds['screen'] == [115, 510, 300, 60], frame_bounds
        assert reader.command({'op': 'activate'}, 'invoked')['queued']
        wait_receipt(server, '/frame-clicked', client)
        wait_receipt(server, '/frame-focused', client)
        after = len(client.events)
        returned = reader.command({'op': 'find', 'name': 'Native text'}, 'found')
        assert returned['focus_queued'], returned
        client.wait('atspi-focus', lambda v: v['name'] == 'Native text', after=after)
        current, _ = client.node('Native text')
        after = len(client.events)
        assert client.command(dict(current, op='focus', focused=False))['ok']
        blurred = client.wait('update', lambda v: not v['view_focused'], after=after)
        client.wait('bridge-applied', lambda v: v['sequence'] == blurred['sequence'], after=after)
        blur_state = reader.command({'op': 'state'}, 'state')
        assert not blur_state['focused'], blur_state
        after = len(client.events)
        assert client.command(dict(current, op='focus', focused=True))['ok']
        client.wait('atspi-focus', lambda v: v['name'] == 'Native text', after=after)
        focus_state = reader.command({'op': 'state'}, 'state')
        assert focus_state['focused'], focus_state
        reader.send({'op': 'quit'})
        assert reader.child.wait(timeout=60) == 0
        spoken = [json.loads(line) for line in sink.read_text().splitlines()]
        assert any('Native Action' in value['text'] for value in spoken), spoken
        assert any('Frame Action' in value['text'] for value in spoken), spoken
        assert any('Gap recovery' in value['text'] for value in spoken), spoken
        client.close()
        return {'native_action': interaction, 'cross_frame': cross_frame, 'spoken': spoken,
                'dropped_delta': dropped, 'resync': requested, 'recovered': recovered, 'app_value': app_value,
                'bounds': bounds, 'frame_bounds': frame_bounds, 'blur_state': blur_state, 'focus_state': focus_state,
                'consumer': 'installed unmodified Orca core, real AT-SPI, app speech-server plugin',
                'keyboard_device_path': 'not part of windowless connector proof'}
    finally:
        if reader:
            reader.dispose()
        if client:
            client.dispose()
        if bus:
            bus.close()
        display.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('tree-actions', 'all'), default='all')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(WORK, out)
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        partial(Fixture, directory=str(REPO / 'tools/testdata/guest-accessibility')))
    server.lock, server.receipts = threading.Lock(), []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f'http://127.0.0.1:{server.server_port}'
    try:
        results = {'native': native(out, evidence / 'native', base, server)}
        print('GUEST_AX_PASS', flush=True)
        if args.check == 'all':
            with server.lock:
                server.receipts.clear()
            results['orca'] = orca(out, evidence / 'orca', base, server)
            print('GUEST_ORCA_PASS', flush=True)
        results['protected_acceptance'] = 'pending M2 merge; explicit developmental protection used'
        (evidence / 'results.json').write_text(json.dumps(results, ensure_ascii=True, indent=2) + '\n')
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=30)
    return 0


if __name__ == '__main__':
    sys.exit(main())
