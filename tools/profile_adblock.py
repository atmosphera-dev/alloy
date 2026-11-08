# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Dedicated NUC SDK consumer: encrypted AdBlock lifecycle, cold read and peers."""
import argparse
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
from urllib.parse import parse_qs, urlsplit
from workspace import temp_path, validate_output, workspace_path
import profile_system_cache as system_cache

REPO=Path(__file__).resolve().parents[1]
W=workspace_path(REPO)

def hashes(root):
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file() and not p.is_symlink()}

class Handler(http.server.BaseHTTPRequestHandler):
    peer_requests=0
    counter_lock=threading.Lock()

    def log_message(self,*args):
        pass

    def do_GET(self):
        url=urlsplit(self.path)
        if url.path in ('/adblock-private-resource-133.js','/adblock-beta-private-resource-140.js','/adblock-public-control.js'):
            if url.path=='/adblock-private-resource-133.js' and parse_qs(url.query).get('owner')==['1']:
                with self.counter_lock:
                    type(self).peer_requests+=1
            body=b'/* fixture resource; positive native fetch control */'
            kind='application/javascript'
        else:
            body=b'<!doctype html><title>AdBlock fixture</title><body>native context probe</body>'
            kind='text/html'
        self.send_response(200)
        self.send_header('Content-Type',kind)
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.end_headers()
        self.wfile.write(body)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--phase',choices=('modes','write','read','locked','wrong','isolation'),help='Run only this case; no full-chain success is implied')
    parser.add_argument('--expect-isolation-red',action='store_true',help='Accept only the original native alpha-to-beta filter leak with all preceding controls passed')
    parser.add_argument('--profile-root',type=Path,help='Explicit shared owned profile fixture for separately executed cold/lifecycle cases')
    args=parser.parse_args()
    assert socket.gethostname()=='wakaba-build' and os.geteuid()==1000
    assert os.environ['TMPDIR']==str(temp_path(W))
    out=args.out.resolve();validate_output(W,out,REPO)
    evidence=args.evidence.resolve()
    assert evidence.is_relative_to(W/'evidence') and not evidence.exists()
    interfaces=json.loads(subprocess.check_output(['ip','-j','addr'],text=True,timeout=10))
    routes=subprocess.check_output(['ip','route'],text=True,timeout=10)
    assert [v['ifname'] for v in interfaces]==['lo'] and not routes.strip()
    evidence.mkdir();results=[]
    (evidence/'isolation.json').write_text(json.dumps(dict(interfaces=interfaces,routes=routes,netns=os.readlink('/proc/self/ns/net')),indent=2)+'\n')
    profiles=args.profile_root.resolve() if args.profile_root else evidence
    valid_profile_root=profiles.is_relative_to(W/'evidence')
    print('ADBLOCK_SETUP_CHECK '+json.dumps(dict(operation='profile-fixture-boundary',passed=valid_profile_root,path=str(profiles))),flush=True)
    assert valid_profile_root
    profiles.mkdir(exist_ok=True)
    alpha=profiles/'alpha-profile';beta=profiles/'beta-profile'
    (evidence/'profile-fixture.json').write_text(json.dumps(dict(root=str(profiles),alpha=str(alpha),beta=str(beta),requested_phase=args.phase),indent=2)+'\n')
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    fixture='http://adblock-fixture.test:'+str(server.server_port)
    try:
        requested=(args.phase,) if args.phase else ('modes','write','read','locked','wrong')
        for phase in requested:
            root=evidence/phase;root.mkdir()
            checks=[]
            def check(operation,condition,**observed):
                row=dict(case=phase,operation=operation,passed=bool(condition),observed=observed)
                checks.append(row)
                (root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
                print('ADBLOCK_CHECK '+json.dumps(row,sort_keys=True),flush=True)
                assert condition,row
            owned={n:root/n for n in ('home','cache','config','data','runtime')}
            for p in owned.values():p.mkdir(mode=0o700)
            xdg_cache=system_cache.create(root)
            argv=['xvfb-run','-a','-s','-screen 0 1280x720x24',str(out/'alloy_encrypted_adblock_consumer'),
                  '--alloy-no-disk','--off-screen-rendering-enabled','--ozone-platform=x11','--disable-gpu',
                  '--no-proxy-server','--host-resolver-rules=MAP adblock-fixture.test 127.0.0.1']
            env=dict(os.environ,HOME=str(owned['home']),XDG_CACHE_HOME=str(xdg_cache),XDG_CONFIG_HOME=str(owned['config']),
                     XDG_DATA_HOME=str(owned['data']),XDG_RUNTIME_DIR=str(owned['runtime']),
                     ALLOY_ADBLOCK_PHASE=phase,ALLOY_ADBLOCK_ALPHA=str(alpha),ALLOY_ADBLOCK_BETA=str(beta),ALLOY_ADBLOCK_FIXTURE=fixture)
            before=hashes(alpha) if phase in ('locked','wrong') else None
            peer_before=Handler.peer_requests
            (root/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
            with (root/'stdio.log').open('x') as log:
                process=subprocess.Popen(argv,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                timed_out=False
                try:
                    code=process.wait(timeout=150)
                except subprocess.TimeoutExpired:
                    timed_out=True
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid,signal.SIGTERM)
                        try:process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
            code=process.returncode
            system_cache.check(xdg_cache)
            (root/'exit.json').write_text(json.dumps(dict(exit=code,timed_out=timed_out,timeout_seconds=150),indent=2)+'\n')
            check('consumer-time-bound',not timed_out,exit=code,timed_out=timed_out,timeout_seconds=150)
            output=(root/'stdio.log').read_text()
            events=[json.loads(l[len('ADBLOCK_EVENT '):]) for l in output.splitlines() if l.startswith('ADBLOCK_EVENT ')]
            if args.expect_isolation_red:
                check('red-is-only-isolation-case',phase=='isolation' and args.phase=='isolation')
                failed=[l for l in output.splitlines() if l.startswith('ADBLOCK_CHECK ok=0')]
                check('red-native-positive-controls',{'event':'isolation-positive','owner':0} in events,events=events)
                check('red-actual-original-filter-leak','ADBLOCK_ISOLATION beta_empty=0 beta_equals_alpha=1' in output)
                check('red-exact-isolation-assertion',failed==['ADBLOCK_CHECK ok=0 operation=encrypted-filter-isolation alpha-not-visible-to-beta'],failed_checks=failed)
                check('red-aborted-at-intended-assertion',code in (-signal.SIGABRT,128+signal.SIGABRT),exit=code)
                result=dict(phase=phase,exit=code,expected_negative_verified=True,positive_controls_verified=True,failed_operation='encrypted-filter-isolation alpha-not-visible-to-beta',events=events)
                (evidence/'result.json').write_text(json.dumps(dict(config=out.name,results=[result],original_encrypted_isolation_red=True,standalone_prerequisite_gate=False),indent=2)+'\n')
                print('PASS expected original encrypted-filter-isolation red',flush=True)
                return
            check('consumer-exit',code==0,exit=code)
            check('consumer-completion-marker','PASS encrypted-adblock-browser-consumer' in output)
            if phase=='isolation':
                check('encrypted-native-isolation',events==[{'event':'isolation-positive','owner':0},{'event':'lock-pending-writer-refused'},{'event':'encrypted-filter-isolation'}],events=events)
                check('pending-lock-writer-explicit-refusal','ADBLOCK_PENDING_WRITE accepted=0 state=' in output)
                callbacks=[json.loads(l[len('ADBLOCK_LOCK_CALLBACK '):]) for l in output.splitlines() if l.startswith('ADBLOCK_LOCK_CALLBACK ')]
                check('all-isolation-lock-completions-ok',[r['owner'] for r in callbacks]==[0,0,1] and all(r['status']==0 and r['profile_state']==0 for r in callbacks),callbacks=callbacks)
                check('accepted-value-cold-reload-without-partial-refused-write','ADBLOCK_CHECK ok=1 operation=isolation accepted rule persisted without refused replacement' in output)
            elif phase=='modes':
                check('memory-isolation-and-stateless-refusal',events==[{'event':'memory-isolation-and-stateless-refusal'}],events=events)
            elif phase in ('locked','wrong'):
                check('locked-or-wrong-context-refused',events==[{'event':'refused'}],events=events)
                after=hashes(alpha)
                check('refused-credentials-preserved-profile-bytes',after==before,before=before,after=after)
            else:
                check('both-original-filter-readbacks',{e['owner'] for e in events if e['event']=='filters'}=={0,1},events=events)
                check('both-original-network-outcomes',{e['owner'] for e in events if e['event']=='network' and not e['after_alpha_lock']}=={0,1},events=events)
                check('peer-network-after-alpha-lock',any(e['event']=='network' and e['owner']==1 and e['after_alpha_lock'] for e in events),events=events)
                check('native-lock-completion',any(e['event']=='locked' for e in events),events=events)
                check('peer-server-observed-before-and-after-lock',Handler.peer_requests-peer_before>=2,peer_requests=Handler.peer_requests-peer_before)
            # Profile/XDG scans include the filter's compiled token. Native
            # stdio may report the fixture request URL; scan it for the complete
            # private configuration, rather than confusing that URL with a rule.
            scans=[]
            for path in (alpha,beta,*owned.values(),root/'stdio.log'):
                if not path.exists():continue
                markers=['adblock-private-resource-133.js','adblock-beta-private-resource-140.js','adblock-refused-pending-resource-158.js']
                if path==root/'stdio.log':markers=[v+'$important' for v in markers]
                scan=subprocess.run(['rg','-a','-F','-l',*[a for marker in markers for a in ('-e',marker)],str(path)],capture_output=True,text=True,timeout=30)
                scans.append(dict(path=str(path),exit=scan.returncode,stdout=scan.stdout,stderr=scan.stderr))
                (root/'scan.json').write_text(json.dumps(scans,indent=2)+'\n')
                check('private-marker-scan',scan.returncode==1,path=str(path),exit=scan.returncode,stdout=scan.stdout,stderr=scan.stderr)
            ordinary_dat=[str(p) for path in (alpha,beta,*owned.values()) for p in path.rglob('*.dat') if p.is_file()]
            check('ordinary-dat-absence',not ordinary_dat,ordinary_dat_files=ordinary_dat)
            partial_files=[str(p) for path in (alpha,beta,*owned.values()) for p in path.rglob('.adblock-stage-*') if p.is_file()]
            check('ordinary-partial-filter-file-absence',not partial_files,partial_files=partial_files)
            results.append(dict(phase=phase,exit=code,events=events,peer_requests=Handler.peer_requests-peer_before,marker_matches=0,ordinary_dat_files=0))
            (root/'scan.json').write_text(json.dumps(scans,indent=2)+'\n')
            (evidence/'stages.json').write_text(json.dumps(results,indent=2)+'\n')
            print('PASS encrypted-adblock-browser-'+phase,flush=True)
        verified={r['phase'] for r in results}
        (evidence/'result.json').write_text(json.dumps(dict(config=out.name,results=results,focused_phase=args.phase,verified_phases=sorted(verified),native_api_and_request_consumer={'write','read'}.issubset(verified),
            encrypted_filter_isolation='isolation' in verified,
            memory_isolation_and_stateless_refusal='modes' in verified,osr_alloy_style_only=bool({'write','read'}&verified),
            cold_encrypted_profile_read='read' in verified,original_lock_and_peer_continuity=bool({'write','read'}&verified),locked_and_wrong_password_refused={'locked','wrong'}.issubset(verified),
            marker_scan_absent=True,combined_tree_only=True,original_redgreen_complete=False,standalone_prerequisite_gate=False),indent=2)+'\n')
    except BaseException as error:
        (evidence/'failure.json').write_text(json.dumps(dict(case=phase,error=str(error),traceback=traceback.format_exc(),completed=results),indent=2)+'\n')
        (evidence/'failure.txt').write_text(traceback.format_exc());raise
    finally:
        server.shutdown();server.server_close()
        files=[p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink() and p.name!='SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(evidence))+'\n' for p in files))

if __name__=='__main__':
    main()
