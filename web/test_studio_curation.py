#!/usr/bin/env python3
"""
web/test_studio_curation.py -- unit tests for server.py's
build_default_curation_blocks() (the Studio's "Apply default curation"
button, INVESTIGATION.md 4.3 Tier 3 item 2's "default curation" work).
Not wired into make test/run_tests.sh, matching this codebase's existing
"web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention -- run standalone:

    python3 web/test_studio_curation.py
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


def _touch(path):
    with open(path, "w") as f:
        f.write("x")


class BuildDefaultCurationBlocksTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _write_manifest(self, passes):
        manifest = {
            "layout_version": "1.0.0", "suite": "s", "benchmark": "b", "run_id": "r",
            "command": ["true"], "passes": passes,
        }
        with open(os.path.join(self.tmpdir, "manifest.json"), "w") as f:
            json.dump(manifest, f)

    def test_includes_every_pass_output(self):
        self._write_manifest([
            {"name": "counters", "output": "counters.txt", "manifest": None, "status": "ok"},
            {"name": "ibs", "output": "ibs.txt", "manifest": None, "status": "ok"},
        ])
        _touch(os.path.join(self.tmpdir, "counters.txt"))
        _touch(os.path.join(self.tmpdir, "ibs.txt"))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = [b["source_file"] for b in blocks]
        self.assertIn("counters.txt", source_files)
        self.assertIn("ibs.txt", source_files)
        # pass order preserved, matching manifest.json's own passes[] order
        self.assertLess(source_files.index("counters.txt"), source_files.index("ibs.txt"))

    def test_includes_command_txt_and_narrative_analysis_but_not_critique_or_prompt(self):
        self._write_manifest([{"name": "quick", "output": "quick.txt", "manifest": None, "status": "ok"}])
        for name in ("quick.txt", "command.txt", "aianalysis.gpt-oss_20b.txt",
                     "aiprompt.txt", "aiprompt.critique.gpt-oss_20b.txt"):
            _touch(os.path.join(self.tmpdir, name))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = {b["source_file"] for b in blocks}
        self.assertIn("command.txt", source_files)
        self.assertIn("aianalysis.gpt-oss_20b.txt", source_files)
        self.assertNotIn("aiprompt.txt", source_files)
        self.assertNotIn("aiprompt.critique.gpt-oss_20b.txt", source_files)

    def test_excludes_summary_txt_and_run_manifest(self):
        self._write_manifest([{"name": "quick", "output": "quick.txt", "manifest": None, "status": "ok"}])
        for name in ("quick.txt", "summary.txt", "launch.log"):
            _touch(os.path.join(self.tmpdir, name))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = {b["source_file"] for b in blocks}
        self.assertNotIn("summary.txt", source_files)
        self.assertNotIn("manifest.json", source_files)
        self.assertNotIn("launch.log", source_files)

    def test_includes_every_plot(self):
        self._write_manifest([{"name": "quick", "output": "quick.txt", "manifest": None, "status": "ok"}])
        _touch(os.path.join(self.tmpdir, "quick.txt"))
        os.mkdir(os.path.join(self.tmpdir, "plots"))
        _touch(os.path.join(self.tmpdir, "plots", "a.png"))
        _touch(os.path.join(self.tmpdir, "plots", "b.png"))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = {b["source_file"] for b in blocks}
        self.assertIn("plots/a.png", source_files)
        self.assertIn("plots/b.png", source_files)

    def test_every_block_defaults_to_full_depth(self):
        self._write_manifest([{"name": "quick", "output": "quick.txt", "manifest": None, "status": "ok"}])
        _touch(os.path.join(self.tmpdir, "quick.txt"))
        blocks = server.build_default_curation_blocks(self.tmpdir)
        self.assertTrue(blocks)
        self.assertTrue(all(b["depth"] == "full" for b in blocks))

    def test_no_run_manifest_still_returns_something_sensible(self):
        # No manifest.json at all (e.g. the legacy fixed-config layout) --
        # no pass list to walk, but command.txt (if present) still gets
        # picked up rather than the function crashing or returning nothing.
        _touch(os.path.join(self.tmpdir, "command.txt"))
        blocks = server.build_default_curation_blocks(self.tmpdir)
        self.assertEqual([b["source_file"] for b in blocks], ["command.txt"])


if __name__ == "__main__":
    unittest.main()
