#!/usr/bin/env python3
"""
web/test_testpoint_web.py -- unit tests for server.py's build_testpoint_publish_argv()
(INVESTIGATION.md's test-point README deep-dive, Tier 3 item 7: the report page's "Publish
test-point report" button). Covers only the pure, HTTP-free argv-construction logic -- the
surrounding subprocess/threading/SSE plumbing (execute_testpoint_publish(), TESTPOINT_RUNS) has no
automated coverage, matching this codebase's existing boundary for this class of feature
(execute_analyze()/ANALYZE_RUNS are equally untested; verified manually in a real browser instead,
see CLAUDE.md's UI-change convention). Not wired into make test/run_tests.sh, matching this
codebase's existing "web/ is stdlib-only Python, not covered by the C toolchain's test targets"
convention -- run standalone:

    python3 web/test_testpoint_web.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class BuildTestpointPublishArgvTest(unittest.TestCase):
    def setUp(self):
        self.cfg = {
            "wspy_testpoint_bin": "/repo/wspy-testpoint",
            "output_root": "/repo/web/runs",
            "store_db": "/repo/web/runs/store.db",
        }

    def test_select_runs_argv(self):
        select_runs_argv, _ = server.build_testpoint_publish_argv(
            self.cfg, "phoronix", "compress-7zip-default", "amd-395")
        self.assertEqual(select_runs_argv, [
            "/repo/wspy-testpoint", "select-runs",
            "--suite", "phoronix", "--benchmark", "compress-7zip-default", "--machine", "amd-395",
            "--output-root", "/repo/web/runs",
        ])

    def test_render_argv(self):
        _, render_argv = server.build_testpoint_publish_argv(
            self.cfg, "phoronix", "compress-7zip-default", "amd-395")
        self.assertEqual(render_argv, [
            "/repo/wspy-testpoint", "render",
            "--suite", "phoronix", "--benchmark", "compress-7zip-default", "--machine", "amd-395",
            "--db", "/repo/web/runs/store.db",
        ])

    def test_no_separate_aggregate_call(self):
        # render already shells out to wspy-summary/wspy-archetype itself -- a separate `aggregate`
        # subcommand invocation here would be redundant, nothing downstream consumes its output.
        select_runs_argv, render_argv = server.build_testpoint_publish_argv(
            self.cfg, "manual", "mybench", "test-machine")
        self.assertNotIn("aggregate", select_runs_argv)
        self.assertNotIn("aggregate", render_argv)

    def test_uses_cfg_values_not_hardcoded_defaults(self):
        # Respects whatever this server instance is actually configured to use, rather than falling
        # back to wspy-testpoint's own separate CLI defaults.
        cfg = dict(self.cfg, output_root="/custom/outroot", store_db="/custom/store.db")
        select_runs_argv, render_argv = server.build_testpoint_publish_argv(
            cfg, "manual", "mybench", "test-machine")
        self.assertIn("/custom/outroot", select_runs_argv)
        self.assertIn("/custom/store.db", render_argv)

    def test_report_root_unset_by_default(self):
        # No --report-root/--report-root-remote flags at all when unset, so wspy-testpoint falls
        # through to its own default chain (real deployments should get the real configured
        # report-root, not something this function silently overrides).
        select_runs_argv, render_argv = server.build_testpoint_publish_argv(
            self.cfg, "manual", "mybench", "test-machine")
        self.assertNotIn("--report-root", select_runs_argv)
        self.assertNotIn("--report-root", render_argv)

    def test_report_root_override_passed_to_both_commands(self):
        cfg = dict(self.cfg, report_root="/scratch/reportroot", report_root_remote="/scratch/remote.git")
        select_runs_argv, render_argv = server.build_testpoint_publish_argv(
            cfg, "manual", "mybench", "test-machine")
        for argv in (select_runs_argv, render_argv):
            self.assertIn("--report-root", argv)
            self.assertIn("/scratch/reportroot", argv)
            self.assertIn("--report-root-remote", argv)
            self.assertIn("/scratch/remote.git", argv)


if __name__ == "__main__":
    unittest.main()
