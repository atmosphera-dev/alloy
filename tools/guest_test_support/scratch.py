# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Private scratch observations; never retain a mount/directory capability."""
import json
from pathlib import Path
import stat
import subprocess


def scratch_view(pid):
    return Path('/proc') / str(pid) / 'root/dev/shm'


def private_directory(root, relative):
    """Validate beneath a proc magic-link root without realpath's view change."""
    relative = Path(relative)
    assert not relative.is_absolute() and '..' not in relative.parts, relative
    owner = root.stat()
    current = root
    for part in relative.parts:
        current = current / part
        info = current.lstat()
        assert stat.S_ISDIR(info.st_mode) and info.st_dev == owner.st_dev, current
        assert info.st_uid == owner.st_uid and info.st_mode & 0o777 == 0o700, current
    return current


def scratch_receipt(pid):
    code = r'''
import json,os,pathlib,sys
p=pathlib.Path('/proc')/sys.argv[1]
root=p/'root/dev/shm'; st=root.stat(); fs=os.statvfs(root)
lines=[line.split() for line in (p/'mountinfo').read_text().splitlines()]
rows=[line for line in lines if line[4]=='/dev/shm' and
      os.makedev(*map(int,line[2].split(':')))==st.st_dev]
assert len(rows)==1,rows
row=rows[0]; split=row.index('-')
assert row[split+1:split+3]==['tmpfs','alloy-scratch'],row
assert {'nosuid','nodev','noexec'}<=set(row[5].split(',')),row
assert not any(v.startswith(('shared:','master:')) for v in row[6:split]),row
assert st.st_dev!=os.stat('/dev/shm').st_dev
assert st.st_mode&0o777==0o700
assert 0<fs.f_blocks*fs.f_frsize<=1073741824
assert not list(pathlib.Path('/dev/shm').glob('alloy-guest-*'))
print(json.dumps(dict(pid=int(sys.argv[1]),device=st.st_dev,mount=row,
                     namespace=str((p/'ns/mnt').readlink()),limit=fs.f_blocks*fs.f_frsize)))
'''
    result = subprocess.run(['sudo', '-n', 'python3', '-c', code, str(pid)],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def scratch_references(receipt):
    # No observer in the ordinary proof keeps an FD open. Verify no process
    # mount view OR open file retains this exact tmpfs device after teardown.
    code = r'''
import errno,json,os,pathlib,select,sys
device=int(sys.argv[1]); refs=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdecimal(): continue
 handle=-1
 try:
  handle=os.pidfd_open(int(p.name))
  exited=select.poll(); exited.register(handle,select.POLLIN)
  if exited.poll(0): continue
  for line in (p/'mountinfo').read_text().splitlines():
   fields=line.split(); major,minor=map(int,fields[2].split(':'))
   if os.makedev(major,minor)==device: refs.append(dict(pid=int(p.name),mount=line))
  for fd in (p/'fd').iterdir():
   try:
    if fd.stat().st_dev==device: refs.append(dict(pid=int(p.name),fd=fd.name))
   except (FileNotFoundError,ProcessLookupError,PermissionError): pass
 except (FileNotFoundError,ProcessLookupError,PermissionError): pass
 except OSError as error:
  if error.errno!=errno.EINVAL or handle<0 or not exited.poll(0): raise
 finally:
  if handle>=0: os.close(handle)
print(json.dumps(refs))
'''
    result = subprocess.run(['sudo', '-n', 'python3', '-c', code, str(receipt['device'])],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def scratch_reclaimed(receipt):
    refs = scratch_references(receipt)
    assert not refs, refs
    assert not list(Path('/dev/shm').glob('alloy-guest-*'))
