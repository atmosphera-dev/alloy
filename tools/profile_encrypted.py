# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only encrypted native Profile/CEF end-consumer proof on two real sites."""
import argparse
import base64
import hashlib
import http.server
import json
import os
import signal
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback
from urllib.request import urlopen

from workspace import PRIMARY, temp_path, validate_output, workspace_path
import profile_system_cache as system_cache
import profile_retirement
from profile_probe import CDP_HELPERS, NOISE_FUNCTION

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,first,second,phase,eventLog]=process.argv.slice(1);
''' + CDP_HELPERS + profile_retirement.HELPERS + r'''
async function events(){return(await readFile(eventLog,'utf8')).split('\n')
  .filter(s=>s.startsWith('ENCRYPTED_EVENT ')).map(s=>JSON.parse(s.slice(16)))}
const pages=await until(async()=>{const t=await pageTargets();
  return t.length===2&&t.every(p=>p.url.startsWith(first)||p.url.startsWith(second))?t:null},'two encrypted native browsers');
const sockets=[];
for(const url of [first,second]){
  const target=pages.find(p=>p.url.startsWith(url));check(target,'site target '+url);
  const c=await connect(target);sockets.push(c);await c.send('Page.enable');
  await until(async()=>await value(c,"document.readyState==='complete'&&document.body.dataset.site===location.hostname"),'site document '+url);
}
const [a,b]=sockets;
let controlSerial=0;
async function sendCommand(c,name){
  return value(c,'document.title='+JSON.stringify('ALLOY_ENCRYPTED:'+(++controlSerial)+':'+name)+';true');
}
async function noise(c){return value(c,`(async()=>{
  const draw=__NOISE_FUNCTION__;
  const first=draw(document.createElement('canvas')),repeat=draw(document.createElement('canvas'));
  const worker=await new Promise((resolve,reject)=>{const w=new Worker('/noise-worker.js');
    w.onmessage=e=>{w.terminate();resolve(e.data)};w.onerror=e=>{w.terminate();reject(Error(e.message))};w.postMessage('draw')});
  return {first,repeat,worker};})()`)}
if(phase==='write'||phase==='live'||phase==='all-live'){
  await sendCommand(a,'write');
  await until(async()=>(await events()).some(e=>e.event==='written'),'two native settings/list durable completions');
}
const observations=[];
for(const c of sockets) observations.push(await value(c,`(async()=>({site:location.hostname,
  permission:(await navigator.permissions.query({name:'geolocation'})).state,
  zoom:devicePixelRatio}))()`));
const allDeleted=phase==='after-delete-all'||phase==='after-all-live';
const deleted=phase==='after-delete'||phase==='after-live-delete'||allDeleted;
check(observations[0].permission===(deleted?'prompt':'granted'),'first native permission '+JSON.stringify(observations));
check(observations[1].permission===(allDeleted?'prompt':'denied'),'second native permission '+JSON.stringify(observations));
check(Math.abs(observations[0].zoom-(deleted?1:1.2))<.001,'first actual renderer zoom '+JSON.stringify(observations));
check(Math.abs(observations[1].zoom-(allDeleted?1:1.44))<.001,'second actual renderer zoom '+JSON.stringify(observations));
let live=null;
if(phase==='live'){
  const alphaRecord=await seedRecord(a),betaRecord=await seedRecord(b);
  checkRetainedRecord(await readRecord(a),alphaRecord);
  checkRetainedRecord(await readRecord(b),betaRecord);
  const alphaNoise=await noise(a),betaNoise=await noise(b);
  for(const identity of [alphaNoise,betaNoise])check(identity.first===identity.repeat&&identity.first===identity.worker,
    'native page/worker farbling identity inconsistent '+JSON.stringify(identity));
  for(const c of sockets) await value(c,`(async()=>{
    window.ownedDedicated=new Worker('/dedicated.js');
    await new Promise(resolve=>{ownedDedicated.onmessage=resolve;ownedDedicated.postMessage('start')});
    window.ownedShared=new SharedWorker('/shared.js','owned-'+location.hostname);ownedShared.port.start();
    await new Promise(resolve=>{ownedShared.port.onmessage=resolve;ownedShared.port.postMessage('ready')});
    return true;})()`);
  await value(b,`new Promise(resolve=>{const frame=document.createElement('iframe');
    frame.src=${JSON.stringify(first+'/page/embedded')};frame.onload=resolve;document.body.append(frame)})`);
  await value(a,`(async()=>{const registration=await navigator.serviceWorker.register('/service.js');
    await navigator.serviceWorker.ready;return new Promise(resolve=>{const channel=new MessageChannel();
    channel.port1.onmessage=resolve;registration.active.postMessage('ready',[channel.port2])})})()`);
  const before=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
  // CEF's dedicated targets can omit their URL even after the worker has
  // executed. Their browser-owned parentId is canonical creation ownership;
  // nested workers point to their creator worker rather than a page target.
  const ancestry=new Map(before.map(t=>[t.id,t]));
  function ownsTarget(target,seen=new Set()){
    if(seen.has(target.id))throw Error('cyclic native target ancestry');
    seen.add(target.id);
    if(target.url){
      const origin=new URL(target.url).origin;
      if(origin!=='null')return origin===new URL(first).origin;
    }
    const parent=ancestry.get(target.parentId);
    return !!parent&&ownsTarget(parent,seen);
  }
  const ownedBefore=before.filter(t=>ownsTarget(t));
  const dedicated=ownedBefore.filter(t=>t.type==='worker');
  check(dedicated.length===2&&dedicated.some(t=>dedicated.some(parent=>parent.id===t.parentId)),
    'actual dedicated/nested alpha ownership missing '+JSON.stringify(before));
  check(ownedBefore.some(t=>t.type==='shared_worker')&&ownedBefore.some(t=>t.type==='service_worker'),
    'actual alpha shared/service worker missing '+JSON.stringify(before));
  check(ownedBefore.filter(t=>['worker','shared_worker','service_worker'].includes(t.type)).length>=4,
    'actual alpha workers did not start '+JSON.stringify(before));
  const retiredIds=new Set(ownedBefore.map(t=>t.id));
  await value(a,`window.retirementWriter=setInterval(()=>{
    const marker='record-'+location.hostname;
    localStorage.setItem('record-value',marker);sessionStorage.setItem('record-value',marker);
    document.cookie='record_value='+marker+'; Path=/; Secure; SameSite=Lax';
    try{retainedRecordDb.transaction('values','readwrite').objectStore('values').put(marker,'value')}catch{}
    retainedRecordCache.put('/record-cache',new Response(marker)).catch(()=>{});
  },10);true`);
  await sendCommand(a,'delete');
  await until(async()=>(await events()).some(e=>e.event==='deleted'),'key/native teardown/unlink completion');
  const after=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
  for(const target of after)ancestry.set(target.id,target);
  check(!after.some(t=>retiredIds.has(t.id)||ownsTarget(t)),
    'alpha target survived acknowledged deletion '+JSON.stringify(after));
  const tree=await b.send('Page.getFrameTree');
  check(!(tree.frameTree.childFrames??[]).some(t=>t.frame.url.startsWith(first)),'embedded alpha frame survived');
  check(await value(b,"location.hostname==='beta.test'&&document.readyState==='complete'"),'unrelated beta document lost');
  const worker=await value(b,`new Promise(resolve=>{ownedShared.port.onmessage=e=>resolve(e.data);
    ownedShared.port.postMessage('still-alive')})`);
  check(worker==='still-alive','unrelated beta worker lost');
  const beta=await value(b,"navigator.permissions.query({name:'geolocation'}).then(p=>({permission:p.state,zoom:devicePixelRatio}))");
  check(beta.permission==='denied'&&Math.abs(beta.zoom-1.44)<.001,'beta settings changed on live deletion');
  const retainedNoise=await noise(b);
  check(JSON.stringify(retainedNoise)===JSON.stringify(betaNoise),'unrelated beta farbling identity changed');
  await sendCommand(b,'recreate');
  const freshTarget=await until(async()=>{const targets=await pageTargets();
    return targets.find(t=>t.url===first+'/page/fresh')},'explicit fresh alpha navigation');
  check(!retiredIds.has(freshTarget.id),'fresh navigation reused a retired target');
  const fresh=await connect(freshTarget);sockets.push(fresh);
  await until(async()=>await value(fresh,"document.readyState==='complete'&&document.body.dataset.site===location.hostname"),'fresh alpha document');
  const freshNoise=await noise(fresh);
  check(freshNoise.first===freshNoise.repeat&&freshNoise.first===freshNoise.worker,'fresh page/worker identity inconsistent');
  check(freshNoise.first!==alphaNoise.first,'deleted alpha farbling identity was reconstructed');
  const freshSettings=await value(fresh,"navigator.permissions.query({name:'geolocation'}).then(p=>({permission:p.state,zoom:devicePixelRatio}))");
  check(freshSettings.permission==='prompt'&&Math.abs(freshSettings.zoom-1)<.001,'fresh alpha inherited retired settings');
  const freshRecord=await readRecord(fresh),retainedRecord=await readRecord(b);
  checkRetiredRecord(freshRecord,alphaRecord);
  checkRetainedRecord(retainedRecord,betaRecord);
  live={before,after,tree,beta,alphaNoise,betaNoise,retainedNoise,freshNoise,freshSettings,freshTarget,
    alphaRecord,betaRecord,freshRecord,retainedRecord};
}
let allLive=null,allReader=null;
if(phase==='all-live'){
  async function catalogue(c){
    const before=(await events()).filter(e=>e.event==='records').length;
    await sendCommand(c,'list-records');
    return until(async()=>{const found=(await events()).filter(e=>e.event==='records');
      return found.length>before?found.at(-1).records:null},'public record catalogue');
  }
  const initial=await catalogue(a);
  check(initial.length===2&&['alpha.test','beta.test'].every(domain=>initial.some(r=>r.domain===domain&&r.last_visit>0)),
    'public domain/committed-last-visit metadata '+JSON.stringify(initial));
  const seeded=[];
  for(const c of [a,b]){
    seeded.push(await seedRecord(c));
    await value(c,`window.deleteAllWrites=0;window.deleteAllWriter=setInterval(()=>{
      const marker='record-'+location.hostname;
      localStorage.setItem('record-value',marker);sessionStorage.setItem('record-value',marker);
      document.cookie='record_value='+marker+'; Path=/; Secure; SameSite=Lax';
      try{retainedRecordDb.transaction('values','readwrite').objectStore('values').put(marker,'value')}catch{}
      retainedRecordCache.put('/record-cache',new Response(marker)).catch(()=>{});
      ++deleteAllWrites;
    },10);true`);
    await until(async()=>await value(c,'deleteAllWrites>=2'),'actual live tab writes');
  }
  const afterWrites=await catalogue(a);
  for(const record of initial)check(afterWrites.some(r=>r.domain===record.domain&&r.last_visit===record.last_visit),
    'listing, reads or background writes refreshed last visit');
  const oldPages=pages.map(p=>p.id);
  try{await sendCommand(a,'delete-all')}catch(error){if(!error.cdpTerminal)throw error}
  const empty=await until(async()=>(await events()).find(e=>e.event==='all-empty'),'empty catalogue at delete-all completion');
  check(empty.count===0,'delete-all acknowledged a nonempty catalogue');
  check(!(await pageTargets()).some(t=>oldPages.includes(t.id)),'old live-writing document survived delete-all');
  const fresh=[],readbacks=[];
  for(let i=0;i<2;i++){
    const url=[first,second][i]+'/page/after-all';
    const target=await until(async()=>(await pageTargets()).find(t=>t.url===url),'explicit post-delete-all visit');
    const c=await connect(target);sockets.push(c);fresh.push(c);
    await until(async()=>await value(c,"document.readyState==='complete'&&document.body.dataset.site===location.hostname"),'fresh post-delete-all document');
    const found=await readRecord(c);checkRetiredRecord(found,seeded[i]);readbacks.push(found);
    const settings=await value(c,"navigator.permissions.query({name:'geolocation'}).then(p=>({permission:p.state,zoom:devicePixelRatio}))");
    check(settings.permission==='prompt'&&Math.abs(settings.zoom-1)<.001,'fresh record retained deleted native settings');
  }
  const renewed=await catalogue(fresh[0]);
  check(renewed.length===2&&initial.every(old=>renewed.some(r=>r.domain===old.domain&&r.last_visit>old.last_visit)),
    'later visits did not create fresh public records with new visit times');
  allReader=fresh[0];allLive={initial,afterWrites,empty,renewed,readbacks};
}
const reader=allReader??(phase==='live'?b:a);
let embeddedRouting=null;
if(phase!=='live'&&phase!=='all-live'){
  await value(b,"localStorage.setItem('alloy-list-route','real-beta-value');true");
  embeddedRouting=await value(a,`new Promise(resolve=>{const frame=document.createElement('iframe');
    const receive=e=>{if(e.source===frame.contentWindow&&e.origin===${JSON.stringify(new URL(second).origin)}
      &&e.data?.kind==='list-value'){removeEventListener('message',receive);frame.remove();resolve(e.data.value)}};
    addEventListener('message',receive);frame.src=${JSON.stringify(second+'/page/list-route')};
    frame.onload=()=>frame.contentWindow.postMessage('read-list',${JSON.stringify(new URL(second).origin)});
    document.body.append(frame)})`);
  check(embeddedRouting===(deleted?null:'real-beta-value'),'native embed list did not select its real/temporary data '+embeddedRouting);
}
await sendCommand(reader,'read');
const embeds=await until(async()=>{const e=(await events()).filter(e=>e.event==='embeds');return e.length===2?e:null},'both native-owner embed lists');
for(const url of [first,second]){
  const e=embeds.find(e=>e.outer===url);check(e,'embed owner '+url);
  check(e.count===(deleted||phase==='live'||phase==='all-live'?0:1),'embed cardinality '+JSON.stringify(embeds));
  if(!deleted&&phase!=='live'&&phase!=='all-live')check(e.domain===(url===first?'beta.test':'alpha.test'),'canonical embed domain '+JSON.stringify(embeds));
}
await finishProfile(()=>sendCommand(reader,'finish'),events);
for(const c of sockets)c.ws.close();
console.log(JSON.stringify({phase,observations,embeds,live,allLive,embeddedRouting}));
'''
DRIVE = DRIVE.replace('__NOISE_FUNCTION__', NOISE_FUNCTION)


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if profile_retirement.respond(self):
            return
        kind = 'text/html'
        body = b'''<!doctype html><body><script>document.body.dataset.site=location.hostname;
onmessage=e=>{if(e.source===parent&&e.data==='read-list')parent.postMessage(
  {kind:'list-value',value:localStorage.getItem('alloy-list-route')},e.origin)};
</script></body>'''
        if self.path in ('/dedicated.js', '/nested.js', '/shared.js', '/service.js', '/noise-worker.js'):
            kind = 'application/javascript'
            scripts = {
                '/nested.js': b'onmessage=e=>postMessage(e.data)',
                '/dedicated.js': b'onmessage=e=>{const nested=new Worker("/nested.js");nested.onmessage=e=>postMessage(e.data);nested.postMessage(e.data)}',
                '/shared.js': b'onconnect=e=>{const port=e.ports[0];port.onmessage=e=>port.postMessage(e.data);port.start()}',
                '/service.js': b'oninstall=e=>skipWaiting();onactivate=e=>e.waitUntil(clients.claim());onmessage=e=>e.ports[0].postMessage(e.data)',
                '/noise-worker.js': ('onmessage=()=>postMessage(('+NOISE_FUNCTION+
                                     ')(new OffscreenCanvas(32,32)));').encode(),
            }
            body = scripts[self.path]
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


def profile_hashes(root):
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(root.rglob('*')) if p.is_file() and not p.is_symlink()}


def sdk_cases(out, evidence, cases=('guest', 'local-state-legacy', 'local-state-no-disk',
                                   'c-write', 'c-read', 'cpp-read', 'c-wrong')):
    """Exercise the public settings/bootstrap consumers, without store observers."""
    settings_profile = evidence/'sdk-settings-profile'
    for name in cases:
        root = evidence/('sdk-'+name)
        root.mkdir()
        owned = {key:root/key for key in ('home', 'cache', 'config', 'data', 'runtime', 'profile')}
        for path in owned.values():
            path.mkdir(mode=0o700)
        xdg_cache = system_cache.create(root)
        env = dict(os.environ, HOME=str(owned['home']), XDG_CACHE_HOME=str(xdg_cache),
            XDG_CONFIG_HOME=str(owned['config']), XDG_DATA_HOME=str(owned['data']),
            XDG_RUNTIME_DIR=str(owned['runtime']))
        if name == 'guest':
            binary, marker = 'alloy_guest_settings_consumer', 'PASS guest-settings-api'
        elif name.startswith('local-state-'):
            mode = name.removeprefix('local-state-')
            binary, marker = 'alloy_local_state_consumer', 'PASS memory-local-state-'+mode
            env.update(ALLOY_LOCAL_STATE_RUNTIME=str(owned['profile']), ALLOY_LOCAL_STATE_MODE=mode,
                ALLOY_LOCAL_STATE_CHILDREN='1', ALLOY_LOCAL_STATE_ENCRYPTED=str(root/'encrypted'))
            seed = owned['profile']/'Local State'
            seed_bytes = b'{"alloy":{"bootstrap_probe":"plaintext-seed-must-not-load"}}\n'
            if mode == 'legacy':
                seed.write_bytes(seed_bytes)
        else:
            binary, marker = 'alloy_site_settings_consumer', 'PASS site-settings-api-'+name
            env.update(ALLOY_SETTINGS_MODE=name, ALLOY_SETTINGS_PROFILE=str(settings_profile),
                       ALLOY_SETTINGS_RUNTIME=str(owned['profile']))
        argv = ['xvfb-run', '-a', str(out/binary), '--ozone-platform=x11', '--disable-gpu']
        if name != 'local-state-legacy':
            argv.append('--alloy-no-disk')
        with (root/'stdio.log').open('x') as log:
            result = subprocess.run(argv, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
        save(root/'result.json', dict(argv=argv, exit=result.returncode))
        assert result.returncode == 0, 'SDK consumer failed: '+name
        assert marker in (root/'stdio.log').read_text(), 'SDK marker missing: '+name
        system_cache.check(xdg_cache)
        if name == 'guest':
            assert not any(p.is_file() or p.is_symlink() for path in owned.values() for p in path.rglob('*')), \
                'memory settings created browser files'
        if name.startswith('local-state-'):
            assert seed.read_bytes() == seed_bytes if mode == 'legacy' else not seed.exists(), \
                'Local State changed or was created'
        print('PASS settings-sdk-'+name, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=['encrypted', 'delete', 'keyring'], required=True)
    parser.add_argument('--sdk-case', choices=['guest'])
    parser.add_argument('--phase', choices=['write', 'live', 'all-live'], help='Focused browser phase and its required setup')
    parser.add_argument('--inside', action='store_true')
    args = parser.parse_args()
    assert os.environ['TMPDIR'] == str(temp_path(W))
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
            'flock', '--wait', '1800', str(PRIMARY/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-encrypted-probe',
            'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1', 'python3',
            str(Path(__file__).resolve()), '--out', str(out), '--check', args.check, '--inside']
        if args.sdk_case:
            command += ['--sdk-case', args.sdk_case]
        if args.phase:
            command += ['--phase', args.phase]
        return subprocess.run(['timeout', '--kill-after=30', '4500', *command], timeout=4540).returncode
    assert os.geteuid() == 1000
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    routes = subprocess.check_output(['ip', 'route'], text=True, timeout=10)
    assert [v['ifname'] for v in interfaces] == ['lo'] and not routes.strip()
    if args.check == 'keyring' and not os.environ.get('ALLOY_TEST_KEYRING'):
        root = W/'evidence'/('settings-keyring-session-'+out.name+'-'+str(time.time_ns()))
        return subprocess.run(['python3', str(REPO/'tools/test_keyring.py'), '--root', str(root), '--',
            'python3', str(Path(__file__).resolve()), '--out', str(out), '--check', 'keyring', '--inside'],
            timeout=450).returncode
    native_env = dict(os.environ)
    if args.check == 'keyring':
        native_env['ALLOY_ENCRYPTED_KEYRING'] = '1'
    evidence = W/'evidence'/('profile-settings-'+args.check+'-'+out.name+'-'+str(time.time_ns()))
    evidence.mkdir()
    print('ENCRYPTED_EVIDENCE', evidence, flush=True)
    save(evidence/'isolation.json', dict(interfaces=interfaces, routes=routes,
         uid=os.geteuid(), netns=os.readlink('/proc/self/ns/net')))
    profile = evidence/'encrypted-profile'
    binary = out/'alloy_encrypted_context_consumer'
    results = []
    server = None
    try:
        if args.sdk_case:
            sdk_cases(out, evidence, (args.sdk_case,))
            save(evidence/'result.json', dict(pass_=True, sdk_case=args.sdk_case, config=out.name))
            return 0
        if args.check == 'encrypted' and not args.phase:
            sdk_cases(out, evidence)
        cert, key = evidence/'certificate.pem', evidence/'fixture-key.pem'
        with (evidence/'certificate.log').open('x') as log:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-sha256', '-days', '1', '-subj', '/CN=alpha.test', '-keyout', str(key),
                '-out', str(cert)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        # The native HTTP cache refuses responses whose SSLInfo has a cert
        # error, even when a blanket ignore switch allows navigation. Trust
        # only this throwaway fixture key through Chromium's SPKI test path,
        # which produces a successful verification result for these responses.
        public_key = subprocess.check_output(
            ['openssl', 'x509', '-in', str(cert), '-pubkey', '-noout'], timeout=10)
        spki = subprocess.run(['openssl', 'pkey', '-pubin', '-outform', 'DER'],
            input=public_key, capture_output=True, check=True, timeout=10).stdout
        trusted_key = base64.b64encode(hashlib.sha256(spki).digest()).decode('ascii')
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        first, second = [f'https://{name}:{server.server_port}' for name in ('alpha.test', 'beta.test')]
        phases = (('write', 'delete', 'after-delete', 'live', 'after-live-delete', 'all-live', 'after-all-live', 'delete-all', 'after-delete-all') if args.check == 'delete' else
                  ('write', 'read', 'locked', 'wrong', 'delete', 'after-delete', 'live', 'after-live-delete',
                   'delete-all', 'after-delete-all'))
        if args.phase:
            phases = ('write', args.phase) if args.phase in ('live', 'all-live') else (args.phase,)
        for phase in phases:
            root = evidence/phase
            root.mkdir()
            if phase in ('delete', 'delete-all'):
                command = [str(out/'alloy_encrypted_record_delete_consumer'), str(profile)]
                if phase == 'delete-all':
                    command.append('--all')
                with (root/'stdio.log').open('x') as log:
                    result = subprocess.run(command, env=native_env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
                assert result.returncode == 0
                assert 'PASS encrypted-native-record-delete' in (root/'stdio.log').read_text()
                results.append(dict(phase=phase, exit=0))
                continue
            owned = {name:root/name for name in ('home', 'cache', 'config', 'data', 'runtime')}
            for path in owned.values():
                path.mkdir(mode=0o700)
            xdg_cache = system_cache.create(root)
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1', 0))
                port = reserve.getsockname()[1]
            argv = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24', str(binary),
                '--alloy-no-disk', '--off-screen-rendering-enabled', '--ozone-platform=x11',
                '--disable-gpu', '--ignore-certificate-errors-spki-list='+trusted_key,
                '--remote-debugging-port='+str(port),
                '--host-resolver-rules=MAP alpha.test 127.0.0.1, MAP beta.test 127.0.0.1, MAP cdn.test 127.0.0.1']
            env = dict(native_env, TMPDIR=str(temp_path(W)), HOME=str(owned['home']),
                XDG_CACHE_HOME=str(xdg_cache), XDG_CONFIG_HOME=str(owned['config']),
                XDG_DATA_HOME=str(owned['data']), XDG_RUNTIME_DIR=str(owned['runtime']),
                ALLOY_ENCRYPTED_PHASE=phase, ALLOY_ENCRYPTED_PATH=str(profile),
                ALLOY_ENCRYPTED_FIRST=first, ALLOY_ENCRYPTED_SECOND=second)
            save(root/'argv.json', argv)
            before = profile_hashes(profile) if phase in ('locked', 'wrong') else None
            with (root/'stdio.log').open('x') as log:
                process = subprocess.Popen(argv, cwd=out, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    if phase not in ('locked', 'wrong'):
                        deadline = time.monotonic()+90
                        while True:
                            assert process.poll() is None, 'native browser exited before DevTools readiness'
                            try:
                                with urlopen(f'http://127.0.0.1:{port}/json/version', timeout=1) as response:
                                    if json.load(response).get('webSocketDebuggerUrl'):
                                        break
                            except OSError:
                                pass
                            assert time.monotonic() < deadline, 'native DevTools readiness'
                            time.sleep(.05)
                        driver = subprocess.run(['node', '--input-type=module', '-e', DRIVE,
                            str(port), first, second, phase, str(root/'stdio.log')],
                            capture_output=True, text=True, timeout=150)
                        (root/'driver.stdout').write_text(driver.stdout)
                        (root/'driver.stderr').write_text(driver.stderr)
                        assert driver.returncode == 0, driver.stderr
                        save(root/'browser-proof.json', json.loads(driver.stdout))
                    process.wait(timeout=90)
                    assert process.returncode == 0
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
            output = (root/'stdio.log').read_text()
            system_cache.check(xdg_cache)
            assert 'PASS encrypted-context-consumer' in output
            expected = 'refused' if phase in ('locked', 'wrong') else 'locked'
            assert any(json.loads(line[16:])['event'] == expected for line in output.splitlines()
                       if line.startswith('ENCRYPTED_EVENT '))
            native_events = [json.loads(line[16:]) for line in output.splitlines()
                             if line.startswith('ENCRYPTED_EVENT ')]
            metadata_event = ('metadata-refused' if phase in ('locked', 'wrong') else
                              'metadata-written' if phase in ('write', 'live') else 'metadata-read')
            assert any(e['event'] == metadata_event and e['fields'] == 4 for e in native_events)
            if phase not in ('locked', 'wrong'):
                assert any(e['event'] == 'metadata-revoked' and e['fields'] == 4 for e in native_events)
            if before is not None:
                assert profile_hashes(profile) == before, 'refused credentials mutated encrypted profile'
            for path in [profile, *owned.values()]:
                scan = subprocess.run(['rg', '-a', '-i', '-l', '-e', 'alpha.test', '-e', 'beta.test',
                                       '-e', 'ALLOY_NATIVE_PROFILE_METADATA_88', '-e', 'profile.avatar_index', str(path)],
                                      capture_output=True, text=True, timeout=30)
                assert scan.returncode == 1, 'plaintext domain scan: '+scan.stdout+scan.stderr
            results.append(dict(phase=phase, exit=0, domain_scan_matches=0))
            save(evidence/'stages.json', results)
            print('PASS encrypted-native-'+phase, flush=True)
        save(evidence/'result.json', dict(pass_=True, config=out.name, results=results, focused_phase=args.phase,
              actual_native_profile_and_cef_settings=True, cold_record_deletion='after-delete' in phases,
              native_profile_metadata_cold_read_and_site_deletion_preserved='after-delete' in phases,
              native_profile_metadata_lock_and_wrong_password_denied={'locked', 'wrong'}.issubset(phases),
             native_profile_metadata_plaintext_scan_absent=True,
              live_record_document_worker_deletion='live' in phases))
        return 0
    except BaseException:
        (evidence/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        if server:
            server.shutdown()
            server.server_close()
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink()
                 and p.name != 'SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in files))


if __name__ == '__main__':
    raise SystemExit(main())
