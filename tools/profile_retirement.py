# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Shared end-consumer fixtures for live explicit deletion and decay."""

HELPERS = r'''
async function seedRecord(c){return value(c,`(async()=>{
  const marker='record-'+location.hostname;
  document.cookie='record_value='+marker+'; Path=/; Secure; SameSite=Lax';
  localStorage.setItem('record-value',marker);
  sessionStorage.setItem('record-value',marker);
  const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('record-values',1);
    r.onupgradeneeded=()=>r.result.createObjectStore('values');
    r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error)});
  await new Promise((resolve,reject)=>{const tx=db.transaction('values','readwrite');
    tx.objectStore('values').put(marker,'value');tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error)});
  window.retainedRecordDb=db;
  window.retainedRecordCache=await caches.open('record-values');
  await retainedRecordCache.put('/record-cache',new Response(marker));
  const root=await navigator.storage.getDirectory();
  const file=await root.getFileHandle('record-value',{create:true});
  const writer=await file.createWritable();await writer.write(marker);await writer.close();
  const registration=await navigator.serviceWorker.register('/service.js');
  await navigator.serviceWorker.ready;
  const http=await (await fetch('/record-http',{cache:'force-cache'})).text();
  const repeat=await (await fetch('/record-http',{cache:'force-cache'})).text();
  if(http!==repeat)throw Error('HTTP fixture was not cached');
  const cdnUrl=new URL('/record-http',location.href);cdnUrl.hostname='cdn.test';
  const cdn=await(await fetch(cdnUrl,{cache:'force-cache'})).text();
  const cdnRepeat=await(await fetch(cdnUrl,{cache:'force-cache'})).text();
  if(cdn!==cdnRepeat)throw Error('CDN fixture was not cached');
  return {marker,http,cdn};})()`)}
async function readRecord(c){return value(c,`(async()=>{
  const names=await indexedDB.databases();let idb=null;
  if(names.some(db=>db.name==='record-values')){
    const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('record-values');
      r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error)});
    idb=await new Promise((resolve,reject)=>{const r=db.transaction('values').objectStore('values').get('value');
      r.onsuccess=()=>resolve(r.result??null);r.onerror=()=>reject(r.error)});db.close();
  }
  const namesCache=await caches.keys();let cache=null;
  if(namesCache.includes('record-values')){const response=await (await caches.open('record-values')).match('/record-cache');
    cache=response?await response.text():null}
  const root=await navigator.storage.getDirectory();let opfs=null;
  try{opfs=await(await(await root.getFileHandle('record-value')).getFile()).text()}
  catch(error){if(error.name!=='NotFoundError')throw error}
  const cdnUrl=new URL('/record-http',location.href);cdnUrl.hostname='cdn.test';
  return {cookies:document.cookie,local:localStorage.getItem('record-value'),
    session:sessionStorage.getItem('record-value'),idb,cache,opfs,
    workers:(await navigator.serviceWorker.getRegistrations()).map(r=>r.scope),
    http:await(await fetch('/record-http',{cache:'force-cache'})).text(),
    cdn:await(await fetch(cdnUrl,{cache:'force-cache'})).text()};})()`)}
function checkRetiredRecord(found,before){
  check(!found.cookies.includes('record_value='),'deleted cookie '+JSON.stringify(found));
  for(const key of ['local','session','idb','cache','opfs'])check(found[key]===null,'deleted '+key+' '+JSON.stringify(found));
  check(found.workers.length===0,'deleted service-worker registration '+JSON.stringify(found));
  check(found.http!==before.http,'deleted HTTP response survived '+JSON.stringify(found));
  check(found.cdn!==before.cdn,'deleted top-site CDN response survived '+JSON.stringify(found));
}
function checkRetainedRecord(found,before){
  check(found.cookies.includes('record_value='+before.marker),'other cookie lost '+JSON.stringify(found));
  for(const key of ['local','session','idb','cache','opfs'])check(found[key]===before.marker,'other '+key+' lost '+JSON.stringify(found));
  check(found.workers.length===1,'other service worker lost '+JSON.stringify(found));
  check(found.http===before.http,'other HTTP cache lost '+JSON.stringify(found));
  check(found.cdn===before.cdn,'other top-site CDN cache lost '+JSON.stringify(found));
}
async function finishProfile(command,events){
  // Lock deliberately destroys this target. Like the auto-lock callback probe,
  // require its SDK acknowledgement, not a CDP reply from a retired document.
  try{await command()}catch(error){if(!error.cdpTerminal)throw error}
  await until(async()=>(await events()).some(e=>e.event==='locked'),'profile lock acknowledgement');
}
'''


def respond(handler):
    if handler.path != '/record-http':
        return False
    counts = getattr(handler.server, 'record_http_counts', None)
    if counts is None:
        counts = handler.server.record_http_counts = {}
    host = handler.headers['Host']
    counts[host] = counts.get(host, 0) + 1
    body = f'{host}-response-{counts[host]}'.encode()
    handler.send_response(200)
    handler.send_header('Content-Type', 'text/plain')
    handler.send_header('Access-Control-Allow-Origin', '*')
    handler.send_header('Cache-Control', 'public, max-age=3600')
    handler.send_header('Content-Length', str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)
    return True
