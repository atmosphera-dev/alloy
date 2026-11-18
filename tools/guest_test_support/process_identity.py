# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Read and validate an original process while its pidfd remains pinned."""
import array
import json
import os
from pathlib import Path
import select
import socket


def identity(pid, handle):
    poll = select.poll()
    poll.register(handle, select.POLLIN)
    if poll.poll(0):
        raise ProcessLookupError(pid)
    # Check the kernel pidfd's namespace-local identity, not a caller's bare PID.
    fields = dict(line.split(':', 1) for line in
                  (Path('/proc/self/fdinfo') / str(handle)).read_text().splitlines() if ':' in line)
    if int(fields['Pid']) != pid:
        raise ProcessLookupError(pid)
    proc = Path('/proc') / str(pid)
    start = (proc / 'stat').read_text().rpartition(')')[2].split()[19]
    executable = (proc / 'exe').stat()
    result = dict(start_ticks=start, exe_device=executable.st_dev, exe_inode=executable.st_ino)
    if poll.poll(0):
        raise ProcessLookupError(pid)
    return result


def validate_channel():
    """VM-root read-only validator; one already-pinned descriptor per request."""
    assert os.getuid() == 0
    channel = socket.socket(fileno=0)
    while True:
        packet, control, flags, _ = channel.recvmsg(4096, socket.CMSG_SPACE(array.array('i').itemsize))
        if not packet:
            break
        handles = []
        try:
            for level, kind, data in control:
                assert level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
                received = array.array('i')
                received.frombytes(data)
                handles.extend(received)
            assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) and len(handles) == 1
            expected = json.loads(packet)
            try:
                current = identity(expected['pid'], handles[0])
            except (FileNotFoundError, ProcessLookupError):
                response = b'E'
            else:
                response = b'V' if current == expected['identity'] else b'R'
            channel.sendall(response)
        finally:
            for handle in handles:
                os.close(handle)


if __name__ == '__main__':
    validate_channel()
