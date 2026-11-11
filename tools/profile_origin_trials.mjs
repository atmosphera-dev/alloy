// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
async function embeddedTrial(c,tag,kind='critical'){
  const path='/embedded-origin-trial?marker='+encodeURIComponent(secret)+
    '&tag='+encodeURIComponent(tag)+'&kind='+encodeURIComponent(kind);
  const args=[embeddedBase,'trial-read',secret,path].map(v=>JSON.stringify(v)).join(',');
  const result=await value(c,`(${callEmbeddedFrame.toString()})(${args})`);
  check(result.secure&&result.url===embeddedBase+path,'native embedded trial navigation '+tag);
  return result;
}
async function ordinaryTrial(c,tag,kind='critical'){
  const url=embeddedBase+'/embedded-origin-trial?marker='+encodeURIComponent(secret)+
    '&tag='+encodeURIComponent(tag)+'&kind='+encodeURIComponent(kind);
  const result=await c.send('Page.navigate',{url});check(!result.errorText,'ordinary trial navigation');
  await until(async()=>await value(c,`document.readyState==='complete'&&isSecureContext&&location.href===${JSON.stringify(url)}`),'ordinary signed trial '+tag);
  await committedHistory(c,url);return {url,secure:true};
}
