# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Tracked storage and initial network consumer slice, not full6E proof."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SCOPES = ('per-tab', 'per-outer-site')


def frame_script():
    return r'''
addEventListener('message',async event=>{
  const outer=location.protocol+'//127.0.0.1:'+location.port;
  if(event.source!==parent||event.origin!==outer)return;
  const {serial,action,marker}=event.data||{};
  if(typeof serial!=='string'||typeof action!=='string'||typeof marker!=='string')return;
  try{
''' + (ROOT/'profile_embedded_data.js').read_text() + r'''
    if(action==='media-read'){
      const stream=await navigator.mediaDevices.getUserMedia({video:true});
      try{
        const track=stream.getVideoTracks()[0],settings=track.getSettings();
        const devices=await navigator.mediaDevices.enumerateDevices();
        event.source.postMessage({serial,result:{id:settings.deviceId,group:settings.groupId,
          label:track.label,state:track.readyState,
          devices:devices.filter(d=>d.kind==='videoinput').map(d=>({id:d.deviceId,group:d.groupId,label:d.label}))}},event.origin);
      }finally{stream.getTracks().forEach(t=>t.stop())}
      return;
    }
    if(action==='sw-write'||action==='sw-read'){
      let registration;
      if(action==='sw-write'){
        registration=await navigator.serviceWorker.register('/embedded-sw.js?marker='+encodeURIComponent(marker),{scope:'/'});
        const worker=registration.installing||registration.waiting||registration.active;
        if(!worker)throw Error('native service worker missing');
        if(worker.state!=='activated')await new Promise((resolve,reject)=>{
          const update=()=>{if(worker.state==='activated')resolve();else if(worker.state==='redundant')reject(Error('native service worker redundant'))};
          worker.addEventListener('statechange',update);update();
        });
      }else{
        const registrations=await navigator.serviceWorker.getRegistrations();
        if(registrations.length>1)throw Error('unexpected scoped service-worker inventory');
        registration=registrations[0];
      }
      let result=null;
      if(registration){
        if(!registration.active||registration.active.state!=='activated')throw Error('native service worker not active');
        result=await new Promise((resolve,reject)=>{
          const channel=new MessageChannel();
          channel.port1.onmessage=event=>{channel.port1.close();resolve(event.data)};
          channel.port1.onmessageerror=()=>{channel.port1.close();reject(Error('native service worker message error'))};
          registration.active.postMessage('scope-value',[channel.port2]);
        });
      }
      event.source.postMessage({serial,result},event.origin);return;
    }
    if(action==='admission-read'){
      const denied={};
      const serviceWorker=navigator.serviceWorker;
      if(!serviceWorker||typeof serviceWorker.register!=='function'||typeof serviceWorker.getRegistration!=='function'||typeof serviceWorker.getRegistrations!=='function')throw Error('native service-worker API missing');
      const nativeRejection=async operation=>{
        try{await operation();return {status:'admitted',name:'',message:''}}
        catch(error){
          if(!(error instanceof DOMException)||!['NotSupportedError','InvalidStateError','SecurityError'].includes(error.name))throw error;
          return {status:'rejected',name:error.name,message:error.message};
        }
      };
      denied.serviceWorker={api:true,
        register:await nativeRejection(()=>serviceWorker.register('/embedded-sw.js?marker='+encodeURIComponent(marker),{scope:'/'})),
        single:await nativeRejection(()=>serviceWorker.getRegistration('/')),
        inventory:await nativeRejection(()=>serviceWorker.getRegistrations())};
      document.cookie='kernel_probe='+marker+'; Path=/; SameSite=None; Secure';
      denied.cookie=document.cookie;
      for(const key of ['localStorage','sessionStorage']){
        try{window[key].setItem('kernel_probe',marker);denied[key]='admitted'}catch(error){denied[key]=error.name}
      }
      try{await new Promise((resolve,reject)=>{const r=indexedDB.open('kernel-probe');
        r.onsuccess=()=>{r.result.close();resolve()};r.onerror=()=>reject(r.error)});denied.idb='admitted'}catch(error){denied.idb=error.name}
      try{await caches.open('kernel-probe');denied.cache='admitted'}catch(error){denied.cache=error.name}
      try{await navigator.storage.getDirectory();denied.opfs='admitted'}catch(error){denied.opfs=error.name}
      try{await navigator.storageBuckets.open('kernel-probe');denied.bucket='admitted'}catch(error){denied.bucket=error.name}
      try{await navigator.storageBuckets.keys();denied.bucketKeys='admitted'}catch(error){denied.bucketKeys=error.name}
      try{await navigator.storageBuckets.delete('kernel-probe');denied.bucketDelete='admitted'}catch(error){denied.bucketDelete=error.name}
      event.source.postMessage({serial,result:denied},event.origin);return;
    }
    if(action==='trial-read'){
      event.source.postMessage({serial,result:{url:location.href,secure:isSecureContext}},event.origin);
      return;
    }
    if(action==='network-write'||action==='network-read'){
      const query='?marker='+encodeURIComponent(marker);
      if(action==='network-write'){
        document.cookie='embedded_js='+marker+'; Path=/; Secure; SameSite=None; Max-Age=31536000';
        const set=await fetch('/embedded-cookie-write'+query,{credentials:'include',cache:'no-store'});
        if(!set.ok)throw Error('native HTTP cookie fixture '+set.status);
        await set.json();
      }
      const echo=await(await fetch('/embedded-network-echo'+query,
        {credentials:'include',cache:'no-store'})).json();
      const cache=[];
      for(let n=0;n<2;n++)cache.push(await(await fetch('/embedded-http-cache'+query,
        {credentials:'include',cache:'default'})).json());
      event.source.postMessage({serial,result:{cookie:document.cookie,echo,cache}},event.origin);
      return;
    }
    if(action==='scalar-write'){
      localStorage.setItem('embedded-storage',marker);
      sessionStorage.setItem('embedded-storage',marker);
    }else if(action!=='scalar-read')throw Error('unsupported embedded storage action');
    event.source.postMessage({serial,result:{local:localStorage.getItem('embedded-storage'),
      session:sessionStorage.getItem('embedded-storage')}},event.origin);
  }catch(error){event.source.postMessage({serial,error:String(error)},event.origin)}
});
'''


def frame_body():
    return ('<!doctype html><meta charset="utf-8"><script>' + frame_script() +
            '</script>').encode()


def build_drive(helpers, scope):
    assert scope in SCOPES
    from profile_memory import DRIVE
    fixture = (ROOT/'profile_embedded_frame.mjs').read_text()
    fragment = r'''
const embeddedScope=__SCOPE__;
const embeddedURL=new URL(base);embeddedURL.hostname='localhost';
const embeddedBase=embeddedURL.origin;
''' + fixture + (ROOT/'profile_origin_trials.mjs').read_text() + r'''
function embeddedDataCheck(sample,present,label,allStages=false){
  const actors=['frame','dedicated','shared'];
  check(JSON.stringify(Object.keys(sample).sort())===JSON.stringify(['dedicated','frame','reread','shared']),
    'embedded storage stage inventory '+label);
  for(const [stage,value] of Object.entries(sample)){
    const completed=allStages?actors:stage==='frame'?['frame']:stage==='dedicated'?['frame','dedicated']:actors;
    bucketMetadataCheck(value.named.metadata,secret,present,completed);
    if(present&&!allStages&&stage!=='reread')bucketRetirementCheck(value.named.retirement,secret,stage);
    else check(value.named.retirement===undefined,'unexpected bucket retirement write');
    for(const namespace of ['default','named']){
      for(const kind of ['idb','cache','opfs'])for(const actor of actors){
        const expected=present&&completed.includes(actor)?secret+'|'+namespace+'|'+actor+'|'+kind:null;
        check(value[namespace][kind][actor]===expected,
          'embedded storage '+label+' '+stage+' '+namespace+' '+kind+' '+actor+' '+JSON.stringify(value));
      }
      const expectedSync=present&&completed.includes('dedicated')?secret+'|'+namespace+'|sync':null;
      check(value[namespace].sync===expectedSync,'embedded sync '+label+' '+stage);
      if(stage==='dedicated'){
        check(value[namespace].syncRead===expectedSync,'embedded native sync readback '+label);
        check(value[namespace].syncSize===(expectedSync?new TextEncoder().encode(expectedSync).length:0),
          'embedded native sync size '+label);
      }
    }
  }
}
const scalarOriginal=await embedded(main,'scalar-write');
const scalarPeer=await embedded(shared,'scalar-read');
check(scalarOriginal.local===secret&&scalarOriginal.session===secret,'embedded scalar writer');
check(scalarPeer.local===(embeddedScope==='per-tab'?null:secret)&&scalarPeer.session===null,
  'embedded scalar peer '+embeddedScope+' '+JSON.stringify(scalarPeer));
pass('embedded-storage-local-session-scope');
const dataOriginal=await embedded(main,'data-write');
const dataPeer=await embedded(shared,'data-read');
embeddedDataCheck(dataOriginal,true,'writer');
embeddedDataCheck(dataPeer,embeddedScope==='per-outer-site','peer',true);
check((await nativeEvents()).filter(e=>e.event==='embedded-scope-configured').length===1,
  'native embedded scope was not configured once before storage');
check((await nativeEvents()).some(e=>e.event==='embedded-scope-reused'),'sharing alias scope not reused');
pass('embedded-storage-default-named-frame-worker-values');
pass('embedded-storage-dedicated-sync-write-read-truncate-flush');
proof.observations.bucketDevtools=await inspectBucketDevtools(main,secret,'memory');
proof.observations.embeddedStorage={scope:embeddedScope,scalarOriginal,scalarPeer,dataOriginal,dataPeer,
  storageOnly:true,allModeKernelProof:false,fullItem6EGate:false};
function embeddedNetworkCheck(sample,present,label){
  const parse=value=>new Map(value.split(';').map(v=>v.trim()).filter(Boolean).map(v=>{
    const split=v.indexOf('=');return[v.slice(0,split),v.slice(split+1)];}));
  const js=parse(sample.cookie),http=parse(sample.echo.cookie);
  const wanted=present?secret:null;
  check((js.get('embedded_js')??null)===wanted,'embedded JS cookie '+label+' '+JSON.stringify(sample));
  check(!js.has('embedded_http'),'HttpOnly cookie exposed to script '+label);
  for(const name of ['embedded_js','embedded_http'])
    check((http.get(name)??null)===wanted,'embedded HTTP cookie '+label+' '+name+' '+JSON.stringify(sample));
  check(sample.cache.length===2&&sample.cache.every(v=>v.marker===secret&&/^[a-f0-9]{32}$/.test(v.cacheToken)),
    'embedded HTTP cache actual response '+label);
  check(sample.cache[0].cacheToken===sample.cache[1].cacheToken,'embedded HTTP cache missed repeated read '+label);
}
const networkOriginal=await embedded(main,'network-write');
const networkPeer=await embedded(shared,'network-read');
embeddedNetworkCheck(networkOriginal,true,'writer');
embeddedNetworkCheck(networkPeer,embeddedScope==='per-outer-site','peer');
check((networkOriginal.cache[0].cacheToken===networkPeer.cache[0].cacheToken)===(embeddedScope==='per-outer-site'),
  'embedded native HTTP cache sharing '+embeddedScope);
proof.observations.embeddedNetwork={scope:embeddedScope,original:networkOriginal,peer:networkPeer,
  allModeKernelProof:false,fullItem6EGate:false};
pass('embedded-network-http-js-cookie-scope');
pass('embedded-network-http-cache-scope');
const mediaOriginal=await embedded(main,'media-read');
const mediaRepeat=await embedded(main,'media-read');
const mediaPeer=await embedded(shared,'media-read');
function embeddedMediaCheck(sample,label){
  check(sample.id&&sample.group&&sample.label&&sample.state==='live','native embedded media '+label);
  check(sample.devices.some(v=>v.id===sample.id&&v.label),'native enumerated media identity '+label);
}
for(const [label,sample] of Object.entries({original:mediaOriginal,repeat:mediaRepeat,peer:mediaPeer}))embeddedMediaCheck(sample,label);
check(mediaOriginal.id===mediaRepeat.id&&mediaOriginal.group===mediaRepeat.group,'native embedded media unstable');
check((mediaOriginal.id===mediaPeer.id)===(embeddedScope==='per-outer-site'),'native media scope sharing');
check(mediaOriginal.group!==mediaPeer.group,'media group identity not document-specific');
proof.observations.embeddedMedia={scope:embeddedScope,original:mediaOriginal,repeat:mediaRepeat,peer:mediaPeer};
pass('embedded-media-native-device-id-scope');
const swOriginal=await embedded(main,'sw-write');
const swPeer=await embedded(shared,'sw-read');
check(swOriginal?.marker===secret&&swOriginal.scope===embeddedBase+'/','native service-worker response');
check(embeddedScope==='per-outer-site'?swPeer?.marker===secret:swPeer===null,'native service-worker scope sharing');
proof.observations.embeddedServiceWorker={scope:embeddedScope,original:swOriginal,peer:swPeer};
pass('embedded-service-worker-native-message-scope');
'''
    fragment = fragment.replace('__SCOPE__', json.dumps(scope))
    anchor = "await navigate(isolated.c,'memory-independent');"
    assert DRIVE.count(anchor) == 1
    trial_fragment = r'''
const ordinaryTrialInvalid=await ordinaryTrial(isolated.c,'ordinary-invalid','invalid');
const ordinaryTrialHeader=await ordinaryTrial(isolated.c,'ordinary-header');
const ordinaryTrialRepeat=await ordinaryTrial(isolated.c,'ordinary-repeat');
const ordinaryTrialScript=await ordinaryTrial(isolated.c,'ordinary-script','script');
const ordinaryTrialAfterScript=await ordinaryTrial(isolated.c,'ordinary-after-script');
const trialOriginal=await embeddedTrial(main,'original');
const trialPeer=await embeddedTrial(shared,'peer');
const trialRepeat=await embeddedTrial(main,'original-repeat');
const trialScript=await embeddedTrial(main,'script','script');
const trialAfterScript=await embeddedTrial(main,'after-script');
proof.observations.embeddedOriginTrials={scope:embeddedScope,
  ordinary:{invalid:ordinaryTrialInvalid,header:ordinaryTrialHeader,repeat:ordinaryTrialRepeat,
    script:ordinaryTrialScript,afterScript:ordinaryTrialAfterScript},
  original:trialOriginal,peer:trialPeer,repeat:trialRepeat,script:trialScript,afterScript:trialAfterScript};
'''
    drive_trials = DRIVE.replace(anchor, trial_fragment+'\n'+anchor)
    anchor = "let isolated=await peer(main,targets,'memory-peer');"
    assert DRIVE.count(anchor) == 1
    drive = drive_trials.replace(anchor, fragment+'\n'+anchor)
    anchor = 'const recreated=await read(main);empty(recreated);'
    assert drive.count(anchor) == 1
    drive = drive.replace(anchor, anchor+r'''
proof.observations.embeddedOriginTrials.fresh=await embeddedTrial(main,'fresh');
const freshMedia=await embedded(main,'media-read');embeddedMediaCheck(freshMedia,'fresh');
check(![mediaOriginal.id,mediaPeer.id,proof.observations.embeddedMedia.held.id].includes(freshMedia.id),'fresh context retained media identity');
proof.observations.embeddedMedia.fresh=freshMedia;
pass('embedded-media-native-retirement');
const freshWorker=await embedded(main,'sw-read');check(freshWorker===null,'fresh context retained service worker');
proof.observations.embeddedServiceWorker.fresh=freshWorker;
pass('embedded-service-worker-native-retirement');
const embeddedFreshScalar=await embedded(main,'scalar-read');
check(embeddedFreshScalar.local===null&&embeddedFreshScalar.session===null,'fresh embedded scalar retained old data');
const embeddedFreshData=await embedded(main,'data-read');
embeddedDataCheck(embeddedFreshData,false,'fresh-context');
proof.observations.embeddedStorage.fresh={scalar:embeddedFreshScalar,data:embeddedFreshData};
pass('embedded-storage-empty-after-native-context-retirement');
const embeddedFreshNetwork=await embedded(main,'network-read');
embeddedNetworkCheck(embeddedFreshNetwork,false,'fresh-context');
check(![networkOriginal,networkPeer,proof.observations.embeddedNetwork.held].some(
  v=>v.cache[0].cacheToken===embeddedFreshNetwork.cache[0].cacheToken),
  'fresh native context inherited embedded HTTP cache');
proof.observations.embeddedNetwork.fresh=embeddedFreshNetwork;
pass('embedded-network-empty-after-native-context-retirement');
''')
    anchor = "for(let i=0;i<targets.length;i++)await close(targets[i],connections[i]);\nawait control(parent,'open-tab-held');"
    assert drive.count(anchor) == 1
    drive = drive.replace(anchor, anchor.replace("await control(parent,'open-tab-held');",
        "const embeddedLastTabClosed=performance.now();\nawait control(parent,'open-tab-held');"))
    anchor = "await navigate(held,'memory-held-reference');retained(await read(held),secret);"
    assert drive.count(anchor) == 1
    drive = drive.replace(anchor, anchor+r'''
check(performance.now()-embeddedLastTabClosed<30000,'fixture missed native outer-scope keepalive window');
proof.observations.embeddedOriginTrials.held=await embeddedTrial(held,'held');
const heldMedia=await embedded(held,'media-read');embeddedMediaCheck(heldMedia,'held');
check((heldMedia.id===mediaOriginal.id)===(embeddedScope==='per-outer-site'),'media tab-close scope');
check(embeddedScope==='per-outer-site'||heldMedia.id!==mediaPeer.id,'media new tab inherited peer');
proof.observations.embeddedMedia.held=heldMedia;
const heldWorker=await embedded(held,'sw-read');
check(embeddedScope==='per-outer-site'?heldWorker?.marker===secret:heldWorker===null,'service-worker tab-close scope');
proof.observations.embeddedServiceWorker.held=heldWorker;
const embeddedHeldNetwork=await embedded(held,'network-read');
embeddedNetworkCheck(embeddedHeldNetwork,embeddedScope==='per-outer-site','held-tab');
check((embeddedHeldNetwork.cache[0].cacheToken===networkOriginal.cache[0].cacheToken)===(embeddedScope==='per-outer-site'),
  'embedded HTTP cache tab-close lifetime '+embeddedScope);
check(embeddedScope==='per-outer-site'||embeddedHeldNetwork.cache[0].cacheToken!==networkPeer.cache[0].cacheToken,
  'new tab inherited a peer temporary cache');
proof.observations.embeddedNetwork.held=embeddedHeldNetwork;
pass('embedded-network-tab-close-scope');
''')
    assert drive.count('__CDP_HELPERS__') == 1
    anchor = "await close(heldTarget,held);\nawait control(parent,'dispose-stateless');"
    assert drive.count(anchor) == 1
    expiry = r'''
const expiryStart=performance.now(),expiryWait=embeddedScope==='per-outer-site'?31000:0;
if(expiryWait)await sleep(expiryWait);
const expiryElapsed=performance.now()-expiryStart;
check(expiryElapsed>=expiryWait,'native keepalive deadline not reached');
await control(parent,'open-tab-expired');
const expiredTarget=await until(async()=>{const t=await guests();return t.length===1?t[0]:null},'native tab after scope expiry');
const expired=await connect(expiredTarget);await expired.send('Page.enable');
await navigate(expired,'memory-after-scope-expiry');
const expiryTop=await read(expired);retained(expiryTop,secret);
check(expiryTop.session===null,'new top-level tab inherited sessionStorage');
pass('embedded-scope-expiry-preserves-top-context');
const expiryScalar=await embedded(expired,'scalar-read');
check(expiryScalar.local===null&&expiryScalar.session===null,'scope close/expiry retained scalar');
const expiryData=await embedded(expired,'data-read');embeddedDataCheck(expiryData,false,'native-expiry');
const expiryNetwork=await embedded(expired,'network-read');embeddedNetworkCheck(expiryNetwork,false,'native-expiry');
check(![networkOriginal,networkPeer,proof.observations.embeddedNetwork.held].some(
  value=>value.cache[0].cacheToken===expiryNetwork.cache[0].cacheToken),'native expiry retained HTTP cache');
const expiryMedia=await embedded(expired,'media-read');embeddedMediaCheck(expiryMedia,'native-expiry');
const expiryMediaRepeat=await embedded(expired,'media-read');embeddedMediaCheck(expiryMediaRepeat,'native-expiry-repeat');
check(![mediaOriginal.id,mediaPeer.id,proof.observations.embeddedMedia.held.id].includes(expiryMedia.id),'native expiry retained media identity');
check(expiryMedia.id===expiryMediaRepeat.id&&expiryMedia.group===expiryMediaRepeat.group,'native expiry media unstable');
const expiryWorker=await embedded(expired,'sw-read');check(expiryWorker===null,'native expiry retained service worker');
const expiryTrial=await embeddedTrial(expired,'expired'),expiryTrialRepeat=await embeddedTrial(expired,'expired-repeat');
const expiryScalarAfter=await embedded(expired,'scalar-write');
check(expiryScalarAfter.local===secret&&expiryScalarAfter.session===secret,'native expired scope not usable');
const expiryDataAfter=await embedded(expired,'data-write');embeddedDataCheck(expiryDataAfter,true,'native-expiry-positive');
const expiryWorkerAfter=await embedded(expired,'sw-write');
check(expiryWorkerAfter?.marker===secret&&expiryWorkerAfter.scope===embeddedBase+'/','native expired SW not usable');
proof.observations.embeddedExpiry={scope:embeddedScope,waitMilliseconds:expiryWait,elapsedMilliseconds:expiryElapsed,
  top:expiryTop,scalarBefore:expiryScalar,dataBefore:expiryData,networkBefore:expiryNetwork,
  media:expiryMedia,mediaRepeat:expiryMediaRepeat,workerBefore:expiryWorker,trial:expiryTrial,trialRepeat:expiryTrialRepeat,
  scalarAfter:expiryScalarAfter,dataAfter:expiryDataAfter,workerAfter:expiryWorkerAfter};
pass('embedded-scope-native-close-or-expiry');
await close(expiredTarget,expired);
'''
    drive = drive.replace(anchor, "await close(heldTarget,held);\n" + expiry +
                          "await control(parent,'dispose-stateless');")
    return drive.replace('__CDP_HELPERS__', helpers)


def build_scenario(helpers, scope):
    from profile_memory import build_scenario as memory_scenario
    from profile_browser_lifecycle import make_scenario
    plan = memory_scenario(helpers)['browser_lifecycle']
    restart, = [index for index, step in enumerate(plan)
                if step['command'] == 'memory-restart']
    plan.insert(restart, dict(command='open-tab-expired', count=1))
    return make_scenario(build_drive(helpers, scope),
                         [(step['command'], step['count']) for step in plan])
