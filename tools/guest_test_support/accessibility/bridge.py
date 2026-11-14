#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Small app-owned AT-SPI reference, not an engine platform backend.

Protocol reference: at-spi2-core 2.52 xml/{Accessible,Application,Action,
Component,Text,Cache,Socket,Event}.xml. All native actions return through the
translated Alloy SDK. No DOM shim or guest display/screen-reader bus.
"""
import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib

PREFIX = 'org.a11y.atspi.'
ACCESSIBLE = PREFIX + 'Accessible'
ACTION = PREFIX + 'Action'
COMPONENT = PREFIX + 'Component'
TEXT = PREFIX + 'Text'
APPLICATION = PREFIX + 'Application'
ROOT = '/org/a11y/atspi/accessible/root'
NULL = '/org/a11y/atspi/null'
PROPERTIES = 'org.freedesktop.DBus.Properties'
ROLES = {
    'application': Atspi.Role.APPLICATION, 'frame': Atspi.Role.FRAME,
    'rootWebArea': Atspi.Role.DOCUMENT_WEB, 'button': Atspi.Role.PUSH_BUTTON,
    'link': Atspi.Role.LINK, 'textField': Atspi.Role.ENTRY,
    'textFieldWithComboBox': Atspi.Role.COMBO_BOX, 'checkBox': Atspi.Role.CHECK_BOX,
    'radioButton': Atspi.Role.RADIO_BUTTON, 'heading': Atspi.Role.HEADING,
    'paragraph': Atspi.Role.PARAGRAPH, 'staticText': Atspi.Role.STATIC,
    'inlineTextBox': Atspi.Role.STATIC, 'image': Atspi.Role.IMAGE,
    'genericContainer': Atspi.Role.SECTION, 'group': Atspi.Role.GROUPING,
    'iframe': Atspi.Role.INTERNAL_FRAME, 'list': Atspi.Role.LIST,
    'listItem': Atspi.Role.LIST_ITEM, 'menuListPopup': Atspi.Role.POPUP_MENU,
    'menuListOption': Atspi.Role.MENU_ITEM, 'comboBoxSelect': Atspi.Role.COMBO_BOX,
}


def emit(value):
    print('GUEST_AX_EVENT ' + json.dumps(value, ensure_ascii=True), flush=True)


def codepoint_offset(text, utf16_offset):
    encoded = text.encode('utf-16-le')
    if not isinstance(utf16_offset, int) or not 0 <= utf16_offset <= len(encoded) // 2:
        return None
    try:
        return len(encoded[:utf16_offset * 2].decode('utf-16-le'))
    except UnicodeDecodeError:
        return None


@dataclass(frozen=True)
class ServiceContext:
    """Injected service data/callbacks; accessible objects know no consumer."""
    bus: object
    null: object
    desktop_ref: object
    application_ref: object
    get_application_id: object
    set_application_id: object
    is_active: object
    is_focused: object
    presentation: object
    action: object


class Accessible(dbus.service.Object):
    def __init__(self, context, path, data):
        super().__init__(context.bus, path)
        self.context, self.path, self.data = context, path, data
        self.children, self.parent = [], None
        self.target = None
        self.stale = False
        self.caret, self.selection = -1, None

    def ref(self):
        return dbus.Struct((self.context.bus.get_unique_name(), dbus.ObjectPath(self.path)), signature='so')

    def interfaces(self):
        result = [ACCESSIBLE, COMPONENT]
        if self.path == ROOT:
            result.append(APPLICATION)
        elif self.target:
            result.append(TEXT)
            if self.actionable():
                result.append(ACTION)
        return result

    def actionable(self):
        return self.data is not None and (self.data.get('role') in ('button', 'link', 'checkBox', 'radioButton')
                or 'doDefault' in self.data.get('actions', []))

    def text(self):
        return '' if not self.data else self.data.get('value') or self.data.get('name', '')

    def props(self, interface):
        data = self.data or {}
        if interface == ACCESSIBLE:
            return {'Name': dbus.String(data.get('name', '')),
                    'Description': dbus.String(data.get('description', '')),
                    'Parent': self.parent.ref() if self.parent else self.context.desktop_ref(),
                    'ChildCount': dbus.Int32(len(self.children)),
                    'Locale': dbus.String('C.UTF-8'), 'HelpText': dbus.String(''),
                    'AccessibleId': dbus.String(self.path)}
        if interface == APPLICATION and self.path == ROOT:
            return {'ToolkitName': dbus.String('AlloyReference'), 'Version': dbus.String('experimental'),
                    'AtspiVersion': dbus.String('2.1'), 'Id': dbus.Int32(self.context.get_application_id())}
        if interface == TEXT:
            return {'CharacterCount': dbus.Int32(len(self.text())), 'CaretOffset': dbus.Int32(self.caret)}
        if interface == ACTION:
            return {'NActions': dbus.Int32(int(self.actionable()))}
        return {}

    @dbus.service.method(PROPERTIES, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        properties = self.props(str(interface))
        if name not in properties:
            raise dbus.exceptions.DBusException('Unknown property', name='org.freedesktop.DBus.Error.InvalidArgs')
        return properties[name]

    @dbus.service.method(PROPERTIES, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        return self.props(str(interface))

    @dbus.service.method(PROPERTIES, in_signature='ssv', out_signature='')
    def Set(self, interface, name, value):
        if interface == APPLICATION and name == 'Id' and self.path == ROOT:
            self.context.set_application_id(int(value))
        else:
            raise dbus.exceptions.DBusException('Read only', name='org.freedesktop.DBus.Error.PropertyReadOnly')

    @dbus.service.method(ACCESSIBLE, in_signature='i', out_signature='(so)')
    def GetChildAtIndex(self, index):
        return self.children[index].ref() if 0 <= index < len(self.children) else self.context.null

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='a(so)')
    def GetChildren(self):
        return [child.ref() for child in self.children]

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='i')
    def GetIndexInParent(self):
        return self.parent.children.index(self) if self.parent and self in self.parent.children else -1

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='a(ua(so))')
    def GetRelationSet(self):
        return []

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='u')
    def GetRole(self):
        return int(ROLES.get((self.data or {}).get('role'), Atspi.Role.UNKNOWN))

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='s')
    def GetRoleName(self):
        return (self.data or {}).get('role', 'unknown')

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='s')
    def GetLocalizedRoleName(self):
        return self.GetRoleName()

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='au')
    def GetState(self):
        if self.stale:
            states = [Atspi.StateType.STALE]
        elif self.data is None:
            states = [Atspi.StateType.DEFUNCT]
        else:
            states = [Atspi.StateType.ENABLED, Atspi.StateType.SENSITIVE]
            if not self.data.get('ignored'):
                states += [Atspi.StateType.VISIBLE, Atspi.StateType.SHOWING]
            if 'focusable' in self.data.get('states', []):
                states.append(Atspi.StateType.FOCUSABLE)
            if self.context.is_focused(self):
                states.append(Atspi.StateType.FOCUSED)
            if self.data.get('role') == 'frame':
                if self.context.is_active():
                    states.append(Atspi.StateType.ACTIVE)
            if 'editable' in self.data.get('states', []):
                states.append(Atspi.StateType.EDITABLE)
            if 'multiline' in self.data.get('states', []):
                states.append(Atspi.StateType.MULTI_LINE)
        value = sum(1 << int(state) for state in set(states))
        return [dbus.UInt32(value & 0xffffffff), dbus.UInt32(value >> 32)]

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='a{ss}')
    def GetAttributes(self):
        return {'toolkit': 'AlloyReference'}

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='(so)')
    def GetApplication(self):
        return self.context.application_ref()

    @dbus.service.method(ACCESSIBLE, in_signature='', out_signature='as')
    def GetInterfaces(self):
        return self.interfaces()

    @dbus.service.method(APPLICATION, in_signature='u', out_signature='s')
    def GetLocale(self, _category):
        return 'C.UTF-8'

    @dbus.service.method(ACTION, in_signature='i', out_signature='s')
    def GetName(self, index):
        return 'click' if index == 0 and self.actionable() else ''

    @dbus.service.method(ACTION, in_signature='i', out_signature='s')
    def GetLocalizedName(self, index):
        return self.GetName(index)

    @dbus.service.method(ACTION, in_signature='i', out_signature='s')
    def GetDescription(self, index):
        return self.GetName(index)

    @dbus.service.method(ACTION, in_signature='i', out_signature='s')
    def GetKeyBinding(self, _index):
        return ''

    @dbus.service.method(ACTION, in_signature='', out_signature='a(sss)')
    def GetActions(self):
        return [('click', 'Invoke native page action', '')] if self.actionable() else []

    @dbus.service.method(ACTION, in_signature='i', out_signature='b')
    def DoAction(self, index):
        return index == 0 and self.actionable() and self.context.action(self, 'default')

    @dbus.service.method(COMPONENT, in_signature='', out_signature='b')
    def GrabFocus(self):
        return self.context.action(self, 'focus')

    @dbus.service.method(COMPONENT, in_signature='u', out_signature='(iiii)')
    def GetExtents(self, coordinate):
        if not self.data or not self.data.get('bounds_available', False):
            return (0, 0, 0, 0)
        x, y, width, height = self.data['bounds']
        # One app transform for presentation and accessibility: scale followed
        # by the app-selected screen origin. Native bounds already include scroll.
        scale, origin = self.context.presentation()
        screen_x = origin[0] if coordinate == Atspi.CoordType.SCREEN else 0
        screen_y = origin[1] if coordinate == Atspi.CoordType.SCREEN else 0
        left, top = math.floor(x * scale) + screen_x, math.floor(y * scale) + screen_y
        right, bottom = math.ceil((x + width) * scale) + screen_x, math.ceil((y + height) * scale) + screen_y
        return (left, top, right - left, bottom - top)

    @dbus.service.method(COMPONENT, in_signature='u', out_signature='ii')
    def GetPosition(self, coordinate):
        return self.GetExtents(coordinate)[:2]

    @dbus.service.method(COMPONENT, in_signature='', out_signature='ii')
    def GetSize(self):
        return self.GetExtents(Atspi.CoordType.WINDOW)[2:]

    @dbus.service.method(COMPONENT, in_signature='iiu', out_signature='b')
    def Contains(self, x, y, coordinate):
        left, top, width, height = self.GetExtents(coordinate)
        return left <= x < left + width and top <= y < top + height

    @dbus.service.method(COMPONENT, in_signature='iiu', out_signature='(so)')
    def GetAccessibleAtPoint(self, x, y, coordinate):
        for child in reversed(self.children):
            if child.Contains(x, y, coordinate):
                return child.ref()
        return self.ref() if self.Contains(x, y, coordinate) else self.context.null

    @dbus.service.method(COMPONENT, in_signature='', out_signature='u')
    def GetLayer(self):
        return int(Atspi.ComponentLayer.WIDGET)

    @dbus.service.method(COMPONENT, in_signature='', out_signature='n')
    def GetMDIZOrder(self):
        return 0

    @dbus.service.method(COMPONENT, in_signature='', out_signature='d')
    def GetAlpha(self):
        return 1.0

    @dbus.service.method(COMPONENT, in_signature='u', out_signature='b')
    def ScrollTo(self, _type):
        return self.context.action(self, 'scroll-visible')

    @dbus.service.method(TEXT, in_signature='ii', out_signature='s')
    def GetText(self, start, end):
        return self.text()[max(0, start):None if end == -1 else max(0, end)]

    @dbus.service.method(TEXT, in_signature='i', out_signature='i')
    def GetCharacterAtOffset(self, offset):
        return ord(self.text()[offset]) if 0 <= offset < len(self.text()) else 0

    @dbus.service.method(TEXT, in_signature='iu', out_signature='sii')
    def GetStringAtOffset(self, offset, granularity):
        text = self.text()
        if not 0 <= offset < len(text):
            return ('', len(text), len(text))
        if granularity == Atspi.TextGranularity.CHAR:
            return (text[offset], offset, offset + 1)
        if granularity == Atspi.TextGranularity.WORD:
            start, end = offset, offset
            while start and not text[start - 1].isspace():
                start -= 1
            while end < len(text) and not text[end].isspace():
                end += 1
            return (text[start:end], start, end)
        return (text, 0, len(text))

    @dbus.service.method(TEXT, in_signature='iu', out_signature='sii')
    def GetTextAtOffset(self, offset, _boundary):
        return self.GetStringAtOffset(offset, Atspi.TextGranularity.LINE)

    @dbus.service.method(TEXT, in_signature='ib', out_signature='a{ss}ii')
    def GetAttributeRun(self, _offset, _defaults):
        return ({}, 0, len(self.text()))

    @dbus.service.method(TEXT, in_signature='', out_signature='a{ss}')
    def GetDefaultAttributeSet(self):
        return {}

    @dbus.service.method(TEXT, in_signature='', out_signature='i')
    def GetNSelections(self):
        return int(self.selection is not None)

    @dbus.service.method(TEXT, in_signature='i', out_signature='ii')
    def GetSelection(self, index):
        if index != 0 or self.selection is None:
            raise dbus.exceptions.DBusException('No such selection', name='org.freedesktop.DBus.Error.InvalidArgs')
        return self.selection

    @dbus.service.method(TEXT, in_signature='iii', out_signature='b')
    def SetSelection(self, index, start, end):
        if index != 0 or not 0 <= start <= end <= len(self.text()):
            return False
        anchor = len(self.text()[:start].encode('utf-16-le')) // 2
        focus = len(self.text()[:end].encode('utf-16-le')) // 2
        return self.context.action(self, 'selection', anchor=anchor, focus=focus)

    @dbus.service.method(TEXT, in_signature='ii', out_signature='b')
    def AddSelection(self, start, end):
        return self.SetSelection(0, start, end)

    @dbus.service.method(TEXT, in_signature='i', out_signature='b')
    def RemoveSelection(self, index):
        return index == 0 and self.selection is not None and self.SetCaretOffset(self.caret)

    @dbus.service.method(TEXT, in_signature='i', out_signature='b')
    def SetCaretOffset(self, offset):
        if not 0 <= offset <= len(self.text()):
            return False
        # AT-SPI offsets count Unicode code points; Chromium counts UTF-16 units.
        units = len(self.text()[:offset].encode('utf-16-le')) // 2
        return self.context.action(self, 'selection', anchor=units, focus=units)

    @dbus.service.signal(PREFIX + 'Event.Object', signature='siiva{sv}')
    def StateChanged(self, state, enabled, unused, value, properties):
        pass

    @dbus.service.signal(PREFIX + 'Event.Object', signature='siiva{sv}')
    def ChildrenChanged(self, operation, index, unused, child, properties):
        pass

    @dbus.service.signal(PREFIX + 'Event.Object', signature='siiva{sv}')
    def PropertyChange(self, name, unused, unused2, value, properties):
        pass


class Cache(dbus.service.Object):
    @dbus.service.method(PREFIX + 'Cache', in_signature='', out_signature='a((so)(so)(so)iiassusau)')
    def GetItems(self):
        # No optimistic cache: all data is served from the current app model.
        return []


class Bridge:
    def __init__(self, bus_address, send, origin=(40, 60)):
        self.bus = dbus.bus.BusConnection(bus_address)
        self.send, self.origin, self.scale = send, origin, 1.0
        self.application_id = -1
        self.null = dbus.Struct(('', dbus.ObjectPath(NULL)), signature='so')
        self.desktop = self.null
        self.focus = None
        self.awaiting_snapshot = True
        self.trees, self.objects, self.update = {}, {}, None
        self.context = ServiceContext(self.bus, self.null, lambda: self.desktop, lambda: self.root.ref(),
            lambda: self.application_id, lambda value: setattr(self, 'application_id', value),
            lambda: bool(self.update and self.update['view_focused']), lambda obj: self.focus is obj,
            lambda: (self.scale, self.origin), self.action)
        self.root = Accessible(self.context, ROOT, {'role': 'application', 'name': 'Alloy reference app'})
        self.frame = Accessible(self.context, '/org/a11y/atspi/accessible/frame', {'role': 'frame', 'name': 'Guest view'})
        self.root.children = [self.frame]
        self.frame.parent = self.root
        self.cache = Cache(self.bus, '/org/a11y/atspi/cache')
        registry = self.bus.get_object(PREFIX + 'Registry', ROOT)
        self.desktop = dbus.Interface(registry, PREFIX + 'Socket').Embed(self.root.ref())

    def action(self, obj, action, **arguments):
        if not obj.data or not obj.target or not self.update:
            return False
        command = dict(obj.target, op='action', action=action, source='screen-reader',
                       document=self.update['document'], geometry=self.update['geometry'],
                       capture_us=str(time.monotonic_ns() // 1000), value='', anchor=0, focus=0)
        command.update(arguments)
        self.send(command)
        emit({'event': 'atspi-action', 'command': command, 'name': obj.data['name']})
        return True

    def apply(self, update):
        if self.awaiting_snapshot and not update['snapshot']:
            return
        if self.update and int(update['sequence']) != int(self.update['sequence']) + 1 and not update['snapshot']:
            self.trees.clear()
            self.update = None
            self.awaiting_snapshot = True
            self.focus = None
            self.frame.children = []
            for obj in self.objects.values():
                obj.data, obj.target, obj.children = None, None, []
                obj.stale = True
                obj.StateChanged('stale', 1, 0, dbus.Int32(0), {})
            self.send({'op': 'resync'})
            emit({'event': 'bridge-resync', 'observed_sequence': update['sequence']})
            return
        self.awaiting_snapshot = False
        self.update, self.scale = update, update['scale']
        if update['snapshot']:
            self.trees.clear()
        for tree in update.get('removed', []):
            self.trees.pop(tree, None)
        for tree in update['trees']:
            self.trees[tree['id']] = tree
        current, by_node = {}, {}
        before = {obj.path: list(obj.children) for obj in [self.frame, *self.objects.values()]}
        for tree in self.trees.values():
            for node in tree['nodes']:
                key = (update['document'], tree['id'], tree['generation'], node['id'], node['generation'])
                obj = self.objects.get(key)
                if obj is None:
                    obj = Accessible(self.context, '/org/a11y/atspi/accessible/n' + str(len(self.objects) + 1), node)
                    self.objects[key] = obj
                old_name = (obj.data or {}).get('name')
                was_stale = obj.stale
                obj.stale = False
                obj.data = node
                obj.target = dict(tree=tree['id'], tree_generation=tree['generation'],
                                  node=node['id'], node_generation=node['generation'])
                anchor = node['attributes'].get('textSelStart', -1)
                focus = node['attributes'].get('textSelEnd', -1)
                if tree.get('selection_anchor') == node['id'] and tree.get('selection_focus') == node['id']:
                    anchor, focus = tree['selection_anchor_offset'], tree['selection_focus_offset']
                first, last = codepoint_offset(obj.text(), anchor), codepoint_offset(obj.text(), focus)
                obj.caret = last if last is not None else -1
                obj.selection = tuple(sorted((first, last))) if first is not None and last is not None and first != last else None
                obj.children, obj.parent = [], None
                current[key] = obj
                by_node[(tree['id'], node['id'])] = obj
                if old_name != node['name']:
                    obj.PropertyChange('accessible-name', 0, 0, node['name'], {})
                if was_stale:
                    obj.StateChanged('stale', 0, 0, dbus.Int32(0), {})
        self.frame.children = []
        for tree in self.trees.values():
            for node in tree['nodes']:
                obj = by_node[(tree['id'], node['id'])]
                obj.children = [by_node[(tree['id'], child)] for child in node['children']]
                child_tree = self.trees.get(node['child_tree'])
                if child_tree:
                    obj.children.append(by_node[(child_tree['id'], child_tree['root'])])
                for child in obj.children:
                    child.parent = obj
        for tree in self.trees.values():
            root = by_node[(tree['id'], tree['root'])]
            if not root.parent:
                root.parent = self.frame
                self.frame.children.append(root)
        for key, obj in self.objects.items():
            if key not in current and (obj.data is not None or obj.stale):
                obj.data, obj.target, obj.children = None, None, []
                obj.stale = False
                obj.StateChanged('defunct', 1, 0, dbus.Int32(0), {})
        for obj in [self.frame, *current.values()]:
            old = before.get(obj.path, [])
            for index, child in enumerate(old):
                if child not in obj.children:
                    obj.ChildrenChanged('remove', index, 0, child.ref(), {})
            for index, child in enumerate(obj.children):
                if child not in old:
                    obj.ChildrenChanged('add', index, 0, child.ref(), {})
        # Only the primary native tree names the globally focused frame. Other
        # frame trees may retain their last local focus after focus leaves them.
        # Iterating those local values would invent a competing focus authority.
        root = self.trees.get(update['root_tree'])
        target = self.trees.get(root['focused_tree']) if root and root['focused_tree'] else root
        focused = by_node.get((target['id'], target['focus'])) if update['view_focused'] and target and target.get('focus', 0) != 0 else None
        if focused is not self.focus:
            if self.focus and self.focus.data is not None:
                self.focus.StateChanged('focused', 0, 0, dbus.Int32(0), {})
            self.focus = focused
            if focused:
                focused.StateChanged('focused', 1, 0, dbus.Int32(0), {})
                emit({'event': 'atspi-focus', 'name': focused.data['name'], 'path': focused.path})
        emit({'event': 'bridge-applied', 'sequence': update['sequence'], 'snapshot': update['snapshot']})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bus', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS='disabled:', NO_AT_BRIDGE='1')
    for name in ('AT_SPI_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY'):
        env.pop(name, None)
    child = subprocess.Popen([str(args.out / 'alloy_guest_accessibility_consumer')],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, bufsize=0)
    def send(command):
        child.stdin.write((json.dumps(command, ensure_ascii=True) + '\n').encode())
    bridge = Bridge(args.bus, send)
    loop = GLib.MainLoop()
    buffers = {}
    drop_next_delta = False
    native_closed = False
    def readable(fd, condition, native):
        nonlocal drop_next_delta, native_closed
        chunk = os.read(fd, 65536)
        if not chunk:
            if native:
                loop.quit()
            else:
                send({'op': 'quit'})
            return False
        buffer = buffers.get(fd, b'') + chunk
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            if native:
                print(line.decode(), flush=True)
                if line.startswith(b'GUEST_AX_EVENT '):
                    event = json.loads(line[len(b'GUEST_AX_EVENT '):])
                    if event['event'] == 'closed':
                        native_closed = True
                    if event['event'] == 'update':
                        if drop_next_delta and not event['snapshot']:
                            drop_next_delta = False
                            emit({'event': 'bridge-update-dropped', 'sequence': event['sequence']})
                        else:
                            bridge.apply(event)
            else:
                command = json.loads(line)
                if command['op'] == 'drop-next-delta':
                    drop_next_delta = True
                    emit({'event': 'queued', 'op': command['op'], 'ok': True, 'id': '0'})
                else:
                    send(command)
        buffers[fd] = buffer
        return True
    GLib.io_add_watch(child.stdout.fileno(), GLib.IO_IN | GLib.IO_HUP, readable, True)
    GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, readable, False)
    emit({'event': 'bridge-ready', 'bus_name': bridge.bus.get_unique_name()})
    try:
        loop.run()
    finally:
        if not native_closed and child.poll() is None:
            send({'op': 'quit'})
        child.wait(timeout=60)
    assert native_closed, 'native session exited without final completion'
    return child.returncode


if __name__ == '__main__':
    sys.exit(main())
