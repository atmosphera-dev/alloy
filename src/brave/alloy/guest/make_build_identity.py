# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fingerprint the explicit guest implementation inputs used by both peers."""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('inputs', type=Path, nargs='+')
args = parser.parse_args()
digest = hashlib.sha256()
for index, path in enumerate(args.inputs):
    body = path.read_bytes()
    digest.update(index.to_bytes(8, 'big'))
    digest.update(len(body).to_bytes(8, 'big'))
    digest.update(body)
args.out.parent.mkdir(parents=True, exist_ok=True)
body = '#define ALLOY_GUEST_BUILD_ID "' + digest.hexdigest() + '"\n'
if not args.out.exists() or args.out.read_text() != body:
    args.out.write_text(body)
