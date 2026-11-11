# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Exercise the smoke CDP consumer across document-binding invalidation."""
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke import CDP


PEER = r'''
const expectedURL = 'http://127.0.0.1/fixture';
let documents = 0;
globalThis.fetch = async url => ({ok: true, json: async () =>
  url.endsWith('/json/version') ? {webSocketDebuggerUrl: 'ws://browser'} :
  [{type: 'page', url: expectedURL, webSocketDebuggerUrl: 'ws://page'}]});
globalThis.WebSocket = class extends EventTarget {
  constructor() { super(); queueMicrotask(() => this.dispatchEvent(new Event('open'))); }
  close() { this.dispatchEvent(new Event('close')); }
  send(data) {
    const {id, method, params} = JSON.parse(data);
    let result = {}, error;
    if (method === 'Browser.getVersion') result = {product: 'Alloy', protocolVersion: '1.3'};
    if (method === 'DOM.getDocument') result = {root: {nodeId: ++documents}};
    if (method === 'DOM.querySelector') {
      if (documents === 1 || mode === 'persistent')
        error = {code: -32000, message: mode === 'unexpected' ? 'Unexpected failure' : 'Could not find node with given id'};
      else result = {nodeId: params.nodeId * 10};
    }
    if (method === 'DOM.getOuterHTML') result = {outerHTML: '<title>Internet</title>'};
    if (method === 'Runtime.evaluate') result = {result: {value: expectedURL}};
    queueMicrotask(() => this.dispatchEvent(new MessageEvent('message', {
      data: JSON.stringify(error ? {id, error} : {id, result})
    })));
  }
};
'''


class DocumentSampling(unittest.TestCase):
    def run_consumer(self, mode):
        source = 'const mode = ' + repr(mode) + ';\n' + PEER + CDP
        return subprocess.run(['node', '--input-type=module', '-e', source,
                               '9229', 'http://127.0.0.1/fixture', '1'],
                              capture_output=True, text=True, timeout=5)

    def test_navigation_invalidates_old_node_bindings(self):
        result = self.run_consumer('navigation')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"title_node":20', result.stdout)
        self.assertIn('"title_html":"<title>Internet</title>"', result.stdout)

    def test_other_protocol_errors_are_not_retried(self):
        result = self.run_consumer('unexpected')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unexpected failure', result.stderr)

    def test_invalidated_documents_still_obey_deadline(self):
        result = self.run_consumer('persistent')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Internet title node not observed', result.stderr)


if __name__ == '__main__':
    unittest.main()
