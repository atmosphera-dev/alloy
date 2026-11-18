#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Read-only VM-root observation of one installed fixture's executable inodes."""
import json
import os
from pathlib import Path
import sys

root = Path(sys.argv[1])
assert os.getuid() == 0 and root.is_absolute() and root.is_dir()
executables = [root / name for name in ('alloy_guest_host', 'alloy_guest_engine', 'alloy_guest_sdk_consumer')]
executables += list((root / 'zypak/bin').glob('zypak-*'))
identities = {(p.stat().st_dev, p.stat().st_ino): p for p in executables if p.is_file()}
rows = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdecimal():
        continue
    try:
        info = (proc / 'exe').stat()
        executable = identities.get((info.st_dev, info.st_ino))
        if executable is None:
            continue
        name = executable.name
        argv = (proc / 'cmdline').read_bytes().replace(b'\0', b' ').decode()
        status = dict(line.split(':', 1) for line in (proc / 'status').read_text().splitlines())
        fds = {}
        for descriptor in (proc / 'fd').iterdir():
            try:
                fds[descriptor.name] = str(descriptor.readlink())
            except FileNotFoundError:
                pass
        role = 'browser'
        if name == 'alloy_guest_sdk_consumer':
            role = 'app'
        elif '--alloy-supervisor' in argv:
            role = 'supervisor'
        elif name.startswith('zypak-'):
            role = 'trusted-setup'
        else:
            for field in argv.split():
                if field.startswith('--type='):
                    role = field.removeprefix('--type=')
                    break
        rows.append(dict(pid=int(proc.name), exe=str(executable), role=role, argv=argv,
            status={k: v.strip() for k, v in status.items()}, fds=fds,
            namespaces={n: str((proc / 'ns' / n).readlink()) for n in ('user', 'pid', 'net', 'mnt')},
            apparmor=(proc / 'attr/current').read_text().strip()))
    except (FileNotFoundError, ProcessLookupError):
        pass
print(json.dumps(rows))
