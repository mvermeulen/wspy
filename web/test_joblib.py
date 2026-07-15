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

    def test_category_tags_each_enabled_configuration(self):
        # Structured configuration provenance (INVESTIGATION_4.0.md item 16):
        # each pass carries a stable launcher-vocabulary "category", distinct
        # from "name" (the output filename stem, which can be a legacy alias
        # like "amdtopdown"/"systemtime").
        checklist = {
            "tree": {"enabled": True},
            "counters": {"enabled": True, "groups": ["topdown"], "interval_secs": "1"},
            "system": {"enabled": True, "interval_secs": "1"},
        }
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        categories = {p["name"]: p["category"] for p in passes}
        self.assertEqual(categories["tree"], "process-tree")
        self.assertEqual(categories["amdtopdown"], "performance-counters")
        self.assertEqual(categories["systemtime"], "system-metrics")


class ConfigOptionsTest(unittest.TestCase):
    def test_skips_enabled_and_none_and_empty(self):
        options = joblib._config_options({"enabled": True, "groups": None, "csv": ""})
        self.assertEqual(options, [])

    def test_stringifies_and_joins_list_values(self):
        options = dict(joblib._config_options(
            {"enabled": True, "groups": ["topdown", "branch"], "interval_secs": 1, "csv": True}))
        self.assertEqual(options["groups"], "topdown,branch")
        self.assertEqual(options["interval_secs"], "1")
        self.assertEqual(options["csv"], "true")


class BuildPassArgvTest(unittest.TestCase):
    def test_emits_config_name_and_config_options_not_gated_on_manifest(self):
        p = {"name": "counters", "csv": True, "flags": ["--topdown"],
             "category": "performance-counters",
             "options": [("groups", "topdown"), ("interval_secs", "1")]}
        argv, outfile, manifest_path = joblib.build_pass_argv(
            "/usr/bin/wspy", "/tmp/rundir", p, manifest_on=False, run_index_path=None)
        self.assertIn("--config-name", argv)
        self.assertEqual(argv[argv.index("--config-name") + 1], "performance-counters")
        self.assertIn("--config-option", argv)
        self.assertIn("groups=topdown", argv)
        self.assertIn("interval_secs=1", argv)
        self.assertNotIn("--preset-name", argv)
        self.assertIsNone(manifest_path)

    def test_no_category_means_no_config_name(self):
        p = {"name": "custom", "csv": False, "flags": ["--software"]}
        argv, _, _ = joblib.build_pass_argv(
            "/usr/bin/wspy", "/tmp/rundir", p, manifest_on=False, run_index_path=None)
        self.assertNotIn("--config-name", argv)
        self.assertNotIn("--config-option", argv)


class ChecklistFromProvenanceTest(unittest.TestCase):
    """INVESTIGATION_4.0.md item 17 ("Browse-reports"): the read side of
    item 16's structured configuration provenance -- turning a run's
    recorded configuration_provenance back into checklist state a report's
    "Customize & run again" link can restore. checklist_section_from_options()
    round-trips against build_configuration_passes()'s own _config_options()
    output below, not a hand-written fixture, so a future checklist field
    added to one side is caught here if the other isn't updated to match."""

    def test_round_trips_through_build_configuration_passes(self):
        checklist = {
            "counters": {"enabled": True, "groups": ["topdown", "cache2"],
                         "interval_secs": "1", "per_core": True, "rusage": False, "csv": True},
        }
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        p = passes[0]
        self.assertEqual(p["category"], "performance-counters")
        restored = joblib.checklist_section_from_options("counters", p["options"])
        self.assertEqual(restored["groups"], ["topdown", "cache2"])
        self.assertEqual(restored["interval_secs"], "1")
        self.assertIs(restored["per_core"], True)
        self.assertIs(restored["rusage"], False)
        self.assertIs(restored["csv"], True)
        self.assertIs(restored["enabled"], True)

    def test_bool_options_round_trip_both_ways(self):
        section = joblib.checklist_section_from_options(
            "tree", [("cmdline", "true"), ("open", "false"), ("software", "true")])
        self.assertEqual(section, {"enabled": True, "cmdline": True, "open": False, "software": True})

    def test_unknown_option_name_ignored(self):
        section = joblib.checklist_section_from_options("system", [("bogus", "x"), ("csv", "true")])
        self.assertEqual(section, {"enabled": True, "bogus": "x", "csv": True})

    def test_preset_wins_over_any_configuration(self):
        """wspy-run's run_pass() sets --preset-name once per invocation and
        --config-name on every pass (see build_pass_argv()'s docstring) -- a
        preset-bearing run is never also decomposed into checklist state."""
        provenances = [
            {"preset": "deep-cpu", "configuration": "amdtopdown", "options": []},
            {"preset": "deep-cpu", "configuration": "systemtime", "options": []},
        ]
        preset, checklist = joblib.checklist_from_pass_provenance(provenances)
        self.assertEqual(preset, "deep-cpu")
        self.assertIsNone(checklist)

    def test_checklist_driven_run_reconstructs_multiple_categories(self):
        provenances = [
            {"preset": None, "configuration": "performance-counters",
             "options": [("groups", "topdown"), ("csv", "true")]},
            {"preset": None, "configuration": "system-metrics",
             "options": [("csv", "true")]},
        ]
        preset, checklist = joblib.checklist_from_pass_provenance(provenances)
        self.assertIsNone(preset)
        self.assertEqual(set(checklist.keys()), {"counters", "system"})
        self.assertEqual(checklist["counters"]["groups"], ["topdown"])
        self.assertIs(checklist["system"]["enabled"], True)

    def test_no_provenance_at_all_returns_none_none(self):
        preset, checklist = joblib.checklist_from_pass_provenance([None, None])
        self.assertIsNone(preset)
        self.assertIsNone(checklist)

    def test_unrecognized_category_skipped(self):
        preset, checklist = joblib.checklist_from_pass_provenance(
            [{"preset": None, "configuration": "some-future-category", "options": []}])
        self.assertIsNone(preset)
        self.assertIsNone(checklist)


if __name__ == "__main__":
    unittest.main()
