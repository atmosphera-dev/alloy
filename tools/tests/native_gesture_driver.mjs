// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Transport adapter only. The actual native test controls activation delivery
// and picker admission; this imports the exact sequencer used by the live probe.
import assert from 'node:assert/strict';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const {createNativeActionSequencer} = await import(pathToFileURL(process.argv[2]));
const lines = createInterface({input: process.stdin});
let pending;
lines.on('line', line => {
  assert(pending, 'native reply without a request');
  const resolve = pending;
  pending = undefined;
  resolve(JSON.parse(line));
});
function request(command) {
  assert(!pending, 'overlapping native requests');
  return new Promise(resolve => {
    pending = resolve;
    process.stdout.write(JSON.stringify({command}) + '\n');
  });
}
const action = createNativeActionSequencer({
  prime: () => request('prime'),
  acknowledge: async () => {
    const receipt = await request('acknowledge');
    assert.equal(receipt.native_active, true);
  },
  dispatch: async () => {
    const result = await request('action');
    assert.equal(result.ok, true, result.message);
    assert.equal(result.granted, true, 'native selected file grant missing');
    return result;
  },
});
let ok = false;
try {
  await action('replacement-document', {});
  ok = true;
} catch (error) {
  console.error(String(error));
  process.exitCode = 1;
} finally {
  process.stdout.write(JSON.stringify({command: 'done', ok}) + '\n');
  lines.close();
  process.stdin.pause();
}
