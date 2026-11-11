// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
import {readFile} from 'node:fs/promises';
import readline from 'node:readline';
const [port,base,phase,eventLog,marker]=process.argv.slice(2);
__CDP_HELPERS__
const origin=host=>base.replace('alpha.test',host);
const observations={phase,checks:[]};
const sockets=[];
async function events(){return(await readFile(eventLog,'utf8')).split('\n')
  .filter(s=>s.startsWith('EMBEDDED_PROFILE_EVENT ')).map(s=>JSON.parse(s.slice(23)))}
async function receipt(name){return until(async()=>(await events()).find(e=>e.event===name),'native '+name)}
async function command(client,name,event){
  const before=(await events()).length;await control(client,name);
  return until(async()=>(await events()).slice(before).find(e=>e.event===event),'new native '+event);
}
/**
 * Dispatch a command that retires its target and await its native completion.
 * Only a terminal CDP error is tolerated; dispatch and SDK acknowledgement remain required.
 * @param {object} client CDP connection to the target being retired.
 * @param {string} name Identified native command action.
 * @param {string} event SDK completion event expected after dispatch.
 * @returns {Promise<object>} The fresh SDK completion event.
 */
async function terminalCommand(client,name,event){
  const before=(await events()).length,request=profileControlRequest(name);
  try{await value(client,`document.title=${JSON.stringify(request.title)}`)}
  catch(error){if(!error.cdpTerminal)throw error}
  await profileControlReply(request);
  return until(async()=>(await events()).slice(before).find(e=>e.event===event),'new native '+event);
}
function pass(name){observations.checks.push(name)}
async function page(owner,host,label='initial'){
  const url=origin(host)+'/page/p'+owner+'/'+label;
  const target=await until(async()=>(await pageTargets()).find(t=>t.url===url),'profile page '+url);
  const c=await connect(target);sockets.push(c);await c.send('Page.enable');
  await until(async()=>{
    const state=await value(c,"({url:location.href,origin:location.origin,ready:document.readyState,fixture:document.body?.dataset.embeddedProfileFixture})");
    check(state.url===url&&state.ready==='complete'&&state.fixture==='ready',
      'expected fixture document '+JSON.stringify(state));return true;
  },'document ready');
  return {c,target};
}
let serial=0;
async function frame(c,host,action='read',valueMarker=marker){
  const id='embedded-'+(++serial),url=origin(host)+'/frame';
  return value(c,`new Promise((resolve,reject)=>{
    const f=document.createElement('iframe');f.id=${JSON.stringify(id)};
    f.allow='geolocation *';
    const receive=e=>{if(e.source!==f.contentWindow||e.origin!==${JSON.stringify(origin(host))}||e.data?.id!==${JSON.stringify(id)})return;
      removeEventListener('message',receive);e.data.error?reject(Error(e.data.error)):resolve(e.data.value)};
    addEventListener('message',receive);f.onload=()=>f.contentWindow.postMessage(
      {id:${JSON.stringify(id)},action:${JSON.stringify(action)},marker:${JSON.stringify(valueMarker)}},${JSON.stringify(origin(host))});
    f.src=${JSON.stringify(url)};document.body.append(f)})`);
}
async function real(c,owner,host){
  const wanted='REAL_'+owner+'_'+host+'_'+marker;
  await value(c,`localStorage.setItem('embedded-profile',${JSON.stringify(wanted)});
    document.cookie='embedded_profile='+${JSON.stringify(wanted)}+'; Path=/; SameSite=None; Secure';
    document.cookie='embedded_strict='+${JSON.stringify(wanted)}+'; Path=/; SameSite=Strict; Secure';
    fetch('/cookie-set?marker='+${JSON.stringify(wanted)},{credentials:'include'}).then(r=>r.ok)`);
  return wanted;
}
async function own(c){return value(c,`navigator.permissions.query({name:'notifications'}).then(async p=>({
  permission:p.state,local:localStorage.getItem('embedded-profile'),cookie:document.cookie,
  http:(await (await fetch('/cookie-echo',{credentials:'include'})).json()).cookie}))`)}
function temporary(sample,label){check(sample.local===null&&sample.cookie===''&&sample.http===''&&sample.permission!=='granted',
  'unlisted temporary selection '+label+' '+JSON.stringify(sample))}
await receipt('ready');
const a0=await page(0,'alpha.test'),b0=await page(0,'beta.test'),g0=await page(0,'gamma.test');
const a1=await page(1,'alpha.test'),b1=await page(1,'beta.test'),g1=await page(1,'gamma.test');
const real0=await real(b0.c,0,'beta.test'),real1=await real(b1.c,1,'beta.test');
await real(a0.c,0,'alpha.test');
const gamma0=await real(g0.c,0,'gamma.test');
const cold=phase==='after-delete';
const top0=await own(b0.c),top1=await own(b1.c);
check(top0.cookie.includes(real0)&&top1.cookie.includes(real1),
  'real first-party cookie controls '+JSON.stringify({top0,top1}));
check(top0.permission===(cold?'prompt':'granted')&&top1.permission==='denied',
  'real record permissions after unlock '+JSON.stringify({top0,top1}));
const listed=await frame(a0.c,'beta.test');
if(cold)temporary(listed,'cold after deletion');
else check(listed.local===real0&&listed.permission==='granted'&&listed.cookie.includes(real0)&&
    listed.http.includes('embedded_http='+real0)&&listed.http.includes('embedded_profile='+real0),
  'listed embedded site did not read its own real data '+JSON.stringify(listed));
check(!listed.cookie.includes('embedded_http=')&&!listed.cookie.includes('embedded_strict=')&&
  !listed.http.includes('embedded_strict='),'native SameSite/HttpOnly restrictions '+JSON.stringify(listed));
check(!listed.cross.includes(gamma0)&&!listed.redirect.includes(gamma0),
  'listed exception escaped its origin through fetch/redirect '+JSON.stringify(listed));
const unlisted=await frame(g0.c,'beta.test'),reverse=await frame(b0.c,'alpha.test'),isolated=await frame(a1.c,'beta.test');
for(const [label,sample] of Object.entries({unlisted,reverse,isolated}))temporary(sample,label);
const temporaryMarker='TEMP_'+marker;
const stored=await frame(g0.c,'beta.test','write',temporaryMarker);
check(stored.local===temporaryMarker&&stored.cookie.includes(temporaryMarker)&&
  stored.http.includes('embedded_http='+temporaryMarker),'temporary jar unusable');
check((await own(b0.c)).local===real0&&(await own(b1.c)).local===real1,'temporary write escaped to first-party/profile sibling');
pass('real-directional-list-own-record-browser-access');
pass('encrypted-cold-record-permissions-and-profile-isolation');
observations.initial={top0,top1,listed,unlisted,reverse,isolated,stored};
if(!cold){
  const listedMarker='LISTED_'+marker;
  const written=await frame(a0.c,'beta.test','write',listedMarker);
  const realAfterWrite=await own(b0.c);
  check(written.cookie.includes('embedded_profile='+listedMarker)&&
    written.http.includes('embedded_http='+listedMarker)&&
    realAfterWrite.cookie.includes('embedded_profile='+listedMarker)&&
    realAfterWrite.http.includes('embedded_http='+listedMarker),
    'listed script/HTTP writes did not reach own real jar '+JSON.stringify({written,realAfterWrite}));
  await real(b0.c,0,'beta.test');
  pass('listed-script-http-writes-reach-own-real-jar');
  await command(a0.c,'block-cookies','cookies-blocked');
  const blocked=await frame(a0.c,'beta.test','write','BLOCKED_'+marker);
  check(blocked.cookie===''&&blocked.http==='','listed exception bypassed explicit cookie block '+JSON.stringify(blocked));
  await command(a0.c,'unblock-cookies','cookies-unblocked');
  // The denied writes must not replace either first-party cookie value.
  const unblocked=await frame(a0.c,'beta.test');
  check(unblocked.cookie.includes(real0)&&unblocked.http.includes('embedded_http='+real0)&&
    !unblocked.cookie.includes('BLOCKED_')&&!unblocked.http.includes('BLOCKED_'),
    'explicit cookie block admitted a write '+JSON.stringify(unblocked));
  await real(b0.c,0,'beta.test');
  pass('listed-script-http-cookies-preserve-block-samesite-origin-and-redirect-boundaries');
  await command(a0.c,'clear-list','list-cleared');
  temporary(await frame(a0.c,'beta.test'),'explicit list removal');
  await command(a0.c,'restore-list','list-restored');
  const restored=await frame(a0.c,'beta.test');
  check(restored.local===real0&&restored.permission==='granted','restored list lost original beta record');
  pass('public-list-replacement-affects-new-document-admission');
}
// The controller scans the live roots before any deletion or lock can erase a
// native write. This is a browser-result barrier, not a filesystem observer.
console.log(JSON.stringify({phase:'live',observations}));
const input=readline.createInterface({input:process.stdin});
for await(const line of input){check(line==='continue','live scan acknowledgement');break}
input.close();process.stdin.pause();
if(phase==='read'){
  await command(g0.c,'delete-outer','outer-deleted');
  check(!(await pageTargets()).some(t=>t.id===a0.target.id),'deleted outer original browser survived');
  const fresh=await page(0,'alpha.test','after-delete');
  temporary(await frame(fresh.c,'beta.test'),'deleted outer list');
  const survivor=await own(b0.c);
  check(survivor.local===real0&&survivor.permission==='granted','outer deletion removed embedded site real record');
  check((await frame(g0.c,'beta.test')).local===temporaryMarker,'outer deletion changed unrelated temporary jar');
  check((await own(b1.c)).local===real1&&(await own(b1.c)).permission==='denied','outer deletion changed independent profile');
  pass('outer-record-delete-invalidates-list-and-scope-preserves-siblings');
  // Restore on the fresh outer generation, then delete Y to exercise R5 cleanup.
  await command(g0.c,'restore-list','list-restored');
  check((await frame(fresh.c,'beta.test')).local===real0,'fresh outer cannot use surviving real beta record');
  await command(g0.c,'delete-inner','inner-deleted');
  check(!(await pageTargets()).some(t=>t.id===b0.target.id),'deleted embedded real browser survived');
  for(const c of [fresh.c,g0.c]){
    const tree=await c.send('Page.getFrameTree');
    check(!(tree.frameTree.childFrames??[]).some(f=>f.frame.url.startsWith(origin('beta.test'))),'deleted embedded frame survived');
  }
  const freshBeta=await page(0,'beta.test','after-inner-delete');
  const reset=await own(freshBeta.c);
  check(reset.permission==='prompt','fresh beta inherited deleted record permission '+JSON.stringify(reset));
  await real(freshBeta.c,0,'beta.test');
  temporary(await frame(fresh.c,'beta.test'),'deleted embedded generated reference');
  temporary(await frame(g0.c,'beta.test'),'deleted embedded temporary jar in another outer');
  check((await own(b1.c)).local===real1&&(await own(b1.c)).permission==='denied','embedded deletion crossed profiles');
  pass('embedded-record-delete-removes-generated-list-reference-and-live-frames');
  await terminalCommand(g0.c,'lock-zero','zero-locked');
  const remaining=await pageTargets();
  check(!remaining.some(t=>t.url.includes('/page/p0/')),'acknowledged lock left profile zero browsers');
  check((await own(b1.c)).local===real1&&(await own(b1.c)).permission==='denied','lock retired a different profile');
  pass('profile-lock-revokes-original-list-context-and-browsers');
}
await terminalCommand(g1.c,'finish','finished');
for(const c of sockets)c.ws.close();
console.log(JSON.stringify(observations));
