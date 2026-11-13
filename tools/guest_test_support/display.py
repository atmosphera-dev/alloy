# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Headless Ozone fixture environment, with every harness path on disk."""
import os
from pathlib import Path


def environment(root: Path, out: Path):
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    env = dict(os.environ)
    for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS',
                 'ALLOY_GUEST_RUNTIME', 'ALLOY_GUEST_BOOTSTRAP',
                 'LD_PRELOAD', 'LD_AUDIT'):
        env.pop(name, None)
    for name, leaf in (('HOME', 'home'), ('TMPDIR', 'tmp'),
                       ('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache'),
                       ('XDG_DATA_HOME', 'data'), ('XDG_STATE_HOME', 'state'),
                       ('XDG_RUNTIME_DIR', 'runtime')):
        path = root / leaf
        path.mkdir(mode=0o700)
        env[name] = str(path)
    env.update(PYTHONDONTWRITEBYTECODE='1', LD_LIBRARY_PATH=str(out),
               GSETTINGS_BACKEND='memory', GIO_USE_VFS='local',
               DBUS_SESSION_BUS_ADDRESS='disabled:', NO_AT_BRIDGE='1')
    return env
