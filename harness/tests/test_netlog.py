# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Normal shutdown closes the last event inline and adds polledData, not events."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('netlog_stream',Path(__file__).resolve().parents[1]/'analysis/netlog_stream.py')
stream=importlib.util.module_from_spec(spec);spec.loader.exec_module(stream)


class NetlogEnvelope(unittest.TestCase):
    def adapt(self,ending):
        scratch=tempfile.TemporaryDirectory();self.addCleanup(scratch.cleanup)
        root=Path(scratch.name)
        event=dict(source=dict(id=4),time='123',type=564,phase=2)
        raw=root/'raw.json'
        raw.write_text('{"constants":{"timeTickOffset":"0"}},\n"events": [\n'+json.dumps(event)+ending)
        destination=root/'view';destination.mkdir()
        receipt=stream.adapt(raw,destination)
        events=[json.loads(s.rstrip(',')) for s in (destination/'event_file_0.json').read_text().splitlines()]
        return receipt,events,event

    def test_normally_closed_inline_last_event_with_polled_data(self):
        receipt,events,event=self.adapt('],\n"polledData": [{"not":"an event"}]\n}\n')
        self.assertEqual(events,[event]);self.assertTrue(receipt['events_array_closed'])
        self.assertEqual(receipt['footer_lines'],2);self.assertFalse(receipt['partial_tails'])

    def test_live_complete_event_and_deferred_tail(self):
        receipt,events,event=self.adapt(',\n{"source":')
        self.assertEqual(events,[event]);self.assertFalse(receipt['events_array_closed'])
        self.assertEqual(len(receipt['partial_tails']),1)

    def test_invalid_suffix_is_not_silently_removed(self):
        with self.assertRaisesRegex(ValueError,'Unexpected event envelope'):
            self.adapt(', THIS IS NOT THE ENVELOPE\n')


if __name__=='__main__':unittest.main()
