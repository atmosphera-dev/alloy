# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Outside-run-loop snapshots and one-pass tools. NUC only; 600s total budget."""
import argparse
from pathlib import Path
import collections
import datetime
import fcntl
import hashlib
import importlib.util
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.parse
from dscp import check as dscp_check, packets, classes as wire_classes
from netlog_stream import adapt as adapt_netlog

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--campaign',required=True,type=Path)
parser.add_argument('--source',required=True,type=Path)
parser.add_argument('--tag',required=True)
parser.add_argument('--out',type=Path,help='Exclusive analysis output, outside a sealed run for readback')
args=parser.parse_args()
campaign=args.campaign.resolve();config=json.loads((campaign/'config.json').read_text())
W=Path('/home/builder/work/alloy'); E=campaign/'analysis'
LANES=[l['name'] for l in config['lanes']]
assert socket.gethostname()=='wakaba-build' and os.environ['TMPDIR']=='/home/builder/work/tmp'
assert os.geteuid()==0
start=time.monotonic(); deadline=start+595
lock=(W/'harness-analysis.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
tag=args.tag
E.mkdir(exist_ok=True)
out=args.out.resolve() if args.out else E/tag
out.mkdir(); inputs=out/'inputs';inputs.mkdir();tools=out/'tools';tools.mkdir()
def dump(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def run(argv,label,limit=200,env=None):
    remain=int(deadline-time.monotonic());assert remain>0,'analysis deadline'
    t=time.monotonic();p=subprocess.run(['timeout',str(min(limit,remain)),*map(str,argv)],env=env or os.environ,
        capture_output=True,text=True,timeout=min(limit,remain)+5)
    row=dict(argv=list(map(str,argv)),exit=p.returncode,seconds=time.monotonic()-t,stdout=p.stdout,stderr=p.stderr)
    dump(out/(label+'.json'),row)
    assert p.returncode==0,(label,row)
    return p.stdout
def fixed_copy(src,dst):
    size=src.stat().st_size
    with src.open('rb') as f, dst.open('xb') as g:
        left=size
        while left:
            b=f.read(min(left,1024*1024));assert b,src
            g.write(b);left-=len(b)
    manifest.append(dict(source=str(src),copy=str(dst.relative_to(out)),bytes=size,sha256=digest(dst)))
    return dst
def lines(srcs,dst,since=None):
    with dst.open('xb') as target:
        for src in sorted(srcs):
            raw=out/'raw'/src.relative_to(campaign);raw.parent.mkdir(parents=True,exist_ok=True)
            fixed_copy(src,raw)
            data=raw.read_bytes();complete=data[:data.rfind(b'\n')+1]
            if len(complete)!=len(data):partial.append(dict(path=str(src),deferred_bytes=len(data)-len(complete)))
            if since is None:target.write(complete)
            else:
                for line in complete.splitlines(True):
                    stamp=line.split(b' ',1)[0].decode()
                    if datetime.datetime.fromisoformat(stamp).timestamp()>=since:target.write(line)
def rows(path):
    for line in path.read_text().splitlines():yield json.loads(line)
def pcap(src,dst,since,guest,gateway):
    raw=out/'raw'/src.relative_to(campaign);raw.parent.mkdir(parents=True,exist_ok=True)
    fixed_copy(src,raw)
    data=raw.read_bytes();assert len(data)>=24,src
    endian={b'\xd4\xc3\xb2\xa1':'<',b'\xa1\xb2\xc3\xd4':'>'}.get(data[:4]);assert endian,src
    assert struct.unpack(endian+'I',data[20:24])[0]==1,'expected Ethernet veth capture'
    pos=24; kept=0;pre=0
    with dst.open('xb') as target:
        target.write(data[:24])
        while pos+16<=len(data):
            sec,usec,n,original=struct.unpack(endian+'IIII',data[pos:pos+16]);end=pos+16+n
            if end>len(data):break
            chunk=data[pos:end];frame=data[pos+16:end];pos=end
            if sec+usec/1e6<since:pre+=1;continue
            target.write(chunk);kept+=1
    if pos<len(data):partial.append(dict(path=str(src),deferred_bytes=len(data)-pos,kind='pcap-tail'))
    return dict(kept_packets=kept,prebrowser_packets=pre)

manifest=[];partial=[];results={};packet_details={}
contract=json.loads((campaign/'config.json').read_text())
boundary=json.loads((campaign/'collection-start.json').read_text())['epoch']
dump(out/'contract.json',contract)
status=json.loads((campaign/'status.json').read_text());dump(out/'status.json',status)
for source in ['quick-extract.py','quick-ip-map.py','dscp.py','hosts.py','netlog_relabel.py']:
    shutil.copy2(Path(__file__).parent/source,tools/source)

for l in json.loads((campaign/'network-ready.json').read_text())['lanes']:
    lane=l['name']
    src=campaign/lane;dst=inputs/lane;dst.mkdir();(dst/'captures').mkdir()
    walk=out/(lane+'-walker.jsonl');lines(src.glob('walker*.jsonl'),walk)
    events=list(rows(walk));begin=min(r['epoch'] for r in events if r['event']=='lane_start')
    lines(src.glob('dnsmasq*.log'),dst/'dnsmasq.log',since=boundary)
    lines(src.glob('proxy*.jsonl'),dst/'proxy.jsonl')
    # Proxy preflight controls only reach example.com, explicitly retained and labelled.
    protocol_outside=collections.Counter();control=collections.Counter();packet_details[lane]={}
    for p in sorted((src/'captures').glob('all-*.pcap')):
        packet_details[lane][p.name]=pcap(p,dst/'captures'/p.name,boundary,l['guest'],l['host'])
    guards=[]
    for p in sorted((src/'profiles').glob('**/alloy-guard.log')):
        q=out/'guard'/lane/p.relative_to(src);q.parent.mkdir(parents=True,exist_ok=True)
        raw=out/'raw'/p.relative_to(campaign);raw.parent.mkdir(parents=True,exist_ok=True);fixed_copy(p,raw)
        data=raw.read_bytes();q.write_bytes(data[:data.rfind(b'\n')+1]);guards.extend(rows(q))
    annotation=collections.Counter(g['annotation_id'] for g in guards)
    classes=collections.Counter(g['class'] for g in guards)
    page_actions={'same-origin-link','search-result','book-switch'}
    navigations=[r for r in events if r['event']=='navigation' and r['action'] in page_actions]
    searches=[r for r in events if r['event']=='search']
    switches=[r for r in events if r['event']=='book_switch']
    page_ok=sum(bool(r.get('success')) for r in navigations)
    search_ok=sum(bool(r.get('success') and r.get('results',0)>0 and r.get('consumer',{}).get('text')) for r in searches)
    good_books={r['book']['book'] for r in switches if r.get('success')}
    native_canaries=[]
    for p in sorted(src.glob('run-*/debug.log')):
        native_canaries.extend(re.findall(r'ALLOY_GUARD_CANARY_RESULT[^\n]*',p.read_text(errors='replace')))
    results[lane]=dict(guard_entries=len(guards),guard_by_annotation=dict(annotation),guard_by_class=dict(classes),
        page_success=page_ok,page_attempts=len(navigations),search_success=search_ok,search_attempts=len(searches),
        book_success=sum(bool(r.get('success')) for r in switches),book_attempts=len(switches),distinct_books=len(good_books),
        books=sorted(good_books),walker_action_errors=sum(r['event']=='walker_action_error' for r in events),
        fatal=[r for r in events if r['event']=='fatal'],
        feature_counts=dict(collections.Counter(r.get('name') for r in events if r['event']=='feature')),
        startup_canary_results=native_canaries,
        startup_canary_denials=sum(g['annotation_id']=='alloy_guard_canary' and g['endpoint']=='https://example.com/alloy-guard-canary' for g in guards),
        dscp=dscp_check(sorted((dst/'captures').glob('all-*.pcap')),l,boundary),
        protocol_control=dict(control),
        browser_start_epoch=begin)
    # Lossless complete-event adaptation of the append-only NetLog, not a new classifier.
    for p in sorted(src.glob('run-*/netlog.json')):
        runview=dst/p.parent.name/'netlog-parts-after-stop';runview.mkdir(parents=True)
        raw=out/'raw'/p.relative_to(campaign);raw.parent.mkdir(parents=True,exist_ok=True);fixed_copy(p,raw)
        adapted=adapt_netlog(raw,runview)
        partial.extend(adapted['partial_tails'])
        dump(runview/'provenance.json',dict(source_marker=str(p),**adapted,raw_copy=str(raw),raw_sha256=digest(raw),
             snapshot_epoch=time.time(),bounded_netlog=False,adaptation='envelope only; every complete event retained in order'))

quick_out=out/'quick'
run(['python3',tools/'quick-extract.py','--campaign',inputs,'--out',quick_out,'--origin',str(contract['campaign_origin']),
     *[x for lane in LANES for x in ['--lane',lane]]],'quick-extract',limit=60)
quick=json.loads((quick_out/'quick-extract.json').read_text())
for lane in LANES:
    mapout=out/(lane+'-ip-map.json')
    run(['python3',tools/'quick-ip-map.py','--campaign',inputs,'--config',campaign/'config.json','--lane',lane,'--out',mapout],
        'quick-ip-map-'+lane,limit=180)
    packet_rows=json.loads(mapout.read_text())
    byhost={}
    for host,value in quick[lane]['dns'].items():
        if value['label']=='outside':byhost.setdefault(host,dict(dns_queries=0,proxy_requests=0,tls_hellos=0,packets=0,syn=0))['dns_queries']+=value['n']
    for host,value in quick[lane]['proxy'].items():
        if value['label']=='outside':
            target=byhost.setdefault(host,dict(dns_queries=0,proxy_requests=0,tls_hellos=0,packets=0,syn=0));target['proxy_requests']+=sum(value['req'].values());target['tls_hellos']+=value['sni']
    for row in packet_rows:
        if row['host']=='example.com':continue
        target=byhost.setdefault(row['host'],dict(dns_queries=0,proxy_requests=0,tls_hellos=0,packets=0,syn=0));target['packets']+=row['packets'];target['syn']+=row['syn']
    results[lane]['outside_by_host']=byhost
    results[lane]['packet_destinations']=packet_rows
    results[lane]['canary_proxy_requests']=[r for r in rows(inputs/lane/'proxy.jsonl') if 'alloy-guard-canary' in r.get('path','')]

run(['python3',tools/'netlog_relabel.py','--source',args.source,'--campaign',inputs,
     *[x for lane in LANES for x in ['--lane',lane]],'--output',out/'relabel'],'netlog-relabel',limit=240)
relabel=json.loads((out/'relabel/relabel.json').read_text())
for lane in LANES:
    data=relabel['lanes'][lane];results[lane]['netlog_annotations']={relabel['seen_annotations'][h]['label']:v['requests'] for h,v in data['annotations'].items()}
    results[lane]['netlog_events']=sum(r['coverage']['raw_events'] for r in data['runs'].values())
    results[lane]['netlog_unknowns']={h:v['requests'] for h,v in data['annotations'].items() if not relabel['seen_annotations'][h]['ids']}
    results[lane]['outside_annotation_requests']={relabel['seen_annotations'][h]['label']:{host:hv['requests'] for host,hv in v['hosts'].items() if host not in ['nomad.home','localhost','127.0.0.1','example.com','(no host)']} for h,v in data['annotations'].items()}
    results[lane]['outside_annotation_requests']={k:v for k,v in results[lane]['outside_annotation_requests'].items() if v}
    results[lane]['browser_service_netlog_requests']={relabel['seen_annotations'][h]['label']:v['requests'] for h,v in data['annotations'].items() if relabel['seen_annotations'][h]['category']=='browser service'}
    results[lane]['unlogged_browser_service_annotations']={name:n for name,n in results[lane]['browser_service_netlog_requests'].items() if results[lane]['guard_by_annotation'].get(name,0)<n}
violations={l:r['outside_by_host'] for l,r in results.items() if r['outside_by_host']}
uplink=[]
for p in sorted(campaign.glob('uplink-*.pcap')):
    q=out/'raw'/p.name;q.parent.mkdir(parents=True,exist_ok=True);fixed_copy(p,q)
    uplink.extend(value for _,value in packets(q,live=tag!='final') if value['epoch']>=boundary)
wire=dict(uplink_packets=len(uplink),nonzero_uplink_packets=sum(p['dscp']!=0 for p in uplink),
          kiwix_uplink_packets=sum(p['dst']=='10.4.20.237' and p['dport']==8090 for p in uplink))
def nft_counts(value):
    return {r['rule']['comment']:next(e['counter']['packets'] for e in r['rule']['expr'] if 'counter' in e)
            for r in value['nftables'] if 'rule' in r and r['rule'].get('comment','').startswith('class:')}
nftpath=campaign/'final-nft.json'
latest=json.loads(nftpath.read_text()) if nftpath.exists() else json.loads(run(['nft','-j','list','table','inet','alloy_harness'],'nft-snapshot',limit=20))
dump(out/'nft.json',latest)
baseline=nft_counts(json.loads((campaign/'baseline-nft.json').read_text()))
delta={k:v-baseline[k] for k,v in nft_counts(latest).items()}
failures=[]
for lane,r in results.items():
    mapping=wire_classes()
    r['nft_codepoint_counts']={str(code):sum(v for k,v in delta.items() if k.startswith('class:'+lane+':'+mapping.get(code,'unmarked' if code==0 else 'codepoint-'+str(code))+':')) for code in range(64)}
    r['nft_class_counts']={name:r['nft_codepoint_counts'][str(code)] for code,name in mapping.items()}
    r['nft_matches_pcap']=all((n==r['dscp']['codepoints'].get(int(code),0) if tag=='final' else n>=r['dscp']['codepoints'].get(int(code),0)) for code,n in r['nft_codepoint_counts'].items())
    if r['dscp']['invalid_packets']:failures.append(lane+': invalid DSCP packet')
    if not r['nft_matches_pcap']:failures.append(lane+': nft/capture count mismatch')
    if r['outside_by_host']:failures.append(lane+': outside contact')
    if r['netlog_unknowns'] or r['browser_service_netlog_requests']:failures.append(lane+': unknown/browser-service NetLog start')
    if r['fatal']:failures.append(lane+': walker fatal')
    if r['startup_canary_denials']!=1 or not any('-138' in s for s in r['startup_canary_results']) or r['canary_proxy_requests']:
        failures.append(lane+': guard canary not proven blocked')
    # The accepted six-lane soak range, with exactly the same consumer definitions.
    for kind,minimum in [('page',76/77),('search',.75),('book',1.)]:
        if not r[kind+'_attempts'] or r[kind+'_success']/r[kind+'_attempts']<minimum:
            failures.append(lane+': '+kind+' success below phase1 range')
    if r['distinct_books']<2:failures.append(lane+': fewer than two books')
    required={'spellcheck','canary','geolocation','notification','eme','speech','webrtc','push','audio','download','pdf','translate-fr','translate-de','kiwix-video','renderer-crash'}
    if not required<=set(r['feature_counts']):failures.append(lane+': trigger attempts missing')
    if tag=='final':
        shutdowns=list((campaign/lane).glob('run-*/browser-shutdown.json'))
        if not shutdowns:failures.append(lane+': missing shutdown receipt')
        for p in shutdowns:
            shutdown=json.loads(p.read_text())
            if not shutdown['normal'] or shutdown['forced']:failures.append(lane+': browser shutdown not normal')
if not uplink or not wire['kiwix_uplink_packets'] or wire['nonzero_uplink_packets']:
    failures.append('physical uplink positive control/DSCP stripping failed')
if tag=='final':
    if partial:failures.append('partial final evidence tail')
    if not (campaign/'restored.json').exists():failures.append('cleanup not proven')
    if not (campaign/'run-complete.json').exists() or min(status['elapsed'].values())<contract['target']:
        failures.append('fixed-duration exposure incomplete')
    if (campaign/'FAILED.json').exists() or (campaign/'cleanup-failed.json').exists():failures.append('collection/cleanup failed')
    for p in [campaign/'uplink.log',campaign/'nflog.log',*[campaign/l/'tcpdump.log' for l in LANES]]:
        text=p.read_text()
        if '0 packets dropped by kernel' not in text:failures.append('capture drop receipt absent: '+str(p))
analysis_seconds=time.monotonic()-start
fixed_scan_seconds=relabel['catalog']['scan_elapsed_seconds']
variable_seconds=max(0,analysis_seconds-fixed_scan_seconds)
projected_target=fixed_scan_seconds+variable_seconds*contract['target']/max(1,min(status['elapsed'].values()))
if projected_target>600:failures.append('projected result cost exceeds 600-second budget')
report=dict(time=datetime.datetime.now(datetime.timezone.utc).isoformat(),tag=tag,campaign=str(campaign),
    elapsed=status['elapsed'],target=status['target'],lanes=results,outside_contacts=violations,
    analysis_seconds=analysis_seconds,budget_seconds=600,
    fixed_scan_seconds=fixed_scan_seconds,variable_seconds=variable_seconds,
    projected_target_seconds=projected_target,
    projection_24h_seconds=fixed_scan_seconds+variable_seconds*86400/max(1,min(status['elapsed'].values())),
    partial_tails=partial,packet_snapshot_details=packet_details,
    wire=wire,failures=failures,pass_=not failures,
    analysis_complete=True,collection_independent=True)
dump(out/'summary.json',report);dump(out/'input-manifest.json',manifest)
md=['# Alloy regression harness — '+tag,'',f'Campaign: `{campaign}`',f'Analysis: {analysis_seconds:.3f}s / 600s budget.',
    f'Exposure: {status["elapsed"]}; fixed target {status["target"]}s.','',
    '## Outside contacts by host','', '| # | Lane | Host | DNS queries | Proxy requests | TLS hellos | Packets | SYNs |','|---|---|---|---|---|---|---|---|']
number=0
for lane,r in results.items():
    for host,v in r['outside_by_host'].items():
        number+=1;md.append(f'| {number} | {lane} | {host} | {v["dns_queries"]} | {v["proxy_requests"]} | {v["tls_hellos"]} | {v["packets"]} | {v["syn"]} |')
if not number:md.append('| 1 | Both | None | 0 | 0 | 0 | 0 | 0 |')
md+=['','Kiwix, namespace-local pages, and example.com controls are retained and labelled separately. Pre-browser seal-probe packets are retained raw but excluded from browser-exposure packet counts.','',
     '## Walker consumers','', '| # | Lane | Pages success/attempts | Searches success/attempts | Book switches success/attempts | Distinct books | Action errors |', '|---|---|---|---|---|---|---|']
for n,(lane,r) in enumerate(results.items(),1):md.append(f'| {n} | {lane} | {r["page_success"]}/{r["page_attempts"]} | {r["search_success"]}/{r["search_attempts"]} | {r["book_success"]}/{r["book_attempts"]} | {r["distinct_books"]} | {r["walker_action_errors"]} |')
md+=['','## Guard denials','', '| # | Lane | Annotation | Entries |','|---|---|---|---|'];number=0
for lane,r in results.items():
    for annotation,n in r['guard_by_annotation'].items():number+=1;md.append(f'| {number} | {lane} | {annotation} | {n} |')
md+=['','## Guard classes','', '| # | Lane | Class | Entries |','|---|---|---|---|'];number=0
for lane,r in results.items():
    for category,n in r['guard_by_class'].items():number+=1;md.append(f'| {number} | {lane} | {category} | {n} |')
md+=['','## Canary and coverage','']
for lane,r in results.items():
    md.append(f'{lane}: startup canary denials {r["startup_canary_denials"]}; completion records {r["startup_canary_results"]}; proxy path receipts {len(r["canary_proxy_requests"])}; unknown annotation hashes {r["netlog_unknowns"]}; unlogged browser-service starts {r["unlogged_browser_service_annotations"]}.')
md+=['','## DSCP cross-check','', '| # | Lane | Classes | Local ICMP/ND | Kernel TCP (parent class) | Unmarked resolver | Unmarked elsewhere | Invalid | NFT matches |', '|---|---|---|---|---|---|---|---|---|']
for n,(lane,r) in enumerate(results.items(),1):
    md.append(f'| {n} | {lane} | {r["dscp"]["classes"]} | {r["dscp"]["local_kernel_control"]} | {r["dscp"]["kernel_tcp_control_by_parent_class"]} | {r["dscp"]["unmarked_resolver"]} | {r["dscp"]["unmarked_elsewhere"]} | {r["dscp"]["invalid_packets"]} | {r["nft_matches_pcap"]} |')
md+=['',f'Physical uplink: {wire}.',f'Failures: {failures}.',f'Complete NetLog events: { {l:r["netlog_events"] for l,r in results.items()} }.',
     f'Partial live tails retained for later re-read: {len(partial)}. No historical NetLog ring overwrite.',
     '', 'Final exposure and cleanup gates apply only to the final result; interim results retain live-tail qualifications.']
(out/'summary.md').write_text('\n'.join(md)+'\n')
files=sorted(p for p in out.rglob('*') if p.is_file() and p.name!='SHA256SUMS')
(out/'SHA256SUMS').write_text(''.join(digest(p)+'  '+str(p.relative_to(out))+'\n' for p in files))
for p in [out,*out.rglob('*')]:
    if not p.is_symlink():os.chown(p,1000,1000)
if not args.out:
    dump(E/('latest-'+tag+'.json'),dict(directory=str(out),summary=str(out/'summary.json'),analysis_seconds=analysis_seconds,outside_contacts=violations,failures=failures,pass_=not failures))
    os.chown(E/('latest-'+tag+'.json'),1000,1000)
print((out/'summary.md').read_text());print('ANALYSIS_RESULT',out,flush=True)
if failures:sys.exit(3)
