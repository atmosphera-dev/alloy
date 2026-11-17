# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Observe actual Fontconfig, NSS and memory GSettings native consumers."""
import json
import os
from pathlib import Path
import subprocess
from gi.repository import Gio

scratch = Path(os.environ['HOME']).parent
subprocess.run(['fc-cache', '-f'], check=True)
caches = list((Path(os.environ['XDG_CACHE_HOME']) / 'fontconfig').glob('*cache-*'))
assert caches and all(path.stat().st_size > 0 for path in caches), caches
databases = []
for directory in (Path(os.environ['HOME']) / '.pki/nssdb',
                  Path(os.environ['XDG_DATA_HOME']) / 'pki/nssdb'):
    subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:' + str(directory)], check=True)
    subprocess.run(['certutil', '-L', '-d', 'sql:' + str(directory)], check=True)
    assert (directory / 'cert9.db').is_file()
    assert (directory / 'key4.db').is_file()
    databases.append(str(directory.relative_to(scratch)))
assert os.environ['GSETTINGS_BACKEND'] == 'memory'
settings = Gio.Settings.new('org.gtk.Settings.FileChooser')
original = settings.get_boolean('sort-directories-first')
assert settings.set_boolean('sort-directories-first', not original)
Gio.Settings.sync()
assert settings.get_boolean('sort-directories-first') is not original
assert not (Path(os.environ['XDG_CONFIG_HOME']) / 'dconf/user').exists()
print('GUEST_NATIVE_WRITERS ' + json.dumps(dict(font_caches=len(caches), nss=databases,
      memory_gsettings_readback=True)), flush=True)
