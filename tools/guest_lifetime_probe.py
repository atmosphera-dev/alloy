#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Actual-bootstrap custody regressions; run only in the assigned Alloy guest."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import time

from workspace import validate_workspace, workspace_path

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
POLICIES = ('default', 'ignored', 'nochildwait', 'reaper')
CHECKS = ('signals', 'enclosure', 'late', 'fork-control',
          'clone-enosys', 'waitid-denied', 'kill-denied',
          'init-stopped', 'stopped-creator', 'inner-lost', 'app-dead', 'startup-cancel',
          'registration-reuse', 'registration-allocation', 'registration-duplicate', 'pin-reuse',
          'fd-pressure', 'fault-inner-poll', 'fault-inner-recv', 'fault-inner-wait',
           'fault-app-poll', 'fault-app-recv', 'fault-app-wait', 'eintr-registration', 'orphan-reap', 'proc-view',
            'receive-fork', 'custody-caller-close', 'custody-caller-force',
            'custody-late-wait', 'custody-force-retry', 'registration-allocation-denied',
            'kill-refused-probe', 'kill-refused-retire', 'socket-denied', 'mask-restore-denied')


def compile_fixture(destination, root=None):
    destination.mkdir(parents=True)
    root = root or REPO / 'src'
    guest = root / 'brave/alloy/guest'
    pinned = guest / 'linux_lock/pinned_spawn.c'
    owner = guest / 'bootstrap/wait_owner.c'
    flags = ['-DALLOY_LIFETIME_WAIT_OWNER=1'] if owner.exists() else []
    pinned_flags = ['-DALLOY_LIFETIME_PINNED_SPAWN=1'] if pinned.exists() else []
    signal_wrap = ['-Wl,--wrap=alloy_pinned_signal'] if pinned.exists() else []
    objects = []
    if pinned.exists():
        objects.append(str(destination / 'pinned_spawn.o'))
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root), '-c', str(pinned), '-o', objects[0]], check=True)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I', str(root), *flags, *pinned_flags, str(guest / 'session/supervisor.cc'),
                     str(REPO / 'tools/testdata/guest-lifetime/driver.cc'), *objects, '-ldl',
                     '-Wl,--wrap=waitid,--wrap=poll,--wrap=recvmsg,--wrap=sigprocmask', *signal_wrap,
                    '-o', str(destination / 'driver')], check=True)
    sources = [REPO / 'tools/testdata/guest-lifetime/entry.c',
               guest / 'bootstrap/supervisor.c', guest / 'linux_lock/child_launch.c',
               root / 'base/process/alloy_guest_runtime.c']
    if owner.exists():
        sources.append(owner)
    if pinned.exists():
        sources.append(pinned)
    subprocess.run(['cc', '-std=c11', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), *flags, *pinned_flags, *map(str, sources), '-pthread',
                     '-Wl,--wrap=poll,--wrap=recvmsg,--wrap=waitid,--wrap=waitpid', *signal_wrap,
                    '-o', str(destination / 'alloy_guest_host')], check=True)
    registry_sources = [REPO / 'tools/testdata/guest-lifetime/registration.c',
                        guest / 'linux_lock/child_launch.c', root / 'base/process/alloy_guest_runtime.c']
    if pinned.exists():
        registry_sources.append(pinned)
    subprocess.run(['cc', '-std=c11', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                     '-I', str(root), *pinned_flags, *map(str, registry_sources),
                     '-Wl,--wrap=realloc', *signal_wrap, '-pthread',
                    '-o', str(destination / 'alloy_registry_probe')], check=True)
    subprocess.run(['cc', '-std=c11', '-O2', '-static', '-Wall', '-Wextra', '-Werror',
                    str(REPO / 'tools/testdata/guest-lifetime/pin_reuse.c'),
                    '-o', str(destination / 'alloy_pin_probe')], check=True)


def descendants(parent):
    found = []
    pending = [parent]
    while pending:
        pid = pending.pop()
        try:
            for task in (Path('/proc') / str(pid) / 'task').iterdir():
                children = list(map(int, (task / 'children').read_text().split()))
                pending.extend(children)
                found.extend(children)
        except (FileNotFoundError, ProcessLookupError):
            pass
    return found


def line(process, seconds=10):
    # TextIO.readline can prefetch COPY and REAPED together; polling only the
    # underlying descriptor would then mistake the buffered second line for a
    # hang. Keep our own byte buffer, shared across reads of this driver.
    poll = select.poll()
    poll.register(process.stdout, select.POLLIN)
    pending = getattr(process, '_lifetime_lines', b'')
    deadline = time.monotonic() + seconds
    while b'\n' not in pending:
        assert poll.poll(max(0, int((deadline - time.monotonic()) * 1000))), 'No driver response'
        value = os.read(process.stdout.fileno(), 4096)
        assert value, ('Driver exited', process.poll())
        pending += value
    value, process._lifetime_lines = pending.split(b'\n', 1)
    return value.decode().strip()


def status(pid):
    return dict(row.split(':', 1) for row in
                (Path('/proc') / str(pid) / 'status').read_text().splitlines())


def pinned(pid):
    return os.pidfd_open(pid)


def exited(fd, seconds=10):
    poll = select.poll()
    poll.register(fd, select.POLLIN)
    return bool(poll.poll(int(seconds * 1000)))


def cleanup_driver(process, handles, target):
    errors = []
    try:
        for pid in descendants(process.pid):
            try:
                fd = pinned(pid)
                try:
                    handles.append(fd)
                except BaseException:
                    os.close(fd)
                    raise
            except ProcessLookupError:
                pass
            except Exception as error:
                errors.append(error)
    except Exception as error:
        errors.append(error)
    for fd in handles:
        try:
            signal.pidfd_send_signal(fd, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except Exception as error:
            errors.append(error)
        finally:
            try:
                os.close(fd)
            except Exception as error:
                errors.append(error)
    try:
        if process.poll() is None:
            process.kill()
    except Exception as error:
        errors.append(error)
    try:
        process.wait(timeout=10)
    except Exception as error:
        errors.append(error)
    try:
        if process.poll() is not None:
            (target / 'stderr.txt').write_text(process.stderr.read())
    except Exception as error:
        errors.append(error)
    if errors:
        raise ExceptionGroup('Lifetime fixture cleanup failed', errors)


def case(fixture, evidence, policy, check):
    target = evidence / check / policy
    target.mkdir(parents=True)
    for name in ('driver', 'alloy_guest_host'):
        shutil.copy2(fixture / name, target / name)
    if check.startswith('registration-'):
        shutil.copy2(fixture / 'alloy_registry_probe', target / 'alloy_registry_probe')
    command = [str(target / 'driver'), policy, check]
    if check == 'pin-reuse':
        shutil.copy2(fixture / 'alloy_pin_probe', target / 'alloy_pin_probe')
        command = [str(target / 'alloy_pin_probe'), *command]
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, bufsize=1)
    handles = []
    try:
        launched = line(process)
        if check == 'kill-refused-probe':
            assert launched.startswith('REFUSAL_ORIGINAL '), 'Original enclosure not retained'
            handles.append(pinned(int(launched.split()[-1])))
            process.stdin.write('G')
            process.stdin.flush()
            launched = line(process)
        if check.startswith('custody-'):
            if launched.startswith('CUSTODY_STOPPED '):
                owner = int(launched.split()[-1])
                handles.append(pinned(owner))
                children = descendants(owner)
                deadline = time.monotonic() + 10
                while not any(status(pid)['State'].strip().startswith('T')
                              for pid in [owner, *children]):
                    assert time.monotonic() < deadline, 'No confirmed stopped original'
                    time.sleep(.01)
                handles.extend(pinned(pid) for pid in children)
                process.stdin.write('G')
                process.stdin.flush()
                launched = line(process, 15)
            assert launched == 'CUSTODY_OWNER_PASS', launched
            assert process.wait(timeout=10) == 0
            assert all(exited(fd) for fd in handles)
            return dict(policy=policy, check=check, passed=True)
        if check == 'receive-fork':
            assert launched == 'RECEIVE_FORK_CLOSED APP_SIGNALS_UNCHANGED', launched
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True)
        if check == 'fd-pressure':
            assert launched == 'FD_PRESSURE_ACCOUNTED APP_SIGNALS_UNCHANGED', launched
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True, attempts=16)
        if check == 'pin-reuse':
            assert launched.startswith('LAUNCHED 1 OWNER '), launched
            process.stdin.write('K')
            process.stdin.flush()
            observed = line(process)
            assert observed == 'REAPED APP_SIGNALS_UNCHANGED', observed
            observed = line(process)
            assert observed == 'PIN_IDENTITY_ATOMIC', observed
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True)
        if check == 'startup-cancel':
            assert launched.startswith('CREATED '), launched
            owner = int(launched.split()[-1])
            handles.append(pinned(owner))
            deadline = time.monotonic() + 10
            stopped = []
            while time.monotonic() < deadline:
                children = descendants(owner)
                stopped = [pid for pid in children if status(pid)['State'].strip().startswith('T')]
                if stopped:
                    break
                time.sleep(.01)
            assert stopped, 'Startup cancellation did not observe an actually stopped probe'
            handles.extend(pinned(pid) for pid in children)
            process.stdin.write('K')
            process.stdin.flush()
            assert all(exited(fd) for fd in handles), 'A startup original survived cancellation'
            assert line(process) == 'CANCELLED_CLOSED APP_SIGNALS_UNCHANGED'
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True, confirmed_stopped=stopped)
        if check in ('clone-enosys', 'waitid-denied', 'kill-denied', 'kill-refused-probe',
                     'socket-denied', 'mask-restore-denied'):
            expected = {'clone-enosys': 'clone3(', 'waitid-denied': 'waitid(P_PIDFD)',
                        'kill-denied': 'pidfd_send_signal(SIGKILL)',
                        'kill-refused-probe': 'pidfd_send_signal(SIGKILL)',
                        'socket-denied': 'socketpair(',
                        'mask-restore-denied': 'sigprocmask(parent signal restore)'}[check]
            assert launched.startswith('REFUSED ') and expected in launched, launched
            assert process.wait(timeout=10) == 0
            if check == 'kill-refused-probe':
                assert all(exited(fd) for fd in handles), 'Original enclosure survived refusal'
            assert not descendants(process.pid)
            return dict(policy=policy, check=check, passed=True, reason=launched)
        assert launched.startswith('LAUNCHED 1 OWNER '), launched
        owner = int(launched.split()[-1])
        if check == 'kill-refused-retire':
            handles.append(pinned(owner))
            children = descendants(owner)
            assert len(children) == 1, children
            handles.extend(pinned(pid) for pid in children)
            signal.pidfd_send_signal(handles[1], signal.SIGSTOP)
            deadline = time.monotonic() + 10
            while not status(children[0])['State'].strip().startswith('T'):
                assert time.monotonic() < deadline
                time.sleep(.01)
            process.stdin.write('C')
            process.stdin.flush()
            assert line(process) == 'FAILED_CLEANUP APP_SIGNALS_UNCHANGED'
            assert process.wait(timeout=10) == 0
            assert all(exited(fd) for fd in handles)
            return dict(policy=policy, check=check, passed=True)
        if check.startswith('registration-'):
            observed = line(process)
            assert observed == 'REGISTRY_PASS APP_SIGNALS_UNCHANGED', observed
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True)
        if check.startswith('fault-'):
            if check.startswith('fault-inner-'):
                observed = line(process)
                assert observed == 'FAULT_OBSERVED', observed
            process.stdin.write('C')
            process.stdin.flush()
            observed = line(process)
            assert observed == 'FAILED_CLEANUP APP_SIGNALS_UNCHANGED', observed
            assert process.wait(timeout=10) == 0
            return dict(policy=policy, check=check, passed=True)
        if check == 'eintr-registration':
            observed = line(process)
            assert observed == 'FAULT_OBSERVED', observed
        handles.append(pinned(owner))
        deadline = time.monotonic() + 10
        children = []
        ready = False
        while time.monotonic() < deadline:
            children = descendants(owner)
            ready = any(status(pid)['Name'].strip() == 'alloy-lifetime' for pid in children)
            if ready and (check not in ('late', 'inner-lost', 'stopped-creator', 'app-dead') or any(
                    status(pid)['State'].strip().startswith('T') for pid in children)):
                break
            time.sleep(.01)
        assert children and ready, 'No admitted fixture entry'
        if check in ('late', 'inner-lost', 'stopped-creator', 'app-dead'):
            assert any(status(pid)['State'].strip().startswith('T') for pid in children), \
                'No confirmed stopped unregistered child'
        handles.extend(pinned(pid) for pid in children)
        if check == 'orphan-reap':
            orphan = next(pid for pid in children if status(pid)['Name'].strip() == 'alloy-orphan')
            handle = handles[[owner, *children].index(orphan)]
            deadline = time.monotonic() + 10
            while not status(orphan)['State'].strip().startswith('T'):
                assert time.monotonic() < deadline
                time.sleep(.01)
            signal.pidfd_send_signal(handle, signal.SIGCONT)
            assert exited(handle), 'The released orphan did not exit'
            while (Path('/proc') / str(orphan)).exists():
                assert time.monotonic() < deadline, 'Init retained an orphan zombie during ACTIVE'
                time.sleep(.01)
        if check == 'signals':
            for pid in [owner, *children]:
                row = status(pid)
                assert int(row['SigBlk'], 16) == 0, (pid, row)
                assert int(row['SigIgn'], 16) == 0, (pid, row)
        if check == 'enclosure':
            nspid = status(owner)['NSpid'].split()
            assert len(nspid) >= 2 and nspid[-1] == '1', nspid
        if check == 'app-dead':
            process.kill()
            assert process.wait(timeout=10) == -signal.SIGKILL
            assert all(exited(fd) for fd in handles), 'An enclosure original survived app death'
            return dict(policy=policy, check=check, passed=True)
        if check in ('init-stopped', 'stopped-creator', 'inner-lost'):
            inner = next((pid for pid in children if b'--alloy-supervisor' in
                          (Path('/proc') / str(pid) / 'cmdline').read_bytes().split(b'\0')), owner)
            target_pid = owner if check == 'init-stopped' else inner
            target_fd = handles[[owner, *children].index(target_pid)]
            signal.pidfd_send_signal(target_fd, signal.SIGKILL if check == 'inner-lost' else signal.SIGSTOP)
            if check != 'inner-lost':
                deadline = time.monotonic() + 10
                while not status(target_pid)['State'].strip().startswith('T'):
                    assert time.monotonic() < deadline, 'Stop was not observed'
                    time.sleep(.01)
        command = 'F' if check == 'fork-control' else 'C' if check in ('inner-lost', 'eintr-registration') else 'K'
        process.stdin.write(command)
        process.stdin.flush()
        if check == 'fork-control':
            copy = line(process)
            assert copy.startswith('COPY '), copy
            handles.append(pinned(int(copy.split()[-1])))
        else:
            assert all(exited(fd) for fd in handles), 'A guest original survived init death'
        assert line(process) == 'REAPED APP_SIGNALS_UNCHANGED'
        assert process.wait(timeout=10) == 0
        if check == 'fork-control':
            assert not exited(handles[-1], 0), 'Guest retirement killed the unrelated app fork'
        return dict(policy=policy, check=check, passed=True, launched=launched)
    finally:
        # Fixture deadlines fail tests, never product timeout abandonment. Kill
        # retained exact originals, including the intentionally copied app fork.
        cleanup_driver(process, handles, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--check', choices=('all', *CHECKS), default='all')
    parser.add_argument('--fixture', type=Path, help='Previously built baseline fixture for red reruns')
    parser.add_argument('--source-root', type=Path, help='Baseline source overlay for red-first fixture builds')
    args = parser.parse_args()
    validate_workspace(WORK, REPO)
    args.evidence.mkdir(parents=True)
    fixture = args.fixture or args.evidence / 'fixture'
    if args.fixture is None:
        compile_fixture(fixture, args.source_root)
    rows = []
    for check in (CHECKS if args.check == 'all' else (args.check,)):
        for policy in POLICIES:
            try:
                rows.append(case(fixture, args.evidence, policy, check))
            except Exception as error:
                rows.append(dict(policy=policy, check=check, passed=False,
                                 error=f'{type(error).__name__}: {error}'))
    (args.evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(json.dumps(rows, indent=2), flush=True)
    assert all(row['passed'] for row in rows), 'Lifetime regression failed'
    print('GUEST_LIFETIME_PASS', flush=True)


if __name__ == '__main__':
    main()
