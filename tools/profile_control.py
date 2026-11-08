# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Identified native fixture commands; acknowledgement is not async completion."""
import json
import re
import uuid

PREFIX = 'ALLOY_PROFILE_PROBE:'

def page_control_script(action):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,256}', action):
        raise ValueError('invalid profile control action')
    request_id = uuid.uuid4().hex
    request = dict(id=request_id, action=action,
                   title=PREFIX+request_id+':'+action)
    slot = 'globalThis[Symbol.for("alloy.test.profile-control.request")]'
    return slot+'='+json.dumps(request)+';document.title='+slot+'.title;'

CDP_CONTROL = r'''
function profileControlRequest(action,id=crypto.randomUUID().replaceAll('-','')){
  check(/^[a-f0-9]{32}$/.test(id),'invalid native control request identity');
  check(/^[A-Za-z0-9_-]{1,256}$/.test(action),'invalid native control action');
  return {id,action,title:'ALLOY_PROFILE_PROBE:'+id+':'+action};
}
async function profileControlReply(request){
  const reply=await until(async()=>{
    const events=await nativeEvents();
    return events.find(e=>e.event==='control-reply'&&e.request_id===request.id&&
      e.token===request.action&&e.phase==='dispatched');
  },'native control dispatch reply '+request.action+' '+request.id);
  check(reply.executions===1&&reply.browser>0,'invalid native control reply '+JSON.stringify(reply));
  return reply;
}
async function control(c,action){
  const request=profileControlRequest(action);
  await value(c,`document.title=${JSON.stringify(request.title)}`);
  return profileControlReply(request);
}
async function pageControl(c){
  const request=await value(c,'globalThis[Symbol.for("alloy.test.profile-control.request")]');
  check(request&&request.title===profileControlRequest(request.action,request.id).title,
    'page native control request missing');
  return profileControlReply(request);
}
'''
