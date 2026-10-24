#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Render CEF then Alloy layers; publish only changed files, preserving mtimes.

Baselines, content-addressed blobs and a crash-resumable publication journal live
outside src. Changed layers are replayed against saved baselines in an isolated
target-only tree, never by resetting the live Chromium checkout. Local edits to
managed files are rejected before publication, including on a no-change rerun.
"""

import argparse
from collections import Counter
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile

import prune_origin
from workspace import temp_path, validate_source, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
ROOT = WORK / 'src'
STATE = WORK / 'apply-state'


def digest(body):
    return hashlib.sha256(body).hexdigest()


def atomic_json(path, value):
    temp = path.with_suffix('.new')
    temp.write_text(json.dumps(value, indent=2) + '\n')
    os.replace(temp, path)


def relative(name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '.git' in p.parts or not p.parts:
        raise RuntimeError(f'Unsafe managed path: {name}')
    return str(p)


def target(root, name):
    p = root / relative(name)
    # Symlinks (including parent directories) may escape the owned tree.
    if p.resolve() != p.absolute():
        raise RuntimeError(f'Symlink in managed path: {p}')
    return p


def patch_files(body, strip):
    names = re.findall(rb'^(?:---|\+\+\+) ([^\t\n]+)', body, re.M)
    return {relative(os.fsdecode(n)[strip:]) for n in names if n != b'/dev/null'}


def run(args, cwd, check=True):
    p = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True,
                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                                TMPDIR=str(temp_path(WORK))), timeout=180)
    if check and p.returncode:
        raise RuntimeError(f'{args}:\n{p.stdout.decode(errors="replace")}\n'
                           f'{p.stderr.decode(errors="replace")}')
    return p


def capture(path):
    if not path.exists():
        return None
    if not path.is_file():
        raise RuntimeError(f'Expected regular file: {path}')
    body = path.read_bytes()
    sha = digest(body)
    blob = STATE / 'blobs' / sha
    if not blob.exists():
        blob.write_bytes(body)
    return dict(sha256=sha, mode=path.stat().st_mode & 0o777)


def identity(path):
    if not path.exists():
        return None
    return dict(sha256=digest(path.read_bytes()), mode=path.stat().st_mode & 0o777)


def materialize(path, entry):
    if entry is None:
        if path.exists():
            path.unlink()
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    # Replace atomically, with the same filesystem and no partial consumer reads.
    fd, temp = tempfile.mkstemp(prefix='.alloy-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write((STATE / 'blobs' / entry['sha256']).read_bytes())
            f.flush()
            os.fsync(f.fileno())
        os.chmod(temp, entry['mode'])
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def publish(state, state_path):
    pending = state['pending']
    touched = 0
    for name in pending.get('remove_dirs', []):
        directory = target(ROOT, name)
        if directory.exists():
            for p in directory.rglob('*'):
                if p.is_file() and str(p.relative_to(ROOT)) not in pending['outputs']:
                    raise RuntimeError(f'Unmanaged file in removed product directory: {p}; preserved')
    for name, desired in pending['outputs'].items():
        p = target(ROOT, name)
        actual = identity(p)
        old = state.get('outputs', {}).get(name, state['baseline'][name])
        if actual not in (old, desired):
            raise RuntimeError(f'Local edit in {name}; preserved, refusing publication')
    for name, desired in pending['outputs'].items():
        p = target(ROOT, name)
        if identity(p) != desired:
            materialize(p, desired)
            touched += 1
    # Remove only the now-empty owned product directories. Untracked files are
    # never silently discarded; they prevent the directory-absence gate.
    for name in pending.get('remove_dirs', []):
        p = target(ROOT, name)
        if p.exists():
            for directory in sorted((d for d in p.rglob('*') if d.is_dir()),
                                    key=lambda d: len(d.parts), reverse=True):
                directory.rmdir()
            p.rmdir()
    state.update(state.pop('pending'))
    atomic_json(state_path, state)
    return touched


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--lists', type=Path, default=WORK / 'lists',
                        help='Pinned wakaba-lists delivery (default: %(default)s)')
    parser.add_argument('--cef-only', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    try:
        validate_source(WORK, args.root, REPO)
    except RuntimeError as error:
        parser.error(str(error))
    if args.report and args.report.resolve().is_relative_to(ROOT):
        parser.error('Report must be outside src, to preserve no-touch reapplication')
    pins = json.loads((REPO / 'pins.json').read_text())
    lists_data = args.lists / 'data'
    if not args.cef_only:
        marker = args.lists / 'wakaba-lists.commit'
        revision = marker.read_text().strip()
        if revision != pins['wakaba_lists']:
            raise RuntimeError(f'wakaba-lists pin mismatch: {revision}; expected {pins["wakaba_lists"]}')
        if not lists_data.is_dir():
            raise RuntimeError(f'Missing wakaba-lists data directory: {lists_data}')
    STATE.mkdir(exist_ok=True)
    (STATE / 'blobs').mkdir(exist_ok=True)
    with (WORK / 'source.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for rel, key in [('.', 'chromium'), ('brave', 'brave-core'), ('cef', 'cef')]:
            got = run(['git', 'rev-parse', 'HEAD'], ROOT / rel).stdout.decode().strip()
            if got != pins[key]['revision']:
                raise RuntimeError(f'{rel} pin mismatch: {got}')
        state_path = STATE / 'state.json'
        state = json.loads(state_path.read_text()) if state_path.exists() else dict(schema=1, baseline={})
        if 'pending' in state:
            print('RESUME_PUBLICATION', publish(state, state_path), flush=True)
        if args.cef_only and state.get('mode') == 'full':
            raise RuntimeError('CEF-only measurement would remove composed layers; use the saved report')
        scope = {}
        cfg = ROOT / 'cef/patch/patch.cfg'
        exec(compile(cfg.read_bytes(), str(cfg), 'exec'), scope)
        cef = []
        inputs = {'pins': pins, 'patch.cfg': digest(cfg.read_bytes())}
        paths = set(state['baseline'])
        repo_roots = {'.', 'brave', 'cef'}
        for row in scope['patches']:
            patch = ROOT / 'cef/patch/patches' / (row['name'] + '.patch')
            body = patch.read_bytes()
            prefix = relative(row['path']) if row.get('path') else '.'
            repo_roots.add(prefix)
            files = {relative(str(PurePosixPath(prefix) / f)) for f in patch_files(body, 0)}
            paths.update(files)
            cef.append(dict(name=row['name'], root=prefix, patch=patch, files=files,
                            condition=row.get('condition')))
            inputs['cef/' + row['name']] = digest(body)
        series_path = REPO / 'patches/series.json'
        composition = json.loads(series_path.read_text()) if series_path.exists() and not args.cef_only else {}
        layers = []
        downstream = []
        if composition:
            inputs['series.json'] = digest(series_path.read_bytes())
            for row in composition['series']:
                patch = REPO / 'patches' / relative(row['patch'])
                body = patch.read_bytes()
                prefix = relative(row['root']) if row['root'] != '.' else '.'
                files = {relative(str(PurePosixPath(prefix) / f)) for f in patch_files(body, 2)}
                paths.update(files)
                repo_roots.add(prefix)
                stage = row.get('stage', 'composition')
                if stage not in ('composition', 'after-prune'):
                    raise RuntimeError(f'Unknown patch stage: {stage}')
                (layers if stage == 'composition' else downstream).append(
                    dict(patch=patch, root=prefix, files=files))
                inputs[row['patch']] = digest(body)
        prune_paths = []
        if not args.cef_only:
            prune_paths = prune_origin.discover(ROOT)
            paths.update(prune_paths)
            inputs['tools/prune_origin.py'] = digest((REPO / 'tools/prune_origin.py').read_bytes())
        overlays = {}
        if not args.cef_only and (REPO / 'src').exists():
            for p in sorted((REPO / 'src').rglob('*')):
                if p.is_file():
                    name = relative(str(p.relative_to(REPO / 'src')))
                    overlays[name] = p
                    paths.add(name)
                    inputs['src/' + name] = dict(sha256=digest(p.read_bytes()), mode=p.stat().st_mode & 0o777)
        # Release data participates in the same incremental, local-edit-safe
        # publication contract as source overlays; builds never fetch it.
        if not args.cef_only:
            for p in sorted(lists_data.rglob('*')):
                if p.is_file():
                    rel = relative(str(p.relative_to(lists_data)))
                    name = 'brave/alloy/data/' + rel
                    overlays[name] = p
                    paths.add(name)
                    inputs['data/' + rel] = dict(sha256=digest(p.read_bytes()), mode=p.stat().st_mode & 0o777)
        mode = 'cef-only' if args.cef_only else 'full'
        fingerprint = digest(json.dumps(dict(inputs=inputs, mode=mode), sort_keys=True).encode())
        # Refuse external edits even when the patch set has not changed.
        for name, expected in state.get('outputs', {}).items():
            if identity(target(ROOT, name)) != expected:
                raise RuntimeError(f'Local edit in {name}; preserved, refusing replay')
        if state.get('fingerprint') == fingerprint:
            report = dict(state['report'], files_touched=0)
        else:
            for name in sorted(paths):
                if name not in state['baseline']:
                    state['baseline'][name] = capture(target(ROOT, name))
            atomic_json(state_path, state)
            with tempfile.TemporaryDirectory(prefix='render-', dir=STATE) as tmp:
                mirror = Path(tmp)
                for rel in sorted(repo_roots):
                    d = mirror / rel
                    d.mkdir(parents=True, exist_ok=True)
                    run(['git', 'init', '--quiet'], d)
                for name, entry in state['baseline'].items():
                    if entry is not None:
                        materialize(target(mirror, name), entry)
                rows = []
                for row in cef:
                    if row['condition'] and row['condition'] not in os.environ:
                        raise RuntimeError(f'Unexpected conditional CEF patch: {row["name"]}')
                    cwd = mirror / row['root']
                    check = run(['git', 'apply', '-p0', '--ignore-whitespace', '--check',
                                 '--verbose', row['patch']], cwd, check=False)
                    actual = run([sys.executable, ROOT / 'cef/tools/patcher.py', '--patch-file',
                                  row['name'], '--patch-dir', cwd], ROOT / 'cef', check=False)
                    status = 'conflict' if actual.returncode else (
                        'offset' if b'offset' in check.stdout + check.stderr else 'clean')
                    if bool(check.returncode) != bool(actual.returncode):
                        raise RuntimeError(f'CEF patcher/check disagree: {row["name"]}')
                    rows.append(dict(name=row['name'], initial=status, final=status,
                                     check=(check.stdout + check.stderr).decode(errors='replace'),
                                     patcher=(actual.stdout + actual.stderr).decode(errors='replace')))
                counts = dict(Counter(r['initial'] for r in rows))
                if counts != {'clean': 86, 'offset': 21, 'conflict': 10}:
                    raise RuntimeError(f'Unexpected CEF baseline: {counts}')
                for layer in layers:
                    run(['git', 'apply', '-p1', '--binary', '--check', layer['patch']], mirror / layer['root'])
                    run(['git', 'apply', '-p1', '--binary', layer['patch']], mirror / layer['root'])
                if not args.cef_only:
                    conflicts = {r['name'] for r in rows if r['initial'] == 'conflict'}
                    if conflicts != set(composition.get('cef_resolutions', [])):
                        raise RuntimeError('Composition must cover every conflicted CEF patch')
                    for name, expected in composition['final_files'].items():
                        p = target(mirror, name)
                        got = digest(p.read_bytes()) if p.exists() else None
                        if got != expected:
                            raise RuntimeError(f'Composition differs from exported spike: {name}')
                    for r in rows:
                        r['final'] = 'resolved' if r['initial'] == 'conflict' else 'applied'
                pruning = []
                for name in prune_paths:
                    p = target(mirror, name)
                    before = p.read_bytes() if p.exists() else None
                    after = prune_origin.render(name, before)
                    if after != before:
                        if after is None:
                            p.unlink()
                        else:
                            p.parent.mkdir(parents=True, exist_ok=True)
                            p.write_bytes(after)
                        pruning.append(dict(path=name, action='delete' if after is None else 'rewrite',
                                            before=digest(before) if before is not None else None,
                                            after=digest(after) if after is not None else None))
                        print('PRUNE', pruning[-1]['action'], name, flush=True)
                    if prune_origin.render(name, after) != after:
                        raise RuntimeError(f'Non-idempotent pruning: {name}')
                for layer in downstream:
                    run(['git', 'apply', '-p1', '--binary', '--check', layer['patch']], mirror / layer['root'])
                    run(['git', 'apply', '-p1', '--binary', layer['patch']], mirror / layer['root'])
                for name, p in overlays.items():
                    materialize(target(mirror, name), capture(p))
                outputs = {name: capture(target(mirror, name)) for name in sorted(paths)}
                report = dict(initial_counts=counts, patches=rows,
                               applied=107 if args.cef_only else 117,
                               unresolved=10 if args.cef_only else 0, pruning=pruning)
                remove_dirs = [] if args.cef_only else ['brave/' + d for d in prune_origin.DELETE_DIRS]
                state['pending'] = dict(outputs=outputs, fingerprint=fingerprint, mode=mode,
                                        report=report, remove_dirs=remove_dirs)
                atomic_json(state_path, state)
                report = dict(report, files_touched=publish(state, state_path))
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            atomic_json(args.report, report)
        print(json.dumps({k: v for k, v in report.items() if k not in ('patches', 'pruning')}, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
