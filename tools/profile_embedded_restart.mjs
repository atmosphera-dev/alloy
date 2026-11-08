// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Browser-boundary restart/move/Back checks; no kernel observer is required.
import {readFile} from 'node:fs/promises';
import readline from 'node:readline';
const [port,base,mode,scope,phase,secret,eventLog]=process.argv.slice(2);
__CDP_HELPERS__
__EMBEDDED_FRAME_HELPERS__
const embeddedBase=base.replace('127.0.0.1','localhost');
const targets=await until(async()=>{const p=await pageTargets();return p.length===2?p:null},'two native no-disk browsers');
const clients=await Promise.all(targets.map(connect));
for(let n=0;n<2;n++){
  await clients[n].send('Page.enable');
  const url=base+'/page/kernel-'+phase+'-'+n;
  const result=await clients[n].send('Page.navigate',{url});check(!result.errorText,'kernel outer navigation');
  await committedHistory(clients[n],url);
  await until(async()=>await value(clients[n],"document.readyState==='complete'&&!!window.docNonce"),'kernel outer ready');
}
const [main,peer]=clients;
const ordinaryMarker='ORDINARY_'+secret.slice(-32);
async function callOrdinaryServiceWorker(marker,write){
  const scope=location.origin+'/ordinary-control/';
  let registration=await navigator.serviceWorker.getRegistration(scope);
  if(write)registration=await navigator.serviceWorker.register('/ordinary-sw.js?marker='+encodeURIComponent(marker),{scope:'/ordinary-control/'});
  if(!registration)return null;
  const worker=registration.installing||registration.waiting||registration.active;
  if(!worker)throw Error('ordinary native service worker missing');
  if(worker.state!=='activated')await new Promise((resolve,reject)=>{
    const update=()=>{
      if(worker.state==='activated'||worker.state==='redundant'){
        worker.removeEventListener('statechange',update);
        worker.state==='activated'?resolve():reject(Error('ordinary native service worker redundant'));
      }
    };
    worker.addEventListener('statechange',update);update();
  });
  if(!registration.active||registration.active.state!=='activated')throw Error('ordinary native service worker inactive');
  const reply=await new Promise((resolve,reject)=>{
    const channel=new MessageChannel();
    channel.port1.onmessage=event=>{channel.port1.close();resolve(event.data)};
    channel.port1.onmessageerror=()=>{channel.port1.close();reject(Error('ordinary worker message error'))};
    registration.active.postMessage('scope-value',[channel.port2]);
  });
  return {marker:reply.marker,scope:reply.scope,script:registration.active.scriptURL,state:registration.active.state};
}
async function ordinaryWorker(client,write){
  return value(client,`(${callOrdinaryServiceWorker.toString()})(${JSON.stringify(ordinaryMarker)},${JSON.stringify(write)})`);
}
async function ordinaryCdn(client){
  const cdn=embeddedBase+'/ordinary-cdn.js?marker='+encodeURIComponent(ordinaryMarker);
  return value(client,`new Promise((resolve,reject)=>{const s=document.createElement('script');s.src=${JSON.stringify(cdn)};s.onload=()=>resolve(window.ordinaryCdnToken);s.onerror=()=>reject(Error('ordinary CDN load'));document.head.append(s)})`);
}
async function firstPartyState(client,marker,write){
  return value(client,`(async()=>{
    const marker=${JSON.stringify(marker)};
    if(${JSON.stringify(write)}){
      localStorage.setItem('embedded-storage',marker);sessionStorage.setItem('embedded-storage',marker);
      document.cookie='embedded_js='+marker+'; Path=/; Max-Age=31536000; SameSite=Lax';
    }
    const echo=await(await fetch('/echo',{credentials:'include',cache:'no-store'})).json();
    const workers=(await navigator.serviceWorker.getRegistrations()).map(registration=>({scope:registration.scope,script:registration.active?.scriptURL??null}));
    return {local:localStorage.getItem('embedded-storage'),session:sessionStorage.getItem('embedded-storage'),cookie:document.cookie,httpCookie:echo.cookie,workers};
  })()`);
}
function cookieValues(sample,present,label){
  const parse=text=>Object.fromEntries(text.split(';').map(v=>v.trim()).filter(Boolean).map(v=>{const i=v.indexOf('=');return[v.slice(0,i),v.slice(i+1)]}));
  const js=parse(sample.cookie),http=parse(sample.echo.cookie);
  check((js.embedded_js??null)===(present?secret:null)&&!js.embedded_http,'move/Back JS cookies '+label);
  for(const key of ['embedded_js','embedded_http'])check((http[key]??null)===(present?secret:null),'move/Back HTTP cookies '+label);
  check(!sample.cookie.includes('ORDINARY_AWAY_')&&!sample.echo.cookie.includes('ORDINARY_AWAY_'),'real first-party cookie entered temporary scope');
  check(sample.cache.length===2&&sample.cache[0].cacheToken===sample.cache[1].cacheToken&&sample.cache.every(v=>v.marker===secret),'move/Back HTTP cache '+label);
}
async function embeddedSnapshot(client){
  return {scalar:await embedded(client,'scalar-read'),data:await embedded(client,'data-read'),network:await embedded(client,'network-read'),worker:await embedded(client,'sw-read')};
}
let ordinary;
if(mode==='persistent'){
  const previous=await value(main,`({local:localStorage.getItem('ordinary-control'),cookie:document.cookie})`);
  if(phase==='writer')check(previous.local===null,'fresh ordinary profile imported control');
  else check(previous.local===ordinaryMarker&&previous.cookie.includes('ordinary_control='+ordinaryMarker),'real ordinary control did not persist');
  await value(main,`localStorage.setItem('ordinary-control',${JSON.stringify(ordinaryMarker)});document.cookie='ordinary_control='+${JSON.stringify(ordinaryMarker)}+'; Path=/; Max-Age=31536000; SameSite=Lax';true`);
  const token=await ordinaryCdn(main);
  check(/^[a-f0-9]{32}$/.test(token),'ordinary CDN response token missing');
  const beforeWorker=await ordinaryWorker(main,false);
  const expectedWorker={marker:ordinaryMarker,scope:base+'/ordinary-control/',script:base+'/ordinary-sw.js?marker='+ordinaryMarker,state:'activated'};
  check(JSON.stringify(beforeWorker)===JSON.stringify(phase==='writer'?null:expectedWorker),'ordinary native worker restart control');
  const worker=await ordinaryWorker(main,true),peerWorker=await ordinaryWorker(peer,false);
  check(JSON.stringify(worker)===JSON.stringify(expectedWorker)&&JSON.stringify(peerWorker)===JSON.stringify(worker),'ordinary native worker activation/message/peer');
  ordinary={marker:ordinaryMarker,previous,cdnToken:token,serviceWorker:{before:beforeWorker,original:worker,peer:peerWorker}};
}
function dataValues(sample,present,label){
  check(JSON.stringify(Object.keys(sample).sort())===JSON.stringify(['dedicated','frame','reread','shared']),'kernel actor inventory');
  for(const [stage,values] of Object.entries(sample)){
    const completed=label==='peer'||stage==='shared'||stage==='reread'?['frame','dedicated','shared']:stage==='frame'?['frame']:['frame','dedicated'];
    bucketMetadataCheck(values.named.metadata,secret,present,completed);
    if(present&&label==='writer'&&stage!=='reread')bucketRetirementCheck(values.named.retirement,secret,stage);
    else check(values.named.retirement===undefined,'unexpected kernel bucket retirement write');
    for(const ns of ['default','named']){
      for(const kind of ['idb','cache','opfs'])for(const actor of ['frame','dedicated','shared'])
        check(values[ns][kind][actor]===(present&&completed.includes(actor)?secret+'|'+ns+'|'+actor+'|'+kind:null),'kernel '+label+' '+stage+' '+ns+' '+actor+' '+kind);
      const sync=present&&completed.includes('dedicated')?secret+'|'+ns+'|sync':null;
      check(values[ns].sync===sync,'kernel sync bytes');
      if(stage==='dedicated')check(values[ns].syncRead===sync&&values[ns].syncSize===(sync?new TextEncoder().encode(sync).length:0),'kernel sync native read/size');
    }
  }
}
const proof={mode,scope,phase,marker:secret};
const bucketDevtools=await inspectBucketDevtools(main,secret,mode);
if(mode==='memory'||mode==='persistent'){
  const before=await embedded(main,'scalar-read');check(before.local===null&&before.session===null,'process restart retained scalar');
  const empty=await embedded(main,'data-read');dataValues(empty,false,'empty');
  const networkBefore=await embedded(main,'network-read');check(!networkBefore.cookie&&!networkBefore.echo.cookie,'process restart retained cookies');
  const workerBefore=await embedded(main,'sw-read'),peerWorkerBefore=await embedded(peer,'sw-read');
  check(workerBefore===null&&peerWorkerBefore===null,'process restart retained embedded service worker');
  const scalar=await embedded(main,'scalar-write');check(scalar.local===secret&&scalar.session===secret,'kernel scalar positive');
  const other=await embedded(peer,'scalar-read');check(other.local===(scope==='per-outer-site'?secret:null)&&other.session===null,'kernel scalar sharing');
  const data=await embedded(main,'data-write');dataValues(data,true,'writer');
  const shared=await embedded(peer,'data-read');dataValues(shared,scope==='per-outer-site','peer');
  const network=await embedded(main,'network-write');check(network.cookie.includes('embedded_js='+secret)&&!network.cookie.includes('embedded_http='),'kernel JS cookie positive/HttpOnly');
  check(network.echo.cookie.includes('embedded_js='+secret)&&network.echo.cookie.includes('embedded_http='+secret),'kernel HTTP cookies positive');
  for(const sample of [networkBefore,network])check(sample.cache.length===2&&sample.cache.every(v=>v.marker===secret)&&
    sample.cache[0].cacheToken===sample.cache[1].cacheToken,'kernel positive HTTP cache consumer');
  const worker=await embedded(main,'sw-write'),workerRepeat=await embedded(main,'sw-read'),workerPeer=await embedded(peer,'sw-read');
  check(worker.marker===secret&&worker.scope===embeddedBase+'/','kernel native service-worker activation/message');
  check(JSON.stringify(workerRepeat)===JSON.stringify(worker),'kernel native service-worker repeat');
  check(JSON.stringify(workerPeer)===JSON.stringify(scope==='per-outer-site'?worker:null),'kernel native service-worker sharing');
  proof.observations={before,empty,scalar,other,data,shared,networkBefore,network,bucketDevtools,
    serviceWorker:{before:workerBefore,peerBefore:peerWorkerBefore,original:worker,repeat:workerRepeat,peer:workerPeer}};
  if(ordinary)proof.observations.ordinary=ordinary;
  const history=await committedHistory(main),historyBefore=history.entries[history.currentIndex];
  const originalURL=base+'/page/kernel-'+phase+'-0',awayURL=embeddedBase+'/page/outer-away-'+phase;
  check(historyBefore.url===originalURL,'native move source entry differs');
  const awayNavigation=await main.send('Page.navigate',{url:awayURL});check(!awayNavigation.errorText,'native outer move refused');
  await committedHistory(main,awayURL);
  await until(async()=>await value(main,`document.readyState==='complete'&&location.href===${JSON.stringify(awayURL)}&&!!window.docNonce`),'real first-party away document');
  const awayMarker='ORDINARY_AWAY_'+secret.slice(-32),awayBefore=await firstPartyState(main,awayMarker,false);
  const realPersisted=mode==='persistent'&&phase==='reader';
  check(awayBefore.workers.length===0,'temporary SW appeared in genuine first-party inventory');
  check(awayBefore.local===(realPersisted?awayMarker:null)&&awayBefore.session===null,'temporary state imported into first-party/real restart failed');
  for(const text of [awayBefore.cookie,awayBefore.httpCookie]){
    check(!text.includes('embedded_http=')&&!text.includes('embedded_js='+secret),'temporary cookie imported into first-party');
    check(text.includes('embedded_js='+awayMarker)===realPersisted,'real first-party cookie restart control');
  }
  const awayAfter=await firstPartyState(main,awayMarker,true);
  check(awayAfter.workers.length===0,'real-only write promoted a temporary SW registration');
  check(awayAfter.local===awayMarker&&awayAfter.session===awayMarker&&awayAfter.cookie.includes('embedded_js='+awayMarker)&&awayAfter.httpCookie.includes('embedded_js='+awayMarker),'real first-party positive control');
  const peerWhileAway=await embeddedSnapshot(peer),scopeShared=scope==='per-outer-site';
  check(peerWhileAway.scalar.local===(scopeShared?secret:null)&&peerWhileAway.scalar.session===null,'outer move changed unaffected peer scalar');
  dataValues(peerWhileAway.data,scopeShared,'peer');cookieValues(peerWhileAway.network,scopeShared,'unmoved peer');
  check((peerWhileAway.network.cache[0].cacheToken===network.cache[0].cacheToken)===scopeShared,'outer move peer native cache isolation');
  check(JSON.stringify(peerWhileAway.worker)===JSON.stringify(scopeShared?worker:null),'outer move changed unaffected peer SW');
  await main.send('Page.navigateToHistoryEntry',{entryId:historyBefore.id});
  const back=await committedHistory(main,originalURL),historyAfter=back.entries[back.currentIndex];
  check(historyAfter.id===historyBefore.id,'Back replaced the original native entry');
  await until(async()=>await value(main,`document.readyState==='complete'&&location.href===${JSON.stringify(originalURL)}&&!!window.docNonce`),'native Back restored document');
  const returned=await embeddedSnapshot(main);
  check(returned.scalar.local===(scopeShared?secret:null)&&returned.scalar.session===(scopeShared?secret:null),'Back scope scalar/session contract');
  dataValues(returned.data,scopeShared,'peer');cookieValues(returned.network,scopeShared,'Back');
  check(JSON.stringify(returned.worker)===JSON.stringify(scopeShared?worker:null),'Back scope SW contract');
  check((returned.network.cache[0].cacheToken===network.cache[0].cacheToken)===scopeShared,'Back native cache scope contract');
  let repopulated=null;
  if(!scopeShared){
    repopulated={scalar:await embedded(main,'scalar-write'),data:await embedded(main,'data-write'),network:await embedded(main,'network-write'),worker:await embedded(main,'sw-write')};
    check(repopulated.scalar.local===secret&&repopulated.scalar.session===secret,'fresh Back scope unusable');
    dataValues(repopulated.data,true,'writer');cookieValues(repopulated.network,true,'fresh Back scope');
    check(repopulated.worker.marker===secret&&repopulated.worker.scope===embeddedBase+'/','fresh Back SW unusable');
  }
  let ordinaryAfterBack=null;
  if(ordinary){
    ordinaryAfterBack={...await value(main,"({local:localStorage.getItem('ordinary-control'),cookie:document.cookie})"),worker:await ordinaryWorker(main,false),cdnToken:await ordinaryCdn(main)};
    check(ordinaryAfterBack.local===ordinaryMarker&&ordinaryAfterBack.cookie.includes('ordinary_control='+ordinaryMarker),'Back lost top-level ordinary state');
    check(JSON.stringify(ordinaryAfterBack.worker)===JSON.stringify(ordinary.serviceWorker.original)&&ordinaryAfterBack.cdnToken===ordinary.cdnToken,'Back lost ordinary SW/R1 CDN');
  }
  proof.observations.moveBack={originalURL,awayURL,historyBefore,historyAfter,awayBefore,awayAfter,peerWhileAway,returned,repopulated,ordinaryAfterBack};
}else{
  const denied=await embedded(main,'admission-read');check(denied.cookie==='','stateless cookie admitted');
  for(const kind of ['localStorage','sessionStorage','idb','cache','opfs','bucket','bucketKeys','bucketDelete'])check(denied[kind]&&denied[kind]!=='admitted','stateless '+kind+' admitted');
  check(denied.serviceWorker?.api===true,'stateless native SW API missing');
  for(const operation of ['register','single','inventory']){
    const result=denied.serviceWorker[operation];
    check(result.status==='rejected'&&['NotSupportedError','InvalidStateError','SecurityError'].includes(result.name)&&result.message,'stateless native SW '+operation+' '+JSON.stringify(result));
  }
  proof.observations={denied,bucketDevtools};
}
console.log(JSON.stringify({phase:'live',proof}));
const input=readline.createInterface({input:process.stdin});
for await(const line of input){check(line==='close','invalid kernel close barrier');break;}
input.close();process.stdin.pause();
for(let n=0;n<2;n++){
  clients[n].ws.close();
  const result=await fetch(`http://127.0.0.1:${port}/json/close/${targets[n].id}`);check(result.ok,'kernel native close refused');
}
console.log(JSON.stringify({phase:'closed'}));
