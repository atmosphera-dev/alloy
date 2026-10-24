#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only real network-loaded dedicated, nested and shared worker regression."""
import argparse
import errno
import hashlib
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
import traceback

from workspace import namespace_prefix, temp_path, validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
TMP = str(temp_path(W))
PAGE = b'''<!doctype html><title>WORKER_PENDING</title><script>
const dedicated=url=>new Promise((resolve,reject)=>{
  const w=new Worker(url);w.onmessage=e=>{w.terminate();resolve(e.data)};
  w.onerror=e=>{w.terminate();reject(Error(e.message||'worker script failed'))};
});
const shared=new Promise((resolve,reject)=>{const w=new SharedWorker('/shared.js');
  w.port.onmessage=e=>{w.port.close();resolve(e.data)};w.onerror=()=>reject(Error('shared script failed'))});
Promise.all([dedicated('/dedicated.js'),dedicated('/nested.js'),shared]).then(v=>{
  if(JSON.stringify(v)!==JSON.stringify(['dedicated-ok','nested-child-ok','shared-ok']))throw Error(JSON.stringify(v));
  document.title='ALLOY_WORKER_PASS';
}).catch(e=>{document.title='ALLOY_WORKER_FAIL:'+e.message});
</script>'''
SCRIPTS = {
    '/dedicated.js': b"postMessage('dedicated-ok');",
    '/nested-child.js': b"postMessage('child-ok');",
    '/nested.js': b"const w=new Worker('/nested-child.js');w.onmessage=e=>{w.terminate();postMessage('nested-'+e.data)};w.onerror=()=>{throw Error('nested child failed')};",
    '/shared.js': b"onconnect=e=>e.ports[0].postMessage('shared-ok');",
}
DRIVE = r'''
const [port,url]=process.argv.slice(1),sleep=ms=>new Promise(r=>setTimeout(r,ms));
let targets=[],ws;
try{
  for(let i=0;i<200;i++){try{targets=(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).filter(t=>t.type==='page')}catch{}
    if(targets.some(t=>t.url===url))break;await sleep(100)}
  const t=targets.find(t=>t.url===url);if(!t)throw Error('native page target missing');
  const pending=new Map();let id=0;ws=new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject});
  ws.onmessage=e=>{const m=JSON.parse(e.data),p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result)}};
  const send=(method,params={})=>new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}))});
  let title='';for(let i=0;i<200;i++){const r=await send('Runtime.evaluate',{expression:'document.title',returnByValue:true});title=r.result.value;
    if(title.startsWith('ALLOY_WORKER_'))break;await sleep(100)}
  console.log(JSON.stringify({title}));if(title!=='ALLOY_WORKER_PASS')throw Error(title);
}finally{
  if(ws)ws.close();
  for(const t of targets){const r=await fetch(`http://127.0.0.1:${port}/json/close/${t.id}`);if(!r.ok)throw Error('native close refused')}
}
'''


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.events_lock:
            self.server.paths.append(self.path)
            save(self.server.evidence/'requests.json', self.server.paths)
        body = PAGE if self.path=='/page' else SCRIPTS.get(self.path, b'')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html' if self.path=='/page' else 'application/javascript')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    assert os.environ['TMPDIR']==TMP
    out = args.out.resolve()
    validate_output(W,out,REPO)
    if not args.inside:
        command = namespace_prefix(W) + ['unshare','--net','--fork','/bin/sh','-c',
            'ip link set lo up && exec runuser -u builder -- "$@"','alloy-worker-probe',
            'env','TMPDIR='+TMP,'PYTHONDONTWRITEBYTECODE=1',
            'python3',str(Path(__file__).resolve()),'--out',str(out),'--inside']
        return subprocess.run(['timeout','--kill-after=30','150',*command], timeout=190).returncode
    assert os.geteuid()==1000
    interfaces = json.loads(subprocess.check_output(['ip','-j','addr'],timeout=10))
    routes = subprocess.check_output(['ip','route'],text=True,timeout=10)
    assert [v['ifname'] for v in interfaces]==['lo'] and not routes.strip()
    with socket.socket() as s:
        assert s.connect_ex(('1.1.1.1',443))==errno.ENETUNREACH
    evidence = W/'evidence'/('worker-probe-'+out.name+'-'+str(time.time_ns()))
    evidence.mkdir()
    save(evidence/'isolation.json',dict(interfaces=interfaces,routes=routes,
         netns=os.readlink('/proc/self/ns/net'),uid=os.geteuid()))
    print('WORKER_PROBE_EVIDENCE',evidence,flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    server.daemon_threads=True
    server.paths=[]
    server.events_lock=threading.Lock()
    server.evidence=evidence
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
    url='http://127.0.0.1:'+str(server.server_port)+'/page'
    argv=['xvfb-run','-a','-s','-screen 0 1280x720x24',str(out/'cefclient'),
          '--off-screen-rendering-enabled','--ozone-platform=x11','--disable-gpu',
          '--cache-path='+str(evidence/'profile'),'--url='+url,
          '--remote-debugging-port='+str(port)]
    save(evidence/'argv.json',argv)
    try:
        with (evidence/'stdio.log').open('xb') as log:
            process=subprocess.Popen(argv,cwd=out,stdout=log,stderr=subprocess.STDOUT,
                                     start_new_session=True)
            try:
                result=subprocess.run(['node','--input-type=module','-e',DRIVE,str(port),url],
                    capture_output=True,text=True,timeout=50)
                (evidence/'driver.stdout').write_text(result.stdout)
                (evidence/'driver.stderr').write_text(result.stderr)
                process.wait(timeout=25)
                assert process.returncode==0, process.returncode
                assert result.returncode==0, result.stderr
                assert set(SCRIPTS).issubset(server.paths),server.paths
                save(evidence/'result.json',dict(pass_=True,config=out.name,
                     paths=server.paths,normal_shutdown=True,driver=json.loads(result.stdout)))
            finally:
                if process.poll() is None:
                    os.killpg(process.pid,signal.SIGTERM)
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
        print('PASS network-dedicated-nested-shared-workers',flush=True)
        return 0
    except BaseException:
        (evidence/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        server.shutdown();server.server_close()
        files=[p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink()]
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in files))


if __name__=='__main__':
    raise SystemExit(main())
