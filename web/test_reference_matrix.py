#!/usr/bin/env python3
"""
web/test_reference_matrix.py -- unit tests for server.py's
resolve_reference_matrix_row_publish_status() (INVESTIGATION.md 4.3 Tier 3 item 5's per-cell
"already posted" lookup). Not wired into make test/run_tests.sh, same "web/ is stdlib-only Python,
not covered by the C toolchain's test targets" convention as the rest of web/test_*.py -- run
standalone:

    python3 web/test_reference_matrix.py

No network access: wp_client.find_page()/list_child_pages() are mocked directly, same approach
web/test_wp_client.py's FindOrCreatePagePathTest uses for the same underlying primitives.
"""
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


if __name__ == "__main__":
    unittest.main()
