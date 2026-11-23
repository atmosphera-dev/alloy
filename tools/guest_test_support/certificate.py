# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Synthetic CA input executes in the NSS database owner's mount view."""
import os
from pathlib import Path
import subprocess


def stage_nss_certificate(pid, relative, certificate, account, log, env=None):
    # NSS/SQLite canonicalizes database paths itself. A /proc/PID/root alias is
    # not sufficient: perform the entire input operation in the observed view.
    relative = Path(relative)
    assert not relative.is_absolute() and '..' not in relative.parts
    directory = '/dev/shm/' + str(relative)
    prefix = ['sudo', '-n', 'nsenter', '--mount=/proc/' + str(pid) + '/ns/mnt',
              'sudo', '-n', '-u', account, 'env', 'TMPDIR=' + os.environ['TMPDIR'],
              'PYTHONDONTWRITEBYTECODE=1']
    code = ('import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); '
            'from guest_test_support.scratch import private_directory; '
            'private_directory(Path("/dev/shm"),sys.argv[2])')
    subprocess.run(prefix + ['python3', '-c', code, str(Path(__file__).resolve().parents[1]), str(relative)],
                   env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
    exists = subprocess.run(prefix + ['test', '-f', directory + '/cert9.db'], env=env,
                            stdout=log, stderr=subprocess.STDOUT, timeout=20).returncode
    assert exists in (0, 1), exists
    if exists:
        subprocess.run(prefix + ['certutil', '-N', '--empty-password', '-d', 'sql:' + directory],
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
    subprocess.run(prefix + ['certutil', '-A', '-d', 'sql:' + directory, '-n', 'Alloy guest fixture',
                             '-t', 'C,,', '-i', str(certificate)], env=env, stdout=log,
                   stderr=subprocess.STDOUT, check=True, timeout=20)
