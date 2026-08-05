#!/usr/bin/env python3
"""
web/test_wordpress_recovery.py -- unit tests for server.py's
recover_machine_metrics_from_wordpress() (INVESTIGATION.md 4.3 item 21: recovering real metric
values from already-published WordPress pages for a machine with no local runs.json/wspy-store
presence). Not wired into make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by
the C toolchain's test targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_wordpress_recovery.py

No network access: wp_client.find_page()/list_child_pages()/fetch_page_raw_content() are mocked
directly, same approach web/test_wp_client.py and web/test_reference_matrix.py use for the same
underlying primitives.
"""
import html
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server

FAKE_WP_CFG = {"wordpress": {"site_url": "https://example.org/workload",
                              "username": "wspy", "app_password": "secret"}}


def preformatted_page(text):
    """A minimal raw WordPress page content string wrapping `text` exactly the way
    render_export_wordpress() does for a full-depth "pre" block."""
    return ('<!-- wp:preformatted -->\n<pre class="wp-block-preformatted">'
            + html.escape(text) + '</pre>\n<!-- /wp:preformatted -->')


COUNTERS_TXT = "elapsed              10.0\n##### pass  0 (mask 0x1) #####\ninstructions         100\n"
COUNTERS_TXT_2 = "elapsed              20.0\n##### pass  0 (mask 0x1) #####\ninstructions         200\n"
IBS_TXT = "ibs_sample_fetch_count     5\n"


class RecoverMachineMetricsFromWordpressTest(unittest.TestCase):
    def _walk_pages(self, machine_page_id=4):
        pages = {("phoronix", 0): {"id": 1}, ("coremark", 1): {"id": 2},
                  ("default", 2): {"id": 3}, ("amd-395", 3): {"id": machine_page_id}}

        def fake_find_page(site_url, username, app_password, slug, parent):
            return pages.get((slug, parent))
        return fake_find_page

    def test_empty_when_no_wordpress_configured(self):
        rows = server.recover_machine_metrics_from_wordpress(
            None, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(rows, [])

    def test_empty_when_hierarchy_level_missing(self):
        with patch("server.wp_client.find_page", return_value=None), \
             patch("server.wp_client.list_child_pages") as mock_list:
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(rows, [])
        mock_list.assert_not_called()

    def test_aggregates_across_run_pages(self):
        def fake_list_child_pages(site_url, username, app_password, parent):
            self.assertEqual(parent, 4)
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"},
                    {"id": 102, "slug": "run-2", "date": "2026-08-02T00:00:00"}]

        raw_by_id = {101: preformatted_page(COUNTERS_TXT), 102: preformatted_page(COUNTERS_TXT_2)}

        def fake_fetch(site_url, username, app_password, page_id):
            return raw_by_id[page_id]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", side_effect=fake_fetch):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(by_metric["elapsed"]["n"], "2")
        self.assertEqual(by_metric["elapsed"]["min"], "10.0")
        self.assertEqual(by_metric["elapsed"]["max"], "20.0")
        self.assertEqual(float(by_metric["elapsed"]["mean"]), 15.0)
        self.assertEqual(by_metric["instructions"]["n"], "2")

    def test_merges_counters_and_ibs_blocks_from_same_run(self):
        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        raw = ('<!-- wp:heading --><h2>Counters</h2><!-- /wp:heading -->'
               + preformatted_page(COUNTERS_TXT)
               + '<!-- wp:heading --><h2>IBS</h2><!-- /wp:heading -->'
               + preformatted_page(IBS_TXT))

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=raw):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        metrics = {r["metric"] for r in rows}
        self.assertIn("elapsed", metrics)
        self.assertIn("ibs_sample_fetch_count", metrics)

    def test_unrecognized_block_shape_skipped(self):
        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        raw = preformatted_page("just a process tree dump, not counter output\n")

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=raw):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(rows, [])

    def test_no_run_pages_returns_empty(self):
        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", return_value=[]):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")
        self.assertEqual(rows, [])

    def test_caps_at_max_recovered_runs_most_recent_first(self):
        pages = [{"id": 100 + i, "slug": f"run-{i}", "date": f"2026-08-{i+1:02d}T00:00:00"}
                 for i in range(server.MAX_WORDPRESS_RECOVERED_RUNS + 5)]

        def fake_list_child_pages(site_url, username, app_password, parent):
            return pages

        fetched_ids = []

        def fake_fetch(site_url, username, app_password, page_id):
            fetched_ids.append(page_id)
            return preformatted_page(COUNTERS_TXT)

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", side_effect=fake_fetch):
            server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        self.assertEqual(len(fetched_ids), server.MAX_WORDPRESS_RECOVERED_RUNS)
        # most-recent-first: the highest-dated pages (largest ids here) are the ones fetched
        expected_ids = sorted((p["id"] for p in pages), reverse=True)[:server.MAX_WORDPRESS_RECOVERED_RUNS]
        self.assertEqual(sorted(fetched_ids), sorted(expected_ids))

    def test_duplicate_label_within_one_run_keeps_first_occurrence(self):
        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        text = "##### pass  0 (mask 0x1) #####\ninstructions  111\n##### pass  1 (mask 0x4) #####\ninstructions  222\n"
        raw = preformatted_page(text)

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=raw):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(by_metric["instructions"]["min"], "111.0")
        self.assertEqual(by_metric["instructions"]["max"], "111.0")


if __name__ == "__main__":
    unittest.main()
