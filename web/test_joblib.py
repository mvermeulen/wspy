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
