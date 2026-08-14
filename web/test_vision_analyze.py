#!/usr/bin/env python3
"""
web/test_vision_analyze.py -- unit tests for server.py's build_vision_analyze_argvs() (the "AI vision
analysis" card's pure argv-construction logic, INVESTIGATION.md's vision deep-dive multi-plot-template
follow-on) and render_vision_card()'s plot-checkbox generation. Covers only the pure, HTTP-free logic --
the surrounding subprocess/threading/SSE plumbing (execute_multi_vision_analyze()/VISION_RUNS) has no
automated coverage, matching this codebase's existing boundary for this class of feature (see
test_testpoint_web.py's own docstring). Not wired into make test/run_tests.sh, matching this codebase's
existing "web/ is stdlib-only Python, not covered by the C toolchain's test targets" convention -- run
standalone:

    python3 web/test_vision_analyze.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


def _touch(path):
    with open(path, "w") as f:
        f.write("")


class BuildVisionAnalyzeArgvsTest(unittest.TestCase):
    def test_single_image_no_models(self):
        argvs = server.build_vision_analyze_argvs(
            "/repo/wspy-analyze", "/repo/web/runs/r1", ["plots/amdtopdown.topdown.png"], [], False, False)
        self.assertEqual(argvs, [
            ["/repo/wspy-analyze", "--rundir", "/repo/web/runs/r1",
             "--image", "plots/amdtopdown.topdown.png"],
        ])

    def test_multiple_images_produce_one_argv_each_in_order(self):
        argvs = server.build_vision_analyze_argvs(
            "/repo/wspy-analyze", "/repo/web/runs/r1",
            ["plots/amdtopdown.topdown.png", "plots/systemtime.system-cpu.png"], [], False, False)
        self.assertEqual(len(argvs), 2)
        self.assertIn("plots/amdtopdown.topdown.png", argvs[0])
        self.assertIn("plots/systemtime.system-cpu.png", argvs[1])

    def test_models_applied_to_every_image(self):
        argvs = server.build_vision_analyze_argvs(
            "/repo/wspy-analyze", "/repo/web/runs/r1",
            ["plots/a.topdown.png", "plots/b.system-cpu.png"],
            ["gemma4:26b", "qwen3.5:35b"], False, False)
        for argv in argvs:
            self.assertEqual(argv.count("--model"), 2)
            self.assertIn("gemma4:26b", argv)
            self.assertIn("qwen3.5:35b", argv)

    def test_all_models_and_critique_flags_applied_to_every_image(self):
        argvs = server.build_vision_analyze_argvs(
            "/repo/wspy-analyze", "/repo/web/runs/r1",
            ["plots/a.topdown.png", "plots/b.power-vs-frequency.png"], [], True, True)
        for argv in argvs:
            self.assertIn("--all-models", argv)
            self.assertIn("--critique", argv)

    def test_no_flags_when_nothing_selected(self):
        argvs = server.build_vision_analyze_argvs(
            "/repo/wspy-analyze", "/repo/web/runs/r1", ["plots/a.topdown.png"], [], False, False)
        self.assertNotIn("--all-models", argvs[0])
        self.assertNotIn("--critique", argvs[0])
        self.assertNotIn("--model", argvs[0])


class RenderVisionCardTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        os.mkdir(os.path.join(self.tmpdir, "plots"))

    def _touch_plot(self, name):
        _touch(os.path.join(self.tmpdir, "plots", name))

    def test_offers_a_checkbox_per_registered_plot_present(self):
        self._touch_plot("amdtopdown.topdown.png")
        self._touch_plot("systemtime.system-cpu.png")
        self._touch_plot("systemtime.power-vs-frequency.png")
        html_out = server.render_vision_card(self.tmpdir, "s", "b", "r1")
        self.assertIn('value="plots/amdtopdown.topdown.png"', html_out)
        self.assertIn('value="plots/systemtime.system-cpu.png"', html_out)
        self.assertIn('value="plots/systemtime.power-vs-frequency.png"', html_out)

    def test_topdown_checked_by_default_others_not(self):
        self._touch_plot("amdtopdown.topdown.png")
        self._touch_plot("systemtime.system-cpu.png")
        html_out = server.render_vision_card(self.tmpdir, "s", "b", "r1")
        topdown_tag = html_out[html_out.index('value="plots/amdtopdown.topdown.png"') - 80:
                                html_out.index('value="plots/amdtopdown.topdown.png"') + 80]
        cpu_tag = html_out[html_out.index('value="plots/systemtime.system-cpu.png"') - 80:
                            html_out.index('value="plots/systemtime.system-cpu.png"') + 80]
        self.assertIn("checked", topdown_tag)
        self.assertNotIn("checked", cpu_tag)

    def test_unregistered_plot_kind_gets_no_checkbox(self):
        # A plot template with no dedicated vision prompt (not in
        # joblib.VISION_PLOT_KINDS) -- e.g. temp-vs-frequency -- must not
        # appear as a checkbox, since wspy-analyze --image would hard-error
        # resolving a template for it (default_vision_template_path()).
        self._touch_plot("systemtime.temp-vs-frequency.png")
        html_out = server.render_vision_card(self.tmpdir, "s", "b", "r1")
        self.assertNotIn("temp-vs-frequency", html_out)

    def test_no_registered_plots_shows_fallback_message(self):
        self._touch_plot("systemtime.temp-vs-frequency.png")
        html_out = server.render_vision_card(self.tmpdir, "s", "b", "r1")
        self.assertIn("No vision-analysis-enabled plot", html_out)

    def test_multiple_matches_for_same_kind_get_one_checkbox_each(self):
        self._touch_plot("amdtopdown.topdown.png")
        self._touch_plot("counters.topdown.png")
        html_out = server.render_vision_card(self.tmpdir, "s", "b", "r1")
        self.assertIn('value="plots/amdtopdown.topdown.png"', html_out)
        self.assertIn('value="plots/counters.topdown.png"', html_out)


if __name__ == "__main__":
    unittest.main()
