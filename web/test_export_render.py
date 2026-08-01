#!/usr/bin/env python3
"""
web/test_export_render.py -- unit tests for server.py's export_block_content()/
render_export_wordpress() image_url override (INVESTIGATION.md 4.3 Tier 3
item 2, sub-step 7: wspy-publish's --from-rundir resolves a curated
report's images to already-uploaded WordPress media URLs instead of this
server's own /files/... URL). Not wired into make test/run_tests.sh,
matching this codebase's existing "web/ is stdlib-only Python, not covered
by the C toolchain's test targets" convention -- run standalone:

    python3 web/test_export_render.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class ExportBlockContentImageUrlTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        with open(os.path.join(self.tmpdir, "chart.png"), "wb") as f:
            f.write(b"\x89PNG...")
        self.block = {"source_file": "chart.png", "source_kind": "image",
                      "depth": "full", "title": "chart"}

    def test_default_uses_base_url(self):
        kind, payload, note = server.export_block_content(
            self.tmpdir, "http://127.0.0.1:8765/files/x", self.block)
        self.assertEqual(kind, "image")
        self.assertEqual(payload, "http://127.0.0.1:8765/files/x/chart.png")
        self.assertIsNone(note)

    def test_image_url_override_replaces_base_url_link(self):
        kind, payload, note = server.export_block_content(
            self.tmpdir, "http://127.0.0.1:8765/files/x", self.block,
            image_url=lambda filename: "https://example.org/wp-content/uploads/" + filename)
        self.assertEqual(kind, "image")
        self.assertEqual(payload, "https://example.org/wp-content/uploads/chart.png")

    def test_non_full_depth_ignores_image_url(self):
        block = dict(self.block, depth="none")
        kind, payload, note = server.export_block_content(
            self.tmpdir, "http://127.0.0.1:8765/files/x", block,
            image_url=lambda filename: "https://example.org/should-not-be-used")
        self.assertEqual(kind, "none")


class RenderExportWordpressImageUrlTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        with open(os.path.join(self.tmpdir, "chart.png"), "wb") as f:
            f.write(b"\x89PNG...")

    def test_embeds_overridden_image_url_in_wp_image_block(self):
        blocks = [{"source_file": "chart.png", "source_kind": "image",
                   "depth": "full", "title": "chart"}]
        content = server.render_export_wordpress(
            self.tmpdir, "http://127.0.0.1:8765/files/x", "Report", "", blocks,
            image_url=lambda filename: "https://example.org/wp-content/uploads/" + filename)
        self.assertIn('src="https://example.org/wp-content/uploads/chart.png"', content)
        self.assertNotIn("127.0.0.1", content)

    def test_defaults_to_base_url_when_no_override_given(self):
        blocks = [{"source_file": "chart.png", "source_kind": "image",
                   "depth": "full", "title": "chart"}]
        content = server.render_export_wordpress(
            self.tmpdir, "http://127.0.0.1:8765/files/x", "Report", "", blocks)
        self.assertIn('src="http://127.0.0.1:8765/files/x/chart.png"', content)


if __name__ == "__main__":
    unittest.main()
