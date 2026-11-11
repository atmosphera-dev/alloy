# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Public encrypted-profile/list/delete/lock composition at the browser boundary."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import ssl
import subprocess
import threading
import time
import traceback
from urllib.request import urlopen
from urllib.parse import parse_qs, urlsplit

import profile_system_cache as system_cache
from profile_inventory import observe_tree
from profile_embedded_restart import line, scan_markers
from workspace import PRIMARY, temp_path, validate_output, workspace_path

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        query = parse_qs(parsed.query)
        if parsed.path in ('/cookie-set', '/cookie-echo', '/cookie-redirect'):
            if parsed.path == '/cookie-redirect':
                self.send_response(302)
                self.send_header('Location', query['to'][0])
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            body = json.dumps(dict(cookie=self.headers.get('Cookie', ''))).encode()
            self.send_response(200)
            if parsed.path == '/cookie-set':
                marker = query['marker'][0]
                assert marker.replace('_', '').replace('.', '').isalnum()
                self.send_header('Set-Cookie', 'embedded_http='+marker+
                    '; Path=/; SameSite=None; Secure; HttpOnly')
            self.send_header('Access-Control-Allow-Origin', self.headers.get('Origin', 'null'))
            self.send_header('Access-Control-Allow-Credentials', 'true')
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)
            return
        body = b'''<!doctype html><body><script>
document.body.dataset.embeddedProfileFixture='ready';
onmessage=async event=>{
  if(event.source!==parent||!['alpha.test','beta.test','gamma.test'].some(host=>
    event.origin===location.protocol+'//'+host+':'+location.port))return;
  const {id,action,marker}=event.data||{};
  try{
    if(action==='write'){
      localStorage.setItem('embedded-profile',marker);
      document.cookie='embedded_profile='+marker+'; Path=/; SameSite=None; Secure';
      await fetch('/cookie-set?marker='+encodeURIComponent(marker),{credentials:'include'});
    }else if(action!=='read')throw Error('unknown action');
    const permission=(await navigator.permissions.query({name:'notifications'})).state;
    event.source.postMessage({id,value:{local:localStorage.getItem('embedded-profile'),
      cookie:document.cookie,permission,
      http:(await (await fetch('/cookie-echo',{credentials:'include'})).json()).cookie,
      cross:(await (await fetch(location.origin.replace('beta.test','gamma.test')+'/cookie-echo',
        {credentials:'include'})).json()).cookie,
      redirect:(await (await fetch('/cookie-redirect?to='+encodeURIComponent(
        location.origin.replace('beta.test','gamma.test')+'/cookie-echo'),
        {credentials:'include'})).json()).cookie}},event.origin);
  }catch(error){event.source.postMessage({id,error:String(error)},event.origin)}
};</script></body>'''
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Permissions-Policy', 'geolocation=*')
        self.end_headers()
        self.wfile.write(body)


def hashes(root):
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file() and not p.is_symlink()}


def run(out, evidence, helpers, scopes, *, phases=None):
    validate_output(W, out, REPO)
    assert os.environ['TMPDIR'] == str(temp_path(W))
    evidence.mkdir()
    results = []
    cert, key = evidence/'certificate.pem', evidence/'fixture-key.pem'
    with (evidence/'certificate.log').open('xb') as log:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-sha256', '-days', '1', '-subj', '/CN=alpha.test', '-addext',
            'subjectAltName=DNS:alpha.test,DNS:beta.test,DNS:gamma.test',
            '-keyout', str(key), '-out', str(cert)], stdout=log,
            stderr=subprocess.STDOUT, check=True, timeout=30)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'https://alpha.test:'+str(server.server_port)
    try:
        for scope in scopes:
            case = evidence/scope
            case.mkdir()
            profiles = case/'encrypted'
            profiles.mkdir()
            marker = 'ALLOY_EMBED_PROFILE_'+secrets.token_hex(16)
            for phase in phases or ('write', 'locked', 'wrong', 'read', 'after-delete'):
                root = case/phase
                root.mkdir()
                owned = {name:root/name for name in ('home', 'profile', 'cache', 'config', 'data', 'runtime')}
                for path in owned.values():
                    path.mkdir(mode=0o700)
                xdg_cache = system_cache.create(root)
                xdg_runtime = system_cache.create_runtime(root)
                # R11: seed the isolated OS/NSS input before browser measurement.
                nss = owned['data']/'pki/nssdb'
                nss.mkdir(parents=True)
                with (root/'certificate-input.log').open('xb') as log:
                    subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:'+str(nss)],
                                   stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
                    subprocess.run(['certutil', '-A', '-d', 'sql:'+str(nss), '-n', 'Embedded fixture',
                                    '-t', 'C,,', '-i', str(cert)], stdout=log,
                                   stderr=subprocess.STDOUT, check=True, timeout=20)
                before = observe_tree(root, tuple(owned), hashes=True,
                                      report_path=root/'before-runtime.json')
                profile_before = hashes(profiles) if phase in ('locked', 'wrong') else None
                with socket.socket() as reserve:
                    reserve.bind(('127.0.0.1', 0))
                    port = reserve.getsockname()[1]
                command = [str(out/'alloy_embedded_profile_consumer'), '--alloy-no-disk',
                    '--off-screen-rendering-enabled', '--ozone-platform=x11', '--disable-gpu',
                    '--remote-debugging-port='+str(port),
                    '--host-resolver-rules=MAP *.test 127.0.0.1']
                argv = ['xvfb-run', '-a', *command]
                env = dict(os.environ, TMPDIR=str(temp_path(W)), HOME=str(owned['home']),
                    XDG_CACHE_HOME=str(xdg_cache), XDG_CONFIG_HOME=str(owned['config']),
                    XDG_DATA_HOME=str(owned['data']), XDG_RUNTIME_DIR=str(xdg_runtime),
                    ALLOY_EMBEDDED_PHASE=phase, ALLOY_EMBEDDED_PROFILES=str(profiles),
                    ALLOY_EMBEDDED_BASE=base, ALLOY_EMBEDDED_SCOPE=scope,
                    ALLOY_EMBEDDED_RUNTIME=str(owned['profile']))
                save(root/'argv.json', argv)
                script = root/'driver.mjs'
                script.write_text((REPO/'tools/profile_embedded_profiles.mjs').read_text().replace('__CDP_HELPERS__', helpers))
                subprocess.run(['node', '--check', str(script)], check=True, timeout=20)
                record = dict(scope=scope, phase=phase, argv=argv)
                results.append(record)
                save(evidence/'stages.json', results)
                with (root/'stdio.log').open('xb') as log:
                    process = subprocess.Popen(argv, cwd=out, env=env, stdout=log,
                                               stderr=subprocess.STDOUT, start_new_session=True)
                    driver = None
                    try:
                        if phase not in ('locked', 'wrong'):
                            deadline = time.monotonic()+60
                            while True:
                                assert process.poll() is None, 'browser exited before SDK readiness'
                                try:
                                    with urlopen('http://127.0.0.1:'+str(port)+'/json/version', timeout=1) as response:
                                        if json.load(response).get('webSocketDebuggerUrl'):
                                            break
                                except OSError:
                                    pass
                                assert time.monotonic() < deadline, 'SDK DevTools readiness'
                                time.sleep(.05)
                            with (root/'driver.stderr').open('x') as errors:
                                driver = subprocess.Popen(['node', str(script),
                                str(port), base, phase, str(root/'stdio.log'), marker],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, text=True)
                                live = line(driver, 150)
                                save(root/'live-browser-proof.json', live)
                                assert live['phase'] == 'live'
                                live_paths = observe_tree(root, tuple(owned), hashes=True,
                                    report_path=root/'live-runtime.json')
                                assert live_paths == before, 'live browser runtime roots changed'
                                save(root/'live-marker-scan.json', dict(
                                    runtime=scan_markers(root, marker, roots=tuple(owned)),
                                    encrypted=scan_markers(profiles, marker,
                                        roots=('profile-0', 'profile-1'), allow_vanished=True)))
                                system_cache.check(xdg_cache)
                                system_cache.check_runtime(xdg_runtime)
                                driver.stdin.write('continue\n')
                                driver.stdin.flush()
                                stdout, _ = driver.communicate(timeout=150)
                            (root/'driver.stdout').write_text(stdout)
                            record['driver_exit'] = driver.returncode
                            save(evidence/'stages.json', results)
                            assert driver.returncode == 0, (root/'driver.stderr').read_text()
                            save(root/'browser-proof.json', json.loads(stdout))
                        process.wait(timeout=60)
                        record['exit'] = process.returncode
                        save(evidence/'stages.json', results)
                        assert process.returncode == 0
                    finally:
                        if driver is not None and driver.poll() is None:
                            driver.terminate()
                            driver.wait(timeout=15)
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGTERM)
                            process.wait(timeout=15)
                assert 'PASS embedded-profile-consumer' in (root/'stdio.log').read_text()
                system_cache.check(xdg_cache)
                system_cache.check_runtime(xdg_runtime)
                after = observe_tree(root, tuple(owned), hashes=True,
                                     report_path=root/'after-runtime.json')
                save(root/'runtime-boundary.json', dict(before=before, after=after))
                assert before == after, 'browser wrote outside encrypted profiles/system font cache'
                if profile_before is not None:
                    assert hashes(profiles) == profile_before, 'refused credentials changed encrypted state'
                for path in profiles.rglob('*'):
                    if path.is_file():
                        data = path.read_bytes().lower()
                        assert all(value not in data for value in
                                   (b'alpha.test', b'beta.test', b'gamma.test', marker.lower().encode())), str(path)
                record['verified'] = True
                save(evidence/'stages.json', results)
                print('PASS embedded-profile', scope, phase, flush=True)
        save(evidence/'result.json', dict(pass_=True, results=results,
            encrypted_profile_verified=phases is None, actual_public_list_and_record_binding=True,
            delete_and_explicit_lock_verified=phases is None, decay_verified=False, autolock_verified=False))
        return results
    except BaseException:
        (evidence/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        server.shutdown()
        server.server_close()
        paths = [p for p in sorted(evidence.rglob('*')) if p.is_file()
                 and not p.is_symlink() and p.name != 'SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in paths))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--scope', choices=('both', 'per-tab', 'per-outer-site'), default='both')
    parser.add_argument('--phase', choices=('write',))
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
            'flock', '--wait', '1800', str(PRIMARY/'controller.lock'), 'unshare', '--net', '--fork',
            '/bin/sh', '-c', 'ip link set lo up && exec runuser -u builder -- "$@"', 'embedded-profiles',
            'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1',
            'python3', str(Path(__file__).resolve()), '--out', str(out), '--scope', args.scope, '--inside']
        if args.phase:
            command += ['--phase', args.phase]
        return subprocess.run(['timeout', '--kill-after=30', '1200', *command], timeout=1240).returncode
    assert os.geteuid() == 1000
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    assert [i['ifname'] for i in interfaces] == ['lo']
    assert not subprocess.check_output(['ip', 'route'], text=True, timeout=10).strip()
    from profile_probe import CDP_HELPERS
    scopes = ('per-tab', 'per-outer-site') if args.scope == 'both' else (args.scope,)
    evidence = W/'evidence'/('profile-embedded-profiles-'+out.name+'-'+secrets.token_hex(8))
    print('EMBEDDED_PROFILES_EVIDENCE', evidence, flush=True)
    run(out, evidence, CDP_HELPERS, scopes, phases=(args.phase,) if args.phase else None)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
