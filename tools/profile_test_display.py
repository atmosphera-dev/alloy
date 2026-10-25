#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""An owned authenticated Xvfb fixture with a separate workload lifetime."""
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys


class Display:
    def __init__(self, label):
        assert socket.gethostname() == 'wakaba-build'
        assert os.environ['TMPDIR'] == '/home/builder/work/tmp'
        assert os.geteuid() == 1000 and label.isdigit()
        self.log = (Path('/home/builder/work/alloy/logs')/
                    ('sr3-display-'+label+'.log')).open('xb')
        bridge = '''import json,os,sys
print(json.dumps({key:os.environ[key] for key in ('DISPLAY','XAUTHORITY')}),flush=True)
sys.exit(0 if sys.stdin.readline()=='stop\\n' else 1)
'''
        self.process = subprocess.Popen(['systemd-run', '--user', '--quiet', '--pipe',
            '--unit=alloy-sr3-display-'+label, '--property=RuntimeMaxSec=2450',
            '--setenv=TMPDIR=/home/builder/work/tmp',
            '--setenv=PYTHONDONTWRITEBYTECODE=1',
            'xvfb-run', '-a', '-s', '-screen 0 1280x720x24',
            sys.executable, '-u', '-c', bridge], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.log, text=True)
        try:
            assert select.select([self.process.stdout], [], [], 30)[0], 'Xvfb startup deadline'
            line = self.process.stdout.readline()
            assert line, 'display fixture exited before readiness'
            self.environment = json.loads(line)
            assert set(self.environment) == {'DISPLAY', 'XAUTHORITY'}
            assert Path(self.environment['XAUTHORITY']).is_file()
        except BaseException:
            self.process.stdin.close()
            self.process.wait(timeout=30)
            self.log.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        try:
            self.process.communicate('stop\n', timeout=30)
            assert self.process.returncode == 0, 'display fixture shutdown failed'
        finally:
            self.log.close()
