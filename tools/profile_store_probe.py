#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Encrypted profile SDK consumers on the assigned build guest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from workspace import temp_path, validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def main():
    """Compile generated-C consumers and prove SDK lifecycle, callback and key-source contracts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=['core', 'keyring'], default='core')
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    env = dict(os.environ, TMPDIR=str(temp_path(WORK)),
               PYTHONDONTWRITEBYTECODE='1', LD_LIBRARY_PATH=str(out))
    if args.check == 'keyring':
        from keyring_probe import probe
        return probe(out, env)
    evidence = WORK / 'evidence' / ('profile-core-' + out.name + '-' + str(time.time_ns()))
    evidence.mkdir(mode=0o700)
    roots = evidence / 'profiles'
    roots.mkdir(mode=0o700)
    rows = []

    def run(name, argv, expected=0):
        argv = list(map(str, argv))
        start = time.monotonic()
        result = subprocess.run(['timeout', '--kill-after=20', '120', *argv],
            cwd=WORK, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=150)
        (evidence / (name + '.log')).write_bytes(result.stdout)
        row = dict(name=name, argv=argv, exit=result.returncode,
                   expected_exit=expected, seconds=time.monotonic() - start)
        rows.append(row)
        (evidence / 'stages.json').write_text(json.dumps(rows, indent=2) + '\n')
        if result.returncode != expected:
            raise RuntimeError(f'{name}: exit {result.returncode}, expected {expected}')
        return result.stdout.decode(errors='replace')

    try:
        binary = out / 'alloy_profile_store_consumer'
        run('compile-c', ['gcc', '-std=c11', '-D_GNU_SOURCE', '-DUSING_CEF_SHARED',
            '-I', WORK / 'src', '-I', WORK / 'src/cef', '-I', out / 'gen/cef',
            REPO / 'tools/profile_store_consumer.c', '-L', out,
            '-Wl,-rpath,' + str(out), '-lcef', '-o', binary])
        text = run('c-api', [binary, roots])
        for marker in ('password', 'external', 'cancel', 'refcounts'):
            if 'PASS profile-api-' + marker not in text:
                raise RuntimeError('C consumer did not observe ' + marker)
        text = run('cpp-api', [out / 'alloy_profile_cpp_consumer', roots])
        if 'PASS profile-api-cpp-wrapper' not in text:
            raise RuntimeError('C++ wrapper consumer result missing')
        records_binary = out / 'alloy_profile_records_c_consumer'
        run('compile-records-c', ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-D_GNU_SOURCE', '-DUSING_CEF_SHARED',
            '-I', WORK / 'src', '-I', WORK / 'src/cef', '-I', out / 'gen/cef',
            REPO / 'tools/profile_records_consumer.c', '-L', out,
            '-Wl,-rpath,' + str(out), '-lcef', '-o', records_binary])
        text = run('records-c-api', ['xvfb-run', '-a', records_binary,
            roots / 'c-records', evidence / 'records-runtime'])
        if 'PASS profile-records-c-wrapper' not in text:
            raise RuntimeError('C record callback consumer result missing')
        if (roots / 'invalid-unicode').exists():
            raise RuntimeError('Malformed password created profile files')
        run('ciphertext-markers', ['rg', '-a', '-i', '-l',
            '-e', 'PUBLIC_API_PASSWORD_23fc75a9',
            '-e', 'CPP_PROFILE_PASSWORD_09fd1d7c', roots], expected=1)
        path = roots / 'password-profile/root'
        altered = bytearray(path.read_bytes()); altered[-1] ^= 1; path.write_bytes(altered)
        before = {str(p.relative_to(roots)):hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in roots.rglob('*') if p.is_file()}
        text = run('corrupt-root', [binary, '--corrupt', roots / 'password-profile'])
        if 'PASS profile-api-corruption' not in text:
            raise RuntimeError('Corruption did not reach the native caller')
        after = {str(p.relative_to(roots)):hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in roots.rglob('*') if p.is_file()}
        if before != after:
            raise RuntimeError('Failed unlock changed, razed or repaired profile data')
        hashes = {}
        for name in ('libcef.so', 'alloy_profile_store_consumer', 'alloy_profile_cpp_consumer'):
            with (out / name).open('rb') as stream:
                hashes[name] = hashlib.file_digest(stream, 'sha256').hexdigest()
        (evidence / 'result.json').write_text(json.dumps(dict(pass_=True,
            out=str(out), stages=rows, binaries=hashes,
            ciphertext_markers_absent=True, corrupt_unlock_preserved_files=True), indent=2) + '\n')
        print('PASS profile-core-api')
    finally:
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS']
        (evidence / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() +
            '  ' + str(p.relative_to(evidence)) + '\n' for p in files))
        print('PROFILE_CORE_EVIDENCE ' + str(evidence), flush=True)


if __name__ == '__main__':
    main()
