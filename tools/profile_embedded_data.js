// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Native storage fixture; marker arrives only through postMessage.
async function nativeBucketRetirement(marker, actor, sibling) {
  const manager = navigator.storageBuckets;
  const name = 'retired-' + marker.slice(-16) + '-' + actor;
  const expires = 1893456000000 + parseInt(marker.slice(-6), 16) * 10;
  const payload = marker + '|retired|' + actor;
  const siblingBefore = await sibling.expires();
  const bucket = await manager.open(name);
  const writeRead = async (directory, text) => {
    const file = await directory.getFileHandle('value', {create: true});
    const writer = await file.createWritable();
    await writer.write(text); await writer.close();
    return (await file.getFile()).text();
  };
  const originalDirectory = await bucket.getDirectory();
  const beforeBytes = await writeRead(originalDirectory, payload);
  const originalFile = await originalDirectory.getFileHandle('value');
  const positiveRetainedBytes = await (await originalFile.getFile()).text();
  const openDatabase = target => new Promise((resolve, reject) => {
    const request = target.indexedDB.open('retained-connection', 1);
    let upgrade = null;
    request.onupgradeneeded = event => {
      upgrade = {oldVersion: event.oldVersion, newVersion: event.newVersion};
      request.result.createObjectStore('values');
    };
    request.onerror = () => reject(request.error);
    request.onblocked = () => reject(Error('native retained database unexpectedly blocked'));
    request.onsuccess = () => resolve({database: request.result, upgrade});
  });
  const transactionDone = transaction => new Promise((resolve, reject) => {
    transaction.oncomplete = resolve;
    transaction.onabort = () => reject(transaction.error || Error('native IDB transaction aborted'));
    transaction.onerror = () => reject(transaction.error);
  });
  const writeDatabase = async (database, text) => {
    const transaction = database.transaction('values', 'readwrite');
    const completed = transactionDone(transaction);
    transaction.objectStore('values').put(text, 'key');
    await completed;
  };
  const readDatabase = async database => {
    const transaction = database.transaction('values');
    const completed = transactionDone(transaction);
    const result = await new Promise((resolve, reject) => {
      const request = transaction.objectStore('values').get('key');
      request.onsuccess = () => resolve(request.result ?? null);
      request.onerror = () => reject(request.error);
    });
    await completed;
    return result;
  };
  const originalFactory = bucket.indexedDB;
  const cacheName = 'retained-cache';
  const cacheUrl = new URL('/retained-cache-' + actor, location.origin).href;
  const originalStorage = bucket.caches;
  const originalCache = await originalStorage.open(cacheName);
  const cacheResponse = suffix => new Response(payload + '|cache' + suffix,
    {status: 201, headers: {'x-alloy-cache': payload + suffix}});
  const readCache = async storage => {
    const response = await storage.match(cacheUrl);
    return response === undefined ? {missing: true} : {missing: false,
      body: await response.text(), status: response.status, header: response.headers.get('x-alloy-cache')};
  };
  await originalCache.put(cacheUrl, cacheResponse(''));
  const cacheBefore = {cache: await readCache(originalCache), storage: await readCache(originalStorage),
    cacheKeys: (await originalCache.keys()).map(request => request.url), storageKeys: await originalStorage.keys(),
    has: await originalStorage.has(cacheName)};
  const siblingCache = await sibling.caches.open('actual-data');
  const siblingCacheValue = async () => {
    const response = await siblingCache.match(new URL('/actual-data-' + actor, location.origin));
    return response ? response.text() : null;
  };
  const siblingCacheBefore = await siblingCacheValue();
  const originalIdb = await openDatabase(bucket);
  await writeDatabase(originalIdb.database, payload + '|idb');
  const beforeIdb = await readDatabase(originalIdb.database), closeEvents = [];
  const idbClosed = new Promise(resolve => {
    originalIdb.database.addEventListener('close', event => {
      const result = {type: event.type, trusted: event.isTrusted};
      closeEvents.push(result); resolve(result);
    });
  });
  const transactionRefusal = () => {
    let transaction;
    try { transaction = originalIdb.database.transaction('values'); }
    catch (error) {
      if (!(error instanceof DOMException)) throw error;
      return {status: 'threw', name: error.name, message: error.message};
    }
    transaction.abort();
    return {status: 'admitted'};
  };
  const factoryRequest = action => new Promise((resolve, reject) => {
    const upgrades = [];
    const request = action === 'open'
      ? originalFactory.open('retained-connection', 1)
      : originalFactory.deleteDatabase('retained-connection');
    request.onupgradeneeded = event => upgrades.push({oldVersion: event.oldVersion, newVersion: event.newVersion});
    request.onblocked = () => reject(Error('retained native factory request blocked: ' + action));
    request.onerror = event => {
      event.preventDefault();
      const error = request.error;
      if (!(error instanceof DOMException)) { reject(Error('native IDB request error missing')); return; }
      resolve({status: 'error', action, event: event.type, trusted: event.isTrusted,
        name: error.name, message: error.message, upgrades});
    };
    request.onsuccess = () => {
      if (action === 'open') request.result.close();
      resolve({status: 'admitted', action, upgrades});
    };
  });
  const factoryDatabases = async () => {
    try {
      const databases = await originalFactory.databases();
      return {status: 'fulfilled', databases: databases.map(value => ({name: value.name, version: value.version}))};
    } catch (error) {
      if (!(error instanceof DOMException)) throw error;
      return {status: 'rejected', name: error.name, message: error.message};
    }
  };
  await bucket.setExpires(expires);
  const beforeExpires = await bucket.expires();
  const pendingTransaction = originalIdb.database.transaction('values', 'readwrite');
  const pendingStore = pendingTransaction.objectStore('values');
  const pendingErrors = [], pendingEvents = [];
  let pendingReads = 0, stagedWrite = null, pendingRequest;
  const pendingAborted = new Promise(resolve => {
    pendingTransaction.oncomplete = event => {
      pendingEvents.push({type: event.type, trusted: event.isTrusted});
      resolve({status: 'completed'});
    };
    pendingTransaction.onabort = event => {
      const error = pendingTransaction.error;
      pendingEvents.push({type: event.type, trusted: event.isTrusted});
      resolve({status: 'aborted', name: error?.name, message: error?.message});
    };
  });
  const pendingReady = new Promise((resolve, reject) => {
    const error = event => {
      event.preventDefault();
      const request = event.target;
      pendingErrors.push({type: event.type, trusted: event.isTrusted,
        name: request.error?.name, message: request.error?.message});
      if (!pendingReads) reject(request.error || Error('pending IDB positive read failed'));
    };
    const spin = () => {
      pendingRequest = pendingStore.get('key');
      pendingRequest.onerror = error;
      pendingRequest.onsuccess = event => {
        if (pendingRequest.result !== payload + '|idb') {
          reject(Error('pending native IDB read changed')); return;
        }
        ++pendingReads;
        spin();
        resolve({type: event.type, trusted: event.isTrusted, value: payload + '|idb'});
      };
    };
    const write = pendingStore.put(payload + '|uncommitted', 'pending');
    write.onerror = error;
    write.onsuccess = event => {
      stagedWrite = {type: event.type, trusted: event.isTrusted, key: write.result};
      spin();
    };
  });
  const positivePendingRead = await pendingReady;
  const pendingBeforeDelete = {reads: pendingReads, requestState: pendingRequest.readyState,
    terminalEvents: pendingEvents.length, requestErrors: pendingErrors.length};
  await manager.delete(name);
  const closeEvent = await idbClosed;
  const pendingAbort = await pendingAborted;
  const oldStoreRefusal = () => {
    try { pendingStore.get('key'); }
    catch (error) {
      if (!(error instanceof DOMException)) throw error;
      return {status: 'threw', name: error.name, message: error.message};
    }
    return {status: 'admitted'};
  };
  const storeAfterDelete = oldStoreRefusal();
  const transactionAfterDelete = transactionRefusal();
  const afterDeleteKeys = await manager.keys();
  const rejection = async operation => {
    try {
      const value = await operation();
      return typeof value === 'string' ? {status: 'fulfilled', value} : {status: 'fulfilled'};
    }
    catch (error) {
      if (!(error instanceof DOMException)) throw error;
      return {status: 'rejected', name: error.name, message: error.message};
    }
  };
  const cacheOutcome = async operation => {
    try {
      const value = await operation();
      const describe = item => ({type: Object.prototype.toString.call(item),
        url: item?.url ?? null, status: item?.status ?? null});
      const returned = value === undefined ? {type: 'undefined'}
        : Array.isArray(value) ? {type: 'array', length: value.length, entries: value.map(describe)}
        : typeof value === 'boolean' ? {type: 'boolean', value} : describe(value);
      return {status: 'fulfilled', value: returned};
    } catch (error) {
      if (!(error instanceof DOMException)) throw error;
      return {status: 'rejected', name: error.name, message: error.message};
    }
  };
  const cacheProbe = async () => ({
    cacheMatch: await readCache(originalCache),
    cacheMatchAll: await cacheOutcome(() => originalCache.matchAll()),
    cacheKeys: await cacheOutcome(() => originalCache.keys()),
    cachePut: await cacheOutcome(() => originalCache.put(cacheUrl, cacheResponse('|stale'))),
    cacheDelete: await originalCache.delete(cacheUrl),
    storageMatch: await readCache(originalStorage), storageKeys: await originalStorage.keys(),
    storageHas: await cacheOutcome(() => originalStorage.has(cacheName)),
    storageOpen: await cacheOutcome(() => originalStorage.open(cacheName)),
    storageDelete: await originalStorage.delete(cacheName)});
  const cacheAfterDelete = await cacheProbe();
  const denied = {};
  const openAfterDelete = await factoryRequest('open');
  const deleteAfterDelete = await factoryRequest('delete');
  const databasesAfterDelete = await factoryDatabases();
  denied.persisted = await rejection(() => bucket.persisted());
  denied.estimate = await rejection(() => bucket.estimate());
  denied.expires = await rejection(() => bucket.expires());
  denied.setExpires = await rejection(() => bucket.setExpires(expires + 2));
  denied.directory = await rejection(() => bucket.getDirectory());
  const directoryAfterDelete = await rejection(() => originalDirectory.getFileHandle('value'));
  const fileAfterDelete = await rejection(async () => (await originalFile.getFile()).text());
  const fresh = await manager.open(name);
  const freshStorage = fresh.caches, freshCache = await freshStorage.open(cacheName);
  const freshCacheBefore = await readCache(freshCache);
  await freshCache.put(cacheUrl, cacheResponse('|fresh'));
  const freshCacheValue = await readCache(freshCache);
  const freshIdb = await openDatabase(fresh);
  const freshIdbBefore = await readDatabase(freshIdb.database);
  const freshPendingTransaction = freshIdb.database.transaction('values');
  const freshPendingDone = transactionDone(freshPendingTransaction);
  const freshPending = await new Promise((resolve, reject) => {
    const request = freshPendingTransaction.objectStore('values').get('pending');
    request.onsuccess = () => resolve(request.result ?? null);
    request.onerror = () => reject(request.error);
  });
  await freshPendingDone;
  await writeDatabase(freshIdb.database, payload + '|idb|fresh');
  const freshIdbValue = await readDatabase(freshIdb.database);
  const transactionAfterReopen = transactionRefusal();
  const storeAfterReopen = oldStoreRefusal();
  const openAfterReopen = await factoryRequest('open');
  const deleteAfterReopen = await factoryRequest('delete');
  const databasesAfterReopen = await factoryDatabases();
  const freshValueAfterFactoryProbes = await readDatabase(freshIdb.database);
  const cacheAfterReopen = await cacheProbe();
  const freshCacheAfterStale = {cache: await readCache(freshCache), storage: await readCache(freshStorage),
    keys: await freshStorage.keys(), has: await freshStorage.has(cacheName)};
  const siblingCacheAfter = await siblingCacheValue();
  const freshIdbAfterStale = await readDatabase(freshIdb.database);
  freshIdb.database.close();
  const freshExpires = await fresh.expires();
  const directory = await fresh.getDirectory();
  let absent = false;
  try { await directory.getFileHandle('value'); }
  catch (error) {
    if (!(error instanceof DOMException) || error.name !== 'NotFoundError') throw error;
    absent = true;
  }
  const freshBytes = await writeRead(directory, payload + '|fresh');
  await fresh.setExpires(expires + 1);
  const staleMutation = await rejection(() => bucket.setExpires(expires + 2));
  const freshAfterStale = await fresh.expires();
  const fileAfterReopen = await rejection(async () => (await originalFile.getFile()).text());
  const freshBytesAfterStale = await (await (await directory.getFileHandle('value')).getFile()).text();
  await manager.delete(name);
  return {actor, name, beforeBytes, beforeExpires, afterDeleteKeys, denied,
    freshExpires, absent, freshBytes, staleMutation, freshAfterStale,
    siblingBefore, siblingAfter: await sibling.expires(), finalKeys: await manager.keys(),
    fileRetirement: {positiveRetainedBytes, directoryAfterDelete, fileAfterDelete,
      fileAfterReopen, freshBytesAfterStale},
    idbRetirement: {beforeValue: beforeIdb, originalUpgrade: originalIdb.upgrade,
      closeEvent, closeEvents, transactionAfterDelete, freshUpgrade: freshIdb.upgrade,
      freshBefore: freshIdbBefore, freshValue: freshIdbValue, transactionAfterReopen,
      freshAfterStale: freshIdbAfterStale},
    idbFactoryRetirement: {openAfterDelete, deleteAfterDelete, databasesAfterDelete,
      openAfterReopen, deleteAfterReopen, databasesAfterReopen, freshValueAfterFactoryProbes},
    idbInFlightRetirement: {stagedWrite, positivePendingRead, pendingBeforeDelete,
      pendingAbort, pendingEvents, pendingErrors, storeAfterDelete, storeAfterReopen, freshPending},
    cacheRetirement: {cacheName, cacheUrl, before: cacheBefore, afterDelete: cacheAfterDelete,
      freshBefore: freshCacheBefore, freshValue: freshCacheValue, afterReopen: cacheAfterReopen,
      freshAfterStale: freshCacheAfterStale, siblingBefore: siblingCacheBefore, siblingAfter: siblingCacheAfter}};
}

async function nativeData(mode, actor, marker, bucketName) {
  const actors = ['frame', 'dedicated', 'shared'];
  const request = r => new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const fileOrNull = async (root, name) => {
    try { return await root.getFileHandle(name); }
    catch (error) { if (error.name === 'NotFoundError') return null; throw error; }
  };
  const result = {};
  for (const namespace of ['default', 'named']) {
    const bucket = namespace === 'named'
      ? await navigator.storageBuckets.open(bucketName)
      : { indexedDB, caches, getDirectory: () => navigator.storage.getDirectory() };
    const open = bucket.indexedDB.open('actual-data', 1);
    open.onupgradeneeded = () => open.result.createObjectStore('values');
    const db = await request(open);
    const cache = await bucket.caches.open('actual-data');
    const directory = await bucket.getDirectory();
    const value = kind => marker + '|' + namespace + '|' + actor + '|' + kind;
    if (mode === 'write') {
      const transaction = db.transaction('values', 'readwrite');
      const committed = new Promise((resolve, reject) => {
        transaction.oncomplete = resolve;
        transaction.onerror = () => reject(transaction.error);
        transaction.onabort = () => reject(transaction.error || Error('IDB transaction aborted'));
      });
      transaction.objectStore('values').put(value('idb'), actor);
      await committed;
      await cache.put(new URL('/actual-data-' + actor, location.origin), new Response(value('cache')));
      const file = await directory.getFileHandle(actor, {create: true});
      const writer = await file.createWritable();
      await writer.write(value('opfs'));
      await writer.close();
    }
    const stored = {idb: {}, cache: {}, opfs: {}};
    const transaction = db.transaction('values');
    const reads = actors.map(who => request(transaction.objectStore('values').get(who)));
    const idb = await Promise.all(reads);
    db.close();
    for (let index = 0; index < actors.length; ++index) {
      const who = actors[index];
      stored.idb[who] = idb[index] ?? null;
      const response = await cache.match(new URL('/actual-data-' + who, location.origin));
      stored.cache[who] = response ? await response.text() : null;
      const file = await fileOrNull(directory, who);
      stored.opfs[who] = file ? await (await file.getFile()).text() : null;
    }
    let syncFile = await fileOrNull(directory, 'sync-access');
    if (actor === 'dedicated') {
      if (mode === 'write') syncFile = await directory.getFileHandle('sync-access', {create: true});
      if (syncFile) {
        const handle = await syncFile.createSyncAccessHandle();
        try {
          const bytes = new TextEncoder().encode(marker + '|' + namespace + '|sync');
          if (mode === 'write') {
            handle.truncate(bytes.length + 8);
            if (handle.write(bytes, {at: 0}) !== bytes.length) throw Error('short sync write');
            handle.flush();
            if (handle.getSize() !== bytes.length + 8) throw Error('sync extend size');
            const extended = new Uint8Array(bytes.length + 8);
            if (handle.read(extended, {at: 0}) !== extended.length) throw Error('short extended read');
            if (extended.slice(bytes.length).some(value => value !== 0)) throw Error('sync extension not zero');
            handle.truncate(bytes.length);
            handle.flush();
          }
          const bytesRead = new Uint8Array(handle.getSize());
          if (handle.read(bytesRead, {at: 0}) !== bytesRead.length) throw Error('short sync read');
          stored.syncRead = new TextDecoder().decode(bytesRead);
          stored.syncSize = handle.getSize();
        } finally { handle.close(); }
      } else {
        stored.syncRead = null;
        stored.syncSize = 0;
      }
    }
    stored.sync = syncFile ? await (await syncFile.getFile()).text() : null;
    if (namespace === 'named') {
      const expires = 1893456000000 + parseInt(marker.slice(-6), 16) * 10 + actors.indexOf(actor);
      if (mode === 'write') await bucket.setExpires(expires);
      const estimate = await bucket.estimate();
      stored.metadata = {name: bucket.name, expires: await bucket.expires(),
        persisted: await bucket.persisted(), usage: estimate.usage, quota: estimate.quota,
        keys: await navigator.storageBuckets.keys()};
      if (mode === 'write') stored.retirement = await nativeBucketRetirement(marker, actor, bucket);
    }
    result[namespace] = stored;
  }
  return result;
}

if (action === 'data-write' || action === 'data-read') {
  const mode = action === 'data-write' ? 'write' : 'read';
  const name = 'scope-' + marker.slice(-16);
  const frame = await nativeData(mode, 'frame', marker, name);
  const workerCode = nativeBucketRetirement.toString() + '\n' + nativeData.toString() + `
    const run = async (data, reply) => {
      try { reply({result: await nativeData(data.mode, data.actor, data.marker, data.name)}); }
      catch (error) { reply({error: String(error)}); }
    };
    onmessage = event => run(event.data, value => postMessage(value));
    onconnect = event => { const port = event.ports[0];
      port.onmessage = event => run(event.data, value => port.postMessage(value)); port.start(); };
  `;
  const url = URL.createObjectURL(new Blob([workerCode], {type: 'text/javascript'}));
  const dedicated = await new Promise((resolve, reject) => {
    const worker = new Worker(url);
    worker.onmessage = event => { worker.terminate();
      event.data.error ? reject(Error(event.data.error)) : resolve(event.data.result); };
    worker.onerror = event => { worker.terminate(); reject(Error(event.message)); };
    worker.postMessage({mode, actor: 'dedicated', marker, name});
  });
  const shared = await new Promise((resolve, reject) => {
    const worker = new SharedWorker(url);
    worker.port.onmessage = event => { worker.port.close();
      event.data.error ? reject(Error(event.data.error)) : resolve(event.data.result); };
    worker.onerror = event => { worker.port.close(); reject(Error(event.message)); };
    worker.port.start(); worker.port.postMessage({mode, actor: 'shared', marker, name});
  });
  URL.revokeObjectURL(url);
  const reread = await nativeData('read', 'frame', marker, name);
  event.source.postMessage({serial, result: {frame, dedicated, shared, reread}}, event.origin);
  return;
}
