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
import io
import json
import os
import sys
import tarfile
import tempfile
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
        self.assertIn("--counters=topdown", passes[0]["flags"])

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

    def test_ibs_defaults_to_aggregate_no_interval(self):
        checklist = {"ibs": {"enabled": True, "profile": "basic"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--ibs-basic", passes[0]["flags"])
        self.assertNotIn("--interval", passes[0]["flags"])

    def test_ibs_interval_given_adds_interval_flag(self):
        checklist = {"ibs": {"enabled": True, "profile": "memory-deep", "interval_secs": "1"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--ibs-memory-deep", passes[0]["flags"])
        idx = passes[0]["flags"].index("--interval")
        self.assertEqual(passes[0]["flags"][idx + 1], "1")

    def test_ibs_maxcnt_ldlat_fetchlat_overrides(self):
        checklist = {"ibs": {"enabled": True, "profile": "memory-deep",
                              "maxcnt": "500", "ldlat": "128", "fetchlat": "256"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        flags = passes[0]["flags"]
        self.assertEqual(flags[flags.index("--ibs-maxcnt") + 1], "500")
        self.assertEqual(flags[flags.index("--ibs-ldlat") + 1], "128")
        self.assertEqual(flags[flags.index("--ibs-fetchlat") + 1], "256")

    def test_category_tags_each_enabled_configuration(self):
        # Structured configuration provenance (INVESTIGATION.md's "What
        # shipped in 4.1"): each pass carries a stable launcher-vocabulary
        # "category", distinct
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

    def test_counters_power_checkbox_adds_power_flag(self):
        # power has no card/pass of its own -- it's a checkbox inside
        # "counters" (and "system", see below), folded into that same pass.
        checklist = {"counters": {"enabled": True, "groups": ["topdown"], "power": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--power", passes[0]["flags"])
        self.assertEqual(passes[0]["category"], "performance-counters")

    def test_counters_power_forces_plain_flags_over_passes_bin_packing(self):
        # --passes fatals against --power (wspy.c), so checking "power" must
        # bypass the --passes bin-packing branch even with 2+ groups and no
        # interval given.
        checklist = {"counters": {"enabled": True, "groups": ["topdown", "cache2"], "power": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertIn("--power", flags)
        self.assertFalse(any(f.startswith("--passes=") for f in flags))

    def test_counters_per_core_checkbox_adds_per_core_flag_with_no_interval(self):
        # Regression test: per_core used to be gated on "interval is not
        # None", so leaving the interval field blank (aggregate mode --
        # the only mode that produced a wspy CSV with a "core" column at
        # the time) silently dropped --per-core.
        checklist = {"counters": {"enabled": True, "groups": ["topdown"], "per_core": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--per-core", passes[0]["flags"])
        self.assertNotIn("--interval", passes[0]["flags"])

    def test_counters_per_core_honors_interval(self):
        # Regression test: per_core used to force interval=None
        # unconditionally, because --per-core + --interval produced a wspy
        # CSV with no "core" column. wspy.c fixed that (one row per core per
        # tick), and wspy-core-report/wspy-plot already collapse multiple
        # rows per core -- so checking "per-core" must no longer silently
        # drop a given interval.
        checklist = {"counters": {"enabled": True, "groups": ["topdown"],
                                   "per_core": True, "interval_secs": "1"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertIn("--per-core", flags)
        self.assertIn("--interval", flags)
        self.assertIn("1", flags)

    def test_counters_per_core_freq_requires_per_core(self):
        # --per-core-freq is fatal in wspy.c without --per-core -- checking
        # the freq box alone (per-core left unchecked) must not emit the
        # flag at all, matching "what runs matches what the preview shows"
        # rather than producing a run that fatals.
        checklist = {"counters": {"enabled": True, "groups": ["topdown"], "per_core_freq": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertNotIn("--per-core-freq", passes[0]["flags"])

    def test_counters_per_core_freq_with_per_core_adds_flag(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown"],
                                   "per_core": True, "per_core_freq": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertIn("--per-core", flags)
        self.assertIn("--per-core-freq", flags)

    def test_counters_per_core_forces_plain_flags_over_passes_bin_packing(self):
        # --passes fatals against --per-core (wspy.c), so checking
        # "per-core" must bypass the --passes bin-packing branch even with
        # 2+ groups and no interval given.
        checklist = {"counters": {"enabled": True, "groups": ["topdown", "cache2"],
                                   "per_core": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertIn("--per-core", flags)
        self.assertFalse(any(f.startswith("--passes=") for f in flags))

    def test_system_power_checkbox_adds_power_flag(self):
        checklist = {"system": {"enabled": True, "power": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--power", passes[0]["flags"])
        self.assertEqual(passes[0]["category"], "system-metrics")

    def test_power_unchecked_produces_no_power_flag_anywhere(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown"]},
                     "system": {"enabled": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 2)
        for p in passes:
            self.assertNotIn("--power", p["flags"])

    def test_gpu_nvidia_checkbox_adds_gpu_nvidia_flag(self):
        checklist = {"gpu": {"enabled": True, "nvidia": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--gpu-nvidia", passes[0]["flags"])

    def test_gpu_nvidia_combines_with_amd_backends(self):
        checklist = {"gpu": {"enabled": True, "busy": True, "nvidia": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--gpu-busy", passes[0]["flags"])
        self.assertIn("--gpu-nvidia", passes[0]["flags"])

    def test_gpu_enabled_with_no_backends_produces_no_pass(self):
        checklist = {"gpu": {"enabled": True}}
        self.assertEqual(joblib.build_configuration_passes("/tmp/rundir", checklist), [])


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

    def test_affinity_appends_flag(self):
        p = {"name": "custom", "csv": False, "flags": ["--software"]}
        argv, _, _ = joblib.build_pass_argv(
            "/usr/bin/wspy", "/tmp/rundir", p, manifest_on=False, run_index_path=None,
            affinity="domain=1")
        self.assertIn("--affinity", argv)
        self.assertEqual(argv[argv.index("--affinity") + 1], "domain=1")

    def test_affinity_all_omits_flag(self):
        p = {"name": "custom", "csv": False, "flags": ["--software"]}
        argv, _, _ = joblib.build_pass_argv(
            "/usr/bin/wspy", "/tmp/rundir", p, manifest_on=False, run_index_path=None,
            affinity="all")
        self.assertNotIn("--affinity", argv)


class AffinitySpecTest(unittest.TestCase):
    def test_valid_specs(self):
        for spec in ("all", "nosmt", "thread=0", "thread=23", "domain=1", "coretype=0",
                     "coretype=1", "cpuset=0", "cpuset=0,2-3", "cpuset=0-3,12-15"):
            self.assertTrue(joblib.valid_affinity_spec(spec), spec)

    def test_invalid_specs(self):
        for spec in ("", None, "bogus", "thread=", "thread=-1", "domain=abc",
                     "coretype=", "coretype=-1", "cpuset="):
            self.assertFalse(joblib.valid_affinity_spec(spec), spec)

    def test_build_wspy_run_argv_includes_affinity(self):
        argv = joblib.build_wspy_run_argv("/usr/bin/wspy-run", "/usr/bin/wspy", "/tmp/out",
                                           "manual", "sleep", "run1", "quick", ["sleep", "1"],
                                           affinity="nosmt")
        self.assertIn("--affinity", argv)
        self.assertEqual(argv[argv.index("--affinity") + 1], "nosmt")

    def test_build_wspy_run_argv_omits_default_all(self):
        argv = joblib.build_wspy_run_argv("/usr/bin/wspy-run", "/usr/bin/wspy", "/tmp/out",
                                           "manual", "sleep", "run1", "quick", ["sleep", "1"],
                                           affinity="all")
        self.assertNotIn("--affinity", argv)

    def test_build_job_round_trips_affinity(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick",
                                affinity="domain=0")
        self.assertEqual(joblib.validate_job(job), [])
        self.assertEqual(job["affinity"], "domain=0")

    def test_validate_job_rejects_bad_affinity(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        job["affinity"] = "bogus"
        self.assertTrue(joblib.validate_job(job))


class ResolveColumnGroupTest(unittest.TestCase):
    def test_counters_column(self):
        self.assertEqual(joblib.resolve_column_group("retire"), "topdown")

    def test_system_column(self):
        self.assertEqual(joblib.resolve_column_group("load"), "system")

    def test_system_network_column_prefix(self):
        self.assertEqual(joblib.resolve_column_group("net eth0"), "system")

    def test_system_disk_column_prefix(self):
        self.assertEqual(joblib.resolve_column_group("disk nvme0n1 read"), "system")
        self.assertEqual(joblib.resolve_column_group("disk nvme0n1 write"), "system")
        self.assertEqual(joblib.resolve_column_group("disk nvme0n1 time"), "system")

    def test_system_memory_pressure_columns(self):
        for col in ("mem_free_mb", "mem_cached_mb", "mem_dirty_mb", "mem_writeback_mb",
                    "swap_free_mb", "committed_as_mb"):
            self.assertEqual(joblib.resolve_column_group(col), "system")

    def test_power_columns(self):
        self.assertEqual(joblib.resolve_column_group("pkg_joules"), "power")
        self.assertEqual(joblib.resolve_column_group("pkg_watts"), "power")

    def test_unrecognized_column(self):
        self.assertIsNone(joblib.resolve_column_group("not_a_real_column"))


class AutofitChecklistForCustomPlotsTest(unittest.TestCase):
    def test_power_column_autofits_into_counters_by_default(self):
        # No system column/section in play, so power folds into
        # "Performance counters" by default (auto-enabling it).
        checklist, notes = joblib.autofit_checklist_for_custom_plots(
            {}, [{"name": "power-plot", "columns": ["pkg_watts"]}])
        self.assertTrue(checklist["counters"]["enabled"])
        self.assertTrue(checklist["counters"]["power"])
        self.assertEqual(checklist["counters"]["interval_secs"], "1")
        self.assertTrue(checklist["counters"]["csv"])
        self.assertNotIn("system", checklist)
        self.assertTrue(any("power" in n.lower() for n in notes))

    def test_power_column_folds_into_system_when_system_also_requested(self):
        checklist, _ = joblib.autofit_checklist_for_custom_plots(
            {}, [{"name": "combo-plot", "columns": ["load", "pkg_watts"]}])
        self.assertTrue(checklist["system"]["enabled"])
        self.assertTrue(checklist["system"]["power"])
        self.assertNotIn("counters", checklist)

    def test_power_column_folds_into_already_enabled_system(self):
        checklist, _ = joblib.autofit_checklist_for_custom_plots(
            {"system": {"enabled": True}},
            [{"name": "power-plot", "columns": ["pkg_watts"]}])
        self.assertTrue(checklist["system"]["power"])
        self.assertNotIn("counters", checklist)

    def test_already_enabled_counters_power_is_not_reported_as_a_change(self):
        checklist, notes = joblib.autofit_checklist_for_custom_plots(
            {"counters": {"enabled": True, "power": True, "interval_secs": "5"}},
            [{"name": "power-plot", "columns": ["pkg_watts"]}])
        self.assertEqual(checklist["counters"]["interval_secs"], "5")  # not overwritten
        self.assertFalse(any("power" in n.lower() for n in notes))


class ChecklistFromProvenanceTest(unittest.TestCase):
    """INVESTIGATION.md's "What shipped in 4.1" ("Browse-reports"): the read
    side of structured configuration provenance -- turning a run's
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

    def test_counters_power_round_trips_through_build_configuration_passes(self):
        checklist = {"counters": {"enabled": True, "groups": ["topdown"], "power": True,
                                   "interval_secs": "1", "csv": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        p = passes[0]
        self.assertEqual(p["category"], "performance-counters")
        restored = joblib.checklist_section_from_options("counters", p["options"])
        self.assertIs(restored["power"], True)
        self.assertEqual(restored["interval_secs"], "1")
        self.assertIs(restored["csv"], True)
        self.assertIs(restored["enabled"], True)

    def test_system_power_round_trips_through_build_configuration_passes(self):
        checklist = {"system": {"enabled": True, "power": True, "csv": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        p = passes[0]
        self.assertEqual(p["category"], "system-metrics")
        restored = joblib.checklist_section_from_options("system", p["options"])
        self.assertIs(restored["power"], True)
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


class ParseRunKeyTest(unittest.TestCase):
    def test_valid_key(self):
        self.assertEqual(joblib.parse_run_key("suite/bench/run1"), ("suite", "bench", "run1"))

    def test_wrong_segment_count(self):
        self.assertIsNone(joblib.parse_run_key("suite/bench"))
        self.assertIsNone(joblib.parse_run_key("suite/bench/run1/extra"))

    def test_rejects_dotdot_segment(self):
        self.assertIsNone(joblib.parse_run_key("suite/../run1"))

    def test_rejects_invalid_characters(self):
        self.assertIsNone(joblib.parse_run_key("suite/bench/run one"))


class BuildProctreeJsonDiffArgvTest(unittest.TestCase):
    def test_json_argv(self):
        self.assertEqual(
            joblib.build_proctree_json_argv("./proctree", "/r/process.tree.txt"),
            ["./proctree", "--json", "/r/process.tree.txt"])

    def test_diff_argv(self):
        self.assertEqual(
            joblib.build_proctree_diff_argv("./proctree", "/tmp/a.json", "/tmp/b.json"),
            ["./proctree", "--diff", "--json", "/tmp/a.json", "/tmp/b.json"])


# Phoronix runtime-estimation logic (moved here from server.py's "Estimated
# runtime display" Check button -- INVESTIGATION.md's 4.2 "Size wspy-run's
# --tree pass timeout" item -- so scripts/estimate_tree_timeout.py could
# reuse it). Pure-logic pieces only, matching this file's own stated scope;
# estimate_phoronix_workload_seconds()'s subprocess-spawning half is tested
# separately below via a fake phoronix_bin script, mirroring
# tests/wspy_queue_smoke.sh's own fake-binary convention.

class ParsePhoronixTestNamesTest(unittest.TestCase):
    def test_batch_run_single_test(self):
        self.assertEqual(
            joblib.parse_phoronix_test_names("phoronix-test-suite batch-run coremark"),
            ["coremark"])

    def test_run_multiple_tests(self):
        self.assertEqual(
            joblib.parse_phoronix_test_names("phoronix-test-suite run coremark blender"),
            ["coremark", "blender"])

    def test_ignores_flags(self):
        self.assertEqual(
            joblib.parse_phoronix_test_names("phoronix-test-suite benchmark --no-log coremark"),
            ["coremark"])

    def test_non_phoronix_command_returns_empty(self):
        self.assertEqual(joblib.parse_phoronix_test_names("sleep 10"), [])

    def test_unrecognized_subcommand_returns_empty(self):
        self.assertEqual(joblib.parse_phoronix_test_names("phoronix-test-suite info coremark"), [])

    def test_too_few_tokens_returns_empty(self):
        self.assertEqual(joblib.parse_phoronix_test_names("phoronix-test-suite batch-run"), [])

    def test_unbalanced_quotes_returns_empty_not_raises(self):
        self.assertEqual(joblib.parse_phoronix_test_names('phoronix-test-suite run "unterminated'), [])

    def test_full_path_binary_recognized(self):
        self.assertEqual(
            joblib.parse_phoronix_test_names("/usr/bin/phoronix-test-suite run coremark"),
            ["coremark"])


class ResolvePhoronixSubsetNameTest(unittest.TestCase):
    def test_strips_subset_suffix(self):
        self.assertEqual(joblib.resolve_phoronix_subset_name("dirt-rally2-subset"), ("dirt-rally2", True))

    def test_leaves_real_test_name_unchanged(self):
        self.assertEqual(joblib.resolve_phoronix_subset_name("coremark"), ("coremark", False))

    def test_bare_suffix_not_stripped(self):
        # "-subset" alone (nothing before it) isn't a real test name either
        # way, so there's nothing meaningful to strip.
        self.assertEqual(joblib.resolve_phoronix_subset_name("-subset"), ("-subset", False))


class ParseDurationSecondsTest(unittest.TestCase):
    def test_seconds_only(self):
        self.assertEqual(joblib._parse_duration_seconds("132 Seconds"), 132.0)

    def test_minutes_and_seconds(self):
        self.assertEqual(joblib._parse_duration_seconds("2 Minutes, 12 Seconds"), 132.0)

    def test_hours_minutes_seconds(self):
        self.assertEqual(joblib._parse_duration_seconds("1 Hour, 3 Minutes, 5 Seconds"), 3785.0)

    def test_none_for_empty_string(self):
        self.assertIsNone(joblib._parse_duration_seconds(""))

    def test_none_for_none(self):
        self.assertIsNone(joblib._parse_duration_seconds(None))

    def test_none_for_unmatched_text(self):
        self.assertIsNone(joblib._parse_duration_seconds("not a duration at all"))


class ParsePhoronixInfoFieldsTest(unittest.TestCase):
    def test_parses_label_value_lines(self):
        output = "Test Installed: Yes\nTimes Run: 3\nEstimated Run-Time: 132 Seconds\n"
        fields = joblib.parse_phoronix_info_fields(output)
        self.assertEqual(fields["Test Installed"], "Yes")
        self.assertEqual(fields["Times Run"], "3")
        self.assertEqual(fields["Estimated Run-Time"], "132 Seconds")

    def test_strips_ansi_codes(self):
        output = "\x1b[1mTest Installed:\x1b[0m Yes\n"
        self.assertEqual(joblib.parse_phoronix_info_fields(output)["Test Installed"], "Yes")

    def test_ignores_non_matching_lines(self):
        output = "Test Installed: Yes\nsome free-form prose with no colon-value shape here\n"
        fields = joblib.parse_phoronix_info_fields(output)
        self.assertEqual(len(fields), 1)

    def test_empty_output_yields_empty_dict(self):
        self.assertEqual(joblib.parse_phoronix_info_fields(""), {})
        self.assertEqual(joblib.parse_phoronix_info_fields(None), {})


class EstimatePhoronixRuntimeTest(unittest.TestCase):
    def test_measured_average_when_already_run(self):
        fields = {"Test Installed": "Yes", "Times Run": "5", "Average Run-Time": "100 Seconds"}
        result = joblib.estimate_phoronix_runtime(fields)
        self.assertEqual(result["source"], "measured")
        self.assertEqual(result["seconds"], 100.0)

    def test_falls_back_to_latest_run_time_if_no_average(self):
        fields = {"Test Installed": "Yes", "Times Run": "1", "Latest Run-Time": "50 Seconds"}
        result = joblib.estimate_phoronix_runtime(fields)
        self.assertEqual(result["source"], "measured")
        self.assertEqual(result["seconds"], 50.0)

    def test_installed_but_never_run_uses_generic_estimate(self):
        fields = {"Test Installed": "Yes", "Times Run": "0", "Estimated Run-Time": "200 Seconds"}
        result = joblib.estimate_phoronix_runtime(fields)
        self.assertEqual(result["source"], "installed-not-run")
        self.assertEqual(result["seconds"], 200.0)

    def test_not_installed_uses_generic_estimate(self):
        fields = {"Test Installed": "No", "Estimated Run-Time": "300 Seconds"}
        result = joblib.estimate_phoronix_runtime(fields)
        self.assertEqual(result["source"], "not-installed")
        self.assertEqual(result["seconds"], 300.0)


class EstimatePhoronixWorkloadSecondsTest(unittest.TestCase):
    """Exercises the subprocess-spawning orchestration loop against a fake
    `phoronix_bin` shell script (mirroring tests/wspy_queue_smoke.sh's own
    fake-binary convention) rather than a real phoronix-test-suite
    install."""

    def _make_fake_phoronix(self, tmpdir, responses):
        """responses: {test_name: "Test Installed: Yes\\n..." (or None to
        simulate a nonzero-exit "no such test")}. The fake script just
        looks up argv[2] (the test name after "info") in a case statement."""
        path = os.path.join(tmpdir, "fake-phoronix-test-suite")
        lines = ["#!/bin/sh", 'if [ "$1" != "info" ]; then exit 1; fi', 'case "$2" in']
        for name, output in responses.items():
            if output is None:
                lines.append(f'  {name}) exit 1 ;;')
            else:
                escaped = output.replace("'", "'\\''")
                lines.append(f"  {name}) printf '%s' '{escaped}'; exit 0 ;;")
        lines.append('  *) exit 1 ;;')
        lines.append('esac')
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        os.chmod(path, 0o755)
        return path

    def test_single_test_estimate(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = self._make_fake_phoronix(tmpdir, {
                "coremark": "Test Installed: No\nEstimated Run-Time: 100 Seconds\n",
            })
            result = joblib.estimate_phoronix_workload_seconds(
                "phoronix-test-suite run coremark", phoronix_bin=fake_bin)
            self.assertEqual(result["total_seconds"], 100.0)
            self.assertFalse(result["truncated"])
            self.assertEqual(len(result["tests"]), 1)

    def test_sums_across_multiple_tests(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = self._make_fake_phoronix(tmpdir, {
                "coremark": "Test Installed: No\nEstimated Run-Time: 100 Seconds\n",
                "blender": "Test Installed: No\nEstimated Run-Time: 200 Seconds\n",
            })
            result = joblib.estimate_phoronix_workload_seconds(
                "phoronix-test-suite batch-run coremark blender", phoronix_bin=fake_bin)
            self.assertEqual(result["total_seconds"], 300.0)

    def test_partial_failure_makes_total_none(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = self._make_fake_phoronix(tmpdir, {
                "coremark": "Test Installed: No\nEstimated Run-Time: 100 Seconds\n",
                "no-such-test": None,
            })
            result = joblib.estimate_phoronix_workload_seconds(
                "phoronix-test-suite batch-run coremark no-such-test", phoronix_bin=fake_bin)
            self.assertIsNone(result["total_seconds"])
            self.assertEqual(len(result["tests"]), 2)
            self.assertIn("error", result["tests"][1])

    def test_non_phoronix_workload_returns_empty(self):
        result = joblib.estimate_phoronix_workload_seconds("sleep 10")
        self.assertEqual(result, {"tests": [], "total_seconds": None, "truncated": False})

    def test_truncates_beyond_max_tests(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            fake_bin = self._make_fake_phoronix(tmpdir, {
                f"test{i}": f"Test Installed: No\nEstimated Run-Time: {i} Seconds\n" for i in range(1, 8)
            })
            workload = "phoronix-test-suite batch-run " + " ".join(f"test{i}" for i in range(1, 8))
            result = joblib.estimate_phoronix_workload_seconds(workload, phoronix_bin=fake_bin, max_tests=5)
            self.assertEqual(len(result["tests"]), 5)
            self.assertTrue(result["truncated"])


class ParseOpenbenchmarkingIdTest(unittest.TestCase):
    def test_extracts_id_from_result_url(self):
        self.assertEqual(
            joblib.parse_openbenchmarking_id("https://openbenchmarking.org/result/2607160-PTS-7700X3D886"),
            "2607160-PTS-7700X3D886")

    def test_extracts_id_from_url_with_query_string(self):
        self.assertEqual(
            joblib.parse_openbenchmarking_id(
                "https://openbenchmarking.org/result/2607160-PTS-7700X3D886?export=xml-suite"),
            "2607160-PTS-7700X3D886")

    def test_bare_id_passes_through(self):
        self.assertEqual(joblib.parse_openbenchmarking_id("2607160-PTS-7700X3D886"), "2607160-PTS-7700X3D886")

    def test_strips_query_from_bare_ref(self):
        self.assertEqual(joblib.parse_openbenchmarking_id("2607160-PTS-7700X3D886&export=xml-suite"),
                          "2607160-PTS-7700X3D886")


class ParsePhoronixXmlTestPointsTest(unittest.TestCase):
    def test_suite_definition_shape(self):
        xml = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <SuiteInformation><Title>t</Title></SuiteInformation>
  <Execute><Test>pts/compress-7zip</Test></Execute>
  <Execute><Test>pts/blender-1.2.1</Test><Arguments>quad-mesh</Arguments></Execute>
</PhoronixTestSuite>"""
        points = joblib.parse_phoronix_xml_test_points(xml)
        self.assertEqual(points, [
            {"test_id": "pts/compress-7zip", "arguments": "", "description": ""},
            {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh", "description": ""},
        ])

    def test_result_composite_shape(self):
        # Trimmed real shape: a <Result> block's own <Identifier>/<Arguments>
        # are direct children; the per-hardware <Data><Entry><Identifier>
        # nested two levels deeper must NOT be picked up as a test id.
        xml = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <Result>
    <Identifier>system/selenium-1.0.47</Identifier>
    <Arguments>pspdfkit Firefox</Arguments>
    <Data><Entry><Identifier>Ryzen 7 7700X</Identifier><Value>1.0</Value></Entry></Data>
  </Result>
  <Result>
    <Identifier>pts/coremark-1.0.1</Identifier>
    <Arguments></Arguments>
    <Data><Entry><Identifier>Ryzen 7 7700X</Identifier><Value>2.0</Value></Entry></Data>
  </Result>
</PhoronixTestSuite>"""
        points = joblib.parse_phoronix_xml_test_points(xml)
        self.assertEqual(points, [
            {"test_id": "system/selenium-1.0.47", "arguments": "pspdfkit Firefox", "description": ""},
            {"test_id": "pts/coremark-1.0.1", "arguments": "", "description": ""},
        ])

    def test_captures_description_alongside_arguments(self):
        # Regression: a real PTS install silently batch-runs *every* option
        # in a test's menu instead of just the pinned one when a custom
        # suite's <Execute> has <Arguments> but no <Description> (confirmed
        # live 2026-07-23 -- see materialize_phoronix_test_point()'s own
        # comment). The real composite XML this is trimmed from always
        # pairs the two; parsing must not drop the Description half.
        xml = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <Result>
    <Identifier>pts/build-linux-kernel-1.17.1</Identifier>
    <Arguments>defconfig</Arguments>
    <Description>Build: defconfig</Description>
    <Data><Entry><Identifier>Ryzen 7 7700X</Identifier><Value>119.058</Value></Entry></Data>
  </Result>
</PhoronixTestSuite>"""
        points = joblib.parse_phoronix_xml_test_points(xml)
        self.assertEqual(points, [
            {"test_id": "pts/build-linux-kernel-1.17.1", "arguments": "defconfig",
             "description": "Build: defconfig"},
        ])

    def test_dedupes_identical_pairs_preserving_order(self):
        xml = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <Execute><Test>pts/x-1.0</Test><Arguments>a</Arguments></Execute>
  <Execute><Test>pts/x-1.0</Test><Arguments>a</Arguments></Execute>
  <Execute><Test>pts/x-1.0</Test><Arguments>b</Arguments></Execute>
</PhoronixTestSuite>"""
        points = joblib.parse_phoronix_xml_test_points(xml)
        self.assertEqual(len(points), 2)
        self.assertEqual(points[0]["arguments"], "a")
        self.assertEqual(points[1]["arguments"], "b")

    def test_no_test_points_yields_empty_list(self):
        xml = b'<?xml version="1.0"?><PhoronixTestSuite><SuiteInformation/></PhoronixTestSuite>'
        self.assertEqual(joblib.parse_phoronix_xml_test_points(xml), [])

    def test_unparseable_xml_raises_parse_error(self):
        import xml.etree.ElementTree as ET
        with self.assertRaises(ET.ParseError):
            joblib.parse_phoronix_xml_test_points(b"not xml at all")


class PhoronixBareTestNameTest(unittest.TestCase):
    def test_strips_prefix_and_version(self):
        self.assertEqual(joblib.phoronix_bare_test_name("pts/blender-1.2.1"), "blender")

    def test_strips_system_prefix(self):
        self.assertEqual(joblib.phoronix_bare_test_name("system/selenium-1.0.47"), "selenium")

    def test_no_prefix_still_strips_version(self):
        self.assertEqual(joblib.phoronix_bare_test_name("coremark-1.0.1"), "coremark")

    def test_name_with_no_version_suffix_unchanged(self):
        self.assertEqual(joblib.phoronix_bare_test_name("pts/build-linux-kernel"), "build-linux-kernel")

    def test_trailing_non_version_dash_segment_kept(self):
        # "-x264" doesn't look like a version (has letters), so nothing strips.
        self.assertEqual(joblib.phoronix_bare_test_name("pts/compress-x264"), "compress-x264")


class PhoronixPinnedVersionTest(unittest.TestCase):
    def test_extracts_version_suffix(self):
        self.assertEqual(joblib.phoronix_pinned_version("pts/build-linux-kernel-1.17.1"), "1.17.1")

    def test_no_version_suffix_returns_none(self):
        self.assertIsNone(joblib.phoronix_pinned_version("pts/build-linux-kernel"))

    def test_letters_suffix_not_treated_as_version(self):
        self.assertIsNone(joblib.phoronix_pinned_version("pts/compress-x264"))


class SlugifyPhoronixArgumentsTest(unittest.TestCase):
    def test_empty_becomes_default(self):
        self.assertEqual(joblib.slugify_phoronix_arguments(""), "default")
        self.assertEqual(joblib.slugify_phoronix_arguments(None), "default")

    def test_lowercases_and_collapses_punctuation(self):
        self.assertEqual(joblib.slugify_phoronix_arguments("pspdfkit Firefox"), "pspdfkit-firefox")

    def test_collapses_non_alnum_runs(self):
        self.assertEqual(joblib.slugify_phoronix_arguments("-m ./data/x.mesh -p 14"), "m-data-x-mesh-p-14")

    def test_truncates_long_arguments(self):
        slug = joblib.slugify_phoronix_arguments("a" * 200)
        self.assertLessEqual(len(slug), 60)

    def test_long_arguments_differing_only_near_the_end_do_not_collide(self):
        # Regression: real OpenVINO test points share an ~83-char common
        # "-m models/intel/<model>/..." prefix and differ only in the
        # trailing "-hint throughput" vs "-hint latency" -- a bare 60-char
        # truncation collapsed both to the same slug (confirmed live
        # 2026-07-23), silently dropping half of every OpenVINO test point.
        prefix = "-m models/intel/face-detection-0206/FP16-INT8/face-detection-0206.xml -d CPU -hint "
        throughput = joblib.slugify_phoronix_arguments(prefix + "throughput")
        latency = joblib.slugify_phoronix_arguments(prefix + "latency")
        self.assertNotEqual(throughput, latency)
        self.assertLessEqual(len(throughput), 60)
        self.assertLessEqual(len(latency), 60)

    def test_hashed_slug_is_deterministic(self):
        text = "a" * 200
        self.assertEqual(joblib.slugify_phoronix_arguments(text), joblib.slugify_phoronix_arguments(text))


class MaterializePhoronixTestPointTest(unittest.TestCase):
    def test_creates_suite_definition_and_source_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh"}
            result = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            self.assertEqual(result["status"], "created")
            self.assertEqual(result["identity"], "blender-quad-mesh")
            suite_path = os.path.join(tmpdir, "blender", "quad-mesh", "suite-definition.xml")
            self.assertTrue(os.path.isfile(suite_path))
            source_path = os.path.join(tmpdir, "blender", "quad-mesh", "source.json")
            with open(source_path) as f:
                source = json.load(f)
            self.assertEqual(source["test_id"], "pts/blender-1.2.1")
            self.assertEqual(source["arguments"], "quad-mesh")

            import xml.etree.ElementTree as ET
            root = ET.parse(suite_path).getroot()
            execute = root.find("Execute")
            self.assertEqual(execute.find("Test").text, "pts/blender-1.2.1")
            self.assertEqual(execute.find("Arguments").text, "quad-mesh")
            # Falls back to the arguments string itself since this point
            # carried no captured description -- see the "no options passed
            # but expecting them" regression test below for why this
            # element must never be empty/missing when Arguments is set.
            self.assertEqual(execute.find("Description").text, "quad-mesh")

    def test_no_arguments_omits_arguments_and_description_elements(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/compress-7zip", "arguments": ""}
            joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(tmpdir, "compress-7zip", "default", "suite-definition.xml")).getroot()
            self.assertIsNone(root.find("Execute/Arguments"))
            self.assertIsNone(root.find("Execute/Description"))

    def test_real_description_is_preferred_over_arguments_fallback(self):
        # Regression: a real PTS install (pts_test_suite.php's suite
        # parser, confirmed live 2026-07-23) silently batch-runs *every*
        # option in a test's menu -- ignoring <Arguments> altogether --
        # whenever a test has configurable options and its <Execute> block
        # has no non-empty <Description>. A build-linux-kernel test point
        # pinned to "defconfig" ran both "defconfig" and "allmodconfig"
        # until this element was added. materialize_phoronix_test_point()
        # must carry the real captured description through when present,
        # not just synthesize one from arguments.
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/build-linux-kernel-1.17.1", "arguments": "defconfig",
                     "description": "Build: defconfig"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            self.assertEqual(root.find("Execute/Description").text, "Build: defconfig")
            with open(os.path.join(info["dir"], "source.json")) as f:
                source = json.load(f)
            self.assertEqual(source["description"], "Build: defconfig")

    def test_reruns_report_exists_without_overwriting(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh"}
            first = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            suite_path = os.path.join(first["dir"], "suite-definition.xml")
            with open(suite_path, "rb") as f:
                original_bytes = f.read()
            second = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/different-src.xml")
            self.assertEqual(second["status"], "exists")
            with open(suite_path, "rb") as f:
                self.assertEqual(f.read(), original_bytes)


class WritePhoronixTestReadmeTest(unittest.TestCase):
    def test_creates_readme_with_description_and_details(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fields = {"Description": "This is a test of 7-Zip compression.",
                      "Test Type": "Processor", "License Type": "Free",
                      "Project Web-Site": "https://www.7-zip.org/"}
            result = joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                                         fields, "/tmp/src.xml")
            self.assertEqual(result["status"], "created")
            path = os.path.join(tmpdir, "compress-7zip", "README.md")
            self.assertEqual(result["path"], path)
            with open(path) as f:
                text = f.read()
            self.assertIn("This is a test of 7-Zip compression.", text)
            self.assertIn("**Test Type:** Processor", text)
            self.assertIn("**License Type:** Free", text)
            self.assertIn("pts/compress-7zip", text)

    def test_reruns_report_exists_without_overwriting(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fields = {"Description": "Original description."}
            first = joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                                        fields, "/tmp/src.xml")
            with open(first["path"]) as f:
                original_text = f.read()
            second = joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                                         {"Description": "Different description."},
                                                         "/tmp/other.xml")
            self.assertEqual(second["status"], "exists")
            with open(first["path"]) as f:
                self.assertEqual(f.read(), original_text)

    def test_skipped_when_fields_unavailable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            result = joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                                         None, "/tmp/src.xml")
            self.assertEqual(result["status"], "skipped")
            self.assertFalse(os.path.isfile(result["path"]))


class ImportPhoronixTestPointsTest(unittest.TestCase):
    """Exercises the top-level orchestration with a fake wspy-ledger shell
    script (mirroring EstimatePhoronixWorkloadSecondsTest's fake-phoronix-
    binary convention above) rather than the real C binary."""

    SUITE_XML = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <Execute><Test>pts/compress-7zip</Test></Execute>
  <Execute><Test>pts/compress-gzip</Test></Execute>
</PhoronixTestSuite>"""

    def _make_fake_ledger(self, tmpdir):
        path = os.path.join(tmpdir, "fake-wspy-ledger")
        with open(path, "w") as f:
            f.write("#!/bin/sh\necho \"fake-ledger: $@\"\nexit 0\n")
        os.chmod(path, 0o755)
        return path

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml",
                check_installed=False, dry_run=True)
            self.assertIsNone(result["error"])
            self.assertEqual(len(result["points"]), 2)
            self.assertTrue(all(p["status"] == "would-create" for p in result["points"]))
            self.assertFalse(os.path.isdir(dest))

    def test_real_run_materializes_and_calls_ledger(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            fake_ledger = self._make_fake_ledger(tmpdir)
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml",
                ledger_bin=fake_ledger, check_installed=False, dry_run=False)
            self.assertIsNone(result["error"])
            self.assertEqual(len(result["points"]), 2)
            for p in result["points"]:
                self.assertEqual(p["status"], "created")
                self.assertIsNotNone(p["ledger"])
                self.assertEqual(p["ledger"]["exit_code"], 0)
            self.assertTrue(os.path.isfile(os.path.join(dest, "compress-7zip", "default",
                                                          "suite-definition.xml")))

    def test_no_ledger_skips_ledger_call(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml",
                check_installed=False, dry_run=False, add_to_ledger=False)
            self.assertTrue(all(p["ledger"] is None for p in result["points"]))

    def test_empty_xml_reports_error(self):
        xml = b'<?xml version="1.0"?><PhoronixTestSuite><SuiteInformation/></PhoronixTestSuite>'
        with tempfile.TemporaryDirectory() as tmpdir:
            result = joblib.import_phoronix_test_points(xml, os.path.join(tmpdir, "dest"), "file", "/tmp/src.xml")
            self.assertIsNotNone(result["error"])
            self.assertEqual(result["points"], [])

    def test_installed_flag_persists_to_source_json_and_inventory(self):
        # Regression: materialize_phoronix_test_point() used to compute
        # "installed" for its own return value but never write it to
        # source.json, so list_materialized_phoronix_test_points() could
        # only ever report "?" for every already-materialized point.
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/compress-7zip", "arguments": ""}
            joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml", installed=True)
            with open(os.path.join(dest, "compress-7zip", "default", "source.json")) as f:
                source = json.load(f)
            self.assertIs(source["installed"], True)
            points = joblib.list_materialized_phoronix_test_points(dest)
            self.assertEqual(len(points), 1)
            self.assertIs(points[0]["installed"], True)

    def test_no_check_installed_skips_readmes(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml", check_installed=False, dry_run=False)
            self.assertEqual(len(result["readmes"]), 2)
            self.assertTrue(all(r["status"] == "skipped" for r in result["readmes"]))
            self.assertFalse(os.path.isfile(os.path.join(dest, "compress-7zip", "README.md")))

    def test_dry_run_reports_readme_skipped_without_check_installed(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml", check_installed=False, dry_run=True)
            self.assertEqual(len(result["readmes"]), 2)
            self.assertTrue(all(r["status"] == "skipped" for r in result["readmes"]))
            self.assertFalse(os.path.isdir(dest))

    def test_dry_run_reports_would_create_readme(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            fake_bin = self._make_fake_phoronix_info(tmpdir, "A compression benchmark.")
            result = joblib.import_phoronix_test_points(
                self.SUITE_XML, dest, "file", "/tmp/src.xml", phoronix_bin=fake_bin,
                check_installed=True, dry_run=True)
            self.assertEqual(len(result["readmes"]), 2)
            self.assertTrue(all(r["status"] == "would-create" for r in result["readmes"]))
            self.assertFalse(os.path.isdir(dest))

    def _make_fake_phoronix_info(self, tmpdir, description):
        path = os.path.join(tmpdir, "fake-phoronix-test-suite")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n"
                    'if [ "$1" != "info" ]; then exit 1; fi\n'
                    f'printf "Test Installed: No\\nDescription: {description}\\n"\n'
                    "exit 0\n")
        os.chmod(path, 0o755)
        return path

    def test_writes_one_readme_per_bare_test_name(self):
        xml = b"""<?xml version="1.0"?>
<PhoronixTestSuite>
  <Execute><Test>pts/blender-1.2.1</Test><Arguments>quad-mesh</Arguments></Execute>
  <Execute><Test>pts/blender-1.2.1</Test><Arguments>bmw27</Arguments></Execute>
</PhoronixTestSuite>"""
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            fake_bin = self._make_fake_phoronix_info(tmpdir, "Blender rendering benchmark.")
            result = joblib.import_phoronix_test_points(
                xml, dest, "file", "/tmp/src.xml", phoronix_bin=fake_bin,
                check_installed=True, add_to_ledger=False)
            self.assertEqual(len(result["readmes"]), 1)
            self.assertEqual(result["readmes"][0]["status"], "created")
            readme_path = os.path.join(dest, "blender", "README.md")
            self.assertEqual(result["readmes"][0]["path"], readme_path)
            with open(readme_path) as f:
                self.assertIn("Blender rendering benchmark.", f.read())


class ListMaterializedPhoronixTestPointsTest(unittest.TestCase):
    def test_lists_materialized_points_with_runs(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh"}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")
            rundir = os.path.join(tmpdir, "runs", "phoronix", info["identity"], "run1")
            os.makedirs(rundir)
            joblib.link_phoronix_test_point_run(info["dir"], "run1", rundir)

            points = joblib.list_materialized_phoronix_test_points(dest)
            self.assertEqual(len(points), 1)
            self.assertEqual(points[0]["identity"], info["identity"])
            self.assertEqual(points[0]["runs"], [
                {"run_id": "run1", "suite": "phoronix", "benchmark": info["identity"]},
            ])

    def test_empty_dest_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(joblib.list_materialized_phoronix_test_points(os.path.join(tmpdir, "nope")), [])

    def test_dir_without_source_json_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point_dir = os.path.join(dest, "sometest", "default")
            os.makedirs(point_dir)
            with open(os.path.join(point_dir, "suite-definition.xml"), "w") as f:
                f.write("<PhoronixTestSuite/>")
            self.assertEqual(joblib.list_materialized_phoronix_test_points(dest), [])

    def test_dangling_run_symlink_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/blender-1.2.1", "arguments": ""}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")
            os.makedirs(os.path.join(info["dir"], "runs"))
            os.symlink(os.path.join(tmpdir, "does-not-exist"),
                       os.path.join(info["dir"], "runs", "run1"))
            points = joblib.list_materialized_phoronix_test_points(dest)
            self.assertEqual(points[0]["runs"], [])


class ReadPhoronixTestDescriptionTest(unittest.TestCase):
    def test_reads_description_from_readme(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                               {"Description": "A compression benchmark."}, "/tmp/src.xml")
            self.assertEqual(joblib.read_phoronix_test_description(tmpdir, "compress-7zip"),
                              "A compression benchmark.")

    def test_none_when_no_readme(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertIsNone(joblib.read_phoronix_test_description(tmpdir, "no-such-test"))

    def test_none_when_readme_has_no_description(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            joblib.write_phoronix_test_readme("compress-7zip", tmpdir, "pts/compress-7zip",
                                               {"Test Type": "Processor"}, "/tmp/src.xml")
            self.assertIsNone(joblib.read_phoronix_test_description(tmpdir, "compress-7zip"))


class GroupMaterializedPhoronixPointsByTestTest(unittest.TestCase):
    def test_groups_and_sorts_by_bare_name(self):
        points = [
            {"bare_name": "blender", "options_slug": "bmw27", "installed": True},
            {"bare_name": "compress-7zip", "options_slug": "default", "installed": False},
            {"bare_name": "blender", "options_slug": "quad-mesh", "installed": None},
        ]
        groups = joblib.group_materialized_phoronix_points_by_test(points)
        self.assertEqual([g["bare_name"] for g in groups], ["blender", "compress-7zip"])
        blender = groups[0]
        self.assertEqual(blender["total_count"], 2)
        self.assertEqual(blender["installed_count"], 1)
        # re-sorted by options_slug regardless of input order
        self.assertEqual([p["options_slug"] for p in blender["points"]], ["bmw27", "quad-mesh"])
        # neither point has a "runs" key at all -- treated as no runs, not an error
        self.assertEqual(blender["run_status"], "none")

    def test_empty_list_yields_empty_groups(self):
        self.assertEqual(joblib.group_materialized_phoronix_points_by_test([]), [])

    def test_run_status_none_when_no_points_have_runs(self):
        points = [{"bare_name": "blender", "options_slug": "a", "runs": []},
                  {"bare_name": "blender", "options_slug": "b", "runs": []}]
        self.assertEqual(joblib.group_materialized_phoronix_points_by_test(points)[0]["run_status"], "none")

    def test_run_status_some_when_only_some_points_have_runs(self):
        points = [{"bare_name": "blender", "options_slug": "a", "runs": [{"run_id": "r1"}]},
                  {"bare_name": "blender", "options_slug": "b", "runs": []}]
        self.assertEqual(joblib.group_materialized_phoronix_points_by_test(points)[0]["run_status"], "some")

    def test_run_status_all_when_every_point_has_runs(self):
        points = [{"bare_name": "blender", "options_slug": "a", "runs": [{"run_id": "r1"}]},
                  {"bare_name": "blender", "options_slug": "b", "runs": [{"run_id": "r2"}]}]
        self.assertEqual(joblib.group_materialized_phoronix_points_by_test(points)[0]["run_status"], "all")


class ResolvePhoronixTestPointDirTest(unittest.TestCase):
    def test_accepts_real_materialized_dir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/blender-1.2.1", "arguments": ""}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")
            self.assertEqual(joblib.resolve_phoronix_test_point_dir(dest, info["dir"]), info["dir"])

    def test_rejects_path_outside_dest_root(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            os.makedirs(dest)
            outside = os.path.join(tmpdir, "elsewhere")
            os.makedirs(outside)
            self.assertIsNone(joblib.resolve_phoronix_test_point_dir(dest, outside))

    def test_rejects_dir_without_suite_definition(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            empty_dir = os.path.join(dest, "sometest", "default")
            os.makedirs(empty_dir)
            self.assertIsNone(joblib.resolve_phoronix_test_point_dir(dest, empty_dir))

    def test_rejects_empty_input(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertIsNone(joblib.resolve_phoronix_test_point_dir(tmpdir, ""))
            self.assertIsNone(joblib.resolve_phoronix_test_point_dir(tmpdir, None))


class CopyPhoronixTestPointToLocalSuiteTest(unittest.TestCase):
    def test_copies_suite_definition_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh"}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")
            fake_pts_home = os.path.join(tmpdir, "pts-home")

            dest_path = joblib.copy_phoronix_test_point_to_local_suite(
                info["dir"], info["identity"], user_data_dir=fake_pts_home)
            expected = os.path.join(fake_pts_home, "test-suites", "local", info["identity"],
                                     "suite-definition.xml")
            self.assertEqual(dest_path, expected)
            with open(dest_path) as f:
                copied = f.read()
            with open(os.path.join(info["dir"], "suite-definition.xml")) as f:
                original = f.read()
            self.assertEqual(copied, original)

            # Idempotent: re-copy overwrites cleanly, doesn't error or duplicate.
            joblib.copy_phoronix_test_point_to_local_suite(
                info["dir"], info["identity"], user_data_dir=fake_pts_home)
            self.assertTrue(os.path.isfile(dest_path))


class ListInstalledPhoronixTestVersionsTest(unittest.TestCase):
    def test_lists_matching_versions_sorted_numerically(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = os.path.join(tmpdir, "installed-tests", "pts")
            for name in ["build-linux-kernel-1.18.0", "build-linux-kernel-1.9.1", "build-linux-kernel-1.17.1",
                         "blender-1.2.1"]:
                os.makedirs(os.path.join(base, name))
            versions = joblib.list_installed_phoronix_test_versions(
                "pts/build-linux-kernel-1.17.1", user_data_dir=tmpdir)
            # Numeric sort, not lexicographic: 1.9.1 sorts before 1.17.1.
            self.assertEqual(versions, ["1.9.1", "1.17.1", "1.18.0"])

    def test_missing_namespace_dir_returns_empty(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(
                joblib.list_installed_phoronix_test_versions("pts/nope-1.0.0", user_data_dir=tmpdir), [])


class RepinPhoronixTestPointTest(unittest.TestCase):
    def test_rewrites_suite_xml_and_source_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/build-linux-kernel-1.17.1", "arguments": "defconfig"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml", installed=False)

            result = joblib.repin_phoronix_test_point(info["dir"], "1.18.0")
            self.assertEqual(result, {
                "old_test_id": "pts/build-linux-kernel-1.17.1",
                "new_test_id": "pts/build-linux-kernel-1.18.0",
                "dir": info["dir"],
            })

            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            self.assertEqual(root.find("Execute/Test").text, "pts/build-linux-kernel-1.18.0")
            self.assertEqual(root.find("Execute/Arguments").text, "defconfig")

            with open(os.path.join(info["dir"], "source.json")) as f:
                source = json.load(f)
            self.assertEqual(source["test_id"], "pts/build-linux-kernel-1.18.0")
            self.assertEqual(source["previous_test_id"], "pts/build-linux-kernel-1.17.1")
            self.assertIs(source["installed"], True)
            self.assertIn("repinned_at", source)

    def test_preserves_namespace_and_bare_name(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "system/selenium-1.0.47", "arguments": ""}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            result = joblib.repin_phoronix_test_point(info["dir"], "1.0.50")
            self.assertEqual(result["new_test_id"], "system/selenium-1.0.50")

    def test_missing_suite_definition_raises(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            with self.assertRaises(FileNotFoundError):
                joblib.repin_phoronix_test_point(tmpdir, "1.0.0")


class LinkPhoronixTestPointRunTest(unittest.TestCase):
    def test_creates_symlink_to_rundir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point_dir = os.path.join(tmpdir, "point")
            os.makedirs(point_dir)
            rundir = os.path.join(tmpdir, "output", "phoronix", "x", "run1")
            os.makedirs(rundir)
            ok = joblib.link_phoronix_test_point_run(point_dir, "run1", rundir)
            self.assertTrue(ok)
            link_path = os.path.join(point_dir, "runs", "run1")
            self.assertTrue(os.path.islink(link_path))
            self.assertEqual(os.path.realpath(link_path), os.path.realpath(rundir))

    def test_replaces_existing_link(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point_dir = os.path.join(tmpdir, "point")
            os.makedirs(point_dir)
            rundir1 = os.path.join(tmpdir, "run1")
            rundir2 = os.path.join(tmpdir, "run2")
            os.makedirs(rundir1)
            os.makedirs(rundir2)
            joblib.link_phoronix_test_point_run(point_dir, "runid", rundir1)
            joblib.link_phoronix_test_point_run(point_dir, "runid", rundir2)
            link_path = os.path.join(point_dir, "runs", "runid")
            self.assertEqual(os.path.realpath(link_path), os.path.realpath(rundir2))

    def test_degrades_to_false_on_unwritable_path(self):
        ok = joblib.link_phoronix_test_point_run("/proc/1/this-should-not-be-writable", "run1", "/tmp")
        self.assertFalse(ok)


class CollectRunFilesTest(unittest.TestCase):
    """collect_run_files() is shared between the curation studio's "+ add"
    buttons and build_reproducibility_bundle()'s archive contents -- exercise
    both the wspy-run unified-layout shape and the legacy fixed-config shape,
    plus the curation.json exclusion."""

    def test_wspy_run_layout(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest = {
                "layout_version": "1.0.0", "suite": "s", "benchmark": "b", "run_id": "r",
                "command": ["true"],
                "passes": [{"name": "quick", "output": "quick.txt",
                            "manifest": "quick.manifest.json", "status": "ok"}],
            }
            with open(os.path.join(tmpdir, "manifest.json"), "w") as f:
                json.dump(manifest, f)
            for name in ("quick.txt", "quick.manifest.json", "summary.txt", "launch.log"):
                open(os.path.join(tmpdir, name), "w").close()
            open(os.path.join(tmpdir, "curation.json"), "w").close()
            items = joblib.collect_run_files(tmpdir)
            filenames = [i["filename"] for i in items]
            self.assertIn("quick.txt", filenames)
            self.assertIn("quick.manifest.json", filenames)
            self.assertIn("summary.txt", filenames)
            self.assertIn("manifest.json", filenames)
            self.assertIn("launch.log", filenames)
            self.assertNotIn("curation.json", filenames)

    def test_legacy_fixed_config_layout(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            for name in (joblib.CSV_NAME, joblib.MANIFEST_NAME, joblib.LOG_NAME):
                open(os.path.join(tmpdir, name), "w").close()
            items = joblib.collect_run_files(tmpdir)
            filenames = [i["filename"] for i in items]
            self.assertIn(joblib.CSV_NAME, filenames)
            self.assertIn(joblib.MANIFEST_NAME, filenames)
            self.assertIn(joblib.LOG_NAME, filenames)

    def test_ai_analysis_files_labeled(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            open(os.path.join(tmpdir, "aiprompt.txt"), "w").close()
            open(os.path.join(tmpdir, "aianalysis.llama3.txt"), "w").close()
            items = joblib.collect_run_files(tmpdir)
            by_name = {i["filename"]: i for i in items}
            self.assertFalse(by_name["aiprompt.txt"]["ai_generated"])
            self.assertTrue(by_name["aianalysis.llama3.txt"]["ai_generated"])

    def test_plot_pngs_included(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            os.mkdir(os.path.join(tmpdir, "plots"))
            open(os.path.join(tmpdir, "plots", "foo.topdown.png"), "w").close()
            items = joblib.collect_run_files(tmpdir)
            filenames = [i["filename"] for i in items]
            self.assertIn("plots/foo.topdown.png", filenames)


class ClassifyBundleKindTest(unittest.TestCase):
    def test_manifest_kinds(self):
        self.assertEqual(joblib.classify_bundle_kind("manifest.json"), "manifest")
        self.assertEqual(joblib.classify_bundle_kind("quick.manifest.json"), "manifest")
        self.assertEqual(joblib.classify_bundle_kind(joblib.MANIFEST_NAME), "manifest")

    def test_derived_kinds(self):
        self.assertEqual(joblib.classify_bundle_kind("summary.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("curation.json"), "derived")
        self.assertEqual(joblib.classify_bundle_kind(joblib.PNG_NAME), "derived")
        self.assertEqual(joblib.classify_bundle_kind("plots/foo.png"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("process.tree.summary.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("aianalysis.llama3.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("aiprompt.txt"), "derived")

    def test_raw_kinds(self):
        self.assertEqual(joblib.classify_bundle_kind("quick.txt"), "raw")
        self.assertEqual(joblib.classify_bundle_kind("quick.csv"), "raw")
        self.assertEqual(joblib.classify_bundle_kind("process.tree.txt"), "raw")
        self.assertEqual(joblib.classify_bundle_kind("launch.log"), "raw")
        self.assertEqual(joblib.classify_bundle_kind(joblib.CSV_NAME), "raw")


class BuildReproducibilityBundleTest(unittest.TestCase):
    def _make_rundir(self, tmpdir):
        manifest = {
            "layout_version": "1.0.0", "suite": "s", "benchmark": "b", "run_id": "r",
            "command": ["true"],
            "passes": [{"name": "quick", "output": "quick.txt",
                        "manifest": "quick.manifest.json", "status": "ok"}],
        }
        with open(os.path.join(tmpdir, "manifest.json"), "w") as f:
            json.dump(manifest, f)
        with open(os.path.join(tmpdir, "quick.txt"), "w") as f:
            f.write("elapsed 1.0\n")
        with open(os.path.join(tmpdir, "quick.manifest.json"), "w") as f:
            f.write("{}")
        with open(os.path.join(tmpdir, "summary.txt"), "w") as f:
            f.write("=== quick ===\nelapsed 1.0\n")

    def test_bundle_contains_expected_files_and_index(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self._make_rundir(tmpdir)
            tar_bytes, index = joblib.build_reproducibility_bundle(tmpdir, "s", "b", "r")
            self.assertEqual(index["schema_version"], joblib.BUNDLE_SCHEMA_VERSION)
            self.assertEqual(index["suite"], "s")
            self.assertEqual(index["benchmark"], "b")
            self.assertEqual(index["run_id"], "r")
            by_path = {e["path"]: e for e in index["files"]}
            self.assertEqual(by_path["quick.txt"]["kind"], "raw")
            self.assertEqual(by_path["quick.manifest.json"]["kind"], "manifest")
            self.assertEqual(by_path["manifest.json"]["kind"], "manifest")
            self.assertEqual(by_path["summary.txt"]["kind"], "derived")

            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tar:
                names = tar.getnames()
                self.assertIn("quick.txt", names)
                self.assertIn(joblib.BUNDLE_MANIFEST_NAME, names)
                bundle_manifest = json.loads(
                    tar.extractfile(joblib.BUNDLE_MANIFEST_NAME).read().decode("utf-8"))
                self.assertEqual(bundle_manifest, index)

    def test_checksums_verify_against_extracted_content(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self._make_rundir(tmpdir)
            tar_bytes, index = joblib.build_reproducibility_bundle(tmpdir, "s", "b", "r")
            import hashlib
            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tar:
                for entry in index["files"]:
                    data = tar.extractfile(entry["path"]).read()
                    self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
                    self.assertEqual(len(data), entry["size_bytes"])

    def test_missing_file_degrades_not_fails(self):
        """A file collect_run_files() lists but that vanishes/becomes
        unreadable between listing and archiving gets kind="missing" rather
        than aborting the whole bundle -- this is an inherent TOCTOU race in
        real use (collect_run_files() itself already checks os.path.isfile()
        at listing time), so it's exercised here by monkeypatching
        collect_run_files() to report a file that was never actually
        created, rather than trying to reproduce the race itself."""
        with tempfile.TemporaryDirectory() as tmpdir:
            self._make_rundir(tmpdir)
            real_collect = joblib.collect_run_files
            try:
                joblib.collect_run_files = lambda rundir: real_collect(rundir) + [
                    {"filename": "vanished.txt", "kind": "text", "label": "vanished.txt",
                     "ai_generated": False}
                ]
                tar_bytes, index = joblib.build_reproducibility_bundle(tmpdir, "s", "b", "r")
            finally:
                joblib.collect_run_files = real_collect
            by_path = {e["path"]: e for e in index["files"]}
            self.assertEqual(by_path["vanished.txt"]["kind"], "missing")
            self.assertNotIn("sha256", by_path["vanished.txt"])
            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tar:
                self.assertNotIn("vanished.txt", tar.getnames())


if __name__ == "__main__":
    unittest.main()
