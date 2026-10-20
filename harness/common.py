# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Shared disk-backed, append-only harness primitives. Execute only on the NUC."""
import datetime, fcntl, json, os, pathlib, subprocess, time, sys, threading
ROOT = pathlib.Path('/home/builder/work/alloy')
SCRIPTS = pathlib.Path(__file__).resolve().parent
TMP = '/home/builder/work/tmp'
CONFIG = json.loads(pathlib.Path(os.environ['ALLOY_HARNESS_CONFIG']).read_text()) if os.environ.get('ALLOY_HARNESS_CONFIG') else {}
LANES = CONFIG.get('lanes', [])
def guard():
    assert subprocess.check_output(['hostname'],text=True,timeout=5).strip()=='wakaba-build'
    assert os.environ.get('TMPDIR')==TMP
def now(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def run(args, timeout=30, check=True, **kw):
    r=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=timeout,**kw)
    if check and r.returncode: raise RuntimeError(f'{args!r}: {r.returncode}: {r.stdout} {r.stderr}')
    return r
def save(path, data):
    path=pathlib.Path(path); tmp=path.with_suffix(path.suffix+f'.next.{os.getpid()}.{threading.get_ident()}.{time.time_ns()}')
    tmp.write_text(json.dumps(data,indent=2,default=str)+'\n'); os.replace(tmp,path)
class Log:
    """New numbered segments, never a cyclic overwrite. Controller enforces total budget."""
    def __init__(self,path,max_bytes=16*1024*1024):
        self.path=pathlib.Path(path); self.max=max_bytes; self.fp=None; self.size=0; self.index=0
        self.mutex=threading.RLock()
        self.path.parent.mkdir(parents=True,exist_ok=True)
        while self.segment().exists(): self.index+=1
    def segment(self): return self.path if self.index==0 else self.path.with_name(self.path.stem+f'.{self.index:05d}'+self.path.suffix)
    def write(self,data):
        with self.mutex:self._write(data)
    def _write(self,data):
        if isinstance(data,str): data=data.encode()
        if self.fp is None or self.size+len(data)>self.max:
            if self.fp: self.fp.close(); self.index+=1
            while True:
                try:self.fp=self.segment().open('xb');break
                except FileExistsError:self.index+=1
            self.size=0
        self.fp.write(data); self.fp.flush(); self.size+=len(data)
    def event(self,event,**fields): self.write(json.dumps(dict(time=now(),epoch=time.time(),event=event,**fields),default=str)+'\n')
    def close(self):
        if self.fp: self.fp.close()
def lock(path):
    fp=open(path,'a+'); fcntl.flock(fp,fcntl.LOCK_EX|fcntl.LOCK_NB)
    fp.seek(0); fp.truncate(); fp.write(json.dumps(dict(pid=os.getpid(),time=now(),cgroup=pathlib.Path('/proc/self/cgroup').read_text()))); fp.flush()
    return fp
def unit(name,args, directory, user='builder', properties=None, env=None, runtime=12000):
    props={'WorkingDirectory':str(directory),'RuntimeMaxSec':str(runtime),'TimeoutStopSec':'90','KillMode':'control-group','UMask':'0077','TasksMax':'2048','MemoryMax':'12G','LimitCORE':'2G'}
    props.update(properties or {})
    cmd=['systemd-run','--unit='+name,'--uid='+user,'--service-type=exec','--quiet']
    for k,v in props.items(): cmd+=['--property='+k+'='+str(v)]
    for k,v in dict(TMPDIR=TMP,PYTHONUNBUFFERED='1',PYTHONDONTWRITEBYTECODE='1',ALLOY_HARNESS_CONFIG=os.environ['ALLOY_HARNESS_CONFIG'],**(env or {})).items(): cmd+=['--setenv='+k+'='+str(v)]
    run(cmd+list(map(str,args)),timeout=30)
def state(name):
    r=run(['systemctl','show',name,'-p','ActiveState,SubState,MainPID,ControlGroup,Result,ExecMainStatus,InvocationID,ExecMainStartTimestamp,NetworkNamespacePath,User,Environment'],check=False)
    return dict(line.split('=',1) for line in r.stdout.splitlines() if '=' in line)
def stop_owned_units(names,wait=0):
    """Independent stop jobs: one absent unit must not cancel the entire stop batch."""
    errors=[]
    for name in names:
        before=state(name)
        if before.get('MainPID')=='0' and not before.get('ControlGroup'):continue
        result=run(['systemctl','stop','--no-block',name],timeout=15,check=False)
        if result.returncode:errors.append(dict(unit=name,state=before,returncode=result.returncode,error=result.stderr))
    until=time.monotonic()+wait
    if wait:
        while time.monotonic()<until:
            live={name:s for name in names if (s:=state(name)).get('MainPID')!='0' or s.get('ControlGroup')}
            if not live:break
            time.sleep(.2)
        else:errors.append(dict(error='owned stop deadline reached',states=live))
    return errors
def records(path, stem):
    for f in sorted(pathlib.Path(path).glob(stem+'*.jsonl'),key=lambda p:(p.name!=stem+'.jsonl',p.name)):
        with f.open() as fp:
            for line in fp:
                try: yield json.loads(line)
                except json.JSONDecodeError: continue # raw partial line retained; gate separately counts malformed
if __name__=='__main__':
    guard()
    # Supervised command output streamer; process remains in this unit's cgroup.
    p=subprocess.Popen(sys.argv[3:],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=os.environ)
    out=Log(sys.argv[2])
    try:
        if 'dnsmasq' in sys.argv[3:]:
            while line:=p.stdout.readline(): out.write(now()+' '+line.decode(errors='replace'))
        else:
            while block:=p.stdout.read1(65536): out.write(block)
        sys.exit(p.wait(timeout=10))
    finally: out.close()
