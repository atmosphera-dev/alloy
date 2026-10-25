#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only filesystem notifications, independent of dying-thread registers.

The privileged notification collector never changes access decisions. The caller
supplies the completed native process accounting for attribution after collection.
"""
import ctypes
import fcntl
import json
import os
from pathlib import Path
import select
import re
import socket
import struct
import subprocess
import sys
import time

WORK = Path('/home/builder/work/alloy')
FAN_CLOEXEC = 0x1
FAN_NONBLOCK = 0x2
FAN_REPORT_PIDFD = 0x80
FAN_REPORT_FID = 0x200
FAN_REPORT_DFID_NAME = 0xC00
FAN_MARK_ADD = 0x1
FAN_MARK_FILESYSTEM = 0x100
FAN_MODIFY = 0x2
FAN_ACCESS = 0x1
FAN_OPEN = 0x20
FAN_ATTRIB = 0x4
FAN_MOVED_TO = 0x80
FAN_CREATE = 0x100
FAN_DELETE = 0x200
FAN_Q_OVERFLOW = 0x4000
FAN_ONDIR = 0x40000000
WRITE_MASK = FAN_CREATE | FAN_MODIFY | FAN_MOVED_TO | FAN_DELETE | FAN_ATTRIB
METADATA = struct.Struct('=IBBHQii')
INFO = struct.Struct('=BBH')
# linux/pidfd.h, ABI verified by the NUC's native sizeof/ioctl probe.
PIDFD_GET_INFO = (3 << 30) | (88 << 16) | (0xFF << 8) | 11
PIDFD_INFO_CGROUPID = 1 << 2
PIDFD_INFO_EXIT = 1 << 3
# Kernel/device interfaces, not ordinary browser-storage filesystems. These are
# reported in coverage, not silently treated as successfully marked data mounts.
KERNEL_INTERFACES = frozenset(('proc', 'sysfs', 'devpts', 'cgroup2', 'securityfs',
    'pstore', 'efivarfs', 'bpf', 'autofs', 'tracefs', 'configfs', 'debugfs',
    'mqueue', 'fusectl', 'binfmt_misc'))


def boundary():
    assert socket.gethostname() == 'wakaba-build'
    assert os.environ['TMPDIR'] == '/home/builder/work/tmp'


def process(pid):
    try:
        text = Path('/proc', str(pid), 'stat').read_text()
        fields = text[text.rindex(')')+2:].split()
        return dict(pid=pid, parent=int(fields[1]), start_ticks=int(fields[19]),
                    state=fields[0], name=text[text.index('(')+1:text.rindex(')')])
    except (FileNotFoundError, ProcessLookupError):
        return None


def process_snapshot():
    return {int(path.name): row for path in Path('/proc').iterdir()
            if path.name.isdigit() and (row := process(int(path.name))) is not None}


def pidfd_info(fd):
    data = bytearray(88)
    struct.pack_into('=Q', data, 0, PIDFD_INFO_EXIT)
    deadline = time.monotonic()+.05
    while True:
        try:
            fcntl.ioctl(fd, PIDFD_GET_INFO, data, True)
            break
        except ProcessLookupError:
            # The task can disappear between the kernel's live lookup and its
            # final identifier copy. Retry the same pinned pidfd for its cached
            # exit identity; never substitute a lookup of the numeric PID.
            if time.monotonic() >= deadline:
                raise
            time.sleep(0)
    mask, cgroup, pid, tgid, parent = struct.unpack_from('=QQIII', data)
    result = dict(mask=mask, pid=pid, tgid=tgid, parent=parent)
    if mask & PIDFD_INFO_CGROUPID:
        result['cgroup'] = cgroup
    if mask & PIDFD_INFO_EXIT:
        result['exit_code'] = struct.unpack_from('=i', data, 60)[0]
    return result


def ancestry(pid):
    chain = []
    seen = set()
    while pid > 0:
        assert pid not in seen, 'cyclic process ancestry'
        seen.add(pid)
        row = process(pid)
        if row is None:
            break
        chain.append(row)
        pid = row['parent']
    return chain


def mounts():
    rows = []
    text = Path('/proc/self/mountinfo').read_text()
    for line in text.splitlines():
        left, right = line.split(' - ', 1)
        fields, filesystem = left.split(), right.split()
        point = fields[4]
        for escaped, value in ((r'\040', ' '), (r'\011', '\t'),
                               (r'\012', '\n'), (r'\134', '\\')):
            point = point.replace(escaped, value)
        rows.append(dict(mount_id=int(fields[0]), device=fields[2], point=point,
            root=fields[3], options=fields[5], filesystem=filesystem[0]))
    return text, rows


def directory_guard(status):
    # Listing a directory may update atime. Permission/entry changes update
    # ctime/mtime; these are the independent no-writable-object precondition.
    return tuple(getattr(status, key) for key in ('st_dev', 'st_ino', 'st_mode',
        'st_uid', 'st_gid', 'st_nlink', 'st_size', 'st_mtime_ns', 'st_ctime_ns'))


def file_identity(path):
    """Obtain the kernel's opaque identity, without opening or decoding the file."""
    boundary()
    libc = ctypes.CDLL(None, use_errno=True)
    libc.name_to_handle_at.argtypes = [ctypes.c_int, ctypes.c_char_p,
                                      ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    libc.name_to_handle_at.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(8+128)
    struct.pack_into('=I', buffer, 0, 128)
    mount_id = ctypes.c_int()
    if libc.name_to_handle_at(-100, os.fsencode(path), buffer, ctypes.byref(mount_id), 0) < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(path))
    size, kind = struct.unpack_from('=Ii', buffer)
    assert 0 < size <= 128
    return dict(fsid=os.statvfs(path).f_fsid.to_bytes(8, sys.byteorder).hex(),
                handle_type=kind, handle=buffer.raw[8:8+size].hex())


def decode_events(data):
    """Strictly decode every kernel record; malformed input never means silence."""
    offset = 0
    result = []
    while offset < len(data):
        assert len(data)-offset >= METADATA.size, 'truncated fanotify metadata'
        length, version, reserved, metadata_len, mask, fd, pid = METADATA.unpack_from(data, offset)
        assert version == 3 and reserved == 0 and metadata_len == METADATA.size
        assert metadata_len <= length <= len(data)-offset
        assert fd == -1, ('unexpected event descriptor', fd)
        row = dict(pid=pid, mask=mask, handles=[], pidfd=None)
        cursor, end = offset+metadata_len, offset+length
        while cursor < end:
            assert end-cursor >= INFO.size
            kind, pad, size = INFO.unpack_from(data, cursor)
            assert pad == 0 and INFO.size <= size <= end-cursor
            if kind in (1, 2, 3):
                assert size >= 20, 'short filesystem identity'
                handle_bytes, handle_type = struct.unpack_from('=Ii', data, cursor+12)
                name_start = cursor+20+handle_bytes
                assert name_start <= cursor+size
                item = dict(type=kind, fsid=data[cursor+4:cursor+12].hex(),
                    handle_type=handle_type, handle=data[cursor+20:name_start].hex())
                if kind == 2:
                    name = data[name_start:cursor+size]
                    assert b'\0' in name, 'unterminated event name'
                    item['name'] = name.split(b'\0', 1)[0].decode('utf-8', errors='surrogateescape')
                row['handles'].append(item)
            elif kind == 4:
                assert size == 8 and row['pidfd'] is None
                row['pidfd'] = struct.unpack_from('=i', data, cursor+4)[0]
            else:
                raise AssertionError(('unsupported fanotify information', kind))
            cursor += size
        assert cursor == end
        if mask & FAN_Q_OVERFLOW:
            row['error'] = 'kernel notification queue overflow'
        else:
            assert mask & (WRITE_MASK | 0x1 | 0x20), ('unexpected event mask', mask)
            assert row['handles'], 'missing filesystem identity'
        result.append(row)
        offset = end
    assert offset == len(data)
    return result


def serve():
    boundary()
    assert os.geteuid() == 0
    configuration = json.loads(sys.stdin.buffer.readline())
    seeds = configuration['seeds']
    assert isinstance(seeds, list)
    assert all(Path(name).is_absolute() and Path(name).is_relative_to(WORK/'evidence')
               and Path(name).is_file() for name in seeds)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fanotify_init.argtypes = [ctypes.c_uint, ctypes.c_uint]
    libc.fanotify_init.restype = ctypes.c_int
    libc.fanotify_mark.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_uint64,
                                  ctypes.c_int, ctypes.c_char_p]
    libc.fanotify_mark.restype = ctypes.c_int
    fd = libc.fanotify_init(FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_PIDFD |
        FAN_REPORT_FID | FAN_REPORT_DFID_NAME, os.O_RDONLY | os.O_CLOEXEC)
    if fd < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    controller_fd = os.pidfd_open(configuration['controller_pid'])
    try:
        controller = pidfd_info(controller_fd)
        assert controller['mask'] & PIDFD_INFO_CGROUPID
    finally:
        os.close(controller_fd)
    before, coverage = mounts()
    baseline = process_snapshot()
    baseline_cgroups = set()
    for pid, row in baseline.items():
        try:
            identity_fd = os.pidfd_open(pid)
        except ProcessLookupError:
            continue
        try:
            try:
                row['kernel_identity'] = pidfd_info(identity_fd)
            except ProcessLookupError:
                continue
            if 'cgroup' in row['kernel_identity']:
                baseline_cgroups.add(row['kernel_identity']['cgroup'])
        finally:
            os.close(identity_fd)
    other_cgroups = sorted(baseline_cgroups-{controller['cgroup']})
    marks, seen = [], set()
    inaccessible = {}
    events = []
    try:
        for row in coverage:
            if row['filesystem'] in KERNEL_INTERFACES:
                row['coverage'] = 'kernel-interface'
                continue
            if row['filesystem'] == 'hugetlbfs':
                # This NUC's hugetlbfs has no export handles. It is empty and
                # root-owned, with no write permission for builder or its ACL
                # mask. Thus no writable object exists for the browser. Any
                # metadata/directory change invalidates this precondition.
                status = os.stat(row['point'])
                assert status.st_uid == status.st_gid == 0 and not status.st_mode & 0o022
                assert not list(Path(row['point']).iterdir())
                inaccessible[row['point']] = directory_guard(status)
                row['coverage'] = 'empty-root-with-no-builder-write-permission'
                continue
            identity = (row['device'], os.statvfs(row['point']).f_fsid)
            if identity not in seen:
                rc = libc.fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                    WRITE_MASK | FAN_ONDIR, -100, os.fsencode(row['point']))
                if rc < 0:
                    error = ctypes.get_errno()
                    raise OSError(error, os.strerror(error), row['point'])
                seen.add(identity)
                marks.append(dict(point=row['point'], device=row['device'], fsid=identity[1]))
            row['coverage'] = 'filesystem-mark'
        for name in seeds:
            rc = libc.fanotify_mark(fd, FAN_MARK_ADD, FAN_OPEN | FAN_ACCESS,
                                   -100, os.fsencode(name))
            if rc < 0:
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error), name)
        ready = dict(phase='ready', observer_pid=os.getpid(), marks=marks, coverage=coverage,
                     mountinfo=before, baseline=baseline, seeds=seeds,
                     controller_identity=controller, other_cgroups=other_cgroups)
        print(json.dumps(ready), flush=True)

        def drain():
            while True:
                try:
                    data = os.read(fd, 1024*1024)
                except BlockingIOError:
                    return
                assert data, 'unexpected end of notification queue'
                for row in decode_events(data):
                    pidfd = row.pop('pidfd')
                    if pidfd is not None and pidfd >= 0:
                        try:
                            status = os.fstat(pidfd)
                            row['pidfd_identity'] = [status.st_dev, status.st_ino]
                            try:
                                row['kernel_identity'] = pidfd_info(pidfd)
                            except ProcessLookupError:
                                row['kernel_identity_error'] = 'process exited during identity acquisition'
                            try:
                                current = os.pidfd_open(row['pid'])
                            except ProcessLookupError:
                                row['ancestry'] = []
                            else:
                                try:
                                    live = os.fstat(current)
                                    row['ancestry'] = ancestry(row['pid']) if (
                                        live.st_dev, live.st_ino) == (
                                        status.st_dev, status.st_ino) else []
                                finally:
                                    os.close(current)
                        finally:
                            os.close(pidfd)
                    else:
                        row['pidfd_error'] = pidfd
                        row['ancestry'] = []
                    row['observed_ns'] = time.monotonic_ns()
                    events.append(row)
                    assert len(events) <= 200000, 'observer event bound exceeded'

        while True:
            readable, _, _ = select.select([fd, sys.stdin.buffer], [], [], 30)
            if fd in readable:
                drain()
            if sys.stdin.buffer in readable:
                command = sys.stdin.buffer.readline()
                assert command == b'stop\n', 'observer controller disappeared or invalid stop'
                drain()
                break
        after, _ = mounts()
        assert before == after, 'mount coverage changed during observation'
        for point, previous in inaccessible.items():
            assert directory_guard(os.stat(point)) == previous and not list(Path(point).iterdir()), (
                'previously inaccessible filesystem changed', point)
    finally:
        os.close(fd)
    print(json.dumps(dict(phase='stopped', events=events, mountinfo_after=after)), flush=True)


class Observer:
    def __init__(self, script, evidence, seeds=()):
        boundary()
        assert os.geteuid() == 1000
        self.evidence = Path(evidence)
        self.errors = (self.evidence/'fanotify.stderr').open('xb')
        self.child = subprocess.Popen(['sudo', '-n', 'env',
            'TMPDIR=/home/builder/work/tmp', 'PYTHONDONTWRITEBYTECODE=1',
            sys.executable, str(script), '--serve'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.errors, text=True)
        try:
            self.child.stdin.write(json.dumps(dict(seeds=list(map(str, seeds)),
                controller_pid=os.getpid()))+'\n')
            self.child.stdin.flush()
            readable, _, _ = select.select([self.child.stdout], [], [], 20)
            assert readable, 'fanotify observer readiness deadline'
            line = self.child.stdout.readline()
            assert line, 'fanotify observer failed before marks were installed'
            self.ready = json.loads(line)
            assert self.ready['phase'] == 'ready'
            (self.evidence/'fanotify-ready.json').write_text(json.dumps(self.ready, indent=2)+'\n')
        except BaseException:
            self.child.stdin.close()
            self.child.wait(timeout=20)
            self.errors.close()
            raise

    def stop(self, browser_pids, root_pid):
        output, _ = self.child.communicate('stop\n', timeout=60)
        self.errors.close()
        assert self.child.returncode == 0, 'fanotify observer failed'
        stopped = json.loads(output)
        assert stopped['phase'] == 'stopped'
        assert root_pid in browser_pids, 'native root missing from process accounting'
        result = attribute(stopped['events'], self.ready['baseline'], browser_pids,
            self.ready['controller_identity']['cgroup'], self.ready['other_cgroups'])
        (self.evidence/'fanotify-result.json').write_text(json.dumps(result, indent=2)+'\n')
        return result

    def abort(self):
        """Preserve failed-run notifications without manufacturing attribution."""
        if self.child.poll() is None:
            output, _ = self.child.communicate('stop\n', timeout=60)
            (self.evidence/'fanotify-aborted.json').write_text(output)
        self.errors.close()


def attribute(events, baseline, browser_pids, controller_cgroup=None, other_cgroups=()):
    baseline = {int(pid): row for pid, row in baseline.items()}
    mutations, reads, unknown = [], [], []
    unrelated = unrelated_cgroup = 0
    for event in events:
        if event.get('error') or event['pid'] <= 0:
            unknown.append(event)
        elif event['pid'] in browser_pids or any(
                row['pid'] in browser_pids for row in event['ancestry']):
            if event['mask'] & WRITE_MASK:
                mutations.append(event)
            if event['mask'] & (FAN_OPEN | FAN_ACCESS):
                reads.append(event)
        elif (event.get('kernel_identity', {}).get('mask', 0) & PIDFD_INFO_CGROUPID
              and event['kernel_identity']['cgroup'] != controller_cgroup
              and event['kernel_identity']['cgroup'] in other_cgroups):
            # This is a kernel-identified different pre-existing workload,
            # including its cached identity after reaping. Numeric PID guesses
            # and unknown cgroups never take this path. Native IDs win above.
            unrelated += 1
            unrelated_cgroup += 1
        elif any(row['pid'] in baseline and
                baseline[row['pid']]['start_ticks'] == row['start_ticks']
                and (row['pid'] == event['pid'] or row['name'] != 'systemd')
                for row in event['ancestry']):
            unrelated += 1
        else:
            unknown.append(event)
    return dict(events=events, mutations=mutations, parent_reads=reads,
        unknown=unknown, unrelated=unrelated, browser_pids=sorted(browser_pids),
        unrelated_cgroup=unrelated_cgroup,
        pass_=not mutations and not reads and not unknown)


def process_accounting(prefix):
    """Task identity/termination only; no syscall is accepted as filesystem proof."""
    tasks = {}
    for path in sorted(Path(prefix).parent.glob(Path(prefix).name+'.*')):
        suffix = path.name[len(Path(prefix).name)+1:]
        assert suffix.isdigit() and int(suffix) not in tasks
        lines = path.read_text().splitlines()
        assert lines, ('empty process accounting receipt', path)
        last = re.sub(r'^\d+\.\d+\s+', '', lines[-1])
        exited = re.fullmatch(r'\+\+\+ exited with (\d+) \+\+\+', last)
        killed = re.fullmatch(r'\+\+\+ killed by .+ \+\+\+', last)
        assert exited or killed, ('missing process termination', path, last)
        tasks[int(suffix)] = dict(path=str(path), terminal=(dict(kind='exit',
            code=int(exited[1])) if exited else dict(kind='signal', detail=last)))
    assert tasks, 'missing process accounting'
    return tasks


if __name__ == '__main__':
    assert sys.argv[1:] == ['--serve']
    serve()
