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
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

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


class ExpandPresetNamesTest(unittest.TestCase):
    def test_plain_name_passes_through(self):
        self.assertEqual(joblib.expand_preset_names("deep-cpu"), ["deep-cpu"])

    def test_comma_list_passes_through(self):
        self.assertEqual(joblib.expand_preset_names("deep-cpu,tree-heavy"),
                          ["deep-cpu", "tree-heavy"])

    def test_composite_preset_expands_to_constituent_profiles(self):
        # Regression coverage: execute_profile_run()'s tree-heavy/gpu-compute
        # post-processing detection used to check the raw, unexpanded
        # profile string, so a composite preset's embedded tree pass
        # (zen4plus-deep composes tree-heavy) never triggered
        # run_proctree_besteffort() automatically.
        self.assertEqual(joblib.expand_preset_names("zen4plus-deep"),
                          ["deep-cpu", "ibs-sample", "tree-heavy"])
        self.assertIn("tree-heavy", joblib.expand_preset_names("zen4plus-deep"))
        self.assertEqual(joblib.expand_preset_names("zen-portable"),
                          ["quick", "ibs-basic"])


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

    def test_counters_software_group_actually_enables_it(self):
        # Regression test for ALL_GROUPS' "software" entry, which used to be
        # (wrongly) marked default_on -- checking it here used to silently
        # emit nothing at all (wspy's own counter_mask default is COUNTER_IPC
        # only, never software), surfaced via item 10's --target having
        # nothing to attach. Fixed by marking it not-default_on like every
        # other non-ipc group.
        checklist = {"counters": {"enabled": True, "groups": ["software"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--counters=software", passes[0]["flags"])

    def test_counters_arm_group_selectable(self):
        # 4.4(a) "ARM group exposure" slice: the 3 ARM-only groups
        # (arm-dcache-mem/arm-icache-tlb/arm-mem-align-tlb) are now real
        # ALL_GROUPS entries, selectable through the same checklist path as
        # any other group.
        checklist = {"counters": {"enabled": True, "groups": ["arm-dcache-mem"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--counters=arm-dcache-mem", passes[0]["flags"])

    def test_tree_default_has_no_counter_groups(self):
        # A bare "tree" pass (no groups selected) is tree-structure-only --
        # --no-ipc for wspy's own default-on ipc group, no --counters=<list>
        # at all, matching "counters"' own empty-selection behavior.
        checklist = {"tree": {"enabled": True}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertIn("--no-ipc", flags)
        self.assertFalse(any(f.startswith("--counters=") for f in flags))

    def test_tree_groups_selects_counters_and_keeps_ipc_when_checked(self):
        # Same counter_group_flags() helper "counters" uses -- selecting ipc
        # suppresses --no-ipc, selecting software emits --counters=software.
        # This is also what --target (below) attaches to for its pid-scoped
        # matches, since it's the same wspy process/counter_mask.
        checklist = {"tree": {"enabled": True, "groups": ["ipc", "software"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        flags = passes[0]["flags"]
        self.assertNotIn("--no-ipc", flags)
        self.assertIn("--counters=software", flags)

    def test_tree_target_flag_passthrough(self):
        checklist = {"tree": {"enabled": True, "target": "comm=coremark.exe,cmdline=iter"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertEqual(len(passes), 1)
        self.assertIn("--target=comm=coremark.exe,cmdline=iter", passes[0]["flags"])

    def test_tree_target_omitted_when_blank(self):
        checklist = {"tree": {"enabled": True, "target": "   "}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        self.assertFalse(any(f.startswith("--target=") for f in passes[0]["flags"]))

    def test_tree_groups_round_trip_through_config_options(self):
        checklist = {"tree": {"enabled": True, "groups": ["ipc", "software"]}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        restored = joblib.checklist_section_from_options("tree", passes[0]["options"])
        self.assertEqual(sorted(restored["groups"]), ["ipc", "software"])

    # --symbol-sample/--symbol-sample-event (item 9's Run-tab drill-down,
    # INVESTIGATION.md's "Symbol-level profiling deep-dive") -- mirrors the
    # --target tests above.
    def test_symbol_sample_flags_passthrough(self):
        checklist = {"tree": {"enabled": True, "target": "comm=coremark.exe",
                               "symbol_sample": True, "symbol_sample_event": "cache-misses"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        flags = passes[0]["flags"]
        self.assertIn("--symbol-sample", flags)
        self.assertIn("--symbol-sample-event=cache-misses", flags)

    def test_symbol_sample_omitted_when_unchecked(self):
        checklist = {"tree": {"enabled": True, "target": "comm=coremark.exe",
                               "symbol_sample": False, "symbol_sample_event": "cycles"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        flags = passes[0]["flags"]
        self.assertNotIn("--symbol-sample", flags)
        self.assertFalse(any(f.startswith("--symbol-sample-event") for f in flags))

    def test_symbol_sample_event_omitted_when_blank(self):
        # A checked box with no event selected still emits the boolean flag
        # (defaults to cycles server-side, see wspy.c) but no bogus
        # --symbol-sample-event= with an empty value.
        checklist = {"tree": {"enabled": True, "target": "comm=coremark.exe",
                               "symbol_sample": True, "symbol_sample_event": ""}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        flags = passes[0]["flags"]
        self.assertIn("--symbol-sample", flags)
        self.assertFalse(any(f.startswith("--symbol-sample-event") for f in flags))

    def test_symbol_sample_round_trips_through_config_options(self):
        checklist = {"tree": {"enabled": True, "target": "comm=coremark.exe",
                               "symbol_sample": True, "symbol_sample_event": "instructions"}}
        passes = joblib.build_configuration_passes("/tmp/rundir", checklist)
        restored = joblib.checklist_section_from_options("tree", passes[0]["options"])
        self.assertTrue(restored["symbol_sample"])
        self.assertEqual(restored["symbol_sample_event"], "instructions")

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


class LoadProfileConfPassesTest(unittest.TestCase):
    """Against the real profiles/*.conf files -- same posture as
    ExpandPresetNamesTest above, which reads the real profiles/*.spec files
    rather than fixtures, since these are checked-in, stable repo data."""

    def test_quick_has_one_pass_no_interval(self):
        passes = joblib._load_profile_conf_passes("quick")
        self.assertEqual(len(passes), 1)
        self.assertNotIn("--interval", passes[0])

    def test_deep_cpu_has_two_interval_passes_and_one_passes_sweep(self):
        passes = joblib._load_profile_conf_passes("deep-cpu")
        interval_passes = [p for p in passes if "--interval" in p]
        non_interval = [p for p in passes if "--interval" not in p]
        self.assertEqual(len(interval_passes), 2)  # systemtime, amdtopdown
        self.assertEqual(len(non_interval), 1)  # counters (--passes=... sweep)
        self.assertTrue(any(t.startswith("--passes=") for t in non_interval[0]))

    def test_unknown_profile_returns_empty(self):
        self.assertEqual(joblib._load_profile_conf_passes("not-a-real-profile"), [])

    def test_composite_spec_file_returns_empty(self):
        # zen-portable has a .spec file, not a .conf file -- this function
        # only reads .conf files; profile_plottable_columns() below is what
        # calls expand_preset_names() first to resolve a composite down to
        # its constituent .conf-backed names before ever reaching here.
        self.assertEqual(joblib._load_profile_conf_passes("zen-portable"), [])


class WspyListColumnsTest(unittest.TestCase):
    def test_parses_header_from_stdout(self):
        def fake_run(argv, cwd, capture_output, text, timeout):
            self.assertIn("--list-columns", argv)
            return subprocess.CompletedProcess(argv, 0, stdout="a,b,c,\n", stderr="warning: noise\n")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            cols = joblib._wspy_list_columns("/fake/wspy", ["--counters=topdown"])
        self.assertEqual(cols, {"a", "b", "c"})

    def test_ignores_blank_trailing_lines(self):
        # stderr diagnostics never reach this (subprocess.run separates the
        # streams) but a trailing blank line on stdout itself is real and
        # must be skipped in favor of the last non-blank line.
        def fake_run(argv, cwd, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 0, stdout="time,ipc,\n\n", stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            cols = joblib._wspy_list_columns("/fake/wspy", [])
        self.assertEqual(cols, {"time", "ipc"})

    def test_empty_set_on_nonzero_exit(self):
        def fake_run(argv, cwd, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 1, stdout="", stderr="error")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            cols = joblib._wspy_list_columns("/fake/wspy", [])
        self.assertEqual(cols, set())

    def test_empty_set_on_missing_binary(self):
        with patch("joblib.subprocess.run", side_effect=OSError("no such file")):
            cols = joblib._wspy_list_columns("/does/not/exist", [])
        self.assertEqual(cols, set())

    def test_empty_set_on_timeout(self):
        with patch("joblib.subprocess.run",
                    side_effect=subprocess.TimeoutExpired(cmd="wspy", timeout=30)):
            cols = joblib._wspy_list_columns("/fake/wspy", [])
        self.assertEqual(cols, set())


class ProfilePlottableColumnsTest(unittest.TestCase):
    def test_only_interval_passes_are_queried(self):
        calls = []

        def fake_run(argv, cwd, capture_output, text, timeout):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, stdout="x,\n", stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            # deep-cpu.conf has 3 passes: systemtime (--interval), counters
            # (--passes=..., no --interval), amdtopdown (--interval) -- only
            # the two --interval ones should ever reach --list-columns.
            cols = joblib.profile_plottable_columns("/fake/wspy/1", "deep-cpu")
        self.assertEqual(len(calls), 2)
        self.assertEqual(cols, {"x"})

    def test_composite_expands_before_querying(self):
        def fake_run(argv, cwd, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 0, stdout="ibs_fetch,ibs_op,\n", stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            # zen-portable = quick,ibs-basic -- quick has no --interval pass,
            # ibs-basic's does, so only ibs-basic's columns should surface.
            cols = joblib.profile_plottable_columns("/fake/wspy/2", "zen-portable")
        self.assertEqual(cols, {"ibs_fetch", "ibs_op"})

    def test_result_is_cached_per_wspy_bin_and_token(self):
        calls = []

        def fake_run(argv, cwd, capture_output, text, timeout):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, stdout="ibs_fetch,\n", stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            joblib.profile_plottable_columns("/fake/wspy/3", "ibs-basic")
            n_after_first = len(calls)
            joblib.profile_plottable_columns("/fake/wspy/3", "ibs-basic")
        self.assertEqual(len(calls), n_after_first)  # second call served from cache, no new subprocess

    def test_no_conf_file_returns_empty(self):
        self.assertEqual(joblib.profile_plottable_columns("/fake/wspy/4", "not-a-real-profile"), set())


class BuildSupplementaryPlotPassesTest(unittest.TestCase):
    def test_no_custom_plots_returns_empty(self):
        passes, notes = joblib.build_supplementary_plot_passes(
            "/tmp/rundir", "deep-cpu", [], "/fake/wspy/5")
        self.assertEqual(passes, [])
        self.assertEqual(notes, [])

    def test_covered_column_produces_no_supplementary_pass(self):
        def fake_run(argv, cwd, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 0, stdout="retire,frontend,\n", stderr="")

        custom_plots = [{"name": "myplot", "columns": ["retire"]}]
        with patch("joblib.subprocess.run", side_effect=fake_run):
            passes, notes = joblib.build_supplementary_plot_passes(
                "/tmp/rundir", "deep-cpu", custom_plots, "/fake/wspy/6")
        self.assertEqual(passes, [])
        self.assertEqual(notes, [])

    def test_missing_column_adds_supplementary_pass(self):
        def fake_run(argv, cwd, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 0, stdout="retire,\n", stderr="")

        custom_plots = [{"name": "myplot", "columns": ["l2miss"]}]
        with patch("joblib.subprocess.run", side_effect=fake_run):
            passes, notes = joblib.build_supplementary_plot_passes(
                "/tmp/rundir", "deep-cpu", custom_plots, "/fake/wspy/7")
        self.assertEqual(len(passes), 1)
        self.assertTrue(passes[0]["name"].startswith("plotdata-"))
        self.assertTrue(any("l2miss" in n for n in notes))


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

    def test_build_job_round_trips_phoronix_test_point(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick",
                                phoronix_test_point="coremark/default")
        self.assertEqual(joblib.validate_job(job), [])
        self.assertEqual(job["phoronix_test_point"], "coremark/default")

    def test_build_job_defaults_phoronix_test_point_to_none(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        self.assertIsNone(job["phoronix_test_point"])

    def test_validate_job_rejects_non_string_phoronix_test_point(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="quick")
        job["phoronix_test_point"] = ["not", "a", "string"]
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

    def test_arm_columns(self):
        # 4.4(a) "ARM group exposure" slice -- topdown.c's
        # arm_raw_events[]/print_arm_{dcache_mem,icache_tlb,mem_align_tlb}()
        # CSV column names, resolved to their ALL_GROUPS entry.
        for col in ("l1d_cache_refill", "l1d_tlb_refill", "l2d_cache_refill", "l2d_tlb_refill"):
            self.assertEqual(joblib.resolve_column_group(col), "arm-dcache-mem")
        for col in ("l1i_cache_refill", "l1i_tlb_refill", "l2i_tlb_refill"):
            self.assertEqual(joblib.resolve_column_group(col), "arm-icache-tlb")
        for col in ("dtlb_walk", "itlb_walk", "ld_align_lat", "st_align_lat"):
            self.assertEqual(joblib.resolve_column_group(col), "arm-mem-align-tlb")


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
            "tree", [("cmdline", "true"), ("open", "false"), ("futex", "true")])
        self.assertEqual(section, {"enabled": True, "cmdline": True, "open": False, "futex": True})

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


def _make_node(pid, comm, utime=0.0, stime=0.0, ppid=0, children=None):
    return {"pid": pid, "comm": comm, "ppid": ppid, "utime_seconds": utime, "stime_seconds": stime,
            "children": children or []}


class RenderTopProcessesTextTest(unittest.TestCase):
    def _tree_json(self):
        # root(0s) -> a(1s), b(5s) -> c(3s), d(0.1s)
        c = _make_node(3, "c", utime=2.0, stime=1.0, ppid=2)
        d = _make_node(4, "d", utime=0.1, ppid=2)
        b = _make_node(2, "b", utime=5.0, ppid=1, children=[c, d])
        a = _make_node(5, "a", utime=1.0, ppid=1)
        root = _make_node(1, "root", children=[a, b])
        return {"process_count": 5, "tree": root}

    def test_ranks_by_cpu_time_descending(self):
        text = joblib.render_top_processes_text(self._tree_json(), top_min=2, top_max=2)
        lines = [l for l in text.splitlines() if "pid=" in l]
        self.assertEqual(len(lines), 2)
        self.assertIn("pid=2", lines[0])   # b: 5s, highest
        self.assertIn("pid=3", lines[1])   # c: 3s, second

    def test_top_count_respects_min_and_max_clamp(self):
        text = joblib.render_top_processes_text(self._tree_json(), top_fraction=0.5,
                                                  top_min=1, top_max=1)
        lines = [l for l in text.splitlines() if "pid=" in l]
        self.assertEqual(len(lines), 1)

    def test_no_tree_data(self):
        self.assertEqual(joblib.render_top_processes_text({}), "(no tree data)\n")


class RenderTop1pctTreeTextTest(unittest.TestCase):
    def _tree_json(self):
        c = _make_node(3, "c", utime=2.0, stime=1.0, ppid=2)
        d = _make_node(4, "d", utime=0.1, ppid=2)
        b = _make_node(2, "b", utime=5.0, ppid=1, children=[c, d])
        a = _make_node(5, "a", utime=1.0, ppid=1)
        root = _make_node(1, "root", children=[a, b])
        return {"process_count": 5, "tree": root}

    def test_kept_nodes_marked_and_ancestor_chain_preserved(self):
        text = joblib.render_top1pct_tree_text(self._tree_json(), top_min=1, top_max=1)
        # only pid=2 (b, 5s) is kept; root is its ancestor so must still appear
        self.assertIn("pid=1)", text)   # root, ancestor of the kept node
        self.assertIn("* ", text)
        self.assertIn("pid=2)", text)   # b, the kept node itself
        self.assertNotIn("pid=5)", text)   # a, sibling of b, not an ancestor -- omitted

    def test_omitted_subtree_reports_per_level_counts(self):
        # top_min=1 keeps only pid=2 (b, 5s). Each collapsed subtree gets its
        # own omitted-count line at the level it was collapsed, not one
        # combined total: b's own two children (c, d) collapse to "2" under
        # b, and sibling branch a (no children of its own) collapses to "1"
        # under root -- keeps the shape of what was hidden visible, per the
        # function's own docstring, rather than flattening it into one number.
        text = joblib.render_top1pct_tree_text(self._tree_json(), top_min=1, top_max=1)
        self.assertIn("2 more process(es) omitted", text)
        self.assertIn("1 more process(es) omitted", text)

    def test_no_tree_data(self):
        self.assertEqual(joblib.render_top1pct_tree_text({}), "(no tree data)\n")


class BuildSymbolizeArgvTest(unittest.TestCase):
    # item 9's web UI drill-down (INVESTIGATION.md's "Symbol-level profiling
    # deep-dive") -- /api/symbolize builds this argv via build_symbolize_argv().
    def test_pid_selector(self):
        self.assertEqual(
            joblib.build_symbolize_argv("./wspy-symbolize", "./proctree", "/r/process.tree.txt", pid=123),
            ["./wspy-symbolize", "--tree-file", "/r/process.tree.txt", "--proctree", "./proctree",
             "--json", "--pid", "123"])

    def test_comm_selector(self):
        self.assertEqual(
            joblib.build_symbolize_argv("./wspy-symbolize", "./proctree", "/r/process.tree.txt", comm="myworker"),
            ["./wspy-symbolize", "--tree-file", "/r/process.tree.txt", "--proctree", "./proctree",
             "--json", "--comm", "myworker"])

    def test_pid_takes_precedence_when_both_given(self):
        # Caller's responsibility to give exactly one (see the function's own
        # docstring) -- pid wins if both are passed, rather than emitting an
        # argv with both --pid and --comm.
        self.assertEqual(
            joblib.build_symbolize_argv("./wspy-symbolize", "./proctree", "/r/process.tree.txt",
                                         pid=123, comm="myworker"),
            ["./wspy-symbolize", "--tree-file", "/r/process.tree.txt", "--proctree", "./proctree",
             "--json", "--pid", "123"])


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

    def test_resolves_local_suite_to_real_test_id(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            joblib.materialize_phoronix_test_point(
                {"test_id": "pts/build-linux-kernel-1.18.0", "arguments": "defconfig"},
                dest, "file", "/tmp/src.xml")
            fake_bin = self._make_fake_phoronix(tmpdir, {
                "pts/build-linux-kernel-1.18.0":
                    "Test Installed: Yes\nTimes Run: 3\nAverage Run-Time: 90 Seconds\n",
            })
            workload = "phoronix-test-suite batch-run local/build-linux-kernel-defconfig"
            result = joblib.estimate_phoronix_workload_seconds(
                workload, phoronix_bin=fake_bin, dest_root=dest)
            self.assertEqual(len(result["tests"]), 1)
            entry = result["tests"][0]
            self.assertNotIn("error", entry)
            self.assertEqual(entry["installed"], "Yes")
            self.assertEqual(entry["queried_name"], "pts/build-linux-kernel-1.18.0")
            self.assertEqual(entry["queried_name_reason"], "local-suite")
            self.assertEqual(result["total_seconds"], 90.0)

    def test_unmatched_local_suite_falls_back_to_querying_as_is(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")  # empty -- no materialized points
            fake_bin = self._make_fake_phoronix(tmpdir, {
                "local/some-hand-installed-suite": "Test Installed: Yes\nEstimated Run-Time: 5 Seconds\n",
            })
            workload = "phoronix-test-suite batch-run local/some-hand-installed-suite"
            result = joblib.estimate_phoronix_workload_seconds(
                workload, phoronix_bin=fake_bin, dest_root=dest)
            entry = result["tests"][0]
            self.assertNotIn("error", entry)
            self.assertNotIn("queried_name", entry)


class ResolvePhoronixLocalSuiteTestIdsTest(unittest.TestCase):
    def test_maps_local_identity_to_test_id(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.materialize_phoronix_test_point(
                {"test_id": "pts/blender-1.2.1", "arguments": "quad-mesh"}, dest, "file", "/tmp/src.xml")
            mapping = joblib.resolve_phoronix_local_suite_test_ids(
                [f"local/{info['identity']}", "pts/coremark-1.0.1"], dest)
            self.assertEqual(mapping, {f"local/{info['identity']}": "pts/blender-1.2.1"})

    def test_no_local_names_skips_scan(self):
        # dest_root doesn't even need to exist when nothing looks like local/<identity>
        self.assertEqual(
            joblib.resolve_phoronix_local_suite_test_ids(["pts/coremark-1.0.1"], "/nonexistent/dest"), {})

    def test_unmatched_local_identity_is_omitted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(
                joblib.resolve_phoronix_local_suite_test_ids(["local/no-such-point"], tmpdir), {})


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

    def test_no_arguments_with_description_includes_description_element(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/build-python-1.0.0", "arguments": "",
                     "description": "Build Configuration: Default"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            self.assertIsNone(root.find("Execute/Arguments"))
            self.assertEqual(root.find("Execute/Description").text, "Build Configuration: Default")

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


class FindMaterializedPhoronixTestPointTest(unittest.TestCase):
    def test_finds_entry_by_identity(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")

            entry = joblib.find_materialized_phoronix_test_point(dest, info["identity"])
            self.assertIsNotNone(entry)
            self.assertEqual(entry["identity"], info["identity"])
            self.assertEqual(entry["test_id"], "pts/openssl-1.0.0")

    def test_returns_none_when_not_found(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            self.assertIsNone(joblib.find_materialized_phoronix_test_point(dest, "openssl-sha256"))


class ResolveTestIdentityTest(unittest.TestCase):
    def test_matches_materialized_phoronix_test_point(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            point = {"test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
            info = joblib.materialize_phoronix_test_point(point, dest, "file", "/tmp/src.xml")

            test, test_point, warning = joblib.resolve_test_identity("phoronix", info["identity"], dest)
            self.assertEqual(test, info["bare_name"])
            self.assertEqual(test_point, info["options_slug"])
            self.assertIsNone(warning)

    def test_unmatched_phoronix_identity_falls_back_with_warning(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            test, test_point, warning = joblib.resolve_test_identity("phoronix", "openssl-sha256", dest)
            self.assertEqual(test, "openssl-sha256")
            self.assertEqual(test_point, "default")
            self.assertIsNotNone(warning)

    def test_non_phoronix_suite_always_falls_back_with_no_warning(self):
        test, test_point, warning = joblib.resolve_test_identity(
            "some-other-suite", "some-benchmark", "/does/not/matter")
        self.assertEqual(test, "some-benchmark")
        self.assertEqual(test_point, "default")
        self.assertIsNone(warning)

    def test_cpu2026_falls_back_with_no_warning_when_dest_root_not_given(self):
        # cpu2026_dest_root defaults to None -- existing callers (e.g. wspy-testpoint) that never
        # pass it keep their exact prior behavior, no cpu2026-specific matching attempted at all.
        test, test_point, warning = joblib.resolve_test_identity(
            "cpu2026", "706.stockfish_r-gcc_O3-base", "/does/not/matter")
        self.assertEqual(test, "706.stockfish_r-gcc_O3-base")
        self.assertEqual(test_point, "default")
        self.assertIsNone(warning)

    def test_matches_materialized_cpu2026_point_when_dest_root_given(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O3")

            test, test_point, warning = joblib.resolve_test_identity(
                "cpu2026", info["identity"], "/does/not/matter", dest)
            self.assertEqual(test, "706.stockfish_r")
            self.assertEqual(test_point, "gcc_O3-base")
            self.assertIsNone(warning)

    def test_unmatched_cpu2026_identity_falls_back_with_warning(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            test, test_point, warning = joblib.resolve_test_identity(
                "cpu2026", "706.stockfish_r-gcc_O3-base", "/does/not/matter", dest)
            self.assertEqual(test, "706.stockfish_r-gcc_O3-base")
            self.assertEqual(test_point, "default")
            self.assertIsNotNone(warning)


class ParseSummaryCsvTest(unittest.TestCase):
    def test_parses_rows_by_header(self):
        text = "group,metric,n,verdict\nipc,ipc,3,PASS\ncache,l1_miss_rate,3,WARN:noisy\n"
        rows = joblib.parse_summary_csv(text)
        self.assertEqual(rows, [
            {"group": "ipc", "metric": "ipc", "n": "3", "verdict": "PASS"},
            {"group": "cache", "metric": "l1_miss_rate", "n": "3", "verdict": "WARN:noisy"},
        ])

    def test_empty_text_returns_empty_list(self):
        self.assertEqual(joblib.parse_summary_csv(""), [])

    def test_header_only_returns_empty_list(self):
        self.assertEqual(joblib.parse_summary_csv("group,metric,n,verdict\n"), [])

    def test_short_row_is_skipped(self):
        text = "group,metric,n,verdict\nipc,ipc,3\n"  # missing the trailing verdict field
        self.assertEqual(joblib.parse_summary_csv(text), [])


class EnumerateReferenceMatrixCellsTest(unittest.TestCase):
    def test_no_cell_without_a_runs_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            point = {"test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
            joblib.materialize_phoronix_test_point(point, phoronix_dest, "file", "/tmp/src.xml")
            report_root_path = os.path.join(tmpdir, "report-root")
            os.makedirs(report_root_path)

            cells = joblib.enumerate_reference_matrix_cells(report_root_path, phoronix_dest, None)
            self.assertEqual(cells, [])

    def test_one_cell_per_machine_with_runs_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            point = {"test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
            info = joblib.materialize_phoronix_test_point(point, phoronix_dest, "file", "/tmp/src.xml")
            report_root_path = os.path.join(tmpdir, "report-root")
            for machine in ("amd-395", "amd-370-64gb"):
                machine_dir = os.path.join(report_root_path, "phoronix", info["bare_name"],
                                            info["options_slug"], machine)
                os.makedirs(machine_dir)
                with open(os.path.join(machine_dir, "runs.json"), "w") as f:
                    f.write("{}")

            cells = joblib.enumerate_reference_matrix_cells(report_root_path, phoronix_dest, None)
            self.assertEqual([c["machine"] for c in cells], ["amd-370-64gb", "amd-395"])  # sorted
            for c in cells:
                self.assertEqual(c["suite"], "phoronix")
                self.assertEqual(c["test"], info["bare_name"])
                self.assertEqual(c["test_point"], info["options_slug"])
                self.assertEqual(c["benchmark"], info["identity"])

    def test_machine_directory_without_runs_json_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            point = {"test_id": "pts/openssl-1.0.0", "arguments": "-evp sha256"}
            info = joblib.materialize_phoronix_test_point(point, phoronix_dest, "file", "/tmp/src.xml")
            report_root_path = os.path.join(tmpdir, "report-root")
            machine_dir = os.path.join(report_root_path, "phoronix", info["bare_name"],
                                        info["options_slug"], "amd-395")
            os.makedirs(machine_dir)  # no runs.json written

            cells = joblib.enumerate_reference_matrix_cells(report_root_path, phoronix_dest, None)
            self.assertEqual(cells, [])

    def test_cpu2026_test_point_uses_tag_tune_as_test_point(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cpu2026_dest = os.path.join(tmpdir, "cpu2026")
            info = joblib.register_cpu2026_point(cpu2026_dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O3")
            report_root_path = os.path.join(tmpdir, "report-root")
            machine_dir = os.path.join(report_root_path, "cpu2026", "706.stockfish_r", "gcc_O3-base",
                                        "amd-395")
            os.makedirs(machine_dir)
            with open(os.path.join(machine_dir, "runs.json"), "w") as f:
                f.write("{}")

            cells = joblib.enumerate_reference_matrix_cells(report_root_path, None, cpu2026_dest)
            self.assertEqual(len(cells), 1)
            self.assertEqual(cells[0], {"suite": "cpu2026", "test": "706.stockfish_r",
                                         "test_point": "gcc_O3-base", "machine": "amd-395",
                                         "benchmark": info["identity"]})

    def test_no_dest_roots_given_returns_empty(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(joblib.enumerate_reference_matrix_cells(tmpdir, None, None), [])


class CountStatsPoolRunsTest(unittest.TestCase):
    def test_counts_only_stats_pool_role(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            runs_json_path = os.path.join(tmpdir, "runs.json")
            with open(runs_json_path, "w") as f:
                json.dump({"runs": [{"role": "stats-pool"}, {"role": "stats-pool"},
                                     {"role": "supplementary"}, {"role": "excluded"}]}, f)
            self.assertEqual(joblib.count_stats_pool_runs(runs_json_path), 2)

    def test_zero_when_missing(self):
        self.assertEqual(joblib.count_stats_pool_runs("/does/not/exist/runs.json"), 0)

    def test_zero_when_unparseable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            runs_json_path = os.path.join(tmpdir, "runs.json")
            with open(runs_json_path, "w") as f:
                f.write("not json")
            self.assertEqual(joblib.count_stats_pool_runs(runs_json_path), 0)


class LoadReferenceMatrixCellRunsTest(unittest.TestCase):
    def test_returns_full_runs_list_all_roles(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            runs_json_path = os.path.join(tmpdir, "runs.json")
            runs = [{"run_id": "r1", "role": "stats-pool"}, {"run_id": "r2", "role": "supplementary"},
                    {"run_id": "r3", "role": "excluded"}]
            with open(runs_json_path, "w") as f:
                json.dump({"runs": runs}, f)
            self.assertEqual(joblib.load_reference_matrix_cell_runs(runs_json_path), runs)

    def test_empty_list_when_missing(self):
        self.assertEqual(joblib.load_reference_matrix_cell_runs("/does/not/exist/runs.json"), [])

    def test_empty_list_when_unparseable(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            runs_json_path = os.path.join(tmpdir, "runs.json")
            with open(runs_json_path, "w") as f:
                f.write("not json")
            self.assertEqual(joblib.load_reference_matrix_cell_runs(runs_json_path), [])


class AggregateReferenceMatrixCellTest(unittest.TestCase):
    def test_returns_parsed_rows_on_success(self):
        fake_stdout = "group,metric,n,min,max,mean,stddev,cv_percent,verdict\nipc,ipc,3,1.0,1.2,1.1,0.1,9.0,PASS\n"

        def fake_run(argv, capture_output, text, timeout):
            self.assertIn("aggregate", argv)
            self.assertIn("--csv", argv)
            self.assertIn("--quiet", argv)
            return subprocess.CompletedProcess(argv, 0, stdout=fake_stdout, stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            rows = joblib.aggregate_reference_matrix_cell(
                "/path/to/wspy-testpoint", "/path/to/store.db", "phoronix", "openssl-sha256",
                "amd-395")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["metric"], "ipc")
        self.assertEqual(rows[0]["verdict"], "PASS")

    def test_none_on_nonzero_exit(self):
        def fake_run(argv, capture_output, text, timeout):
            return subprocess.CompletedProcess(argv, 1, stdout="", stderr="no stats-pool runs")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            rows = joblib.aggregate_reference_matrix_cell(
                "/path/to/wspy-testpoint", "/path/to/store.db", "phoronix", "openssl-sha256",
                "amd-395")
        self.assertIsNone(rows)

    def test_none_on_launch_failure(self):
        with patch("joblib.subprocess.run", side_effect=OSError("no such file")):
            rows = joblib.aggregate_reference_matrix_cell(
                "/does/not/exist", "/path/to/store.db", "phoronix", "openssl-sha256", "amd-395")
        self.assertIsNone(rows)

    def test_passes_report_root_override_through(self):
        # No report_root_remote parameter -- `wspy-testpoint aggregate` never accepted
        # --report-root-remote (it only reads an already-selected run set, never clones); a caller
        # passing one through here was a latent, never-hit bug, found live building
        # scripts/publish_reference_matrix.py.
        captured = {}

        def fake_run(argv, capture_output, text, timeout):
            captured["argv"] = argv
            return subprocess.CompletedProcess(argv, 0, stdout="metric\n", stderr="")

        with patch("joblib.subprocess.run", side_effect=fake_run):
            joblib.aggregate_reference_matrix_cell(
                "wspy-testpoint", "store.db", "phoronix", "openssl-sha256", "amd-395",
                report_root_path="/custom/root")
        self.assertIn("--report-root", captured["argv"])
        self.assertIn("/custom/root", captured["argv"])
        self.assertNotIn("--report-root-remote", captured["argv"])


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

    def test_lists_matching_versions_across_namespaces(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            pts_base = os.path.join(tmpdir, "installed-tests", "pts")
            os.makedirs(os.path.join(pts_base, "compress-zstd-1.6.0"))
            versions = joblib.list_installed_phoronix_test_versions(
                "system/compress-zstd-1.5.0", user_data_dir=tmpdir)
            self.assertEqual(versions, ["1.6.0"])


def _write_phoronix_test_definition(user_data_dir, namespace, name, menu_entries):
    """Test helper: writes a minimal test-profiles/<namespace>/<name>/
    test-definition.xml with one <TestSettings><Option><Menu><Entry> per
    (entry_name, value) pair in menu_entries -- enough for
    joblib._phoronix_menu_entries() to parse."""
    import xml.etree.ElementTree as ET
    root = ET.Element("PhoronixTestSuite")
    settings = ET.SubElement(ET.SubElement(root, "TestSettings"), "Option")
    menu = ET.SubElement(settings, "Menu")
    for entry_name, value in menu_entries:
        entry = ET.SubElement(menu, "Entry")
        ET.SubElement(entry, "Name").text = entry_name
        ET.SubElement(entry, "Value").text = value
    out_dir = os.path.join(user_data_dir, "test-profiles", namespace, name)
    os.makedirs(out_dir, exist_ok=True)
    ET.ElementTree(root).write(os.path.join(out_dir, "test-definition.xml"))


class RepinPhoronixTestPointTest(unittest.TestCase):
    def test_rewrites_suite_xml_and_source_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/build-linux-kernel-1.17.1", "arguments": "defconfig"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml", installed=False)

            # Isolated user_data_dir with no test-profiles/ at all -- nothing
            # to validate Arguments against, so "arguments_status" is omitted.
            result = joblib.repin_phoronix_test_point(info["dir"], "1.18.0", user_data_dir=tmpdir)
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
            result = joblib.repin_phoronix_test_point(info["dir"], "1.0.50", user_data_dir=tmpdir)
            self.assertEqual(result["new_test_id"], "system/selenium-1.0.50")

    def test_repins_across_namespaces_when_installed_in_different_namespace(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            pts_base = os.path.join(tmpdir, "installed-tests", "pts")
            os.makedirs(os.path.join(pts_base, "compress-zstd-1.6.0"))
            point = {"test_id": "system/compress-zstd-1.5.0", "arguments": "-b3"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            result = joblib.repin_phoronix_test_point(info["dir"], "1.6.0", user_data_dir=tmpdir)
            self.assertEqual(result["new_test_id"], "pts/compress-zstd-1.6.0")

    def test_missing_suite_definition_raises(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            with self.assertRaises(FileNotFoundError):
                joblib.repin_phoronix_test_point(tmpdir, "1.0.0")

    def test_arguments_still_valid_are_verified_not_rewritten(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/openfoam-1.2.0", "arguments": "incompressibleFluid/drivaerFastback/ -m S",
                     "description": "Input: drivaerFastback, Small Mesh Size - Mesh Time"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            _write_phoronix_test_definition(tmpdir, "pts", "openfoam-1.3.0", [
                ("drivaerFastback, Small Mesh Size", "incompressibleFluid/drivaerFastback/ -m S"),
            ])

            result = joblib.repin_phoronix_test_point(info["dir"], "1.3.0", user_data_dir=tmpdir)
            self.assertEqual(result["arguments_status"], "verified")
            self.assertNotIn("new_arguments", result)

            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            self.assertEqual(root.find("Execute/Arguments").text, "incompressibleFluid/drivaerFastback/ -m S")

    def test_stale_arguments_rewritten_via_description_match(self):
        # Reproduces the real 2026-08-15 OpenFOAM 1.2.0 -> 1.3.0 case: the
        # drivaerFastback tutorial path was renamed out from under a
        # suite-definition.xml that only had its <Test> version bumped.
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/openfoam-1.2.0", "arguments": "incompressible/simpleFoam/drivaerFastback/ -m S",
                     "description": "Input: drivaerFastback, Small Mesh Size - Mesh Time"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            _write_phoronix_test_definition(tmpdir, "pts", "openfoam-1.3.0", [
                ("motorBike", "incompressibleFluid/motorBike/"),
                ("drivaerFastback, Small Mesh Size", "incompressibleFluid/drivaerFastback/ -m S"),
            ])

            result = joblib.repin_phoronix_test_point(info["dir"], "1.3.0", user_data_dir=tmpdir)
            self.assertEqual(result["arguments_status"], "updated")
            self.assertEqual(result["old_arguments"], "incompressible/simpleFoam/drivaerFastback/ -m S")
            self.assertEqual(result["new_arguments"], "incompressibleFluid/drivaerFastback/ -m S")

            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            self.assertEqual(root.find("Execute/Arguments").text, "incompressibleFluid/drivaerFastback/ -m S")

            with open(os.path.join(info["dir"], "source.json")) as f:
                source = json.load(f)
            self.assertEqual(source["previous_arguments"], "incompressible/simpleFoam/drivaerFastback/ -m S")
            self.assertEqual(source["arguments"], "incompressibleFluid/drivaerFastback/ -m S")

    def test_stale_arguments_with_no_description_match_left_untouched_and_flagged(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point = {"test_id": "pts/openfoam-1.2.0", "arguments": "incompressible/simpleFoam/drivaerFastback/ -m S",
                     "description": "Input: some other option - Mesh Time"}
            info = joblib.materialize_phoronix_test_point(point, tmpdir, "file", "/tmp/src.xml")
            _write_phoronix_test_definition(tmpdir, "pts", "openfoam-1.3.0", [
                ("drivaerFastback, Small Mesh Size", "incompressibleFluid/drivaerFastback/ -m S"),
            ])

            result = joblib.repin_phoronix_test_point(info["dir"], "1.3.0", user_data_dir=tmpdir)
            self.assertEqual(result["arguments_status"], "stale")
            self.assertNotIn("new_arguments", result)

            import xml.etree.ElementTree as ET
            root = ET.parse(os.path.join(info["dir"], "suite-definition.xml")).getroot()
            # Left as-is -- still broken, but not silently guessed at.
            self.assertEqual(root.find("Execute/Arguments").text, "incompressible/simpleFoam/drivaerFastback/ -m S")

            with open(os.path.join(info["dir"], "source.json")) as f:
                source = json.load(f)
            self.assertNotIn("previous_arguments", source)


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


class Cpu2026BenchmarksCatalogTest(unittest.TestCase):
    """CPU2026_BENCHMARKS is a hand-maintained static catalog (display/grouping only, sourced from
    spec.org's own overview page) -- these are regression guards against the specific gap found live
    2026-08-08: 749.fotonik3d_r was missing entirely (its only sibling, 849.fotonik3d_s, was present),
    so scripts/publish_reference_matrix.py's row_group_for_test() silently fell through to "other"
    for every real cpu2026 reference-matrix row for that benchmark."""

    def test_fotonik3d_r_present_as_fprate(self):
        self.assertEqual(joblib.CPU2026_BENCHMARKS.get("749.fotonik3d_r"),
                          {"suite": "fprate", "lang": "Fortran"})

    def test_every_entry_has_a_recognized_suite_value(self):
        valid = {"intrate", "intspeed", "fprate", "fpspeed"}
        for bench, info in joblib.CPU2026_BENCHMARKS.items():
            self.assertIn(info["suite"], valid, bench)


class Cpu2026SuiteInstalledTest(unittest.TestCase):
    def test_true_when_shrc_present(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            with open(os.path.join(tmpdir, "shrc"), "w") as f:
                f.write("")
            self.assertTrue(joblib.cpu2026_suite_installed(tmpdir))

    def test_false_when_shrc_absent(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertFalse(joblib.cpu2026_suite_installed(tmpdir))


class DiscoverInstalledCpu2026BenchmarksTest(unittest.TestCase):
    def test_lists_numbered_benchmark_dirs_sorted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = os.path.join(tmpdir, "benchspec", "CPU")
            for name in ["721.gcc_r", "706.stockfish_r", "Docs"]:
                os.makedirs(os.path.join(root, name))
            # a stray file (not a directory) alongside the benchmark dirs
            # must not be mistaken for one
            with open(os.path.join(root, "710.omnetpp_r"), "w") as f:
                f.write("not a directory")
            self.assertEqual(joblib.discover_installed_cpu2026_benchmarks(tmpdir),
                              ["706.stockfish_r", "721.gcc_r"])

    def test_missing_benchspec_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(joblib.discover_installed_cpu2026_benchmarks(tmpdir), [])


class DiscoverCpu2026ConfigsTest(unittest.TestCase):
    def test_lists_cfg_tags_sorted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = os.path.join(tmpdir, "config")
            os.makedirs(root)
            for name in ["gcc_O2.cfg", "aocc_O3.cfg", "notes.txt"]:
                with open(os.path.join(root, name), "w") as f:
                    f.write("")
            self.assertEqual(joblib.discover_cpu2026_configs(tmpdir), ["aocc_O3", "gcc_O2"])

    def test_missing_config_dir_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(joblib.discover_cpu2026_configs(tmpdir), [])


class Cpu2026BenchmarkBuiltTest(unittest.TestCase):
    def test_true_when_exe_names_contain_tag_and_tune(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            exe_dir = os.path.join(tmpdir, "benchspec", "CPU", "706.stockfish_r", "exe")
            os.makedirs(exe_dir)
            with open(os.path.join(exe_dir, "stockfish_r_base.gcc_O2-m64"), "w") as f:
                f.write("")
            self.assertTrue(joblib.cpu2026_benchmark_built(tmpdir, "706.stockfish_r", "gcc_O2"))
            self.assertFalse(joblib.cpu2026_benchmark_built(tmpdir, "706.stockfish_r", "aocc_O3"))

    def test_false_when_exe_dir_missing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertFalse(joblib.cpu2026_benchmark_built(tmpdir, "706.stockfish_r", "gcc_O2"))

    def test_base_build_does_not_count_as_peak_built(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            exe_dir = os.path.join(tmpdir, "benchspec", "CPU", "706.stockfish_r", "exe")
            os.makedirs(exe_dir)
            with open(os.path.join(exe_dir, "stockfish_r_base.gcc_O2-m64"), "w") as f:
                f.write("")
            self.assertTrue(joblib.cpu2026_benchmark_built(tmpdir, "706.stockfish_r", "gcc_O2", tune="base"))
            self.assertFalse(joblib.cpu2026_benchmark_built(tmpdir, "706.stockfish_r", "gcc_O2", tune="peak"))


class RegisterCpu2026PointTest(unittest.TestCase):
    def test_creates_source_json_and_readme(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            result = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2",
                                                     hostname="host-a")
            self.assertEqual(result["status"], "created")
            self.assertEqual(result["identity"], "706.stockfish_r-gcc_O2-base")
            source_path = os.path.join(dest, "706.stockfish_r", "gcc_O2", "base", "source.json")
            with open(source_path) as f:
                source = json.load(f)
            self.assertEqual(source["schema_version"], 2)
            self.assertEqual(source["config_file"], "gcc_O2.cfg")
            self.assertEqual(source["hosts"]["host-a"]["specdir"], "/opt/cpu2026")
            self.assertEqual(source["tune"], "base")
            readme_path = os.path.join(dest, "706.stockfish_r", "README.md")
            self.assertTrue(os.path.isfile(readme_path))
            with open(readme_path) as f:
                readme = f.read()
            self.assertIn("intrate", readme)

    def test_unknown_benchmark_readme_falls_back_gracefully(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            joblib.register_cpu2026_point(dest, "/opt/cpu2026", "999.unknown_r", "gcc_O2")
            with open(os.path.join(dest, "999.unknown_r", "README.md")) as f:
                readme = f.read()
            self.assertIn("not in the built-in catalog", readme)

    def test_idempotent_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", hostname="host-a")
            second = joblib.register_cpu2026_point(dest, "/other/specdir", "706.stockfish_r", "gcc_O2",
                                                     hostname="host-a")
            self.assertEqual(second["status"], "exists")
            source_path = os.path.join(dest, "706.stockfish_r", "gcc_O2", "base", "source.json")
            with open(source_path) as f:
                source = json.load(f)
            # untouched by the second call -- same host re-registering doesn't repoint its specdir
            self.assertEqual(source["hosts"]["host-a"]["specdir"], "/opt/cpu2026")

    def test_second_host_adds_without_touching_first(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", hostname="host-a")
            second = joblib.register_cpu2026_point(dest, "/other/specdir", "706.stockfish_r", "gcc_O2",
                                                     hostname="host-b")
            self.assertEqual(second["status"], "host_added")
            source_path = os.path.join(dest, "706.stockfish_r", "gcc_O2", "base", "source.json")
            with open(source_path) as f:
                source = json.load(f)
            self.assertEqual(source["hosts"]["host-a"]["specdir"], "/opt/cpu2026")
            self.assertEqual(source["hosts"]["host-b"]["specdir"], "/other/specdir")

    def test_built_status_uses_only_this_hosts_specdir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            specdir_a = os.path.join(tmpdir, "cpu2026-a")
            exe_dir = os.path.join(specdir_a, "benchspec", "CPU", "706.stockfish_r", "exe")
            os.makedirs(exe_dir)
            with open(os.path.join(exe_dir, "stockfish_r_base.gcc_O2-m64"), "w") as f:
                f.write("")
            dest = os.path.join(tmpdir, "dest")
            joblib.register_cpu2026_point(dest, specdir_a, "706.stockfish_r", "gcc_O2", hostname="host-a")
            joblib.register_cpu2026_point(dest, "/does/not/exist", "706.stockfish_r", "gcc_O2", hostname="host-b")

            points_a = joblib.list_materialized_cpu2026_points(dest, hostname="host-a")
            self.assertTrue(points_a[0]["built"])
            self.assertEqual(points_a[0]["specdir"], specdir_a)

            points_b = joblib.list_materialized_cpu2026_points(dest, hostname="host-b")
            self.assertFalse(points_b[0]["built"])

            points_c = joblib.list_materialized_cpu2026_points(dest, hostname="host-c")
            self.assertFalse(points_c[0]["built"])
            self.assertEqual(points_c[0]["specdir"], "")

    def test_base_and_peak_of_same_tag_do_not_collide(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            base = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", tune="base")
            peak = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", tune="peak")
            self.assertEqual(base["status"], "created")
            self.assertEqual(peak["status"], "created")
            self.assertNotEqual(base["dir"], peak["dir"])
            self.assertNotEqual(base["identity"], peak["identity"])


class ResolveCpu2026PointDirTest(unittest.TestCase):
    def test_accepts_real_registered_dir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            self.assertEqual(joblib.resolve_cpu2026_point_dir(dest, info["dir"]), info["dir"])

    def test_rejects_path_outside_dest_root(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            os.makedirs(dest)
            outside = os.path.join(tmpdir, "elsewhere")
            os.makedirs(outside)
            self.assertIsNone(joblib.resolve_cpu2026_point_dir(dest, outside))

    def test_rejects_dir_without_source_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            empty_dir = os.path.join(dest, "706.stockfish_r", "gcc_O2")
            os.makedirs(empty_dir)
            self.assertIsNone(joblib.resolve_cpu2026_point_dir(dest, empty_dir))

    def test_rejects_empty_input(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertIsNone(joblib.resolve_cpu2026_point_dir(tmpdir, ""))
            self.assertIsNone(joblib.resolve_cpu2026_point_dir(tmpdir, None))


class UnregisterCpu2026PointTest(unittest.TestCase):
    def test_removes_point_dir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            self.assertTrue(joblib.unregister_cpu2026_point(dest, info["dir"]))
            self.assertFalse(os.path.exists(info["dir"]))

    def test_cleans_up_empty_tag_and_bench_dirs(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            joblib.unregister_cpu2026_point(dest, info["dir"])
            self.assertFalse(os.path.exists(os.path.join(dest, "706.stockfish_r", "gcc_O2")))
            self.assertFalse(os.path.exists(os.path.join(dest, "706.stockfish_r")))

    def test_keeps_bench_dir_when_other_tags_remain(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            gcc = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            aocc = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "aocc_O3")
            joblib.unregister_cpu2026_point(dest, gcc["dir"])
            self.assertFalse(os.path.exists(gcc["dir"]))
            self.assertTrue(os.path.isfile(os.path.join(aocc["dir"], "source.json")))
            self.assertTrue(os.path.isfile(os.path.join(dest, "706.stockfish_r", "README.md")))

    def test_keeps_tag_dir_when_other_tune_remains(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            base = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", tune="base")
            peak = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2", tune="peak")
            joblib.unregister_cpu2026_point(dest, peak["dir"])
            self.assertFalse(os.path.exists(peak["dir"]))
            self.assertTrue(os.path.isfile(os.path.join(base["dir"], "source.json")))

    def test_does_not_touch_run_data_only_the_symlink(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            rundir = os.path.join(tmpdir, "runs", "cpu2026", info["identity"], "run1")
            os.makedirs(rundir)
            joblib.link_cpu2026_point_run(info["dir"], "run1", rundir)
            joblib.unregister_cpu2026_point(dest, info["dir"])
            self.assertFalse(os.path.exists(info["dir"]))
            self.assertTrue(os.path.isdir(rundir))

    def test_rejects_path_outside_dest_root(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            os.makedirs(dest)
            outside = os.path.join(tmpdir, "elsewhere")
            os.makedirs(outside)
            with open(os.path.join(outside, "source.json"), "w") as f:
                f.write("{}")
            self.assertFalse(joblib.unregister_cpu2026_point(dest, outside))
            self.assertTrue(os.path.exists(outside))

    def test_rejects_dir_without_source_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            empty_dir = os.path.join(dest, "706.stockfish_r", "gcc_O2", "base")
            os.makedirs(empty_dir)
            self.assertFalse(joblib.unregister_cpu2026_point(dest, empty_dir))
            self.assertTrue(os.path.exists(empty_dir))


class ListMaterializedCpu2026PointsTest(unittest.TestCase):
    def test_lists_registered_points_with_built_status_and_runs(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            specdir = os.path.join(tmpdir, "cpu2026")
            exe_dir = os.path.join(specdir, "benchspec", "CPU", "706.stockfish_r", "exe")
            os.makedirs(exe_dir)
            with open(os.path.join(exe_dir, "stockfish_r_base.gcc_O2-m64"), "w") as f:
                f.write("")
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, specdir, "706.stockfish_r", "gcc_O2")
            rundir = os.path.join(tmpdir, "runs", "cpu2026", info["identity"], "run1")
            os.makedirs(rundir)
            joblib.link_cpu2026_point_run(info["dir"], "run1", rundir)

            points = joblib.list_materialized_cpu2026_points(dest)
            self.assertEqual(len(points), 1)
            self.assertEqual(points[0]["identity"], info["identity"])
            self.assertTrue(points[0]["built"])
            self.assertEqual(points[0]["runs"], [
                {"run_id": "run1", "suite": "cpu2026", "benchmark": info["identity"]},
            ])

    def test_not_built_when_no_matching_exe(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            specdir = os.path.join(tmpdir, "cpu2026")
            dest = os.path.join(tmpdir, "dest")
            joblib.register_cpu2026_point(dest, specdir, "706.stockfish_r", "gcc_O2")
            points = joblib.list_materialized_cpu2026_points(dest)
            self.assertFalse(points[0]["built"])

    def test_empty_dest_returns_empty_list(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(joblib.list_materialized_cpu2026_points(os.path.join(tmpdir, "nope")), [])

    def test_dir_without_source_json_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            os.makedirs(os.path.join(dest, "706.stockfish_r", "gcc_O2", "base"))
            self.assertEqual(joblib.list_materialized_cpu2026_points(dest), [])

    def test_dangling_run_symlink_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")
            os.makedirs(os.path.join(info["dir"], "runs"))
            os.symlink(os.path.join(tmpdir, "does-not-exist"),
                       os.path.join(info["dir"], "runs", "run1"))
            points = joblib.list_materialized_cpu2026_points(dest)
            self.assertEqual(points[0]["runs"], [])


class FindMaterializedCpu2026PointTest(unittest.TestCase):
    def test_finds_entry_by_identity(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            info = joblib.register_cpu2026_point(dest, "/opt/cpu2026", "706.stockfish_r", "gcc_O2")

            entry = joblib.find_materialized_cpu2026_point(dest, info["identity"])
            self.assertIsNotNone(entry)
            self.assertEqual(entry["identity"], info["identity"])
            self.assertEqual(entry["bench"], "706.stockfish_r")
            self.assertEqual(entry["tag"], "gcc_O2")
            self.assertEqual(entry["tune"], "base")

    def test_returns_none_when_not_found(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = os.path.join(tmpdir, "dest")
            self.assertIsNone(
                joblib.find_materialized_cpu2026_point(dest, "706.stockfish_r-gcc_O2-base"))


class GroupMaterializedCpu2026PointsByBenchTest(unittest.TestCase):
    def test_groups_and_sorts_by_bench(self):
        points = [
            {"bench": "721.gcc_r", "tag": "gcc_O2", "tune": "base", "built": True, "runs": []},
            {"bench": "706.stockfish_r", "tag": "aocc_O3", "tune": "base", "built": False, "runs": []},
            {"bench": "706.stockfish_r", "tag": "gcc_O2", "tune": "base", "built": True, "runs": []},
        ]
        groups = joblib.group_materialized_cpu2026_points_by_bench(points)
        self.assertEqual([g["bench"] for g in groups], ["706.stockfish_r", "721.gcc_r"])
        stockfish = groups[0]
        self.assertEqual(stockfish["total_count"], 2)
        self.assertEqual(stockfish["built_count"], 1)
        self.assertEqual([p["tag"] for p in stockfish["points"]], ["aocc_O3", "gcc_O2"])
        self.assertEqual(stockfish["run_status"], "none")

    def test_empty_list_yields_empty_groups(self):
        self.assertEqual(joblib.group_materialized_cpu2026_points_by_bench([]), [])

    def test_run_status_all_when_every_point_has_runs(self):
        points = [{"bench": "706.stockfish_r", "tag": "a", "tune": "base", "built": True, "runs": [{"run_id": "r1"}]},
                  {"bench": "706.stockfish_r", "tag": "b", "tune": "base", "built": True, "runs": [{"run_id": "r2"}]}]
        self.assertEqual(joblib.group_materialized_cpu2026_points_by_bench(points)[0]["run_status"], "all")

    def test_run_status_some_when_only_some_points_have_runs(self):
        points = [{"bench": "706.stockfish_r", "tag": "a", "tune": "base", "built": True, "runs": [{"run_id": "r1"}]},
                  {"bench": "706.stockfish_r", "tag": "b", "tune": "base", "built": False, "runs": []}]
        self.assertEqual(joblib.group_materialized_cpu2026_points_by_bench(points)[0]["run_status"], "some")

    def test_base_and_peak_of_same_tag_sort_together(self):
        points = [
            {"bench": "706.stockfish_r", "tag": "gcc_O2", "tune": "peak", "built": True, "runs": []},
            {"bench": "706.stockfish_r", "tag": "gcc_O2", "tune": "base", "built": True, "runs": []},
        ]
        groups = joblib.group_materialized_cpu2026_points_by_bench(points)
        self.assertEqual([p["tune"] for p in groups[0]["points"]], ["base", "peak"])


class LinkCpu2026PointRunTest(unittest.TestCase):
    def test_creates_symlink_to_rundir(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            point_dir = os.path.join(tmpdir, "point")
            os.makedirs(point_dir)
            rundir = os.path.join(tmpdir, "output", "cpu2026", "x", "run1")
            os.makedirs(rundir)
            ok = joblib.link_cpu2026_point_run(point_dir, "run1", rundir)
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
            joblib.link_cpu2026_point_run(point_dir, "runid", rundir1)
            joblib.link_cpu2026_point_run(point_dir, "runid", rundir2)
            link_path = os.path.join(point_dir, "runs", "runid")
            self.assertEqual(os.path.realpath(link_path), os.path.realpath(rundir2))

    def test_degrades_to_false_on_unwritable_path(self):
        ok = joblib.link_cpu2026_point_run("/proc/1/this-should-not-be-writable", "run1", "/tmp")
        self.assertFalse(ok)


class BuildCpu2026ArgvTest(unittest.TestCase):
    def test_shell_argv_sources_shrc_and_cds_into_specdir(self):
        argv = joblib.build_cpu2026_shell_argv("/opt/cpu2026", "echo hi")
        self.assertEqual(argv[0], "bash")
        self.assertEqual(argv[1], "-c")
        self.assertIn("source /opt/cpu2026/shrc", argv[2])
        self.assertIn("cd /opt/cpu2026", argv[2])
        self.assertIn("ulimit -s unlimited", argv[2])
        self.assertIn("echo hi", argv[2])
        # cd MUST precede source: shrc discovers its own SPEC root by
        # walking up from $PWD (not from its own script path), so sourcing
        # it before cd-ing into specdir makes it search upward from
        # whatever directory the caller's subprocess happened to start in.
        self.assertLess(argv[2].index("cd /opt/cpu2026"), argv[2].index("source /opt/cpu2026/shrc"))

    def test_build_argv_uses_build_action(self):
        argv = joblib.build_cpu2026_build_argv("/opt/cpu2026", "gcc_O2.cfg", "706.stockfish_r")
        self.assertIn("runcpu --config gcc_O2.cfg --action=build --tune base 706.stockfish_r", argv[2])

    def test_run_workload_uses_validate_action_and_nobuild(self):
        workload = joblib.build_cpu2026_run_workload("/opt/cpu2026", "gcc_O2.cfg", "706.stockfish_r")
        self.assertIn("--action=validate", workload)
        self.assertIn("--nobuild", workload)
        self.assertIn("--iterations 1", workload)
        # round-trips through shlex.split() back to the 3-token bash -c argv
        import shlex
        reparsed = shlex.split(workload)
        self.assertEqual(reparsed[0], "bash")
        self.assertEqual(reparsed[1], "-c")
        self.assertIn("runcpu --config gcc_O2.cfg --action=validate", reparsed[2])


class CsvHasTimeColumnTest(unittest.TestCase):
    def test_true_when_time_column_present(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "interval.csv")
            with open(path, "w") as f:
                f.write("time,ipc,\n1,1.5,\n")
            self.assertTrue(joblib.csv_has_time_column(path))

    def test_false_when_no_time_column(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "aggregate.csv")
            with open(path, "w") as f:
                f.write("ipc,retire,\n1.5,20.0,\n")
            self.assertFalse(joblib.csv_has_time_column(path))

    def test_false_for_empty_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "empty.csv")
            open(path, "w").close()
            self.assertFalse(joblib.csv_has_time_column(path))

    def test_false_for_missing_file(self):
        self.assertFalse(joblib.csv_has_time_column("/does/not/exist.csv"))


class ParseIntervalCsvTest(unittest.TestCase):
    def test_parses_columns_and_drops_trailing_empty_header(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "amdtopdown.csv")
            with open(path, "w") as f:
                # trailing comma before the newline -- every wspy CSV row is
                # comma-terminated (CLAUDE.md's own noted pitfall), which
                # produces one trailing empty header field to ignore.
                f.write("time,retire,frontend,\n1,20.0,10.0,\n2,25.0,12.0,\n")
            result = joblib.parse_interval_csv(path)
            self.assertEqual(result["columns"], ["time", "retire", "frontend"])
            self.assertEqual(result["dimension_columns"], ["time"])
            self.assertEqual(result["gpu_columns"], [])
            self.assertEqual(result["series"]["time"], [1.0, 2.0])
            self.assertEqual(result["series"]["retire"], [20.0, 25.0])
            self.assertEqual(result["row_count"], 2)
            self.assertFalse(result["decimated"])

    def test_detects_phase_and_gpu_columns(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "interval.csv")
            with open(path, "w") as f:
                f.write("time,phase,gpu_activity\n1,warmup,10.0\n2,steady,20.0\n")
            result = joblib.parse_interval_csv(path)
            self.assertEqual(result["dimension_columns"], ["time", "phase"])
            self.assertEqual(result["gpu_columns"], ["gpu_activity"])
            # phase stays a raw string series, not cast to float
            self.assertEqual(result["series"]["phase"], ["warmup", "steady"])

    def test_malformed_or_empty_cell_becomes_none(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "interval.csv")
            with open(path, "w") as f:
                f.write("time,ipc\n1,abc\n2,\n3,1.5\n")
            result = joblib.parse_interval_csv(path)
            self.assertEqual(result["series"]["ipc"], [None, None, 1.5])

    def test_decimates_above_max_rows_but_keeps_endpoints_representative(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "interval.csv")
            with open(path, "w") as f:
                f.write("time,ipc\n")
                for i in range(1000):
                    f.write(f"{i},{i}\n")
            result = joblib.parse_interval_csv(path, max_rows=100)
            self.assertEqual(result["row_count"], 1000)
            self.assertTrue(result["decimated"])
            self.assertLessEqual(len(result["series"]["time"]), 100)
            self.assertGreater(len(result["series"]["time"]), 0)

    def test_not_decimated_when_under_cap(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "interval.csv")
            with open(path, "w") as f:
                f.write("time,ipc\n1,1.0\n2,2.0\n")
            result = joblib.parse_interval_csv(path, max_rows=100)
            self.assertFalse(result["decimated"])
            self.assertEqual(len(result["series"]["time"]), 2)


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

    def test_command_txt_labeled(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest = {
                "layout_version": "1.0.0", "suite": "s", "benchmark": "b", "run_id": "r",
                "command": ["true"], "passes": [],
            }
            with open(os.path.join(tmpdir, "manifest.json"), "w") as f:
                json.dump(manifest, f)
            open(os.path.join(tmpdir, joblib.COMMAND_TXT_NAME), "w").close()
            items = joblib.collect_run_files(tmpdir)
            by_name = {i["filename"]: i for i in items}
            self.assertEqual(by_name[joblib.COMMAND_TXT_NAME]["label"], "command line")

    def test_archetype_badge_absent_until_generated(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            items = joblib.collect_run_files(tmpdir)
            self.assertNotIn(joblib.ARCHETYPE_BADGE_NAME, [i["filename"] for i in items])

    def test_archetype_badge_offered_once_present(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            open(os.path.join(tmpdir, joblib.ARCHETYPE_BADGE_NAME), "w").close()
            items = joblib.collect_run_files(tmpdir)
            by_name = {i["filename"]: i for i in items}
            self.assertIn(joblib.ARCHETYPE_BADGE_NAME, by_name)
            self.assertEqual(by_name[joblib.ARCHETYPE_BADGE_NAME]["kind"], "markdown")
            self.assertFalse(by_name[joblib.ARCHETYPE_BADGE_NAME]["ai_generated"])

    def test_archetype_similar_absent_until_generated(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            items = joblib.collect_run_files(tmpdir)
            self.assertNotIn(joblib.ARCHETYPE_SIMILAR_NAME, [i["filename"] for i in items])

    def test_archetype_similar_offered_once_present(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            open(os.path.join(tmpdir, joblib.ARCHETYPE_SIMILAR_NAME), "w").close()
            items = joblib.collect_run_files(tmpdir)
            by_name = {i["filename"]: i for i in items}
            self.assertIn(joblib.ARCHETYPE_SIMILAR_NAME, by_name)
            self.assertEqual(by_name[joblib.ARCHETYPE_SIMILAR_NAME]["kind"], "markdown")
            self.assertFalse(by_name[joblib.ARCHETYPE_SIMILAR_NAME]["ai_generated"])


class ResolveStorePassRowsTest(unittest.TestCase):
    """resolve_store_pass_rows()/pick_counters_pass_id() -- shared by wspy-testpoint (aggregate/render's
    --run-id resolution) and server.py's characterization-badge generator. A minimal `runs` table (just
    the columns these two functions touch) rather than store.c's full schema, matching this test file's
    "test the function under test, not the whole subsystem" convention elsewhere."""

    def _connect(self):
        conn = sqlite3.connect(":memory:")
        conn.execute("CREATE TABLE runs (hostname TEXT, run_id TEXT, config_name TEXT, "
                     "manifest_path TEXT, output_path TEXT)")
        self.addCleanup(conn.close)
        return conn

    def test_resolves_by_directory_path_correlation(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-a', 'counters', "
                     "'/out/cpu2026/707.ntest_r-gcc_O3-base/wraprun1/counters.manifest.json', NULL)")
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-b', 'systemtime', NULL, "
                     "'/out/cpu2026/707.ntest_r-gcc_O3-base/wraprun1/systemtime.csv')")
        rows = joblib.resolve_store_pass_rows(
            conn.cursor(), "roswell", "cpu2026", "707.ntest_r-gcc_O3-base", "wraprun1")
        self.assertEqual(sorted(rows), [("pass-a", "counters"), ("pass-b", "systemtime")])

    def test_underscore_in_benchmark_name_does_not_over_match_via_like_wildcard(self):
        # "707.ntest_r-gcc_O3-base" contains underscores -- unescaped these are LIKE single-char
        # wildcards and would also match "707.ntestXr-gccXO3-base"-shaped paths.
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'wrong-match', 'counters', "
                     "'/out/cpu2026/707XntestXr-gccXO3-base/wraprun1/counters.manifest.json', NULL)")
        rows = joblib.resolve_store_pass_rows(
            conn.cursor(), "roswell", "cpu2026", "707.ntest_r-gcc_O3-base", "wraprun1")
        self.assertEqual(rows, [])

    def test_exact_hostname_run_id_match_for_bare_wspy_runs(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('archhost', 'archrun1', 'counters', NULL, NULL)")
        rows = joblib.resolve_store_pass_rows(
            conn.cursor(), "archhost", "manual", "archbench", "archrun1")
        self.assertEqual(rows, [("archrun1", "counters")])

    def test_no_match_returns_empty_list(self):
        conn = self._connect()
        rows = joblib.resolve_store_pass_rows(conn.cursor(), "roswell", "cpu2026", "bench", "run1")
        self.assertEqual(rows, [])

    def test_pick_counters_pass_id_prefers_counters(self):
        rows = [("pass-b", "systemtime"), ("pass-a", "counters"), ("pass-c", "ibs")]
        self.assertEqual(joblib.pick_counters_pass_id(rows), "pass-a")

    def test_pick_counters_pass_id_falls_back_to_first_when_no_counters_pass(self):
        rows = [("pass-b", "systemtime"), ("pass-c", "ibs")]
        self.assertEqual(joblib.pick_counters_pass_id(rows), "pass-b")


class ResolveStoreRunDirectoryTest(unittest.TestCase):
    """resolve_store_run_directory() -- the reverse of resolve_store_pass_rows() above, used by
    server.py's similarity-panel generator to turn one of wspy-archetype --nearest's bare
    (hostname, run_id) neighbor identities back into a linkable (suite, benchmark, run_id) report
    path. Same minimal `runs` table convention as ResolveStorePassRowsTest."""

    def _connect(self):
        conn = sqlite3.connect(":memory:")
        conn.execute("CREATE TABLE runs (hostname TEXT, run_id TEXT, config_name TEXT, "
                     "manifest_path TEXT, output_path TEXT)")
        self.addCleanup(conn.close)
        return conn

    def test_resolves_from_output_path(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-a', 'topdown', NULL, "
                     "'/out/phoronix/coremark-default/wraprun1/amdtopdown.csv')")
        result = joblib.resolve_store_run_directory(conn.cursor(), "roswell", "pass-a")
        self.assertEqual(result, ("phoronix", "coremark-default", "wraprun1"))

    def test_resolves_from_manifest_path_when_output_path_is_null(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-a', 'counters', "
                     "'/out/cpu2026/706.stockfish_r-gcc_O3-base/wraprun1/counters.manifest.json', NULL)")
        result = joblib.resolve_store_run_directory(conn.cursor(), "roswell", "pass-a")
        self.assertEqual(result, ("cpu2026", "706.stockfish_r-gcc_O3-base", "wraprun1"))

    def test_no_matching_row_returns_none(self):
        conn = self._connect()
        result = joblib.resolve_store_run_directory(conn.cursor(), "roswell", "nope")
        self.assertIsNone(result)

    def test_both_paths_null_returns_none(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-a', 'counters', NULL, NULL)")
        result = joblib.resolve_store_run_directory(conn.cursor(), "roswell", "pass-a")
        self.assertIsNone(result)

    def test_path_too_shallow_to_hold_suite_benchmark_run_id_returns_none(self):
        conn = self._connect()
        conn.execute("INSERT INTO runs VALUES ('roswell', 'pass-a', 'counters', NULL, "
                     "'/counters.csv')")
        result = joblib.resolve_store_run_directory(conn.cursor(), "roswell", "pass-a")
        self.assertIsNone(result)


class ClassifyBundleKindTest(unittest.TestCase):
    def test_manifest_kinds(self):
        self.assertEqual(joblib.classify_bundle_kind("manifest.json"), "manifest")
        self.assertEqual(joblib.classify_bundle_kind("quick.manifest.json"), "manifest")
        self.assertEqual(joblib.classify_bundle_kind(joblib.MANIFEST_NAME), "manifest")
        self.assertEqual(joblib.classify_bundle_kind(joblib.COMMAND_TXT_NAME), "manifest")

    def test_derived_kinds(self):
        self.assertEqual(joblib.classify_bundle_kind("summary.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("curation.json"), "derived")
        self.assertEqual(joblib.classify_bundle_kind(joblib.PNG_NAME), "derived")
        self.assertEqual(joblib.classify_bundle_kind("plots/foo.png"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("process.tree.summary.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("process.tree.top.txt"), "derived")
        self.assertEqual(joblib.classify_bundle_kind("process.tree.top1pct.txt"), "derived")
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
