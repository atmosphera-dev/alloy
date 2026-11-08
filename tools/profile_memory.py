# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Native guest-context consumer sequence, composed with profile_probe helpers."""

DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,base,mode,secret,baseline,eventLog]=process.argv.slice(1);
''' + '__CDP_HELPERS__' + r'''
const proof={mode:'memory',checks:[],observations:{}};
const start=performance.now();
const pass=name=>proof.checks.push(name);
const parentTarget=await until(async()=>{
  const t=await pageTargets();return t.length===1&&t[0].url==='about:blank'?t[0]:null;
},'ready persistent control browser');
proof.parentTarget=parentTarget.id;
const parent=await connect(parentTarget);await parent.send('Page.enable');
async function guests(){return(await pageTargets()).filter(t=>t.id!==parentTarget.id)}
async function navigate(c,label){
  const url=base+'/page/'+label;const result=await c.send('Page.navigate',{url});
  check(!result.errorText,'memory navigation '+JSON.stringify(result));
  await until(async()=>await value(c,`document.readyState==='complete'&&location.href===${JSON.stringify(url)}&&!!window.docNonce`),'memory page '+label);
  await committedHistory(c,url);
  await pageControl(c);
}
async function write(c,marker){return value(c,`(async()=>{
  const marker=${JSON.stringify(marker)};
  document.cookie='guest='+marker+';path=/;SameSite=Lax';
  localStorage.setItem('guest',marker);sessionStorage.setItem('guest',marker);
  const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('guest-db',1);
    r.onupgradeneeded=()=>r.result.createObjectStore('values');
    r.onerror=()=>reject(r.error);r.onsuccess=()=>resolve(r.result)});
  try{await new Promise((resolve,reject)=>{const tx=db.transaction('values','readwrite');
    tx.objectStore('values').put(marker,'key');tx.oncomplete=resolve;
    tx.onerror=()=>reject(tx.error);tx.onabort=()=>reject(tx.error)});
  }finally{db.close()}
  await(await caches.open('guest-cache')).put('/guest-saved',new Response(marker));
  return {http:await(await fetch('/memory-cache')).text()};
})()`)}
async function read(c){return value(c,`(async()=>{
  let indexed=null;
  if((await indexedDB.databases()).some(db=>db.name==='guest-db')){
    const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('guest-db');
      r.onerror=()=>reject(r.error);r.onsuccess=()=>resolve(r.result)});
    try{indexed=await new Promise((resolve,reject)=>{const r=db.transaction('values').objectStore('values').get('key');
      r.onsuccess=()=>resolve(r.result??null);r.onerror=()=>reject(r.error)});
    }finally{db.close()}
  }
  const response=await caches.match('/guest-saved');
  const cookie=document.cookie.split(';').map(v=>v.trim()).find(v=>v.startsWith('guest='));
  return {cookie:cookie?cookie.slice(6):null,local:localStorage.getItem('guest'),indexed,
    cacheStorage:response?await response.text():null,
    session:sessionStorage.getItem('guest'),http:await(await fetch('/memory-cache')).text()};
})()`)}
function retained(result,marker){
  for(const key of ['cookie','local','indexed','cacheStorage'])
    check(result[key]===marker,'memory '+key+' readback '+JSON.stringify(result));
  check(result.http==='memory-http-value','native HTTP cache response');
}
function empty(result){
  for(const key of ['cookie','local','indexed','cacheStorage','session'])
    check(result[key]===null,'independent/fresh '+key+' inherited '+JSON.stringify(result));
}
async function close(target,c){
  const before=(await nativeEvents()).filter(e=>e.event==='closed').length;
  c.ws.close();const result=await fetch(`http://127.0.0.1:${port}/json/close/${target.id}`);
  check(result.ok,'native browser close refused');
  await until(async()=>!(await pageTargets()).some(t=>t.id===target.id),'native target close');
  await until(async()=>(await nativeEvents()).filter(e=>e.event==='closed').length===before+1,
    'native on_before_close completion before releasing context references');
}
async function pair(){
  const targets=await until(async()=>{const t=await guests();return t.length===2?t:null},'two shared memory browsers');
  const connections=await Promise.all(targets.map(connect));
  await Promise.all(connections.map(c=>c.send('Page.enable')));
  return {targets,connections};
}
async function peer(primary,known,name){
  await control(primary,name);
  const target=await until(async()=>{
    const t=(await guests()).filter(t=>!known.some(k=>k.id===t.id));return t.length===1?t[0]:null;
  },'independent native peer browser');
  const c=await connect(target);await c.send('Page.enable');return {target,c};
}
async function destroyed(requests,contexts){
  return until(async()=>{
    const path=eventLog.replace(/stdio\.log$/,'native-context-stopped.json');
    const result=JSON.parse(await readFile(path,'utf8')),done=result.completed;
    return done['request-context']?.length>=requests&&done['browser-context']?.length>=contexts&&
      done['otr-profile']?.length>=contexts?result:null;
  },'native request-context, browser-context and OTR destructor returns');
}
await control(parent,'start-stateless');
let {targets,connections}=await pair();let [main,shared]=connections;
await navigate(main,'memory-write');await write(main,secret);
await navigate(main,'memory-navigation');
const navigation=await read(main);retained(navigation,secret);
check(navigation.session===secret,'same-tab sessionStorage did not survive navigation');
await navigate(shared,'memory-shared');
const sharing=await read(shared);retained(sharing,secret);
check(sharing.session===null,'sessionStorage leaked into a separate tab');
proof.observations.sharing=sharing;pass('memory-navigation-and-sharing');

let isolated=await peer(main,targets,'memory-peer');
await navigate(isolated.c,'memory-independent');
const independent=await read(isolated.c);empty(independent);
await write(isolated.c,secret+'-peer');retained(await read(isolated.c),secret+'-peer');
retained(await read(main),secret);
proof.observations.independent=independent;pass('memory-independent-context-isolation');
await close(isolated.target,isolated.c);
await control(main,'memory-release-peer');
await until(async()=>(await nativeEvents()).filter(e=>e.event==='memory-peer-released').length===1,'independent peer reference release');

const stateless=await peer(main,targets,'memory-peer-stateless');
await navigate(stateless.c,'memory-stateless-peer');
const refused=await value(stateless.c,`(async()=>{
  const result={cookie:document.cookie};document.cookie='guest=forbidden;path=/';
  result.cookieAfter=document.cookie;
  for(const name of ['localStorage','sessionStorage']){
    try{window[name].setItem('guest','forbidden');result[name]='admitted'}catch(e){result[name]=e.name}
  }
  try{await new Promise((resolve,reject)=>{const r=indexedDB.open('guest-db');
    r.onsuccess=()=>{r.result.close();resolve()};r.onerror=()=>reject(r.error)});result.indexed='admitted'}catch(e){result.indexed=e.name}
  try{await(await caches.open('guest-cache')).put('/guest-saved',new Response('forbidden'));result.cacheStorage='admitted'}catch(e){result.cacheStorage=e.name}
  return result;
})()`);
check(refused.cookie===''&&refused.cookieAfter==='','stateless peer admitted cookies');
for(const key of ['localStorage','sessionStorage','indexed','cacheStorage'])
  check(refused[key]&&refused[key]!=='admitted','stateless peer admitted '+key);
retained(await read(main),secret);
proof.observations.stateless=refused;pass('memory-and-stateless-mixed-contexts');
await close(stateless.target,stateless.c);
await control(main,'memory-release-peer');
await until(async()=>(await nativeEvents()).filter(e=>e.event==='memory-peer-released').length===2,'stateless peer reference release');

for(let i=0;i<targets.length;i++)await close(targets[i],connections[i]);
await control(parent,'open-tab-held');
const heldTarget=await until(async()=>{const t=await guests();return t.length===1?t[0]:null},'browser from retained context reference');
const held=await connect(heldTarget);await held.send('Page.enable');
await navigate(held,'memory-held-reference');retained(await read(held),secret);
pass('memory-reference-outlives-all-browsers');
await close(heldTarget,held);
await control(parent,'dispose-stateless');
proof.firstRetirement=await destroyed(4,3);
pass('memory-native-context-retirement');

await control(parent,'memory-restart');
({targets,connections}=await pair());[main,shared]=connections;
await navigate(main,'memory-recreated');
const recreated=await read(main);empty(recreated);
await write(main,secret+'-new');await navigate(shared,'memory-recreated-shared');
retained(await read(shared),secret+'-new');
proof.observations.recreated=recreated;pass('memory-gone-after-native-retirement');
for(let i=0;i<targets.length;i++)await close(targets[i],connections[i]);
await control(parent,'dispose-stateless');
proof.finalRetirement=await destroyed(6,4);
parent.ws.close();proof.driveSeconds=(performance.now()-start)/1000;
console.log(JSON.stringify(proof));
'''


def build_scenario(helpers):
    from profile_browser_lifecycle import make_scenario
    return make_scenario(DRIVE.replace('__CDP_HELPERS__', helpers), [
        ('start-stateless', 2),
        ('memory-peer', 1),
        ('memory-peer-stateless', 1),
        ('open-tab-held', 1),
        ('memory-restart', 2),
    ])
