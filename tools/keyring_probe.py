#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Private-Secret-Service consumer for profile_probe --check keyring.

Row 7 currently covers five records with real SQLite/LevelDB/Simple Cache,
profile-private files, key-source and native API lifecycle. Real browser-store
routing is supplied by item 6.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from workspace import workspace_path, validate_output

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def inventory(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file()}


def run(evidence, env, name, argv, expected=0, limit=120):
    argv = list(map(str, argv))
    start = time.monotonic()
    result = subprocess.run(['timeout', '--kill-after=20', str(limit), *argv],
        cwd=WORK, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=limit + 30)
    (evidence / (name + '.log')).write_bytes(result.stdout)
    row = dict(name=name, argv=argv, exit=result.returncode, expected_exit=expected,
               seconds=time.monotonic() - start)
    (evidence / (name + '.json')).write_text(json.dumps(row, indent=2) + '\n')
    if result.returncode != expected:
        raise RuntimeError(f'{name}: {row}; {result.stdout.decode(errors="replace")}')
    return result.stdout.decode(errors='replace')


def session(out, evidence):
    assert os.environ.get('ALLOY_TEST_KEYRING') == os.environ['HOME']
    env = dict(os.environ, LD_LIBRARY_PATH=str(out))
    roots = evidence / 'profiles'
    roots.mkdir(mode=0o700)
    native = out / 'alloy_keyring_consumer'
    c_api = out / 'alloy_profile_store_consumer'
    for phase in ('create', 'read'):
        text = run(evidence, env, 'native-' + phase, [native, phase, roots / 'native'])
        assert 'PASS keyring-native-' + phase in text
        text = run(evidence, env, 'c-api-' + phase,
                   [c_api, '--keyring', phase, roots / 'c-api'])
        assert 'PASS profile-api-keyring-' + phase in text
    text = run(evidence, env, 'cpp-api',
        [out / 'alloy_profile_cpp_consumer', '--keyring', roots / 'cpp-api'])
    assert 'PASS profile-api-keyring-cpp-wrapper' in text
    # Each profile's salt is the opaque lookup ID in the authenticated bootstrap.
    for name in ('native', 'c-api'):
        before = inventory(roots / name)
        root = (roots / name / 'root').read_bytes()
        salt = root[44 + 24:44 + 56].hex()
        run(evidence, env, name + '-remove-entry', ['secret-tool', 'clear',
            'application', 'alloy', 'xdg:schema', 'org.alloy.ProfileKey.v1', 'profile', salt])
        command = [native, 'missing', roots / name] if name == 'native' else [
            c_api, '--keyring', 'missing', roots / name]
        text = run(evidence, env, name + '-missing', command)
        assert ('PASS keyring-native-missing' if name == 'native' else
                'PASS profile-api-keyring-missing') in text
        assert before == inventory(roots / name), 'Missing-key failure mutated profile'
    # Use fresh profiles for locked-provider refusal; removal never provisions.
    for name in ('native', 'c-api'):
        command = [native, 'create', roots / (name + '-locked')] if name == 'native' else [
            c_api, '--keyring', 'create', roots / (name + '-locked')]
        run(evidence, env, name + '-create-lock-fixture', command)
    text = run(evidence, env, 'collection-alias', ['gdbus', 'call', '--session',
        '--dest', 'org.freedesktop.secrets', '--object-path', '/org/freedesktop/secrets',
        '--method', 'org.freedesktop.Secret.Service.ReadAlias', 'default'])
    import re
    match = re.search(r"'(/org/freedesktop/secrets/collection/[^']+)'", text)
    assert match, text
    run(evidence, env, 'lock-collection', ['gdbus', 'call', '--session',
        '--dest', 'org.freedesktop.secrets', '--object-path', '/org/freedesktop/secrets',
        '--method', 'org.freedesktop.Secret.Service.Lock', "['" + match[1] + "']"])
    for name in ('native', 'c-api'):
        path = roots / (name + '-locked')
        before = inventory(path)
        command = [native, 'locked', path] if name == 'native' else [
            c_api, '--keyring', 'locked', path]
        run(evidence, env, name + '-locked', command)
        assert before == inventory(path), 'Locked-keyring failure mutated profile'
    refused = roots / 'create-locked-refused'
    run(evidence, env, 'c-api-create-locked', [c_api, '--keyring', 'create-locked', refused])
    assert not refused.exists(), 'Locked-keyring create wrote profile files'
    run(evidence, env, 'ciphertext-markers', ['rg', '-a', '-i', '-l',
        '-e', 'keyring-site-', '-e', 'KEYRING_TITLE_DATA_', '-e', 'History',
        '-e', 'KEYRING_NATIVE_STORE_', '-e', 'KEYRING_PRIVATE_SETTINGS_', roots], expected=1)
    (evidence / 'profile-files.json').write_text(json.dumps(inventory(roots), indent=2) + '\n')
    print('PASS keyring-session-consumers', flush=True)


def probe(out, env):
    evidence = WORK / 'evidence' / ('profile-keyring-' + out.name + '-' + str(time.time_ns()))
    evidence.mkdir(mode=0o700)
    try:
        run(evidence, env, 'compile-c', ['gcc', '-std=c11', '-D_GNU_SOURCE', '-DUSING_CEF_SHARED',
            '-I', WORK / 'src', '-I', WORK / 'src/cef', '-I', out / 'gen/cef',
            REPO / 'tools/profile_store_consumer.c', '-L', out, '-Wl,-rpath,' + str(out),
            '-lcef', '-o', out / 'alloy_profile_store_consumer'])
        text = run(evidence, env, 'private-session', [sys.executable, REPO / 'tools/test_keyring.py',
            '--root', evidence / 'keyring', '--', sys.executable, __file__,
            '--session', out, evidence], limit=410)
        assert 'PASS keyring-session-consumers' in text
        for name in ('native', 'c-api'):
            path = evidence / 'profiles' / (name + '-locked')
            before = inventory(path)
            command = [out / 'alloy_keyring_consumer', 'unavailable', path] if name == 'native' else [
                out / 'alloy_profile_store_consumer', '--keyring', 'unavailable', path]
            run(evidence, env, name + '-unavailable', [sys.executable, REPO / 'tools/test_keyring.py',
                '--root', evidence / (name + '-absent'), '--absent', '--', *command])
            assert before == inventory(path)
        refused = evidence / 'profiles/create-unavailable-refused'
        run(evidence, env, 'c-api-create-unavailable', [sys.executable, REPO / 'tools/test_keyring.py',
            '--root', evidence / 'create-absent', '--absent', '--',
            out / 'alloy_profile_store_consumer', '--keyring', 'create-unavailable', refused])
        assert not refused.exists(), 'Absent-provider create wrote profile files'
        hashes = {name: hashlib.file_digest((out / name).open('rb'), 'sha256').hexdigest()
                  for name in ('libcef.so', 'alloy_keyring_consumer', 'alloy_profile_store_consumer',
                               'alloy_profile_cpp_consumer')}
        (evidence / 'result.json').write_text(json.dumps(dict(pass_=True, binaries=hashes,
            five_record_files_cold_read=True, lock_wipes_keys=True, missing_key_fails=True,
            sqlite_leveldb_simple_cache_cold_read=True, profile_private_settings_cold_read=True,
            locked_keyring_fails=True, no_provider_fails_without_activation=True,
            ciphertext_markers_absent=True, browser_store_routing_verified=False), indent=2) + '\n')
        print('PASS profile-keyring')
    finally:
        # Keep raw runtime state on the NUC; collectors take hashes/logs only.
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS']
        (evidence / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() +
            '  ' + str(p.relative_to(evidence)) + '\n' for p in files))
        print('PROFILE_KEYRING_EVIDENCE ' + str(evidence), flush=True)


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] != '--session':
        raise SystemExit(2)
    output = Path(sys.argv[2]).resolve()
    validate_output(WORK, output, REPO)
    session(output, Path(sys.argv[3]).resolve())
