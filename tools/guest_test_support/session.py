# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""SDK process control and exact owned process-exit observation."""
import json
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import threading
import time

from .display import environment


def descendants(pid):
    found = []
    pending = [pid]
    while pending:
        parent = pending.pop()
        try:
            tasks = list((Path('/proc') / str(parent) / 'task').iterdir())
        except FileNotFoundError:
            continue
        # Linux records children on the spawning thread, not just its group's
        # main thread. Session workers intentionally spawn outside app-side UI.
        for task in tasks:
            try:
                children = (task / 'children').read_text()
            except FileNotFoundError:
                continue
            for value in children.split():
                child = int(value)
                if child not in found:
                    found.append(child)
                    pending.append(child)
    return found


def process_role(arguments):
    # Chromium rewrites /proc/cmdline to one space-separated process title.
    # Before that rewrite, the same flags are distinct NUL-separated arguments.
    title = b' '.join(arguments)
    if re.search(rb'(?:^|\s)--alloy-supervisor(?:\s|$)', title):
        return 'supervisor'
    match = re.search(rb'(?:^|\s)--type=([a-z0-9-]+)(?:\s|$)', title)
    return match[1].decode() if match else 'browser'


class OwnedProcesses:
    def __init__(self, app_pid, out):
        self.members = []
        self.handles = []
        for pid in descendants(app_pid):
            handle = None
            try:
                handle = os.pidfd_open(pid)
                exe = (Path('/proc') / str(pid) / 'exe').resolve(strict=True)
                argv = (Path('/proc') / str(pid) / 'cmdline').read_bytes().split(b'\0')
            except (FileNotFoundError, ProcessLookupError):
                if handle is not None:
                    os.close(handle)
                continue
            if exe not in {out / 'alloy_guest_host', out / 'alloy_guest_engine'}:
                os.close(handle)
                continue
            role = process_role(argv)
            self.members.append({'pid': pid, 'exe': str(exe), 'role': role})
            self.handles.append(handle)

    def kill(self, role):
        candidates = [(entry, fd) for entry, fd in zip(self.members, self.handles)
                      if entry['role'] == role]
        if not candidates:
            raise AssertionError(f'No actual admitted {role}: {self.members}')
        signal.pidfd_send_signal(candidates[0][1], signal.SIGKILL)
        return candidates[0][0]

    def kill_all(self, role):
        candidates = [(entry, fd) for entry, fd in zip(self.members, self.handles)
                      if entry['role'] == role]
        if not candidates:
            raise AssertionError(f'No actual admitted {role}: {self.members}')
        for entry, handle in candidates:
            signal.pidfd_send_signal(handle, signal.SIGKILL)
        return [entry for entry, handle in candidates]

    def exited(self, timeout=30):
        import select
        deadline = time.monotonic() + timeout
        for entry, handle in zip(self.members, self.handles):
            poll = select.poll()
            poll.register(handle, select.POLLIN)
            assert poll.poll(max(0, int((deadline - time.monotonic()) * 1000))), entry

    def close(self):
        for handle in self.handles:
            os.close(handle)
        self.handles.clear()


class Client:
    def __init__(self, out: Path, root: Path, flexible=False, trusted_certificate=None):
        self.out = out
        self.root = root
        self.env = environment(root, out)
        if trusted_certificate is not None:
            # Existing boundary probes use this same private OS/NSS input.
            # Trust only the generated fixture CA; never disable TLS validation.
            nss = root / 'data/pki/nssdb'
            nss.mkdir(parents=True, mode=0o700)
            with (root / 'certificate-input.log').open('x') as log:
                subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:' + str(nss)],
                    env=self.env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
                subprocess.run(['certutil', '-A', '-d', 'sql:' + str(nss), '-n', 'Alloy guest fixture',
                    '-t', 'C,,', '-i', str(trusted_certificate)], env=self.env,
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
        command = [str(out / 'alloy_guest_sdk_consumer')]
        if flexible:
            command += ['--flexible', str(root / 'app-profile')]
        self.log = (root / 'consumer.log').open('x')
        self.stderr = (root / 'stderr.log').open('x')
        self.process = subprocess.Popen(command, cwd=out, env=self.env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, text=True, bufsize=1)
        self.events = []
        self.incoming = queue.Queue()

        def read():
            for line in self.process.stdout:
                self.log.write(line)
                self.log.flush()
                if line.startswith('GUEST_EVENT '):
                    self.incoming.put(json.loads(line.removeprefix('GUEST_EVENT ')))
            self.incoming.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def wait(self, name, event, predicate=lambda value: True, timeout=120, after=0):
        deadline = time.monotonic() + timeout
        while True:
            for value in self.events[after:]:
                if value['name'] == name and value['event'] == event and predicate(value):
                    return value
            left = deadline - time.monotonic()
            if left <= 0:
                raise AssertionError(f'Timed out: {name} {event}; {self.root}')
            try:
                value = self.incoming.get(timeout=min(left, 1))
            except queue.Empty:
                continue
            if value is None:
                raise AssertionError(f'SDK consumer exited {self.process.poll()}: {self.root}')
            self.events.append(value)

    def start(self, name='guest', policy='auto', mode='memory', required='unavailable', data=None):
        self.send(f'start {name} {policy} {mode} {required} {int(required == "unavailable")} {data or "-"}')
        return self.wait(name, 'prepared')

    def active(self, name='guest'):
        return self.wait(name, 'state', lambda value: value['state'] == 'ACTIVE')

    def page(self, url, title, name='guest'):
        after = len(self.events)
        self.send(f'create {name} {url}')
        ready = self.wait(name, 'view-ready', after=after)
        return self.wait(name, 'title',
                         lambda value: value['view'] == ready['view'] and value['title'] == title,
                         after=after)

    def closed(self, name='guest', reason='closed'):
        return self.wait(name, 'closed', lambda value: value['reason'] == reason)

    def finish(self):
        self.send('quit')
        self.process.stdin.close()
        code = self.process.wait(timeout=120)
        self.reader.join(timeout=10)
        assert not self.reader.is_alive()
        while not self.incoming.empty():
            value = self.incoming.get_nowait()
            if value is not None:
                self.events.append(value)
        assert code == 0, (code, self.root)
        assert any(v['event'] == 'finished' for v in self.events)
        (self.root / 'events.json').write_text(json.dumps(self.events, indent=2) + '\n')
        self.log.close()
        self.stderr.close()

    def abort(self):
        if self.process.poll() is None:
            handle = os.pidfd_open(self.process.pid)
            signal.pidfd_send_signal(handle, signal.SIGKILL)
            os.close(handle)
            self.process.wait(timeout=30)
        self.reader.join(timeout=10)
        self.log.close()
        self.stderr.close()

    def __enter__(self):
        return self

    def __exit__(self, error, *args):
        if error:
            self.abort()
        elif self.process.poll() is None:
            self.finish()
