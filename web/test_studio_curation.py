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


class LoadDefaultCurationConfigTest(unittest.TestCase):
    def _write_config(self, text):
        fd, path = tempfile.mkstemp()
        with os.fdopen(fd, "w") as f:
            f.write(text)
        self.addCleanup(os.remove, path)
        return path

    def test_parses_included_and_excluded_entries_in_order(self):
        path = self._write_config(
            "## section header, not an entry\n"
            "\n"
            "command.txt\n"
            "# systemtime.csv  # trailing note, ignored\n"
            "plots/*.png\n"
        )
        self.assertEqual(server.load_default_curation_config(path), [
            ("command.txt", True),
            ("systemtime.csv", False),
            ("plots/*.png", True),
        ])

    def test_missing_config_file_returns_empty(self):
        self.assertEqual(server.load_default_curation_config("/no/such/file.conf"), [])

    def test_real_shipped_config_parses_and_orders_command_txt_first(self):
        entries = server.load_default_curation_config()
        self.assertTrue(entries)
        self.assertEqual(entries[0], ("command.txt", True))


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

    def test_includes_vision_analysis_but_not_its_prompt_or_critique(self):
        # Mirrors test_includes_command_txt_and_narrative_analysis_but_not_
        # critique_or_prompt above, for wspy-analyze --image's "aivision."
        # naming (INVESTIGATION.md's "Vision-based topdown-chart analysis").
        self._write_manifest([{"name": "quick", "output": "quick.txt", "manifest": None, "status": "ok"}])
        for name in ("quick.txt", "aivision.amdtopdown.topdown.gemma4-26b.md",
                     "aiprompt.vision.amdtopdown.topdown.md",
                     "aiprompt.critique.vision.amdtopdown.topdown.gemma4-26b.md"):
            _touch(os.path.join(self.tmpdir, name))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = {b["source_file"] for b in blocks}
        self.assertIn("aivision.amdtopdown.topdown.gemma4-26b.md", source_files)
        self.assertNotIn("aiprompt.vision.amdtopdown.topdown.md", source_files)
        self.assertNotIn("aiprompt.critique.vision.amdtopdown.topdown.gemma4-26b.md", source_files)
        vision_block = next(b for b in blocks
                             if b["source_file"] == "aivision.amdtopdown.topdown.gemma4-26b.md")
        self.assertTrue(vision_block["ai_generated"])

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

    def test_config_orders_and_excludes_known_artifacts(self):
        # Regression coverage for default_curation.conf's actual shipped
        # policy: command.txt first, systemtime.system-cpu.png right after
        # it, and known-noisy artifacts (raw per-tick CSVs, the fallback/
        # detail plots) left out even though they exist on disk.
        self._write_manifest([
            {"name": "systemtime", "output": "systemtime.csv", "manifest": None, "status": "ok"},
            {"name": "counters", "output": "counters.txt", "manifest": None, "status": "ok"},
            {"name": "amdtopdown", "output": "amdtopdown.csv", "manifest": None, "status": "ok"},
        ])
        for name in ("systemtime.csv", "counters.txt", "amdtopdown.csv", "command.txt"):
            _touch(os.path.join(self.tmpdir, name))
        os.mkdir(os.path.join(self.tmpdir, "plots"))
        for name in ("amdtopdown.metrics.png", "amdtopdown.topdown.png", "systemtime.system-cpu.png"):
            _touch(os.path.join(self.tmpdir, "plots", name))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = [b["source_file"] for b in blocks]

        self.assertEqual(source_files[0], "command.txt")
        self.assertEqual(source_files[1], "plots/systemtime.system-cpu.png")
        self.assertIn("counters.txt", source_files)
        self.assertIn("plots/amdtopdown.topdown.png", source_files)
        # Excluded by default_curation.conf even though present on disk.
        self.assertNotIn("systemtime.csv", source_files)
        self.assertNotIn("amdtopdown.csv", source_files)
        self.assertNotIn("plots/amdtopdown.metrics.png", source_files)

    def test_config_excluded_pass_output_not_reintroduced_by_fallback(self):
        # The pass-output fallback loop (for profiles the config doesn't
        # know about) must not undo an explicit exclusion for a pass name
        # the config *does* know about.
        self._write_manifest([
            {"name": "tree", "output": "tree.txt", "manifest": None, "status": "ok"},
        ])
        _touch(os.path.join(self.tmpdir, "tree.txt"))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        self.assertNotIn("tree.txt", [b["source_file"] for b in blocks])

    def test_config_unlisted_artifacts_still_appended_via_fallback(self):
        # A pass name/plot the config has no opinion on (e.g. a different
        # wspy-run profile) must still show up, same as pre-config behavior.
        self._write_manifest([
            {"name": "software_branch", "output": "software_branch.txt", "manifest": None, "status": "ok"},
        ])
        _touch(os.path.join(self.tmpdir, "software_branch.txt"))
        os.mkdir(os.path.join(self.tmpdir, "plots"))
        _touch(os.path.join(self.tmpdir, "plots", "software_branch.ipc.png"))

        blocks = server.build_default_curation_blocks(self.tmpdir)
        source_files = [b["source_file"] for b in blocks]
        self.assertIn("software_branch.txt", source_files)
        self.assertIn("plots/software_branch.ipc.png", source_files)

    def test_no_run_manifest_still_returns_something_sensible(self):
        # No manifest.json at all (e.g. the legacy fixed-config layout) --
        # no pass list to walk, but command.txt (if present) still gets
        # picked up rather than the function crashing or returning nothing.
        _touch(os.path.join(self.tmpdir, "command.txt"))
        blocks = server.build_default_curation_blocks(self.tmpdir)
        self.assertEqual([b["source_file"] for b in blocks], ["command.txt"])


if __name__ == "__main__":
    unittest.main()
