#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Read complete per-task strace receipts for the NUC no-disk process probe.

The tracer observes native filesystem/path, process and memory syscalls. It is
run around the native consumer, after Xvfb/controller setup, with -ff -ttt -yy
-xx. It neither rewrites application paths nor suppresses native filesystem
operations. Inventories and seeded-input byte checks remain independent gates.
"""
import re
from pathlib import Path

CREATE_OPERATIONS = frozenset({
    'creat', 'mkdir', 'mkdirat', 'mknod', 'mknodat', 'link', 'linkat',
    'symlink', 'symlinkat', 'rename', 'renameat', 'renameat2',
})
MUTATING_OPERATIONS = CREATE_OPERATIONS | {
    'unlink', 'unlinkat', 'rmdir', 'truncate', 'ftruncate', 'chmod', 'fchmod',
    'fchmodat', 'fchmodat2', 'chown', 'lchown', 'fchown', 'fchownat', 'utime',
    'utimes', 'futimesat', 'utimensat', 'setxattr', 'lsetxattr', 'fsetxattr',
    'removexattr', 'lremovexattr', 'fremovexattr', 'fallocate',
}
OPEN_OPERATIONS = frozenset({'open', 'openat', 'openat2', 'open_by_handle_at'})


def trace_command(tracer, prefix, command):
    return [str(tracer), '-ff', '-ttt', '-yy', '-xx', '-s', '65536',
            '-e', 'trace=%file,%process,%memory,bind,io_uring_setup,io_uring_enter,io_uring_register',
            '-o', str(prefix), '--', *map(str, command)]


def strings(arguments):
    """Decode -xx strings without eval, escaping ambiguities or truncation."""
    assert not re.search(r'"(?:\\x[0-9a-f]{2})*"\.\.\.', arguments), 'truncated pathname'
    result = []
    for match in re.finditer(r'"((?:\\x[0-9a-f]{2})*)"', arguments):
        data = bytes(int(v, 16) for v in re.findall(r'\\x([0-9a-f]{2})', match[1]))
        result.append(data.decode('utf-8', errors='surrogateescape'))
    return result


def descriptor_paths(text):
    # -yy prints the kernel-resolved target after a successful returned fd,
    # including for relative openat and symlink aliases. Keep raw arguments as
    # separate evidence; never normalize filesystem inventories to hide paths.
    return [bytes(int(v, 16) for v in re.findall(r'\\x([0-9a-f]{2})', match[1]))
            .decode('utf-8', errors='surrogateescape')
            for match in re.finditer(r'<((?:\\x[0-9a-f]{2})+)(?=[<>])', text)]


def mount_view():
    result = []
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        head, tail = line.split(' - ', 1)
        point = re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), head.split()[4])
        result.append(dict(point=point, filesystem=tail.split()[0]))
    return result


def kernel_endpoint_open(call, mounts):
    # procfs/sysfs do not implement creation of new ordinary files. open with
    # O_CREAT (including libc creat) can still open an existing kernel control
    # endpoint. Classify by the actual mount, never by a profile-path exclusion.
    if call['syscall'] not in OPEN_OPERATIONS | {'creat'}:
        return False
    targets = call.get('descriptor_paths', [])
    if not targets:
        return False
    target = targets[0]
    matching = [m for m in mounts if target == m['point'] or
                target.startswith(m['point'].rstrip('/')+'/')]
    assert matching, ('unresolved filesystem identity', call)
    mount = max(matching, key=lambda m: len(m['point']))
    return mount['filesystem'] in ('proc', 'sysfs')


def may_mutate(name, arguments):
    return name in MUTATING_OPERATIONS or (
        name == 'bind' and 'AF_UNIX' in arguments and 'sun_path="' in arguments) or (
        name in OPEN_OPERATIONS and bool(re.search(r'\bO_(CREAT|TMPFILE|TRUNC)\b', arguments)))


def read_trace(prefix):
    paths = sorted(Path(prefix).parent.glob(Path(prefix).name+'.*'))
    assert paths, 'no native trace files'
    mounts = mount_view()
    tasks, creations, mutations, opens, unsupported = {}, [], [], [], []
    kernel_opens = []
    for path in paths:
        suffix = path.name[len(Path(prefix).name)+1:]
        assert suffix.isdigit(), 'unexpected trace receipt '+str(path)
        pid = int(suffix)
        assert pid not in tasks
        calls = []
        pending = None
        terminal = None
        for number, raw in enumerate(path.read_text().splitlines(), 1):
            line = re.sub(r'^\d+\.\d+\s+', '', raw)
            if line.startswith('+++ exited with '):
                assert terminal is None
                terminal = dict(kind='exit', code=int(re.fullmatch(r'\+\+\+ exited with (\d+) \+\+\+', line)[1]))
                continue
            if line.startswith('+++ killed by '):
                assert terminal is None
                terminal = dict(kind='signal', detail=line)
                continue
            if line.startswith('--- '):
                continue
            assert terminal is None, 'events after terminal receipt'
            assert not line.startswith(('strace:', '[ Process ')), line
            if '<unfinished ...>' in line:
                assert pending is None, (path, pending, line)
                pending = (number, line.replace('<unfinished ...>', ''))
                continue
            resumed = re.match(r'<\.\.\. ([a-zA-Z0-9_]+) resumed>(.*)', line)
            if resumed:
                assert pending is not None, (path, line)
                number, start = pending
                assert start.startswith(resumed[1]+'('), (start, line)
                line = start+resumed[2]
                pending = None
            match = re.fullmatch(r'([a-zA-Z0-9_]+)\((.*)\)\s+=\s+(.+)', line)
            assert match, (path, number, line)
            name, arguments, result = match.groups()
            calls.append(dict(pid=pid, line=number, syscall=name,
                              arguments=arguments, result=result))
        assert terminal is not None, 'missing task termination: '+str(path)
        if pending:
            number, line = pending
            match = re.match(r'([a-zA-Z0-9_]+)\((.*)', line)
            assert match and not may_mutate(match[1], match[2]), (
                'unresolved mutating syscall', path, number, line)
            # A killed task can have a pending read-only syscall. Keep its
            # entry as observed input access, never imply that it succeeded.
            calls.append(dict(pid=pid, line=number, syscall=match[1],
                              arguments=match[2], result='terminated-before-return'))
        tasks[pid] = dict(path=str(path), terminal=terminal, calls=len(calls))
        for call in calls:
            name, arguments, result = (call[k] for k in ('syscall', 'arguments', 'result'))
            call['strings'] = strings(arguments)
            if name.startswith('io_uring_') and not result.startswith('-1 '):
                unsupported.append(call)
            succeeded = bool(re.match(r'(?:0x[0-9a-f]+|\d+)(?:\s|$|<)', result))
            if name in OPEN_OPERATIONS or name == 'creat':
                call['descriptor_paths'] = descriptor_paths(result)
                opens.append(call)
            if not succeeded:
                continue
            creates = name in CREATE_OPERATIONS or (
                name in OPEN_OPERATIONS and bool(re.search(r'\bO_(CREAT|TMPFILE)\b', arguments))) or (
                name == 'bind' and 'AF_UNIX' in arguments and 'sun_path="' in arguments)
            if creates:
                if kernel_endpoint_open(call, mounts):
                    kernel_opens.append(call)
                else:
                    creations.append(call)
            if may_mutate(name, arguments):
                mutations.append(call)
    assert not unsupported, ('unsupported asynchronous filesystem channel', unsupported)
    return dict(tasks=tasks, creations=creations, mutations=mutations, opens=opens,
                kernel_endpoint_opens=kernel_opens, mount_view=mounts)
