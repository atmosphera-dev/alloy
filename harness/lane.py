# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Namespace-contained Xvfb, browser, trigger server and seeded human-paced CDP walker."""
import http.server, threading, urllib.request, urllib.parse, xml.etree.ElementTree as ET
import random, signal, math, struct, wave, io, traceback, socket
from common import *
from cdp import CDP
from rhythm import Rhythm
guard()
lane=next(l for l in LANES if l['name']==sys.argv[1]); D=pathlib.Path(sys.argv[2]); parent=D.parent
OWN=lock(D/'lane.lock'); LOG=Log(D/'walker.jsonl')
RUN= D/('run-'+str(time.time_ns())); RUN.mkdir()
seed=lane['seed'];RHYTHM=Rhythm(D/'rhythm-state.json',seed,float(os.environ['SOAK_CAMPAIGN_ORIGIN']),time.time);rng=RHYTHM.rng
last_heartbeat=0;browser=None;profile=None;operation='startup';operation_deadline=None
def heartbeat(force=False):
    global last_heartbeat
    if stop:raise InterruptedError('owned lane stop requested; preserve in-flight action')
    if not force and time.monotonic()-last_heartbeat<5:return
    last_heartbeat=time.monotonic()
    save(D/'status.json',dict(time=now(),epoch=time.time(),pid=os.getpid(),browser_pid=browser.pid if browser else None,pages=RHYTHM.data['pages'],block=RHYTHM.data['block'],profile=str(profile),run=str(RUN),phase=RHYTHM.data['phase'] or 'startup',operation=operation,operation_deadline=operation_deadline,seed=seed))
stop=False
def terminate(*_):
    global stop
    stop=True
    RHYTHM.flush()
signal.signal(signal.SIGTERM,terminate);signal.signal(signal.SIGINT,terminate)
assert pathlib.Path('/proc/self/ns/net').stat().st_ino==int(os.environ['SOAK_NETNS_INODE']),'wrong network namespace'
assert (parent/'isolation-verified.json').exists(),'no isolation gate'
resolver=pathlib.Path('/etc/resolv.conf').read_text()
assert resolver.startswith('nameserver '+lane['host']+'\n'),'systemd resolver bind mismatch'
assert 'hosts: files dns' in pathlib.Path('/etc/nsswitch.conf').read_text(),'systemd NSS bind mismatch'
resolved=socket.getaddrinfo('nomad.home',8090,socket.AF_INET,socket.SOCK_STREAM)
assert {entry[4][0] for entry in resolved}=={'10.4.20.237'},'consumer DNS mismatch'
save(RUN/'namespace-consumer-preflight.json',dict(time=now(),uid=os.getuid(),resolver=resolver,addresses=resolved,netns=os.readlink('/proc/self/ns/net'),expected_inode=os.environ['SOAK_NETNS_INODE'],before_browser=True))
LOG.event('lane_start',lane=lane,seed=seed,run=str(RUN),pid=os.getpid(),cgroup=pathlib.Path('/proc/self/cgroup').read_text(),netns=os.readlink('/proc/self/ns/net'))

def pdf():
    parts=[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    text=b'BT /F1 18 Tf 40 300 Td (Wakaba local PDF consumer control) Tj ET'
    parts.append(b'<< /Length '+str(len(text)).encode()+b' >>\nstream\n'+text+b'\nendstream')
    data=b'%PDF-1.4\n'; offsets=[0]
    for i,p in enumerate(parts,1): offsets.append(len(data)); data+=f'{i} 0 obj\n'.encode()+p+b'\nendobj\n'
    x=len(data);data+=b'xref\n0 6\n0000000000 65535 f \n'+b''.join(f'{o:010d} 00000 n \n'.encode() for o in offsets[1:])+f'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{x}\n%%EOF\n'.encode();return data
tone=io.BytesIO()
with wave.open(tone,'wb') as w:
    w.setparams((1,2,16000,0,'NONE','not compressed'));w.writeframes(b''.join(struct.pack('<h',int(3000*math.sin(2*math.pi*440*i/16000))) for i in range(48000)))
download=b'Wakaba local download consumer control\n'
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path=urllib.parse.urlsplit(self.path).path
        content_type='text/html; charset=utf-8'
        if path=='/trigger.js': content_type='text/javascript';body=(SCRIPTS/'trigger.js').read_bytes()
        elif path=='/sw.js': content_type='text/javascript';body=b"self.addEventListener('install',e=>self.skipWaiting());self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));self.addEventListener('push',e=>e.waitUntil(self.registration.showNotification('Wakaba control')));"
        elif path=='/tone.wav': content_type='audio/wav';body=tone.getvalue()
        elif path=='/control.pdf': content_type='application/pdf';body=pdf()
        elif path=='/download.txt': content_type='application/octet-stream';body=download
        elif path in ['/fr','/de']:
            lang=path[1:];text='Bonjour, ceci est une page française. La science et la bibliothèque sont importantes.' if lang=='fr' else 'Guten Tag, dies ist eine deutsche Seite. Wissenschaft und Bibliotheken sind wichtig.'
            body=(f'<!doctype html><html lang="{lang}"><title>Wakaba {lang}</title><body><h1>{text}</h1>'+''.join(f'<p>{text}</p>' for _ in range(100))+'</body></html>').encode()
        else: body=b'<!doctype html><html lang="en"><title>Wakaba soak trigger</title><body><h1>Wakaba soak trigger</h1><textarea id="spell" spellcheck="true" lang="en-US"></textarea><a id="pdf" href="/control.pdf">Open local PDF</a><script src="/trigger.js"></script></body></html>'
        self.send_response(200);self.send_header('Content-Type',content_type);self.send_header('Content-Length',str(len(body)))
        if path=='/download.txt': self.send_header('Content-Disposition','attachment; filename="wakaba-control.txt"')
        self.end_headers();self.wfile.write(body)
        LOG.event('trigger_http',path=self.path,bytes=len(body),status=200)
    def log_message(self,*_): pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',8765),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()

def attach(browser,deadline=45):
    until=time.monotonic()+deadline
    while time.monotonic()<until and browser.poll() is None and not stop:
        try:
            targets=json.load(urllib.request.urlopen('http://127.0.0.1:9222/json/list',timeout=2))
            pages=[t for t in targets if t['type']=='page']
            if pages:
                c=CDP(pages[0]['webSocketDebuggerUrl'],RUN/'cdp.jsonl')
                for method in ['Network.enable','Page.enable','Runtime.enable']:
                    answer=c.call(method);assert 'error' not in answer,(method,answer)
                LOG.event('cdp_attached',target=pages[0],version=c.call('Browser.getVersion'))
                c.tick=heartbeat
                return c
        except Exception as e: LOG.event('attach_attempt',error=repr(e))
        time.sleep(.5)
    raise RuntimeError('no page target')
def nav(c,url,action='page'):
    assert urllib.parse.urlsplit(url).netloc in ['nomad.home:8090','localhost:8765'] or url=='about:blank',url
    result=c.call('Page.navigate',{'url':url}); c.pump(3)
    end=time.monotonic()+25; consumer=None
    while time.monotonic()<end:
        consumer=c.evaluate("(()=>{const f=document.querySelector('iframe');const d=f?.contentDocument||document;return {href:location.href,title:document.title,text:d.body?.innerText?.slice(0,1500),ready:d.readyState,frameHref:f?.contentWindow?.location.href,links:Array.from(d.querySelectorAll('a[href]')).map(a=>({href:a.href,text:a.textContent.slice(0,150)})).filter(a=>a.href.startsWith('http://nomad.home:8090/')).slice(0,500),videos:Array.from(d.querySelectorAll('video, source')).map(v=>v.src||v.currentSrc).filter(Boolean)}})()")
        if consumer and consumer['ready']=='complete' and consumer.get('text'): break
        c.pump(1)
    rendered=(consumer or {}).get('frameHref') or (consumer or {}).get('href')
    doc_status=[e['params']['response']['status'] for e in c.events if e.get('method')=='Network.responseReceived' and e['params'].get('type')=='Document' and urllib.parse.unquote(e['params']['response']['url']).split('#')[0]==urllib.parse.unquote(rendered or '').split('#')[0]]
    success=bool(consumer and consumer.get('text') and consumer['ready']=='complete' and doc_status and 200<=doc_status[-1]<300 and 'error' not in result and 'errorText' not in result.get('result',{}))
    LOG.event('navigation',action=action,url=url,result=result,consumer=consumer,document_status=doc_status[-1] if doc_status else None,success=success)
    if not success: raise RuntimeError('navigation consumer failed '+url)
    return consumer
def feature_suite(c,books,block):
    global operation,operation_deadline
    LOG.event('trigger_suite_start',block=block)
    nav(c,'http://localhost:8765/','trigger-page')
    for feature,permission in [('geolocation','geolocation'),('notification','notifications'),('push','notifications')]:
        try: LOG.event('permission_attempt',feature=feature,result=c.call('Browser.grantPermissions',{'origin':'http://localhost:8765','permissions':[permission]}))
        except Exception as e: LOG.event('permission_attempt',feature=feature,error=repr(e))
    c.evaluate("document.querySelector('#spell').focus()")
    typed=c.call('Input.insertText',{'text':'Thiss sentense contains mispelled wurds and teh spellchek trigger.'})
    value=c.evaluate("({value:document.querySelector('#spell').value,spellcheck:document.querySelector('#spell').spellcheck,active:document.activeElement.id})")
    LOG.event('feature',name='spellcheck',result=typed,consumer=value,status='typed-spellcheck-enabled' if value and value['value'].startswith('Thiss') else 'failed',correction_ui='not exposed by DOM; dictionary traffic captured independently')
    for name in ['canary','geolocation','notification','eme','speech','webrtc','push','audio','download']:
        try:
            operation='feature:'+name;operation_deadline=time.time()+(600 if name=='canary' else 25)
            RHYTHM.begin_action(operation,deadline=operation_deadline);heartbeat(True)
            result=c.evaluate('window.feature('+json.dumps(name)+')',limit=600 if name=='canary' else 25);c.pump(2)
            if name=='canary':
                failures=[f for f in c.failures if '/wakaba-canary' in (f.get('request') or {}).get('url','') and f['failure'].get('errorText')!='net::ERR_ABORTED']
                if lane['mode']=='enforce':
                    assert result.get('consumer')=='network-error' and result.get('errorName')=='TypeError' and failures,('missing browser network-error consumer',result,failures)
                else:assert result.get('value',{}).get('consumer')=='fetch-completed',result
                result['cdp_network_failures']=failures
            if name=='download':
                files=[p for p in (D/'downloads').glob('*') if p.is_file() and p.suffix!='.crdownload']
                matches=[str(p) for p in files if p.read_bytes()==download]
                result['filesystem_matches']=matches;result['status']='success' if matches else 'failed-or-unsupported'
            LOG.event('feature',name=name,result=result,block=block)
            RHYTHM.finish_action()
        except Exception as e:
            LOG.event('feature',name=name,status='failed-or-unsupported',error=repr(e),block=block)
            if name=='canary':raise # A failed control cannot be silently followed by navigation/crash.
    for language in ['fr','de']:
        try:
            consumer=nav(c,'http://localhost:8765/'+language,'translate-'+language)
            LOG.event('feature',name='translate-'+language,consumer=consumer,status='failed-or-unsupported',translation='Foreign-language trigger DOM loaded; cefclient/CDP exposes no native translate action/outcome. No completed translation claimed.')
        except Exception as e: LOG.event('feature',name='translate-'+language,status='failed-or-unsupported',error=repr(e))
    try:
        c.call('Page.navigate',{'url':'http://localhost:8765/control.pdf'});c.pump(6)
        consumer=c.evaluate("({href:location.href,type:document.contentType,embeds:Array.from(document.querySelectorAll('embed')).map(e=>({src:e.src,type:e.type})),text:document.body?.innerText})")
        # Also require PDF request response and loading finish in raw CDP; gate validates.
        ax=c.call('Accessibility.getFullAXTree');texts=[n.get('name',{}).get('value','') for n in ax.get('result',{}).get('nodes',[])]
        pdftext=any('Wakaba local PDF consumer control' in str(t) for t in texts)
        LOG.event('feature',name='pdf',consumer=consumer,accessibility=ax,status='success' if pdftext else 'failed-or-unsupported',rendered_text=pdftext,limitation=None if pdftext else 'PDF viewer present/attempted but accessible rendered PDF text not observed')
    except Exception as e: LOG.event('feature',name='pdf',status='failed-or-unsupported',error=repr(e))
    videos=[b for b in books if b.get('video')]
    try:
        assert videos,'no video books'
        vbook=rng.choice(videos);page=nav(c,'http://nomad.home:8090'+vbook['path'],'video-book')
        # Actual links from the ZIM menu, never invented article/video paths.
        for attempt in range(4):
            result=c.evaluate("(async()=>{const d=document.querySelector('iframe')?.contentDocument||document;const v=d.querySelector('video');if(!v)return {status:'no-video'};v.muted=true;const before=v.currentTime;try{await v.play();await new Promise(r=>setTimeout(r,3000));return {status:v.currentTime>before?'playing':'not-advancing',before,after:v.currentTime,src:v.currentSrc,error:v.error?.message}}catch(e){return {status:'failed',error:String(e),src:v.currentSrc}}})()")
            if result and result['status']!='no-video': break
            choices=[a for a in page['links'] if '/content/'+vbook['book']+'/' in a['href']]
            if not choices: break
            page=nav(c,rng.choice(choices)['href'],'video-article')
        LOG.event('feature',name='kiwix-video',book=vbook,result=result,status='success' if result and result.get('status')=='playing' else 'failed-or-unsupported')
    except Exception as e: LOG.event('feature',name='kiwix-video',status='failed-or-unsupported',error=repr(e))
    LOG.event('trigger_suite_end',block=block)
    operation='active';operation_deadline=None

def verify_kiwix(c,books):
    """Deterministic gate consumer actions, separate from randomized human-paced walk."""
    global operation,operation_deadline
    operation='kiwix-controls';operation_deadline=None
    wiki=next(b for b in books if b['book']=='wikipedia_en_all_mini_2025-12')
    chosen=[wiki,rng.choice([b for b in books if b!=wiki])]
    for book in chosen:
        catalogview,page=catalog_book(c,book)
        LOG.event('book_switch',book=book,catalog_consumer=catalogview,page_consumer=page,kind='consumer-control',success=book['book'] in (page.get('frameHref') or page['href']))
        c.pump(15)
    search=nav(c,'http://nomad.home:8090/search?'+urllib.parse.urlencode(dict(content=wiki['book'],pattern='Science')),'search')
    results=[a for a in search['links'] if '/content/'+wiki['book']+'/' in a['href']];assert results,'search returned no article links'
    LOG.event('search',query='Science',book=wiki,consumer=search,results=len(results),success=True,kind='consumer-control')
    nav(c,results[0]['href'],'search-result');RHYTHM.flush()

def catalog_book(c,book):
    # Kiwix's public hash filter drives resetAndFilter/hashchange and native
    # catalog loading. The default catalog is viewport-paginated and remembers
    # a language cookie, so an arbitrary one of48 books is not initially visible.
    url='http://nomad.home:8090/#'+urllib.parse.urlencode(dict(lang='',q=book['title']))
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        catalog=nav(c,url,'library-catalog')
        links=[a for a in catalog['links'] if book['book'] in urllib.parse.unquote(a['href'])]
        if links:return catalog,nav(c,links[0]['href'],'book-switch')
        c.pump(2)
    raise RuntimeError('native catalog filter did not expose selected book: '+str(book))

def terminate_child(p,name):
    if p and p.poll() is None:
        p.terminate()
        try:p.wait(timeout=10)
        except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
    LOG.event('child_exit',child=name,pid=p.pid if p else None,returncode=p.poll() if p else None)
xvfb=None;browser=None;c=None
try:
    xvlog=(RUN/'xvfb.log').open('xb')
    xvfb=subprocess.Popen(['Xvfb',':99','-screen','0','1600x1000x24','-nolisten','tcp','-ac'],stdout=xvlog,stderr=subprocess.STDOUT)
    time.sleep(1);assert xvfb.poll() is None,'Xvfb died'
    start=time.time(); day=int(json.loads((D/'profile-state.json').read_text()).get('day',0)) if (D/'profile-state.json').exists() else 0
    profile=D/'profiles'/f'day-{day:03d}';profile.mkdir(parents=True,exist_ok=True)
    (D/'downloads').mkdir(exist_ok=True)
    args=[lane['binary'],'--url=about:blank','--ozone-platform=x11','--disable-gpu','--remote-debugging-port=9222','--remote-allow-origins=*','--cache-path='+str(profile),'--log-net-log='+str(RUN/'netlog.json'),'--net-log-capture-mode=Default','--alloy-guard-canary-url=https://example.com/alloy-guard-canary','--log-file='+str(RUN/'debug.log'),'--log-severity=verbose']
    if lane['build'].endswith('osr'):args+=['--off-screen-rendering-enabled']
    save(RUN/'command.json',dict(argv=args,env={k:os.environ.get(k) for k in ['HOME','TMPDIR','DISPLAY','DBUS_SESSION_BUS_ADDRESS']},profile=str(profile),sandbox_disabled=False))
    stdio=(RUN/'browser-stdio.log').open('xb')
    browser=subprocess.Popen(args,cwd=pathlib.Path(lane['binary']).parent,stdout=stdio,stderr=subprocess.STDOUT)
    c=attach(browser)
    try:
        r=c.call('Browser.setDownloadBehavior',{'behavior':'allow','downloadPath':str(D/'downloads'),'eventsEnabled':True});LOG.event('download_behavior',result=r)
        if 'error' in r:LOG.event('download_behavior_page',result=c.call('Page.setDownloadBehavior',{'behavior':'allow','downloadPath':str(D/'downloads')}))
    except Exception as e:LOG.event('download_behavior',error=repr(e))
    # Retain browser's own protocol schema, source identity and seed inputs.
    save(RUN/'protocol.json',json.load(urllib.request.urlopen('http://127.0.0.1:9222/json/protocol',timeout=5)))
    catalog=(D/'prepared-catalog.xml').read_bytes();(RUN/'catalog.xml').write_bytes(catalog)
    root=ET.fromstring(catalog); ns={'a':'http://www.w3.org/2005/Atom'};books=[]
    for entry in root.findall('a:entry',ns):
        links=[x.attrib['href'] for x in entry.findall('a:link',ns) if x.attrib.get('type')=='text/html']
        if links:
            path=links[0];books.append(dict(path=path,book=path.rsplit('/',1)[-1],title=entry.findtext('a:title',namespaces=ns),video='_videos:yes' in entry.findtext('a:tags',default='',namespaces=ns)))
    save(RUN/'books.json',books);assert len(books)>=2,books
    LOG.event('catalog',books=books,count=len(books),seed=seed)
    operation='consumer-ready';heartbeat(True)
    if RHYTHM.data.get('action'):LOG.event('interrupted_action_retained',action=RHYTHM.data['action'])
    page=None;book=next(b for b in books if b['book']==RHYTHM.data.get('book','wikipedia_en_all_mini_2025-12'))
    while not stop and browser.poll() is None:
        phase=RHYTHM.phase();block=RHYTHM.data['block'];pages=RHYTHM.data['pages'];operation=phase;operation_deadline=None
        LOG.event('phase_resume',state=RHYTHM.data,profile=str(profile))
        if phase=='idle':
            while time.time()<RHYTHM.data['until'] and not stop:c.pump(min(5,RHYTHM.data['until']-time.time()));heartbeat()
            continue
        LOG.event('active_block',block=block,active_seconds=RHYTHM.data['duration'])
        if RHYTHM.data['suite_block']!=block:
            feature_suite(c,books,block);RHYTHM.suite_done()
        if not RHYTHM.data.get('kiwix_controls_done'):
            verify_kiwix(c,books);RHYTHM.data['kiwix_controls_done']=True;RHYTHM.flush()
        # Daily Page.crash is an explicit feature attempt, never silent exception recovery.
        day=RHYTHM.day()
        if RHYTHM.crash_due():
            LOG.event('feature',name='renderer-crash',status='attempt',day=day)
            try:LOG.event('feature',name='renderer-crash',result=c.call('Page.crash',limit=5),status='requested')
            except Exception as e:LOG.event('feature',name='renderer-crash',error=repr(e),status='expected-disconnect-or-timeout')
            c.close();c=attach(browser)
        save(D/'probe-ready.json',dict(time=now(),pid=os.getpid(),block=block,canary_network_lifetime_verified=True,kiwix_consumers_verified=True))
        wiki=next(b for b in books if b['book']=='wikipedia_en_all_mini_2025-12')
        consecutive_errors=0
        while time.time()<RHYTHM.data['until'] and not stop:
            try:
                operation='page-action';RHYTHM.begin_action(operation,page=pages+1);heartbeat(True)
                if pages%20==0 or page is None:
                    book=wiki if rng.random()<.6 else rng.choice([b for b in books if b!=wiki])
                    catalogview,page=catalog_book(c,book)
                    LOG.event('book_switch',book=book,catalog_consumer=catalogview,page_consumer=page,success=('/content/'+book['book']) in (page.get('frameHref') or page['href']))
                elif pages in [1,3] or rng.random()<.10:
                    query=rng.choice(['Science','history','computer','water','music'])
                    # Use Wikipedia for mandatory search coverage; subsequent searches use current book.
                    searchbook=wiki if pages in [1,3] else book
                    search=nav(c,'http://nomad.home:8090/search?'+urllib.parse.urlencode(dict(content=searchbook['book'],pattern=query)),'search')
                    results=[a for a in search['links'] if '/content/' in a['href']]
                    LOG.event('search',query=query,book=searchbook,consumer=search,results=len(results),success=bool(results))
                    if results:page=nav(c,rng.choice(results)['href'],'search-result')
                    else:page=search
                elif rng.random()<.11:
                    history=c.call('Page.getNavigationHistory')['result'];index=history['currentIndex']
                    entries=[e for e in history['entries'][:index] if e['url'].startswith('http://nomad.home:8090/')]
                    if entries:c.call('Page.navigateToHistoryEntry',{'entryId':entries[-1]['id']});c.pump(3);page=nav(c,entries[-1]['url'],'back-consumer')
                else:
                    links=[a for a in page['links'] if '/content/'+book['book']+'/' in a['href'] and not a['href'].lower().endswith(('.jpg','.png','.svg','.pdf','.mp4','.webm','.ogg'))]
                    if links:
                        url=rng.choice(links)['href']
                        if rng.random()<.5:
                            relative=url.split('/content/',1)[1];url='http://nomad.home:8090/viewer#'+relative
                        page=nav(c,url,'same-origin-link')
                    else:page=None;continue
                pages+=1;RHYTHM.data['pages']=pages;RHYTHM.data['book']=book['book'];dwell=rng.randint(15,120);LOG.event('dwell',page=pages,seconds=dwell)
                remaining=dwell
                while remaining>0 and not stop:
                    scroll=c.evaluate("(()=>{const w=document.querySelector('iframe')?.contentWindow||window;const before=w.scrollY;w.scrollBy({top:180,behavior:'smooth'});return {before,height:w.document.body.scrollHeight}})()")
                    c.pump(min(remaining,5));remaining-=5
                RHYTHM.finish_action();consecutive_errors=0;heartbeat(True)
            except Exception as e:
                LOG.event('walker_action_error',error=repr(e),traceback=traceback.format_exc());page=None;c.pump(2)
                consecutive_errors+=1
                if consecutive_errors>=3:raise RuntimeError('repeated walker consumer failures')
    if browser.poll() is not None and not stop:raise RuntimeError('browser exited '+str(browser.returncode))
except InterruptedError:
    if not stop:raise
    LOG.event('requested_stop',interrupted_action=RHYTHM.data.get('action'))
except Exception as e:
    kind='browser-crash' if browser and browser.poll() is not None and not stop else 'harness-error'
    LOG.event('fatal',kind=kind,error=repr(e),traceback=traceback.format_exc());save(D/('failure-'+str(time.time_ns())+'.json'),dict(time=now(),kind=kind,pid=os.getpid(),run=str(RUN),error=repr(e),traceback=traceback.format_exc()));raise
finally:
    RHYTHM.flush()
    # Shutdown owns a separate CDP phase. A walking cancellation must never
    # interrupt Page.close or the browser's normal process-exit deadline.
    shutdown=dict(time=now(),normal=False,forced=False)
    if c:
        c.tick=lambda:None
        try:LOG.event('page_close',result=c.call('Page.close',limit=5))
        except Exception as e:LOG.event('page_close',error=repr(e))
        try:c.close()
        except Exception:pass
    if browser:
        try:
            browser.wait(timeout=30)
            shutdown['normal']=browser.returncode==0
        except subprocess.TimeoutExpired:
            shutdown['forced']=True
            terminate_child(browser,'browser')
        shutdown.update(exit=browser.returncode,pid=browser.pid,finished=now())
    save(RUN/'browser-shutdown.json',shutdown)
    LOG.event('browser_shutdown',shutdown=shutdown)
    if (RUN/'netlog.json').exists():LOG.event('netlog_retained',path=str(RUN/'netlog.json'),bytes=(RUN/'netlog.json').stat().st_size,full_run=True)
    terminate_child(xvfb,'xvfb');server.shutdown();LOG.event('lane_exit');LOG.close()
