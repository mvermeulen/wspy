#!/usr/bin/env python3
"""
web/test_incomplete_run.py -- unit tests for server.py's item 6 Phase A pieces
(INVESTIGATION.md 4.4(a) "Detect and resume interrupted wspy-run profiles"):
detect_incomplete_wspy_run(), its find_representative_host_manifest()/load_run_history_entry()
integration, and render_incomplete_run_report()/render_report()'s dispatch. Not wired into
make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_incomplete_run.py

Fixtures are real temp directories with real per-pass wspy manifest.json files (manifest.c's own
write_config_provenance() shape), not mocks -- this is filesystem-shape detection logic, so the
fixture shape is the thing under test.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


def write_pass_manifest(rundir, name, preset=None, configuration=None, argv=None,
                         exit_code=0, hostname="testhost"):
    """A minimal but real-shaped wspy per-pass manifest.json -- see manifest.c's write_manifest()/
    write_config_provenance()."""
    doc = {
        "manifest_schema_version": "1.9.0",
        "command": {"argv": argv or ["true"]},
        "host": {"hostname": hostname, "cpu_vendor": "AMD"},
        "timing": {"start_time": "2026-08-16T02:06:17.000Z", "elapsed_seconds": 1.5},
        "exit_status": {"known": True, "exited": True, "signaled": False, "exit_code": exit_code},
        "configuration_provenance": {"preset": preset, "configuration": configuration, "options": []},
    }
    with open(os.path.join(rundir, f"{name}.manifest.json"), "w") as f:
        json.dump(doc, f)


class RunStatusFromPassesTest(unittest.TestCase):
    """Item 6 Phase B: a resumed run's "skipped" passes (wspy-run --resume) must count the same
    as "ok" towards the overall run status, not read as a failure just because they weren't
    re-executed."""

    def test_all_ok_is_ok(self):
        self.assertEqual(server.run_status_from_passes([{"status": "ok"}, {"status": "ok"}]), "ok")

    def test_mix_of_ok_and_skipped_is_ok(self):
        self.assertEqual(
            server.run_status_from_passes([{"status": "ok"}, {"status": "skipped"}]), "ok")

    def test_all_skipped_is_ok(self):
        self.assertEqual(
            server.run_status_from_passes([{"status": "skipped"}, {"status": "skipped"}]), "ok")

    def test_any_wspy_error_is_failed_even_with_skipped_passes(self):
        self.assertEqual(
            server.run_status_from_passes([{"status": "skipped"}, {"status": "wspy-error"}]),
            "failed")

    def test_empty_is_unknown(self):
        self.assertEqual(server.run_status_from_passes([]), "unknown")


class DetectIncompleteWspyRunTest(unittest.TestCase):
    def test_empty_directory_returns_none(self):
        with tempfile.TemporaryDirectory() as rundir:
            self.assertIsNone(server.detect_incomplete_wspy_run(rundir))

    def test_only_legacy_fixed_name_returns_none(self):
        # Indistinguishable from a genuine item-6 fixed-config report by filename alone --
        # detect_incomplete_wspy_run() deliberately leaves this one case alone.
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "amdtopdown")
            self.assertIsNone(server.detect_incomplete_wspy_run(rundir))

    def test_multiple_manifests_no_top_level_manifest_is_incomplete(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "systemtime", preset="deep-cpu", configuration="system-metrics")
            write_pass_manifest(rundir, "counters", preset="deep-cpu", configuration="performance-counters")
            result = server.detect_incomplete_wspy_run(rundir)
            self.assertIsNotNone(result)
            self.assertEqual(result["completed_passes"], ["counters", "systemtime"])
            self.assertEqual(result["preset"], "deep-cpu")
            self.assertEqual(result["expected_total"], 3)  # systemtime, counters, amdtopdown
            self.assertEqual(result["representative_manifest"], "counters.manifest.json")

    def test_single_non_legacy_named_pass_is_still_incomplete(self):
        # "quick"'s one pass is literally named "run" -- immediately distinguishable from the
        # legacy amdtopdown.manifest.json case even with only one manifest file present.
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "run", preset="quick", configuration="performance-counters")
            result = server.detect_incomplete_wspy_run(rundir)
            self.assertIsNotNone(result)
            self.assertEqual(result["completed_passes"], ["run"])
            self.assertEqual(result["expected_total"], 1)

    def test_no_preset_recorded_leaves_expected_total_none(self):
        # A -c/--config custom pass list run never sets --preset-name.
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "custom1")
            write_pass_manifest(rundir, "custom2")
            result = server.detect_incomplete_wspy_run(rundir)
            self.assertIsNotNone(result)
            self.assertIsNone(result["preset"])
            self.assertIsNone(result["expected_total"])

    def test_top_level_manifest_present_is_not_this_function_s_job(self):
        # detect_incomplete_wspy_run() only looks at *.manifest.json presence -- it doesn't check
        # for RUN_MANIFEST_NAME itself, since render_report()'s own dispatch only calls it after
        # already confirming that file is absent. Still worth pinning: a real top-level
        # manifest.json's basename doesn't end in ".manifest.json" so it's never mistaken for one.
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "counters", preset="quick")
            with open(os.path.join(rundir, "manifest.json"), "w") as f:
                json.dump({"passes": []}, f)
            result = server.detect_incomplete_wspy_run(rundir)
            self.assertIsNotNone(result)  # still flags it -- caller's job to check RUN_MANIFEST_NAME first


class LooksLikeARunDirectoryTest(unittest.TestCase):
    """Regression coverage for the discover_reports()/discover_run_history() directory-discovery
    gate, extended by item 6 Phase A: the fixed TOPLEVEL_MARKER_FILES set alone misses an
    interrupted wspy-run whose only artifacts are its completed passes' own per-process manifests --
    a CLI-launched batch run (the motivating "raised after a real host crash mid-batch" case) never
    gets a web-launcher-only launch.log, and summary.txt/manifest.json are both only ever written
    at the very end, by the crash's definition never reached."""

    def test_empty_directory_is_not_a_run(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(server._looks_like_a_run_directory(d))

    def test_completed_run_with_only_fixed_markers_still_recognized(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "manifest.json"), "w").close()
            self.assertTrue(server._looks_like_a_run_directory(d))

    def test_interrupted_run_with_only_a_per_pass_manifest_is_recognized(self):
        with tempfile.TemporaryDirectory() as d:
            write_pass_manifest(d, "run", preset="quick")
            self.assertTrue(server._looks_like_a_run_directory(d))

    def test_end_to_end_discover_run_history_finds_it(self):
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "20260816T030000")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "systemtime", preset="deep-cpu")
            entries, total = server.discover_run_history(output_root, {})
            self.assertEqual(total, 1)
            self.assertEqual(entries[0]["status"], "incomplete")

    def test_end_to_end_discover_reports_finds_it(self):
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "20260816T030000")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "systemtime", preset="deep-cpu")
            reports = server.discover_reports(output_root, limit=50)
            self.assertEqual(len(reports), 1)
            self.assertEqual(reports[0]["run_id"], "20260816T030000")


class FindRepresentativeHostManifestIncompleteTest(unittest.TestCase):
    def test_falls_back_to_incomplete_runs_representative_manifest(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(rundir, "run", preset="quick", hostname="crashed-host")
            host_manifest = server.find_representative_host_manifest(rundir, None)
            self.assertIsNotNone(host_manifest)
            self.assertEqual(host_manifest["host"]["hostname"], "crashed-host")

    def test_still_none_for_a_directory_with_nothing_at_all(self):
        with tempfile.TemporaryDirectory() as rundir:
            self.assertIsNone(server.find_representative_host_manifest(rundir, None))


class LoadRunHistoryEntryIncompleteTest(unittest.TestCase):
    def test_status_is_incomplete_not_unknown_or_failed(self):
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "20260816T020617")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "run", preset="quick", argv=["xz", "-k", "input.tar"])
            entry = server.load_run_history_entry(output_root, "cpu2026", "801.xz_s",
                                                    "20260816T020617", os.path.getmtime(rundir))
            self.assertEqual(entry["status"], "incomplete")
            self.assertEqual(entry["workload_str"], "xz -k input.tar")
            self.assertEqual(entry["hostname"], "testhost")

    def test_complete_run_status_unaffected(self):
        # Regression guard: a normal, fully-finished run (real top-level manifest.json) must not
        # be reclassified as "incomplete" by this change.
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "20260816T020617")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "run", preset="quick")
            with open(os.path.join(rundir, "manifest.json"), "w") as f:
                json.dump({"command": ["xz"], "passes": [{"name": "run", "status": "ok"}]}, f)
            entry = server.load_run_history_entry(output_root, "cpu2026", "801.xz_s",
                                                    "20260816T020617", os.path.getmtime(rundir))
            self.assertEqual(entry["status"], "ok")


class RenderIncompleteRunReportTest(unittest.TestCase):
    def test_render_report_dispatches_to_incomplete_when_no_top_level_manifest(self):
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "20260816T020617")
            os.makedirs(rundir)
            argv = ["xz", "-k", "-T4", "input1.tar"]
            write_pass_manifest(rundir, "systemtime", preset="deep-cpu", argv=argv)
            write_pass_manifest(rundir, "counters", preset="deep-cpu", argv=argv)
            html = server.render_report(output_root, "cpu2026", "801.xz_s", "20260816T020617")
            self.assertIsNotNone(html)
            self.assertIn("Incomplete run", html)
            self.assertIn("2 of 3 passes ran", html)
            self.assertIn("deep-cpu", html)
            self.assertIn("xz -k -T4 input1.tar", html)
            self.assertIn("counters.manifest.json", html)  # collect_run_files() artifact listing
            self.assertIn("systemtime.manifest.json", html)

    def test_render_report_still_uses_fixed_report_for_the_ambiguous_legacy_case(self):
        # Regression guard for the deliberate "leave it alone" carve-out.
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "legacyrun")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "amdtopdown")
            html = server.render_report(output_root, "cpu2026", "801.xz_s", "legacyrun")
            self.assertIsNotNone(html)
            self.assertNotIn("Incomplete run", html)

    def test_unresolvable_preset_shows_total_unknown(self):
        with tempfile.TemporaryDirectory() as output_root:
            rundir = os.path.join(output_root, "cpu2026", "801.xz_s", "somerun")
            os.makedirs(rundir)
            write_pass_manifest(rundir, "custom1")
            html = server.render_report(output_root, "cpu2026", "801.xz_s", "somerun")
            self.assertIn("expected total unknown", html)


if __name__ == "__main__":
    unittest.main()
