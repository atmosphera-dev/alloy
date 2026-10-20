// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Real consumer callbacks, not protocol-acceptance results. All failures retained.
window.outcomes = {};
window.feature = async function(name) {
  const timeout = (promise, ms=20000) => Promise.race([promise,new Promise((_,reject)=>setTimeout(()=>reject(Error('consumer timeout')),ms))]);
  const O = window.outcomes;
  try {
    let value;
    if (name==='canary') {
      // No application timeout or abort: keep the page until Chromium's own
      // transport completes/fails. The CDP driver has a finite 600-second bound.
      await fetch('https://example.com/wakaba-canary',{mode:'no-cors',cache:'no-store'});
      value={consumer:'fetch-completed',opaque:true};
    } else if (name==='geolocation') {
      value=await timeout(new Promise((resolve,reject)=>navigator.geolocation.getCurrentPosition(p=>resolve({latitude:p.coords.latitude,longitude:p.coords.longitude,accuracy:p.coords.accuracy}),e=>reject(Error(e.code+': '+e.message)))));
    } else if (name==='notification') {
      const permission=await timeout(Notification.requestPermission());
      if(permission!=='granted') throw Error('permission '+permission);
      value=await timeout(new Promise((resolve,reject)=>{const n=new Notification('Wakaba soak local feature control',{body:'No personal data'}); n.onshow=()=>{resolve({consumer:'notification-shown',permission});n.close()};n.onerror=()=>reject(Error('notification error'))}));
    } else if (name==='eme') {
      const access=await timeout(navigator.requestMediaKeySystemAccess('com.widevine.alpha',[{initDataTypes:['cenc'],audioCapabilities:[{contentType:'audio/mp4; codecs="mp4a.40.2"'}],videoCapabilities:[{contentType:'video/mp4; codecs="avc1.42E01E"'}]}]));
      const keys=await timeout(access.createMediaKeys()); const v=document.createElement('video'); await v.setMediaKeys(keys);
      value={keySystem:access.keySystem,consumer:'MediaKeys-attached',configuration:access.getConfiguration()};
    } else if (name==='speech') {
      const C=window.SpeechRecognition||window.webkitSpeechRecognition;
      if(!C) throw Error('unsupported SpeechRecognition');
      value=await timeout(new Promise((resolve,reject)=>{const r=new C();r.lang='en-US';r.onstart=()=>{O.speechStarted=true};r.onresult=e=>{resolve({consumer:'recognition-result',text:e.results[0][0].transcript});r.stop()};r.onerror=e=>reject(Error(e.error));r.onend=()=>{if(!O.speechStarted)reject(Error('ended before start'))};r.start()}));
    } else if (name==='webrtc') {
      const pc=new RTCPeerConnection({iceServers:[]});pc.createDataChannel('wakaba');const offer=await pc.createOffer();await pc.setLocalDescription(offer);
      await timeout(new Promise(resolve=>{if(pc.iceGatheringState==='complete')resolve();else pc.onicegatheringstatechange=()=>{if(pc.iceGatheringState==='complete')resolve()}}),10000);
      value={consumer:'local-description-and-ice',type:pc.localDescription.type,sdp:pc.localDescription.sdp,ice:pc.iceGatheringState};pc.close();
    } else if (name==='push') {
      const registration=await timeout(navigator.serviceWorker.register('/sw.js'));
      await timeout(navigator.serviceWorker.ready);
      // Fixed public VAPID P-256 generator point; no private key or real push account.
      const key=Uint8Array.from(atob('BGsX0fLhLEJH+Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWT+NC4v4af5uO5+tKfA+eFivOM1drMV7Oy7ZAaDe/UfU='),c=>c.charCodeAt(0));
      const sub=await timeout(registration.pushManager.subscribe({userVisibleOnly:true,applicationServerKey:key}));
      value={consumer:'subscription-created',subscription:sub.toJSON(),worker:registration.active?.state};
    } else if (name==='download') {
      const a=document.createElement('a');a.href='/download.txt';a.download='wakaba-control.txt';document.body.append(a);a.click();value={consumer:'click-dispatched-await-filesystem'};
    } else if (name==='audio') {
      const a=document.createElement('audio');a.src='/tone.wav';a.controls=true;document.body.append(a);await timeout(a.play());await new Promise(r=>setTimeout(r,1500));value={consumer:'audio-time-advanced',time:a.currentTime,readyState:a.readyState};a.pause();
    } else throw Error('unknown feature '+name);
    return O[name]={status:'success',value};
  } catch(e) {return O[name]={status:'failed-or-unsupported',error:String(e),errorName:e.name,consumer:name==='canary'&&e instanceof TypeError?'network-error':'feature-error',speechStarted:O.speechStarted};}
};
