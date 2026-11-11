# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Explicit native persistence fence for the ordinary test-control browser."""
import json
from pathlib import Path
import subprocess
import time

DRIVE = r'''
import {readFile} from 'node:fs/promises';
const [port,eventLog]=process.argv.slice(1);
__CDP_HELPERS__
const pages=await pageTargets();
check(pages.length===1&&pages[0].url==='about:blank','control setup must precede no-disk browsers');
check(!(await nativeEvents()).some(e=>['created','memory-start','stateless-start'].includes(e.event)),
  'no-disk work began before persistent-control setup');
const parent=await connect(pages[0]);await parent.send('Page.enable');
await control(parent,'prepare-control');
const done=await until(async()=>{
  const events=await nativeEvents();
  check(!events.some(e=>['created','memory-start','stateless-start'].includes(e.event)),
    'no-disk context admitted while setup persistence was pending');
  const completed=events.filter(e=>e.event==='parent-persistence-complete');
  check(completed.length<=1,'duplicate setup completion');
  return completed[0];
},'native control profile/persistent NetworkContext completion');
parent.ws.close();console.log(JSON.stringify({native:done,noDiskWorkBeforeCompletion:false}));
'''

def complete(root: Path, process, port, helpers):
    assert process.poll() is None
    response=subprocess.run(['node','--input-type=module','-e',
        DRIVE.replace('__CDP_HELPERS__',helpers),str(port),str(root/'stdio.log')],
        capture_output=True,text=True,timeout=90)
    (root/'control-setup.stdout').write_text(response.stdout)
    (root/'control-setup.stderr').write_text(response.stderr)
    assert response.returncode==0,response.stderr
    result=json.loads(response.stdout)
    assert result['native']['event']=='parent-persistence-complete'
    assert not result['noDiskWorkBeforeCompletion']
    result['observed_at']=time.time()
    (root/'native-control-setup.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
