#!/usr/bin/env python3
"""
web/test_compare_curation.py - unit tests for server.py's compare-view
curation layer (INVESTIGATION.md's "Give the report compare view its own
curation/annotation layer" item): compare_id_for_keys()'s order-independence
and exact-match behavior, and load_compare_curation()/save_compare_curation()'s
round-trip. Not wired into make test/run_tests.sh, matching web/'s existing
"stdlib-only Python, not covered by the C toolchain's test targets"
convention (see CLAUDE.md's web/ entry and web/test_joblib.py's own
docstring) -- run standalone:

    python3 web/test_compare_curation.py

resolve_compare_runs()/render_compare()/render_compare_curate_form()'s HTML
output and the actual HTTP routes are exercised by hand against a running
server (see this item's PR description), not re-tested here -- this file
covers the pure id/storage logic only, the same split test_trace_links.py
uses for its own item.
"""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class CompareIdForKeysTest(unittest.TestCase):
    def test_order_independent(self):
        a = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2"])
        b = server.compare_id_for_keys(["phoronix/coremark/r2", "phoronix/coremark/r1"])
        self.assertEqual(a, b)

    def test_different_run_set_gets_different_id(self):
        a = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2"])
        b = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r3"])
        self.assertNotEqual(a, b)

    def test_adding_a_run_changes_the_id(self):
        # Exact-match, not fuzzy: a superset of an existing comparison is a
        # genuinely different comparison, not "the same one plus an extra
        # column" -- same idiom summary.c's mixed-pmu check uses elsewhere.
        a = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2"])
        b = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2", "phoronix/coremark/r3"])
        self.assertNotEqual(a, b)

    def test_duplicate_keys_do_not_affect_id(self):
        a = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2"])
        b = server.compare_id_for_keys(["phoronix/coremark/r1", "phoronix/coremark/r2", "phoronix/coremark/r1"])
        self.assertEqual(a, b)

    def test_deterministic_across_calls(self):
        keys = ["phoronix/coremark/r1", "phoronix/coremark/r2"]
        self.assertEqual(server.compare_id_for_keys(keys), server.compare_id_for_keys(list(keys)))


class CompareCurationStorageTest(unittest.TestCase):
    def setUp(self):
        self.output_root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.output_root, ignore_errors=True)

    def test_load_missing_returns_none(self):
        self.assertIsNone(server.load_compare_curation(self.output_root, "no-such-id"))

    def test_save_then_load_round_trips(self):
        compare_id = "abc123"
        data = {
            "run_keys": ["phoronix/coremark/r1", "phoronix/coremark/r2"],
            "overview_note": "B added --power",
            "row_notes": {"amdtopdown.png": "higher backend-bound in B"},
        }
        server.save_compare_curation(self.output_root, compare_id, data)

        loaded = server.load_compare_curation(self.output_root, compare_id)
        self.assertIsNotNone(loaded)
        self.assertEqual(loaded["overview_note"], "B added --power")
        self.assertEqual(loaded["row_notes"], {"amdtopdown.png": "higher backend-bound in B"})
        self.assertEqual(loaded["schema_version"], server.COMPARE_SCHEMA_VERSION)
        self.assertIn("created", loaded)
        self.assertIn("updated", loaded)

    def test_save_creates_compares_directory(self):
        # save_compare_curation() must not assume <output_root>/compares
        # already exists -- unlike a per-run curation.json, whose rundir is
        # always already there by the time anything writes to it.
        compare_dir = os.path.join(self.output_root, server.COMPARE_CURATION_DIR)
        self.assertFalse(os.path.isdir(compare_dir))
        server.save_compare_curation(self.output_root, "abc123", {"row_notes": {}})
        self.assertTrue(os.path.isdir(compare_dir))

    def test_load_rejects_malformed_row_notes(self):
        compare_dir = os.path.join(self.output_root, server.COMPARE_CURATION_DIR)
        os.makedirs(compare_dir)
        with open(os.path.join(compare_dir, "bad.json"), "w") as f:
            f.write('{"overview_note": "x"}')  # no row_notes at all
        self.assertIsNone(server.load_compare_curation(self.output_root, "bad"))

    def test_resave_overwrites_in_place_same_file(self):
        # Same shape as apply_studio_post()'s own save_curation() call
        # (server.py:2701): a fresh dict each time, no "created" round-trip
        # attempted by the caller -- so re-saving genuinely replaces the
        # file's content, it just doesn't preserve "created" across calls
        # any more than the studio's own save path does.
        compare_id = "abc123"
        server.save_compare_curation(self.output_root, compare_id,
                                      {"run_keys": [], "overview_note": "first", "row_notes": {}})
        server.save_compare_curation(self.output_root, compare_id,
                                      {"run_keys": [], "overview_note": "second", "row_notes": {}})
        second = server.load_compare_curation(self.output_root, compare_id)
        self.assertEqual(second["overview_note"], "second")


class ResolveCompareRunsTest(unittest.TestCase):
    def setUp(self):
        self.output_root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.output_root, ignore_errors=True)

    def _make_run(self, suite, benchmark, run_id):
        rundir = os.path.join(self.output_root, suite, benchmark, run_id)
        os.makedirs(rundir)
        return rundir

    def test_fewer_than_two_valid_runs_returns_empty(self):
        self._make_run("phoronix", "coremark", "r1")
        runs = server.resolve_compare_runs(self.output_root, ["phoronix/coremark/r1"])
        self.assertEqual(runs, [])

    def test_nonexistent_run_directory_excluded(self):
        self._make_run("phoronix", "coremark", "r1")
        runs = server.resolve_compare_runs(
            self.output_root, ["phoronix/coremark/r1", "phoronix/coremark/does-not-exist"])
        self.assertEqual(runs, [])  # only 1 real run left, below the 2-run floor

    def test_two_valid_runs_resolve(self):
        self._make_run("phoronix", "coremark", "r1")
        self._make_run("phoronix", "coremark", "r2")
        runs = server.resolve_compare_runs(
            self.output_root, ["phoronix/coremark/r1", "phoronix/coremark/r2"])
        self.assertEqual(len(runs), 2)
        self.assertEqual(server.compare_run_keys(runs),
                          ["phoronix/coremark/r1", "phoronix/coremark/r2"])

    def test_duplicate_keys_deduped(self):
        self._make_run("phoronix", "coremark", "r1")
        self._make_run("phoronix", "coremark", "r2")
        runs = server.resolve_compare_runs(
            self.output_root,
            ["phoronix/coremark/r1", "phoronix/coremark/r2", "phoronix/coremark/r1"])
        self.assertEqual(len(runs), 2)

    def test_invalid_key_shape_ignored(self):
        self._make_run("phoronix", "coremark", "r1")
        self._make_run("phoronix", "coremark", "r2")
        runs = server.resolve_compare_runs(
            self.output_root, ["phoronix/coremark/r1", "phoronix/coremark/r2", "not-a-valid-key"])
        self.assertEqual(len(runs), 2)


class ApplyCompareCuratePostTest(unittest.TestCase):
    def setUp(self):
        self.output_root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.output_root, "phoronix", "coremark", "r1"))
        os.makedirs(os.path.join(self.output_root, "phoronix", "coremark", "r2"))
        self.keys = ["phoronix/coremark/r1", "phoronix/coremark/r2"]

    def tearDown(self):
        shutil.rmtree(self.output_root, ignore_errors=True)

    def test_saves_overview_and_row_notes(self):
        form = {
            "overview_note": ["B added --power"],
            "row_note__amdtopdown.png": ["higher backend-bound in B"],
            "row_note__systemtime.csv": [""],  # blank -- must not be stored
        }
        run_keys = server.apply_compare_curate_post(self.output_root, self.keys, form)
        self.assertEqual(run_keys, self.keys)

        compare_id = server.compare_id_for_keys(run_keys)
        saved = server.load_compare_curation(self.output_root, compare_id)
        self.assertEqual(saved["overview_note"], "B added --power")
        self.assertEqual(saved["row_notes"], {"amdtopdown.png": "higher backend-bound in B"})

    def test_clearing_a_note_removes_it_on_resave(self):
        server.apply_compare_curate_post(
            self.output_root, self.keys,
            {"overview_note": [""], "row_note__amdtopdown.png": ["some note"]})
        server.apply_compare_curate_post(
            self.output_root, self.keys,
            {"overview_note": [""], "row_note__amdtopdown.png": [""]})

        compare_id = server.compare_id_for_keys(self.keys)
        saved = server.load_compare_curation(self.output_root, compare_id)
        self.assertEqual(saved["row_notes"], {})

    def test_returns_none_when_run_set_no_longer_resolves(self):
        form = {"overview_note": ["x"]}
        result = server.apply_compare_curate_post(
            self.output_root, ["phoronix/coremark/does-not-exist"], form)
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
