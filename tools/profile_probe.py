#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Approved-workspace native storage, lifetime and TLS/network probe."""
import argparse
from datetime import datetime, timezone
import errno
import hashlib
import http.server
import itertools
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback
from urllib.parse import parse_qs, urlsplit
from urllib.request import urlopen

sys.dont_write_bytecode = True
from workspace import PRIMARY, temp_path, validate_output, workspace_path
import profile_system_cache as system_cache
from profile_control import CDP_CONTROL, page_control_script
from profile_inventory import observe_tree

REPO = Path(__file__).resolve().parents[1]
W = workspace_path(REPO)

NOISE_FUNCTION = r'''function(canvas){
  canvas.width=32;canvas.height=32;
  const c=canvas.getContext('2d',{willReadFrequently:true});
  const g=c.createLinearGradient(0,0,32,32);
  g.addColorStop(0,'#123456');g.addColorStop(1,'#fedcba');
  c.fillStyle=g;c.fillRect(0,0,32,32);c.fillStyle='#a13c67';c.fillRect(4,7,19,13);
  const bytes=c.getImageData(0,0,32,32).data;
  let hash=2166136261;for(const b of bytes)hash=Math.imul(hash^b,16777619)>>>0;
  return hash.toString(16);
}'''

CDP_HELPERS = r'''
async function sleep(ms){
  const deadline=performance.now()+ms;
  for(let remaining=deadline-performance.now();remaining>0;remaining=deadline-performance.now())
    await new Promise(resolve=>setTimeout(resolve,Math.ceil(remaining)));
}
const check=(ok,why)=>{if(!ok)throw Error(why)};
async function until(fn,why){let last='';for(let n=0;n<200;n++){try{const v=await fn();if(v)return v}catch(e){if(e.cdpTerminal)throw e;last=String(e)}await sleep(100)}throw Error(why+' '+last)}
async function pageTargets(){return(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).filter(t=>t.type==='page')}
function connect(target){const commandTimeout=120000;return new Promise((resolve,reject)=>{
  const ws=new WebSocket(target.webSocketDebuggerUrl),wait=new Map();let id=0,opened=false,terminal=null;
  function fail(why){
    terminal??=Object.assign(Error('CDP '+target.id+' '+why),{cdpTerminal:true});
    for(const pending of wait.values()){clearTimeout(pending.timer);pending.reject(terminal)}
    wait.clear();if(!opened)reject(terminal);
  }
  ws.onerror=e=>fail('socket error '+(e.message??''));
  ws.onclose=e=>fail('socket closed '+e.code+' '+e.reason);
  ws.onmessage=e=>{
    let m;try{m=JSON.parse(e.data)}catch(error){fail('invalid protocol response');return}
    if(m.id){const pending=wait.get(m.id);if(pending){wait.delete(m.id);clearTimeout(pending.timer);m.error?pending.reject(Error(JSON.stringify(m.error))):pending.resolve(m.result)}}
  };
  ws.onopen=()=>{opened=true;resolve({ws,send(method,params={}){return new Promise((resolve,reject)=>{
    if(terminal){reject(terminal);return}
    if(ws.readyState!==WebSocket.OPEN){fail('socket not open');reject(terminal);return}
    const k=++id,timer=setTimeout(()=>fail('command deadline '+method+' id='+k),commandTimeout);
    wait.set(k,{resolve,reject,timer});
    try{ws.send(JSON.stringify({id:k,method,params}))}catch(error){fail('send failed '+method+' '+error)}
  })}})};
})}
async function value(c,expression){const r=await c.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});check(!r.exceptionDetails,JSON.stringify(r.exceptionDetails));return r.result.value}
async function committedHistory(c,url=null){return until(async()=>{const h=await c.send('Page.getNavigationHistory');const current=h.entries[h.currentIndex]?.url;return current&&(!url||current===url)?h:null},'native navigation entry commit')}
async function nativeEvents(){return(await readFile(eventLog,'utf8')).split('\n').filter(l=>l.startsWith('PROFILE_EVENT ')).map(l=>JSON.parse(l.slice(14)))}
''' + CDP_CONTROL

DATALIST_HELPERS = r'''
async function popupCount(){try{return JSON.parse(await readFile(popupReceipt,'utf8')).completed.length}catch(e){if(e.code==='ENOENT')return 0;throw e}}
async function datalistOutcome(c){
  const before=await popupCount();
  const point=await value(c,`(()=>{
    const input=document.createElement('input');input.id='live-datalist';
    input.name='live-datalist';input.setAttribute('list','live-choices');
    const list=document.createElement('datalist');list.id='live-choices';
    const option=document.createElement('option');option.value='Page-owned choice';
    list.append(option);document.body.prepend(input,list);
    window.datalistKeys=[];for(const type of ['keydown','keyup'])input.addEventListener(type,e=>datalistKeys.push({type:e.type,key:e.key,trusted:e.isTrusted}));
    input.scrollIntoView();const r=input.getBoundingClientRect();
    return {x:r.x+r.width/2,y:r.y+r.height/2};})()`);
  await c.send('Page.bringToFront');
  await c.send('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount:1,...point});
  await c.send('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount:1,...point});
  await until(async()=>await value(c,"document.activeElement?.id==='live-datalist'"),'datalist field focus');
  await c.send('Input.dispatchKeyEvent',{type:'keyDown',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  await c.send('Input.dispatchKeyEvent',{type:'keyUp',key:'ArrowDown',code:'ArrowDown',windowsVirtualKeyCode:40});
  // Observe native completion of the popup attempt. OSR has no native Views
  // host; parity must not pretend a popup exists or substitute a DOM fill.
  await until(async()=>await popupCount()>before,'native datalist popup attempt completion');
  await c.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
  await c.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
  const outcome=await until(async()=>{
    const result=await value(c,`({value:document.querySelector('#live-datalist').value,
      focused:document.activeElement?.id==='live-datalist',
      options:Array.from(document.querySelector('#live-choices').options,o=>o.value),
      keys:datalistKeys,alive:document.readyState==='complete'})`);
    return result.value!==''||result.keys.some(e=>e.type==='keyup'&&e.key==='Enter'&&e.trusted)?result:null;
  },'native datalist input sequence completion');
  check(outcome.alive&&outcome.focused&&outcome.options[0]==='Page-owned choice','datalist page lost its live input');
  outcome.nativePopupAttemptCompleted=true;
  await value(c,"document.querySelector('#live-datalist').remove();document.querySelector('#live-choices').remove();scrollTo(0,0)");
  return outcome;
}
'''

DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,base,mode,secret,baseline,eventLog,exerciseFiles,certificateBase,popupReceipt,gestureReceipt,browserPID,actionModule]=process.argv.slice(1);
const {createNativeActionSequencer}=await import(actionModule);
const filesEnabled=exerciseFiles==='1';
''' + CDP_HELPERS + DATALIST_HELPERS + r'''
const nativeActions=[],gestureOwners=new WeakMap();
async function gestureSnapshot(){
  const receipt=JSON.parse(await readFile(gestureReceipt,'utf8'));
  check(Number.isInteger(receipt.serial)&&receipt.serial>=0,'invalid native gesture receipt');
  return receipt;
}
const userAction=createNativeActionSequencer({
  prime:async target=>{
    const before=await gestureSnapshot();
    const result=await target.send('Runtime.evaluate',{userGesture:true,awaitPromise:true,returnByValue:true,
      expression:'({url:location.href,nonce:window.docNonce})'});
    check(!result.exceptionDetails,'native gesture priming '+JSON.stringify(result));
    return {before:before.serial,document:result.result.value};
  },
  acknowledge:async(target,ticket)=>{
    const receipt=await until(async()=>{const value=await gestureSnapshot();return value.serial>ticket.before?value:null},
      'browser-native gesture acknowledgement');
    check(receipt.serial===ticket.before+1&&receipt.pid===Number(browserPID)&&receipt.active===true,
      'native gesture identity '+JSON.stringify(receipt));
    const previous=gestureOwners.get(target);
    check(previous===undefined||previous===receipt.node,'native gesture reached a different tab');
    gestureOwners.set(target,receipt.node);ticket.native=receipt;
  },
  dispatch:async(target,options,ticket)=>{
    check(!('userGesture' in options),'action must not inject another racing gesture');
    const document=await value(target,'({url:location.href,nonce:window.docNonce})');
    check(JSON.stringify(document)===JSON.stringify(ticket.document),'gesture document retired before action');
    const result=await target.send('Runtime.evaluate',options);
    nativeActions.push({serial:ticket.native.serial,node:ticket.native.node,document});
    return result;
  },
});
const parentTarget=await until(async()=>{const t=await pageTargets();return t.length===1&&t[0].url==='about:blank'?t[0]:null},'ready parent browser');
async function targets(){return(await pageTargets()).filter(t=>t.id!==parentTarget.id)}
const parent=await connect(parentTarget);await parent.send('Page.enable');await committedHistory(parent);
await control(parent,'start-stateless');
await until(async()=>{const t=await targets();const events=await nativeEvents();return t.length===2&&events.filter(e=>e.event==='created').length===2},'two native stateless browsers created');
parent.ws.close();
const list=await until(async()=>{const t=await targets();return t.length===2&&t.every(v=>v.url==='about:blank')?t:null},'two native blank tabs');
const main=await connect(list[0]),keeper=await connect(list[1]);
await main.send('Page.enable');await keeper.send('Page.enable');
await Promise.all([committedHistory(main),committedHistory(keeper)]);
const start=performance.now(),proof={mode,checks:[],loads:[],parentTarget:parentTarget.id};
function pass(name){proof.checks.push(name)}
async function load(label,origin=base){const url=origin+'/page/'+label;const r=await main.send('Page.navigate',{url});check(!r.errorText,'navigation '+JSON.stringify(r));
  const state=await until(async()=>{const v=await value(main,'({ready:document.readyState,url:location.href,nonce:window.docNonce,title:document.title})');return v.ready==='complete'&&v.url===url&&v.nonce?v:null},'load '+label);
  await committedHistory(main,url);await pageControl(main);proof.loads.push(state);return state}
async function echo(){return value(main,"fetch('/echo').then(r=>r.json())")}
async function permission(c=main){return value(c,"navigator.permissions.query({name:'geolocation'}).then(p=>p.state)")}
async function workerPermission(c=main){return value(c,`new Promise((resolve,reject)=>{
  const w=new Worker('/permission-worker.js');w.onmessage=e=>{w.terminate();resolve(e.data)};
  w.onerror=e=>{w.terminate();reject(Error(e.message))};w.postMessage('query');})`)}
async function locate(){return value(main,`new Promise((resolve,reject)=>navigator.geolocation.getCurrentPosition(
  p=>resolve({latitude:p.coords.latitude,longitude:p.coords.longitude}),e=>reject(Error(e.code+':'+e.message)),{timeout:5000}))`)}
async function noise(){return value(main,`(async()=>{
  const draw=__NOISE_FUNCTION__;
  const first=draw(document.createElement('canvas'));
  const repeat=draw(document.createElement('canvas'));
  const worker=await new Promise((resolve,reject)=>{const w=new Worker('/worker.js');
    w.onmessage=e=>{w.terminate();resolve(e.data)};w.onerror=e=>{w.terminate();reject(Error(e.message))};w.postMessage('draw')});
  return {first,repeat,worker};})()`)}
async function mediaIdentity(){return value(main,`(async()=>{
  const stream=await navigator.mediaDevices.getUserMedia({video:true});
  try{
    const track=stream.getVideoTracks()[0],settings=track.getSettings();
    const devices=await navigator.mediaDevices.enumerateDevices();
    return {id:settings.deviceId,group:settings.groupId,label:track.label,state:track.readyState,
      devices:devices.filter(d=>d.kind==='videoinput').map(d=>({id:d.deviceId,group:d.groupId,label:d.label}))};
  }finally{stream.getTracks().forEach(track=>track.stop())}
})()`)}
async function history(){return main.send('Page.getNavigationHistory')}
async function go(entry){const old=await value(main,'window.docNonce');await main.send('Page.navigateToHistoryEntry',{entryId:entry.id});
  return until(async()=>{const v=await value(main,'({ready:document.readyState,url:location.href,nonce:window.docNonce,title:document.title,field:document.querySelector("#saved")?.value,scroll:scrollY,state:history.state})');return v.ready==='complete'&&v.url===entry.url&&v.nonce&&v.nonce!==old?v:null},'fresh traversal '+entry.url)}
if(Number(baseline)>=0){await sleep(Number(baseline)*1000);pass('no-page-baseline')}
else if(certificateBase){
  await load('certificate-live');
  const live=(await nativeEvents()).filter(e=>e.event==='certificate');
  check(live.length>0&&live.every(e=>e.held===0),'real certificate callback was bypassed');
  check((await echo()).connection,'accepted native certificate did not reach HTTP consumer');
  await control(main,'hold-certificate');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='certificate-hold-ready'),'certificate hold control');
  const pendingNavigation=main.send('Page.navigate',{url:certificateBase+'/page/certificate-held'})
    .then(result=>({result}),error=>({protocolError:String(error)}));
  const held=await until(async()=>(await nativeEvents()).find(e=>e.event==='certificate'&&e.held),'held native certificate callback');
  await load('certificate-replacement');
  await control(main,'release-held-certificate');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='certificate-released'),'late certificate callback release');
  check(await value(main,'location.href')===base+'/page/certificate-replacement','late certificate callback retargeted the current document');
  const previous=(await nativeEvents()).filter(e=>e.event==='certificate').length;
  await load('certificate-fresh',certificateBase);
  const decisions=(await nativeEvents()).filter(e=>e.event==='certificate');
  check(decisions.length>previous&&decisions.at(-1).held===0,'retired certificate decision authorized a fresh navigation');
  check((await echo()).connection,'fresh certificate decision did not reach consumer');
  proof.certificates={live,held,decisions,interrupted:await pendingNavigation};pass('native-certificate-callback-and-retirement');
}
else {
  const a=await load('a');
  check(await value(main,"typeof showOpenFilePicker==='function'")===filesEnabled,
    'FileSystemAccessAPI exposure differs from explicit probe configuration');
  if(!filesEnabled)pass('default-file-api-disabled');
  proof.datalist={stateless:await datalistOutcome(main)};
  const storage=await value(main,`(async()=>{
    const out={};const marker=${JSON.stringify(secret)};
    document.cookie='alloy_probe='+marker+'; SameSite=Lax';out.cookie=document.cookie;
    for(const name of ['localStorage','sessionStorage']){try{window[name].setItem('alloy_probe',marker);out[name]=window[name].getItem('alloy_probe')}catch(e){out[name]=null}}
    try{out.idb=await new Promise((resolve,reject)=>{const r=indexedDB.open('alloy_probe');r.onupgradeneeded=()=>r.result.createObjectStore('s');r.onsuccess=()=>{r.result.close();resolve('opened')};r.onerror=()=>reject(r.error)})}catch(e){out.idb=null}
    try{const c=await caches.open('alloy_probe');await c.put('/retained',new Response(marker));out.cache='stored'}catch(e){out.cache=null}
    try{await navigator.storage.getDirectory();out.opfs='opened'}catch(e){out.opfs=null}
    try{await navigator.serviceWorker.register('/sw.js');out.worker='registered'}catch(e){out.worker=null}
    return out})()`);
  check(storage.cookie==='','cookie admitted '+JSON.stringify(storage));
  for(const key of ['localStorage','sessionStorage','idb','cache','opfs','worker'])check(storage[key]===null,key+' admitted '+JSON.stringify(storage));
  proof.storage=storage;pass('storage-admission');
  // Override only the position source, never permission status or prompting.
  // Native GeolocationImpl takes this path before subscribing to real devices.
  await main.send('Emulation.setGeolocationOverride',{latitude:0,longitude:0,accuracy:1});
  check(await permission()==='prompt','fresh document inherited permission');
  check(await workerPermission()==='prompt','fresh worker inherited permission');
  await value(main,`window.permissionWorker=new Worker('/permission-worker.js');
    window.workerPermissionEvents=[];permissionWorker.onmessage=e=>workerPermissionEvents.push(e.data);
    permissionWorker.postMessage('observe')`);
  await until(async()=>await value(main,"workerPermissionEvents.includes('prompt')"),'worker initial permission subscription');
  const position=await locate();check(position.latitude===0&&position.longitude===0,'fake position source bypassed');
  check(await permission()==='granted','native prompt did not grant current document');
  await until(async()=>await value(main,"workerPermissionEvents.includes('granted')"),'worker permission subscription did not observe grant');
  check(await workerPermission()==='granted','new worker lost creator document grant');
  const peerURL=base+'/page/permission-peer';await keeper.send('Page.navigate',{url:peerURL});
  await committedHistory(keeper,peerURL);
  await until(async()=>await value(keeper,"document.readyState==='complete'&&!!window.docNonce"),'peer document load');
  check(await permission(keeper)==='prompt','sibling document inherited grant');
  check(await workerPermission(keeper)==='prompt','sibling worker inherited grant');
  const policyDenied=await value(main,`new Promise(resolve=>{const f=document.createElement('iframe');
    f.allow="geolocation 'none'";f.srcdoc='<p>policy fixture</p>';f.onload=async()=>{
      const state=await f.contentWindow.navigator.permissions.query({name:'geolocation'});resolve(state.state);f.remove()};document.body.append(f)})`);
  check(policyDenied==='denied','document grant bypassed native permissions policy');
  proof.permissions={position,policyDenied};pass('document-permission-grant');
  const before=await echo();check(!before.cookie,'HTTP cookie retained');
  await value(main,"history.pushState({live:true},'', '#same-document')");
  const same=await echo();check(before.connection===same.connection,'pushState rotated current document network');
  check(await value(main,'window.docNonce')===a.nonce,'pushState replaced document');
  check(await permission()==='granted','same-document navigation lost grant');
  proof.sameDocument={before,same};pass('same-document-network');
  const b=await load('b'),after=await echo();
  if(mode==='cut')check(after.connection!==before.connection,'cross-document connection reused');
  else check(after.connection===before.connection,'KEEP positive control did not reuse connection');
  proof.crossDocument={after};pass('cross-document-network');
  check(await permission()==='prompt','new document retained permission');
  check(await workerPermission()==='prompt','new document worker retained permission');
  check(await permission(keeper)==='prompt','prior document grant reached sibling');
  pass('document-permission-boundary');
  const mediaB=await mediaIdentity(),mediaBRepeat=await mediaIdentity();
  check(mediaB.id&&mediaB.state==='live','fake video did not reach the media consumer');
  check(mediaB.id===mediaBRepeat.id,'media device identity unstable inside a document');
  if(filesEnabled){
  await value(keeper,`window.fileChannel=new BroadcastChannel('alloy-explicit-file');
    fileChannel.onmessage=e=>{if(e.data.kind==='save')window.receivedOutput=e.data.file;
      else window.receivedFile=e.data};true`);
  const picked=await userAction(main,{awaitPromise:true,returnByValue:true,
    expression:`(async()=>{[window.selectedFile]=await showOpenFilePicker();
      const file=await selectedFile.getFile();
      window.fileChannel=new BroadcastChannel('alloy-explicit-file');fileChannel.postMessage(selectedFile);
      return {permission:await selectedFile.queryPermission({mode:'read'}),text:await file.text()}})()`});
  check(!picked.exceptionDetails,'native selected file '+JSON.stringify(picked.exceptionDetails));
  check(picked.result.value.permission==='granted'&&picked.result.value.text===secret,'explicit selected file did not reach consumer');
  await until(async()=>await value(keeper,'!!window.receivedFile'),'explicit live file handle communication');
  check(await value(keeper,'receivedFile.getFile().then(f=>f.text())')===secret,'live communicated file unreadable');
  proof.files={selected:picked.result.value};
  const saved=await userAction(main,{awaitPromise:true,returnByValue:true,
    expression:`(async()=>{window.savedFile=await showSaveFilePicker({suggestedName:'selected-output.txt'});
      const writer=await savedFile.createWritable();await writer.write(${JSON.stringify(secret)});await writer.close();
      fileChannel.postMessage({kind:'save',file:savedFile});
      return {permission:await savedFile.queryPermission({mode:'readwrite'}),text:await(await savedFile.getFile()).text()}})()`});
  check(!saved.exceptionDetails&&saved.result.value.permission==='granted'&&saved.result.value.text===secret,
    'explicit selected file write did not reach consumer '+JSON.stringify(saved));
  await until(async()=>await value(keeper,'!!window.receivedOutput'),'explicit live writable handle communication');
  check(await value(keeper,"receivedOutput.queryPermission({mode:'readwrite'})")==='granted','live shared writable handle lost grant');
  proof.files.saved=saved.result.value;
  }
  const zoomBefore=await value(main,'devicePixelRatio'),peerZoom=await value(keeper,'devicePixelRatio');
  await control(main,'zoom-set');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='zoom'&&e.level===1),'native zoom control');
  const zoomed=await until(async()=>{const valueNow=await value(main,'devicePixelRatio');return valueNow>zoomBefore+.1?valueNow:null},'renderer did not consume zoom');
  check(await value(keeper,'devicePixelRatio')===peerZoom,'document zoom leaked to sibling');
  await value(main,`history.replaceState(null,'','#zoom-view');history.replaceState(null,'',${JSON.stringify(base+'/page/b')})`);
  check(await value(main,'devicePixelRatio')===zoomed,'same-document navigation lost zoom');
  const identityB=await noise();
  check(identityB.first===identityB.repeat,'noise changed inside one document');
  check(identityB.first===identityB.worker,'worker did not inherit document noise');
  check(await value(main,'document.cookie')==='','cookie visible on next page');
  await value(main,`document.querySelector('#saved').value=${JSON.stringify(secret)};scrollTo(0,900);history.replaceState({marker:${JSON.stringify(secret)}},'')`);
  const c=await load('c');const identityC=await noise();
  if(filesEnabled){
  check(await value(keeper,"receivedFile.queryPermission({mode:'read'})")==='denied','retired document file authority survived in sibling');
  const retiredFile=await value(keeper,"receivedFile.getFile().then(()=>({allowed:true}),e=>({allowed:false,error:e.name}))");
  check(!retiredFile.allowed&&retiredFile.error==='NotAllowedError','expired live handle still reads file '+JSON.stringify(retiredFile));
  proof.files.retired=retiredFile;pass('document-selected-file');
  const retiredWrite=await value(keeper,"receivedOutput.createWritable().then(()=>({allowed:true}),e=>({allowed:false,error:e.name}))");
  check(!retiredWrite.allowed&&retiredWrite.error==='NotAllowedError','expired shared handle still writes '+JSON.stringify(retiredWrite));
  proof.files.retiredWrite=retiredWrite;
  }
  await until(async()=>Math.abs(await value(main,'devicePixelRatio')-zoomBefore)<1e-6,'new document retained old zoom');
  proof.zoom={before:zoomBefore,zoomed,sibling:peerZoom,after:await value(main,'devicePixelRatio')};pass('document-view-zoom');
  const mediaC=await mediaIdentity();
  check(mediaC.id&&mediaC.id!==mediaB.id,'media device identity retained across documents');
  proof.media={b:mediaB,repeat:mediaBRepeat,c:mediaC};pass('document-media-identity');
  check(identityC.first===identityC.repeat&&identityC.first===identityC.worker,'new document noise unstable');
  check(identityC.first!==identityB.first,'noise identity retained across documents');
  proof.noise={b:identityB,c:identityC};pass('document-noise-identity');
  let h=await history();const bEntry=h.entries.find(e=>e.url===base+'/page/b');check(bEntry,'missing B history entry');
  const back=await go(bEntry);check(back.field===''&&back.scroll===0&&back.state===null,'restore payload retained '+JSON.stringify(back));
  h=await history();const cEntry=h.entries.find(e=>e.url===base+'/page/c');check(cEntry,'missing forward C');
  const forward=await go(cEntry);check(forward.nonce!==c.nonce,'forward document reused');
  await go(bEntry);await load('d');h=await history();
  check(h.currentIndex===h.entries.length-1&&!h.entries.some(e=>e.url===base+'/page/c'),'forward stack not truncated');
  proof.history={back,forward,afterBranch:h};pass('url-only-back-forward');
  await load('permission-pending');
  await control(main,'hold-permission');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='permission-hold-ready'),'native permission hold control');
  await value(main,`window.pendingLocation=new Promise(resolve=>navigator.geolocation.getCurrentPosition(
    ()=>resolve('granted'),()=>resolve('cancelled'),{timeout:15000}));true`);
  const held=await until(async()=>(await nativeEvents()).find(e=>e.event==='permission'&&e.held),'native pending permission');
  await load('permission-replacement');
  const dismissed=await until(async()=>(await nativeEvents()).find(e=>e.event==='permission-dismissed'&&e.prompt===held.prompt),'pending permission cancelled on navigation');
  await control(main,'release-held-permission');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='permission-released'&&e.prompt===held.prompt),'late native callback release');
  check(await permission()==='prompt'&&await workerPermission()==='prompt','expired grant authorized replacement document');
  const replacementPosition=await locate();
  const prompts=(await nativeEvents()).filter(e=>e.event==='permission');
  check(prompts.length===3&&prompts[0].held===0&&prompts[1].held===1&&prompts[2].held===0,'native prompt sequence '+JSON.stringify(prompts));
  check(await permission()==='granted','replacement cannot obtain its own grant');
  check(await permission(keeper)==='prompt','replacement grant leaked to sibling');
  proof.permissions.stale={held,dismissed,prompts,replacementPosition};pass('expired-permission-callback');
  if(filesEnabled){
  await control(main,'hold-file');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='file-hold-ready'),'native file hold control');
  await userAction(main,{expression:"window.pendingFile=showOpenFilePicker().catch(e=>e.name);true"});
  await until(async()=>(await nativeEvents()).some(e=>e.event==='file-picker'&&e.held),'native held file picker');
  await load('file-replacement');
  await control(main,'release-held-file');
  await until(async()=>(await nativeEvents()).some(e=>e.event==='file-released'),'late native file picker callback');
  const freshFile=await userAction(main,{awaitPromise:true,returnByValue:true,
    expression:"showOpenFilePicker().then(async([f])=>({permission:await f.queryPermission({mode:'read'}),text:await(await f.getFile()).text()}))"});
  check(!freshFile.exceptionDetails&&freshFile.result.value.permission==='granted'&&freshFile.result.value.text===secret,'replacement cannot select its own file '+JSON.stringify(freshFile));
  check(await value(keeper,"receivedFile.queryPermission({mode:'read'})")==='denied','replacement grant revived old file authority');
  proof.files.replacement=freshFile.result.value;pass('expired-file-picker-callback');
  }
  await value(main,"fetch('/close-pool').then(r=>r.text())");await sleep(300);
  await load('ticket-a');const inside=await value(main,"fetch('/ticket-echo').then(r=>r.json())");
  check(inside.resumed,'TLS resumption positive control inside a document failed');
  await load('ticket-b');const ticket=await value(main,'window.connectionProof');
  check(ticket.resumed===(mode==='keep'),'cross-document TLS session '+JSON.stringify(ticket));
  check(ticket.connection!==inside.connection,'TLS proof reused open socket instead of handshaking');
  proof.tls={inside,ticket};pass('tls-session-boundary');
  const response=await fetch(`http://127.0.0.1:${port}/json/close/${list[0].id}`);check(response.ok,'native tab close refused');
  await until(async()=>{const t=await targets();return t.length===1&&!t.some(x=>x.id===list[0].id)},'tab did not close');
  await control(keeper,'open-tab-'+secret);
  const reopened=await until(async()=>{const t=await targets();return t.length===2?t.find(x=>x.id!==list[1].id):null},'native tab reopen');
  const fresh=await connect(reopened);const empty=await committedHistory(fresh);
  check(empty.entries.length<=1&&empty.entries.every(e=>e.url==='about:blank'),'closed tab history retained '+JSON.stringify(empty));
  proof.closedTab={empty};pass('tab-close-stack');fresh.ws.close();
}
if(filesEnabled&&Number(baseline)<0)check(nativeActions.length===4,'native gesture action count');
proof.nativeGestureActions=nativeActions;
proof.driveSeconds=(performance.now()-start)/1000;main.ws.close();keeper.ws.close();console.log(JSON.stringify(proof));
'''.replace('__NOISE_FUNCTION__', NOISE_FUNCTION)

DISPOSE = r'''
import {readFile} from 'node:fs/promises';
const [port,eventLog,parentID]=process.argv.slice(1);
''' + CDP_HELPERS + r'''
const pages=await pageTargets();
check(pages.some(p=>p.id===parentID),'persistent parent is not alive');
for(const page of pages.filter(p=>p.id!==parentID)){
  const response=await fetch(`http://127.0.0.1:${port}/json/close/${page.id}`);
  check(response.ok,'stateless browser close refused');
}
const closed=await until(async()=>{
  const events=await nativeEvents(),made=events.filter(e=>e.event==='created');
  const closedIDs=new Set(events.filter(e=>e.event==='closed').map(e=>e.browser));
  return made.length&&made.every(e=>closedIDs.has(e.browser))?Array.from(closedIDs):null;
},'native stateless browser close callbacks');
const remaining=await pageTargets();
check(remaining.length===1&&remaining[0].id===parentID,'unexpected remaining browser');
const parent=await connect(remaining[0]);
await control(parent,'dispose-stateless');
await until(async()=>(await nativeEvents()).some(e=>e.event==='stateless-released'),'native context references released');
parent.ws.close();console.log(JSON.stringify({parent:parentID,closed}));
'''

CLOSE = r'''
const port=process.argv[1];
const t=(await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).filter(t=>t.type==='page');
for(const p of t){const r=await fetch(`http://127.0.0.1:${port}/json/close/${p.id}`);if(!r.ok)throw Error('close refused')}
'''

NORMAL_DATALIST = r'''
import {readFile} from 'node:fs/promises';
const [port,base,parentID,popupReceipt]=process.argv.slice(1);
''' + CDP_HELPERS + DATALIST_HELPERS + r'''
const target=(await pageTargets()).find(t=>t.id===parentID);
check(target,'persistent parent missing for OSR parity');
const parent=await connect(target);await parent.send('Page.enable');
const url=base+'/page/a';await parent.send('Page.navigate',{url});
await until(async()=>await value(parent,"document.readyState==='complete'&&!!window.docNonce&&location.href==="+JSON.stringify(url)),'persistent datalist page load');
await committedHistory(parent,url);
const outcome=await datalistOutcome(parent);
parent.ws.close();console.log(JSON.stringify(outcome));
'''


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def finalize_case(page_root, proof):
    """Create the canonical case value only after its caller finishes enrichment."""
    assert not (page_root/'proof.json').exists()
    snapshot = json.loads(json.dumps(proof))
    save(page_root/'proof.json', snapshot)
    return snapshot


def publish_case(evidence, page_root, results, proof):
    """Publish one finalized value to both evidence views without shared aliases."""
    assert page_root.parent == evidence
    snapshot = finalize_case(page_root, proof)
    results.append(snapshot)
    save(evidence/'results.json', results)


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, certificate, key, evidence, secure=True):
        super().__init__(('127.0.0.1', 0), Handler)
        self.tls = None
        if secure:
            self.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.tls.minimum_version = self.tls.maximum_version = ssl.TLSVersion.TLSv1_2
            self.tls.load_cert_chain(certificate, key)
        self.connections = itertools.count(1)
        self.lock = threading.Lock()
        self.sockets = set()
        self.events = []
        self.evidence = evidence
        self.trial_token = None

    def origin_trial_token(self):
        with self.lock:
            if self.trial_token is None:
                tool = W/'src/tools/origin_trials/generate_token.py'
                # Chromium's published test key, never a production signing key.
                import base64
                public = base64.b64encode((tool.parent/'eftest.key').read_bytes()[32:]).decode()
                assert public == 'dRCs+TocuKkocNKa0AtZ4awrt9XKH2SQCI6o4FY6BNA='
                origin = 'https://localhost:'+str(self.server_port)
                output = subprocess.check_output(['python3', str(tool), origin,
                    'FrobulatePersistent', '--expire-timestamp=2000000000'], text=True, timeout=20)
                tokens = [line for line in output.splitlines()
                          if line and all(c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=' for c in line)]
                self.trial_token = tokens[-1]
                payload = json.loads(base64.b64decode(self.trial_token)[69:])
                assert payload['origin'] == origin and payload['feature'] == 'FrobulatePersistent'
                assert payload['expiry'] == 2000000000
            return self.trial_token

    def get_request(self):
        connection, peer = super().get_request()
        connection.settimeout(20)
        if self.tls is None:
            return connection, peer
        return self.tls.wrap_socket(connection, server_side=True,
                                    do_handshake_on_connect=False), peer

    def close_pool(self):
        time.sleep(.1)
        with self.lock:
            sockets = list(self.sockets)
        for connection in sockets:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def setup(self):
        super().setup()
        with self.server.lock:
            self.connection_id = next(self.server.connections)
            self.server.sockets.add(self.connection)

    def finish(self):
        try:
            super().finish()
        finally:
            with self.server.lock:
                self.server.sockets.discard(self.connection)

    def do_GET(self):
        path = urlsplit(self.path).path
        secure = isinstance(self.connection, ssl.SSLSocket)
        proof = dict(path=path, connection=self.connection_id,
                     resumed=self.connection.session_reused if secure else False,
                     session=self.connection.session.id.hex() if secure else '',
                      cookie=self.headers.get('Cookie', ''))
        if path in ('/embedded-cookie-write', '/embedded-network-echo', '/embedded-http-cache', '/embedded-origin-trial', '/embedded-sw.js'):
            markers = parse_qs(urlsplit(self.path).query).get('marker', [])
            if (len(markers) != 1 or not markers[0].startswith('ALLOY_STATE_') or
                    len(markers[0]) != 44 or any(c not in '0123456789abcdef' for c in markers[0][12:])):
                self.send_error(400, 'invalid embedded fixture marker')
                return
            proof['marker'] = markers[0]
            if path == '/embedded-http-cache':
                proof['cacheToken'] = secrets.token_hex(16)
        if path == '/embedded-origin-trial':
            query = parse_qs(urlsplit(self.path).query)
            tag, = query['tag']
            trial_kind, = query['kind']
            assert tag and all(c in 'abcdefghijklmnopqrstuvwxyz-' for c in tag)
            assert trial_kind in ('critical', 'script', 'invalid')
            proof.update(trialTag=tag, trialKind=trial_kind)
        if path in ('/ordinary-cdn.js', '/ordinary-sw.js'):
            markers=parse_qs(urlsplit(self.path).query).get('marker',[])
            assert len(markers)==1 and markers[0].startswith('ORDINARY_') and len(markers[0])==41
            assert all(c in '0123456789abcdef' for c in markers[0][9:])
            proof['marker']=markers[0]
            if path == '/ordinary-cdn.js':
                proof['cdnToken']=secrets.token_hex(16)
        with self.server.lock:
            self.server.events.append(proof)
            save(self.server.evidence / 'server.json', self.server.events)
        close = path.startswith('/ticket-') or '/page/ticket-' in path or path == '/close-pool'
        kind = 'application/json'
        body = json.dumps(proof).encode()
        if path in ('/embedded-storage-frame', '/embedded-origin-trial'):
            from profile_embedded import frame_body
            kind = 'text/html'
            body = frame_body()
            if path == '/embedded-origin-trial':
                trial_token = self.server.origin_trial_token()
                if trial_kind == 'invalid':
                    trial_token = trial_token[:16]+('A' if trial_token[16] != 'A' else 'B')+trial_token[17:]
                if trial_kind == 'script':
                    meta = ('<meta http-equiv="origin-trial" content="'+trial_token+'">').encode()
                    body = body.replace(b'<meta charset="utf-8">', b'<meta charset="utf-8">'+meta)
        elif path.startswith('/page/'):
            label = path.rsplit('/', 1)[1]
            nonce = secrets.token_hex(8)
            kind = 'text/html'
            script = '' if label.startswith('ticket-') else '<script src="/cache.js"></script>'
            body = (f'<!doctype html><link rel="icon" href="data:,"><input id="saved" autocomplete="on">'
                    f'<div style="height:4000px">fixture</div><script>window.docNonce={json.dumps(nonce)};'
                    f'window.connectionProof={json.dumps(proof)};'
                    f'{page_control_script(label+"_"+nonce)}</script>{script}').encode()
        elif path == '/cache.js':
            kind = 'application/javascript'
            body = b'window.resourceLoaded = true;'
        elif path == '/ordinary-cdn.js':
            kind = 'application/javascript'
            body = ('window.ordinaryCdnToken='+json.dumps(proof['cdnToken'])+';').encode()
        elif path == '/memory-cache':
            kind = 'text/plain'
            body = b'memory-http-value'
        elif path == '/sw.js':
            kind = 'application/javascript'
            body = b'self.addEventListener("fetch",()=>{});'
        elif path in ('/embedded-sw.js', '/ordinary-sw.js'):
            kind = 'application/javascript'
            body = ("const marker="+json.dumps(proof['marker'])+";"
                "self.addEventListener('install',event=>event.waitUntil(self.skipWaiting()));"
                "self.addEventListener('activate',event=>event.waitUntil(self.clients.claim()));"
                "self.addEventListener('message',event=>{if(event.data==='scope-value')"
                "event.ports[0].postMessage({marker,scope:self.registration.scope})});").encode()
        elif path == '/worker.js':
            kind = 'application/javascript'
            body = ('self.onmessage=()=>postMessage(('+NOISE_FUNCTION+
                    ')(new OffscreenCanvas(32,32)));').encode()
        elif path == '/permission-worker.js':
            kind = 'application/javascript'
            body = b'''self.onmessage=async e=>{const p=await navigator.permissions.query({name:'geolocation'});
              postMessage(p.state);if(e.data==='observe')p.onchange=()=>postMessage(p.state)};'''
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'public,max-age=31536000' if path in
                         ('/cache.js', '/memory-cache', '/embedded-http-cache', '/ordinary-cdn.js') else 'no-store')
        if path == '/embedded-origin-trial' and trial_kind != 'script':
            self.send_header('Origin-Trial', trial_token)
            self.send_header('Critical-Origin-Trial', 'FrobulatePersistent')
        if path == '/embedded-cookie-write':
            self.send_header('Set-Cookie', 'embedded_http='+proof['marker']+
                             '; Path=/; Secure; HttpOnly; SameSite=None; Max-Age=31536000')
        if path.startswith('/page/'):
            self.send_header('Set-Cookie', 'alloy_http_probe=refused; SameSite=Lax')
        if close:
            self.send_header('Connection', 'close')
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        if path == '/close-pool':
            threading.Thread(target=self.server.close_pool, daemon=True).start()


def inventory(root):
    observed = observe_tree(root, ('profile', 'home', 'cache', 'config', 'data'), hashes=False,
                            report_path=root/('inventory-observation-'+str(time.monotonic_ns())+'.json'))
    return {name: value['kind'] for name, value in observed.items()}


def await_parent_ready(root, process):
    """Observe native completion, never filesystem quiet or elapsed startup time."""
    receipt = root/'native-ready.json'
    deadline = time.monotonic()+90
    while time.monotonic() < deadline:
        assert process.poll() is None, 'parent process exited before native readiness'
        if receipt.exists():
            ready = json.loads(receipt.read_text())
            assert set(ready['completed']) == {'startup', 'preferred-apps-write', 'blob-limit-setup'}
            events = [json.loads(line[len('PROFILE_EVENT '):]) for line in
                      (root/'stdio.log').read_text().splitlines() if line.startswith('PROFILE_EVENT ')]
            if any(e['event'] == 'bootstrap-created' for e in events):
                assert not any(e['event'] in ('stateless-start', 'created') for e in events)
                assert [e['pid'] for e in events if e['event'] == 'process'] == [ready['pid']]
                # Native completion is the barrier. Root inventories below test
                # the no-files boundary without parsing Chromium store formats.
                return ready
        time.sleep(.05)
    raise AssertionError('native parent startup/store callbacks did not complete')


def inventory_changes(before, after):
    return dict(added={k:v for k,v in after.items() if before.get(k) != v},
                removed={k:v for k,v in before.items() if after.get(k) != v})


def await_context_stopped(root, process, parent_id, port):
    receipt = root/'native-context-stopped.json'
    deadline = time.monotonic()+90
    while time.monotonic() < deadline:
        assert process.poll() is None, 'parent exited before context destruction'
        if receipt.exists():
            stopped = json.loads(receipt.read_text())
            assert len(stopped['completed']['request-context']) >= 2
            assert stopped['completed']['browser-context'] and stopped['completed']['otr-profile']
            with urlopen('http://127.0.0.1:'+str(port)+'/json/list', timeout=5) as response:
                pages = [p['id'] for p in json.load(response) if p['type'] == 'page']
            assert pages == [parent_id], 'persistent parent not the sole live browser'
            return stopped
        time.sleep(.05)
    raise AssertionError('native stateless request contexts/OTR destruction did not finish')


def run_case(out, executable, root, mode, base, marker, symbol_file, baseline=-1,
              exercise_files=False, certificate_base='', fixture=None, alternate_fixture=None,
              memory=False, trusted_certificate=None, embedded_scope=None):
    root.mkdir()
    # An explicitly selected external user file is outside browser-owned roots.
    # Its original bytes must survive both grant lifetime and browser teardown.
    selected_file = root/'selected-user-file.txt'
    selected_file.write_text(marker)
    output_file = root/'selected-user-output.txt'
    roots = {name:root/name for name in ('profile', 'home', 'cache', 'config', 'data')}
    for path in roots.values():
        path.mkdir()
    xdg_cache = system_cache.create(root)
    # R11 keeps the OS/NSS certificate store outside Alloy. Give the isolated
    # test user that external input before either page or no-page run, rather
    # than confusing its first-use OS-store initialization with site storage.
    # It remains in both full inventories, and its bytes must stay unchanged.
    nss = roots['data']/'pki/nssdb'
    nss.mkdir(parents=True)
    with (root/'nss-fixture.log').open('xb') as log:
        subprocess.run(['certutil', '-N', '--empty-password', '-d', 'sql:'+str(nss)],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
        if trusted_certificate is not None:
            # A certificate-error response is deliberately not HTTP-cacheable.
            # Supply this isolated fixture's CA as external OS/NSS input before
            # startup; never bypass certificate validation for memory outcomes.
            assert memory and trusted_certificate.is_relative_to(W/'evidence')
            subprocess.run(['certutil', '-A', '-d', 'sql:'+str(nss),
                '-n', 'Alloy memory fixture', '-t', 'C,,', '-i', str(trusted_certificate)],
                stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
    nss_before = {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in nss.iterdir() if p.is_file()}
    save(root/'nss-before.json', nss_before)
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    argv = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24',
            'gdb', '-nx', '-q', '--batch', '-ex', 'source '+str(REPO/'tools/profile_readiness.py'),
            '-ex', 'run', '--args', str(executable),
            '--off-screen-rendering-enabled', '--ozone-platform=x11', '--disable-gpu',
            '--use-fake-device-for-media-stream',
             '--remote-debugging-port='+str(port)]
    if not certificate_base and not memory:
        argv.append('--ignore-certificate-errors')
    if exercise_files:
        # Brave deliberately defaults its local-file API off. Test the opt-in
        # native path explicitly, in addition to the unchanged default-state run.
        argv.append('--enable-features=FileSystemAccessAPI')
    env = dict(os.environ, TMPDIR=str(temp_path(W)),
               ALLOY_PROFILE_URL='about:blank', ALLOY_PROFILE_ROOT=str(roots['profile']),
                 ALLOY_PROFILE_NETWORK=mode, HOME=str(roots['home']),
                 ALLOY_PROFILE_SELECTED_FILE=str(selected_file),
                 ALLOY_PROFILE_OUTPUT_FILE=str(output_file),
                ALLOY_PROFILE_READY=str(root/'native-ready.json'),
                ALLOY_PROFILE_SYMBOLS=str(symbol_file),
               XDG_CACHE_HOME=str(xdg_cache), XDG_CONFIG_HOME=str(roots['config']),
                 XDG_DATA_HOME=str(roots['data']))
    if memory:
        env.update(ALLOY_PROFILE_MEMORY='1', ALLOY_PROFILE_OBSERVE_LIFETIME='1')
    if embedded_scope is not None:
        from profile_embedded import SCOPES
        assert memory and embedded_scope in SCOPES
        env['ALLOY_PROFILE_EMBEDDED_SCOPE'] = embedded_scope
        argv.append('--origin-trial-public-key=dRCs+TocuKkocNKa0AtZ4awrt9XKH2SQCI6o4FY6BNA=')
    browser_environment = {name:env[name] for name in
                           ('HOME', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME')}
    # The observer's own cache/config is evidence, never a browser-owned path.
    observer = root/'observer'
    observer.mkdir()
    for name in browser_environment:
        path = observer/name
        path.mkdir()
        env[name] = str(path)
    env['ALLOY_PROFILE_BROWSER_ENV'] = json.dumps(browser_environment)
    save(root/'browser-environment.json', browser_environment)
    save(root / 'argv.json', argv)
    started = time.time()
    with (root / 'stdio.log').open('xb') as log:
        process = subprocess.Popen(argv, cwd=out, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            ready = await_parent_ready(root, process)
            from profile_control_setup import complete as complete_control_setup
            control_setup = complete_control_setup(root, process, port, CDP_HELPERS)
            save(root/'before-paths.json', inventory(root))
            from profile_browser_lifecycle import make_scenario, inspect_browser_lifecycle
            phases = [('start-stateless', 2)]
            if not memory and baseline < 0 and not certificate_base:
                phases.append(('open-tab-', 1))
            scenario = make_scenario(DRIVE, phases)
            if memory:
                from profile_memory import build_scenario as memory_scenario
                scenario = memory_scenario(CDP_HELPERS)
                if embedded_scope is not None:
                    from profile_embedded import build_scenario as embedded_scenario
                    scenario = embedded_scenario(CDP_HELPERS, embedded_scope)
            response = subprocess.run(['node', '--input-type=module', '-e', scenario['drive'],
                str(port), base, mode, marker, str(baseline), str(root/'stdio.log'),
                  '1' if exercise_files else '0', certificate_base,
                  str(root/'native-popup-completed.json'),str(root/'native-gesture.json'),str(ready['pid']),
                  str(REPO/'tools/profile_user_action.mjs')], capture_output=True,
                text=True, timeout=120)
            (root / 'driver.stdout').write_text(response.stdout)
            (root / 'driver.stderr').write_text(response.stderr)
            assert response.returncode == 0, response.stderr
            proof = json.loads(response.stdout)
            save(root / 'live-paths.json', inventory(root))
            if not memory:
                dispose = subprocess.run(['node', '--input-type=module', '-e', DISPOSE,
                    str(port), str(root/'stdio.log'), proof['parentTarget']],
                    capture_output=True, text=True, timeout=40)
                (root/'dispose.stdout').write_text(dispose.stdout)
                (root/'dispose.stderr').write_text(dispose.stderr)
                assert dispose.returncode == 0, dispose.stderr
            stopped = await_context_stopped(root, process, proof['parentTarget'], port)
            assert stopped['pid'] == ready['pid']
            save(root/'final-paths.json', inventory(root))
            # Snapshot the stateless network interval before the persistent
            # parity control. Its normal cookies/cache are not stateless traffic.
            proof['statelessServerEnd'] = len(fixture.events)
            proof['statelessAlternateServerEnd'] = len(alternate_fixture.events) if alternate_fixture else 0
            if 'datalist' in proof:
                parity = subprocess.run(['node', '--input-type=module', '-e', NORMAL_DATALIST,
                    str(port), base, proof['parentTarget'], str(root/'native-popup-completed.json')],
                    capture_output=True, text=True, timeout=45)
                (root/'normal-datalist.stdout').write_text(parity.stdout)
                (root/'normal-datalist.stderr').write_text(parity.stderr)
                assert parity.returncode == 0, parity.stderr
                normal = json.loads(parity.stdout)
                proof['datalist']['normal'] = normal
                observed = ('value', 'focused', 'options', 'alive', 'nativePopupAttemptCompleted')
                assert {k:normal[k] for k in observed} == {k:proof['datalist']['stateless'][k] for k in observed}, proof['datalist']
                proof['datalist']['parity'] = True
                proof['checks'].append('osr-datalist-parity')
                save(root/'after-normal-parity-paths.json', inventory(root))
            close = subprocess.run(['node', '--input-type=module', '-e', CLOSE, str(port)],
                                   capture_output=True, text=True, timeout=20)
            (root / 'close.stderr').write_text(close.stderr)
            assert close.returncode == 0, close.stderr
            process.wait(timeout=30)
            assert process.returncode == 0, process.returncode
            assert json.loads((root/'native-exit.json').read_text())['exit_code'] == 0
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
    save(root/'process-final-paths.json', inventory(root))
    system_cache.check(xdg_cache)
    nss_after = {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in nss.iterdir() if p.is_file()}
    save(root/'nss-after.json', nss_after)
    assert nss_before == nss_after, 'browser changed the external OS/NSS input'
    assert selected_file.read_text() == marker, 'browser changed selected user file bytes'
    if baseline < 0 and exercise_files:
        assert output_file.read_text() == marker, 'explicit user output bytes differ'
    else:
        assert not output_file.exists(), 'unrequested external file creation'
    events = [json.loads(line[len('PROFILE_EVENT '):]) for line in
              (root/'stdio.log').read_text().splitlines() if line.startswith('PROFILE_EVENT ')]
    owners = [v['pid'] for v in events if v['event'] == 'process']
    assert len(owners) == 1, owners
    proof.update(pid=owners[0], started=started, finished=time.time())
    proof['parentReadiness'] = ready
    proof['parentPersistenceSetup'] = control_setup
    proof['memoryDestruction' if memory else 'statelessDestruction'] = stopped
    parent_events = [e for e in events if e['event'] == 'bootstrap-created']
    assert len(parent_events) == 1 and parent_events[0]['persistent_context'], parent_events
    assert len([e for e in events if e['event'] == 'bootstrap-closed']) == 1
    assert len([e for e in events if e['event'] == ('memory-start' if memory else 'stateless-start')]) == (2 if memory else 1)
    created = [v for v in events if v['event'] == 'created']
    closed = [v for v in events if v['event'] == 'closed']
    proof['nativeBrowserLifecycle'] = inspect_browser_lifecycle(
        events, scenario['browser_lifecycle'])
    assert {v['browser'] for v in created} == {v['browser'] for v in closed}
    assert all(v['shares_context'] and v['empty_cache_path'] and
               v['parent_preference_isolated'] for v in created), created
    if baseline < 0 and not certificate_base and not memory:
        media_events = [v for v in events if v['event'] == 'media-permission']
        assert len(media_events) == 3 and all(v['permissions'] == 2 for v in media_events), media_events
        file_events = [v for v in events if v['event'] == 'file-picker']
        assert [v['held'] for v in file_events] == ([0, 0, 1, 0] if exercise_files else []), file_events
        assert [v['save'] for v in file_events] == ([0, 1, 0, 0] if exercise_files else []), file_events
        navigations = [v for v in events if v['event'] == 'history-state' and v['browser'] != parent_events[0]['browser']]
        assert any(v['can_back'] and v['can_forward'] for v in navigations), navigations
        assert any(v['can_back'] and not v['can_forward'] and
                   v['url'].endswith('/page/d') for v in navigations), navigations
        assert not created[-1]['can_back'] and not created[-1]['can_forward'], created
        proof['checks'].append('native-history-flags')
    if baseline < 0 and certificate_base:
        assert '--ignore-certificate-errors' not in argv
        assert any(v['event'] == 'certificate' and v['held'] for v in events)
        assert any(v['event'] == 'certificate-released' for v in events)
        proof['certificate_errors_ignored'] = False
    proof['events'] = events
    if memory:
        assert not any(event['event'] == 'certificate' for event in events), \
            'memory storage fixture did not establish trusted TLS'
    before = json.loads((root/'before-paths.json').read_text())
    proof['inventoryChanges'] = {}
    for name in ('live-paths.json', 'final-paths.json'):
        changes = inventory_changes(before, json.loads((root/name).read_text()))
        proof['inventoryChanges'][name] = changes
        save(root/(name+'.difference.json'), changes)
    save(root/'native-proof.json', proof)
    parent_changes = inventory_changes(before, json.loads((root/'process-final-paths.json').read_text()))
    proof['parentProcessTeardownChanges'] = parent_changes
    save(root/'process-final-paths.json.difference.json', parent_changes)
    if (root/'after-normal-parity-paths.json').exists():
        after_parity = json.loads((root/'after-normal-parity-paths.json').read_text())
        proof['normalParityControlChanges'] = inventory_changes(
            json.loads((root/'final-paths.json').read_text()), after_parity)
        proof['afterNormalParityTeardownChanges'] = inventory_changes(
            after_parity, json.loads((root/'process-final-paths.json').read_text()))
    proof['normal_shutdown'] = True
    save(root / 'native-proof.json', proof)
    return proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=['stateless', 'memory', 'encrypted', 'delete', 'core', 'keyring', 'embedded', 'autolock'], default='core')
    parser.add_argument('--embedded-scope', choices=['both', 'per-tab', 'per-outer-site'],
                        default='both', help='Embedded implemented-storage slice only (default both scopes)')
    parser.add_argument('--exercise-files', action='store_true',
                        help='Additionally enable Brave FileSystemAccessAPI for native file-grant checks')
    parser.add_argument('--exercise-certificates', action='store_true',
                        help='Run real certificate callbacks and late decisions without ignoring certificate errors')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.check == 'autolock':
        from profile_autolock import main as autolock_main
        return autolock_main()
    if args.check in ('encrypted', 'delete'):
        from profile_encrypted import main as encrypted_main
        return encrypted_main()
    if args.check in ('core', 'keyring'):
        from profile_store_probe import main as store_main
        result = store_main()
        if result or args.check == 'core':
            return result
        from profile_encrypted import main as encrypted_main
        return encrypted_main()
    if args.exercise_files and args.exercise_certificates:
        parser.error('file and certificate consumer matrices are separate runs')
    if args.check in ('memory', 'embedded') and (args.exercise_files or args.exercise_certificates):
        parser.error('memory uses its native storage and context-lifetime matrix')
    assert os.environ['TMPDIR'] == str(temp_path(W))
    out = args.out.resolve()
    validate_output(W, out, REPO)
    if not args.inside:
        command = ['sudo', '-n', 'flock', '--wait', '1800', str(PRIMARY/'harness-run.lock'),
                   'flock', '--wait', '1800', str(PRIMARY/'controller.lock'),
                   'unshare', '--net', '--fork', '/bin/sh', '-c',
                   'ip link set lo up && exec runuser -u builder -- "$@"', 'alloy-profile-probe',
                   'env', 'TMPDIR='+str(temp_path(W)), 'PYTHONDONTWRITEBYTECODE=1',
                    'python3', str(Path(__file__).resolve()), '--out', str(out),
                    '--check', args.check, '--inside']
        if args.exercise_files:
            command.append('--exercise-files')
        if args.exercise_certificates:
            command.append('--exercise-certificates')
        if args.check == 'embedded':
            command.extend(['--embedded-scope', args.embedded_scope])
        print('PROFILE_WAIT shared namespace locks', flush=True)
        return subprocess.run(['timeout', '--kill-after=30', '4500', *command], timeout=4540).returncode
    assert os.geteuid() == 1000
    interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr'], timeout=10))
    routes = subprocess.check_output(['ip', 'route'], text=True, timeout=10)
    assert [i['ifname'] for i in interfaces] == ['lo'] and not routes.strip()
    with socket.socket() as s:
        assert s.connect_ex(('1.1.1.1', 443)) == errno.ENETUNREACH
    evidence = W/'evidence'/('profile-'+args.check+'-'+out.name+'-'+str(time.time_ns()))
    evidence.mkdir()
    save(evidence/'isolation.json', dict(interfaces=interfaces, routes=routes,
         uid=os.geteuid(), netns=os.readlink('/proc/self/ns/net')))
    print('PROFILE_EVIDENCE', evidence, flush=True)
    fixture = None
    alternate_fixture = None
    try:
        library = (out/'libcef.so').resolve()
        # The deleting destructor calls D2 directly, bypassing the D1 tail-jump
        # wrapper. Observe the actual body which destroys every owned member.
        wanted = {'_ZN21CefRequestContextImplD2Ev':'request-context',
                  '_ZN20ChromeBrowserContextD1Ev':'browser-context',
                  '_ZN23OffTheRecordProfileImplD1Ev':'otr-profile'}
        symbols = subprocess.check_output(['nm', '--defined-only', '--no-demangle', str(library)],
                                         text=True, timeout=60)
        offsets = {wanted[parts[2]]:int(parts[0], 16) for line in symbols.splitlines()
                   for parts in [line.split()] if len(parts) == 3 and parts[2] in wanted}
        assert set(offsets) == set(wanted.values()), 'missing native destruction symbols'
        symbol_file = evidence/'native-symbols.json'
        save(symbol_file, dict(library=str(library), destructors=offsets,
             size=library.stat().st_size, mtime_ns=library.stat().st_mtime_ns))
        executable = out/'alloy_profile_consumer'
        command = ['gcc', '-std=c11', '-D_GNU_SOURCE', '-DUSING_CEF_SHARED',
                   '-I', str(W/'src'), '-I', str(W/'src/cef'), '-I', str(out/'gen/cef'),
                   str(REPO/'tools/profile_consumer.c'), '-L', str(out),
                   '-Wl,-rpath,'+str(out), '-lcef', '-o', str(executable)]
        with (evidence/'compile.log').open('xb') as log:
            subprocess.run(['timeout', '90', *command], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=100)
        cert, key = evidence/'certificate.pem', evidence/'key.pem'
        with (evidence/'certificate.log').open('xb') as log:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-sha256', '-days', '1', '-subj', '/CN=localhost', '-addext',
                'subjectAltName=DNS:localhost,IP:127.0.0.1', '-keyout', str(key),
                '-out', str(cert)], stdout=log, stderr=subprocess.STDOUT,
                check=True, timeout=30)
        fixture = Fixture(cert, key, evidence)
        threading.Thread(target=fixture.serve_forever, daemon=True).start()
        base = 'https://127.0.0.1:'+str(fixture.server_port)
        certificate_base = ''
        if args.exercise_certificates:
            alternate_root = evidence/'alternate-certificate-server'
            alternate_root.mkdir()
            alternate_fixture = Fixture(cert, key, alternate_root)
            threading.Thread(target=alternate_fixture.serve_forever, daemon=True).start()
            certificate_base = 'https://127.0.0.1:'+str(alternate_fixture.server_port)
        results = []
        cases = [(mode, None) for mode in (('keep',) if args.check == 'memory' else ('keep', 'cut'))]
        if args.check == 'embedded':
            from profile_embedded import SCOPES
            cases = [('keep', scope) for scope in
                     (SCOPES if args.embedded_scope == 'both' else (args.embedded_scope,))]
        for mode, embedded_scope in cases:
            marker = 'ALLOY_STATE_'+secrets.token_hex(16)
            before = len(fixture.events)
            alternate_before = len(alternate_fixture.events) if alternate_fixture else 0
            case_name = mode if embedded_scope is None else mode+'-'+embedded_scope
            page_root, baseline_root = evidence/(case_name+'-page'), evidence/(case_name+'-no-page')
            proof = run_case(out, executable, page_root, mode, base, marker, symbol_file,
                               exercise_files=args.exercise_files, certificate_base=certificate_base,
                               fixture=fixture, alternate_fixture=alternate_fixture,
                                memory=args.check in ('memory', 'embedded'),
                                trusted_certificate=cert if args.check in ('memory', 'embedded') else None,
                                embedded_scope=embedded_scope)
            events = fixture.events[before:proof['statelessServerEnd']]
            if args.check in ('memory', 'embedded'):
                cache_requests = [v for v in events if v['path'] == '/memory-cache']
                assert len(cache_requests) == 3, cache_requests
                proof['httpCacheRequests'] = cache_requests
                if args.check == 'embedded':
                    network = proof['observations']['embeddedNetwork']
                    requests = [v for v in events if v['path'] == '/embedded-http-cache' and v.get('marker') == marker]
                    expected = 5 if embedded_scope == 'per-tab' else 3
                    assert len(requests) == expected, requests
                    assert len({v['cacheToken'] for v in requests}) == expected
                    assert {v['cacheToken'] for v in requests} == {
                        sample['cache'][0]['cacheToken'] for sample in
                        (network['original'], network['peer'], network['held'], network['fresh'],
                         proof['observations']['embeddedExpiry']['networkBefore'])}
                    proof['embeddedHttpCacheRequests'] = requests
                    trial_requests = [v for v in events if v['path'] == '/embedded-origin-trial' and v.get('marker') == marker]
                    counts = {tag:sum(v['trialTag'] == tag for v in trial_requests)
                              for tag in {v['trialTag'] for v in trial_requests}}
                    ordinary = {'ordinary-invalid':1, 'ordinary-header':2, 'ordinary-repeat':1,
                                'ordinary-script':1, 'ordinary-after-script':1}
                    assert {k:counts.get(k) for k in ordinary} == ordinary, ('ordinary signed trial fixture control', counts)
                    expected_trials = dict(ordinary, original=2, peer=2 if embedded_scope == 'per-tab' else 1,
                        **{'original-repeat':1, 'script':1, 'after-script':1,
                           'held':2 if embedded_scope == 'per-tab' else 1, 'fresh':2,
                           'expired':2, 'expired-repeat':1})
                    assert counts == expected_trials, ('native critical-restart scope consumer', embedded_scope, counts, expected_trials)
                    proof['observations']['embeddedOriginTrials']['criticalRequestCounts'] = counts
                    proof['embeddedOriginTrialRequests'] = trial_requests
                    proof['checks'].extend(['origin-trial-ordinary-signed-control',
                        'embedded-origin-trial-native-critical-scope', 'embedded-origin-trial-script-retention',
                        'embedded-origin-trial-retirement'])
                assert all(not change['added'] for change in proof['inventoryChanges'].values()), \
                    'memory context added paths: '+json.dumps(proof['inventoryChanges'])
                proof['checks'].extend(['memory-context-http-cache', 'memory-context-no-added-paths'])
                publish_case(evidence, page_root, results, proof)
                for check in proof['checks']:
                    print('PASS', check, flush=True)
                continue
            if alternate_fixture:
                events += alternate_fixture.events[alternate_before:proof['statelessAlternateServerEnd']]
                assert not any(v['path'] == '/page/certificate-held' for v in events), events
            assert sum(v['path']=='/cache.js' for v in events) >= (3 if args.exercise_certificates else 7), events
            assert not any(v['cookie'] for v in events), events
            baseline = run_case(out, executable, baseline_root, mode, base,
                                  marker, symbol_file, proof['driveSeconds'],
                                   exercise_files=args.exercise_files, certificate_base=certificate_base,
                                   fixture=fixture, alternate_fixture=alternate_fixture)
            baseline = finalize_case(baseline_root, baseline)
            proof['blankControl'] = baseline
            # Keep the completed blank control even when the actual inventory fails.
            save(evidence/(mode+'-controls.json'), dict(page=proof, blank=baseline))
            assert all(not change['added'] for change in proof['inventoryChanges'].values()), \
                'stateless context added paths: '+json.dumps(proof['inventoryChanges'])
            for name in ('profile', 'home', 'cache', 'config', 'data'):
                for path in (page_root/name).rglob('*'):
                    if path.is_file() and not path.is_symlink():
                        assert marker.encode() not in path.read_bytes(), 'state marker on disk: '+str(path)
            proof['checks'].extend(['http-cache-refetched', 'no-files-beyond-baseline'])
            publish_case(evidence, page_root, results, proof)
            for check in proof['checks']:
                print('PASS', check, mode, flush=True)
        ordinary=None
        if args.check == 'embedded':
            from profile_embedded_restart import run as run_restart
            from profile_embedded import SCOPES
            scopes = SCOPES if args.embedded_scope == 'both' else (args.embedded_scope,)
            ordinary_root = evidence/'process-restart'
            ordinary = run_restart(out, ordinary_root, Fixture, CDP_HELPERS, scopes)
            assert ordinary['pass_'] and ordinary['ordinary_functional_and_restart']
            assert not ordinary['encrypted_profile_verified']
            save(evidence/'ordinary-persistent.json',dict(root=str(ordinary_root),result=ordinary))
            from profile_embedded_profiles import run as run_profiles
            profiles = run_profiles(out, evidence/'encrypted-list-composition', CDP_HELPERS, scopes)
            assert len(profiles) == 5*len(scopes)
        coverage = dict(embedded_storage_only=False, embedded_network_slice_only=True,
                          ordinary_persistent_exercised=ordinary is not None,
                          encrypted_profile_verified=True,
                          directional_list_delete_and_explicit_lock_verified=True,
                          decay_verified=False, autolock_verified=False,
                         all_mode_kernel_proof=False,
                        full_item6E_gate=False) if args.check == 'embedded' else {}
        save(evidence/'result.json', dict(pass_=True, config=out.name, results=results,
              exercise_files=args.exercise_files,
              exercise_certificates=args.exercise_certificates,
              timestamp=datetime.now(timezone.utc).isoformat(), **coverage))
        return 0
    except BaseException:
        (evidence/'failure.txt').write_text(traceback.format_exc())
        raise
    finally:
        if fixture:
            fixture.shutdown()
            fixture.server_close()
        if alternate_fixture:
            alternate_fixture.shutdown()
            alternate_fixture.server_close()
        files = [p for p in sorted(evidence.rglob('*')) if p.is_file() and not p.is_symlink()
                 and p.name != 'SHA256SUMS']
        (evidence/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+
            '  '+str(p.relative_to(evidence))+'\n' for p in files))


if __name__ == '__main__':
    raise SystemExit(main())
