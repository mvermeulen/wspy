#!/usr/bin/env python3
"""
web/test_jobs_view.py -- unit tests for server.py's item 6 pieces (INVESTIGATION.md 4.4(a)
"Job-browsing view in the web UI"): format_job_configuration() (the "bundle in sharing
structured configuration provenance with the job format" half of the item), render_jobs(),
and render_job_detail(). Not wired into make test/run_tests.sh, same "web/ is stdlib-only
Python, not covered by the C toolchain's test targets" convention as the rest of web/test_*.py --
run standalone:

    python3 web/test_jobs_view.py
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joblib
import server


def make_cfg(jobs_dir, output_root):
    return {"jobs_dir": jobs_dir, "output_root": output_root}


class FormatJobConfigurationTest(unittest.TestCase):
    def test_preset_job_is_one_line(self):
        job = joblib.build_job(["sleep", "1"], "manual", "sleep", "preset", profile="deep-cpu")
        self.assertEqual(server.format_job_configuration(job), ["preset=deep-cpu"])

    def test_custom_job_lists_each_enabled_section_in_dispatch_order(self):
        # tree/counters/system/gpu/ibs order, matching build_configuration_passes() itself --
        # given here in a different order (counters before tree) to actually exercise the sort.
        checklist = {
            "counters": {"enabled": True, "groups": ["topdown", "branch"], "interval_secs": "1"},
            "tree": {"enabled": True, "cmdline": True},
        }
        job = joblib.build_job(["coremark.exe"], "manual", "coremark", "custom", checklist=checklist)
        lines = server.format_job_configuration(job)
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("config=process-tree"))
        self.assertTrue(lines[1].startswith("config=performance-counters"))
        self.assertIn("options=cmdline=true", lines[0])
        self.assertIn("options=groups=topdown,branch, interval_secs=1", lines[1])

    def test_custom_job_skips_disabled_sections(self):
        checklist = {"tree": {"enabled": True}, "gpu": {"enabled": False, "busy": True}}
        job = joblib.build_job(["a"], "manual", "b", "custom", checklist=checklist)
        lines = server.format_job_configuration(job)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("config=process-tree"))

    def test_empty_custom_checklist_is_empty(self):
        job = joblib.build_job(["a"], "manual", "b", "custom", checklist={})
        self.assertEqual(server.format_job_configuration(job), [])

    def test_preset_job_with_no_profile_is_empty(self):
        # Shouldn't normally happen (validate_job() would reject it), but format_job_configuration()
        # itself must degrade rather than crash on a hand-edited/malformed job file.
        job = joblib.build_job(["a"], "manual", "b", "preset", profile="quick")
        job["profile"] = None
        self.assertEqual(server.format_job_configuration(job), [])

    def test_reuses_the_same_formatting_as_a_completed_run_s_provenance(self):
        # The actual "bundle in sharing structured configuration provenance with the job format"
        # claim: a job's own checklist section, run through format_job_configuration(), must
        # read identically to format_config_provenance() on the equivalent real
        # configuration_provenance a completed run would have recorded for that same pass.
        checklist = {"tree": {"enabled": True, "cmdline": True, "groups": ["ipc"]}}
        job = joblib.build_job(["a"], "manual", "b", "custom", checklist=checklist)
        job_line = server.format_job_configuration(job)[0]
        cp = {"preset": None, "configuration": "process-tree",
              "options": joblib.config_options(checklist["tree"])}
        self.assertEqual(job_line, server.format_config_provenance(cp))


class RenderJobsTest(unittest.TestCase):
    def _seed(self, jobs_dir):
        joblib.ensure_jobs_dirs(jobs_dir)
        pending = joblib.build_job(["xz", "-k", "input.tar"], "cpu2026", "801.xz_s", "preset",
                                    profile="deep-cpu")
        joblib.save_job(joblib.job_path(jobs_dir, "pending", pending["job_id"]), pending)
        done = joblib.build_job(["sleep", "1"], "manual", "sleeptest", "preset", profile="quick")
        done["result"] = {"status": "done", "run_id": "20260816T020000.000-abcd1234",
                           "rundir": "manual/sleeptest/20260816T020000.000-abcd1234",
                           "finished_at": "2026-08-16T02:00:05.000Z"}
        joblib.save_job(joblib.job_path(jobs_dir, "done", done["job_id"]), done)
        failed = joblib.build_job(["badcmd"], "manual", "badbench", "preset", profile="quick")
        failed["result"] = {"status": "failed", "error": "wspy binary not found",
                             "finished_at": "2026-08-16T02:05:00.000Z"}
        joblib.save_job(joblib.job_path(jobs_dir, "failed", failed["job_id"]), failed)
        return pending, done, failed

    def test_lists_jobs_across_all_states(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            pending, done, failed = self._seed(jobs_dir)
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {})
            self.assertIn(pending["job_id"], html)
            self.assertIn(done["job_id"], html)
            self.assertIn(failed["job_id"], html)

    def test_state_filter_narrows_the_list(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            pending, done, failed = self._seed(jobs_dir)
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {"state": ["done"]})
            self.assertIn(done["job_id"], html)
            self.assertNotIn(pending["job_id"], html)
            self.assertNotIn(failed["job_id"], html)

    def test_done_job_links_to_its_report(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            _, done, _ = self._seed(jobs_dir)
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {})
            self.assertIn('href="/report/manual/sleeptest/20260816T020000.000-abcd1234"', html)

    def test_failed_job_shows_its_error(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            self._seed(jobs_dir)
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {})
            self.assertIn("wspy binary not found", html)

    def test_unreadable_job_file_degrades_gracefully(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            with open(joblib.job_path(jobs_dir, "pending", "job-broken"), "w") as f:
                f.write("{not valid json")
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {})
            self.assertIn("job-broken", html)
            self.assertIn("unreadable", html)

    def test_no_jobs_at_all(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            html = server.render_jobs(make_cfg(jobs_dir, output_root), {})
            self.assertIn("No jobs match", html)


class RenderJobDetailTest(unittest.TestCase):
    def test_shows_workload_state_and_configuration(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            job = joblib.build_job(["xz", "-k", "input.tar"], "cpu2026", "801.xz_s", "preset",
                                    profile="deep-cpu")
            joblib.save_job(joblib.job_path(jobs_dir, "pending", job["job_id"]), job)
            html = server.render_job_detail(make_cfg(jobs_dir, output_root), job["job_id"])
            self.assertIsNotNone(html)
            self.assertIn("xz -k input.tar", html)
            self.assertIn("pending", html)
            self.assertIn("preset=deep-cpu", html)

    def test_nonexistent_job_returns_none(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            self.assertIsNone(server.render_job_detail(make_cfg(jobs_dir, output_root), "no-such-job"))

    def test_unreadable_job_file_shows_an_error_page_not_a_crash(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            with open(joblib.job_path(jobs_dir, "pending", "job-broken"), "w") as f:
                f.write("{not valid json")
            html = server.render_job_detail(make_cfg(jobs_dir, output_root), "job-broken")
            self.assertIsNotNone(html)
            self.assertIn("Could not read this job file", html)

    def test_raw_json_is_included(self):
        with tempfile.TemporaryDirectory() as jobs_dir, tempfile.TemporaryDirectory() as output_root:
            joblib.ensure_jobs_dirs(jobs_dir)
            job = joblib.build_job(["a"], "manual", "b", "preset", profile="quick",
                                    notes="a distinctive note")
            joblib.save_job(joblib.job_path(jobs_dir, "pending", job["job_id"]), job)
            html = server.render_job_detail(make_cfg(jobs_dir, output_root), job["job_id"])
            self.assertIn("a distinctive note", html)
            # The raw document too (not just the human-readable "Notes:" line above).
            self.assertIn(json.dumps(job["job_id"]).replace('"', "&quot;"), html)


if __name__ == "__main__":
    unittest.main()
