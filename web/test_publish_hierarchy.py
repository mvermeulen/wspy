#!/usr/bin/env python3
"""
web/test_publish_hierarchy.py -- unit tests for server.py's build_run_publish_levels() and
build_run_publish_stub_content() (INVESTIGATION.md Tier 3 item 6: the "Publish to WordPress"
button's hierarchy fix, plus its machine-catalog-link and Phoronix-test-point-content follow-ups).
Covers only the pure, I/O-free levels/stub-content-building logic -- the surrounding
HTTP-route/form-parsing/WP-API-calling plumbing (render_publish_result()) has no automated coverage,
matching this codebase's existing boundary for this class of feature
(execute_analyze()/execute_testpoint_publish() are equally untested; verified manually against a
real running server instead). Not wired into make test/run_tests.sh, matching this codebase's
existing "web/ is stdlib-only Python, not covered by the C toolchain's test targets" convention --
run standalone:

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


class BuildRunPublishStubContentTest(unittest.TestCase):
    def test_machine_only_when_no_phoronix_entry(self):
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/")
        self.assertEqual(list(stub_content.keys()), [3])
        self.assertIn("https://example.org/workload/machine/amd-395/", stub_content[3])

    def test_includes_test_point_content_when_phoronix_entry_given(self):
        entry = {"options_slug": "default", "test_id": "pts/coremark-1.0.1", "arguments": ""}
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/", phoronix_entry=entry)
        self.assertEqual(set(stub_content.keys()), {2, 3})
        self.assertIn("pts/coremark-1.0.1", stub_content[2])
        self.assertIn("default", stub_content[2])

    def test_test_point_content_matches_joblib_test_point_wp_content(self):
        import joblib
        entry = {"options_slug": "sha256", "test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/", phoronix_entry=entry)
        self.assertEqual(stub_content[2], joblib.test_point_wp_content(entry))

    def test_includes_test_point_content_when_cpu2026_entry_given(self):
        entry = {"tag": "gcc_O3", "tune": "base", "config_file": "gcc_O3.cfg", "built": True}
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/", cpu2026_entry=entry)
        self.assertEqual(set(stub_content.keys()), {2, 3})
        self.assertIn("gcc_O3.cfg", stub_content[2])
        self.assertIn("base", stub_content[2])

    def test_cpu2026_content_matches_joblib_cpu2026_test_point_wp_content(self):
        import joblib
        entry = {"tag": "gcc_O3", "tune": "peak", "config_file": "gcc_O3.cfg", "built": False}
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/", cpu2026_entry=entry)
        self.assertEqual(stub_content[2], joblib.cpu2026_test_point_wp_content(entry))

    def test_phoronix_entry_takes_precedence_over_cpu2026_entry(self):
        # Mutually exclusive in practice (a run is either phoronix or cpu2026), but confirm the
        # precedence is deterministic rather than accidental.
        phoronix_entry = {"options_slug": "default", "test_id": "pts/coremark-1.0.1", "arguments": ""}
        cpu2026_entry = {"tag": "gcc_O3", "tune": "base", "config_file": "gcc_O3.cfg", "built": True}
        stub_content = server.build_run_publish_stub_content(
            "https://example.org/workload/machine/amd-395/",
            phoronix_entry=phoronix_entry, cpu2026_entry=cpu2026_entry)
        self.assertIn("pts/coremark-1.0.1", stub_content[2])


if __name__ == "__main__":
    unittest.main()
