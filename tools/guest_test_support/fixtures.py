# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Loopback consumer fixtures; observations contain only synthetic test data."""
from contextlib import contextmanager
import array
import http.server
import json
from pathlib import Path
import subprocess
import ssl
import socket
import threading
from urllib.parse import urlsplit

from probe_blocking import TRACKERS


def tracker_https_socket():
    """Bind the original tracker URLs' HTTPS port, without a root browser.

    This assigned-VM helper only binds loopback and transfers the socket. It
    changes no network configuration, services, executables or capabilities.
    """
    channel, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    code = '''import array,socket
control=socket.socket(fileno=0)
listener=socket.socket()
listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
listener.bind(('127.0.0.1',443))
control.sendmsg([b'bound'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[listener.fileno()]))])
'''
    helper = subprocess.Popen(['sudo', '-n', 'python3', '-c', code], stdin=child,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    child.close()
    try:
        channel.settimeout(15)
        message, controls, flags, address = channel.recvmsg(32, socket.CMSG_SPACE(array.array('i').itemsize))
        stdout, stderr = helper.communicate(timeout=15)
        handles = array.array('i')
        for level, kind, value in controls:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                handles.frombytes(value)
        assert helper.returncode == 0 and message == b'bound' and len(handles) == 1, stderr.decode()
        return socket.socket(fileno=handles[0])
    finally:
        channel.close()


class Fixtures:
    def __init__(self, repo: Path, tls_root=None):
        self.repo = repo
        self.certificate = None
        self.scheme = 'https' if tls_root is not None else 'http'
        self.requests = []
        self.protocol_errors = []
        self.lock = threading.Lock()
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def log_error(self, message, *args):
                with outer.lock:
                    outer.protocol_errors.append({
                        'message': message % args,
                        'tls_on_plain_http': self.raw_requestline.startswith(b'\x16\x03'),
                    })

            def do_GET(self):
                with outer.lock:
                    outer.requests.append({'host': self.headers.get('Host'),
                                           'path': self.path})
                path = urlsplit(self.path).path
                content_type = 'text/html; charset=utf-8'
                if path == '/smoke':
                    body = (repo / 'tools/testdata/smoke/index.html').read_bytes()
                elif path.startswith('/title/'):
                    title = path.removeprefix('/title/')
                    body = ('<!doctype html><title>' + title + '</title>').encode()
                elif path == '/set-cookie':
                    body = b'''<!doctype html><script>document.cookie='alloy_session=one; path=/';
document.title=document.cookie.includes('alloy_session=one')?'COOKIE_SET':'COOKIE_REFUSED';</script>'''
                elif path == '/cookie':
                    body = b'''<!doctype html><script>document.title=
document.cookie.includes('alloy_session=one')?'COOKIE_PRESENT':'COOKIE_ABSENT';</script>'''
                elif path == '/query':
                    body = b"<!doctype html><script>document.title='QUERY:'+location.search;</script>"
                elif path == '/neutral.js':
                    content_type = 'application/javascript'
                    body = b'window.neutralExecuted=true;'
                elif path in [urlsplit(url).path for url in TRACKERS.values()]:
                    content_type = 'application/javascript'
                    body = b'window.trackerExecuted=true;'
                elif path == '/blocking':
                    urls = list(TRACKERS.values()) if outer.certificate is not None else [
                        f'{outer.scheme}://{urlsplit(url).hostname}:{outer.port}{urlsplit(url).path}'
                        for url in TRACKERS.values()]
                    body = ('''<!doctype html><title>BLOCKING_PENDING</title><script>
window.neutralExecuted=false;window.trackerExecuted=false;
const load=url=>new Promise(resolve=>{const s=document.createElement('script');
s.src=url;s.onload=()=>resolve('load');s.onerror=()=>resolve('error');document.head.append(s)});
(async()=>{const neutral=await load('/neutral.js');const outcomes=await Promise.all(''' +
                            json.dumps(urls) + '''.map(load));
document.title='BLOCKING:'+JSON.stringify({neutral:neutral==='load'&&neutralExecuted,
outcomes,trackerExecuted:window.trackerExecuted});})();</script>''').encode()
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Type', content_type)
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        if tls_root is not None:
            listener = tracker_https_socket()
            self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 443), Handler,
                                                         bind_and_activate=False)
            self.server.socket.close()
            self.server.socket = listener
            self.server.server_name = 'localhost'
            self.server.server_port = 443
            self.server.server_activate()
        else:
            self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        if tls_root is not None:
            tls_root.mkdir(mode=0o700)
            self.certificate = tls_root / 'certificate.pem'
            key = tls_root / 'fixture-key.pem'
            domains = sorted({urlsplit(url).hostname for url in TRACKERS.values()})
            names = ','.join(['IP:127.0.0.1'] + ['DNS:' + host for host in domains])
            with (tls_root / 'certificate.log').open('x') as log:
                subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-sha256', '-days', '1', '-subj', '/CN=Alloy loopback fixture',
                    '-addext', 'subjectAltName=' + names,
                    '-keyout', str(key), '-out', str(self.certificate)],
                    stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(self.certificate, key)
            self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.port = self.server.server_port
        self.base = f'{self.scheme}://127.0.0.1:{self.port}'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@contextmanager
def tracker_loopback_aliases():
    """Own-VM fixture only: make an unblocked tracker actually load locally.

    This is not a product resolver, external request, legacy-helper invocation
    or a source/verification-gate change. Restore exactly our block on exit.
    """
    domains = sorted({urlsplit(url).hostname for url in TRACKERS.values()})
    marker = '# alloy-gm-m1-loopback-fixture'
    block = '127.0.0.1 ' + ' '.join(domains) + ' ' + marker + '\n'
    script = '''from pathlib import Path
import sys
p=Path('/etc/hosts'); text=p.read_text(); block=sys.argv[2]
if sys.argv[1]=='add':
    assert sys.argv[3] not in text
    p.write_text(text+('' if text.endswith('\\n') else '\\n')+block)
else:
    assert text.count(block)==1
    p.write_text(text.replace(block,''))
'''
    subprocess.run(['sudo', '-n', 'python3', '-c', script, 'add', block, marker], check=True)
    try:
        yield
    finally:
        subprocess.run(['sudo', '-n', 'python3', '-c', script, 'remove', block, marker], check=True)
