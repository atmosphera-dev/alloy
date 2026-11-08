# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Real encrypted CEF socket/session retirement proof, approved NUC only."""
import argparse
import hashlib
import http.server
import itertools
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import threading
import time
import traceback
from urllib.parse import urlsplit
from urllib.request import urlopen

from workspace import PRIMARY, temp_path, validate_output, workspace_path
from profile_probe import CDP_HELPERS
import profile_system_cache as system_cache

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,first,second,eventLog,control,scenario]=process.argv.slice(1);
''' + CDP_HELPERS + r'''
async function events(){return(await readFile(eventLog,'utf8')).split('\n')
 .filter(l=>l.startsWith('SSL_EVENT ')).map(l=>JSON.parse(l.slice(10)))}
async function state(action='state'){return(await(await fetch(control+'/'+action)).json())}
const proof={scenario};let beta;
try {
 const pages=await until(async()=>{const p=await pageTargets();return p.length===2&&
   p.some(t=>t.url===first+'/page/initial')&&p.some(t=>t.url===second+'/page/initial')?p:null},'initial SSL browsers');
 const a=await connect(pages.find(t=>t.url.startsWith(first)));
 const b=await connect(pages.find(t=>t.url.startsWith(second)));beta=b;
 for(const c of [a,b]){await c.send('Page.enable');await until(async()=>
   await value(c,"document.readyState==='complete'&&document.body.dataset.site===location.hostname"),'native TLS page');}
 const initial=await events();
 check(initial.some(e=>e.event==='certificate'&&e.url.startsWith(first))&&
       initial.some(e=>e.event==='certificate'&&e.url.startsWith(second)),'real initial certificate callbacks missing');
 let audit=0,controlSequence=0;
 async function command(name){await value(b,'document.title='+JSON.stringify('SSL_PROBE:'+(++controlSequence)+':'+name)+';true')}
 async function settings(){const n=++audit;await command('audit');return until(async()=>{
   const e=await events(),setting=e.find(e=>e.event==='settings'&&e.sequence===n),
     list=e.find(e=>e.event==='embed-list'&&e.sequence===n);
   return setting&&list?{...setting,embedCount:list.count}:null;
 },'actual native SSL settings and embed-list');}
 proof.initialSettings=await settings();
 let nonce=0;
 async function frame(url){await value(b,`(()=>{window.sslFrame?.remove();
   window.sslReady=false;window.sslFrame=document.createElement('iframe');
   window.addEventListener('message',e=>{if(e.source===sslFrame.contentWindow&&
      e.origin===${JSON.stringify(first)}&&e.data.kind==='ssl-ready')window.sslReady=true});
   sslFrame.src=${JSON.stringify(url)};document.body.append(sslFrame);return true})()`);}
 async function frameEcho(label){return value(b,`new Promise((resolve,reject)=>{
   const nonce=${JSON.stringify('ssl-echo-'+(++nonce))};
   const timer=setTimeout(()=>{window.removeEventListener('message',receive);reject(Error('frame echo deadline'))},10000);
   function receive(e){if(e.source===sslFrame.contentWindow&&e.origin===${JSON.stringify(first)}&&e.data.nonce===nonce){
     clearTimeout(timer);window.removeEventListener('message',receive);resolve(e.data.result)}}
   window.addEventListener('message',receive);sslFrame.contentWindow.postMessage({kind:'ssl-echo',nonce,label:${JSON.stringify(label)}},${JSON.stringify(first)})})`)}
 const embedded=scenario.startsWith('embedded'),sessionCase=scenario.includes('resumed');
 if(embedded){await frame(first+'/page/embedded');await until(async()=>await value(b,'sslReady'),'listed alpha iframe');}
 async function alphaEcho(label){return embedded?frameEcho(label):value(a,
   'fetch('+JSON.stringify('/echo/'+label)+').then(r=>r.json())')}
 async function betaEcho(label){return value(b,'fetch('+JSON.stringify('/echo/'+label)+').then(r=>r.json())')}
 let alpha=await alphaEcho('warm1');const alphaAgain=await alphaEcho('warm2');
 check(alpha.host==='alpha.test'&&alphaAgain.host==='alpha.test','alpha native TLS SNI ownership missing');
 check(alpha.connection===alphaAgain.connection,'alpha pooled connection positive control '+JSON.stringify({alpha,alphaAgain}));
 proof.alphaPooled={alpha,alphaAgain};
 if(sessionCase){
   await state('close-alpha');const resumed=await alphaEcho('session-positive');
   check(resumed.connection!==alpha.connection&&resumed.resumed===true,'alpha TLS session positive control '+JSON.stringify(resumed));
   proof.alphaSession=resumed;alpha=resumed;
 }
 let warmBeta=await betaEcho('warm1');const betaAgain=await betaEcho('warm2');
 check(warmBeta.host==='beta.test'&&betaAgain.host==='beta.test','beta native TLS SNI ownership missing');
 check(warmBeta.connection===betaAgain.connection,'beta pooled connection positive control');
 if(sessionCase){
   await state('close-beta');const betaSession=await betaEcho('session-positive');
   check(betaSession.connection!==warmBeta.connection&&betaSession.resumed===true,'beta TLS session positive control '+JSON.stringify(betaSession));
   warmBeta=betaSession;
 }
 proof.betaBefore=warmBeta;
 if(sessionCase)await state('close-alpha');
 proof.beforeDelete=await state();
 const oldAlpha=proof.beforeDelete.open.filter(c=>c.host==='alpha.test').map(c=>c.connection);
 await command('arm');await until(async()=>(await events()).some(e=>e.event==='armed'),'certificate hold armed');
 await command('delete');await until(async()=>(await events()).some(e=>e.event==='deleted'),'actual deletion acknowledgement');
 const baseline=proof.beforeDelete.events.length;
 const freshPath=embedded?'/page/embedded-fresh':'/page/fresh';
 // Request fresh authority immediately; waiting for peer EOF first could hide
 // a delayed invalidation window after the native deletion acknowledgement.
 if(embedded)await frame(first+freshPath);
 else await command('fresh');
 proof.afterDelete=await state();proof.deletedSettings=await settings();
 const betaAfter=await betaEcho('after-delete');proof.betaAfter=betaAfter;
 check(betaAfter.connection===warmBeta.connection,'unrelated beta pooled connection lost '+JSON.stringify({warmBeta,betaAfter}));
 const outcome=await until(async()=>{
   const s=await state(),e=await events();proof.lastFreshState=s;
   const reached=s.events.filter(v=>v.kind==='request'&&v.host==='alpha.test'&&v.path===freshPath);
   if(reached.length)return{bypass:reached};
   const held=e.find(v=>v.event==='certificate'&&v.url===first+freshPath&&v.held);
   const denied=e.find(v=>v.event==='load-error'&&v.url===first+freshPath&&v.error===-202);
   return held||denied?{held,denied}:null;
 },'fresh alpha certificate challenge or denial');
 proof.freshOutcome=outcome;
 check(!outcome.bypass,'fresh alpha reached HTTP under retired certificate authority '+JSON.stringify(outcome));
 const handshakes=(await state()).events.slice(baseline).filter(v=>v.kind==='handshake'&&v.host==='alpha.test');
 proof.freshHandshakes=handshakes;
 check(handshakes.every(h=>h.resumed===false),'retired alpha TLS session reused or unobservable '+JSON.stringify(handshakes));
 await command('release');await until(async()=>(await events()).some(e=>e.event==='released'),'held decision cancelled');
 await until(async()=>(await events()).some(e=>e.event==='load-error'&&e.url===first+freshPath&&e.error===-202),'fresh request denied at native load consumer');
 proof.alphaSocketsClosed=await until(async()=>{const s=await state();proof.lastSocketState=s;
   return !s.open.some(c=>oldAlpha.includes(c.connection))?s:null},'retired alpha pooled sockets remained open');
 proof.finalSettings=await settings();
 if(sessionCase){
   await state('close-beta');const betaFinal=await betaEcho('session-after-delete');proof.betaFinal=betaFinal;
   check(betaFinal.connection!==warmBeta.connection&&betaFinal.resumed===true,'unrelated beta session cache lost '+JSON.stringify(betaFinal));
 } else {
   proof.betaFinal=await betaEcho('final');check(proof.betaFinal.connection===warmBeta.connection,'beta pool lost during denial');
 }
 proof.finalServer=await state();
 check(!proof.finalServer.events.some(e=>e.kind==='request'&&e.host==='alpha.test'&&e.path===freshPath),'denied alpha fetched HTTP body');
 await command('finish');a.ws.close();b.ws.close();
 console.log(JSON.stringify({pass_:true,...proof}));
} catch(error){
 proof.failure=String(error);proof.server=await state();proof.native=await events();
 console.log(JSON.stringify({pass_:false,...proof}));throw error;
}
'''


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, cert, key):
        super().__init__(('127.0.0.1', 0), Handler)
        self.lock = threading.Lock()
        self.ids = itertools.count(1)
        self.sockets, self.names, self.numbers, self.events = {}, {}, {}, []
        self.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.tls.minimum_version = self.tls.maximum_version = ssl.TLSVersion.TLSv1_2
        self.tls.load_cert_chain(cert, key)
        self.tls.set_servername_callback(self.sni)

    def sni(self, connection, name, context):
        with self.lock:
            self.names[id(connection)] = name

    def get_request(self):
        connection, peer = super().get_request()
        # Outlive the complete browser observation; fixture idle expiry must
        # never masquerade as engine-owned retirement of a pooled connection.
        connection.settimeout(300)
        wrapped = self.tls.wrap_socket(connection, server_side=True, do_handshake_on_connect=False)
        with self.lock:
            self.sockets[id(wrapped)] = wrapped
            self.numbers[id(wrapped)] = next(self.ids)
        return wrapped, peer

    def finish_request(self, request, address):
        error = None
        try:
            request.do_handshake()
        except (ssl.SSLError, OSError) as failure:
            error = str(failure)
        try:
            resumed = request.session_reused
        except (ValueError, OSError):
            resumed = None
        with self.lock:
            self.events.append(dict(kind='handshake', connection=self.numbers[id(request)],
                host=self.names.get(id(request)), resumed=resumed, completed=error is None, error=error))
        if error is None:
            super().finish_request(request, address)

    def shutdown_request(self, request):
        with self.lock:
            number = self.numbers.pop(id(request), None)
            host = self.names.pop(id(request), None)
            self.sockets.pop(id(request), None)
            self.events.append(dict(kind='closed', connection=number, host=host))
        super().shutdown_request(request)

    def snapshot(self):
        with self.lock:
            return dict(events=list(self.events), open=[dict(connection=self.numbers[k],
                host=self.names.get(k)) for k in self.sockets])

    def close_host(self, host):
        with self.lock:
            connections = [v for k,v in self.sockets.items() if host is None or self.names.get(k)==host]
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = urlsplit(self.path).path
        with self.server.lock:
            proof = dict(kind='request', path=path, connection=self.server.numbers[id(self.connection)],
                host=self.server.names.get(id(self.connection)), resumed=self.connection.session_reused)
            self.server.events.append(proof)
        if path.startswith('/echo/'):
            body, kind = json.dumps(proof).encode(), 'application/json'
        else:
            body = b'''<!doctype html><head><link rel="icon" href="data:,"></head><body><script>
document.body.dataset.site=location.hostname;
onmessage=async e=>{if(e.data.kind==='ssl-echo'){
 const result=await fetch('/echo/'+e.data.label).then(r=>r.json());
 e.source.postMessage({nonce:e.data.nonce,result},e.origin)}};
if(parent!==window)parent.postMessage({kind:'ssl-ready'},'*');
</script></body>'''
            kind = 'text/html'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


class Control(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/close-alpha':
            self.server.fixture.close_host('alpha.test')
        elif self.path == '/close-beta':
            self.server.fixture.close_host('beta.test')
        else:
            assert self.path == '/state'
        body = json.dumps(self.server.fixture.snapshot()).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--scenario', choices=['pooled', 'resumed', 'embedded', 'embedded-resumed'], required=True)
    parser.add_argument('--inside', action='store_true')
    args = parser.parse_args()
    assert os.environ['TMPDIR'] == str(temp_path(W))
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
            'flock', '--wait', '1800', str(PRIMARY/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-ssl-probe',
            'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1', 'python3',
            str(Path(__file__).resolve()), '--out', str(out), '--scenario', args.scenario, '--inside']
        return subprocess.run(['timeout', '--kill-after=30', '4500', *command], timeout=4540).returncode
    assert os.geteuid() == 1000
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    routes = subprocess.check_output(['ip', 'route'], text=True, timeout=10)
    assert [v['ifname'] for v in interfaces] == ['lo'] and not routes.strip()
    evidence = W/'evidence'/('ssl-transport-'+out.name+'-'+args.scenario+'-'+str(time.time_ns()))
    evidence.mkdir()
    print('SSL_TRANSPORT_EVIDENCE', evidence, flush=True)
    fixture = control = process = None
    try:
        save(evidence/'isolation.json', dict(interfaces=interfaces, routes=routes,
            uid=os.geteuid(), netns=os.readlink('/proc/self/ns/net')))
        cert, key = evidence/'certificate.pem', evidence/'fixture-key.pem'
        with (evidence/'certificate.log').open('x') as log:
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-sha256','-days','1',
                '-subj','/CN=alpha.test','-addext','subjectAltName=DNS:alpha.test,DNS:beta.test',
                '-keyout',str(key),'-out',str(cert)], stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
        fixture = Fixture(cert, key)
        control = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Control)
        control.fixture = fixture
        for server in (fixture, control):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        owned = {name:evidence/name for name in ('home','cache','config','data','runtime')}
        for path in owned.values():
            path.mkdir(mode=0o700)
        xdg_cache = system_cache.create(evidence)
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        first, second = [f'https://{host}:{fixture.server_port}' for host in ('alpha.test','beta.test')]
        argv = ['xvfb-run','-a','-s','-screen 0 1280x720x24',str(out/'alloy_encrypted_ssl_consumer'),
            '--alloy-no-disk','--off-screen-rendering-enabled','--ozone-platform=x11','--disable-gpu',
            '--remote-debugging-port='+str(port),
            '--host-resolver-rules=MAP alpha.test 127.0.0.1, MAP beta.test 127.0.0.1']
        assert not any('ignore-certificate-errors' in v for v in argv)
        save(evidence/'argv.json', argv)
        env = dict(os.environ, HOME=str(owned['home']), XDG_CACHE_HOME=str(xdg_cache),
            XDG_CONFIG_HOME=str(owned['config']), XDG_DATA_HOME=str(owned['data']),
            XDG_RUNTIME_DIR=str(owned['runtime']), ALLOY_SSL_PATH=str(evidence/'encrypted-profile'),
            ALLOY_SSL_FIRST=first, ALLOY_SSL_SECOND=second)
        with (evidence/'stdio.log').open('x') as log:
            process = subprocess.Popen(argv,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            deadline = time.monotonic()+90
            while True:
                assert process.poll() is None, 'native SSL consumer exited before DevTools readiness'
                try:
                    with urlopen(f'http://127.0.0.1:{port}/json/version',timeout=1) as response:
                        if json.load(response).get('webSocketDebuggerUrl'):
                            break
                except OSError:
                    pass
                assert time.monotonic()<deadline, 'native DevTools readiness'
                time.sleep(.05)
            driver = subprocess.run(['node','--input-type=module','-e',DRIVE,str(port),first,second,
                str(evidence/'stdio.log'),f'http://127.0.0.1:{control.server_port}',args.scenario],
                capture_output=True,text=True,timeout=180)
            (evidence/'driver.stdout').write_text(driver.stdout)
            (evidence/'driver.stderr').write_text(driver.stderr)
            if driver.stdout.strip():
                save(evidence/'browser-proof.json', json.loads(driver.stdout))
            assert driver.returncode==0, driver.stderr
            process.wait(timeout=90)
            assert process.returncode==0 and 'PASS encrypted-ssl-transport-consumer' in (evidence/'stdio.log').read_text()
        system_cache.check(xdg_cache)
        save(evidence/'server.json', fixture.snapshot())
        save(evidence/'result.json', dict(pass_=True,config=out.name,scenario=args.scenario,
            native_exit=process.returncode,driver_exit=driver.returncode,certificate_errors_ignored=False))
        return 0
    except BaseException:
        save(evidence/'failure.json',dict(pass_=False,config=out.name,scenario=args.scenario,
            error=traceback.format_exc(),server=fixture.snapshot() if fixture else None))
        raise
    finally:
        if process is not None and process.poll() is None:
            os.killpg(process.pid,signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL)
                process.wait(timeout=5)
        if fixture:
            fixture.close_host(None)
        for server in (control, fixture):
            if server:
                server.shutdown()
                server.server_close()
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink() and p.name!='SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in files))


if __name__ == '__main__':
    raise SystemExit(main())
