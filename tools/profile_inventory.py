# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pinned tree observations, never an atomic snapshot or an I/O exemption."""
import hashlib
import json
import os
from pathlib import Path
import stat
import time


def attributes(fd):
    value = os.fstat(fd)
    return dict(device=value.st_dev, inode=value.st_ino, mode=value.st_mode,
                size=value.st_size, mtime_ns=value.st_mtime_ns, ctime_ns=value.st_ctime_ns)


def observe_tree(root, names, *, hashes, report_path, allow_races=False):
    root = Path(root)
    entries, records, races = {}, [], []

    def race(path, stage, error):
        races.append(dict(path=path, stage=stage, error=str(error), observed_ns=time.monotonic_ns()))

    def visit(parent, name, relative, depth):
        assert depth < 128 and len(records) < 100000, 'inventory observation bound exceeded'
        row = dict(path=relative, enumerated_ns=time.monotonic_ns())
        records.append(row)
        try:
            pinned = os.open(name, os.O_PATH | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=parent)
        except OSError as error:
            entries[relative] = dict(kind='raced', stage='enumeration-to-pin', errno=error.errno)
            race(relative, 'enumeration-to-pin', error)
            return
        try:
            before = attributes(pinned)
            row['before'] = before
            mode = before['mode']
            if stat.S_ISLNK(mode):
                entries[relative] = dict(kind='symlink', target=os.readlink('', dir_fd=pinned))
            elif stat.S_ISDIR(mode):
                entries[relative] = dict(kind='directory')
                directory = os.open('/proc/self/fd/'+str(pinned), os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
                try:
                    assert attributes(directory)['inode'] == before['inode']
                    with os.scandir(directory) as iterator:
                        children = sorted(entry.name for entry in iterator)
                    row['enumerated_children'] = children
                    for child in children:
                        visit(directory, child, relative+'/'+child, depth+1)
                finally:
                    os.close(directory)
            elif stat.S_ISREG(mode):
                value = dict(kind='file')
                if hashes:
                    descriptor = os.open('/proc/self/fd/'+str(pinned), os.O_RDONLY | os.O_NOATIME | os.O_NONBLOCK | os.O_CLOEXEC)
                    try:
                        actual = attributes(descriptor)
                        assert (actual['device'], actual['inode']) == (before['device'], before['inode'])
                        digest, size = hashlib.sha256(), 0
                        while size <= before['size']:
                            chunk = os.read(descriptor, min(1024*1024, before['size']+1-size))
                            if not chunk:
                                break
                            digest.update(chunk)
                            size += len(chunk)
                        value.update(sha256=digest.hexdigest(), size=size)
                        if size != before['size']:
                            race(relative, 'size-changed-during-read', dict(before=before['size'], read=size))
                    finally:
                        os.close(descriptor)
                entries[relative] = value
            else:
                entries[relative] = dict(kind='special', mode=mode)
                race(relative, 'unsupported-native-object', mode)
            after = attributes(pinned)
            row['after'] = after
            if before != after:
                race(relative, 'object-changed-during-observation', dict(before=before, after=after))
        except OSError as error:
            entries.setdefault(relative, dict(kind='raced', stage='pinned-object-read', errno=error.errno))
            race(relative, 'pinned-object-read', error)
        finally:
            os.close(pinned)

    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        for name in names:
            visit(directory, name, name, 0)
            # Keep the established comparison projection: children of managed roots.
            if entries.get(name, {}).get('kind') == 'directory':
                entries.pop(name)
            else:
                race(name, 'managed-root-not-directory', entries.get(name))
    finally:
        os.close(directory)
    report = dict(entries=entries, records=records, races=races,
                  allow_races=allow_races, hashes=hashes, atomic_snapshot=False,
                  complete_observation=not races, io_events_exempted=0)
    Path(report_path).write_text(json.dumps(report, indent=2)+'\n')
    assert allow_races or not races, ('incomplete strict inventory observation', races)
    return entries


def verify_observation(report, projection, *, require_stable):
    assert report['entries'] == projection
    assert report['atomic_snapshot'] is False and report['io_events_exempted'] == 0
    assert report['complete_observation'] == (not report['races'])
    if require_stable:
        assert report['allow_races'] is False and not report['races']
    return report['races']
