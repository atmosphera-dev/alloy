#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Public-SDK decay, cold unlock and native live-writer retirement proof."""
import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import threading
import time
from urllib.request import urlopen

from workspace import PRIMARY, temp_path, validate_output, workspace_path
from profile_probe import CDP_HELPERS
from profile_encrypted import Handler, save
from profile_inventory import observe_tree
import profile_system_cache as system_cache
import profile_retirement

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)
DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,first,second,phase,eventLog]=process.argv.slice(1);
''' + CDP_HELPERS + profile_retirement.HELPERS + r'''
async function events(){return(await readFile(eventLog,'utf8')).split('\n')
  .filter(s=>s.startsWith('ENCRYPTED_EVENT ')).map(s=>JSON.parse(s.slice(16)))}
let serial=0;
async function command(c,name){await value(c,'document.title='+JSON.stringify('ALLOY_ENCRYPTED:'+(++serial)+':'+name)+';true')}
const pages=await until(async()=>{const t=await pageTargets();return t.length===2&&t.every(p=>p.url.startsWith(first)||p.url.startsWith(second))?t:null},'two encrypted tabs');
const a=await connect(pages.find(t=>t.url.startsWith(first))),b=await connect(pages.find(t=>t.url.startsWith(second)));
const sockets=[a,b];
for(const c of sockets){await c.send('Page.enable');await until(async()=>await value(c,"document.readyState==='complete'&&document.body.dataset.site===location.hostname"),'fixture document')}
async function settings(c){return value(c,"navigator.permissions.query({name:'geolocation'}).then(p=>({permission:p.state,zoom:devicePixelRatio}))")}
let proof={phase};
let alphaRecord,betaRecord;
if(phase==='decay-read'){
  check((await events()).some(e=>e.event==='decay-policy-reopened'&&e.seconds===6),'stored policy was not applied on cold unlock');
  for(const c of sockets){const state=await settings(c);check(state.permission==='prompt'&&Math.abs(state.zoom-1)<.001,'expired native settings read back '+JSON.stringify(state))}
  await command(a,'read');
  const lists=await until(async()=>{const e=(await events()).filter(e=>e.event==='embeds');return e.length===2?e:null},'cold post-decay lists');
  check(lists.every(e=>e.count===0),'expired embed lists survived unlock');proof.lists=lists;
}else{
  await command(a,'write');await until(async()=>(await events()).some(e=>e.event==='written'),'durable site settings');
  check((await settings(a)).permission==='granted'&&(await settings(b)).permission==='denied','settings positive controls');
  if(phase==='decay-live'){
    alphaRecord=await seedRecord(a);betaRecord=await seedRecord(b);
    checkRetainedRecord(await readRecord(a),alphaRecord);
    checkRetainedRecord(await readRecord(b),betaRecord);
    for(const c of sockets)await value(c,"localStorage.setItem('decay-state','owned-'+location.hostname);document.cookie='decay_state=owned; SameSite=Lax';true");
    await value(a,`(async()=>{
      window.lateWrites=0;window.writer=setInterval(()=>{
        localStorage.setItem('decay-late','late-'+(++lateWrites));document.cookie='late_write='+lateWrites+'; SameSite=Lax';
        const marker='record-'+location.hostname;
        localStorage.setItem('record-value',marker);sessionStorage.setItem('record-value',marker);
        document.cookie='record_value='+marker+'; Path=/; Secure; SameSite=Lax';
        try{retainedRecordDb.transaction('values','readwrite').objectStore('values').put(marker,'value')}catch{}
        retainedRecordCache.put('/record-cache',new Response(marker)).catch(()=>{});
      },50);
      window.ownedDedicated=new Worker('/dedicated.js');await new Promise(resolve=>{ownedDedicated.onmessage=resolve;ownedDedicated.postMessage('ready')});
      window.ownedShared=new SharedWorker('/shared.js','decay-alpha');ownedShared.port.start();await new Promise(resolve=>{ownedShared.port.onmessage=resolve;ownedShared.port.postMessage('ready')});
      await navigator.serviceWorker.register('/service.js');await navigator.serviceWorker.ready;return true;})()`);
    check(await value(a,"localStorage.getItem('decay-state')==='owned-alpha.test'&&document.cookie.includes('decay_state=owned')"),'live writer positive control');
    await until(async()=>await value(a,'lateWrites>2'),'active late writes');
  }
  await command(a,'enable-decay');await until(async()=>(await events()).some(e=>e.event==='decay-enabled'),'persisted six-second policy');
  if(phase==='decay-live'){
    const before=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
    const map=new Map(before.map(t=>[t.id,t]));
    function alpha(t,seen=new Set()){
      check(!seen.has(t.id),'cyclic target ancestry');seen.add(t.id);
      if(t.url){const origin=new URL(t.url).origin;if(origin!=='null')return origin===new URL(first).origin}
      const parent=map.get(t.parentId);return !!parent&&alpha(parent,seen);
    }
    const original=before.filter(t=>alpha(t)),ids=new Set(original.map(t=>t.id));
    check(original.filter(t=>t.type==='worker').length>=2&&original.some(t=>t.type==='shared_worker')&&original.some(t=>t.type==='service_worker'),'live native workers missing '+JSON.stringify(before));
    const start=performance.now();let reloads=0;
    // Only actual top-level committed reloads keep beta alive. Alpha's active
    // writes and workers must not reset alpha's last-visit clock.
    for(let i=0;i<14;i++){
      const current=await pageTargets();if(!current.some(t=>ids.has(t.id)))break;
      const oldDocument=await value(b,'performance.timeOrigin');
      await b.send('Page.reload',{ignoreCache:true});
      await until(async()=>await value(b,"document.readyState==='complete'&&document.body.dataset.site==='beta.test'&&performance.timeOrigin!=="+oldDocument),'visited beta reload');
      reloads++;
      await sleep(1000);
    }
    const after=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
    for(const t of after)map.set(t.id,t);
    check(!after.some(t=>ids.has(t.id)||alpha(t)),'expired alpha page/worker survived '+JSON.stringify(after));
    check((await settings(b)).permission==='denied','visited beta settings decayed');
    check(await value(b,"localStorage.getItem('decay-state')==='owned-beta.test'"),'visited beta data lost');
    // WebContents retirement precedes native cleanup and replacement admission.
    // This public query joins the same settings queue behind that cleanup;
    // target disappearance alone is not a completed-decay acknowledgement.
    await command(b,'read');
    const lists=await until(async()=>{const e=(await events()).filter(e=>e.event==='embeds');return e.length===2?e:null},'live post-decay lists');
    check(lists.every(e=>e.count===0),'outer list or generated beta reference survived');
    await command(b,'recreate');
    const freshTarget=await until(async()=>(await pageTargets()).find(t=>t.url===first+'/page/fresh'),'explicit fresh navigation');
    const fresh=await connect(freshTarget);sockets.push(fresh);
    await until(async()=>await value(fresh,"document.readyState==='complete'&&document.body.dataset.site==='alpha.test'"),'fresh alpha');
    const state=await settings(fresh);
    check(state.permission==='prompt'&&Math.abs(state.zoom-1)<.001,'fresh alpha inherited decayed settings');
    const freshValues=await value(fresh,"({stored:localStorage.getItem('decay-state'),late:localStorage.getItem('decay-late'),cookies:document.cookie})");
    check(freshValues.stored===null&&freshValues.late===null&&!freshValues.cookies.includes('decay_state=')&&!freshValues.cookies.includes('late_write='),
      'late writer recreated retired data '+JSON.stringify(freshValues));
    const freshRecord=await readRecord(fresh),retainedRecord=await readRecord(b);
    checkRetiredRecord(freshRecord,alphaRecord);
    checkRetainedRecord(retainedRecord,betaRecord);
    check(reloads>0,'visited-site reload control missing');
    proof={...proof,before,after,lists,reloads,elapsedSeconds:(performance.now()-start)/1000,freshSettings:state,
      alphaRecord,betaRecord,freshRecord,retainedRecord};
  }
}
// Preserve the observed decay outcome even if later profile lock fails. The
// enclosing phase still requires successful lock and process exit to pass.
console.log(JSON.stringify(proof));
await finishProfile(()=>command(b,'finish'),events);for(const c of sockets)c.ws.close();
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=['decay'], default='decay')
    parser.add_argument('--phase', choices=['decay-live'], help='Run the independent live phase only')
    parser.add_argument('--inside', action='store_true')
    args = parser.parse_args()
    assert os.environ['TMPDIR'] == str(temp_path(W))
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        return subprocess.run(['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
            'flock', '--wait', '1800', str(PRIMARY/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-decay',
            'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1', 'python3', str(Path(__file__).resolve()),
            '--out', str(out), '--inside', *(['--phase', args.phase] if args.phase else [])], timeout=2100).returncode
    assert os.geteuid() == 1000
    assert [v['ifname'] for v in json.loads(subprocess.check_output(['ip', '-j', 'addr']))] == ['lo']
    assert not subprocess.check_output(['ip', 'route'], text=True).strip()
    evidence = W/'evidence'/('profile-decay-'+out.name+'-'+str(time.time_ns()))
    evidence.mkdir()
    print('DECAY_EVIDENCE', evidence, flush=True)
    cert, key = evidence/'certificate.pem', evidence/'fixture-key.pem'
    with (evidence/'certificate.log').open('x') as log:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '1',
            '-subj', '/CN=alpha.test', '-addext', 'subjectAltName=DNS:alpha.test,DNS:beta.test',
                '-keyout', str(key), '-out', str(cert)], stdout=log,
            stderr=subprocess.STDOUT, check=True, timeout=30)
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
    results = []
    try:
        for phase in ((args.phase,) if args.phase else ('decay-seed', 'decay-read', 'decay-live')):
            if phase == 'decay-read':
                # Profile is locked, so only a subsequent unlock can enforce it.
                time.sleep(6.5)
            root = evidence/phase
            root.mkdir()
            owned = {name:root/name for name in ('home', 'cache', 'config', 'data', 'runtime')}
            for path in owned.values():
                path.mkdir(mode=0o700)
            xdg_cache = system_cache.create(root)
            system_runtime = system_cache.create_runtime(root)
            # R11: the OS/NSS trust database is input, not Alloy profile state.
            # Seed it before measurement, then require byte-for-byte preservation.
            nss = owned['data']/'pki/nssdb'
            nss.mkdir(parents=True)
            with (root/'certificate-input.log').open('x') as log:
                subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:'+str(nss)],
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
                subprocess.run(['certutil', '-A', '-d', 'sql:'+str(nss), '-n', 'Decay fixture',
                    '-t', 'C,,', '-i', str(cert)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
            before = observe_tree(root, tuple(owned), hashes=True, report_path=root/'before-runtime.json')
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1', 0)); port = reserve.getsockname()[1]
            profile = evidence/('live-profile' if phase == 'decay-live' else 'cold-profile')
            env = dict(os.environ, HOME=str(owned['home']), XDG_CACHE_HOME=str(xdg_cache),
                XDG_RUNTIME_DIR=str(system_runtime), XDG_CONFIG_HOME=str(owned['config']), XDG_DATA_HOME=str(owned['data']),
                ALLOY_ENCRYPTED_PHASE=phase, ALLOY_ENCRYPTED_PATH=str(profile),
                ALLOY_ENCRYPTED_FIRST=first, ALLOY_ENCRYPTED_SECOND=second)
            argv = ['xvfb-run', '-a', str(out/'alloy_encrypted_context_consumer'), '--alloy-no-disk',
                '--off-screen-rendering-enabled', '--ozone-platform=x11', '--disable-gpu',
                '--ignore-certificate-errors-spki-list='+trusted_key,
                '--remote-debugging-port='+str(port),
                '--host-resolver-rules=MAP alpha.test 127.0.0.1, MAP beta.test 127.0.0.1, MAP cdn.test 127.0.0.1']
            with (root/'stdio.log').open('x') as log:
                process = subprocess.Popen(argv, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    deadline = time.monotonic()+90
                    while True:
                        assert process.poll() is None, 'native browser exited before readiness'
                        try:
                            with urlopen(f'http://127.0.0.1:{port}/json/version', timeout=1) as response:
                                if json.load(response).get('webSocketDebuggerUrl'):
                                    break
                        except OSError:
                            pass
                        assert time.monotonic() < deadline, 'DevTools readiness timed out'
                        time.sleep(.05)
                    driver = subprocess.run(['node', '--input-type=module', '-e', DRIVE,
                        str(port), first, second, phase, str(root/'stdio.log')], capture_output=True, text=True, timeout=180)
                    (root/'driver.stdout').write_text(driver.stdout)
                    (root/'driver.stderr').write_text(driver.stderr)
                    if driver.stdout.strip():
                        save(root/'browser-proof.json', json.loads(driver.stdout))
                    assert driver.returncode == 0, driver.stderr
                    assert process.wait(timeout=90) == 0
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=5)
            assert 'PASS encrypted-context-consumer' in (root/'stdio.log').read_text()
            system_cache.check(xdg_cache)
            system_cache.check_runtime(system_runtime)
            after = observe_tree(root, tuple(owned), hashes=True, report_path=root/'after-runtime.json')
            assert before == after, 'browser runtime roots or OS/NSS input changed'
            scan = subprocess.run(['rg', '-a', '-i', '-l', '-e', 'alpha.test', '-e', 'beta.test',
                '-e', 'ALLOY_NATIVE_PROFILE_METADATA_88', str(profile)], capture_output=True, text=True, timeout=30)
            assert scan.returncode == 1, 'plaintext record metadata '+scan.stdout+scan.stderr
            results.append(dict(phase=phase, pass_=True))
            save(evidence/'stages.json', results)
            print('PASS '+phase, flush=True)
        save(evidence/'result.json', dict(pass_=True, config=out.name, results=results, focused_phase=args.phase))
        print('PASS decay', flush=True)
        return 0
    finally:
        server.shutdown(); server.server_close()
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink() and p.name != 'SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(evidence))+'\n' for p in files))

if __name__ == '__main__':
    raise SystemExit(main())
