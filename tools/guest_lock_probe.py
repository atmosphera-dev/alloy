#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Consumer acceptance of the userns-only Linux guest write boundary."""
import argparse
import array
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import subprocess
import time

from workspace import validate_output, workspace_path
from guest_test_support.fixtures import Fixtures
from guest_test_support.session import Client

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, text=True, **kwargs)


def snapshot(pid):
    # Observations require root for nondumpable sandbox entries. The application
    # and browser themselves always run unprivileged, with no ptrace relaxation.
    code = r'''
import json,os,pathlib,sys
pending=[int(sys.argv[1])]; seen=set(); rows=[]
while pending:
    pid=pending.pop()
    if pid in seen: continue
    seen.add(pid); p=pathlib.Path('/proc')/str(pid)
    try:
        for task in (p/'task').iterdir():
            pending.extend(map(int,(task/'children').read_text().split()))
        exe=str((p/'exe').resolve(strict=True))
        if pathlib.Path(exe).name not in ('alloy_guest_host','alloy_guest_engine'): continue
        argv=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode()
        status=dict(line.split(':',1) for line in (p/'status').read_text().splitlines())
        fds={}
        for fd in (p/'fd').iterdir():
            try: fds[fd.name]=str(fd.readlink())
            except FileNotFoundError: pass
        rows.append(dict(pid=pid,exe=exe,argv=argv,status={k:v.strip() for k,v in status.items()},
            fds=fds,maps=(p/'maps').read_text(),oom_score_adj=int((p/'oom_score_adj').read_text()),
            namespaces={n:str((p/'ns'/n).readlink()) for n in ('user','pid','net')},
            apparmor=(p/'attr/current').read_text().strip()))
    except (FileNotFoundError,ProcessLookupError): pass
print(json.dumps(rows))
'''
    return json.loads(run(['sudo', '-n', 'python3', '-c', code, pid], capture_output=True).stdout)


class Members:
    def __init__(self, pid):
        self.rows = snapshot(pid)
        self.handles = []
        for row in self.rows:
            try:
                self.handles.append((row, os.pidfd_open(row['pid'])))
            except ProcessLookupError:
                pass

    def kill(self, role):
        targets = [(r, fd) for r, fd in self.handles if role_of(r) == role]
        assert targets, (role, self.rows)
        for _, fd in targets:
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL)
            except PermissionError:
                # Transfer the already-pinned identity, never a numeric PID,
                # to the dedicated guest's root fixture for the test account.
                channel, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                code = ('import array,signal,socket; s=socket.socket(fileno=0); '
                        'm,c,f,a=s.recvmsg(1,socket.CMSG_SPACE(4)); '
                        'v=array.array("i"); v.frombytes(c[0][2]); '
                        'signal.pidfd_send_signal(v[0],signal.SIGKILL)')
                helper = subprocess.Popen(['sudo', '-n', 'python3', '-c', code], stdin=child)
                child.close()
                channel.sendmsg([b'K'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                channel.close()
                assert helper.wait(timeout=15) == 0
        return [r['pid'] for r, _ in targets]

    def exited(self):
        deadline = time.monotonic() + 40
        for row, fd in self.handles:
            poll = select.poll()
            poll.register(fd, select.POLLIN)
            assert poll.poll(max(0, int((deadline - time.monotonic()) * 1000))), row

    def close(self):
        for _, fd in self.handles:
            os.close(fd)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def role_of(row):
    if '--alloy-supervisor' in row['argv']:
        return 'supervisor'
    for field in row['argv'].split():
        if field.startswith('--type='):
            return field.removeprefix('--type=')
    return 'browser'


def scratch_of(rows):
    roots = {(r['pid'], int(fd), target) for r in rows if role_of(r) == 'supervisor'
             for fd, target in r['fds'].items() if target.startswith('/dev/shm/alloy-guest-')}
    assert len(roots) == 1, roots
    return roots.pop()


def assert_no_runtime_entries(out):
    # This task machine runs only this driver's sessions. A process orphaned out
    # of the app's ancestry must not evade the late-admission/close observation.
    code = r'''
import json,pathlib,sys
allowed={str(pathlib.Path(sys.argv[1])/n) for n in ('alloy_guest_host','alloy_guest_engine')}
rows=[]
for p in pathlib.Path('/proc').iterdir():
    if not p.name.isdecimal(): continue
    try:
        exe=str((p/'exe').resolve(strict=True))
        if exe in allowed: rows.append(dict(pid=int(p.name),exe=exe))
    except (FileNotFoundError,ProcessLookupError): pass
print(json.dumps(rows))
'''
    rows = json.loads(run(['sudo', '-n', 'python3', '-c', code, out], capture_output=True).stdout)
    assert not rows, rows


def protected(report):
    assert report['filesystem'] == 'data-writes-denied', report
    assert report['scratch'] == 'verified-tmpfs', report
    native = json.loads(report['sandbox'])
    assert native['route'] == 'userns' and native['renderer']['seccomp'] == 2, native
    assert native['renderer']['no_new_privs'], native
    assert native['renderer']['effective_capabilities_decimal'] == '0', native
    assert native['setuid_helper'] == 'never-used-in-guest-mode', native
    limits = json.loads(report['limits'])
    assert 'not-isolated-from-other-same-user-processes' in limits['process_isolation'], limits
    assert 'no truncate' in limits['procfs_exception'], limits
    return native


def inventory(roots):
    # No traversal exclusions. Do not follow symlinks into unrelated trees.
    # Errors fail the inventory rather than masquerading as an empty directory.
    result = {}
    def visit(path):
        info = path.lstat()
        value = [stat.S_IFMT(info.st_mode), stat.S_IMODE(info.st_mode), info.st_uid,
                 info.st_gid, info.st_size, info.st_mtime_ns]
        if stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            value.append(digest.hexdigest())
        elif stat.S_ISLNK(info.st_mode):
            value.append(str(path.readlink()))
        result[str(path)] = value
        if stat.S_ISDIR(info.st_mode):
            for child in sorted(path.iterdir()):
                visit(child)
    for root in roots:
        visit(root)
    return result


def root_inventory(roots):
    # Reuse exactly the same inventory function under sudo for the disposable
    # account's mode-0700 home and system temporary roots.
    import inspect
    code = ('import hashlib,json,stat\nfrom pathlib import Path\n' +
            inspect.getsource(inventory) + '\nprint(json.dumps(inventory([Path(p) for p in ' +
            repr(list(map(str, roots))) + '])))')
    return json.loads(run(['sudo', '-n', 'python3', '-c', code], capture_output=True).stdout)


def inventory_equal(before, after):
    changed = {p: {'before': before.get(p), 'after': after.get(p)}
               for p in before.keys() | after.keys() if before.get(p) != after.get(p)}
    assert not changed, changed


@contextmanager
def disposable_account(evidence):
    name = 'alloym2-' + str(os.getpid())
    home = Path('/home') / name
    run(['sudo', '-n', 'useradd', '--create-home', '--home-dir', home,
         '--shell', '/usr/sbin/nologin', name])
    try:
        # The cloud image's builder home is 0750. Permit only this disposable
        # account to traverse that one ancestor, without joining the builder
        # group or making its private home globally readable/traversable.
        run(['sudo', '-n', 'setfacl', '-m', 'u:' + name + ':x', '/home/builder'])
        yield name, home
    finally:
        try:
            abort_account(name)
            run(['sudo', '-n', 'setfacl', '-x', 'u:' + name, '/home/builder'])
        finally:
            run(['sudo', '-n', 'userdel', '--remove', name])


def abort_account(name, launcher=None):
    # The named UID was freshly created by this context and has no other work.
    # Pin before inspecting credentials, and reject an already-exited identity
    # so PID reuse cannot redirect the root fixture's signal.
    code = r'''
import os,pathlib,pwd,select,signal,sys
uid=pwd.getpwnam(sys.argv[1]).pw_uid
for p in pathlib.Path('/proc').iterdir():
    if not p.name.isdecimal(): continue
    fd=-1
    try:
        fd=os.pidfd_open(int(p.name))
        owned=p.stat().st_uid==uid
        poll=select.poll(); poll.register(fd,select.POLLIN)
        if owned and not poll.poll(0): signal.pidfd_send_signal(fd,signal.SIGKILL)
    except (FileNotFoundError,ProcessLookupError): pass
    finally:
        if fd>=0: os.close(fd)
if int(sys.argv[2]):
    try:
        fd=os.pidfd_open(int(sys.argv[2]))
        signal.pidfd_send_signal(fd,signal.SIGKILL)
        os.close(fd)
    except ProcessLookupError: pass
'''
    # An unreaped direct Popen child cannot have its number recycled. This is
    # only our sudo launcher, never a guest-supplied process identifier.
    pid = launcher.pid if launcher is not None and launcher.poll() is None else 0
    run(['sudo', '-n', 'python3', '-c', code, name, pid])


def native_roles(rows, sentinel=None):
    engines = [r for r in rows if Path(r['exe']).name == 'alloy_guest_engine']
    assert {'browser', 'zygote', 'renderer', 'gpu-process', 'utility'} <= {role_of(r) for r in engines}, rows
    browser = next(r for r in engines if role_of(r) == 'browser')
    for row in engines:
        assert row['status']['NoNewPrivs'] == '1', row
        caps = int(row['status']['CapEff'], 16)
        if role_of(row) == 'zygote' and row['namespaces']['user'] != browser['namespaces']['user']:
            # Exact-pin SandboxLinux::EngageNamespaceSandbox retains only
            # namespace-local CAP_SYS_ADMIN for each child's PID namespace.
            assert caps & ~(1 << 21) == 0, row
        else:
            assert caps == 0, row
        assert all(not (target.startswith('/dev/shm/alloy-guest-') and
                        '/' not in target.removeprefix('/dev/shm/'))
                   for target in row['fds'].values()), row
        for fd in ('0', '1', '2'):
            assert row['fds'].get(fd) == '/dev/null', row
        if sentinel:
            assert str(sentinel) not in row['fds'].values(), row
            assert str(sentinel) not in row['maps'], row
        if role_of(row) == 'renderer':
            assert row['status']['Seccomp'] == '2', row
            assert row['namespaces']['user'] != browser['namespaces']['user'], row
            assert row['namespaces']['pid'] != browser['namespaces']['pid'], row
    return [{k: v for k, v in row.items() if k != 'maps'} for row in rows]


def defaults(out, evidence, fixtures):
    results = []
    with disposable_account(evidence) as (name, home):
        # Only the synthetic public CA is an input, prepared before inventory.
        certificate = home / 'fixture-ca.pem'
        run(['sudo', '-n', 'install', '-m', '0644', fixtures.certificate, certificate])
        outside = evidence / 'external-xdg'
        evidence.chmod(0o755)  # Permit only traversal to this owned fixture input.
        outside.mkdir(mode=0o777)
        run(['sudo', '-n', 'chown', name, outside])
        alias = home / 'xdg-alias'
        run(['sudo', '-n', '-u', name, 'ln', '-s', outside, alias])
        run(['sudo', '-n', '-u', name, 'test', '-w', alias])
        roots = [home, outside, Path('/tmp'), Path('/var/tmp')]
        baseline = root_inventory(roots)
        for death in ('close', 'browser', 'xdg-alias'):
            prefix = ['sudo', '-n', '-u', name, 'env', '-i', 'PATH=/usr/bin:/bin',
                      'LANG=C.UTF-8', 'HOME=' + str(home), 'LD_LIBRARY_PATH=' + str(out)]
            if death == 'xdg-alias':
                prefix += [key + '=' + str(alias) for key in
                           ('XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME',
                            'XDG_STATE_HOME', 'XDG_RUNTIME_DIR')]
            with Client(out, evidence / ('defaults-' + death), launch_prefix=prefix,
                        launch_environment=os.environ,
                        abort_callback=lambda process: abort_account(name, process)) as client:
                report = client.start(policy='manual', required='data-writes-denied')
                protected(report)
                rows = snapshot(client.process.pid)
                _, _, scratch = scratch_of(rows)
                for nss in ('home/.pki/nssdb', 'data/pki/nssdb'):
                    db = Path(scratch) / nss
                    exists = subprocess.run(['sudo', '-n', 'test', '-f', str(db / 'cert9.db')]).returncode
                    assert exists in (0, 1), exists
                    if exists:
                        run(['sudo', '-n', '-u', name, 'certutil', '-N', '--empty-password', '-d', 'sql:' + str(db)])
                    run(['sudo', '-n', '-u', name, 'certutil', '-A', '-d', 'sql:' + str(db),
                         '-n', 'Alloy M2 synthetic CA', '-t', 'C,,', '-i', certificate])
                client.send('accept guest')
                client.active()
                page = client.page(fixtures.base + '/smoke', 'Internet - Alloy smoke fixture')
                # HTTPS success is observed by page load, not just trust input.
                client.wait('guest', 'load', lambda v: v['view'] == page['view'] and v['status'] == 200)
                inventory_equal(baseline, root_inventory(roots))
                with Members(client.process.pid) as members:
                    roles = native_roles(members.rows)
                    if death != 'browser':
                        client.send('close guest')
                        client.closed()
                    else:
                        members.kill('browser')
                        client.closed(reason='failed')
                    members.exited()
                client.finish()
                assert_no_runtime_entries(out)
                inventory_equal(baseline, root_inventory(roots))
                assert not Path(scratch).exists(), scratch
                results.append(dict(case='real-default-' + death, page=page, report=report,
                                    processes=roles, unchanged_paths=len(baseline)))
        (evidence / 'real-default-inventory.json').write_text(json.dumps(baseline, indent=2) + '\n')
    return results


def lifecycle(out, evidence, fixtures):
    rows = []
    sentinel = evidence / 'inherited-disk-sentinel'
    sentinel.write_bytes(b'UNCHANGED-DURABLE-SENTINEL')
    env = dict(os.environ, ALLOY_FIXTURE_DISK_CAPABILITY=str(sentinel))
    for death in ('close', 'browser', 'supervisor', 'app', 'renderer', 'gpu-process', 'scratch'):
        with Client(out, evidence / ('lifetime-' + death), launch_environment=env) as client:
            client.wait('driver', 'inherited-capability')
            report = client.start(required='data-writes-denied')
            protected(report)
            client.active()
            page = client.page(fixtures.base + '/title/LOCK_BEFORE', 'LOCK_BEFORE')
            with Members(client.process.pid) as members:
                roles = native_roles(members.rows, sentinel)
                _, _, scratch = scratch_of(members.rows)
                if death == 'app':
                    client.abort()
                elif death == 'close':
                    # Admission and Close race on the supervisor sequence.
                    client.send('create guest ' + fixtures.base + '/title/LATE_CHILD\nclose guest')
                    client.closed()
                elif death == 'scratch':
                    Path(scratch).chmod(0o500)
                    client.closed(reason='failed')
                else:
                    members.kill(death)
                    if death in ('browser', 'supervisor'):
                        client.closed(reason='failed')
                    else:
                        time.sleep(1)
                        client.send(f'navigate guest {page["view"]} {fixtures.base}/title/LOCK_RECOVERED')
                        client.wait('guest', 'title', lambda v: v['title'] == 'LOCK_RECOVERED')
                        native_roles(snapshot(client.process.pid), sentinel)
                        client.send('close guest')
                        client.closed()
                members.exited()
            if death != 'app':
                client.finish()
            assert_no_runtime_entries(out)
            assert sentinel.read_bytes() == b'UNCHANGED-DURABLE-SENTINEL'
            if death == 'supervisor':
                # Documented volatile remnant, never a product disk fallback.
                if Path(scratch).exists():
                    import shutil
                    shutil.rmtree(scratch)
            else:
                assert not Path(scratch).exists(), scratch
            rows.append(dict(case='lifetime-' + death, report=report, processes=roles))
    return rows


def mode_and_app(out, evidence, fixtures):
    rows = []
    with Client(out, evidence / 'encrypted-app', encrypted_app=True) as client:
        client.wait('app', 'initialized')
        client.send('app ' + fixtures.base + '/set-cookie')
        client.wait('app', 'title', lambda v: v['title'] == 'COOKIE_SET')
        first = client.start(name='memory', required='data-writes-denied')
        protected(first)
        client.active('memory')
        client.page(fixtures.base + '/set-cookie', 'COOKIE_SET', 'memory')
        client.page(fixtures.base + '/cookie', 'COOKIE_PRESENT', 'memory')
        second = client.start(name='stateless', mode='stateless', required='data-writes-denied')
        protected(second)
        client.active('stateless')
        client.page(fixtures.base + '/set-cookie', 'COOKIE_REFUSED', 'stateless')
        client.send('close stateless')
        client.closed('stateless')
        client.page(fixtures.base + '/cookie', 'COOKIE_PRESENT', 'memory')
        client.send('close memory')
        client.closed('memory')
        client.send('app ' + fixtures.base + '/cookie')
        client.wait('app', 'title', lambda v: v['title'] == 'COOKIE_PRESENT')
        client.send('app-lock')
        client.wait('app', 'locked', lambda v: v['ok'])
        client.finish()
        profile = client.root / 'app-profile-encrypted'
        assert profile.is_dir() and any(profile.iterdir())
        for path in profile.rglob('*'):
            if path.is_file():
                assert b'alloy_session' not in path.read_bytes(), path
        rows.append(dict(case='memory-stateless-concurrent-encrypted-app-and-lock',
                         memory=first, stateless=second))
    return rows


def pressure(out, evidence, fixtures):
    # A private mount namespace bounds the pressure fixture to a new 128 MiB
    # tmpfs. Never fill the task VM's shared /dev/shm or its disk workspace.
    script = ('mount --make-rprivate /\n'
              'mount -t tmpfs -o size=128m,mode=1777 tmpfs /dev/shm\n'
              'exec sudo -n -u builder env -i PATH=/usr/bin:/bin HOME=/home/builder '
              'TMPDIR=/home/builder/work/tmp LD_LIBRARY_PATH="' + str(out) + '" "$@"')
    prefix = ['sudo', '-n', 'unshare', '--mount', '--fork', 'bash', '-euc', script, '--']
    with Client(out, evidence / 'pressure', launch_prefix=prefix) as client:
        report = client.start(required='data-writes-denied')
        protected(report)
        client.active()
        client.page(fixtures.base + '/title/BEFORE_PRESSURE', 'BEFORE_PRESSURE')
        with Members(client.process.pid) as members:
            pid, fd, scratch = scratch_of(members.rows)
            code = r'''
import errno,json,os,sys
p='/proc/'+sys.argv[1]+'/fd/'+sys.argv[2]
assert os.stat(p).st_dev != os.stat('/dev/shm').st_dev
assert os.statvfs(p).f_blocks*os.statvfs(p).f_frsize <= 128*1024*1024
fd=os.open(p+'/pressure-fixture',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
total=0
try:
    while True: total+=os.write(fd,b'X'*(1024*1024))
except OSError as e:
    assert e.errno==errno.ENOSPC,e
finally: os.close(fd)
print(json.dumps(dict(bytes=total,error='ENOSPC')))
'''
            filled = json.loads(run(['sudo', '-n', 'python3', '-c', code, pid, fd], capture_output=True).stdout)
            client.closed(reason='failed')
            members.exited()
        client.finish()
    return [dict(case='private-tmpfs-ENOSPC-ends-session-without-disk-fallback', report=report, fill=filled)]


def native_writers(out, evidence, fixtures):
    executable = evidence / 'native-writers'
    run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'src',
         REPO / 'tools/testdata/guest-lock/native-writers.c',
         *[REPO / 'src/brave/alloy/guest/linux_lock' / (leaf + '.c')
           for leaf in ('ram_scratch', 'writer_paths', 'landlock')], '-o', executable])
    log = evidence / 'native-writers.log'
    bus_log = evidence / 'native-bus-double.log'
    address = 'unix:path=' + str(evidence / 'native-bus.sock')
    with (evidence / 'native-bus-daemon.log').open('x') as daemon_log, bus_log.open('x') as monitor_log:
        daemon = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--nopidfile',
            '--print-address=1', '--address=' + address], stdout=subprocess.PIPE, stderr=daemon_log, text=True)
        monitor = None
        try:
            assert daemon.stdout.readline().strip().startswith(address)
            monitor = subprocess.Popen(['stdbuf', '-oL', 'dbus-monitor', '--address', address,
                "type='method_call'"], stdout=monitor_log, stderr=daemon_log)
            time.sleep(0.5)
            assert monitor.poll() is None
            run(['dbus-send', '--bus=' + address, '--dest=org.freedesktop.DBus', '--print-reply',
                 '/org/freedesktop/DBus', 'org.freedesktop.DBus.ListNames'], capture_output=True)
            time.sleep(0.5)
            before = bus_log.read_text().count('method call ')
            assert before > 0, 'Positive service-double control was not observed'
            env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address)
            with log.open('x') as stream:
                run([executable, '/usr/bin/python3', REPO / 'tools/testdata/guest-lock/native_writers.py'],
                    env=env, stdout=stream, stderr=subprocess.STDOUT)
            with Client(out, evidence / 'native-bus-client', launch_environment=env,
                        trusted_certificate=fixtures.certificate) as client:
                report = client.start(required='data-writes-denied')
                protected(report)
                client.active()
                client.page(fixtures.base + '/smoke', 'Internet - Alloy smoke fixture')
                client.send('close guest')
                client.closed()
                client.finish()
            time.sleep(0.5)
            assert bus_log.read_text().count('method call ') == before, bus_log
        finally:
            if monitor is not None:
                monitor.terminate()
                monitor.wait(timeout=15)
            daemon.terminate()
            daemon.wait(timeout=15)
            daemon.stdout.close()
    marker = [line.removeprefix('GUEST_NATIVE_WRITERS ') for line in log.read_text().splitlines()
              if line.startswith('GUEST_NATIVE_WRITERS ')]
    assert len(marker) == 1, log
    return [dict(case='native-fontconfig-NSS-memory-GSettings-and-no-ambient-bus-calls',
                 consumer=json.loads(marker[0]), bus_positive_controls=before, guest_bus_calls=0)]


def package_proof(out, evidence, fixtures):
    os_release = Path('/etc/os-release').read_text()
    assert 'VERSION_ID="24.04"' in os_release, os_release
    restricted = Path('/proc/sys/kernel/apparmor_restrict_unprivileged_userns').read_text().strip()
    assert restricted == '1', restricted
    name = 'alloy-gm-m2-fixture'
    install = Path('/opt') / name / (out.name + '-' + evidence.name)
    stage = evidence / 'package-root'
    target = stage / install.relative_to('/')
    target.mkdir(parents=True)
    for pattern in ('*.so', '*.so.*', '*.pak', '*.dat', '*.bin', '*.json', 'locales',
                    'alloy_guest_host', 'alloy_guest_engine', 'alloy_guest_sdk_consumer'):
        for source in out.glob(pattern):
            assert not source.is_symlink(), source
            run(['cp', '-a', '--reflink=auto', source, target])
    metadata = stage / 'DEBIAN'
    metadata.mkdir()
    (metadata / 'control').write_text('Package: ' + name + '\nVersion: 0.0.1\nArchitecture: amd64\n'
        'Maintainer: Alloy fixture <fixture@invalid>\nDescription: Disposable M2 acceptance fixture\n')
    deb = evidence / (name + '.deb')
    run(['dpkg-deb', '-Zgzip', '-z1', '--root-owner-group', '--build', stage, deb])
    policy = Path('/etc/apparmor.d') / name
    loaded = False
    try:
        run(['sudo', '-n', 'dpkg', '-i', deb])
        assert not (install / 'chrome-sandbox').exists()
        with Client(install, evidence / 'package-unprofiled') as client:
            report = client.start(policy='manual', required='data-writes-denied')
            unprofiled_report = report
            assert report['filesystem'] == 'unavailable', report
            assert 'userns-unavailable:unshare-errno-' in report['limits'], report
            assert not any(role_of(r) == 'renderer' for r in snapshot(client.process.pid))
            client.send('create guest ' + fixtures.base + '/title/NEVER_ADMITTED')
            assert client.wait('guest', 'create-queued')['ok'] is False
            client.send('close guest')
            client.closed()
            client.finish()
        template = (REPO / 'packaging/linux/alloy-guest.apparmor.in').read_text()
        template = template.replace('@@PROFILE@@', name).replace('@@BOOTSTRAP@@', str(install / 'alloy_guest_host'))
        template = template.replace('@@ENGINE@@', str(install / 'alloy_guest_engine'))
        packaged_policy = stage / policy.relative_to('/')
        packaged_policy.parent.mkdir(parents=True)
        packaged_policy.write_text(template)
        for script, operation in (('postinst', '-r'), ('prerm', '-R')):
            path = metadata / script
            path.write_text('#!/bin/sh\nset -e\nif [ -d /sys/module/apparmor ]; then\n'
                            f'  apparmor_parser {operation} {policy}\nfi\n')
            path.chmod(0o755)
        profiled = evidence / (name + '-profiled.deb')
        run(['dpkg-deb', '-Zgzip', '-z1', '--root-owner-group', '--build', stage, profiled])
        run(['sudo', '-n', 'dpkg', '-i', profiled])
        loaded = True
        with Client(install, evidence / 'package-profiled') as client:
            report = client.start(required='data-writes-denied')
            protected(report)
            client.active()
            page = client.page(fixtures.base + '/title/APPARMOR_PACKAGE_PAGE', 'APPARMOR_PACKAGE_PAGE')
            roles = native_roles(snapshot(client.process.pid))
            assert any(name in r['apparmor'] for r in roles if role_of(r) == 'browser')
            client.send('close guest')
            client.closed()
            client.finish()
        return [dict(case='ubuntu-24.04-real-deb-unprofiled-refusal-profiled-page',
                     unprofiled_reason=unprofiled_report['limits'], unprofiled_report=unprofiled_report,
                     restricted_userns=restricted, page=page, report=report, processes=roles)]
    finally:
        # prerm unloads the exact package-owned policy before dpkg removes it.
        run(['sudo', '-n', 'dpkg', '--purge', name])
        if loaded:
            assert not policy.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('baseline', 'all', 'package'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(WORK, out)
    evidence = args.evidence.resolve()
    assert evidence.is_relative_to(WORK), evidence
    assert os.environ.get('TMPDIR', '').startswith('/home/builder/work/')
    assert os.getuid() != 0 and os.uname().nodename == 'alloy-gm-m2'
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    started = time.monotonic()
    result = dict(check=args.check, status='running', cases=[])
    try:
        with Fixtures(REPO, tls_root=evidence / 'tls') as tls:
            if args.check != 'package':
                result['cases'] += native_writers(out, evidence, tls)
                result['cases'] += defaults(out, evidence, tls)
        with Fixtures(REPO) as fixtures:
            if args.check != 'package':
                result['cases'] += lifecycle(out, evidence, fixtures)
                result['cases'] += mode_and_app(out, evidence, fixtures)
                result['cases'] += pressure(out, evidence, fixtures)
            if args.check in ('all', 'package'):
                result['cases'] += package_proof(out, evidence, fixtures)
        result['status'] = 'passed'
        print('GUEST_LOCK_PASS', flush=True)
    except BaseException as error:
        result.update(status='failed', error=repr(error))
        raise
    finally:
        result['seconds'] = time.monotonic() - started
        (evidence / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
