#!/usr/bin/env python3
"""
web/test_wp_client.py -- unit tests for web/wp_client.py's pure logic
(auth header encoding, error parsing, the parent+slug hierarchy walk).
Not wired into make test/run_tests.sh, matching this codebase's existing
"web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention (see web/test_joblib.py) -- run standalone:

    python3 web/test_wp_client.py

No network access: request()'s urllib call is mocked directly, and the
hierarchy-walk tests mock find_page()/create_page() rather than the HTTP
layer, since find_or_create_page_path()'s own logic (reuse vs. create,
parent chaining) is what's under test, not request()'s wire format.
"""
import base64
import io
import os
import sys
import unittest
import urllib.error
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wp_client


class AuthHeaderTest(unittest.TestCase):
    def test_encodes_username_and_password(self):
        header = wp_client.auth_header("wspy", "abcd1234")
        expected = "Basic " + base64.b64encode(b"wspy:abcd1234").decode("ascii")
        self.assertEqual(header, expected)

    def test_strips_spaces_from_display_formatted_password(self):
        # WordPress shows Application Passwords as "abcd 1234 efgh 5678" for
        # readability but the real password has no spaces.
        header = wp_client.auth_header("wspy", "abcd 1234 efgh 5678")
        expected = "Basic " + base64.b64encode(b"wspy:abcd1234efgh5678").decode("ascii")
        self.assertEqual(header, expected)


class FakeHTTPResponse:
    def __init__(self, body):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class RequestTest(unittest.TestCase):
    def test_returns_parsed_json_on_success(self):
        with patch("wp_client.urllib.request.urlopen",
                   return_value=FakeHTTPResponse(b'{"id": 12, "slug": "home"}')):
            result = wp_client.request("https://example.org/workload", "wp/v2/pages/12",
                                        "wspy", "secret")
        self.assertEqual(result, {"id": 12, "slug": "home"})

    def test_sends_both_standard_and_bridge_auth_headers(self):
        # Some hosts (IONOS, confirmed live 2026-07-31) strip the standard
        # Authorization header before PHP sees it; request() also sends an
        # identical value under X-WSPY-Authorization for a site-side
        # plugin (scripts/wp-auth-bridge.php) to recover it from.
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["headers"] = dict(req.header_items())
            return FakeHTTPResponse(b"{}")

        with patch("wp_client.urllib.request.urlopen", side_effect=fake_urlopen):
            wp_client.request("https://example.org/workload", "wp/v2/pages/12", "wspy", "secret")

        self.assertIn("Authorization", captured["headers"])
        self.assertEqual(captured["headers"]["Authorization"], captured["headers"]["X-wspy-authorization"])

    def test_raises_wperror_with_site_message_on_http_error(self):
        body = b'{"code": "rest_cannot_edit_pages", "message": "Sorry, you are not allowed to edit pages."}'
        err = urllib.error.HTTPError("url", 403, "Forbidden", {}, io.BytesIO(body))
        with patch("wp_client.urllib.request.urlopen", side_effect=err):
            with self.assertRaises(wp_client.WPError) as ctx:
                wp_client.request("https://example.org/workload", "wp/v2/pages", "wspy", "secret",
                                   method="POST", json_body={"slug": "x"})
        self.assertEqual(ctx.exception.status, 403)
        self.assertEqual(ctx.exception.code, "rest_cannot_edit_pages")
        self.assertIn("not allowed to edit pages", str(ctx.exception))

    def test_raises_wperror_on_unreachable_host(self):
        err = urllib.error.URLError("Name or service not known")
        with patch("wp_client.urllib.request.urlopen", side_effect=err):
            with self.assertRaises(wp_client.WPError) as ctx:
                wp_client.request("https://nope.invalid", "wp/v2/pages", "wspy", "secret")
        self.assertIsNone(ctx.exception.status)


class MissingCapabilitiesTest(unittest.TestCase):
    def test_reports_absent_capabilities_only(self):
        user = {"capabilities": {"edit_pages": True, "upload_files": False}}
        self.assertEqual(
            wp_client.missing_capabilities(user),
            ["edit_published_pages", "publish_pages", "upload_files"])

    def test_all_present_is_empty(self):
        user = {"capabilities": {c: True for c in wp_client.REQUIRED_PAGE_CAPABILITIES}}
        self.assertEqual(wp_client.missing_capabilities(user), [])


class FindOrCreatePagePathTest(unittest.TestCase):
    def test_reuses_existing_pages_and_creates_only_missing_ones(self):
        # Mirrors the real site's state at design time: "cpu2026" already
        # exists as a top-level page; its child test-level page does not.
        existing = {"cpu2026": {"id": 17, "slug": "cpu2026", "parent": 0}}
        created_calls = []

        def fake_find_page(site_url, username, app_password, slug, parent):
            return existing.get(slug)

        def fake_create_page(site_url, username, app_password, slug, title, parent, **kw):
            created_calls.append((slug, parent))
            return {"id": 100 + len(created_calls), "slug": slug, "parent": parent}

        with patch("wp_client.find_page", side_effect=fake_find_page), \
             patch("wp_client.create_page", side_effect=fake_create_page):
            results = wp_client.find_or_create_page_path(
                "https://example.org/workload", "wspy", "secret",
                [("cpu2026", "cpu2026"), ("500.perlbench_r", "500.perlbench_r")])

        (page1, created1), (page2, created2) = results
        self.assertEqual(page1["id"], 17)
        self.assertFalse(created1)
        self.assertTrue(created2)
        self.assertEqual(page2["parent"], 17)  # chained onto the existing parent's id
        self.assertEqual(created_calls, [("500.perlbench_r", 17)])

    def test_creates_every_level_when_none_exist(self):
        def fake_find_page(*a, **kw):
            return None

        created_calls = []

        def fake_create_page(site_url, username, app_password, slug, title, parent, **kw):
            created_calls.append((slug, parent))
            return {"id": 200 + len(created_calls), "slug": slug, "parent": parent}

        with patch("wp_client.find_page", side_effect=fake_find_page), \
             patch("wp_client.create_page", side_effect=fake_create_page):
            results = wp_client.find_or_create_page_path(
                "https://example.org/workload", "wspy", "secret",
                [("phoronix", "Phoronix"), ("coremark", "coremark"), ("default", "default")])

        self.assertTrue(all(created for _, created in results))
        # each level's parent is the previous level's freshly-created id
        self.assertEqual(created_calls, [("phoronix", 0), ("coremark", 201), ("default", 202)])


class GetPageTest(unittest.TestCase):
    def test_gets_page_by_id_with_no_side_effects(self):
        captured = {}

        def fake_request(site_url, path, username, app_password, method="GET", params=None,
                          json_body=None, timeout=20):
            captured["path"] = path
            captured["method"] = method
            captured["json_body"] = json_body
            return {"id": 42, "status": "draft", "link": "https://example.org/workload/foo/"}

        with patch("wp_client.request", side_effect=fake_request):
            page = wp_client.get_page("https://example.org/workload", "wspy", "secret", 42)

        self.assertEqual(captured["path"], "wp/v2/pages/42")
        self.assertEqual(captured["method"], "GET")
        self.assertIsNone(captured["json_body"])
        self.assertEqual(page["id"], 42)


class PublishPageTest(unittest.TestCase):
    def test_flips_status_to_publish(self):
        captured = {}

        def fake_request(site_url, path, username, app_password, method="GET", params=None,
                          json_body=None, timeout=20):
            captured["path"] = path
            captured["method"] = method
            captured["json_body"] = json_body
            return {"id": 42, "status": "publish", "link": "https://example.org/workload/foo/"}

        with patch("wp_client.request", side_effect=fake_request):
            page = wp_client.publish_page("https://example.org/workload", "wspy", "secret", 42)

        self.assertEqual(captured["path"], "wp/v2/pages/42")
        self.assertEqual(captured["method"], "POST")
        self.assertEqual(captured["json_body"], {"status": "publish"})
        self.assertEqual(page["status"], "publish")


class DeletePageTest(unittest.TestCase):
    def test_trashes_by_default(self):
        captured = {}

        def fake_request(site_url, path, username, app_password, method="GET", params=None,
                          json_body=None, timeout=20):
            captured["path"] = path
            captured["method"] = method
            captured["params"] = params
            return {"id": 42, "status": "trash"}

        with patch("wp_client.request", side_effect=fake_request):
            page = wp_client.delete_page("https://example.org/workload", "wspy", "secret", 42)

        self.assertEqual(captured["path"], "wp/v2/pages/42")
        self.assertEqual(captured["method"], "DELETE")
        self.assertIsNone(captured["params"])
        self.assertEqual(page["status"], "trash")

    def test_force_deletes_permanently(self):
        captured = {}

        def fake_request(site_url, path, username, app_password, method="GET", params=None,
                          json_body=None, timeout=20):
            captured["params"] = params
            return {"deleted": True}

        with patch("wp_client.request", side_effect=fake_request):
            wp_client.delete_page("https://example.org/workload", "wspy", "secret", 42, force=True)

        self.assertEqual(captured["params"], {"force": "true"})


if __name__ == "__main__":
    unittest.main()
