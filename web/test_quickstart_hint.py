#!/usr/bin/env python3
"""
web/test_quickstart_hint.py -- unit tests for server.py's item 3 (INVESTIGATION.md 4.4(a),
"Quickstart guide / guided onboarding path") web-UI half: render_index()'s first-run hint,
shown only when the homepage's "Recent reports" table would otherwise be empty. Not wired into
make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_quickstart_hint.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joblib
import server


def make_cfg(output_root, jobs_dir):
    # Every field render_index()'s own tab-rendering functions read, mirroring main()'s real cfg
    # dict -- none of these binary paths need to actually exist for HTML rendering (only for the
    # buttons' own POST-triggered execution, not covered here).
    return {
        "wspy_bin": "/fake/wspy", "wspy_run_bin": "/fake/wspy-run", "wspy_plot_bin": "/fake/wspy-plot",
        "proctree_bin": "/fake/proctree", "output_root": output_root,
        "wspy_validate_bin": "/fake/wspy-validate", "wspy_store_bin": "/fake/wspy-store",
        "wspy_summary_bin": "/fake/wspy-summary", "wspy_core_report_bin": "/fake/wspy-core-report",
        "wspy_analyze_bin": "/fake/wspy-analyze", "wspy_symbolize_bin": "/fake/wspy-symbolize",
        "wspy_ledger_bin": "/fake/wspy-ledger", "wspy_testpoint_bin": "/fake/wspy-testpoint",
        "wspy_archetype_bin": "/fake/wspy-archetype", "report_root": None, "report_root_remote": None,
        "run_index_file": os.path.join(output_root, "run_index.jsonl"),
        "store_db": os.path.join(output_root, "store.db"), "jobs_dir": jobs_dir,
        "phoronix_bin": "phoronix-test-suite", "phoronix_pts_dir": None,
        "cpu2026_dir": "/fake/cpu2026",
    }


class RenderIndexFirstRunHintTest(unittest.TestCase):
    def test_hint_shown_when_no_runs_exist(self):
        with tempfile.TemporaryDirectory() as output_root, tempfile.TemporaryDirectory() as jobs_dir:
            html = server.render_index(make_cfg(output_root, jobs_dir), {})
        self.assertIn("No runs yet.", html)
        self.assertIn("sudo ./wspy-run --suite demo --benchmark hello", html)
        self.assertIn("README.md", html)

    def test_hint_absent_once_a_run_exists(self):
        with tempfile.TemporaryDirectory() as output_root, tempfile.TemporaryDirectory() as jobs_dir:
            rundir = os.path.join(output_root, "demo", "hello", "run1")
            os.makedirs(rundir)
            with open(os.path.join(rundir, "manifest.json"), "w") as f:
                f.write('{"passes": []}')
            html = server.render_index(make_cfg(output_root, jobs_dir), {})
        self.assertNotIn("No runs yet.", html)
        self.assertIn("demo/hello/run1", html)

    def test_quickstart_hint_command_is_a_real_wspy_run_invocation(self):
        # Regression guard against the hint's own command drifting from what wspy-run actually
        # accepts -- exercises the exact flags via --dry-run rather than just eyeballing the string.
        wspy_run = os.path.join(joblib.REPO_ROOT, "wspy-run")
        if not os.path.isfile(wspy_run):
            self.skipTest("wspy-run not found in this checkout")
        import subprocess
        with tempfile.TemporaryDirectory() as outdir:
            argv = [wspy_run, "--dry-run", "--suite", "demo", "--benchmark", "hello",
                    "-o", outdir, "--run-index", os.path.join(outdir, "index.jsonl"),
                    "quick", "--", "sleep", "2"]
            result = subprocess.run(argv, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("sleep 2", result.stdout)


if __name__ == "__main__":
    unittest.main()
