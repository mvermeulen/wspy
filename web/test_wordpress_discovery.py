#!/usr/bin/env python3
"""
web/test_wordpress_discovery.py -- unit tests for server.py's discover_wordpress_matrix_rows() and
execute_discovery() (INVESTIGATION.md 4.3 item 22: full top-down discovery of reference-matrix rows
purely from what's published on WordPress). Not wired into make test/run_tests.sh, same "web/ is
stdlib-only Python, not covered by the C toolchain's test targets" convention as the rest of
web/test_*.py -- run standalone:

    python3 web/test_wordpress_discovery.py

No network access: wp_client.find_page()/list_child_pages() are mocked directly, same approach
web/test_wordpress_recovery.py uses for the same underlying primitives.
"""
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server
import wp_client

FAKE_WP_CFG = {"wordpress": {"site_url": "https://example.org/workload",
                              "username": "wspy", "app_password": "secret"}}


class DiscoverWordpressMatrixRowsTest(unittest.TestCase):
    def test_empty_when_no_wordpress_configured(self):
        rows = server.discover_wordpress_matrix_rows(None, "phoronix")
        self.assertEqual(rows, [])

    def test_empty_when_suite_page_missing(self):
        with patch("server.wp_client.find_page", return_value=None), \
             patch("server.wp_client.list_child_pages") as mock_list:
            rows = server.discover_wordpress_matrix_rows(FAKE_WP_CFG, "phoronix")
        self.assertEqual(rows, [])
        mock_list.assert_not_called()

    def test_walks_full_hierarchy_to_machine_level(self):
        def fake_find_page(site_url, username, app_password, slug, parent):
            return {"id": 1} if (slug, parent) == ("phoronix", 0) else None

        # phoronix(1) -> coremark(2) -> default(3) -> amd-395(4), amd-370-64gb(5)
        children_by_parent = {
            1: [{"id": 2, "slug": "coremark"}],
            2: [{"id": 3, "slug": "default"}],
            3: [{"id": 4, "slug": "amd-395"}, {"id": 5, "slug": "amd-370-64gb"}],
        }

        def fake_list_child_pages(site_url, username, app_password, parent):
            return children_by_parent.get(parent, [])

        with patch("server.wp_client.find_page", side_effect=fake_find_page), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages):
            rows = server.discover_wordpress_matrix_rows(FAKE_WP_CFG, "phoronix")

        self.assertEqual(rows, [
            {"suite": "phoronix", "test": "coremark", "test_point": "default", "machine": "amd-395"},
            {"suite": "phoronix", "test": "coremark", "test_point": "default",
             "machine": "amd-370-64gb"},
        ])

    def test_test_point_with_no_machine_children_contributes_no_row(self):
        # An auto-created empty stub page (publish_page_at_path()'s parent-stub creation) with no
        # real machine underneath yet must not be reported as a discovered row.
        def fake_find_page(site_url, username, app_password, slug, parent):
            return {"id": 1} if (slug, parent) == ("phoronix", 0) else None

        children_by_parent = {1: [{"id": 2, "slug": "coremark"}], 2: [{"id": 3, "slug": "default"}],
                               3: []}

        def fake_list_child_pages(site_url, username, app_password, parent):
            return children_by_parent.get(parent, [])

        with patch("server.wp_client.find_page", side_effect=fake_find_page), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages):
            rows = server.discover_wordpress_matrix_rows(FAKE_WP_CFG, "phoronix")
        self.assertEqual(rows, [])

    def test_progress_called_once_per_test_page(self):
        def fake_find_page(site_url, username, app_password, slug, parent):
            return {"id": 1} if (slug, parent) == ("phoronix", 0) else None

        children_by_parent = {1: [{"id": 2, "slug": "coremark"}, {"id": 3, "slug": "blender"}]}

        def fake_list_child_pages(site_url, username, app_password, parent):
            return children_by_parent.get(parent, [])

        seen = []
        with patch("server.wp_client.find_page", side_effect=fake_find_page), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages):
            server.discover_wordpress_matrix_rows(FAKE_WP_CFG, "phoronix", progress=seen.append)
        self.assertEqual(len(seen), 2)
        self.assertIn("coremark", seen[0])
        self.assertIn("blender", seen[1])


class ExecuteDiscoveryTest(unittest.TestCase):
    def test_relays_progress_and_finishes_done_with_rows(self):
        state = server.DiscoveryState()
        fake_rows = [{"suite": "phoronix", "test": "coremark", "test_point": "default",
                      "machine": "amd-395"}]

        with patch("server.discover_wordpress_matrix_rows", return_value=fake_rows) as mock_discover:
            server.execute_discovery(state, FAKE_WP_CFG, "phoronix")

        self.assertEqual(state.status, "done")
        self.assertEqual(state.rows, fake_rows)
        self.assertTrue(any("found 1 machine-level page" in line for line in state.lines))
        mock_discover.assert_called_once()

    def test_wp_error_finishes_error_status(self):
        state = server.DiscoveryState()
        with patch("server.discover_wordpress_matrix_rows",
                   side_effect=wp_client.WPError("boom", status=500)):
            server.execute_discovery(state, FAKE_WP_CFG, "phoronix")
        self.assertEqual(state.status, "error")
        self.assertEqual(state.rows, [])
        self.assertTrue(any("boom" in line for line in state.lines))


class DiscoveryStateTest(unittest.TestCase):
    def test_starts_running_with_no_rows(self):
        state = server.DiscoveryState()
        self.assertEqual(state.status, "running")
        self.assertEqual(state.rows, [])
        self.assertEqual(state.lines, [])

    def test_finish_defaults_rows_to_empty_list(self):
        state = server.DiscoveryState()
        state.finish("done")
        self.assertEqual(state.rows, [])
        self.assertEqual(state.status, "done")


if __name__ == "__main__":
    unittest.main()
