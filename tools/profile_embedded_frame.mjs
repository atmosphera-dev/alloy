// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// This ordinary page function is serialized with explicit arguments. Its body
// contains no nested template-language substitutions or hand-written IIFE tail.
import {appendFileSync as appendRetainedCacheOutcome} from 'node:fs';

function recordRetainedCacheOutcomes(data){
  const path=eventLog.replace(/stdio\.log$/,'retained-cache-outcomes.jsonl');
  if(path===eventLog)throw Error('retained-cache outcome path requires stdio.log');
  for(const [stage,value] of Object.entries(data)){
    const retirement=value?.named?.retirement,cache=retirement?.cacheRetirement;
    if(!cache)continue;
    const identity={stage,actor:retirement.actor,bucket:retirement.name,marker:secret};
    appendRetainedCacheOutcome(path,JSON.stringify({...identity,phase:'controls',result:cache})+'\n');
    for(const phase of ['afterDelete','afterReopen'])
      for(const operation of ['cacheMatch','cacheMatchAll','cacheKeys','cachePut','cacheDelete',
        'storageMatch','storageKeys','storageHas','storageOpen','storageDelete'])
        appendRetainedCacheOutcome(path,JSON.stringify({...identity,phase,operation,result:cache[phase][operation]})+'\n');
  }
}

async function callEmbeddedFrame(source,action,marker,navigation=null){
  const slot=Symbol.for('alloy.test.embedded-storage-fixture');
  let fixture=globalThis[slot];
  if(!fixture||!fixture.element.isConnected){
    const element=document.createElement('iframe');
    element.id='embedded-baseline';
    element.allow='camera';
    const ready=new Promise((resolve,reject)=>{
      element.onload=()=>resolve(true);
      element.onerror=()=>reject(Error('embedded fixture navigation failed'));
    });
    fixture={element,ready};globalThis[slot]=fixture;
    element.src=source+(navigation??'/embedded-storage-frame');document.body.append(element);
  }
  if(navigation!==null&&fixture.element.src!==source+navigation){
    if(!navigation.startsWith('/embedded-origin-trial?'))throw Error('invalid trial fixture path');
    fixture.ready=new Promise((resolve,reject)=>{
      fixture.element.onload=()=>resolve(true);
      fixture.element.onerror=()=>reject(Error('trial fixture navigation failed'));
    });
    fixture.element.src=source+navigation;
  }
  await fixture.ready;
  const frame=fixture.element;
  if(!frame.isConnected||!frame.contentWindow)throw Error('embedded fixture detached before call');
  return new Promise((resolve,reject)=>{
    const serial=crypto.randomUUID();
    const timeout=setTimeout(()=>{removeEventListener('message',receive);reject(Error('embedded reply timeout'))},30000);
    function receive(event){
      if(event.source!==frame.contentWindow||event.origin!==source||event.data.serial!==serial)return;
      clearTimeout(timeout);removeEventListener('message',receive);
      event.data.error?reject(Error(event.data.error)):resolve(event.data.result);
    }
    addEventListener('message',receive);
    frame.contentWindow.postMessage({serial,action,marker},source);
  });
}

async function embedded(c,action){
  const args=[embeddedBase,action,secret].map(value=>JSON.stringify(value)).join(',');
  const result=await value(c,`(${callEmbeddedFrame.toString()})(${args})`);
  if(action==='data-write')recordRetainedCacheOutcomes(result);
  return result;
}

function bucketMetadataCheck(metadata,marker,present,completed){
  const name='scope-'+marker.slice(-16);
  const expires=1893456000000+parseInt(marker.slice(-6),16)*10+completed.length-1;
  check(metadata&&metadata.name===name,'native bucket metadata name');
  check(metadata.expires===(present?expires:null),'native bucket expiry value '+JSON.stringify(metadata));
  check(metadata.persisted===false,'temporary bucket persisted');
  check(Number.isFinite(metadata.usage)&&metadata.usage>=0&&Number.isFinite(metadata.quota)&&metadata.quota>0,
    'native bucket quota estimate '+JSON.stringify(metadata));
  check(JSON.stringify(metadata.keys)===JSON.stringify([name]),'native bucket inventory '+JSON.stringify(metadata));
}

function bucketRetirementCheck(result,marker,actor){
  const scopeName='scope-'+marker.slice(-16),name='retired-'+marker.slice(-16)+'-'+actor;
  const expires=1893456000000+parseInt(marker.slice(-6),16)*10;
  const payload=marker+'|retired|'+actor;
  check(result&&result.actor===actor&&result.name===name,'native bucket retirement identity');
  check(result.beforeBytes===payload&&result.beforeExpires===expires,'native bucket retirement positive control');
  const rejected=(value,names,operation='bucket')=>check(value?.status==='rejected'&&names.includes(value.name)&&typeof value.message==='string',
    'native deleted-bucket rejection '+actor+' '+operation+' '+JSON.stringify(value));
  for(const operation of ['persisted','estimate','expires','setExpires'])rejected(result.denied[operation],['UnknownError']);
  rejected(result.denied.directory,['SecurityError','InvalidStateError']);
  rejected(result.staleMutation,['UnknownError']);
  const files=result.fileRetirement;
  check(files&&files.positiveRetainedBytes===payload&&files.freshBytesAfterStale===payload+'|fresh',
    'native retained/recreated file bytes');
  for(const operation of ['directoryAfterDelete','fileAfterDelete','fileAfterReopen'])
    rejected(files[operation],['NotFoundError','SecurityError','InvalidStateError','NotReadableError']);
  const idb=result.idbRetirement;
  check(idb&&idb.beforeValue===payload+'|idb'&&idb.freshBefore===null&&idb.freshValue===payload+'|idb|fresh'&&
    idb.freshAfterStale===idb.freshValue,'native retained/recreated IndexedDB values');
  for(const upgrade of [idb.originalUpgrade,idb.freshUpgrade])
    check(upgrade?.oldVersion===0&&upgrade.newVersion===1,'native IDB recreation metadata');
  check(idb.closeEvent?.type==='close'&&idb.closeEvent.trusted===true&&idb.closeEvents.length===1&&
    idb.closeEvents[0].type==='close'&&idb.closeEvents[0].trusted===true,'native IDB forced-close event');
  for(const refusal of [idb.transactionAfterDelete,idb.transactionAfterReopen])
    check(refusal.status==='threw'&&refusal.name==='InvalidStateError'&&typeof refusal.message==='string',
      'old native IndexedDB transaction admitted '+JSON.stringify(refusal));
  const factory=result.idbFactoryRetirement;
  check(factory&&factory.freshValueAfterFactoryProbes===payload+'|idb|fresh','stale IDB factory changed fresh data');
  for(const [key,action] of [['openAfterDelete','open'],['deleteAfterDelete','delete'],['openAfterReopen','open'],['deleteAfterReopen','delete']]){
    const response=factory[key];
    check(response?.status==='error'&&response.action===action&&response.event==='error'&&response.trusted===true&&
      ['UnknownError','AbortError'].includes(response.name)&&typeof response.message==='string'&&response.upgrades.length===0,
      'stale native IDB factory request admitted '+JSON.stringify(response));
  }
  for(const key of ['databasesAfterDelete','databasesAfterReopen'])
    check(factory[key]?.status==='rejected'&&factory[key].name==='UnknownError'&&typeof factory[key].message==='string',
      'stale native IDB enumeration admitted '+JSON.stringify(factory[key]));
  const inflight=result.idbInFlightRetirement;
  check(inflight&&inflight.stagedWrite?.type==='success'&&inflight.stagedWrite.trusted===true&&
    inflight.stagedWrite.key==='pending'&&inflight.positivePendingRead?.type==='success'&&
    inflight.positivePendingRead.trusted===true&&inflight.positivePendingRead.value===payload+'|idb',
    'native in-flight transaction positive controls');
  check(Number.isInteger(inflight.pendingBeforeDelete.reads)&&inflight.pendingBeforeDelete.reads>=1&&
    inflight.pendingBeforeDelete.requestState==='pending'&&inflight.pendingBeforeDelete.terminalEvents===0&&
    inflight.pendingBeforeDelete.requestErrors===0,'native transaction not pending at deletion');
  check(inflight.pendingAbort.status==='aborted'&&inflight.pendingAbort.name==='UnknownError'&&
    typeof inflight.pendingAbort.message==='string'&&inflight.pendingEvents.length===1&&
    inflight.pendingEvents[0].type==='abort'&&inflight.pendingEvents[0].trusted===true,
    'native in-flight transaction abort');
  check(inflight.pendingErrors.length===1&&inflight.pendingErrors[0].type==='error'&&
    inflight.pendingErrors[0].trusted===true&&inflight.pendingErrors[0].name==='AbortError'&&
    typeof inflight.pendingErrors[0].message==='string','native pending IDB request error');
  for(const response of [inflight.storeAfterDelete,inflight.storeAfterReopen])
    check(response.status==='threw'&&response.name==='TransactionInactiveError'&&typeof response.message==='string',
      'retained native IDB object store admitted '+JSON.stringify(response));
  check(inflight.freshPending===null,'recreated bucket retained staged IDB write');
  const cache=result.cacheRetirement;
  // Native put/read preservation: WPT service-workers/cache-storage/cache-put.https.any.js
  // (Request and Response from fetch), plus Cache::BarrierCallbackForPutComplete::OnSuccess
  // in third_party/blink/renderer/modules/cache_storage/cache.cc. Values are the exact
  // body/status/header written by nativeBucketRetirement, not native default values.
  const cacheValue=(value,suffix)=>check(value?.missing===false&&value.body===payload+'|cache'+suffix&&
    value.status===201&&value.header===payload+suffix,'native retained cache bytes/metadata '+JSON.stringify(value));
  check(cache&&cache.cacheName==='retained-cache'&&typeof cache.cacheUrl==='string'&&
    new URL(cache.cacheUrl).pathname==='/retained-cache-'+actor,'native cache identity');
  cacheValue(cache.before.cache,'');cacheValue(cache.before.storage,'');
  // Positive inventories: CacheImpl::Keys and CacheStorageImpl::Keys enumerate the
  // actual request URLs/cache names (content/browser/cache_storage/cache_storage_dispatcher_host.cc).
  check(JSON.stringify(cache.before.cacheKeys)===JSON.stringify([cache.cacheUrl])&&
    JSON.stringify(cache.before.storageKeys)==='["retained-cache"]'&&cache.before.has===true,
    'native retained cache inventory positive control');
  for(const [phaseName,phase] of [['afterDelete',cache.afterDelete],['afterReopen',cache.afterReopen]]){
    // Cache.match: dispatcher CacheImpl::Match -> NotFound; Blink Cache::MatchImpl
    // (modules/cache_storage/cache.cc:1004-1008) resolves undefined. Storage.match:
    // CacheStorageImpl::Match -> NotFound; Blink cache_storage.cc:190-207 does likewise.
    check(phase.cacheMatch.missing===true&&phase.storageMatch.missing===true&&
      Object.keys(phase.cacheMatch).length===1&&Object.keys(phase.storageMatch).length===1,
      'deleted native cache returned a response '+actor+' '+phaseName);
    // CacheImpl::MatchAll (dispatcher:318-382) converts NotFound to a successful
    // empty vector; Blink Cache::MatchAllImpl resolves []. WPT cache-matchAll.https.any.js:6-13.
    // cacheOutcome records that array's exact shape without changing the operation.
    const all=phase.cacheMatchAll;
    check(all?.status==='fulfilled'&&Object.keys(all).length===2&&all.value?.type==='array'&&
      Object.keys(all.value).length===3&&all.value.length===0&&Array.isArray(all.value.entries)&&
      all.value.entries.length===0,'native deleted-bucket empty array '+actor+' '+phaseName+'.cacheMatchAll '+JSON.stringify(all));
    // Keys: CacheImpl::Keys propagates NotFound; Cache::KeysImpl rejects it.
    // Put: CacheImpl::Batch propagates NotFound; BarrierCallbackForPutComplete rejects it.
    // Blink modules/cache_storage/cache_storage_error.cc:65-68 maps both to NotFoundError.
    for(const key of ['cacheKeys','cachePut'])rejected(phase[key],['NotFoundError'],phaseName+'.'+key);
    // Named deleted bucket: CacheStorageImpl::GetOrCreateCacheStorage returns null,
    // Has/Open return MakeErrorStorage -> kErrorStorage -> UnknownError (dispatcher
    // :751-800,:916-996; cache_storage.cc:138-172; cache_storage_error.cc:61-64).
    for(const key of ['storageHas','storageOpen'])rejected(phase[key],['UnknownError'],phaseName+'.'+key);
    // Cache.delete: Cache::DeleteImpl (cache.cc:1210-1214) maps NotFound to false.
    // Storage.delete: DeleteComplete (cache_storage.cc:174-187) maps Storage/NotFound
    // to false. Storage.keys: dispatcher CacheStorageImpl::Keys returns an empty vector
    // for the null named-bucket storage; Blink CacheStorage::KeysImpl resolves it.
    check(phase.cacheDelete===false&&phase.storageDelete===false&&Array.isArray(phase.storageKeys)&&
      phase.storageKeys.length===0,'deleted native cache storage mutated or enumerated data '+actor+' '+phaseName);
  }
  // New bucket/cache starts empty, old named bindings cannot rebind to it:
  // CacheStorageImpl::GetOrCreateCacheStorage (dispatcher:1016-1057) and
  // CacheStorageManager::OpenCacheStorage (content/browser/cache_storage/cache_storage_manager.cc).
  check(cache.freshBefore.missing===true&&Object.keys(cache.freshBefore).length===1,
    'fresh native cache retained deleted response');
  cacheValue(cache.freshValue,'|fresh');cacheValue(cache.freshAfterStale.cache,'|fresh');
  cacheValue(cache.freshAfterStale.storage,'|fresh');
  check(cache.freshAfterStale.has===true&&JSON.stringify(cache.freshAfterStale.keys)==='["retained-cache"]',
    'old native cache storage deleted recreated cache');
  // Deletion is bucket-locator scoped: CacheStorageManager::DeleteBucketDataDidGetExists
  // erases that map key only; cache_storage_manager_unittest.cc QuotaDeleteBucketData
  // explicitly preserves the other bucket. Sibling bytes remain the exact fixture input.
  check(cache.siblingBefore===marker+'|named|'+actor+'|cache'&&cache.siblingAfter===cache.siblingBefore,
    'native cache retirement changed sibling bytes');
  check(result.freshExpires===null&&result.absent===true&&result.freshBytes===payload+'|fresh',
    'native recreated bucket retained deleted data or was unusable');
  check(result.freshAfterStale===expires+1,'old bucket mutated recreated generation');
  check(result.siblingBefore===expires+['frame','dedicated','shared'].indexOf(actor)&&result.siblingAfter===result.siblingBefore,
    'bucket deletion changed sibling metadata');
  for(const keys of [result.afterDeleteKeys,result.finalKeys])
    check(JSON.stringify(keys)===JSON.stringify([scopeName]),'deleted bucket inventory '+JSON.stringify(keys));
}

async function devtoolsBucketFixture(name,marker,action){
  const manager=navigator.storageBuckets;
  if(action==='keys')return (await manager.keys()).sort();
  if(action==='delete'){await manager.delete(name);return (await manager.keys()).sort()}
  if(action!=='create')throw Error('invalid DevTools fixture action');
  const bucket=await manager.open(name),directory=await bucket.getDirectory();
  let absent=false;
  try{await directory.getFileHandle('value.txt')}
  catch(error){if(!(error instanceof DOMException)||error.name!=='NotFoundError')throw error;absent=true}
  const writeRead=async(root,name,text)=>{
    const file=await root.getFileHandle(name,{create:true}),writer=await file.createWritable();
    await writer.write(text);await writer.close();return (await file.getFile()).text();
  };
  const root=await writeRead(directory,'value.txt',marker);
  const child=await directory.getDirectoryHandle('child',{create:true});
  const nested=await writeRead(child,'nested.txt',marker+'|nested');
  return {absent,root,nested,rootBytes:new TextEncoder().encode(root).length,nestedBytes:new TextEncoder().encode(nested).length};
}

async function inspectBucketDevtools(client,marker,mode){
  const name='devtools-'+marker.slice(-16);
  const payload='DEVTOOLS_'+marker.slice(-32);
  const healthBefore=await value(client,'({url:location.href,ready:document.readyState})');
  check(healthBefore.ready==='complete','DevTools fixture page not ready');
  const tree=await client.send('Page.getFrameTree'),frameId=tree.frameTree.frame.id;
  const {storageKey}=await client.send('Storage.getStorageKey',{frameId});
  check(typeof frameId==='string'&&frameId&&typeof storageKey==='string'&&storageKey,'native DevTools storage identity');
  const request=pathComponents=>client.send('FileSystem.getDirectory',{
    bucketFileSystemLocator:{storageKey,bucketName:name,pathComponents}});
  const fixture=(action,text=payload)=>value(client,`(${devtoolsBucketFixture.toString()})(${[name,text,action].map(v=>JSON.stringify(v)).join(',')})`);
  const rejected=async()=>{
    let response=null;
    try{await request([])}catch(error){
      if(error.cdpTerminal)throw error;
      try{response=JSON.parse(error.message)}catch{throw error}
    }
    const messages=positive?['Storage bucket connection closed.']:
      ['Storage bucket connection closed.','Storage directory access is denied.'];
    check(response?.code===-32000&&messages.includes(response.message),
      'native DevTools closed-bucket response '+JSON.stringify(response));
    return response;
  };
  const positive=mode==='memory'||mode==='persistent';let outcomes;
  if(positive){
    const beforeKeys=await fixture('keys');check(!beforeKeys.includes(name),'DevTools bucket unexpectedly retained');
    const original=await fixture('create');check(original.absent&&original.root===payload&&original.nested===payload+'|nested','native DevTools file positive control');
    const root=await request([]),nested=await request(['child']);
    const checkDirectory=(result,label,file,size,directories)=>{
      const directory=result.directory;
      check(directory&&directory.name===label&&JSON.stringify(directory.nestedDirectories)===JSON.stringify(directories),
        'native DevTools directory structure '+JSON.stringify(result));
      check(directory.nestedFiles.length===1&&directory.nestedFiles[0].name===file&&directory.nestedFiles[0].size===size&&
        Number.isFinite(directory.nestedFiles[0].lastModified)&&typeof directory.nestedFiles[0].type==='string',
        'native DevTools file metadata '+JSON.stringify(result));
    };
    checkDirectory(root,'','value.txt',original.rootBytes,['child']);
    checkDirectory(nested,'child','nested.txt',original.nestedBytes,[]);
    const afterDelete=await fixture('delete');check(JSON.stringify(afterDelete)===JSON.stringify(beforeKeys),'DevTools delete changed other buckets');
    const closed=await rejected();
    const fresh=await fixture('create',payload+'|fresh');check(fresh.absent&&fresh.root===payload+'|fresh'&&fresh.nested===payload+'|fresh|nested','DevTools recreated bucket not fresh/usable');
    const recreated=await request([]);checkDirectory(recreated,'','value.txt',fresh.rootBytes,['child']);
    const finalKeys=await fixture('delete');check(JSON.stringify(finalKeys)===JSON.stringify(beforeKeys),'DevTools cleanup changed other buckets');
    outcomes={beforeKeys,original,root,nested,afterDelete,closed,fresh,recreated,finalKeys};
  }else outcomes={closed:await rejected()};
  const healthAfter=await value(client,'({url:location.href,ready:document.readyState})');
  check(healthAfter.ready==='complete'&&healthAfter.url===healthBefore.url,'DevTools response lost its live page');
  return {mode,name,frameId,storageKey,healthBefore,healthAfter,positive,outcomes};
}
