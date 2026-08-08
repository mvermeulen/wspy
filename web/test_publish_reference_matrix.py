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


class MetricColGroupTest(unittest.TestCase):
    def test_delegates_to_joblib_resolve_column_group_first(self):
        # ipc/l2miss/branch miss are all resolved by joblib.resolve_column_group() itself --
        # metric_col_group() must not shadow that with its own supplementary table.
        self.assertEqual(prm.metric_col_group("ipc"), "ipc")
        # l2miss resolves to joblib's own "cache2" token, then GROUP_ALIASES merges it into "cache"
        # alongside l3miss -- author's call (2026-08-08): L2/L3 cache are useful to browse together.
        self.assertEqual(prm.metric_col_group("l2miss"), "cache")
        self.assertEqual(prm.metric_col_group("l3miss"), "cache")
        self.assertEqual(prm.metric_col_group("branch miss"), "branch")

    def test_supplementary_table_covers_what_joblib_does_not(self):
        # rusage/IBS/GPU/coverage columns aren't in joblib's COLUMN_TO_GROUP (they're not gated by a
        # --counters flag), so metric_col_group() must fill them in itself rather than falling
        # through to "other".
        self.assertEqual(prm.metric_col_group("maxrss"), "process")
        self.assertEqual(prm.metric_col_group("ibs_fetch"), "ibs")
        self.assertEqual(prm.metric_col_group("nv_gpu_busy"), "gpu")
        self.assertEqual(prm.metric_col_group("counters_measured"), "coverage")

    def test_unclassified_metric_falls_back_to_other(self):
        self.assertEqual(prm.metric_col_group("some_brand_new_metric_nobody_has_seen"), "other")

    def test_topdown_variants_all_merge_into_one_group(self):
        # "backend" (main L1 split), l1_bound (--topdown-backend deep-dive), and icache/itlb1/opcache
        # (AMD --topdown-frontend/--topdown-optlb deep-dives) are four different joblib.py group
        # tokens (topdown/topdown-backend/topdown-frontend/topdown-optlb) -- all must collapse to the
        # single "topdown" bucket so the plugin's Columns panel doesn't split one mental model across
        # four near-identical-looking checkboxes.
        for name in ("backend", "l1_bound", "icache", "itlb1", "opcache", "dtlb1"):
            self.assertEqual(prm.metric_col_group(name), "topdown", name)

    def test_column_group_order_covers_every_group_metric_col_group_can_return(self):
        # A group token missing from COLUMN_GROUP_ORDER doesn't break anything (the plugin sorts it
        # in right before "other"), but it does mean the priority list has drifted out of sync with
        # what metric_col_group() can actually produce -- catch that here rather than live in
        # production.
        descs = prm.load_metric_descriptions()
        produced = {prm.metric_col_group(name) for name in descs}
        unlisted = produced - set(prm.COLUMN_GROUP_ORDER)
        self.assertEqual(unlisted, set(), "groups missing from COLUMN_GROUP_ORDER: %s" % unlisted)

    def test_opcache_group_folded_into_other(self):
        # Author's own call (2026-08-07): a single-metric "opcache" group (just the cross-vendor
        # "opcache miss" column) isn't worth its own Columns-panel checkbox.
        self.assertEqual(prm.metric_col_group("opcache miss"), "other")

    def test_l2_and_l3_cache_merged_into_one_group(self):
        # Author's own call (2026-08-08): L2 and L3 cache miss rate are useful to browse together
        # rather than as two separate single-metric-ish checkboxes.
        self.assertEqual(prm.metric_col_group("l2miss"), prm.metric_col_group("l3miss"))
        self.assertEqual(prm.metric_col_group("l2miss"), "cache")

    def test_wordpress_recovery_derived_rates_get_a_real_group(self):
        # These never appear as real local CSV columns -- they only exist via web/counter_text.py's
        # extract_derived_ratios() generic-slug fallback (per-1000-inst densities) or its
        # archetype-facing "_pct" names -- but they're genuine comparable ratios, found auditing a
        # real reference-matrix "Other" dump that used to swallow all of them (2026-08-07).
        for name, expected_group in (
            ("ghz", "ipc"), ("ipc_mean", "ipc"),
            ("backend_pct", "topdown"), ("icache_per_1000_inst", "topdown"),
            ("itlb_miss_per1k", "topdown"), ("tlb_flush_per_1000_inst", "topdown"),
            ("branch_mispredict_pct", "branch"), ("branches_per_1000_inst", "branch"),
            ("l2_miss_pct", "cache"), ("l3_access_per_1000_inst", "cache"),
            ("l1_itlb_per_1000_inst", "topdown"), ("l1_dtlb_per_1000_inst", "topdown"),
            ("avx_128_per_1000_inst", "float"), ("float_per_1000_inst", "float"),
            ("ibs_dc_miss_pct", "ibs"),
        ):
            self.assertEqual(prm.metric_col_group(name), expected_group, name)
            self.assertEqual(prm.metric_col_kind(name), "ratio", name)

    def test_wordpress_recovery_raw_leaks_default_hidden(self):
        # Every one of these is a real primary value web/counter_text.py's parse_counter_text()
        # keeps under a line's own on-screen label, never a real local CSV column -- the line's
        # actually-comparable ratio already has its own correctly-classified name elsewhere. Found
        # auditing a real reference-matrix "Other" dump (2026-08-07): with no RAW_COUNT_METRICS
        # entry, these defaulted to "ratio" (visible), so "Other" never defaulted hidden.
        for name in ("instructions", "cpu-cycles", "slots", "retiring", "speculation",
                     "smt-contention", "branch misses", "icache miss", "l3 miss",
                     "l1 iTLB miss", "l2 dTLB miss", "-- ucode", "-- latency",
                     "float 128", "float scalar"):
            self.assertEqual(prm.metric_col_kind(name), "raw", name)
            self.assertEqual(prm.metric_col_group(name), "other", name)


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

    def test_labeled_multi_name_bullet_still_parses(self):
        # doc/METRICS.md's branch-prediction section uses "- AMD extras: **name**, **name** -- ..."
        # -- a leading "AMD extras: "/"ARM extras: " label before the bold names, which used to make
        # the whole bullet unparseable (regex required bold names immediately after "- "), silently
        # dropping tooltips for every name in these bullets. Found live 2026-08-08.
        path = self._write(
            "- AMD extras: **near_return**, **near_return_mispredicted** — `[raw]` raw counts.\n"
        )
        prm.load_metric_descriptions.cache_clear()
        descs = prm.load_metric_descriptions(path)
        self.assertEqual(descs["near_return"], descs["near_return_mispredicted"])
        self.assertIn("raw counts", descs["near_return"])

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

    def test_amd_arm_branch_extras_present(self):
        # These live in "- AMD extras: **name**, ... -- ..."/"- ARM extras: **name**, ... -- ..."
        # bullets -- previously unparseable, silently dropping tooltips for all seven names.
        for name in ("near_return", "near_return_mispredicted", "indirect_branch_mispredicted",
                     "br_immed_retired", "br_return_retired", "br_pred", "br_mis_pred"):
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
        # data-desc, not title -- a bare title="..." on <span> gets silently stripped by
        # WordPress's post-content sanitizer for the wspy service account (no unfiltered_html);
        # data-* attributes survive, and scripts/wp-refmatrix-assets.php applies the real title
        # client-side instead. See th()'s own comment in publish_reference_matrix.py.
        self.assertIn('data-desc="', wp_html)
        self.assertNotIn('title="', wp_html)
        self.assertIn('data-col-group="ipc"', wp_html)
        self.assertIn('data-col-group="software"', wp_html)  # "context switches"
        # data-metric so the plugin's per-column checkboxes can label themselves without scraping
        # the info-span text out of the <th>.
        self.assertIn('data-metric="ipc"', wp_html)
        self.assertIn('data-metric="context switches"', wp_html)
        # data-group-order on the <table> itself -- single source of truth for the plugin's Columns-
        # panel ordering, not a second hand-maintained list in wp-refmatrix-assets.php.
        self.assertIn('data-group-order="%s"' % ",".join(prm.COLUMN_GROUP_ORDER), wp_html)

    def test_no_data_returns_none(self):
        self.assertEqual(prm.build_machine_page("cpu2026", "amd-370-64gb", []), (None, None))


if __name__ == "__main__":
    unittest.main()
