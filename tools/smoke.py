#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Bounded OSR/Kiwix consumer smoke; Node's built-in CDP WebSocket, no pip deps."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

WORK = Path('/home/builder/work/alloy')
URL = 'http://nomad.home:8090/content/wikipedia_en_all_mini_2025-12/Internet'

CDP = r'''
const [port, expected, seconds] = process.argv.slice(1);
const deadline = Date.now() + Number(seconds) * 1000;
const sleep = () => new Promise(resolve => setTimeout(resolve, 400));
async function connect(url) {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, {once: true});
    ws.addEventListener('error', reject, {once: true});
  });
  let next = 0;
  const pending = new Map();
  ws.addEventListener('message', event => {
    const msg = JSON.parse(event.data);
    const request = pending.get(msg.id);
    if (!request) return;
    pending.delete(msg.id);
    if (msg.error) request.reject(new Error(JSON.stringify(msg.error)));
    else request.resolve(msg.result);
  });
  ws.addEventListener('close', () => {
    for (const request of pending.values()) request.reject(new Error('CDP closed'));
    pending.clear();
  });
  return {
    call: (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++next;
      pending.set(id, {resolve, reject});
      ws.send(JSON.stringify({id, method, params}));
    }),
    close: () => ws.close(),
  };
}
async function endpoint(path) {
  const response = await fetch(`http://127.0.0.1:${port}${path}`, {signal: AbortSignal.timeout(1500)});
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}
let versionInfo;
while (Date.now() < deadline) {
  try { versionInfo = await endpoint('/json/version'); break; }
  catch { await sleep(); }
}
if (!versionInfo) throw new Error('Browser CDP endpoint did not appear');
const browser = await connect(versionInfo.webSocketDebuggerUrl);
const version = await browser.call('Browser.getVersion');
browser.close();
if (!version.product || !version.protocolVersion) throw new Error('Incomplete Browser.getVersion');
let page;
while (Date.now() < deadline) {
  const targets = await endpoint('/json/list');
  const target = targets.find(t => t.type === 'page' && t.url === expected);
  if (target) { page = await connect(target.webSocketDebuggerUrl); break; }
  await sleep();
}
if (!page) throw new Error('Expected Kiwix page target did not appear');
let proof;
while (Date.now() < deadline) {
  const {root} = await page.call('DOM.getDocument', {depth: 1});
  const {nodeId} = await page.call('DOM.querySelector', {nodeId: root.nodeId, selector: 'title'});
  if (nodeId) {
    const {outerHTML} = await page.call('DOM.getOuterHTML', {nodeId});
    const location = await page.call('Runtime.evaluate', {expression: 'location.href', returnByValue: true});
    if (outerHTML.includes('Internet') && location.result.value === expected) {
      proof = {version, url: location.result.value, title_node: nodeId, title_html: outerHTML};
      break;
    }
  }
  await sleep();
}
if (!proof) throw new Error('Kiwix Internet title node not observed in DOM.getDocument');
await page.call('Page.close');
page.close();
console.log(JSON.stringify(proof));
'''


def session_processes(session):
    """Track every live process in our isolated browser/Xvfb session."""
    owned = []
    for path in Path('/proc').iterdir():
        if not path.name.isdecimal():
            continue
        try:
            fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue  # A process may exit while /proc is being inspected.
        if int(fields[3]) == session and fields[0] != 'Z':
            owned.append(dict(pid=int(path.name), group=int(fields[2]), state=fields[0]))
    return owned


def wait_for_session_exit(session, seconds):
    deadline = time.monotonic() + seconds
    while True:
        remaining = session_processes(session)
        if not remaining or time.monotonic() >= deadline:
            return remaining
        time.sleep(0.1)


def terminate_session(session):
    # Timeout/error cleanup is restricted to this invocation's isolated
    # session, including children that created their own process groups.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for group in {p['group'] for p in session_processes(session)}:
            if group <= 1 or group == os.getpgrp():
                raise RuntimeError(f'Unsafe browser process group: {group}')
            try:
                os.killpg(group, sig)
            except ProcessLookupError:
                pass
        if not wait_for_session_exit(session, 5):
            return
    raise RuntimeError('Browser session still alive; preserving its profile')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=9229)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--probe-blocking', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    if socket.gethostname() != 'wakaba-build' or out not in (
            WORK / 'src/out/Release_Alloy', WORK / 'src/out/Verify_Alloy'):
        parser.error('Only the approved NUC Alloy output is supported')
    if args.probe_blocking:
        return subprocess.run([sys.executable, str(Path(__file__).with_name('probe_blocking.py')),
                               '--out', str(out)], timeout=180).returncode
    if not 1024 <= args.port <= 65535 or not 10 <= args.timeout <= 300:
        parser.error('Port must be unprivileged and timeout 10..300 seconds')
    with socket.socket() as check:
        # A completed connection's TIME_WAIT is not a live debugger listener.
        check.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        check.bind(('127.0.0.1', args.port))
    logdir = WORK / 'logs' / f'smoke-{time.time_ns()}'
    logdir.mkdir()
    print('SMOKE_LOGS', logdir, flush=True)
    profile = Path(tempfile.mkdtemp(prefix='alloy-smoke-', dir=WORK))
    process = None
    shutdown = dict(profile=str(profile), normal_shutdown=False, profile_removed=False)
    try:
        command = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24', str(out / 'cefclient'),
                   '--off-screen-rendering-enabled', f'--remote-debugging-port={args.port}',
                   '--ozone-platform=x11', '--disable-gpu', f'--url={URL}',
                   f'--root-cache-path={profile}', f'--cache-path={profile}/Default',
                   f'--log-file={logdir}/debug.log']
        (logdir / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (logdir / 'stdio.log').open('w') as log:
            process = subprocess.Popen(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, TMPDIR='/home/builder/work/tmp'),
                                       start_new_session=True)
            probe = subprocess.run(['node', '--input-type=module', '-e', CDP,
                                    str(args.port), URL, str(args.timeout - 5)],
                                    capture_output=True, text=True, timeout=args.timeout)
            (logdir / 'cdp.stdout').write_text(probe.stdout)
            (logdir / 'cdp.stderr').write_text(probe.stderr)
            print(probe.stdout, end='', flush=True)
            if probe.returncode:
                print(probe.stderr, file=sys.stderr)
                return probe.returncode
            # Page.close initiates normal CEF shutdown. The wrapper exiting
            # alone does not establish that its child processes have exited.
            process.wait(timeout=15)
            shutdown['wrapper_exit'] = process.returncode
            if process.returncode:
                raise RuntimeError(f'Browser shutdown failed: {process.returncode}')
            remaining = wait_for_session_exit(process.pid, 10)
            if remaining:
                raise RuntimeError(f'Browser children still alive: {remaining}')
            shutdown['normal_shutdown'] = True
    finally:
        try:
            if process:
                if session_processes(process.pid):
                    terminate_session(process.pid)
                process.wait(timeout=5)
                shutdown['remaining_processes'] = session_processes(process.pid)
                if shutdown['remaining_processes']:
                    raise RuntimeError('Preserving profile while browser processes remain')
            # Never swallow a deletion error or delete while owned writers run.
            shutil.rmtree(profile)
            shutdown['profile_removed'] = True
        finally:
            (logdir / 'shutdown.json').write_text(json.dumps(shutdown, indent=2) + '\n')
    print('ALLOY_KIWIX_SMOKE_PASS', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
