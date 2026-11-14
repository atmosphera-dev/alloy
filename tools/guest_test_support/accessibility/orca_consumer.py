#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Drive the installed, unmodified Orca core against the app's real AT-SPI bus.

Uses Orca's actual script selection, traversal, focus/action utilities, speech
generator, event manager and speech-server extension. Only the output device is
the explicit app-side speech sink. A private app-test display supports Orca's
modifier initialization; desktop keyboard injection is not part of this proof.
"""
import json
import os
import sys
import traceback
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib
from orca import settings

SINK = 'guest_test_support.accessibility.speech_sink'
settings.speechSystemOverride = SINK
settings.speechServerFactory = SINK
settings.speechFactoryModules = [SINK]
settings.enableBraille = False
settings.enableSound = False
settings.enableMouseReview = False

from orca import orca, script_manager, speech, settings_manager
from orca.ax_object import AXObject
from orca.ax_component import AXComponent


def emit(value):
    print('GUEST_ORCA_EVENT ' + json.dumps(value, ensure_ascii=True), flush=True)


def main():
    assert os.environ.get('AT_SPI_BUS_ADDRESS'), 'private accessibility bus is required'
    assert os.environ.get('ALLOY_AX_SPEECH_SINK'), 'explicit app speech sink is required'
    # Same settings-manager activation as the installed /usr/bin/orca entry;
    # the isolated XDG_DATA_HOME supplies its disk-backed preference directory.
    settings_manager.getManager().activate(customSettings={
        'enableSpeech': True, 'enableBraille': False, 'enableSound': False,
        'enableMouseReview': False, 'speechServerFactory': SINK})
    orca.init()
    speech.init()
    assert speech._speechserver.__class__.__module__ == SINK, 'native speech fallback is forbidden'
    emit({'event': 'ready', 'orca': orca.__file__, 'sink': SINK})
    loop = GLib.MainLoop()
    state = {'current': None, 'script': None, 'app': None}
    buffer = b''

    def find_app():
        desktop = Atspi.get_desktop(0)
        for index in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(index)
            if app.get_name() == 'Alloy reference app':
                return app
        raise AssertionError('reference application is absent from actual AT-SPI desktop')

    def command(data):
        if data['op'] == 'quit':
            loop.quit()
            return
        app = find_app()
        script = script_manager.getManager().getScript(app, state['current'])
        if data['op'] == 'find':
            current, visited = app, []
            seen = set()
            while current:
                assert current not in seen, 'cycle in live AT-SPI tree'
                seen.add(current)
                name = AXObject.get_name(current)
                visited.append(name)
                if name == data['name']:
                    break
                current = AXObject.get_child(current, 0) or script.utilities.findNextObject(current)
            assert current is not None, (data['name'], visited)
            state.update(current=current, script=script, app=app)
            # The consumer asks native Chromium to focus the object; the bridge
            # does not manufacture a focused state in response to this request.
            focused = AXObject.grab_focus(current)
            script.presentObject(current, speechonly=True)
            emit({'event': 'found', 'name': AXObject.get_name(current),
                  'visited': visited, 'focus_queued': bool(focused)})
        elif data['op'] == 'read':
            assert state['current'] is not None
            script.presentObject(state['current'], speechonly=True)
            emit({'event': 'read', 'name': AXObject.get_name(state['current'])})
        elif data['op'] == 'activate':
            assert state['current'] is not None
            accepted = AXObject.do_named_action(state['current'], 'click')
            emit({'event': 'invoked', 'name': AXObject.get_name(state['current']), 'queued': bool(accepted)})
        elif data['op'] == 'bounds':
            assert state['current'] is not None
            window = AXComponent.get_rect(state['current'])
            screen = Atspi.Component.get_extents(state['current'], Atspi.CoordType.SCREEN)
            emit({'event': 'bounds', 'window': [window.x, window.y, window.width, window.height],
                  'screen': [screen.x, screen.y, screen.width, screen.height]})
        elif data['op'] == 'state':
            assert state['current'] is not None
            emit({'event': 'state', 'name': AXObject.get_name(state['current']),
                  'focused': AXObject.has_state(state['current'], Atspi.StateType.FOCUSED)})
        else:
            raise AssertionError('unknown Orca command: ' + data['op'])

    def readable(fd, condition):
        nonlocal buffer
        chunk = os.read(fd, 65536)
        if not chunk:
            loop.quit()
            return False
        buffer += chunk
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            try:
                command(json.loads(line))
            except Exception:
                traceback.print_exc()
                emit({'event': 'failed'})
        return True
    GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, readable)
    try:
        loop.run()
    finally:
        orca.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main())
