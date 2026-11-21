# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Trusted pipe-child fixtures owned from acquisition through final reap."""
from contextlib import contextmanager
import os
import signal
import subprocess


@contextmanager
def pipe_child():
    process = subprocess.Popen(['/bin/cat'], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
    handle = -1
    try:
        handle = os.pidfd_open(process.pid)
        yield process
    finally:
        try:
            try:
                process.communicate(timeout=15)
            except BaseException:
                if handle >= 0:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                # EOF closes this trusted fixture even if pinning failed. A
                # valid pinned child is killed above before final reaping.
                process.communicate()
                raise
        finally:
            if handle >= 0:
                os.close(handle)


def pin_after_exit(process, open_identity=os.pidfd_open):
    handle = open_identity(process.pid)
    try:
        process.communicate(timeout=15)
        return handle
    except BaseException:
        os.close(handle)
        raise
