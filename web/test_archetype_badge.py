#!/usr/bin/env python3
"""
web/test_archetype_badge.py -- unit tests for server.py's characterization-badge generator
(INVESTIGATION.md 4.3 Tier 3 item 3, badges half): format_archetype_badge_markdown(),
find_representative_host_manifest(), and generate_archetype_badge(). Not wired into make test/
run_tests.sh, matching this codebase's existing "web/ is stdlib-only Python, not covered by the C
toolchain's test targets" convention -- run standalone:

    python3 web/test_archetype_badge.py

No network/subprocess: generate_archetype_badge()'s run_sync() call is mocked directly, same
convention web/test_wp_client.py already uses for wp_client.request().
"""
import json
import os
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class FormatArchetypeBadgeMarkdownTest(unittest.TestCase):
    def test_full_scorecard(self):
        md = server.format_archetype_badge_markdown({
            "resource_dominance": "compute-bound", "resource_dominance_pct": "82",
            "confidence": "high", "confidence_reasons": "decisive dominance margin",
            "parallelism_shape": "multi-threaded", "control_flow_style": "steady",
            "runtime_stability": "stable", "memory_attribution": "unknown",
        })
        self.assertIn("**compute-bound** (82%)", md)
        self.assertIn("confidence: **high**", md)
        self.assertIn("| parallelism_shape | multi-threaded |", md)
        self.assertIn("| control_flow_style | steady |", md)
        self.assertIn("| runtime_stability | stable |", md)
        self.assertIn("*decisive dominance margin*", md)
        self.assertIn("doc/PROFILE_COOKBOOK.md", md)

    def test_missing_fields_degrade_to_unknown_without_crashing(self):
        md = server.format_archetype_badge_markdown({})
        self.assertIn("**unknown**", md)
        self.assertIn("confidence: **unknown**", md)
        self.assertIn("| parallelism_shape | unknown |", md)
        # no resource_dominance_pct given -- no bare "()" left behind
        self.assertNotIn("()", md)

    def test_no_confidence_reasons_omits_that_line(self):
        md = server.format_archetype_badge_markdown({"resource_dominance": "memory-bound"})
        self.assertNotIn("confidence_reasons", md)


class FindRepresentativeHostManifestTest(unittest.TestCase):
    def test_wspy_run_layout_reads_first_pass_manifest(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            run_manifest = {
                "passes": [{"name": "counters", "manifest": "counters.manifest.json"}],
            }
            host_manifest = {"host": {"hostname": "roswell"}}
            with open(os.path.join(tmpdir, "counters.manifest.json"), "w") as f:
                json.dump(host_manifest, f)
            result = server.find_representative_host_manifest(tmpdir, run_manifest)
        self.assertEqual(result["host"]["hostname"], "roswell")

    def test_bare_manifest_fallback_when_no_run_manifest(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            host_manifest = {"host": {"hostname": "carlsbad"}}
            with open(os.path.join(tmpdir, server.MANIFEST_NAME), "w") as f:
                json.dump(host_manifest, f)
            result = server.find_representative_host_manifest(tmpdir, None)
        self.assertEqual(result["host"]["hostname"], "carlsbad")

    def test_nothing_resolves_returns_none(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertIsNone(server.find_representative_host_manifest(tmpdir, {"passes": []}))
            self.assertIsNone(server.find_representative_host_manifest(tmpdir, None))


class GenerateArchetypeBadgeTest(unittest.TestCase):
    """Builds a real rundir (<tmpdir>/<suite>/<benchmark>/<run_id>/) plus a real sqlite store.db with a
    path-correlated pass row -- the same directory-vs-store-run_id split PR #194 introduced for
    wspy-testpoint applies equally here (resolve_archetype_run_key() must resolve the *store's* own
    pass run_id, never the rundir's own directory name), so a fake store_db path with a mocked
    run_sync() alone (as this file's earlier draft did) would silently pass while actually shipping the
    same bug -- only a real sqlite lookup catches that class of mistake."""

    def _make_run(self, tmpdir, suite="cpu2026", benchmark="mybench", run_id="wraprun1",
                  hostname="roswell", store_run_id="pass-a", config_name="counters"):
        rundir = os.path.join(tmpdir, suite, benchmark, run_id)
        os.makedirs(rundir)
        with open(os.path.join(rundir, server.MANIFEST_NAME), "w") as f:
            json.dump({"host": {"hostname": hostname}}, f)
        db_path = os.path.join(tmpdir, "store.db")
        conn = sqlite3.connect(db_path)
        conn.execute("CREATE TABLE runs (hostname TEXT, run_id TEXT, config_name TEXT, "
                     "manifest_path TEXT, output_path TEXT)")
        if store_run_id is not None:
            manifest_path = os.path.join(rundir, "counters.manifest.json")
            conn.execute("INSERT INTO runs VALUES (?, ?, ?, ?, NULL)",
                         (hostname, store_run_id, config_name, manifest_path))
        conn.commit()
        conn.close()
        cfg = {"wspy_archetype_bin": "/repo/wspy-archetype", "store_db": db_path}
        return rundir, cfg, suite, benchmark, run_id

    def test_success_uses_resolved_store_run_id_not_directory_name(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(
                tmpdir, run_id="wraprun1", store_run_id="20260802T191322-abc123")
            output = "resource_dominance=compute-bound\nresource_dominance_pct=82\nconfidence=high\n"
            with patch("server.run_sync", return_value=(0, output, False)) as fake_run:
                ok, message = server.generate_archetype_badge(cfg, rundir, suite, benchmark, run_id)

            self.assertTrue(ok)
            self.assertIn("compute-bound", message)
            argv = fake_run.call_args[0][0]
            # the store's own pass run_id, NOT "wraprun1" (the directory name) -- this is the exact
            # assertion that would have caught the bug this test class's own docstring describes.
            self.assertEqual(argv, ["/repo/wspy-archetype", "--db", cfg["store_db"],
                                     "--run", "roswell:20260802T191322-abc123"])
            with open(os.path.join(rundir, server.ARCHETYPE_BADGE_NAME)) as f:
                content = f.read()
            self.assertIn("compute-bound", content)

    def test_not_in_store_fails_without_calling_run_sync(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir, store_run_id=None)
            with patch("server.run_sync") as fake_run:
                ok, message = server.generate_archetype_badge(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("not found", message)
            self.assertIn("wspy-store", message)
            fake_run.assert_not_called()
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_BADGE_NAME)))

    def test_no_hostname_fails_without_calling_run_sync(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir = os.path.join(tmpdir, "cpu2026", "mybench", "run1")
            os.makedirs(rundir)  # no manifest.json at all -> hostname unresolvable
            cfg = {"wspy_archetype_bin": "/repo/wspy-archetype",
                   "store_db": os.path.join(tmpdir, "store.db")}
            with patch("server.run_sync") as fake_run:
                ok, message = server.generate_archetype_badge(cfg, rundir, "cpu2026", "mybench", "run1")
            self.assertFalse(ok)
            self.assertIn("hostname", message)
            fake_run.assert_not_called()

    def test_nonzero_exit_fails_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            with patch("server.run_sync",
                       return_value=(1, "wspy-archetype: no such run in the store\n", False)):
                ok, message = server.generate_archetype_badge(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("no such run", message)
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_BADGE_NAME)))

    def test_timeout_fails_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            with patch("server.run_sync", return_value=(None, "", True)):
                ok, message = server.generate_archetype_badge(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("timed out", message)
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_BADGE_NAME)))


if __name__ == "__main__":
    unittest.main()
