#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Prepare and build cefclient with Brave's native offline Siso environment.

Run under a transient user unit; stdout is the durable build log. This never
calls Brave sync/apply_patches or CEF's hook (which would rerun its patcher).
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
WORK = Path('/home/builder/work/alloy')
ROOT = WORK / 'src'
CONFIG_OUT = {'release': 'Release_Alloy', 'verify': 'Verify_Alloy'}

# The gate compiles the actual support consumed by native profile/device tests,
# plus overrides local to content's unit/browser-test translation units. Keep
# these in the build gate so unbuilt test doubles cannot hide an API mismatch.
GATE_TARGETS = [
    'cefclient', 'alloy_guard_unittests', 'alloy_traffic_unittests',
    'alloy_site_records_unittests', 'alloy_navigation_mock_unittests',
    'alloy_api_test_support', 'leveldb_leveldbutil',
    'obj/content/test/content_unittests/storage_partition_impl_unittest.o',
    'obj/content/test/content_browsertests/renderer_accessible_http_cache_browsertest.o',
    'obj/chrome/browser/policy/unit_tests/browser_dm_token_storage_linux_unittest.o',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--config', choices=CONFIG_OUT, default='release',
                        help='Committed build configuration (default release)')
    parser.add_argument('--target', action='append',
                        help='GN/Ninja target (repeatable; default cefclient)')
    parser.add_argument('--gate', action='store_true',
                        help='Also build Alloy tests and native API/test-double support')
    parser.add_argument('--timeout', type=int, default=32400)
    args = parser.parse_args()
    targets = list(dict.fromkeys((args.target or ['cefclient']) +
                                (GATE_TARGETS if args.gate else [])))
    out = ROOT / 'out' / CONFIG_OUT[args.config]
    if socket.gethostname() != 'wakaba-build' or ROOT.resolve() != ROOT:
        parser.error('Only the approved NUC Alloy source tree is supported')
    state = json.loads((WORK / 'apply-state/state.json').read_text())
    if state.get('mode') != 'full' or state['report']['unresolved'] or 'pending' in state:
        raise RuntimeError('Full apply must succeed before building')
    depot = ROOT / 'brave/vendor/depot_tools'
    env = dict(os.environ, TMPDIR='/home/builder/work/tmp', DEPOT_TOOLS_UPDATE='0',
               PATH=f'{depot}:{ROOT}/third_party/rust-toolchain/bin:{depot}/python2-bin:{depot}/python-bin:/usr/bin:/bin',
               PYTHONPATH=':'.join(str(ROOT / p) for p in ['brave/script', 'tools/grit/grit/extern',
                                                         'brave/vendor/requests', 'build', 'third_party/depot_tools']),
               TARGET_ARCH='x64', RUSTUP_HOME=str(ROOT / 'third_party/rust-toolchain'),
               GSUTIL_ENABLE_LUCI_AUTH='0', BRAVE_CHANNEL='development',
               DEPOT_TOOLS_WIN_TOOLCHAIN='0', NINJA_STATUS='[%f/%t] ', PYTHONUNBUFFERED='1')
    with (WORK / 'source.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        out.mkdir(parents=True, exist_ok=True)
        for source, dest in [(args.config + '.gn', 'args.gn'), ('release.gn', 'release.gn'),
                             ('brave_static_args.gni', 'brave_static_args.gni')]:
            body = (REPO / 'tools/args' / source).read_bytes()
            path = out / dest
            if not path.exists() or path.read_bytes() != body:
                path.write_bytes(body)
        commands = [([sys.executable, 'tools/version_manager.py', '-u', '--fast-check'], ROOT / 'cef', 600),
                    ([str(ROOT / 'buildtools/linux64/gn'), 'gen', str(out)], ROOT, 600)]
        if not args.prepare_only:
            commands.append((['autoninja', '-C', str(out), *targets], ROOT, args.timeout))
        start = time.time()
        for command, cwd, limit in commands:
            print('COMMAND', command, 'CWD', cwd, 'START', time.strftime('%FT%TZ', time.gmtime()), flush=True)
            result = subprocess.run(['timeout', '--kill-after=120s', str(limit), *command],
                                    cwd=cwd, env=env, timeout=limit + 150)
            print('EXIT', result.returncode, 'ELAPSED', time.time() - start, flush=True)
            if result.returncode:
                return result.returncode
        if not args.prepare_only:
            if 'cefclient' in targets and (
                    not (out / 'cefclient').is_file() or not (out / 'libcef.so').is_file()):
                raise RuntimeError('Build exited without expected artifacts')
            print('ALLOY_CEFCLIENT_BUILD_FINISHED' if 'cefclient' in targets
                  else 'ALLOY_TARGET_BUILD_FINISHED', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
