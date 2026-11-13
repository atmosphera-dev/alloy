# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Build the Linux pre-engine entry with only static libc, never libcef/C++."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.parent.mkdir(parents=True, exist_ok=True)
sources = ['base/process/alloy_guest_runtime.c',
           'brave/alloy/guest/bootstrap/main.c',
           'brave/alloy/guest/bootstrap/supervisor.c']
subprocess.run(['cc', '-std=c11', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2',
                '-I', str(args.root), *[str(args.root / name) for name in sources],
                '-o', str(args.out)], check=True)
programs = subprocess.check_output(['readelf', '-l', str(args.out)], text=True)
dynamic = subprocess.check_output(['readelf', '-d', str(args.out)], text=True)
if 'INTERP' in programs or '(NEEDED)' in dynamic:
    raise RuntimeError('The pre-engine entry acquired a dynamic dependency')
