#!/usr/bin/env python3
"""
web/test_reference_matrix.py -- unit tests for server.py's reference-matrix pieces
(INVESTIGATION.md 4.3 Tier 3 item 5): resolve_reference_matrix_row_publish_status() (per-cell
"already posted" lookup), render_reference_tab() (the overview), and
render_reference_test_point_detail() (the cross-machine comparison page). Not wired into make
test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test targets"
convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_reference_matrix.py

No network/subprocess access: wp_client.find_page()/list_child_pages() are mocked directly (same
approach web/test_wp_client.py's FindOrCreatePagePathTest uses for the same underlying primitives),
and joblib.subprocess.run() is mocked for the aggregation-calling detail-page tests. server.py's
module-level PHORONIX_DEST_ROOT/CPU2026_DEST_ROOT are monkeypatched per test to a throwaway
tempfile.TemporaryDirectory() rather than touching this checkout's real workload/ tree.
Every RenderReferenceTestPointDetailTest case also patches wp_client.load_config -- that function
render_reference_test_point_detail() now calls unconditionally (item 21's WordPress-recovery merge,
tested for real in web/test_wordpress_recovery.py) would otherwise read this machine's real
~/.config/wspy/publish.json and, if one exists, reach live WordPress from what must stay a
network-free unit test.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joblib
import server

FAKE_WP_CFG = {"wordpress": {"site_url": "https://example.org/workload",
                              "username": "wspy", "app_password": "secret"}}


class ResolveReferenceMatrixRowPublishStatusTest(unittest.TestCase):
    def test_empty_when_no_wordpress_configured(self):
        status = server.resolve_reference_matrix_row_publish_status(None, "phoronix", "coremark", "default")
        self.assertEqual(status, {})

    def test_walks_hierarchy_and_lists_machine_children(self):
        pages = {("phoronix", 0): {"id": 1}, ("coremark", 1): {"id": 2}, ("default", 2): {"id": 3}}

        def fake_find_page(site_url, username, app_password, slug, parent):
            return pages.get((slug, parent))

        def fake_list_child_pages(site_url, username, app_password, parent):
            self.assertEqual(parent, 3)
            return [{"slug": "amd-395", "link": "https://example.org/workload/.../amd-395/",
                     "status": "publish"},
                    {"slug": "amd-370-64gb", "link": "https://example.org/workload/.../amd-370-64gb/",
                     "status": "draft"}]

        with patch("server.wp_client.find_page", side_effect=fake_find_page), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages):
            status = server.resolve_reference_matrix_row_publish_status(
                FAKE_WP_CFG, "phoronix", "coremark", "default")

        self.assertEqual(status["amd-395"]["status"], "publish")
        self.assertEqual(status["amd-370-64gb"]["status"], "draft")
        self.assertTrue(status["amd-395"]["link"].endswith("amd-395/"))

    def test_empty_when_test_point_level_page_does_not_exist(self):
        pages = {("phoronix", 0): {"id": 1}, ("coremark", 1): {"id": 2}}  # no "default" page

        def fake_find_page(site_url, username, app_password, slug, parent):
            return pages.get((slug, parent))

        with patch("server.wp_client.find_page", side_effect=fake_find_page), \
             patch("server.wp_client.list_child_pages") as mock_list:
            status = server.resolve_reference_matrix_row_publish_status(
                FAKE_WP_CFG, "phoronix", "coremark", "default")

        self.assertEqual(status, {})
        mock_list.assert_not_called()

    def test_empty_when_suite_level_page_does_not_exist(self):
        with patch("server.wp_client.find_page", return_value=None), \
             patch("server.wp_client.list_child_pages") as mock_list:
            status = server.resolve_reference_matrix_row_publish_status(
                FAKE_WP_CFG, "phoronix", "coremark", "default")
        self.assertEqual(status, {})
        mock_list.assert_not_called()


def materialize_fake_phoronix_point(phoronix_dest, bare_name="coremark", options="", test_id=None):
    point = {"test_id": test_id or f"pts/{bare_name}-1.0.0", "arguments": options}
    return joblib.materialize_phoronix_test_point(point, phoronix_dest, "file", "/tmp/src.xml")


def write_runs_json(report_root_path, suite, test, test_point, machine, runs):
    machine_dir = os.path.join(report_root_path, suite, test, test_point, machine)
    os.makedirs(machine_dir, exist_ok=True)
    with open(os.path.join(machine_dir, "runs.json"), "w") as f:
        json.dump({"runs": runs}, f)


class RenderReferenceTabTest(unittest.TestCase):
    def test_no_cells_shows_placeholder(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_root_path = os.path.join(tmpdir, "report-root")
            os.makedirs(report_root_path)
            with patch("server.PHORONIX_DEST_ROOT", os.path.join(tmpdir, "phoronix")), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")):
                html_out = server.render_reference_tab({"report_root": report_root_path})
        self.assertIn("No test points have a curated run set yet", html_out)

    def test_shows_run_count_and_links_to_detail(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395",
                             [{"role": "stats-pool"}, {"role": "stats-pool"}, {"role": "excluded"}])

            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("server.wp_client.load_config", return_value=None):
                html_out = server.render_reference_tab({"report_root": report_root_path})

        self.assertIn("amd-395", html_out)
        self.assertIn("2 run(s)", html_out)
        self.assertIn(f"/reference/phoronix/{info['bare_name']}/{info['options_slug']}", html_out)

    def test_publish_status_badge_shown_when_wordpress_configured(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395", [{"role": "stats-pool"}])

            def fake_find_page(site_url, username, app_password, slug, parent):
                return {"id": {"phoronix": 1, info["bare_name"]: 2, info["options_slug"]: 3}[slug]}

            def fake_list_child_pages(site_url, username, app_password, parent):
                return [{"slug": "amd-395", "link": "https://example.org/amd-395/", "status": "publish"}]

            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("server.wp_client.load_config", return_value=FAKE_WP_CFG), \
                 patch("server.wp_client.find_page", side_effect=fake_find_page), \
                 patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages):
                html_out = server.render_reference_tab({"report_root": report_root_path})

        self.assertIn("badge-published", html_out)
        self.assertIn("https://example.org/amd-395/", html_out)


class RenderReferenceTestPointDetailTest(unittest.TestCase):
    # Every test here patches wp_client.load_config to return None (item 21's WordPress-recovery
    # merge is inert with no config) -- without it, render_reference_test_point_detail's new
    # unconditional wp_client.load_config() call would read this machine's real
    # ~/.config/wspy/publish.json and, if one exists, make live WordPress calls from what must stay a
    # network-free unit test.
    def test_no_curated_run_set_shows_message(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_root_path = os.path.join(tmpdir, "report-root")
            os.makedirs(report_root_path)
            with patch("server.PHORONIX_DEST_ROOT", os.path.join(tmpdir, "phoronix")), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("server.wp_client.load_config", return_value=None):
                html_out = server.render_reference_test_point_detail(
                    {"report_root": report_root_path}, "phoronix", "coremark", "default")
        self.assertIn("No machine here has a curated", html_out)

    def test_renders_metric_rows_across_machine_columns(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395", [{"role": "stats-pool"}])

            fake_csv = ("group,metric,n,min,max,mean,stddev,cv_percent,verdict\n"
                        "ipc,ipc,2,1.0,1.2,1.1,0.05,4.5,PASS\n")

            def fake_run(argv, capture_output, text, timeout):
                return subprocess.CompletedProcess(argv, 0, stdout=fake_csv, stderr="")

            cfg = {"report_root": report_root_path, "report_root_remote": None,
                   "wspy_testpoint_bin": "wspy-testpoint", "store_db": "store.db"}
            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("joblib.subprocess.run", side_effect=fake_run), \
                 patch("server.wp_client.load_config", return_value=None):
                html_out = server.render_reference_test_point_detail(
                    cfg, "phoronix", info["bare_name"], info["options_slug"])

        self.assertIn("amd-395", html_out)
        self.assertIn("1.1", html_out)
        self.assertIn("(n=2)", html_out)
        self.assertNotIn("metric-warn", html_out)

    def test_non_pass_verdict_gets_warn_class(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395", [{"role": "stats-pool"}])

            fake_csv = ("group,metric,n,min,max,mean,stddev,cv_percent,verdict\n"
                        "ipc,ipc,2,1.0,1.2,1.1,0.05,4.5,WARN:noisy\n")

            def fake_run(argv, capture_output, text, timeout):
                return subprocess.CompletedProcess(argv, 0, stdout=fake_csv, stderr="")

            cfg = {"report_root": report_root_path, "report_root_remote": None,
                   "wspy_testpoint_bin": "wspy-testpoint", "store_db": "store.db"}
            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("joblib.subprocess.run", side_effect=fake_run), \
                 patch("server.wp_client.load_config", return_value=None):
                html_out = server.render_reference_test_point_detail(
                    cfg, "phoronix", info["bare_name"], info["options_slug"])

        self.assertIn("metric-warn", html_out)

    def test_aggregation_failure_shows_message_not_a_crash(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395", [{"role": "stats-pool"}])

            cfg = {"report_root": report_root_path, "report_root_remote": None,
                   "wspy_testpoint_bin": "wspy-testpoint", "store_db": "store.db"}
            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("joblib.subprocess.run", side_effect=OSError("no such file")), \
                 patch("server.wp_client.load_config", return_value=None):
                html_out = server.render_reference_test_point_detail(
                    cfg, "phoronix", info["bare_name"], info["options_slug"])

        self.assertIn("failed or produced no rows", html_out)

    def test_wordpress_only_machine_appears_as_extra_column(self):
        # No local runs.json anywhere -- item 21's whole point: a machine known only via WordPress
        # still shows up, clearly marked, instead of the page just saying "no data at all".
        with tempfile.TemporaryDirectory() as tmpdir:
            report_root_path = os.path.join(tmpdir, "report-root")
            os.makedirs(report_root_path)
            cfg = {"report_root": report_root_path, "report_root_remote": None,
                   "wspy_testpoint_bin": "wspy-testpoint", "store_db": "store.db"}

            def fake_status(wp_cfg, suite, test, test_point):
                return {"amd-395-96gb": {"link": "https://example.org/x/", "status": "publish"}}

            with patch("server.PHORONIX_DEST_ROOT", os.path.join(tmpdir, "phoronix")), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("server.wp_client.load_config", return_value={"wordpress": {}}), \
                 patch("server.resolve_reference_matrix_row_publish_status", side_effect=fake_status), \
                 patch("server.recover_machine_metrics_from_wordpress",
                       return_value=[{"metric": "elapsed", "n": "1", "min": "1.0", "max": "1.0",
                                      "mean": "1.0", "stddev": "0"}]):
                html_out = server.render_reference_test_point_detail(
                    cfg, "phoronix", "coremark", "default")

        self.assertIn("amd-395-96gb", html_out)
        self.assertIn("(from WordPress)", html_out)
        self.assertIn("wp-recovered", html_out)
        self.assertIn("elapsed", html_out)

    def test_local_data_wins_over_wordpress_for_the_same_machine(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            phoronix_dest = os.path.join(tmpdir, "phoronix")
            info = materialize_fake_phoronix_point(phoronix_dest)
            report_root_path = os.path.join(tmpdir, "report-root")
            write_runs_json(report_root_path, "phoronix", info["bare_name"], info["options_slug"],
                             "amd-395", [{"role": "stats-pool"}])

            fake_csv = ("group,metric,n,min,max,mean,stddev,cv_percent,verdict\n"
                        "ipc,ipc,2,1.0,1.2,1.1,0.05,4.5,PASS\n")

            def fake_run(argv, capture_output, text, timeout):
                return subprocess.CompletedProcess(argv, 0, stdout=fake_csv, stderr="")

            def fake_status(wp_cfg, suite, test, test_point):
                return {"amd-395": {"link": "https://example.org/x/", "status": "publish"}}

            recover_mock_calls = []

            def fake_recover(wp_cfg, suite, test, test_point, machine):
                recover_mock_calls.append(machine)
                return [{"metric": "ipc", "n": "9", "min": "9", "max": "9", "mean": "9", "stddev": "0"}]

            cfg = {"report_root": report_root_path, "report_root_remote": None,
                   "wspy_testpoint_bin": "wspy-testpoint", "store_db": "store.db"}
            with patch("server.PHORONIX_DEST_ROOT", phoronix_dest), \
                 patch("server.CPU2026_DEST_ROOT", os.path.join(tmpdir, "cpu2026")), \
                 patch("joblib.subprocess.run", side_effect=fake_run), \
                 patch("server.wp_client.load_config", return_value={"wordpress": {}}), \
                 patch("server.resolve_reference_matrix_row_publish_status", side_effect=fake_status), \
                 patch("server.recover_machine_metrics_from_wordpress", side_effect=fake_recover):
                html_out = server.render_reference_test_point_detail(
                    cfg, "phoronix", info["bare_name"], info["options_slug"])

        # amd-395 already has local data -- recover_machine_metrics_from_wordpress must never be
        # called for it, and the real (store-based) 1.1 value must win, not WordPress's 9.
        self.assertEqual(recover_mock_calls, [])
        self.assertIn("1.1", html_out)
        self.assertNotIn(">9 ", html_out)
        self.assertNotIn("(from WordPress)", html_out)


if __name__ == "__main__":
    unittest.main()
