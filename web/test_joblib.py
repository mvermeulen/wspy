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


if __name__ == "__main__":
    unittest.main()
