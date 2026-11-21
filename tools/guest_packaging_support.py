# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Local Flatpak fixture construction and external-entry observations."""
from contextlib import AbstractContextManager, contextmanager
import array
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import time
import uuid

from guest_lock_probe import Members
from guest_test_support.session import Client

ZYPAK_REVISION = '693a71c5ffa80ec9c9ce2ae03b1ccc493c698e53'
RUNTIME = 'org.freedesktop.Platform'
BRANCH = '25.08'
IDENTITY_EXCHANGE_TIMEOUT = 15


def terminate_identity(handle):
    """Fixture-only root signaling of an already-pinned original identity."""
    try:
        signal.pidfd_send_signal(handle, signal.SIGKILL)
        return
    except ProcessLookupError:
        return
    except PermissionError:
        pass
    channel, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    channel.settimeout(IDENTITY_EXCHANGE_TIMEOUT)
    helper = None
    monitor = -1
    try:
        script = Path(__file__).parent / 'guest_test_support/process_identity.py'
        helper = subprocess.Popen(['sudo', '-n', 'env', 'TMPDIR=' + os.environ['TMPDIR'],
                                   'python3', '-B', str(script), '--terminate'], stdin=child)
        monitor = os.pidfd_open(helper.pid)
        child.close()
        channel.sendmsg([b'K'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [handle]))])
        channel.close()
        assert helper.wait(timeout=IDENTITY_EXCHANGE_TIMEOUT) == 0, 'Pinned helper termination failed'
    finally:
        child.close()
        channel.close()
        if helper is not None and helper.poll() is None:
            if monitor >= 0:
                # sudo's owned monitor forwards TERM to its command before
                # waiting/reaping. No bare process identity is signaled.
                try:
                    signal.pidfd_send_signal(monitor, signal.SIGTERM)
                    signal.pidfd_send_signal(monitor, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            helper.wait()
        if monitor >= 0:
            os.close(monitor)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


class LocalFlatpak(AbstractContextManager):
    def __init__(self, repo, out, evidence):
        self.repo, self.out, self.evidence = repo, out, evidence
        self.token = uuid.uuid4().hex
        self.app_id = 'org.atmosphera.AlloyGuestM7.r' + self.token
        self.remote = 'alloy-m7-' + self.token
        self.stage = evidence / 'staging'
        self.installed = False
        self.remote_added = False
        self.library = None
        self.log = None

    def run(self, command, **kwargs):
        command = list(map(str, command))
        self.log.write('+ ' + repr(command) + '\n')
        self.log.flush()
        try:
            result = subprocess.run(command, text=True, check=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=kwargs.pop('timeout', 600), **kwargs)
        except subprocess.CalledProcessError as error:
            self.log.write(error.stdout or '')
            self.log.flush()
            raise
        self.log.write(result.stdout)
        self.log.flush()
        return result.stdout.strip()

    def __enter__(self):
        self.stage.mkdir(parents=True, exist_ok=False)
        self.log = (self.evidence / 'package-build.log').open('x')
        try:
            self._build()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def _build(self):
        self.run(['flatpak', 'info', '--user', RUNTIME + '//' + BRANCH])
        source = self.stage / 'zypak'
        self.run(['git', 'clone', '--no-checkout', '--depth=1', '--branch', 'v2025.09',
                  'https://github.com/refi64/zypak.git', source])
        self.run(['git', '-C', source, 'checkout', '--detach', ZYPAK_REVISION])
        assert self.run(['git', '-C', source, 'rev-parse', 'HEAD']) == ZYPAK_REVISION
        self.run(['git', '-C', source, 'submodule', 'update', '--init', '--depth=1'])
        patch = self.repo / 'tools/testdata/guest-packaging/zypak-alloy.patch'
        self.run(['git', '-C', source, 'apply', '--check', patch])
        self.run(['git', '-C', source, 'apply', patch])
        message_test = source / 'alloy-message-fds'
        self.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
            '-I', source / 'src', '-I', source / 'nickle',
            self.repo / 'tools/testdata/guest-packaging/message_fds.cc',
            source / 'src/dbus/bus_writable_message.cc',
            *shlex.split(self.run(['pkg-config', '--cflags', '--libs', 'dbus-1'])),
            '-o', message_test])
        self.run([message_test, self.stage])
        environment_test = source / 'alloy-spawn-environment'
        self.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror',
            '-fsanitize=address', '-fsanitize-address-use-after-scope', '-fno-omit-frame-pointer',
            '-I', source / 'src', '-I', source / 'nickle',
            self.repo / 'tools/testdata/guest-packaging/spawn_environment.cc',
            source / 'src/preload/host/spawn_strategy/spawn_launcher_delegate.cc',
            source / 'src/base/env.cc', source / 'src/base/debug.cc',
            source / 'src/base/debug_internal/log_stream.cc',
            *shlex.split(self.run(['pkg-config', '--cflags', '--libs', 'dbus-1'])),
            '-o', environment_test])
        environment_fixture = self.stage / 'spawn-environment-fixture'
        environment_fixture.mkdir()
        self.run([environment_test, environment_fixture],
                 env=dict(os.environ, ASAN_OPTIONS='detect_stack_use_after_return=1'))
        self.run(['make', '-C', source, '-j2'])
        package = self.stage / 'package'
        self.run(['flatpak', 'build-init', package, self.app_id, RUNTIME, RUNTIME, BRANCH])
        library = package / 'files/lib/alloy'
        library.mkdir(parents=True)
        copied = set()
        for pattern in ('*.so', '*.so.*', '*.pak', '*.dat', '*.bin', '*.json', 'locales',
                        'alloy_guest_host', 'alloy_guest_engine', 'alloy_guest_sdk_consumer'):
            for original in self.out.glob(pattern):
                assert not original.is_symlink(), original
                if original.name not in copied:
                    self.run(['cp', '-a', '--reflink=auto', original, library])
                    copied.add(original.name)
        assert {'libcef.so', 'alloy_guest_host', 'alloy_guest_engine',
                'alloy_guest_sdk_consumer'} <= copied, copied
        prefix = library / 'zypak'
        # The pinned install recipe uses bash brace expansion for this alias.
        self.run(['make', '-C', source, 'install', 'SHELL=/bin/bash',
                  'FLATPAK_DEST=' + str(prefix)])
        # make install creates an absolute wrapper symlink for the staging
        # prefix. The installed package uses the same relative bin layout.
        wrapper = prefix / 'bin/zypak-wrapper'
        assert wrapper.is_symlink()
        wrapper.unlink()
        wrapper.symlink_to('zypak-wrapper.sh')
        self._stage_dependencies(library, prefix)
        data = package / 'files/share/alloy-data'
        shutil.copytree(self.repo / 'tools/testdata/alloy-data', data)
        bin_dir = package / 'files/bin'
        bin_dir.mkdir()
        launcher = bin_dir / 'alloy-m7-sdk'
        launcher.write_text('#!/bin/sh\nset -eu\n'
            'export LD_LIBRARY_PATH=/app/lib/alloy:/app/lib\n'
            'export ZYPAK_CEF_LIBRARY_PATH=/app/lib/alloy/libcef.so\n'
            'exec /app/lib/alloy/zypak/bin/zypak-wrapper '
            '/app/lib/alloy/alloy_guest_sdk_consumer "$@"\n')
        launcher.chmod(0o755)
        self.run(['flatpak', 'build-finish', '--command=alloy-m7-sdk', '--share=network',
                  '--filesystem=xdg-run/alloy-guest:create',
                  '--talk-name=org.freedesktop.portal.Flatpak', package])
        self._install_package(package)

    def _install_package(self, package):
        export = self.stage / 'export'
        self.run(['flatpak', 'build-export', export, package, 'stable'])
        self.run(['flatpak', 'remote-add', '--user', '--no-gpg-verify', self.remote,
                  'file://' + str(export)])
        self.remote_added = True
        self.run(['flatpak', 'install', '--user', '--noninteractive', '-y',
                  self.remote, self.app_id, 'stable'])
        self.installed = True
        installed = Path(self.run(['flatpak', 'info', '--user', '--show-location', self.app_id]))
        self.library = installed / 'files/lib/alloy'
        assert self.library.is_dir(), self.library
        (self.evidence / 'flatpak-metadata.txt').write_text(
            self.run(['flatpak', 'info', '--user', '--show-metadata', self.app_id]) + '\n')
        (self.evidence / 'flatpak-info.txt').write_text(
            self.run(['flatpak', 'info', '--user', self.app_id]) + '\n')
        (self.evidence / 'package-inputs.json').write_text(json.dumps(dict(
            app_id=self.app_id, zypak=ZYPAK_REVISION,
            zypak_patch=digest(self.repo / 'tools/testdata/guest-packaging/zypak-alloy.patch'),
            runtime=self.run(['flatpak', 'info', '--user', RUNTIME + '//' + BRANCH]),
            files={str(path.relative_to(package)): digest(path)
                   for path in sorted(package.rglob('*')) if path.is_file() and not path.is_symlink()},
        ), indent=2) + '\n')

    def _stage_dependencies(self, library, prefix):
        destination = library.parent
        dependencies = {}
        # Keep the runtime's coherent libc/loader. Other native dependencies are
        # fixture package inputs copied from this VM, never host installations.
        excluded = re.compile(r'^(?:ld-linux|lib(?:c|m|dl|pthread|rt|resolv)\.so)')
        for executable in (library / 'libcef.so', prefix / 'bin/zypak-helper',
                           prefix / 'bin/zypak-sandbox'):
            text = self.run(['ldd', executable])
            assert 'not found' not in text, text
            for line in text.splitlines():
                match = re.search(r'=> (/\S+) \(', line)
                if not match:
                    continue
                original = Path(match[1])
                if excluded.match(original.name) or original.parent == library:
                    continue
                checksum = digest(original)
                assert original.name not in dependencies or dependencies[original.name]['sha256'] == checksum
                target = destination / original.name
                if not target.exists():
                    shutil.copy2(original, target, follow_symlinks=True)
                assert digest(target) == checksum
                dependencies[original.name] = dict(source=str(original), sha256=checksum)
        (self.evidence / 'native-dependencies.json').write_text(json.dumps(dependencies, indent=2) + '\n')

    def snapshot(self):
        script = self.repo / 'tools/testdata/guest-packaging/observe.py'
        return json.loads(self.run(['sudo', '-n', 'env', 'TMPDIR=' + os.environ['TMPDIR'],
                                    'python3', '-B', script, self.library]))

    def client(self, root, **kwargs):
        return PackageClient(self, root, **kwargs)

    def abort(self, process):
        # Portal-spawned entries are not descendants of the app wrapper. The
        # installed package's inode inventory owns cleanup, just as acceptance
        # owns process-exit observation. Signal only already-pinned identities.
        with PackageMembers(self.snapshot(), include_app=True) as members:
            members.terminate()
        if process.poll() is None:
            try:
                handle = os.pidfd_open(process.pid)
                try:
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
                finally:
                    os.close(handle)
            except ProcessLookupError:
                pass

    @contextmanager
    def without_permission(self, section, key, value=None):
        """A cold permission variant: reuse verified bytes, never runtime identity."""
        assert self.installed and not self.snapshot(), 'Retire runtime before package replacement'
        variant = LocalFlatpak(self.repo, self.out, self.evidence / ('permission-' + uuid.uuid4().hex))
        parsed = configparser.ConfigParser(interpolation=None)
        parsed.optionxform = str
        parsed.read_string((self.stage / 'package/metadata').read_text())
        parsed['Application']['name'] = variant.app_id
        if value is None:
            assert parsed.remove_option(section, key)
        else:
            values = [entry for entry in parsed[section][key].split(';') if entry]
            assert values.count(value) == 1, values
            values.remove(value)
            parsed[section][key] = ';'.join(values) + (';' if values else '')
        variant.stage.mkdir(parents=True, exist_ok=False)
        variant.log = (variant.evidence / 'package-build.log').open('x')
        try:
            package = variant.stage / 'package'
            variant.run(['cp', '-a', '--reflink=auto', self.stage / 'package', package])
            metadata = package / 'metadata'
            with metadata.open('w') as stream:
                parsed.write(stream, space_around_delimiters=False)
            variant._install_package(package)
            yield variant
        finally:
            variant.__exit__(None, None, None)

    @contextmanager
    def missing_asset(self, relative):
        """Install an intentionally incomplete local package, then restore it."""
        assert self.installed and not self.snapshot(), 'Retire runtime before package replacement'
        library = self.stage / 'package/files/lib/alloy'
        asset = library / relative
        assert asset.resolve().is_relative_to(library.resolve()) and asset.is_file()
        saved = self.stage / 'withheld-package-asset'
        asset.rename(saved)
        try:
            self._redeploy()
            yield
        finally:
            saved.rename(asset)
            self._redeploy()

    def _redeploy(self):
        self.run(['flatpak', 'build-export', self.stage / 'export', self.stage / 'package', 'stable'])
        self.run(['flatpak', 'update', '--user', '--noninteractive', '-y', self.app_id])
        installed = Path(self.run(['flatpak', 'info', '--user', '--show-location', self.app_id]))
        self.library = installed / 'files/lib/alloy'

    def __exit__(self, *args):
        try:
            if self.installed:
                self.run(['flatpak', 'uninstall', '--user', '--noninteractive', '-y',
                          '--delete-data', self.app_id])
                self.installed = False
            if self.remote_added:
                self.run(['flatpak', 'remote-delete', '--user', self.remote])
                self.remote_added = False
        finally:
            if self.log is not None:
                self.log.close()
                self.log = None


class PackageClient(Client):
    def __init__(self, package, root, session_bus=True, **kwargs):
        self.package = package
        kwargs.setdefault('abort_callback', package.abort)
        if kwargs.get('flexible') or kwargs.get('encrypted_app'):
            kwargs['app_profile'] = Path.home() / '.var/app' / package.app_id / 'cache/app-profile'
        super().__init__(package.library, root,
            launch_prefix=['flatpak', 'run', '--user',
                *([] if session_bus else ['--no-session-bus']),
                '--env=LD_LIBRARY_PATH=/app/lib/alloy:/app/lib',
                '--env=ZYPAK_CEF_LIBRARY_PATH=/app/lib/alloy/libcef.so',
                '--command=/app/lib/alloy/zypak/bin/zypak-wrapper', package.app_id],
            executable='/app/lib/alloy/alloy_guest_sdk_consumer',
            launch_environment=dict(os.environ), **kwargs)

    def stage_guest_certificate(self, before, name):
        rows = self.package.snapshot()
        roots = {Path(target) for row in rows if row['role'] == 'supervisor' and row['pid'] not in before
                 for target in row['fds'].values()
                 if re.fullmatch(r'/run/user/\d+/alloy-guest/alloy-guest-\d+-[^/]+', target)}
        assert len(roots) == 1, roots
        scratch = roots.pop()
        assert scratch.resolve(strict=True) == scratch and scratch.stat().st_mode & 0o777 == 0o700
        assert subprocess.check_output(['stat', '-f', '-c', '%T', scratch], text=True).strip() == 'tmpfs'
        with (self.root / ('guest-certificate-input-' + name + '.log')).open('x') as log:
            for nss in (scratch / 'home/.pki/nssdb', scratch / 'data/pki/nssdb'):
                if not (nss / 'cert9.db').exists():
                    subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:' + str(nss)],
                        stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
                subprocess.run(['certutil', '-A', '-d', 'sql:' + str(nss), '-n', 'Alloy guest fixture',
                    '-t', 'C,,', '-i', str(self.trusted_certificate)],
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)


class PackageMembers(Members):
    def __init__(self, rows, include_app=False):
        self.rows = [row for row in rows if include_app or row['role'] != 'app']
        self.handles = []
        try:
            for row in self.rows:
                try:
                    self.handles.append((row, os.pidfd_open(row['pid'])))
                except ProcessLookupError:
                    pass
            self._validate_identities()
        except BaseException:
            self.close()
            raise

    def _validate_identities(self):
        if not self.handles:
            self.rows = []
            return
        # The observer's numeric PID is only a lookup hint. Revalidate the live
        # birth/executable identity after pinning, before any signal or root
        # signaling helper can receive this capability.
        channel, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        channel.settimeout(IDENTITY_EXCHANGE_TIMEOUT)
        script = Path(__file__).parent / 'guest_test_support/process_identity.py'
        try:
            helper = subprocess.Popen(['sudo', '-n', 'env', 'TMPDIR=' + os.environ['TMPDIR'],
                                       'python3', '-B', str(script)], stdin=child)
        except BaseException:
            child.close()
            channel.close()
            raise
        child.close()
        live = []
        validator = -1
        monitor = -1
        transport_failed = False
        try:
            monitor = os.pidfd_open(helper.pid)
            packet, control, flags, _ = channel.recvmsg(1, socket.CMSG_SPACE(array.array('i').itemsize))
            handles = array.array('i')
            for level, kind, data in control:
                assert level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
                handles.frombytes(data)
            try:
                assert packet == b'I' and not flags and len(handles) == 1, 'Invalid validator identity'
                validator = handles.pop()
            finally:
                for handle in handles:
                    os.close(handle)
            deadline = time.monotonic() + IDENTITY_EXCHANGE_TIMEOUT
            for row, handle in self.handles:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('Process identity exchange deadline expired')
                channel.settimeout(remaining)
                payload = json.dumps(dict(pid=row['pid'], identity=row['identity'])).encode()
                channel.sendmsg([payload], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                             array.array('i', [handle]))])
                result = channel.recv(2)
                assert result in (b'V', b'E', b'R'), 'Process identity validation failed'
                assert result != b'R', ('Observed package process identity changed', row)
                if result == b'V':
                    live.append((row, handle))
            retired = {handle for row, handle in self.handles} - {handle for row, handle in live}
            for handle in retired:
                os.close(handle)
            self.handles = live
            self.rows = [row for row, handle in live]
        except (TimeoutError, OSError):
            transport_failed = True
            raise
        finally:
            channel.close()
            try:
                if transport_failed and validator >= 0:
                    terminate_identity(validator)
                    # sudo mirrors a stopped command's job-control state.
                    # Resume its original monitor after retiring that command,
                    # otherwise a successful SIGKILL can still leave wait stuck.
                    if monitor >= 0:
                        try:
                            signal.pidfd_send_signal(monitor, signal.SIGCONT)
                        except ProcessLookupError:
                            pass
                elif transport_failed and monitor >= 0:
                    # Startup failed before the trusted helper transferred its
                    # pidfd. Its sudo monitor owns command termination/reaping.
                    try:
                        signal.pidfd_send_signal(monitor, signal.SIGTERM)
                        signal.pidfd_send_signal(monitor, signal.SIGCONT)
                    except ProcessLookupError:
                        pass
                try:
                    status = helper.wait(timeout=IDENTITY_EXCHANGE_TIMEOUT)
                except subprocess.TimeoutExpired:
                    if validator >= 0:
                        terminate_identity(validator)
                        if monitor >= 0:
                            try:
                                signal.pidfd_send_signal(monitor, signal.SIGCONT)
                            except ProcessLookupError:
                                pass
                    elif monitor >= 0:
                        signal.pidfd_send_signal(monitor, signal.SIGTERM)
                        signal.pidfd_send_signal(monitor, signal.SIGCONT)
                    status = helper.wait()
                if not transport_failed:
                    assert status == 0, 'Process identity validator failed'
            finally:
                if validator >= 0:
                    os.close(validator)
                if monitor >= 0:
                    os.close(monitor)

    def terminate(self):
        for row, handle in self.handles:
            terminate_identity(handle)
        self.exited()
