# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Private app-test X connection, never the guest browser's display.

Xserver os/utils.c: -displayfd disables lock-file creation even for a non-root
server; -nolock alone does not. Both local/unix transports are disabled so no
filesystem socket directory is created. The authenticated TCP listener is
blocked from every non-loopback interface before the server is started.
"""
import os
from pathlib import Path
import secrets
import select
import socket
import subprocess


class PrivateDisplay:
    def __init__(self, root: Path, env):
        root.mkdir(mode=0o700)
        self.env = dict(env)
        self.child = None
        self.policy = self.cache_mounted = False
        self.table = 'alloy_m6_display_' + str(os.getpid())
        self.log = (root / 'server.log').open('xb')
        read_fd, write_fd = os.pipe()
        try:
            # Xserver xkb/ddxLoad.c otherwise falls back to its compiled-in RAM
            # temp directory when its fixed cache directory is not writable.
            # Bind the owned disk cache at that native owner, and restore it.
            self.cache = root / 'xkb-cache'
            self.cache.mkdir(mode=0o700)
            existing = subprocess.run(['findmnt', '-rn', '--mountpoint', '/var/lib/xkb'],
                                      env=self.env, capture_output=True, text=True)
            assert existing.returncode == 1, 'an existing XKB cache mount is not ours'
            subprocess.run(['sudo', '-n', 'mount', '--bind', str(self.cache), '/var/lib/xkb'],
                           env=self.env, check=True, stdout=self.log, stderr=subprocess.STDOUT)
            self.cache_mounted = True
            assert self.cache.samefile('/var/lib/xkb') and os.access('/var/lib/xkb', os.W_OK | os.X_OK)
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            assert port > 6000
            display = port - 6000
            auth = root / 'authority'
            auth.touch(mode=0o600)
            cookie = secrets.token_hex(16)
            subprocess.run(['xauth', '-f', str(auth), 'add', f'127.0.0.1:{display}',
                            'MIT-MAGIC-COOKIE-1', cookie], env=self.env, check=True,
                           stdout=self.log, stderr=subprocess.STDOUT)
            policy = (f'table inet {self.table} {{\n chain input {{\n'
                      f'  type filter hook input priority -5; policy accept;\n'
                      f'  iifname != "lo" tcp dport {port} drop;\n }}\n}}\n')
            subprocess.run(['sudo', '-n', 'nft', '-f', '-'], input=policy, text=True, check=True,
                           env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
            self.policy = True
            self.child = subprocess.Popen(['Xvfb', ':' + str(display), '-displayfd', str(write_fd),
                '-listen', 'tcp', '-nolisten', 'unix', '-nolisten', 'local', '-nolisten', 'inet6',
                '-auth', str(auth), '-noreset', '-screen', '0', '800x600x24', '-fp', 'built-ins'],
                env=self.env, pass_fds=(write_fd,), stdout=self.log, stderr=subprocess.STDOUT)
            os.close(write_fd)
            write_fd = -1
            assert select.select([read_fd], [], [], 60)[0], 'private display did not become ready'
            actual = os.read(read_fd, 64).strip()
            assert actual == str(display).encode(), (actual, self.child.poll())
            self.env.update(DISPLAY=f'127.0.0.1:{display}', XAUTHORITY=str(auth), XDG_SESSION_TYPE='x11')
            subprocess.run(['xkbcomp', self.env['DISPLAY'], '-'], env=self.env, check=True,
                           stdout=self.log, stderr=subprocess.STDOUT)
            subprocess.run(['sudo', '-n', 'nft', 'list', 'table', 'inet', self.table],
                           env=self.env, check=True, stdout=self.log, stderr=subprocess.STDOUT)
        except BaseException:
            self.close()
            raise
        finally:
            os.close(read_fd)
            if write_fd >= 0:
                os.close(write_fd)

    def close(self):
        if self.child and self.child.poll() is None:
            self.child.terminate()
            self.child.wait(timeout=30)
        if self.policy:
            subprocess.run(['sudo', '-n', 'nft', 'delete', 'table', 'inet', self.table],
                           env=self.env, check=True, stdout=self.log, stderr=subprocess.STDOUT)
            self.policy = False
        if self.cache_mounted:
            assert self.cache.samefile('/var/lib/xkb'), 'refusing to unmount an unowned cache'
            subprocess.run(['sudo', '-n', 'umount', '/var/lib/xkb'], env=self.env, check=True,
                           stdout=self.log, stderr=subprocess.STDOUT)
            self.cache_mounted = False
        self.log.close()
