# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Embedded state at the browser boundary, including same-roots process restart."""
import hashlib
import json
import os
from pathlib import Path
import select
import secrets
import signal
import socket
import subprocess
import threading
import time
import traceback

from profile_inventory import observe_tree
from workspace import temp_path, validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
ROOTS = ('profile', 'home', 'cache', 'config', 'data')


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def snapshot(root, output, allow_races=False):
    return observe_tree(root, ROOTS, hashes=True, allow_races=allow_races,
                        report_path=output)


def line(process, seconds):
    ready, _, _ = select.select([process.stdout], [], [], seconds)
    assert ready, 'embedded browser driver barrier timed out'
    value = process.stdout.readline()
    assert value, 'embedded browser driver exited before barrier'
    return json.loads(value)


def process_identity(pid):
    # PID plus start ticks proves a real cold process, without kernel observers.
    fields = Path('/proc/'+str(pid)+'/stat').read_text().rsplit(')', 1)[1].split()
    return dict(pid=pid, start_ticks=int(fields[19]))


def run_phase(root, backing, phase, mode, scope, marker, executable, out, fixture, helpers):
    root.mkdir()
    root_identity = {name:dict(device=(backing/name).stat().st_dev,
                               inode=(backing/name).stat().st_ino) for name in ROOTS}
    before = snapshot(backing, root/'before-observation.json')
    save(root/'before.json', before)
    env = dict(os.environ, TMPDIR=str(temp_path(W)), ALLOY_PROFILE_NO_DISK='1',
        ALLOY_PROFILE_URL='about:blank', ALLOY_PROFILE_ROOT=str(backing/'profile'),
        ALLOY_PROFILE_NETWORK='cut' if mode == 'stateless-cut' else 'keep',
        ALLOY_PROFILE_EMBEDDED_SCOPE=scope,
        ALLOY_PROFILE_SELECTED_FILE=str(root/'unused-input'),
        ALLOY_PROFILE_OUTPUT_FILE=str(root/'unused-output'), HOME=str(backing/'home'),
        XDG_CACHE_HOME=str(backing/'cache'), XDG_CONFIG_HOME=str(backing/'config'),
        XDG_DATA_HOME=str(backing/'data'), XDG_RUNTIME_DIR=str(root/'runtime'))
    for name in ('ALLOY_PROFILE_MEMORY', 'ALLOY_PROFILE_PERSISTENT'):
        env.pop(name, None)
    if mode == 'memory':
        env['ALLOY_PROFILE_MEMORY'] = '1'
    if mode == 'persistent':
        env.pop('ALLOY_PROFILE_NO_DISK')
        env['ALLOY_PROFILE_PERSISTENT'] = '1'
    source = (REPO/'tools/profile_embedded_restart.mjs').read_text()
    source = source.replace('__CDP_HELPERS__', helpers).replace(
        '__EMBEDDED_FRAME_HELPERS__', (REPO/'tools/profile_embedded_frame.mjs').read_text())
    script = root/'driver.mjs'
    script.write_text(source)
    subprocess.run(['node', '--check', str(script)], check=True, timeout=20)
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    argv = ['xvfb-run', '-a', str(executable), '--off-screen-rendering-enabled', '--ozone-platform=x11',
            '--disable-gpu', '--remote-debugging-port='+str(port)]
    save(root/'argv.json', argv)
    browser = driver = None
    try:
        with (root/'stdio.log').open('xb') as log, (root/'driver.stderr').open('xb') as errors:
            browser = subprocess.Popen(argv, cwd=out, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            driver = subprocess.Popen(['node', str(script), str(port),
                'http://127.0.0.1:'+str(fixture.server_port), mode, scope, phase,
                marker, str(root/'stdio.log')], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=errors, text=True)
            live = line(driver, 150)
            save(root/'live.json', live)
            assert live['phase'] == 'live'
            events = [json.loads(v[14:]) for v in (root/'stdio.log').read_text().splitlines()
                      if v.startswith('PROFILE_EVENT ')]
            pid, = [v['pid'] for v in events if v['event'] == 'process']
            identity = process_identity(pid)
            assert identity['start_ticks'] > 0 and browser.poll() is None
            current = snapshot(backing, root/'live-observation.json', mode == 'persistent')
            save(root/'live-paths.json', current)
            if mode != 'persistent':
                assert current == before, 'embedded work changed backing roots while live'
            # Section 6 row 8b requires a live marker scan, not only a final scan.
            scan_markers(backing, marker)
            driver.stdin.write('close\n')
            driver.stdin.flush()
            closed = line(driver, 30)
            save(root/'closed.json', closed)
            assert closed['phase'] == 'closed'
            driver.communicate(timeout=30)
            assert driver.returncode == 0
            browser.wait(timeout=60)
            assert browser.returncode == 0
        events = [json.loads(v[14:]) for v in (root/'stdio.log').read_text().splitlines()
                  if v.startswith('PROFILE_EVENT ')]
        assert len([v for v in events if v['event'] == 'created']) == len([
            v for v in events if v['event'] == 'closed']) == 2
        after = snapshot(backing, root/'after-observation.json')
        save(root/'after.json', after)
        scan_markers(backing, marker)
        if mode != 'persistent':
            assert after == before
        assert root_identity == {name:dict(device=(backing/name).stat().st_dev,
                                           inode=(backing/name).stat().st_ino) for name in ROOTS}
        result = dict(pass_=True, pid=pid, identity=identity, root_identity=root_identity,
                      actual=live['proof'], roots=str(backing), normal_exit=True,
                      embedded_marker_absent=True)
        save(root/'result.json', result)
        return result
    except BaseException:
        (root/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        if driver is not None and driver.poll() is None:
            driver.terminate()
            driver.wait(timeout=15)
        if browser is not None and browser.poll() is None:
            os.killpg(browser.pid, signal.SIGTERM)
            browser.wait(timeout=15)


def scan_markers(backing, marker):
    for name in ROOTS:
        for path in (backing/name).rglob('*'):
            if path.is_file() and not path.is_symlink():
                assert marker.encode() not in path.read_bytes(), ('embedded marker on disk', str(path))


def run(out, evidence, fixture_type, helpers, scopes, modes=('persistent',)):
    validate_output(W, out, REPO)
    assert os.environ['TMPDIR'] == str(temp_path(W))
    evidence.mkdir()
    print('PROFILE_RESTART_EVIDENCE', evidence, flush=True)
    executable = out/'alloy_profile_consumer'
    fixture = fixture_type(None, None, evidence, secure=False)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    results = []
    try:
        for mode in modes:
            for scope in scopes:
                case = evidence/(mode+'-'+scope)
                case.mkdir()
                backing = case/'backing'
                backing.mkdir()
                for name in ROOTS:
                    (backing/name).mkdir()
                marker = 'ALLOY_STATE_'+secrets.token_hex(16)
                first = run_phase(case/'writer', backing, 'writer', mode, scope, marker,
                                  executable, out, fixture, helpers)
                second = run_phase(case/'reader', backing, 'reader', mode, scope, marker,
                                   executable, out, fixture, helpers)
                assert first['pid'] != second['pid']
                assert first['identity']['start_ticks'] != second['identity']['start_ticks']
                assert first['roots'] == second['roots'] == str(backing)
                assert first['root_identity'] == second['root_identity']
                if mode in ('memory', 'persistent'):
                    old = first['actual']['observations']['network']['cache'][0]['cacheToken']
                    new = second['actual']['observations']['networkBefore']['cache'][0]['cacheToken']
                    assert old != new, 'process restart reused embedded HTTP cache'
                if mode == 'persistent':
                    top = first['actual']['observations']['ordinary']
                    reloaded = second['actual']['observations']['ordinary']
                    assert reloaded['previous']['local'] == top['marker']
                    assert reloaded['cdnToken'] == top['cdnToken']
                    assert first['actual']['marker'] != top['marker']
                result = dict(mode=mode, scope=scope, first_pid=first['pid'],
                              second_pid=second['pid'], same_roots=True, normal_exits=True,
                              embedded_marker_absent=True, pass_=True)
                results.append(result)
                save(evidence/'cases.json', results)
                print('PASS embedded-process-restart', mode, scope, flush=True)
        assert len(results) == len(scopes)*len(modes)
        result = dict(pass_=True, results=results, actual_process_restart=True,
                      ordinary_functional_and_restart='persistent' in modes,
                      encrypted_profile_verified=False, full_item6E_gate=False)
        save(evidence/'result.json', result)
        return result
    except BaseException:
        (evidence/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        fixture.shutdown()
        fixture.server_close()
        paths = [p for p in sorted(evidence.rglob('*')) if p.is_file()
                 and not p.is_symlink() and p.name != 'SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in paths))
