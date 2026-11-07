#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Offline cefclient tracker probe in a loopback-only net namespace."""
import argparse
import errno
import functools
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time

from workspace import namespace_prefix, temp_path, validate_output, workspace_owner, workspace_path

REPO=Path(__file__).resolve().parents[1]
WORK=workspace_path(REPO)
TMP=str(temp_path(WORK))
TRACKERS={
    'gtm':'https://www.googletagmanager.com/gtag/js?id=G-TEST',
    'gpt':'https://securepubads.g.doubleclick.net/tag/js/gpt.js',
    'fb':'https://connect.facebook.net/en_US/fbevents.js',
}
CDP=r'''
const [port, url, resultPath, holdSeconds, spell, withData] = process.argv.slice(1);
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const fs=await import('node:fs');
const readinessStarted=Date.now();
let target;
for(let i=0;i<150;i++){
  try{target=(await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(t=>t.type==='page');if(target)break;}catch{}
  await sleep(200);
}
if(!target)throw Error('No page target');
const ws=new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});
let id=0;const pending=new Map();const events=[];const requests={};
ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);if(!p)return;pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}else{events.push(m);if(m.method==='Network.requestWillBeSent')requests[m.params.requestId]=m.params.request.url;}};
const call=(method,params={})=>new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}));});
await call('Network.enable');await call('Page.enable');
let queryReady=false;
const readinessURL=new URL('ready.html',url).href;
const readiness=[];
const readinessDeadline=Date.now()+30000;
while(Date.now()<readinessDeadline){
  // Same-site requests are intentionally exempt from query cleaning. Each
  // attempt starts from the original blank-origin condition, with idle time
  // for Chromium's startup-idle fence to release the BEST_EFFORT parser.
  // Rapid navigation would continually reset the loaded-and-idle timer.
  await call('Page.navigate',{url:'about:blank'});
  await sleep(4000);
  await call('Page.navigate',{url:readinessURL+'?fbclid=alloy-query-ready&keep=1'});
  await sleep(100);
  const observed=await call('Runtime.evaluate',{expression:'location.href',returnByValue:true});
  readiness.push(observed.result.value);
  const expected=readinessURL+(withData==='yes'?'?keep=1':'?fbclid=alloy-query-ready&keep=1');
  if(observed.result.value===expected){queryReady=true;break;}
}
if(!queryReady)throw Error('Unexpected QueryFilter readiness URL: '+JSON.stringify(readiness));
const queryReadyMs=Date.now()-readinessStarted;
await call('Page.navigate',{url:'about:blank'});
await call('Page.navigate',{url:url+'?fbclid=alloy-query-probe&keep=1'});
for(let i=0;i<100;i++){const x=await call('Runtime.evaluate',{expression:'document.readyState',returnByValue:true});if(x.result.value==='complete')break;await sleep(100);}
await sleep(4000);
if(spell==='yes'){
  await call('Runtime.evaluate',{expression:`document.querySelector('textarea').focus()`});
  await call('Input.insertText',{text:'hello qzxqzxqzx world '});
  await call('Input.dispatchKeyEvent',{type:'keyDown',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
  await call('Input.dispatchKeyEvent',{type:'keyUp',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
  await sleep(12000);
}
const result=await call('Runtime.evaluate',{expression:'JSON.stringify({outcomes:window.outcomes,neutral:window.neutralExecuted,value:document.querySelector("textarea")?.value,url:location.href})',returnByValue:true});
await sleep(Number(holdSeconds)*1000);
const failures=events.filter(e=>e.method==='Network.loadingFailed').map(e=>({...e.params,url:requests[e.params.requestId]}));
fs.writeFileSync(resultPath,JSON.stringify({target,queryReadyMs,readinessURL,readiness,consumer:JSON.parse(result.result.value),failures,events},null,2));
await call('Page.close');ws.close();
'''

STARTUP_CONTROL=r'''
const [port,url,resultPath]=process.argv.slice(1);
const fs=await import('node:fs');
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let target;
for(let i=0;i<150;i++){
  try{target=(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find(t=>t.type==='page');if(target)break;}catch{}
  await sleep(200);
}
if(!target)throw Error('Startup control has no browser page');
const ws=new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
let id=0;const pending=new Map();
ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}};
const call=(method,params={})=>new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}));});
let proof;
for(let i=0;i<100;i++){
  const result=await call('Runtime.evaluate',{expression:'({url:location.href,title:document.title,neutral:window.neutralExecuted})',returnByValue:true});
  const value=result.result.value;
  if(value?.url===url&&value.title==='Alloy offline probe'&&value.neutral===true){proof=value;break;}
  await sleep(100);
}
if(!proof)throw Error('Startup control did not render its fixture');
fs.writeFileSync(resultPath,JSON.stringify(proof,null,2));
await call('Page.close');ws.close();
'''


def startup_control(argv,out,log,url,port,requests):
    # Both browsers inherit this invocation's single Xvfb and network namespace.
    profile=log/'control-profile';profile.mkdir()
    control=[arg for arg in argv if not arg.startswith('--alloy-data-dir=')]
    replacements={'--cache-path=':str(profile),'--url=':url,
                  '--log-file=':str(log/'control-debug.log'),
                  '--log-net-log=':str(log/'control-netlog.json')}
    control=[next((prefix+value for prefix,value in replacements.items()
                   if arg.startswith(prefix)),arg) for arg in control]
    env=dict(os.environ,ALLOY_SPELL_URL=url,ALLOY_SPELL_PROFILE=str(profile),TMPDIR=TMP)
    with (log/'control-stdio.log').open('w') as stream:
        process=subprocess.Popen(control,cwd=out,stdout=stream,stderr=subprocess.STDOUT,
                                 start_new_session=True,env=env)
        try:
            result=subprocess.run(['node','--input-type=module','-e',STARTUP_CONTROL,
                                   str(port),url,str(log/'control-page.json')],
                                  capture_output=True,text=True,timeout=50)
            (log/'control-cdp.stdout').write_text(result.stdout)
            (log/'control-cdp.stderr').write_text(result.stderr)
            assert result.returncode==0,result.stderr
            process.wait(timeout=15)
            proof=dict(argv=control,exit=process.returncode,
                       display=os.environ['DISPLAY'],requests=list(requests),
                       page=json.loads((log/'control-page.json').read_text()))
            (log/'startup-control.json').write_text(json.dumps(proof,indent=2)+'\n')
            assert process.returncode==0,proof
            assert '/index.html' in requests,proof
            print('ALLOY_STARTUP_CONTROL_PASS',json.dumps(proof['page']),flush=True)
            return len(requests)
        finally:
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)


def has_sentinel_spelling_marker(node):
    # CEF flattens marker type bitmasks. Only a single range with a single
    # spelling type is unambiguous; never zip flattened types with ranges.
    starts=node.get('markerStarts',[])
    ends=node.get('markerEnds',[])
    if node.get('markerTypes')!=['spelling'] or len(starts)!=1 or len(ends)!=1:
        return False
    return node.get('name','')[starts[0]:ends[0]]=='qzxqzxqzx'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--inside',action='store_true')
    p.add_argument('--hold-seconds',type=int,default=0)
    p.add_argument('--spell-consumer',type=Path)
    p.add_argument('--alloy-data-dir',type=Path,
                   help='Embedder input; absent means verify without lists')
    p.add_argument('--expect-startup-failure',action='store_true')
    p.add_argument('--profile',type=Path,
                   help='Retained profile beneath workspace evidence/; supports warm restart')
    a=p.parse_args()
    out=a.out.resolve()
    try:
        validate_output(WORK,out,REPO)
    except RuntimeError as error:
        p.error(str(error))
    if not 0<=a.hold_seconds<=900:p.error('Hold duration 0..900 seconds')
    if a.expect_startup_failure and a.alloy_data_dir is None:
        p.error('--expect-startup-failure requires --alloy-data-dir')
    if a.spell_consumer and a.alloy_data_dir is None:
        p.error('Spelling requires --alloy-data-dir')
    if a.profile is not None and not a.profile.resolve().is_relative_to(WORK/'evidence'):
        p.error('--profile must be beneath the selected workspace evidence/')
    if not a.inside:
        command=namespace_prefix(WORK)+['unshare','--net','--fork','/bin/sh','-c',
                  'owner=$1; shift; ip link set lo up && exec runuser -u "$owner" -- "$@"','alloy-offline',
                  workspace_owner(WORK).pw_name,
                  'env','TMPDIR='+TMP,'PYTHONDONTWRITEBYTECODE=1',
                  'xvfb-run','-a','-s','-screen 0 1280x720x24',
                  'python3',str(Path(__file__).resolve()),*sys.argv[1:],'--inside']
        return subprocess.run(['timeout','--kill-after=20s',str(a.hold_seconds+150),*command],timeout=a.hold_seconds+180).returncode
    assert os.geteuid()!=0,'Browser must run as the workspace owner with its sandbox enabled'
    interfaces=json.loads(subprocess.check_output(['ip','-j','addr'],timeout=10))
    assert [i['ifname'] for i in interfaces]==['lo'],interfaces
    routes=subprocess.check_output(['ip','route'],text=True,timeout=10)
    assert not routes.strip(),routes
    with socket.socket() as s:
        assert s.connect_ex(('1.1.1.1',443))==errno.ENETUNREACH
    log=WORK/'evidence/item03'/f'probe-{time.time_ns()}'
    log.mkdir(parents=True)
    (log/'isolation.json').write_text(json.dumps(dict(netns=os.readlink('/proc/self/ns/net'),uid=os.geteuid(),interfaces=interfaces,routes=routes,outside_connect_errno=errno.ENETUNREACH),indent=2)+'\n')
    site=log/'site';site.mkdir()
    (site/'ready.html').write_text('<!doctype html><title>QueryFilter readiness</title>')
    # The tracker URLs are the spike item-5 test page's exact inputs. Local
    # neutral execution proves the page loaded despite the absence of egress.
    (site/'index.html').write_text('<!doctype html><title>Alloy offline probe</title><h1>Alloy offline probe</h1><textarea spellcheck="true" lang="en-US"></textarea><button>Leave field</button><script>window.outcomes={};</script>')
    (site/'neutral.js').write_text('window.neutralExecuted=true;')
    page=(site/'index.html').read_text()+ '<script>setTimeout(()=>{for(const [name,url] of Object.entries('+json.dumps(dict(TRACKERS,neutral='/neutral.js'))+')){let s=document.createElement("script");s.src=url;s.onload=()=>outcomes[name]="loaded";s.onerror=()=>outcomes[name]="error";document.head.append(s)}},1000)</script>'
    (site/'index.html').write_text(page)
    requests=[]
    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            super().do_GET()
    handler=functools.partial(Handler,directory=site)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url=f'http://127.0.0.1:{server.server_port}/index.html'
    profile=a.profile.resolve() if a.profile is not None else log/'profile'
    profile.mkdir(parents=True,exist_ok=True)
    port=9237
    executable=a.spell_consumer.resolve() if a.spell_consumer else out/'cefclient'
    argv=[str(executable),'--off-screen-rendering-enabled',f'--remote-debugging-port={port}','--ozone-platform=x11','--disable-gpu',f'--cache-path={profile}',f'--url=about:blank',f'--log-file={log}/debug.log',f'--log-net-log={log}/netlog.json','--net-log-capture-mode=Everything','--enable-logging=stderr','--vmodule=query_filter_component_installer=1']
    if a.alloy_data_dir is not None:
        argv.append('--alloy-data-dir='+str(a.alloy_data_dir))
    if a.expect_startup_failure:
        argv[argv.index('--url=about:blank')]='--url='+url
    (log/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    print('OFFLINE_PROBE_EVIDENCE',log,flush=True)
    with (log/'stdio.log').open('w') as stream:
        process=None
        try:
            control_requests=startup_control(argv,out,log,url,port,requests) if a.expect_startup_failure else 0
            process=subprocess.Popen(argv,cwd=out,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True,env=dict(os.environ,ALLOY_SPELL_URL='about:blank',ALLOY_SPELL_PROFILE=str(profile),TMPDIR=TMP))
            if a.expect_startup_failure:
                process.wait(timeout=20)
                proof=dict(exit=process.returncode,requests=requests[control_requests:],
                           display=os.environ['DISPLAY'],control='startup-control.json')
                (log/'startup-failure.json').write_text(json.dumps(proof,indent=2)+'\n')
                assert process.returncode!=0,proof
                assert not proof['requests'],proof
                print('ALLOY_DATA_DIR_STARTUP_REJECTED',json.dumps(proof),flush=True)
                return 0
            result=subprocess.run(['node','--input-type=module','-e',CDP,str(port),url,str(log/'cdp.json'),str(a.hold_seconds),'yes' if a.spell_consumer else 'no','yes' if a.alloy_data_dir is not None else 'no'],capture_output=True,text=True,timeout=a.hold_seconds+110)
            (log/'cdp.stdout').write_text(result.stdout);(log/'cdp.stderr').write_text(result.stderr)
            assert result.returncode==0,result.stderr
            proof=json.loads((log/'cdp.json').read_text())
            assert proof['consumer']['neutral'] is True,proof['consumer']
            with_data=a.alloy_data_dir is not None
            suffix='?keep=1' if with_data else '?fbclid=alloy-query-probe&keep=1'
            assert proof['consumer']['url']==url+suffix,proof['consumer']
            ready_suffix='?keep=1' if with_data else '?fbclid=alloy-query-ready&keep=1'
            assert proof['readiness'][-1]==proof['readinessURL']+ready_suffix,proof['readiness']
            if out.name=='Verify_Alloy' and with_data:
                expected_rules=len(json.loads((a.alloy_data_dir/'lists/local-data/1/query-filter.json').read_text()))
                query_log=(log/'stdio.log').read_text()
                assert f'Loaded {expected_rules} embedder QueryFilter rules' in query_log,query_log[-3000:]
            print('ALLOY_QUERY_FILTER_PASS',proof['consumer']['url'],flush=True)
            for name,url in TRACKERS.items():
                failures=[f for f in proof['failures'] if f.get('url')==url]
                assert failures,(name,proof['failures'])
                assert all((f['errorText']=='net::ERR_BLOCKED_BY_CLIENT')==with_data for f in failures),(name,failures)
            process.wait(timeout=15)
            assert process.returncode==0,process.returncode
            if a.spell_consumer:
                trees=[json.loads(line[3:]) for line in (log/'stdio.log').read_text().splitlines() if line.startswith('AX ')]
                markers=[]
                def visit(x):
                    if isinstance(x,dict):
                        if 'spelling' in x.get('markerTypes',[]):markers.append(x)
                        for value in x.values():visit(value)
                    elif isinstance(x,list):
                        for value in x:visit(value)
                for tree in trees:visit(tree)
                (log/'spell-markers.json').write_text(json.dumps(markers,indent=2)+'\n')
                assert markers,'No native spelling marker delivered to CEF consumer'
                assert any(has_sentinel_spelling_marker(m) for m in markers),markers
                print('ALLOY_EMBEDDER_DICTIONARY_NATIVE_MARKER_PASS',json.dumps(markers),flush=True)
            print('ALLOY_OFFLINE_TRACKER_PROBE_PASS',json.dumps(proof['consumer']),flush=True)
        finally:
            server.shutdown()
            if process is not None and process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    return 0

if __name__=='__main__':sys.exit(main())
