#!/usr/bin/env python3
"""
web/test_progress_indicator.py -- unit tests for server.py's item 2 (INVESTIGATION.md 4.4(a),
"Report-page guided flow / progress indicator") pieces: render_progress_indicator(),
_curation_has_content(), and each report shape's own wiring of the indicator. Not wired into
make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_progress_indicator.py

Every test patches server.wp_client.load_config (same approach web/test_reference_matrix.py's own
tests use) rather than touching this machine's real ~/.config/wspy/publish.json, so results stay
deterministic regardless of whether wspy-publish has actually been configured here.
"""
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class CurationHasContentTest(unittest.TestCase):
    def test_none_is_false(self):
        self.assertFalse(server._curation_has_content(None))

    def test_no_blocks_no_note_is_false(self):
        self.assertFalse(server._curation_has_content({"blocks": []}))

    def test_all_blocks_depth_none_is_false(self):
        self.assertFalse(server._curation_has_content(
            {"blocks": [{"depth": "none"}, {"depth": "none"}]}))

    def test_one_included_block_is_true(self):
        self.assertTrue(server._curation_has_content(
            {"blocks": [{"depth": "none"}, {"depth": "full"}]}))

    def test_overview_note_alone_is_true(self):
        self.assertTrue(server._curation_has_content(
            {"blocks": [], "overview_note": "a note"}))


class RenderProgressIndicatorTest(unittest.TestCase):
    def test_all_stages_default_to_not_started_with_no_wp_config(self):
        with tempfile.TemporaryDirectory() as rundir:
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
        self.assertIn("Run: <span class=\"status-ok\">done</span>", html)
        self.assertIn("Curate: <span class=\"status-unknown\">not started</span>", html)
        self.assertIn("Characterize: <span class=\"status-unknown\">not generated</span>", html)
        self.assertIn("Publish: <span class=\"status-unknown\">not configured</span>", html)

    def test_run_status_variants(self):
        with tempfile.TemporaryDirectory() as rundir:
            with patch("server.wp_client.load_config", return_value=None):
                for status, expected_label, expected_class in [
                    ("ok", "done", "status-ok"),
                    ("skipped", "done", "status-ok"),
                    ("failed", "failed", "status-failed"),
                    ("incomplete", "incomplete", "status-incomplete"),
                    ("unknown", "unknown", "status-unknown"),
                ]:
                    html = server.render_progress_indicator(rundir, "manual", "bench", "run1", status)
                    self.assertIn(f'Run: <span class="{expected_class}">{expected_label}</span>',
                                  html, f"status={status}")

    def test_curation_started_but_not_curated(self):
        with tempfile.TemporaryDirectory() as rundir:
            with open(os.path.join(rundir, "curation.json"), "w") as f:
                json.dump({"blocks": [{"depth": "none"}]}, f)
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
        self.assertIn("Curate: <span class=\"status-warn\">started</span>", html)

    def test_curation_actually_curated(self):
        with tempfile.TemporaryDirectory() as rundir:
            with open(os.path.join(rundir, "curation.json"), "w") as f:
                json.dump({"blocks": [{"depth": "full"}]}, f)
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
        self.assertIn("Curate: <span class=\"status-ok\">curated</span>", html)

    def test_characterization_badge_present(self):
        with tempfile.TemporaryDirectory() as rundir:
            open(os.path.join(rundir, server.ARCHETYPE_BADGE_NAME), "w").close()
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
        self.assertIn("Characterize: <span class=\"status-ok\">generated</span>", html)

    def test_similarity_panel_alone_also_counts(self):
        with tempfile.TemporaryDirectory() as rundir:
            open(os.path.join(rundir, server.ARCHETYPE_SIMILAR_NAME), "w").close()
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
        self.assertIn("Characterize: <span class=\"status-ok\">generated</span>", html)

    def test_publish_configured_shows_without_a_network_call(self):
        with tempfile.TemporaryDirectory() as rundir:
            with patch("server.wp_client.load_config",
                       return_value={"wordpress": {"site_url": "https://example.org"}}) as mock_load:
                html = server.render_progress_indicator(rundir, "manual", "bench", "run1", "ok")
            mock_load.assert_called_once()  # a local file read, never wp_client.find_page()/list_child_pages()
        self.assertIn("Publish: <span class=\"status-warn\">configured</span>", html)

    def test_studio_links_point_at_this_run(self):
        with tempfile.TemporaryDirectory() as rundir:
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_progress_indicator(rundir, "cpu2026", "801.xz_s", "run42", "ok")
        self.assertIn('href="/studio/cpu2026/801.xz_s/run42"', html)


def write_pass_manifest(path, exit_code=0, hostname="h1", preset=None):
    doc = {
        "exit_status": {"known": True, "exited": True, "signaled": False, "exit_code": exit_code},
        "host": {"hostname": hostname}, "command": {"argv": ["sleep", "1"]},
        "configuration_provenance": {"preset": preset, "configuration": "run", "options": []},
    }
    with open(path, "w") as f:
        json.dump(doc, f)


class ReportShapesIncludeIndicatorTest(unittest.TestCase):
    """Each of the three report-render functions must actually surface the indicator -- a
    regression class distinct from RenderProgressIndicatorTest above, which only exercises the
    function in isolation."""

    def test_wspy_run_report(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(os.path.join(rundir, "run.manifest.json"), preset="quick")
            open(os.path.join(rundir, "run.txt"), "w").close()
            run_manifest = {"command": ["sleep", "1"],
                             "passes": [{"name": "run", "output": "run.txt",
                                         "manifest": "run.manifest.json", "status": "ok"}]}
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_wspy_run_report(rundir, "manual", "bench", "run1", run_manifest)
        self.assertIn('class="progress-indicator"', html)
        self.assertIn("Run: <span class=\"status-ok\">done</span>", html)

    def test_fixed_report(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(os.path.join(rundir, server.MANIFEST_NAME), exit_code=0)
            open(os.path.join(rundir, server.CSV_NAME), "w").close()
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_fixed_report(rundir, "manual", "bench", "run1")
        self.assertIn('class="progress-indicator"', html)
        self.assertIn("Run: <span class=\"status-ok\">done</span>", html)

    def test_fixed_report_failed_exit(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(os.path.join(rundir, server.MANIFEST_NAME), exit_code=1)
            open(os.path.join(rundir, server.CSV_NAME), "w").close()
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_fixed_report(rundir, "manual", "bench", "run1")
        self.assertIn("Run: <span class=\"status-failed\">failed</span>", html)

    def test_incomplete_run_report(self):
        with tempfile.TemporaryDirectory() as rundir:
            write_pass_manifest(os.path.join(rundir, "systemtime.manifest.json"), preset="deep-cpu")
            open(os.path.join(rundir, "systemtime.txt"), "w").close()
            incomplete = server.detect_incomplete_wspy_run(rundir)
            self.assertIsNotNone(incomplete)
            with patch("server.wp_client.load_config", return_value=None):
                html = server.render_incomplete_run_report(rundir, "manual", "bench", "run1", incomplete)
        self.assertIn('class="progress-indicator"', html)
        self.assertIn("Run: <span class=\"status-incomplete\">incomplete</span>", html)


if __name__ == "__main__":
    unittest.main()
