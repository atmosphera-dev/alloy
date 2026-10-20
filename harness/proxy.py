# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Raw mitmproxy stream is separate; failed attempts are not reduced to HTTP successes."""
import os, pathlib
from mitmproxy import http, tls
from common import Log
log=Log(pathlib.Path(os.environ['SOAK_LANE'])/'proxy.jsonl')
def request(flow: http.HTTPFlow):
    r=flow.request
    log.event('request',id=flow.id,host=r.pretty_host,method=r.method,url=r.pretty_url,path=r.path,query=list(r.query.items(multi=True)),request_bytes=len(r.raw_content or b''),headers=list(r.headers.items(multi=True)),client=str(flow.client_conn.peername),server=str(flow.server_conn.address))
def response(flow: http.HTTPFlow):
    log.event('response',id=flow.id,host=flow.request.pretty_host,url=flow.request.pretty_url,status=flow.response.status_code,response_bytes=len(flow.response.raw_content or b''),headers=list(flow.response.headers.items(multi=True)))
def error(flow: http.HTTPFlow):
    log.event('error',id=flow.id,host=flow.request.pretty_host if flow.request else None,error=str(flow.error))
def tls_clienthello(data):
    log.event('tls_clienthello',sni=data.client_hello.sni,client=str(data.context.client.peername),server=str(data.context.server.address))
def tls_failed_client(data: tls.TlsData):
    log.event('tls_failed_client',sni=data.conn.sni,error=data.conn.error,classification='TLS rejection; pinning/certificate cause requires evidence, not assumed')
def tls_failed_server(data: tls.TlsData):
    log.event('tls_failed_server',sni=data.conn.sni,error=data.conn.error)
