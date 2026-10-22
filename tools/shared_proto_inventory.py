#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only, read-only native LevelDB inspection of shared-proto namespaces."""
from pathlib import Path
import re
import socket
import subprocess

WORK = Path('/home/builder/work/alloy')
# SharedProtoDatabaseClient::PrefixForDatabase uses '<ProtoDbType>_'. The
# retained page-download metadata is 21 and OriginTrials is 44; __global is
# shared-catalog metadata. Neither retained backend is changed by removals.
RETAINED = frozenset({21, 44})


def inspect_shared_proto(directory, utility):
    if socket.gethostname() != 'wakaba-build':
        raise RuntimeError('Shared-proto inspection runs only on the approved NUC')
    directory, utility = Path(directory), Path(utility)
    if not directory.is_relative_to(WORK) or not utility.is_relative_to(WORK/'src/out'):
        raise ValueError('Only owned NUC roots/output tools are supported')
    rows, forbidden = [], []
    for path in sorted(directory.rglob('*')):
        if path.is_symlink() or not path.is_file():
            continue
        if not re.fullmatch(r'[0-9]+\.(?:log|ldb|sst)', path.name):
            continue
        result = subprocess.run([str(utility), 'dump', str(path)],
            capture_output=True, text=True, timeout=30)
        if result.returncode or re.search(r'(?im)^(?:corruption:|badkey |iterator error:|\s*error:)', result.stdout):
            raise RuntimeError(f'Native LevelDB inspection failed for {path}: '
                               f'{result.stderr}\n{result.stdout}')
        for line in result.stdout.splitlines():
            match = re.match(r"\s*(?:put|del) '([^']*)'", line)
            if not match:
                match = re.match(r"'([^']*)' @ [0-9]+ : (?:val|del) =>", line)
            if not match:
                continue
            key = match.group(1)
            if key == '__global' and path.parent.name == 'metadata':
                namespace = 'catalogue'
            else:
                prefix = re.match(r'([0-9]+)_', key)
                if not prefix:
                    raise RuntimeError(f'Unknown shared-proto key format in {path}')
                namespace = int(prefix.group(1))
                if namespace not in RETAINED:
                    forbidden.append(dict(file=str(path), namespace=namespace))
            # Record namespaces, never token values or origin-bearing key tails.
            rows.append(dict(file=str(path), namespace=namespace))
    return dict(directory=str(directory), namespaces=rows, forbidden=forbidden)
