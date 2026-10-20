#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fixed 2-hour gate / 24-hour release runner, supervised independently of SSH."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def seal(directory):
    """Called only after collector AND its ExecStopPost AND analysis have exited."""
    links={str(p.relative_to(directory)):os.readlink(p) for p in directory.rglob('*') if p.is_symlink()}
    (directory/'SYMLINKS.json').write_text(json.dumps(links,indent=2)+'\n')
    paths = sorted(p for p in directory.rglob('*') if p.is_file() and not p.is_symlink() and p != directory/'SHA256SUMS')
    (directory / 'SHA256SUMS').write_text(''.join(digest(p) + '  ' + str(p.relative_to(directory)) + '\n' for p in paths))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--mode', choices=('gate', 'release'), required=True)
    parser.add_argument('--lanes', default='observe,enforce')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    modes = args.lanes.split(',')
    if not modes or len(set(modes)) != len(modes) or any(m not in ('observe', 'enforce') for m in modes):
        parser.error('--lanes is observe, enforce or observe,enforce')
    if os.geteuid() != 0:
        # Keep the user's transient-unit supervision, including signal forwarding.
        os.execvp('sudo', ['sudo', '-n', 'env', 'TMPDIR=/home/builder/work/tmp',
                  'PYTHONDONTWRITEBYTECODE=1', 'PYTHONUNBUFFERED=1', sys.executable,
                  str(Path(__file__).resolve()), *sys.argv[1:]])
    os.environ.update(TMPDIR='/home/builder/work/tmp', PYTHONDONTWRITEBYTECODE='1')
    from common import ROOT, TMP, SCRIPTS, guard, lock, now, run, save, state, unit
    guard()
    owner = lock(ROOT / 'harness-run.lock')
    build = args.build.resolve()
    assert all((build / name).is_file() for name in ('cefclient', 'libcef.so', 'args.gn'))
    flags = (build / 'args.gn').read_text()
    for name in ('alloy_enable_guard_log', 'alloy_enable_guard_canary', 'alloy_enable_class_marking'):
        assert re.search(r'\b' + name + r'\s*=\s*true\b', flags), 'Requires verification config: ' + name
    d = (args.out or ROOT / 'harness-runs' / ('alloy-' + args.mode + '-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))).resolve()
    assert d.is_relative_to(ROOT) and not d.is_relative_to(build) and d != ROOT
    d.parent.mkdir(parents=True, exist_ok=True)
    d.mkdir(exist_ok=False)
    target = 7200 if args.mode == 'gate' else 86400
    lanes = [dict(name='alloy-osr-' + m, build='alloy-osr', mode=m, n=n, ns='alloy-osr-' + m,
                  host=f'10.77.{n}.1', guest=f'10.77.{n}.2', veth=f'alh{n}', peer=f'alg{n}', port=18080+n,
                  binary=str(build / 'cefclient'), seed=51000+n)
             for n, m in enumerate(modes, 1)]
    config = dict(time=now(), campaign_origin=time.time(), build=str(build), mode=args.mode,
                  target=target, lanes=lanes, out=str(d), source=str(build.parents[1]),
                  artifacts={name: digest(build / name) for name in ('cefclient', 'libcef.so')},
                  harness_sha256={str(p.relative_to(SCRIPTS)): digest(p) for p in SCRIPTS.rglob('*') if p.is_file() and '__pycache__' not in str(p)})
    save(d / 'config.json', config)
    # As in phase1, every long run executes an immutable, run-owned source copy.
    # Retain the canonical table at its normal relative path beside that copy.
    snapshot=d/'source'
    shutil.copytree(SCRIPTS,snapshot/'harness',ignore=shutil.ignore_patterns('__pycache__'))
    table=snapshot/'src/net/base/alloy_traffic_classes.inc'
    table.parent.mkdir(parents=True)
    shutil.copy2(SCRIPTS.parent/'src/net/base/alloy_traffic_classes.inc',table)
    SCRIPTS=snapshot/'harness'
    sys.path.insert(0,str(SCRIPTS))
    run(['chown', '-R', 'builder:builder', d])
    os.environ['ALLOY_HARNESS_CONFIG'] = str(d / 'config.json')
    # These modules now share this run's immutable config; no global phase1 inputs.
    import common
    common.CONFIG = config
    common.LANES = lanes
    common.SCRIPTS = SCRIPTS
    import network
    from monitor import Monitor
    collector = d.name + '-controller'
    assert re.fullmatch(r'[A-Za-z0-9_-]+', d.name), 'Use a systemd-safe output basename'
    stop = False
    def interrupted(*_):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    checkpoints = [('30m', 1800)] if args.mode == 'gate' else [('1h', 3600), ('6h', 21600)]
    index = 0
    analysis = None
    analysis_log = None
    monitor = None
    failure = None
    begin = time.monotonic()
    def start_analysis(tag):
        nonlocal analysis, analysis_log
        analysis_log = (d / ('analysis-' + tag + '.log')).open('xb')
        analysis = subprocess.Popen(['timeout', '--kill-after=10', '600', sys.executable,
                   str(SCRIPTS / 'analysis/report.py'), '--campaign', str(d), '--source', config['source'], '--tag', tag],
                   stdout=analysis_log, stderr=subprocess.STDOUT, env=os.environ)
        save(d / 'analysis-state.json', dict(time=now(), tag=tag, pid=analysis.pid, state='running'))
    try:
        unit(collector, ['python3', SCRIPTS / 'controller.py', d], ROOT, user='root', runtime=target + 1200,
             properties={'TimeoutStopSec': '300', 'MemoryMax': '4G',
                         'StandardOutput': 'append:' + str(d / 'collector.log'),
                         'StandardError': 'append:' + str(d / 'collector.log'),
                         'ExecStopPost': f'/usr/bin/timeout 240 /usr/bin/python3 {SCRIPTS}/network.py cleanup {d}'})
        save(d / 'supervisor.json', dict(time=now(), collector=state(collector), scheduler_pid=os.getpid(),
             scheduler_cgroup=Path('/proc/self/cgroup').read_text()))
        print(json.dumps(dict(directory=str(d), collector=collector, target=target)), flush=True)
        while time.monotonic() - begin < target + 1500:
            if stop:
                raise RuntimeError('scheduler interrupted')
            if (d / 'collection-start.json').exists():
                monitor = monitor or Monitor(d)
                bad = monitor.sample()
                if bad:
                    save(d / 'EARLY-STOP.json', dict(time=now(), violations=bad))
                    raise RuntimeError('outside contact or invalid DSCP packet; see EARLY-STOP.json')
            status = json.loads((d / 'status.json').read_text()) if (d / 'status.json').exists() else {}
            exposure = min(status.get('elapsed', {'pending': 0}).values())
            if analysis and analysis.poll() is not None:
                code = analysis.returncode
                analysis_log.close()
                save(d / 'analysis-state.json', dict(time=now(), state='finished', exit=code, tag=checkpoints[index-1][0]))
                analysis = None
                if code:
                    raise RuntimeError('interim analysis failed; collection stopped')
            if not analysis and index < len(checkpoints) and exposure >= checkpoints[index][1]:
                tag = checkpoints[index][0]
                index += 1
                start_analysis(tag)
            s = state(collector)
            if s.get('ActiveState') in ('failed', 'inactive'):
                assert (d / 'restored.json').exists(), 'collector exited without full cleanup'
                assert not (d / 'FAILED.json').exists() and not (d / 'cleanup-failed.json').exists(), 'collector failed'
                break
            save(d / 'scheduler-status.json', dict(time=now(), exposure=exposure,
                 next=checkpoints[index][0] if index < len(checkpoints) else 'final', analysis_pid=analysis.pid if analysis else None))
            time.sleep(5)
        else:
            raise RuntimeError('finite scheduler deadline; no extensions')
    except BaseException as exc:
        failure = repr(exc)
        save(d / 'NEEDS_ATTENTION.json', dict(time=now(), error=failure))
    finally:
        # Stop producers immediately on a safety/analysis failure, even if a
        # separate analysis is still draining its bounded snapshot.
        if failure and state(collector).get('ActiveState') not in ('inactive', 'failed'):
            (d / 'STOP').touch()
            run(['systemctl', 'stop', collector], timeout=310, check=False)
        if analysis:
            # Analysis has its own finite timeout; don't race a seal against it.
            try:
                analysis.wait(timeout=620)
            except subprocess.TimeoutExpired:
                analysis.kill()
                analysis.wait(timeout=10)
            analysis_log.close()
        if state(collector).get('ActiveState') not in ('inactive', 'failed'):
            (d / 'STOP').touch()
            run(['systemctl', 'stop', collector], timeout=310, check=False)
        if not (d / 'restored.json').exists():
            try:
                network.cleanup(d)
            except BaseException as exc:
                failure = (failure or '') + '; cleanup: ' + repr(exc)
        # Duplicate cleanup must now be a byte-preserving no-op.
        if (d / 'restored.json').exists():
            receipts = {p.name: digest(p) for p in (d / 'restored.json', d / 'producer-quiesced.json')}
            network.cleanup(d)
            assert receipts == {name: digest(d / name) for name in receipts}
        if not failure:
            start_analysis('final')
            code = analysis.wait(timeout=620)
            analysis_log.close()
            analysis = None
            if code:
                failure = 'final analysis failed'
        if not failure:
            assert config['artifacts']=={name:digest(build/name) for name in config['artifacts']}, 'Binary identity changed during run'
            assert config['harness_sha256']=={str(p.relative_to(SCRIPTS)):digest(p) for p in SCRIPTS.rglob('*') if p.is_file() and '__pycache__' not in str(p)}, 'Run source changed'
        save(d / 'result.json', dict(time=now(), pass_=not failure, error=failure, directory=str(d)))
        run(['chown', '-R', 'builder:builder', d], timeout=60)
        seal(d)
        print((d / 'result.json').read_text(), flush=True)
    return int(bool(failure))


if __name__ == '__main__':
    sys.exit(main())
