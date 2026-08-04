#!/usr/bin/env python3
"""
web/test_archetype_similar.py -- unit tests for server.py's similarity-panel generator
(INVESTIGATION.md 4.3 Tier 3 item 3, similarity-panels half): format_archetype_similar_markdown() and
generate_archetype_similar(). Not wired into make test/run_tests.sh, matching
web/test_archetype_badge.py's own "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention -- run standalone:

    python3 web/test_archetype_similar.py

No network/subprocess: generate_archetype_similar()'s run_sync() call is mocked directly, same
convention web/test_archetype_badge.py already uses for generate_archetype_badge().
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


class FormatArchetypeSimilarMarkdownTest(unittest.TestCase):
    def test_neighbors_render_as_table_with_links(self):
        md = server.format_archetype_similar_markdown([
            {"hostname": "roswell", "run_id": "run1", "distance": "0.064", "compared_features": "5",
             "link": "/report/phoronix/coremark-default/run1dir"},
            {"hostname": "roswell", "run_id": "run2", "distance": "0.75", "compared_features": "5",
             "link": None},
        ])
        self.assertIn("[roswell:run1](/report/phoronix/coremark-default/run1dir)", md)
        self.assertIn("| 0.064 | 5 |", md)
        self.assertIn("roswell:run2 (report not available)", md)
        self.assertIn("doc/PROFILE_COOKBOOK.md", md)

    def test_empty_neighbor_list_renders_explanatory_sentence_not_empty_table(self):
        md = server.format_archetype_similar_markdown([])
        self.assertIn("No other collected runs", md)
        self.assertNotIn("|---|---|---|", md)


class GenerateArchetypeSimilarTest(unittest.TestCase):
    """Same real-rundir + real-sqlite-store approach as GenerateArchetypeBadgeTest
    (web/test_archetype_badge.py) and for the same reason: resolve_archetype_run_key() must resolve the
    *store's* own pass run_id, never the rundir's own directory name, and generate_archetype_similar()
    additionally needs a real runs table to resolve each neighbor's own directory via
    joblib.resolve_store_run_directory() -- a fully-mocked store would silently pass either bug."""

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

    def _add_neighbor_row(self, cfg, hostname, neighbor_store_run_id, suite, benchmark, neighbor_run_id):
        neighbor_dir = os.path.join(os.path.dirname(cfg["store_db"]), suite, benchmark, neighbor_run_id)
        conn = sqlite3.connect(cfg["store_db"])
        conn.execute("INSERT INTO runs VALUES (?, ?, 'topdown', NULL, ?)",
                     (hostname, neighbor_store_run_id, os.path.join(neighbor_dir, "amdtopdown.csv")))
        conn.commit()
        conn.close()

    def test_success_resolves_neighbor_links_and_writes_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(
                tmpdir, run_id="wraprun1", store_run_id="20260802T191322-abc123")
            self._add_neighbor_row(cfg, "roswell", "neighbor-1", "phoronix", "coremark-default",
                                    "othrun1")
            output = "hostname,run_id,distance,compared_features\nroswell,neighbor-1,0.064,5\n"
            with patch("server.run_sync", return_value=(0, output, False)) as fake_run:
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)

            self.assertTrue(ok)
            self.assertIn("1 similar run found", message)
            argv = fake_run.call_args[0][0]
            # the store's own pass run_id, NOT "wraprun1" (the directory name) -- this is the exact
            # assertion that would have caught the bug this test class's own docstring describes.
            self.assertEqual(argv, ["/repo/wspy-archetype", "--db", cfg["store_db"],
                                     "--nearest", "roswell:20260802T191322-abc123", "--csv"])
            with open(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)) as f:
                content = f.read()
            self.assertIn("[roswell:neighbor-1](/report/phoronix/coremark-default/othrun1)", content)

    def test_neighbor_with_unresolvable_directory_still_renders(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            # no matching row for "ghost-run" in the store at all
            output = "hostname,run_id,distance,compared_features\nroswell,ghost-run,1.2,3\n"
            with patch("server.run_sync", return_value=(0, output, False)):
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)
            self.assertTrue(ok)
            with open(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)) as f:
                content = f.read()
            self.assertIn("roswell:ghost-run (report not available)", content)

    def test_zero_neighbors_still_succeeds_and_writes_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            output = "hostname,run_id,distance,compared_features\n"
            with patch("server.run_sync", return_value=(0, output, False)):
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)
            self.assertTrue(ok)
            self.assertEqual(message, "no similar runs found")
            with open(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)) as f:
                content = f.read()
            self.assertIn("No other collected runs", content)

    def test_not_in_store_fails_without_calling_run_sync(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir, store_run_id=None)
            with patch("server.run_sync") as fake_run:
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("not found", message)
            fake_run.assert_not_called()
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)))

    def test_nonzero_exit_fails_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            with patch("server.run_sync",
                       return_value=(1, "wspy-archetype: no such run in the store\n", False)):
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("no such run", message)
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)))

    def test_timeout_fails_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rundir, cfg, suite, benchmark, run_id = self._make_run(tmpdir)
            with patch("server.run_sync", return_value=(None, "", True)):
                ok, message = server.generate_archetype_similar(cfg, rundir, suite, benchmark, run_id)
            self.assertFalse(ok)
            self.assertIn("timed out", message)
            self.assertFalse(os.path.exists(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME)))


if __name__ == "__main__":
    unittest.main()
