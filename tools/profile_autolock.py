#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Profile auto-lock at the public SDK boundary on the assigned build guest."""
import argparse
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
import uuid
from urllib.request import urlopen

from workspace import PRIMARY, temp_path, validate_output, workspace_path
import profile_system_cache as system_cache
from profile_probe import CDP_HELPERS
from profile_control import page_control_script

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
DRIVE = r'''
import {readFile} from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const [port,eventLog,trigger,choice]=process.argv.slice(1);
''' + CDP_HELPERS + r'''
function stage(name){console.error('AUTOLOCK_STEP '+name)}
async function events(){return(await readFile(eventLog,'utf8')).split('\n').filter(s=>s.startsWith('AUTOLOCK_EVENT ')).map(s=>JSON.parse(s.slice(15)))}
async function processes(){
  // A process snapshot has no target/session lifetime to retain across profile
  // retirement. The unrelated control page below keeps its original connection.
  const metadata=await(await fetch(`http://127.0.0.1:${port}/json/version`)).json();
  const browser=await connect({id:'browser',webSocketDebuggerUrl:metadata.webSocketDebuggerUrl});
  try{return(await browser.send('SystemInfo.getProcessInfo')).processInfo.filter(p=>p.type==='renderer').map(p=>p.id)}
  finally{browser.ws.close()}
}
const controlTarget=await until(async()=>(await pageTargets()).find(t=>t.url.endsWith('/control')),'control browser');
let controlPage=await connect(controlTarget);
await until(async()=>await value(controlPage,"document.readyState==='complete'&&document.body.dataset.fixture==='control'"),'control ready');
await pageControl(controlPage);
const controlNonce=await value(controlPage,'globalThis.docNonce');
check(typeof controlNonce==='string','control document identity missing');
await until(async()=>(await events()).some(e=>e.event==='available'&&(e.value&62)===62),'all native triggers available on private bus');
const baseline=await processes();
stage('baseline-processes');
await control(controlPage,'start');
const managedTarget=await until(async()=>(await pageTargets()).find(t=>t.url.endsWith('/managed')),'managed page');
const page=await connect(managedTarget);
await until(async()=>await value(page,"window.workersReady===true"),'dedicated, shared and service workers');
const ownedPids=(await processes()).filter(pid=>!baseline.includes(pid));
stage('managed-workers-running');
check(ownedPids.length>0,'no original profile page process');
const targetsBefore=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
check(targetsBefore.some(t=>t.type==='worker')&&targetsBefore.some(t=>t.type==='shared_worker')&&targetsBefore.some(t=>t.type==='service_worker'),
  'actual workers missing '+JSON.stringify(targetsBefore));
const oldTargets=targetsBefore.filter(t=>t.id!==controlTarget.id).map(t=>t.id);
async function pendingNavigation(){
  await control(controlPage,'pending-navigation');
  await until(async()=>(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).some(t=>t.id===managedTarget.id),
    'original target retained during pending navigation');
  await until(async()=>await value(controlPage,"fetch('/pending-status').then(r=>r.json()).then(x=>x.pending)"),
    'server received pending navigation');
}
if(trigger==='explicit'||trigger==='close')await pendingNavigation();
if(trigger==='close'){
  await control(controlPage,'finish');
}else{
  if(trigger==='title-callback'){
    const request=profileControlRequest('title-callback-lock');
    try{await value(page,`document.title=${JSON.stringify(request.title)}`)}
    catch(error){if(!error.cdpTerminal)throw error} // The command retires this exact target.
    await profileControlReply(request);
  }else if(trigger==='navigation-callback'){
    await control(controlPage,'navigation-callback-lock');
  }else if(trigger==='explicit'){
    await control(controlPage,'explicit-lock');
  }else if(trigger==='idle'){
    await control(controlPage,'arm-idle');
    await sleep(1000);await control(controlPage,'input');
    await until(async()=>(await events()).some(e=>e.event==='native-input'),'native CEF input');
    await sleep(1000);check(!(await events()).some(e=>e.event==='locked'),'input did not restart idle period');
    await control(controlPage,'embedder-input');
    await sleep(1000);check(!(await events()).some(e=>e.event==='locked'),'embedder input did not restart idle period');
  }else if(trigger==='embedder-sleep'){
    await control(controlPage,'embedder-sleep');
  }else{
    await promisify(execFile)('dbus-send',['--session','--print-reply','--dest=org.alloy.AutoLockFixture',
      '/org/alloy/Test','org.alloy.AutoLockFixture.Trigger','string:'+trigger]);
  }
  const expected={explicit:0,'title-callback':0,'navigation-callback':0,idle:2,sleep:3,screen:4,switch:5,'logind-lock':4,'embedder-sleep':3}[trigger];
  await until(async()=>(await events()).some(e=>e.event==='locked'&&e.value===expected),'lock acknowledgement and refused read');
  if(trigger==='explicit')await until(async()=>(await events()).some(e=>e.event==='late-context-refused'),
    'queued native context refused after navigation admission closed');
  if(trigger.endsWith('-callback')){
    const receipts=await events();
    check(receipts.some(e=>e.event===trigger+'-returned'),'observer callback did not return before teardown');
    check(receipts.filter(e=>e.event==='explicit-complete').length===2,'joined locks did not both complete');
    check(receipts.filter(e=>e.event==='callback'&&e.value===0).length===1,'joined locks duplicated retirement');
    check(receipts.some(e=>e.event==='pending-writers-refused'),'synchronous pending-lock mutation refusal missing');
    check(receipts.filter(e=>e.event==='pending-mutation-refused').length===4,'pending-lock mutation callbacks did not all refuse');
  }
  stage('lock-acknowledged');
  const afterPids=await processes();
  stage('locked-processes');
  check(!ownedPids.some(pid=>afterPids.includes(pid)),'original page process survived lock callback '+JSON.stringify({ownedPids,afterPids}));
  const afterTargets=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  check(!afterTargets.some(t=>oldTargets.includes(t.id)),'original page/worker survived acknowledgement '+JSON.stringify(afterTargets));
  const survivingControl=afterTargets.find(t=>t.id===controlTarget.id);
  check(survivingControl,'unrelated control target retired');
  controlPage.ws.close();controlPage=await connect(survivingControl);
  check(await value(controlPage,"document.body.dataset.fixture==='control'&&globalThis.docNonce==="+JSON.stringify(controlNonce)),
    'unrelated control document was replaced or did not survive');
  stage('control-survived');
  if(choice==='locked-page'){
    const target=await until(async()=>(await pageTargets()).find(t=>t.url.endsWith('/locked')),'embedder locked page');
    const locked=await connect(target);
    check(await until(async()=>await value(locked,"document.body.textContent==='Profile locked'"),'locked page content'), 'locked page failed');
    locked.ws.close();
  }else check(!(await pageTargets()).some(t=>t.url.endsWith('/locked')),'close choice created a locked tab');
  await control(controlPage,'unlock');
  stage('unlock-requested');
  await until(async()=>(await events()).some(e=>e.event==='unlocked'),'readback after explicit password unlock');
  if(trigger==='explicit')check((await events()).some(e=>e.event==='completed-write-readback'),'completed write missing after unlock');
  if(trigger.endsWith('-callback'))await until(async()=>(await events()).some(e=>e.event==='pending-mutations-not-persisted'),
    'refused mutations absent from reopened filters, preferences and site record');
  await until(async()=>(await pageTargets()).some(t=>t.url.endsWith('/reopened')),'fresh context after unlock');
  await control(controlPage,'finish');
}
for(const c of [page,controlPage])c.ws.close();
console.log(JSON.stringify({trigger,choice,ownedPids,oldTargets,events:await events()}));
'''


class Handler(http.server.BaseHTTPRequestHandler):
    pending = threading.Event()
    def log_message(self, *args):
        pass

    def do_GET(self):
        kind = 'text/html'
        if self.path == '/pending':
            Handler.pending.set()
            time.sleep(30)
            body = b'<!doctype html><body>Pending navigation completed'
        elif self.path == '/pending-status':
            kind = 'application/json'
            body = json.dumps(dict(pending=Handler.pending.is_set())).encode()
        elif self.path.endswith('.js'):
            kind = 'application/javascript'
            body = {
                '/dedicated.js': b'onmessage=e=>postMessage(e.data)',
                '/shared.js': b'onconnect=e=>{const p=e.ports[0];p.onmessage=e=>p.postMessage(e.data);p.start()}',
                '/service.js': b'oninstall=e=>skipWaiting();onactivate=e=>e.waitUntil(clients.claim());onmessage=e=>e.ports[0].postMessage(e.data)',
            }[self.path]
        elif self.path == '/locked':
            body = b'<!doctype html><body>Profile locked'
        else:
            body = ('''<!doctype html><body data-fixture="%s"><script>globalThis.docNonce=crypto.randomUUID();
            if(location.pathname==='/managed'){
              // Script-generated events must never reset the trusted-input timer.
              setInterval(()=>dispatchEvent(new KeyboardEvent('keydown',{key:'a'})),100);
              (async()=>{
                const d=new Worker('/dedicated.js');await new Promise(r=>{d.onmessage=r;d.postMessage('ready')});
                const s=new SharedWorker('/shared.js','autolock');s.port.start();await new Promise(r=>{s.port.onmessage=r;s.port.postMessage('ready')});
                const sw=await navigator.serviceWorker.register('/service.js');await navigator.serviceWorker.ready;
                await new Promise(r=>{const c=new MessageChannel();c.port1.onmessage=r;sw.active.postMessage('ready',[c.port2])});
                window.owned=[d,s,sw];window.workersReady=true;
              })();
            }</script>''' % self.path.removeprefix('/')).encode()
            if self.path == '/control':
                body += ('<script>'+page_control_script('ready')+'</script>').encode()
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when security retirement cancels the held response.


def wait_for(predicate, process, why, timeout=60):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        assert process.poll() is None, why+' process exited early'
        time.sleep(.1)
    raise RuntimeError(why+' timed out')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=['autolock'], default='autolock')
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--case', choices=['title-callback', 'navigation-callback', 'explicit', 'idle', 'sleep', 'screen', 'logind-lock', 'switch', 'embedder-sleep', 'close'])
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(W, out, REPO)
    assert os.environ['TMPDIR'] == str(temp_path(W))
    if not args.inside:
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
            'flock', '--wait', '1800', str(PRIMARY/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-autolock',
            'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1', 'python3',
            str(Path(__file__).resolve()), '--out', str(out), '--inside']
        if args.case:
            command += ['--case', args.case]
        return subprocess.run(['timeout', '--kill-after=30', '1800', *command], timeout=1840).returncode
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], text=True))
    assert [row['ifname'] for row in interfaces] == ['lo']
    assert not subprocess.check_output(['ip', 'route'], text=True).strip()
    evidence = W/'evidence'/('profile-autolock-'+out.name+'-'+uuid.uuid4().hex[:8])
    evidence.mkdir()
    print('AUTOLOCK_EVIDENCE', evidence, flush=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    cases = [args.case] if args.case else ['title-callback', 'navigation-callback', 'explicit', 'idle', 'sleep', 'screen', 'logind-lock', 'switch', 'embedder-sleep', 'close']
    try:
        for trigger in cases:
            for choice in (['close'] if trigger == 'close' else ['close', 'locked-page']):
                Handler.pending.clear()
                root = evidence/(trigger+'-'+choice)
                root.mkdir()
                for name in ('home', 'config', 'data'):
                    (root/name).mkdir(mode=0o700)
                cache = system_cache.create(root)
                runtime = system_cache.create_runtime(root)
                address = 'unix:path='+str(temp_path(W)/('al-'+uuid.uuid4().hex[:10]+'.bus'))
                bus = subprocess.check_output(['dbus-daemon', '--session', '--fork', '--address='+address,
                    '--print-address=1', '--print-pid=1'], text=True).splitlines()
                bus_pid = int(bus[-1])
                env = dict(os.environ, HOME=str(root/'home'), XDG_CONFIG_HOME=str(root/'config'),
                    XDG_DATA_HOME=str(root/'data'), XDG_CACHE_HOME=str(cache), XDG_RUNTIME_DIR=str(runtime),
                    DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address,
                    ALLOY_AUTOLOCK_ROOT=str(root), ALLOY_AUTOLOCK_BASE=f'http://127.0.0.1:{server.server_port}',
                    ALLOY_AUTOLOCK_CHOICE=choice)
                fixture = process = None
                try:
                    with (root/'bus.log').open('x') as bus_log, (root/'consumer.log').open('x') as log:
                        fixture = subprocess.Popen(['/usr/bin/python3', str(REPO/'tools/testdata/autolock_service.py')],
                            env=env, stdout=bus_log, stderr=subprocess.STDOUT)
                        wait_for(lambda:'READY_AUTOLOCK_BUS' in (root/'bus.log').read_text(), fixture, 'private signal service')
                        with socket.socket() as reserve:
                            reserve.bind(('127.0.0.1', 0)); port = reserve.getsockname()[1]
                        argv = ['xvfb-run', '-a', str(out/'alloy_profile_auto_lock_consumer'), '--alloy-no-disk',
                            '--ozone-platform=x11', '--disable-gpu', '--remote-debugging-port='+str(port)]
                        process = subprocess.Popen(argv, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
                        wait_for(lambda:'control-created' in (root/'consumer.log').read_text(), process, 'CEF control')
                        driver = subprocess.run(['node', '--input-type=module', '-e', DRIVE,
                            str(port), str(root/'consumer.log'), trigger, choice], env=env, text=True,
                            capture_output=True, timeout=150)
                        (root/'driver.stdout').write_text(driver.stdout)
                        (root/'driver.stderr').write_text(driver.stderr)
                        (root/'driver.json').write_text(json.dumps(dict(argv=argv, exit=driver.returncode,
                            consumer_exit_at_driver_completion=process.poll()))+'\n')
                        if driver.returncode:
                            time.sleep(1)
                            observation = dict(consumer_exit_after_one_second=process.poll())
                            for endpoint in ('version', 'list'):
                                try:
                                    with urlopen(f'http://127.0.0.1:{port}/json/{endpoint}', timeout=3) as response:
                                        observation[endpoint] = json.load(response)
                                except Exception as error:
                                    observation[endpoint] = str(error)
                            (root/'failure-observation.json').write_text(json.dumps(observation, indent=2)+'\n')
                        assert driver.returncode == 0, str(root/'driver.stderr')
                        exit_code = process.wait(timeout=90)
                        assert exit_code == 0, str(root/'consumer.log')
                    assert 'PASS profile-auto-lock-consumer' in (root/'consumer.log').read_text()
                    if trigger == 'close':
                        with (root/'close-write-readback.log').open('x') as readback:
                            reopened = subprocess.run(argv, cwd=out, env=dict(env, ALLOY_AUTOLOCK_REOPEN='1'),
                                stdout=readback, stderr=subprocess.STDOUT, timeout=90)
                        assert reopened.returncode == 0, str(root/'close-write-readback.log')
                        assert 'completed-write-readback' in (root/'close-write-readback.log').read_text()
                    assert 'SESSION_BY_PID' in (root/'bus.log').read_text()
                    proof = json.loads(driver.stdout)
                    for pid in proof['ownedPids']:
                        assert not Path('/proc', str(pid)).exists(), 'profile page process survived browser close'
                    system_cache.check(cache); system_cache.check_runtime(runtime)
                    print('PASS autolock-'+trigger+'-'+choice, flush=True)
                finally:
                    if process and process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM); process.wait(timeout=20)
                    if fixture and fixture.poll() is None:
                        fixture.terminate(); fixture.wait(timeout=20)
                    os.kill(bus_pid, signal.SIGTERM)
        (evidence/'result.json').write_text(json.dumps(dict(passed=True, cases=cases, config=out.name))+'\n')
        print('PASS autolock', flush=True)
        return 0
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    raise SystemExit(main())
