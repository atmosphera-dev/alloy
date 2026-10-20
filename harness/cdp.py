# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Adapted from retained spike scripts/05-suite.py CDP (lines 18–57).

Same synchronous websocket-client request/event dispatch; append-only timestamped
segments, bounded pending replies and page request provenance added for soak.
"""
import json, time, websocket
from common import Log
class CDP:
    def __init__(self,url,path):
        self.ws=websocket.create_connection(url,timeout=1,suppress_origin=True)
        self.log=Log(path); self.seq=0; self.events=[]; self.replies={}; self.requests={}
        self.tick=lambda:None
        self.failures=[]
    def receive(self):
        r=json.loads(self.ws.recv()); self.log.event('received',message=r)
        if 'method' in r:
            self.events.append(r)
            if len(self.events)>5000: self.events=self.events[-2500:]
            p=r.get('params',{}); rid=p.get('requestId')
            if r['method']=='Network.requestWillBeSent':
                self.requests[rid]=dict(url=p['request']['url'],initiator=p.get('initiator'),documentURL=p.get('documentURL'),frameId=p.get('frameId'),wallTime=p.get('wallTime'),type=p.get('type'))
                self.log.event('page_request',requestId=rid,**self.requests[rid])
            if r['method']=='Network.loadingFailed':
                failure=dict(requestId=rid,request=self.requests.get(rid),failure=p)
                self.log.event('page_failed',**failure);self.failures.append(failure)
                if len(self.failures)>2000:self.failures=self.failures[-1000:]
            if len(self.requests)>10000: self.requests=dict(list(self.requests.items())[-5000:])
        elif 'id' in r: self.replies[r['id']]=r
        return r
    def call(self,method,params=None,limit=15):
        self.seq+=1; ident=self.seq; request=dict(id=ident,method=method,params=params or {})
        self.log.event('sent',message=request); self.ws.send(json.dumps(request))
        end=time.monotonic()+limit
        while time.monotonic()<end:
            self.tick()
            if ident in self.replies: return self.replies.pop(ident)
            try: self.receive()
            except websocket.WebSocketTimeoutException: continue
        raise TimeoutError(method)
    def pump(self,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            self.tick()
            try: self.receive()
            except websocket.WebSocketTimeoutException: pass
    def evaluate(self,expression,limit=20):
        r=self.call('Runtime.evaluate',dict(expression=expression,awaitPromise=True,returnByValue=True,userGesture=True),limit)
        if 'error' in r or 'exceptionDetails' in r.get('result',{}): raise RuntimeError(r)
        return r.get('result',{}).get('result',{}).get('value')
    def close(self): self.ws.close(); self.log.close()
