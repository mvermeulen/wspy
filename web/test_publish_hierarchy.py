#!/usr/bin/env python3
"""
web/test_publish_hierarchy.py -- unit tests for server.py's build_run_publish_levels()
(INVESTIGATION.md Tier 3 item 6: the "Publish to WordPress" button's hierarchy fix). Covers only
the pure, I/O-free levels-building logic -- the surrounding HTTP-route/form-parsing/WP-API-calling
plumbing (render_publish_result()) has no automated coverage, matching this codebase's existing
boundary for this class of feature (execute_analyze()/execute_testpoint_publish() are equally
untested; verified manually against a real running server instead). Not wired into make
test/run_tests.sh, matching this codebase's existing "web/ is stdlib-only Python, not covered by
the C toolchain's test targets" convention -- run standalone:

    python3 web/test_publish_hierarchy.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class BuildRunPublishLevelsTest(unittest.TestCase):
    def test_five_level_path_in_order(self):
        levels = server.build_run_publish_levels(
            "phoronix", "coremark", "default", "amd-395", "20260801T144909.213-5181153d")
        self.assertEqual(levels, [
            ("phoronix", "phoronix"),
            ("coremark", "coremark"),
            ("default", "default"),
            ("amd-395", "amd-395"),
            ("20260801T144909.213-5181153d", "20260801T144909.213-5181153d"),
        ])

    def test_every_level_has_matching_slug_and_title(self):
        levels = server.build_run_publish_levels(
            "cpu2026", "706.stockfish_r", "gcc_O3-base", "amd-395", "some-run-id")
        for slug, title in levels:
            self.assertEqual(slug, title)

    def test_run_id_is_the_leaf(self):
        levels = server.build_run_publish_levels("suite", "test", "test_point", "machine", "run-id")
        self.assertEqual(levels[-1], ("run-id", "run-id"))
        self.assertEqual(levels[-2], ("machine", "machine"))


if __name__ == "__main__":
    unittest.main()
