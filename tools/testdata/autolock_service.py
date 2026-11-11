#!/usr/bin/python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Owned private-bus logind/screensaver double. Never suspends or locks a host."""
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
names = [dbus.service.BusName(name, bus) for name in (
    'org.freedesktop.login1', 'org.freedesktop.ScreenSaver', 'org.alloy.AutoLockFixture')]
SESSION = 'org.freedesktop.login1.Session'
MANAGER = 'org.freedesktop.login1.Manager'
OWN = '/org/freedesktop/login1/session/own'


class Manager(dbus.service.Object):
    @dbus.service.method(MANAGER, in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        print('SESSION_BY_PID', int(pid), flush=True)
        return dbus.ObjectPath(OWN)

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface, property):
        assert interface == MANAGER and property == 'PreparingForSleep'
        return dbus.Boolean(False)

    @dbus.service.signal(MANAGER, signature='b')
    def PrepareForSleep(self, preparing):
        pass


class Session(dbus.service.Object):
    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        assert interface == SESSION
        return {'Active': dbus.Boolean(True), 'LockedHint': dbus.Boolean(False)}

    @dbus.service.signal('org.freedesktop.DBus.Properties', signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.signal(SESSION, signature='')
    def Lock(self):
        pass


class Screen(dbus.service.Object):
    @dbus.service.method('org.freedesktop.ScreenSaver', in_signature='', out_signature='b')
    def GetActive(self):
        return False

    @dbus.service.signal('org.freedesktop.ScreenSaver', signature='b')
    def ActiveChanged(self, active):
        pass


manager = Manager(bus, '/org/freedesktop/login1')
session = Session(bus, OWN)
screen = Screen(bus, '/org/freedesktop/ScreenSaver')


class Control(dbus.service.Object):
    @dbus.service.method('org.alloy.AutoLockFixture', in_signature='s', out_signature='')
    def Trigger(self, trigger):
        print('TRIGGER', trigger, flush=True)
        if trigger == 'sleep':
            manager.PrepareForSleep(True)
        elif trigger == 'screen':
            screen.ActiveChanged(True)
        elif trigger == 'switch':
            session.PropertiesChanged(SESSION, {'Active': dbus.Boolean(False)}, [])
        elif trigger == 'logind-lock':
            session.Lock()
        else:
            raise ValueError(trigger)


control = Control(bus, '/org/alloy/Test')
print('READY_AUTOLOCK_BUS', flush=True)
GLib.MainLoop().run()
