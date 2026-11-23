# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""One owner for temporary native consumers using the build's app allowance."""
from contextlib import contextmanager
import os
import uuid


@contextmanager
def native_consumer_executable(out):
    # The validated build output's alloy_* attachment permits the app-side
    # enclosure creation. Evidence directories deliberately have no blanket
    # userns allowance. These unique fixture names are never packaged.
    executable = out / ('alloy_guest_fixture-' + uuid.uuid4().hex)
    descriptor = os.open(executable, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o700)
    os.close(descriptor)
    try:
        yield executable
    finally:
        executable.unlink(missing_ok=True)
