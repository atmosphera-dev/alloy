#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""NUC-only fresh-profile store inventory; fail on selected store-map rows."""
import argparse
import errno
import http.server
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time

from smoke import terminate_session, wait_for_session_exit, session_processes

WORK = Path('/home/builder/work/alloy')
TMP = '/home/builder/work/tmp'
# Store-map row -> native path, including an empty directory and SQL sidecars.
# Row 10 here is ONLY restore copies; live Session Storage is not removed by R.
PATHS = {
    10: ('Sessions', 'Sessions_Encrypted', 'Tabs', 'workspaces'),
    23: ('Trust Tokens',),
    27: ('Sessions', 'Sessions_Encrypted'),
    29: ('Download Service',),
    31: ('CdmStorage.db', 'Media Licenses'),
    32: ('VideoDecodeStats', 'WebrtcVideoStats'),
    33: ('Top Sites',),
    34: ('Network Action Predictor', 'Predictor Database'),
    35: ('DIPS',),
    36: ('Feature Engagement Tracker',),
    37: ('heavy_ad_intervention_opt_out.db',),
    38: ('Site Characteristics Database',),
    39: (),
    40: (),
    41: ('Web Data',),
    42: ('Account Web Data',),
    43: ('Login Data',),
    44: ('Login Data For Account',),
    45: ('Affiliation Database',),
    46: ('AutofillStrikeDatabase',),
    47: ('BudgetDatabase',),
    50: ('ClientCertificates',),
    51: ('Extension State', 'Extension Rules', 'Extension Scripts',
         'Local Extension Settings', 'Sync Extension Settings',
         'Managed Extension Settings'),
    52: ('Sync Data',),
    53: ('shared_proto_db',),
    54: ('chrome_cart_db', 'commerce_subscription_db', 'discount_infos_db',
         'discounts_db', 'parcel_tracking_db', 'coupon_db'),
    59: ('Bookmarks', 'Bookmarks.bak', 'BookmarkMergedSurfaceOrdering'),
    60: ('ReadingList', 'Reading List', 'Offline Pages', 'OfflinePages'),
    64: ('component_crx_cache', 'extensions_crx_cache'),
    69: ('GCM Store',),
    71: (),  # Registrations share Service Worker user data; not a whole DB.
    76: ('Web Applications', 'WebAppStore', 'Isolated Web Apps'),
    77: ('Shortcuts',),
    78: ('optimization_guide_model_store', 'OptimizationHints',
         'optimization_guide_hint_cache_store', 'segmentation_platform',
         'OptGuideOnDeviceModel'),
    80: (),
    81: ('Web Data',),  # Discovered search shares the removed Web Data owner.
    83: ('CriticalActions.db',),
    84: ('ContextHub.db',),
    85: ('feedv2',),
    86: ('DownloadMetadata',),
    87: ('Articles',),
    88: ('Tabs',),
    89: ('share_history',),
    90: ('share_ranking',),
    93: ('workspaces',),
    94: ('BraveWallet', 'brave_wallet', 'AIChat', 'ai_chat'),
    96: ('persisted_state_db',),
}
# Exact dotted preference names (JSON nests these keys). Empty defaults do not
# constitute retained activity. Neither reading nor reporting mutates the file.
PREFS = {
    39: ('profile.content_settings.exceptions.site_engagement',
         'profile.content_settings.exceptions.media_engagement'),
    40: ('profile.content_settings.permission_actions',
         'profile.content_settings.exceptions.permission_autoblocking_data',
         'profile.content_settings.exceptions.permission_actions_history'),
    77: ('zerosuggest.cachedresults', 'zerosuggest.cachedresults_with_url',
         'zerosuggest.cachedresults_composebox'),
    80: ('login_detection.oauth_signed_in_origins',
         'site_isolation.user_triggered_isolated_origins',
         'site_isolation.web_triggered_isolated_origins'),
}

CDP = r'''
const [port,url,downloads,resultPath,closePath]=process.argv.slice(1);
const fs=await import('node:fs');
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
let target;
for(let i=0;i<150;i++){
  try{target=(await(await fetch(`http://127.0.0.1:${port}/json/list`,
    {signal:AbortSignal.timeout(1000)})).json()).find(t=>t.type==='page');
    if(target)break;}catch{}
  await sleep(200);
}
if(!target)throw Error('No page target');
const ws=new WebSocket(target.webSocketDebuggerUrl);
await new Promise((ok,no)=>{ws.onopen=ok;ws.onerror=no});
let id=0;const pending=new Map();
ws.onmessage=e=>{const m=JSON.parse(e.data);const p=pending.get(m.id);
  if(p){clearTimeout(p.timer);pending.delete(m.id);
    m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result)}};
const call=(method,params={})=>new Promise((resolve,reject)=>{
  const n=++id;const timer=setTimeout(()=>{pending.delete(n);
    reject(Error('CDP timeout '+method))},15000);
  pending.set(n,{resolve,reject,timer});ws.send(JSON.stringify({id:n,method,params}))});
await call('Page.enable');
await call('Page.setDownloadBehavior',{behavior:'allow',downloadPath:downloads});
await call('Page.navigate',{url});
let proof;
for(let i=0;i<200;i++){
  const x=await call('Runtime.evaluate',{
    expression:'JSON.stringify(window.proof || null)',returnByValue:true});
  proof=JSON.parse(x.result.value || 'null');
  if(proof?.error)throw Error(proof.error);
  if(proof?.ready && proof?.frame)break;
  await sleep(100);
}
if(!proof?.ready || !proof?.frame)throw Error('Incomplete fixture '+JSON.stringify(proof));
if(proof.cookie!=='inventory=first' || proof.local!=='first' || proof.session!=='first')
  throw Error('Top-level storage exercise failed '+JSON.stringify(proof));
if(proof.indexedDB!==true || proof.cache!==true || proof.worker!==true)
  throw Error('Asynchronous storage exercise failed '+JSON.stringify(proof));
await call('Runtime.evaluate',{expression:'document.querySelector("a").click()'});
for(let i=0;i<200 && !fs.existsSync(downloads+'/inventory.txt');i++)await sleep(100);
if(fs.readFileSync(downloads+'/inventory.txt','utf8')!=='alloy inventory download\n')
  throw Error('Download consumer did not receive the expected bytes');
fs.writeFileSync(resultPath,JSON.stringify({target,proof,download:true},null,2));
// Python inventories the live roots before allowing normal Page.close shutdown.
for(let i=0;i<200 && !fs.existsSync(closePath);i++)await sleep(100);
if(!fs.existsSync(closePath))throw Error('No inventory close handshake');
await call('Page.close');ws.close();
'''

PAGE = r'''<!doctype html><title>Alloy store inventory</title>
<a href="/download" download="inventory.txt">Download fixture</a>
<script>
window.proof={};
addEventListener('message',e=>{
  if(e.origin===location.origin.replace('127.0.0.1','127.0.0.2') &&
     e.source===document.querySelector('iframe')?.contentWindow && e.data==='frame-ready')
    proof.frame=true;
});
(async()=>{
  document.cookie='inventory=first; Max-Age=3600; SameSite=Lax';
  localStorage.setItem('inventory','first');sessionStorage.setItem('inventory','first');
  proof.cookie=document.cookie;proof.local=localStorage.getItem('inventory');
  proof.session=sessionStorage.getItem('inventory');
  await new Promise((ok,no)=>{const r=indexedDB.open('inventory',1);
    r.onupgradeneeded=()=>r.result.createObjectStore('values');r.onerror=()=>no(r.error);
    r.onsuccess=()=>{const db=r.result;const tx=db.transaction('values','readwrite');
      tx.objectStore('values').put('first','inventory');
      tx.oncomplete=()=>{db.close();proof.indexedDB=true;ok()};tx.onerror=()=>no(tx.error)}});
  const cache=await caches.open('inventory');await cache.put('/cached',new Response('first'));
  proof.cache=(await(await cache.match('/cached')).text())==='first';
  await navigator.serviceWorker.register('/sw.js');
  await navigator.serviceWorker.ready;proof.worker=true;
  const frame=document.createElement('iframe');
  frame.src=location.origin.replace('127.0.0.1','127.0.0.2')+'/frame';
  document.body.append(frame);proof.ready=true;
})().catch(e=>proof.error=String(e));
</script>'''

FRAME = r'''<!doctype html><title>Cross-site inventory frame</title><script>
try {document.cookie='inventory=embedded; SameSite=None; Secure';
localStorage.setItem('inventory','embedded');sessionStorage.setItem('inventory','embedded');
parent.postMessage('frame-ready','*');}catch(e){parent.postMessage('frame-error:'+e,'*')}
</script>'''


class Site(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = {'/': PAGE, '/frame': FRAME,
                '/sw.js': "self.addEventListener('install',e=>self.skipWaiting());"
                          "self.addEventListener('activate',e=>e.waitUntil(clients.claim()));",
                '/download': 'alloy inventory download\n'}.get(self.path)
        self.send_response(200 if body is not None else 404)
        if self.path == '/download':
            self.send_header('Content-Disposition', 'attachment; filename="inventory.txt"')
        self.send_header('Content-Type', 'application/javascript' if self.path == '/sw.js'
                         else 'text/plain' if self.path == '/download' else 'text/html')
        body = (body or '').encode()
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def inventory(roots):
    entries = []
    hits = {row: set() for row in PATHS}
    for root in roots:
        for path in sorted(root.rglob('*')):
            relative = path.relative_to(root)
            parts = list(relative.parts)
            while parts and parts[0] == 'Default':
                parts.pop(0)
            name = '/'.join(parts)
            entry = dict(root=str(root), path=str(relative),
                         kind='symlink' if path.is_symlink() else
                         'directory' if path.is_dir() else 'file')
            entries.append(entry)
            label = str(path.relative_to(roots[0].parent))
            for row, patterns in PATHS.items():
                if any(name == p or name.startswith(p + '/') or
                       name in (p + '-journal', p + '-wal', p + '-shm')
                       for p in patterns):
                    hits[row].add(label)
            if not path.is_symlink() and path.name in ('Preferences', 'Secure Preferences'):
                prefs = json.loads(path.read_text())
                for row, keys in PREFS.items():
                    for key in keys:
                        value = prefs
                        for part in key.split('.'):
                            value = value.get(part) if isinstance(value, dict) else None
                        if value:
                            hits[row].add(label + '#' + key)
    return entries, hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--forbid-rows', default='', help='Comma-separated store-map row numbers')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    out = args.out.resolve()
    if socket.gethostname() != 'wakaba-build' or out not in {
            WORK / 'src/out/Release_Alloy', WORK / 'src/out/Verify_Alloy'}:
        parser.error('Only the approved NUC Alloy release/verify outputs are supported')
    if os.environ.get('TMPDIR') != TMP:
        parser.error('TMPDIR must be ' + TMP)
    if not re.fullmatch(r'(?:[0-9]+(?:,[0-9]+)*)?', args.forbid_rows):
        parser.error('--forbid-rows requires comma-separated integers')
    forbidden = {int(n) for n in args.forbid_rows.split(',') if n}
    if forbidden - PATHS.keys():
        parser.error('Unmapped removal rows: ' + str(sorted(forbidden - PATHS.keys())))
    if not args.inside:
        command = ['sudo', '-n', 'unshare', '--net', '--fork', '/bin/sh', '-c',
                   'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-inventory',
                   'env', 'TMPDIR=' + TMP, 'PYTHONDONTWRITEBYTECODE=1',
                   sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--inside']
        return subprocess.run(['timeout', '--kill-after=20s', '180', *command], timeout=210).returncode
    if os.geteuid() == 0:
        parser.error('Browser must run as builder with its sandbox enabled')
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    routes = subprocess.check_output(['ip', 'route'], text=True, timeout=10)
    assert [i['ifname'] for i in interfaces] == ['lo'] and not routes.strip()
    with socket.socket() as check:
        assert check.connect_ex(('1.1.1.1', 443)) == errno.ENETUNREACH
    evidence = WORK / 'evidence' / f'store-inventory-{time.time_ns()}'
    evidence.mkdir()
    print('STORE_INVENTORY_EVIDENCE', evidence, flush=True)
    save(evidence / 'isolation.json', dict(interfaces=interfaces, routes=routes,
         uid=os.geteuid(), netns=os.readlink('/proc/self/ns/net'), outside_errno=errno.ENETUNREACH))
    # CEF's request-context cache must be below its installation root. Scan the
    # installation root recursively so the nested profile and caches are covered.
    roots = [evidence / 'profile']
    roots[0].mkdir()
    downloads = evidence / 'downloads'
    downloads.mkdir()
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Site)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as port:
        port.bind(('127.0.0.1', 0))
        debug_port = port.getsockname()[1]
    argv = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24', str(out / 'cefclient'),
            '--off-screen-rendering-enabled', '--ozone-platform=x11', '--disable-gpu',
            f'--remote-debugging-port={debug_port}', '--url=about:blank',
            f'--root-cache-path={roots[0]}', f'--cache-path={roots[0] / "Default"}']
    save(evidence / 'argv.json', argv)
    process = node = None
    try:
        with (evidence / 'stdio.log').open('w') as log, \
                (evidence / 'cdp.stdout').open('w') as stdout, \
                (evidence / 'cdp.stderr').open('w') as stderr:
            process = subprocess.Popen(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True, env=dict(os.environ, TMPDIR=TMP))
            node = subprocess.Popen(['node', '--input-type=module', '-e', CDP,
                str(debug_port), f'http://127.0.0.1:{server.server_port}/', str(downloads),
                str(evidence / 'consumer.json'), str(evidence / 'close')],
                stdout=stdout, stderr=stderr)
            deadline = time.monotonic() + 100
            while not (evidence / 'consumer.json').exists():
                if node.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Fixture failed; see ' + str(evidence / 'cdp.stderr'))
                time.sleep(0.1)
            live, hits = inventory(roots)
            save(evidence / 'live-paths.json', live)
            (evidence / 'close').touch()
            assert node.wait(timeout=30) == 0
            assert process.wait(timeout=20) == 0
            assert not wait_for_session_exit(process.pid, 10), 'Browser children remain'
            final, final_hits = inventory(roots)
            save(evidence / 'final-paths.json', final)
            for row in hits:
                hits[row].update(final_hits[row])
            save(evidence / 'stores.json', {str(row): sorted(paths) for row, paths in hits.items()})
            failed = forbidden & {row for row, paths in hits.items() if paths}
            for row, paths in sorted(hits.items()):
                if row in forbidden:
                    print(('FAIL store-present ' if paths else 'PASS store-absent ') + str(row),
                          json.dumps(sorted(paths)) if paths else '', flush=True)
                elif paths:
                    print('PRESENT store', row, json.dumps(sorted(paths)), flush=True)
            save(evidence / 'result.json', dict(pass_=not failed, forbidden=sorted(forbidden),
                 failed=sorted(failed), normal_shutdown=True, consumer=json.loads(
                     (evidence / 'consumer.json').read_text())))
            if failed:
                return 1
    finally:
        if node and node.poll() is None:
            node.terminate()
            node.wait(timeout=5)
        if process:
            if session_processes(process.pid):
                terminate_session(process.pid)
            process.wait(timeout=5)
        server.shutdown()
        server.server_close()
    print('PASS store-inventory', evidence, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
