# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Observe completed browser-native DevTools gestures without inferior calls."""
import time

import gdb


def observe_native_gestures(path, save):
    serial = 0
    save(path, dict(serial=serial, pid=0, node=None, active=False))

    class NativeGestureCompleted(gdb.FinishBreakpoint):
        def __init__(self, node):
            super().__init__(gdb.newest_frame(), internal=True)
            self.node = node

        def stop(self):
            nonlocal serial
            # Pinned FrameTreeNode::UpdateUserActivationState returns bool.
            active = int(gdb.parse_and_eval('$rax')) & 255
            assert active in (0, 1)
            if active:
                serial += 1
                save(path, dict(serial=serial, pid=gdb.selected_inferior().pid,
                    node=self.node, active=True, completed=time.time(),
                    scope='Browser FrameTreeNode DevTools activation completed'))
            return False

    class NativeGesture(gdb.Breakpoint):
        def stop(self):
            assert 'x86-64' in gdb.newest_frame().architecture().name()
            # Source-verified x86-64 SysV arguments: this, update, notification.
            # Pinned mojom enums: notify=0, DevTools=2. No object layout reads.
            if int(gdb.parse_and_eval('$esi')) == 0 and int(gdb.parse_and_eval('$edx')) == 2:
                NativeGestureCompleted(hex(int(gdb.parse_and_eval('$rdi'))))
            return False

    return NativeGesture('content::FrameTreeNode::UpdateUserActivationState', internal=True)
