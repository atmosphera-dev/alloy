# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Trusted root fixture stops after identity transfer and request receipt."""
import array
import os
import signal
import socket
import sys

assert os.getuid() == 0
channel = socket.socket(fileno=0)
if sys.argv[1:] == ['before-identity']:
    signal.raise_signal(signal.SIGSTOP)
    raise AssertionError('Stopped startup helper unexpectedly resumed')
assert not sys.argv[1:]
handle = os.pidfd_open(os.getpid())
channel.sendmsg([b'I'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [handle]))])
os.close(handle)
packet, control, flags, _ = channel.recvmsg(4096, socket.CMSG_SPACE(array.array('i').itemsize))
for level, kind, data in control:
    assert level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
    handles = array.array('i')
    handles.frombytes(data)
    for handle in handles:
        os.close(handle)
assert packet and not flags
signal.raise_signal(signal.SIGSTOP)
raise AssertionError('Stopped validator unexpectedly resumed instead of termination')
