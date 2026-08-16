#!/usr/bin/env python3
"""
web/test_interval_viewer_link.py -- unit tests for server.py's render_interval_viewer()
(INVESTIGATION.md 4.4(a) item 8(c), "Linked navigation between the tree and interval/timeline
views"): the full-column --interval timeline page gained an optional "Tree + timeline (combined)"
link, resolved by the caller (the /interval-viewer/... route handler) via the same
joblib.find_combined_timeline_csv() the report page's own reciprocal link already uses, then passed
in as an already-built URL rather than render_interval_viewer() re-deriving it. This only tests that
thin rendering contract -- find_combined_timeline_csv() itself is covered by web/test_joblib.py.
Not wired into make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C
toolchain's test targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_interval_viewer_link.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class RenderIntervalViewerCombinedLinkTest(unittest.TestCase):
    def test_no_link_when_not_eligible(self):
        html = server.render_interval_viewer("cpu2026", "801.xz_s", "20260816T020617",
                                               "amdtopdown.csv", combined_timeline_url=None)
        self.assertNotIn("Tree + timeline (combined)", html)
        self.assertIn("Back to report", html)

    def test_link_present_and_points_at_the_resolved_url(self):
        url = "/timeline-viewer/cpu2026/801.xz_s/20260816T020617/tree-heavy.interval.csv"
        html = server.render_interval_viewer("cpu2026", "801.xz_s", "20260816T020617",
                                               "amdtopdown.csv", combined_timeline_url=url)
        self.assertIn("Tree + timeline (combined)", html)
        self.assertIn(f'href="{url}"', html)
        # The link can legitimately point at a *different* pass's own combined-eligible CSV than
        # the one currently being viewed (filename) -- e.g. viewing a plain --interval pass's full
        # column set while a separate --tree-heavy pass in the same run is the one that qualifies.
        self.assertNotIn("tree-heavy.interval.csv)</span>", html)  # page title still names filename, not url's CSV

    def test_default_omits_link_for_callers_that_dont_pass_one(self):
        html = server.render_interval_viewer("cpu2026", "801.xz_s", "20260816T020617", "amdtopdown.csv")
        self.assertNotIn("Tree + timeline (combined)", html)


if __name__ == "__main__":
    unittest.main()
