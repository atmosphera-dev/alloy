#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only offline cefclient tracker probe in a loopback-only net namespace."""
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

WORK=Path('/home/builder/work/alloy')
OUTPUTS={WORK/'src/out'/name for name in ('Release_Alloy','Verify_Alloy')}
TRACKERS={
    'gtm':'https://www.googletagmanager.com/gtag/js?id=G-TEST',
    'gpt':'https://securepubads.g.doubleclick.net/tag/js/gpt.js',
    'fb':'https://connect.facebook.net/en_US/fbevents.js',
}
CDP=r'''
const [port, url, resultPath, holdSeconds, spell] = process.argv.slice(1);
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
  if(observed.result.value===readinessURL+'?keep=1'){queryReady=true;break;}
}
if(!queryReady)throw Error('Bundled QueryFilter did not clean the readiness URL: '+JSON.stringify(readiness));
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

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--inside',action='store_true')
    p.add_argument('--hold-seconds',type=int,default=0)
    p.add_argument('--spell-consumer',type=Path)
    a=p.parse_args()
    out=a.out.resolve()
    if socket.gethostname()!='wakaba-build' or out not in OUTPUTS:
        p.error('Only the approved NUC Alloy release/verify outputs are supported')
    if not 0<=a.hold_seconds<=900:p.error('Hold duration 0..900 seconds')
    if not a.inside:
        command=['sudo','unshare','--net','--fork','/bin/sh','-c',
                 'ip link set lo up && exec runuser -u builder -- "$@"','alloy-offline',
                 'env','TMPDIR=/home/builder/work/tmp','PYTHONDONTWRITEBYTECODE=1',
                 'python3',str(Path(__file__).resolve()),*sys.argv[1:],'--inside']
        return subprocess.run(['timeout','--kill-after=20s',str(a.hold_seconds+150),*command],timeout=a.hold_seconds+180).returncode
    assert os.geteuid()!=0,'Browser must run as builder with its sandbox enabled'
    interfaces=json.loads(subprocess.check_output(['ip','-j','addr'],timeout=10))
    assert [i['ifname'] for i in interfaces]==['lo'],interfaces
    routes=subprocess.check_output(['ip','route'],text=True,timeout=10)
    assert not routes.strip(),routes
    with socket.socket() as s:
        assert s.connect_ex(('1.1.1.1',443))==errno.ENETUNREACH
    log=WORK/'evidence/item03'/f'probe-{time.time_ns()}'
    log.mkdir()
    (log/'isolation.json').write_text(json.dumps(dict(netns=os.readlink('/proc/self/ns/net'),uid=os.geteuid(),interfaces=interfaces,routes=routes,outside_connect_errno=errno.ENETUNREACH),indent=2)+'\n')
    site=log/'site';site.mkdir()
    (site/'ready.html').write_text('<!doctype html><title>QueryFilter readiness</title>')
    # The tracker URLs are the spike item-5 test page's exact inputs. Local
    # neutral execution proves the page loaded despite the absence of egress.
    (site/'index.html').write_text('<!doctype html><title>Alloy offline probe</title><h1>Alloy offline probe</h1><textarea spellcheck="true" lang="en-US"></textarea><button>Leave field</button><script>window.outcomes={};</script>')
    (site/'neutral.js').write_text('window.neutralExecuted=true;')
    page=(site/'index.html').read_text()+ '<script>setTimeout(()=>{for(const [name,url] of Object.entries('+json.dumps(dict(TRACKERS,neutral='/neutral.js'))+')){let s=document.createElement("script");s.src=url;s.onload=()=>outcomes[name]="loaded";s.onerror=()=>outcomes[name]="error";document.head.append(s)}},1000)</script>'
    (site/'index.html').write_text(page)
    handler=functools.partial(http.server.SimpleHTTPRequestHandler,directory=site)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url=f'http://127.0.0.1:{server.server_port}/index.html'
    profile=log/'profile';profile.mkdir()
    port=9237
    executable=a.spell_consumer.resolve() if a.spell_consumer else out/'cefclient'
    argv=['xvfb-run','-a','-s','-screen 0 1280x720x24',str(executable),'--off-screen-rendering-enabled',f'--remote-debugging-port={port}','--ozone-platform=x11','--disable-gpu',f'--cache-path={profile}',f'--url=about:blank',f'--log-file={log}/debug.log',f'--log-net-log={log}/netlog.json','--net-log-capture-mode=Everything','--enable-logging=stderr','--vmodule=query_filter_component_installer=1']
    (log/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    print('OFFLINE_PROBE_EVIDENCE',log,flush=True)
    with (log/'stdio.log').open('w') as stream:
        process=subprocess.Popen(argv,cwd=out,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True,env=dict(os.environ,ALLOY_SPELL_URL='about:blank',ALLOY_SPELL_PROFILE=str(profile),TMPDIR='/home/builder/work/tmp'))
        try:
            result=subprocess.run(['node','--input-type=module','-e',CDP,str(port),url,str(log/'cdp.json'),str(a.hold_seconds),'yes' if a.spell_consumer else 'no'],capture_output=True,text=True,timeout=a.hold_seconds+110)
            (log/'cdp.stdout').write_text(result.stdout);(log/'cdp.stderr').write_text(result.stderr)
            assert result.returncode==0,result.stderr
            proof=json.loads((log/'cdp.json').read_text())
            assert proof['consumer']['neutral'] is True,proof['consumer']
            assert proof['consumer']['url']==url+'?keep=1',proof['consumer']
            assert proof['readiness'][-1]==proof['readinessURL']+'?keep=1',proof['readiness']
            if out.name=='Verify_Alloy':
                expected_rules=len(json.loads((out/'alloy-data/lists/local-data/1/query-filter.json').read_text()))
                query_log=(log/'stdio.log').read_text()
                assert f'Loaded {expected_rules} bundled QueryFilter rules' in query_log,query_log[-3000:]
            print('ALLOY_BUNDLED_QUERY_FILTER_PASS',proof['consumer']['url'],flush=True)
            for name,url in TRACKERS.items():
                assert any(f.get('url')==url and f['errorText']=='net::ERR_BLOCKED_BY_CLIENT' for f in proof['failures']),(name,proof['failures'])
            if a.spell_consumer:
                process.wait(timeout=15)
                assert process.returncode==0,process.returncode
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
                print('ALLOY_BUNDLED_DICTIONARY_NATIVE_MARKER_PASS',json.dumps(markers),flush=True)
            print('ALLOY_OFFLINE_TRACKER_PROBE_PASS',json.dumps(proof['consumer']),flush=True)
        finally:
            server.shutdown()
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    return 0

if __name__=='__main__':sys.exit(main())
