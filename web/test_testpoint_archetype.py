#!/usr/bin/env python3
"""
web/test_testpoint_archetype.py -- unit tests for wspy-testpoint's
collect_wordpress_archetype_scorecards()/render_archetype_section() (INVESTIGATION.md 4.3 item 23:
wspy-archetype characterization for WordPress-recovered machines with no local wspy-store presence).
Not wired into make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C
toolchain's test targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_testpoint_archetype.py

wspy-testpoint has a hyphenated filename (not a plain importable module) -- loaded here via
importlib.util, same approach a caller would need regardless of where this test file lives.
subprocess.run and server.py's WordPress-facing functions are mocked directly; no network/subprocess
access.
"""
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import unittest
from unittest.mock import patch

WEB_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(WEB_DIR)
sys.path.insert(0, WEB_DIR)

# wspy-testpoint has no .py suffix, so spec_from_file_location() can't infer a loader from the
# extension alone -- an explicit SourceFileLoader is needed.
_loader = importlib.machinery.SourceFileLoader("wspy_testpoint", os.path.join(REPO_ROOT, "wspy-testpoint"))
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
testpoint = importlib.util.module_from_spec(_spec)
sys.modules["wspy_testpoint"] = testpoint  # so unittest.mock.patch("wspy_testpoint.xxx") can resolve it
_loader.exec_module(testpoint)


class CollectWordpressArchetypeScorecardsTest(unittest.TestCase):
    def test_empty_when_no_wordpress_configured(self):
        scorecards = testpoint.collect_wordpress_archetype_scorecards(
            "wspy-archetype", None, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(scorecards, [])

    def test_skips_the_local_machine_itself(self):
        with patch("wspy_testpoint.web_export.resolve_reference_matrix_row_publish_status",
                   return_value={"amd-395": {"link": "x", "status": "publish"}}), \
             patch("wspy_testpoint.web_export.recover_machine_metrics_from_wordpress") as mock_recover:
            scorecards = testpoint.collect_wordpress_archetype_scorecards(
                "wspy-archetype", {"wordpress": {}}, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(scorecards, [])
        mock_recover.assert_not_called()

    def test_scores_each_other_published_machine(self):
        def fake_status(wp_cfg, suite, test, test_point):
            return {"amd-395": {}, "amd-395-96gb": {}, "amd-370-64gb": {}}

        def fake_recover(wp_cfg, suite, test, test_point, machine):
            return [{"metric": "retire_pct", "mean": "70.0"}, {"metric": "frontend_pct", "mean": "5.0"},
                    {"metric": "backend_pct", "mean": "20.0"}, {"metric": "speculate_pct", "mean": "5.0"}]

        captured_argvs = []

        def fake_run(argv, capture_output, text):
            captured_argvs.append(argv)
            with open(argv[2]) as f:
                payload = json.load(f)
            self.assertEqual(payload["retire_pct"], 70.0)
            return subprocess.CompletedProcess(
                argv, 0, stdout="hostname=(guest)\nrun_id=(guest)\nresource_dominance=compute-bound\n"
                                 "confidence=medium\n", stderr="")

        with patch("wspy_testpoint.web_export.resolve_reference_matrix_row_publish_status",
                   side_effect=fake_status), \
             patch("wspy_testpoint.web_export.recover_machine_metrics_from_wordpress",
                   side_effect=fake_recover), \
             patch("wspy_testpoint.subprocess.run", side_effect=fake_run):
            scorecards = testpoint.collect_wordpress_archetype_scorecards(
                "wspy-archetype", {"wordpress": {}}, "phoronix", "coremark", "default", "amd-395")

        # amd-395 (the local machine) excluded -- only the other two get scored.
        self.assertEqual(len(captured_argvs), 2)
        machines = sorted(s["machine"] for s in scorecards)
        self.assertEqual(machines, ["amd-370-64gb", "amd-395-96gb"])
        self.assertTrue(all(s["resource_dominance"] == "compute-bound" for s in scorecards))

    def test_machine_with_no_recovered_rows_is_skipped(self):
        def fake_status(wp_cfg, suite, test, test_point):
            return {"amd-395": {}, "amd-395-96gb": {}}

        with patch("wspy_testpoint.web_export.resolve_reference_matrix_row_publish_status",
                   side_effect=fake_status), \
             patch("wspy_testpoint.web_export.recover_machine_metrics_from_wordpress", return_value=[]), \
             patch("wspy_testpoint.subprocess.run") as mock_run:
            scorecards = testpoint.collect_wordpress_archetype_scorecards(
                "wspy-archetype", {"wordpress": {}}, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(scorecards, [])
        mock_run.assert_not_called()

    def test_launch_failure_for_one_machine_does_not_abort_others(self):
        def fake_status(wp_cfg, suite, test, test_point):
            return {"amd-395": {}, "bad-machine": {}, "good-machine": {}}

        def fake_recover(wp_cfg, suite, test, test_point, machine):
            return [{"metric": "retire_pct", "mean": "10.0"}]

        call_count = {"n": 0}

        def fake_run_first_fails(argv, capture_output, text):
            call_count["n"] += 1
            if call_count["n"] == 1:
                raise OSError("no such binary")
            return subprocess.CompletedProcess(
                argv, 0, stdout="resource_dominance=memory-bound\nconfidence=low\n", stderr="")

        with patch("wspy_testpoint.web_export.resolve_reference_matrix_row_publish_status",
                   side_effect=fake_status), \
             patch("wspy_testpoint.web_export.recover_machine_metrics_from_wordpress",
                   side_effect=fake_recover), \
             patch("wspy_testpoint.subprocess.run", side_effect=fake_run_first_fails):
            scorecards = testpoint.collect_wordpress_archetype_scorecards(
                "wspy-archetype", {"wordpress": {}}, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(len(scorecards), 1)

    def test_writes_temp_file_and_cleans_it_up(self):
        written_paths = []

        def fake_status(wp_cfg, suite, test, test_point):
            return {"amd-395": {}, "other-machine": {}}

        def fake_recover(wp_cfg, suite, test, test_point, machine):
            return [{"metric": "retire_pct", "mean": "10.0"}]

        def fake_run(argv, capture_output, text):
            written_paths.append(argv[2])
            self.assertTrue(os.path.isfile(argv[2]))
            return subprocess.CompletedProcess(argv, 0, stdout="resource_dominance=unknown\n", stderr="")

        with patch("wspy_testpoint.web_export.resolve_reference_matrix_row_publish_status",
                   side_effect=fake_status), \
             patch("wspy_testpoint.web_export.recover_machine_metrics_from_wordpress",
                   side_effect=fake_recover), \
             patch("wspy_testpoint.subprocess.run", side_effect=fake_run):
            testpoint.collect_wordpress_archetype_scorecards(
                "wspy-archetype", {"wordpress": {}}, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(len(written_paths), 1)
        self.assertFalse(os.path.isfile(written_paths[0]))  # cleaned up after use


class RenderArchetypeSectionTest(unittest.TestCase):
    def test_none_when_nothing_collected_at_all(self):
        self.assertIsNone(testpoint.render_archetype_section([], []))
        self.assertIsNone(testpoint.render_archetype_section([], None))

    def test_local_only_unchanged_from_original_behavior(self):
        scorecards = [{"run_id": "r1", "resource_dominance": "compute-bound", "confidence": "high"}]
        md = testpoint.render_archetype_section(scorecards)
        self.assertIn("Consistent", md)
        self.assertNotIn("WordPress-recovered", md)

    def test_wordpress_only_still_renders_when_local_has_no_known_dominance(self):
        scorecards = [{"run_id": "r1", "resource_dominance": "unknown"}]
        wp_scorecards = [{"machine": "amd-395-96gb", "resource_dominance": "memory-bound",
                           "confidence": "medium"}]
        md = testpoint.render_archetype_section(scorecards, wp_scorecards)
        self.assertIsNotNone(md)
        self.assertIn("WordPress-recovered peers", md)
        self.assertIn("amd-395-96gb", md)
        self.assertNotIn("Consistent", md)  # no local table, since local has nothing known

    def test_wordpress_section_never_merged_into_local_consistency_verdict(self):
        scorecards = [{"run_id": "r1", "resource_dominance": "compute-bound", "confidence": "high"}]
        wp_scorecards = [{"machine": "amd-395-96gb", "resource_dominance": "memory-bound",
                           "confidence": "medium"}]
        md = testpoint.render_archetype_section(scorecards, wp_scorecards)
        # Still says "Consistent" for the one real local run -- the WordPress-recovered machine's
        # differing classification must not flip this to "Diverges".
        self.assertIn("**Consistent**", md)
        self.assertIn("Differs from this machine's own", md)

    def test_agrees_note_when_wordpress_matches_local(self):
        scorecards = [{"run_id": "r1", "resource_dominance": "compute-bound", "confidence": "high"}]
        wp_scorecards = [{"machine": "amd-395-96gb", "resource_dominance": "compute-bound",
                           "confidence": "medium"}]
        md = testpoint.render_archetype_section(scorecards, wp_scorecards)
        self.assertIn("Agrees with this machine's own", md)

    def test_wordpress_scorecards_with_unknown_dominance_excluded_from_table_note(self):
        scorecards = [{"run_id": "r1", "resource_dominance": "compute-bound", "confidence": "high"}]
        wp_scorecards = [{"machine": "amd-395-96gb", "resource_dominance": "unknown"}]
        md = testpoint.render_archetype_section(scorecards, wp_scorecards)
        self.assertNotIn("WordPress-recovered peers", md)


if __name__ == "__main__":
    unittest.main()
