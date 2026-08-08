#!/usr/bin/env python3
"""
web/test_testpoint_web.py -- unit tests for server.py's build_testpoint_publish_argv() and
build_reference_publish_argv() (INVESTIGATION.md's test-point README deep-dive, Tier 3 item 7: the
report page's "Publish test-point report" button; and the Reference tab's "Publish reference matrix"
button, wrapping scripts/publish_reference_matrix.py). Covers only the pure, HTTP-free
argv-construction logic -- the surrounding subprocess/threading/SSE plumbing
(execute_testpoint_publish()/TESTPOINT_RUNS, execute_reference_publish()/REFERENCE_PUBLISH_RUNS) has
no automated coverage, matching this codebase's existing boundary for this class of feature
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

REFERENCE_PUBLISH_SCRIPT = os.path.join(server.REPO_ROOT, "scripts", "publish_reference_matrix.py")


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


class BuildReferencePublishArgvTest(unittest.TestCase):
    def setUp(self):
        self.cfg = {
            "wspy_testpoint_bin": "/repo/wspy-testpoint",
            "store_db": "/repo/web/runs/store.db",
        }

    def test_dry_run_defaults_true(self):
        # A web button is a much easier way to fat-finger a real publish than a deliberately-typed
        # terminal command -- the safe default belongs at this layer, opposite of the CLI script's
        # own default of False.
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertIn("--dry-run", argv)

    def test_dry_run_false_when_explicitly_disabled(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [], dry_run=False)
        self.assertNotIn("--dry-run", argv)

    def test_empty_suites_and_machines_mean_no_filter(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--suite", argv)
        self.assertNotIn("--machine", argv)

    def test_suites_and_machines_passed_as_repeatable_flags(self):
        argv = server.build_reference_publish_argv(
            self.cfg, ["phoronix", "cpu2026"], ["amd-370-64gb", "amd-395-96gb"])
        self.assertEqual(argv.count("--suite"), 2)
        self.assertIn("phoronix", argv)
        self.assertIn("cpu2026", argv)
        self.assertEqual(argv.count("--machine"), 2)
        self.assertIn("amd-370-64gb", argv)
        self.assertIn("amd-395-96gb", argv)

    def test_skip_wordpress_discovery_off_by_default(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--skip-wordpress-discovery", argv)

    def test_skip_wordpress_discovery_when_requested(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [], skip_wordpress_discovery=True)
        self.assertIn("--skip-wordpress-discovery", argv)

    def test_skip_local_store_off_by_default(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--skip-local-store", argv)

    def test_skip_local_store_when_requested(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [], skip_local_store=True)
        self.assertIn("--skip-local-store", argv)

    def test_publish_off_by_default(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--publish", argv)

    def test_publish_when_requested(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [], do_publish=True)
        self.assertIn("--publish", argv)

    def test_force_off_by_default(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--force", argv)

    def test_force_when_requested(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [], force=True)
        self.assertIn("--force", argv)

    def test_uses_cfg_db_and_testpoint_bin(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertIn("--db", argv)
        self.assertIn("/repo/web/runs/store.db", argv)
        self.assertIn("--wspy-testpoint-bin", argv)
        self.assertIn("/repo/wspy-testpoint", argv)

    def test_report_root_unset_by_default(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertNotIn("--report-root", argv)

    def test_report_root_override_passed_through(self):
        cfg = dict(self.cfg, report_root="/scratch/reportroot", report_root_remote="/scratch/remote.git")
        argv = server.build_reference_publish_argv(cfg, [], [])
        self.assertIn("--report-root", argv)
        self.assertIn("/scratch/reportroot", argv)
        self.assertIn("--report-root-remote", argv)
        self.assertIn("/scratch/remote.git", argv)

    def test_invokes_the_real_script_via_current_interpreter(self):
        argv = server.build_reference_publish_argv(self.cfg, [], [])
        self.assertEqual(argv[0], sys.executable)
        self.assertEqual(argv[1], REFERENCE_PUBLISH_SCRIPT)


if __name__ == "__main__":
    unittest.main()
