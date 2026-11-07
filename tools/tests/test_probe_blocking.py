# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
import sys
from pathlib import Path
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from probe_blocking import has_sentinel_spelling_marker


class SpellingMarkerProof(unittest.TestCase):
    def marker(self,types,starts,ends):
        return dict(name='hello qzxqzxqzx world ',markerTypes=types,
                    markerStarts=starts,markerEnds=ends)

    def test_flattened_types_cannot_attribute_the_sentinel(self):
        node=self.marker(['spelling','grammar'],[0,6],[5,15])
        self.assertFalse(has_sentinel_spelling_marker(node))

    def test_omitted_marker_types_cannot_attribute_the_sentinel(self):
        node=self.marker(['spelling'],[0,6],[5,15])
        self.assertFalse(has_sentinel_spelling_marker(node))

    def test_single_spelling_range_covers_the_sentinel(self):
        self.assertTrue(has_sentinel_spelling_marker(self.marker(['spelling'],[6],[15])))

    def test_single_spelling_range_covers_other_text(self):
        self.assertFalse(has_sentinel_spelling_marker(self.marker(['spelling'],[0],[5])))

    def test_sentinel_has_only_a_grammar_marker(self):
        self.assertFalse(has_sentinel_spelling_marker(self.marker(['grammar'],[6],[15])))

    def test_mismatched_ranges_are_not_proof(self):
        self.assertFalse(has_sentinel_spelling_marker(self.marker(['spelling'],[6],[15,21])))

    def test_absent_marker_is_not_proof(self):
        self.assertFalse(has_sentinel_spelling_marker({}))


if __name__=='__main__':unittest.main()
