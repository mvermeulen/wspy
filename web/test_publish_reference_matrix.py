#!/usr/bin/env python3
"""
web/test_publish_reference_matrix.py -- unit tests for scripts/publish_reference_matrix.py's table-
interactivity pieces (metric_col_kind()'s raw/ratio classification, load_metric_descriptions()'s
doc/METRICS.md parsing, and build_machine_page()'s data-col-kind/tooltip markup) -- the Python half of
the reference-matrix search/sort/column-toggle toolbar; scripts/wp-refmatrix-assets.php (the WordPress
plugin that actually renders the toolbar) has no Python-testable logic of its own. Not wired into
make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention as web/test_wp_client.py -- run standalone:

    python3 web/test_publish_reference_matrix.py
"""
import os
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
sys.path.insert(0, os.path.join(REPO_ROOT, "web"))
import publish_reference_matrix as prm  # noqa: E402


class MetricColKindTest(unittest.TestCase):
    def test_known_raw_counts_hidden_by_default(self):
        for name in ("context switches", "page faults", "maxrss", "pkg_joules", "float"):
            self.assertEqual(prm.metric_col_kind(name), "raw", name)

    def test_known_ratios_visible_by_default(self):
        for name in ("ipc", "retire", "l1_bound", "l2miss", "branch miss", "bandwidth"):
            self.assertEqual(prm.metric_col_kind(name), "ratio", name)

    def test_unclassified_metric_defaults_to_visible(self):
        # Anything not explicitly curated in RAW_COUNT_METRICS must default to "ratio" (visible) --
        # silently hiding a future metric nobody classified yet is the worse failure mode.
        self.assertEqual(prm.metric_col_kind("some_brand_new_metric_nobody_has_seen"), "ratio")


class LoadMetricDescriptionsSyntheticTest(unittest.TestCase):
    """Exact-match assertions against a small synthetic doc/METRICS.md-shaped file, so these don't
    depend on the real file's prose staying byte-identical."""

    def _write(self, text):
        f = tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False, dir=tempfile.gettempdir())
        f.write(text)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_single_name_bullet(self):
        path = self._write(
            "## Section\n\n"
            "- **ipc** — `[feature]` instructions / cpu-cycles, a comparable ratio.\n"
        )
        prm.load_metric_descriptions.cache_clear()
        descs = prm.load_metric_descriptions(path)
        self.assertIn("ipc", descs)
        self.assertIn("instructions / cpu-cycles", descs["ipc"])
        self.assertNotIn("[feature]", descs["ipc"])
        self.assertNotIn("`", descs["ipc"])

    def test_multi_name_bullet_shares_description(self):
        path = self._write(
            "- **l1_bound**, **l2_bound**, **l3_bound** — `[raw]`, each as % of cache-level stalls.\n"
        )
        prm.load_metric_descriptions.cache_clear()
        descs = prm.load_metric_descriptions(path)
        self.assertEqual(descs["l1_bound"], descs["l2_bound"])
        self.assertEqual(descs["l2_bound"], descs["l3_bound"])
        self.assertIn("cache-level stalls", descs["l1_bound"])

    def test_wrapped_continuation_line_is_rejoined(self):
        path = self._write(
            "- **retire** — `[feature]` (`retire_pct`) retiring_slots / slots_no_contention * 100.\n"
            "  Fraction of pipeline slots that produced useful retired work.\n"
        )
        prm.load_metric_descriptions.cache_clear()
        descs = prm.load_metric_descriptions(path)
        # The description must come from the joined bullet, not be truncated at the first physical
        # line's wrap point.
        self.assertIn("retiring_slots / slots_no_contention", descs["retire"])

    def test_blank_line_ends_a_bullet(self):
        path = self._write(
            "- **ipc** — `[feature]` instructions / cpu-cycles.\n"
            "\n"
            "Some unrelated prose paragraph that must not get glued onto ipc's description.\n"
        )
        prm.load_metric_descriptions.cache_clear()
        descs = prm.load_metric_descriptions(path)
        self.assertNotIn("unrelated prose", descs["ipc"])

    def test_missing_file_returns_empty_dict(self):
        prm.load_metric_descriptions.cache_clear()
        self.assertEqual(prm.load_metric_descriptions("/does/not/exist.md"), {})
        prm.load_metric_descriptions.cache_clear()


class LoadMetricDescriptionsRealDocTest(unittest.TestCase):
    """Loose sanity checks against the real doc/METRICS.md -- this is the single source of truth the
    tooltips actually ship from, so it's worth confirming the parser gets *something* real out of it,
    without pinning to exact prose that's free to be edited."""

    def setUp(self):
        prm.load_metric_descriptions.cache_clear()
        self.descs = prm.load_metric_descriptions()
        self.addCleanup(prm.load_metric_descriptions.cache_clear)

    def test_finds_a_substantial_number_of_metrics(self):
        self.assertGreater(len(self.descs), 50)

    def test_known_metrics_present_with_nonempty_description(self):
        for name in ("ipc", "retire", "l2miss", "bandwidth", "context switches"):
            self.assertIn(name, self.descs)
            self.assertTrue(self.descs[name])

    def test_descriptions_carry_no_markdown_bold_or_backticks(self):
        for desc in self.descs.values():
            self.assertNotIn("**", desc)
            self.assertNotIn("`", desc)


class BuildMachinePageTest(unittest.TestCase):
    def test_data_col_kind_and_tooltip_present(self):
        entries = [
            ("coremark", "tp1", [
                {"metric": "ipc", "mean": 1.23, "n": 3, "verdict": "PASS"},
                {"metric": "context switches", "mean": 4021, "n": 3, "verdict": "PASS"},
            ], "local"),
        ]
        _, wp_html = prm.build_machine_page("cpu2026", "amd-370-64gb", entries)
        self.assertIn('class="wspy-refmatrix"', wp_html)
        self.assertIn('data-col-kind="ratio"', wp_html)
        self.assertIn('data-col-kind="raw"', wp_html)
        self.assertIn('class="wspy-info"', wp_html)

    def test_no_data_returns_none(self):
        self.assertEqual(prm.build_machine_page("cpu2026", "amd-370-64gb", []), (None, None))


if __name__ == "__main__":
    unittest.main()
