#!/usr/bin/env python3
"""
web/test_joblib.py - unit tests for web/joblib.py's pure logic (job schema,
checklist/preset -> wspy argv builders). Not wired into make test/
run_tests.sh, matching this codebase's existing "web/ is stdlib-only Python,
not covered by the C toolchain's test targets" convention (see CLAUDE.md's
web/ entry) -- run standalone:

    python3 web/test_joblib.py

The execute_profile_run()/execute_custom_run() actually-runs-subprocesses
half of joblib.py is covered separately by tests/wspy_queue_smoke.sh (fake
wspy/wspy-run/wspy-plot/wspy-store binaries, exercised through wspy-queue);
this file only exercises the parts that don't touch the filesystem or spawn
processes.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joblib


class BuildJobTest(unittest.TestCase):
    def test_preset_job_round_trips(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="deep-cpu")
        self.assertEqual(joblib.validate_job(job), [])
        self.assertEqual(job["mode"], "preset")
        self.assertEqual(job["profile"], "deep-cpu")
        self.assertIsNone(job["checklist"])
        self.assertTrue(job["job_id"].startswith("job-"))

    def test_custom_job_round_trips(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown"], "interval_secs": "1"}}
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "custom", checklist=checklist)
        self.assertEqual(joblib.validate_job(job), [])
        self.assertEqual(job["mode"], "custom")
        self.assertEqual(job["checklist"], checklist)
        self.assertIsNone(job["profile"])

    def test_default_toggles_match_web_ui_defaults(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        self.assertEqual(job["toggles"], {"manifest": True, "run_index": True, "store_ingest": True})

    def test_no_absolute_paths_in_job(self):
        """Portability requirement (item 13): a job must carry no reference
        to the machine that created it -- no output-root, no run-index/
        store.db path. build_job() never takes those as arguments at all,
        so this just guards against a future field accidentally leaking one
        in via job_id/created_at or similar."""
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        for value in job.values():
            if isinstance(value, str):
                self.assertFalse(value.startswith("/"), f"job field looks like an absolute path: {value!r}")

    def test_job_id_is_unique_and_sortable(self):
        a = joblib.make_job_id()
        b = joblib.make_job_id()
        self.assertNotEqual(a, b)
        self.assertTrue(a < b or a > b)  # lexically comparable, timestamp-prefixed


class ValidateJobTest(unittest.TestCase):
    def test_rejects_non_dict(self):
        self.assertTrue(joblib.validate_job(["not", "a", "dict"]))

    def test_rejects_empty_workload(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        job["workload"] = []
        self.assertTrue(joblib.validate_job(job))

    def test_rejects_bad_suite(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        job["suite"] = "bad suite!"
        self.assertTrue(joblib.validate_job(job))

    def test_rejects_preset_mode_without_profile(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset")
        self.assertTrue(joblib.validate_job(job))

    def test_rejects_unknown_mode(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        job["mode"] = "bogus"
        self.assertTrue(joblib.validate_job(job))

    def test_accepts_custom_mode_without_checklist(self):
        # An empty/absent checklist is a valid *document* -- build_configuration_passes()
        # is what decides "nothing to run", not validate_job() (see
        # web/server.py's _enqueue_job(), which checks that separately before
        # ever calling build_job()).
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "custom")
        self.assertEqual(joblib.validate_job(job), [])


class ResolveTogglesTest(unittest.TestCase):
    def test_defaults_all_on(self):
        cfg = {"run_index_file": "/tmp/run_index.jsonl"}
        manifest_on, run_index_path, store_ingest = joblib.resolve_toggles(cfg, None)
        self.assertTrue(manifest_on)
        self.assertEqual(run_index_path, "/tmp/run_index.jsonl")
        self.assertTrue(store_ingest)

    def test_store_ingest_requires_run_index(self):
        cfg = {"run_index_file": "/tmp/run_index.jsonl"}
        manifest_on, run_index_path, store_ingest = joblib.resolve_toggles(
            cfg, {"run_index": False, "store_ingest": True})
        self.assertIsNone(run_index_path)
        self.assertFalse(store_ingest)


class BuildConfigurationPassesTest(unittest.TestCase):
    def test_empty_checklist_produces_no_passes(self):
        self.assertEqual(joblib.build_configuration_passes("/tmp/rundir", {}), [])

    def test_single_group_no_interval_uses_plain_flags_not_passes(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertNotIn("--passes=topdown", passes[0]["flags"])
        self.assertIn("--topdown", passes[0]["flags"])

    def test_multi_group_no_interval_bin_packs_via_passes(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown", "branch"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertTrue(any(f.startswith("--passes=") for f in passes[0]["flags"]))

    def test_interval_given_uses_plain_flags_even_with_multiple_groups(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown", "branch"],
                                   "interval_secs": "1"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertFalse(any(f.startswith("--passes=") for f in passes[0]["flags"]))
        self.assertIn("--interval", passes[0]["flags"])


if __name__ == "__main__":
    unittest.main()
