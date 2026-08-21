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

# A real topdown.c print_topdown() line shape, reused from web/test_counter_text.py's own fixture --
# 31509439317734 is the raw accumulated backend-slot count (the line's primary value), 24.4 is the
# real backend_pct/"backend" CSV-column percentage (the second, parenthetical comment number). Needs
# a "##### pass N" line of its own, or classify_counter_text() won't recognize this block as
# "counters" shape at all and recover_machine_metrics_from_wordpress() would skip it outright.
TOPDOWN_COUNTERS_TXT = ("##### pass  0 (mask 0x1) #####\n"
                         "backend              31509439317734 # 17.2% (24.4%)\n")


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

    def test_backend_reports_the_percentage_not_the_raw_slot_count(self):
        # Found live 2026-08-07 against a real reference-matrix page: the "backend" row showed
        # 95578418927389 (n=1)* -- a raw accumulated slot count, not the percentage local wspy-store
        # runs always show under that same column. Root cause: recover_machine_metrics_from_wordpress()
        # kept the line's raw primary value under the bare label "backend" (parse_counter_text()'s own
        # output), and extract_derived_ratios() only ever emitted the correctly-scaled percentage under
        # a *different* name ("backend_pct", needed separately for wspy-archetype --run-guest), so
        # nothing ever overwrote the raw value under "backend" itself.
        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content",
                   return_value=preformatted_page(TOPDOWN_COUNTERS_TXT)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(float(by_metric["backend"]["mean"]), 24.4)
        # The "_pct" name wspy-archetype --run-guest needs must still carry the same correct value --
        # this fix is additive, not a rename, so that consumer keeps working too.
        self.assertEqual(float(by_metric["backend_pct"]["mean"]), 24.4)

    def test_icache_reports_the_percentage_not_the_raw_access_count(self):
        # Same class of bug as backend/frontend above, reported separately (2026-08-07) against the
        # same real reference-matrix page after the backend/frontend fix landed: "icache" (AMD
        # --topdown-optlb) still showed a raw access count instead of the real miss-rate percentage.
        # doc/METRICS.md already documented this exact trap ("don't be misled by the name match") --
        # the block's own separate "icache" line (not "icache miss") carries the raw count as its
        # primary value.
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "instructions         54412256231580 # 2.38 IPC\n"
                "icache               1196643001389   # 260.598 icache per 1000 inst\n"
                "icache miss          1196643001389   # 8.4% icache miss rate\n")

        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=preformatted_page(text)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(float(by_metric["icache"]["mean"]), 8.4)
        self.assertEqual(float(by_metric["icache_miss_pct"]["mean"]), 8.4)

    def test_opcache_miss_reports_the_percentage_not_the_raw_access_count(self):
        # Reported live (2026-08-07) after the icache fix above landed: "opcache miss" on a real
        # reference-matrix page showed 1806064670420.0 (n=1)* -- reused here verbatim. Root cause
        # was actually a mistake in an earlier version of *this* fix: "opcache"/"opcache miss" is a
        # genuine same-label collision in wspy's own C code between print_topdown_op() (AMD-specific
        # bare "opcache") and print_cache()/print_opcache() (cross-vendor "opcache miss") -- unlike
        # icache, which has no such collision. Targeting the bare "opcache" name unconditionally
        # (icache's own fix pattern) silently broke this, the more common, cross-vendor case.
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "instructions         54412256231580 # 2.38 IPC\n"
                "opcache              1234567          # 4.500 opcache per 1000 inst\n"
                "opcache miss         1806064670420    # 3.70% opcache miss\n")

        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=preformatted_page(text)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(float(by_metric["opcache miss"]["mean"]), 3.7)
        # The bare "opcache" name is deliberately never *targeted* by the derived-ratio fix (the
        # collision means there's no reliable value to put there) -- its own line's raw primary value
        # still comes through untouched, same as "instructions"/"cpu-cycles" do. That's fine: it's an
        # honest raw count, not a wrong one under a misleading name, and stays correctly classified
        # as raw for the reference matrix's own grouping (metric_col_kind() in
        # publish_reference_matrix.py, unaffected by this fix).
        self.assertEqual(float(by_metric["opcache"]["mean"]), 1234567.0)

    def test_itlb_dtlb_generic_and_ctxswitch_rate_recovered(self):
        # Issue #281's two scoring-impact findings from the broader item-24 sweep that followed #278.
        # itlb_generic_miss_pct/dtlb_generic_miss_pct feed archetype.c's memory_attribution_locus axis;
        # ctxswitch_rate is a real, already-promoted run_features value. Neither was reachable before
        # this fix -- the generic iTLB/dTLB miss lines slugified to the wrong name, and ctxswitch_rate
        # (like fault_rate before it) has no single line's comment to derive it from at all.
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "elapsed              300.749\n"
                "nvcsw                1000                     # 80.00%\n"
                "nivcsw               250                      # 20.00%\n"
                "iTLB                 500             # 5.000 iTLB per 1000 inst\n"
                "iTLB miss            25              # 5.00% iTLB miss\n"
                "dTLB                 500             # 5.000 dTLB per 1000 inst\n"
                "dTLB miss            10              # 2.00% dTLB miss\n")

        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=preformatted_page(text)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(float(by_metric["itlb_generic_miss_pct"]["mean"]), 5.0)
        self.assertEqual(float(by_metric["dtlb_generic_miss_pct"]["mean"]), 2.0)
        self.assertAlmostEqual(float(by_metric["ctxswitch_rate"]["mean"]), (1000 + 250) / 300.749)

    def test_float_reports_the_real_percentage_not_the_per_mille_comment(self):
        # Issue #278's first sub-gap, filed live from compiler-flag-miner (a WordPress-anonymous
        # reference-matrix client that hit this scoring vectorization_density for a WordPress-recovered
        # machine): print_float()'s own PRINT_NORMAL line prints a per-1000-inst density ("4.323 float
        # per 1000 inst" on the block's "instructions" line), but store.c's real float_pct feature
        # (SIMPLE_METRIC_FEATURES, sourced from the CSV "float" column's *100.0 formula) is the same
        # ratio at percent scale -- ten times the printed comment's number. Before this fix,
        # extract_derived_ratios() only ever produced "float_per_1000_inst" (a display-only slug, per
        # web/joblib.py's resolve_column_group()), so run_snapshot_apply_feature() never saw a
        # "float_pct" it recognized at all.
        text = ("##### pass  0 (mask 0x800) #####################\n"
                "instructions         54412256231580 # 4.323 float per 1000 inst\n")

        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=preformatted_page(text)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertEqual(float(by_metric["float_pct"]["mean"]), 0.4323)
        # The original per-1000-inst display name must survive alongside it, unconverted.
        self.assertEqual(float(by_metric["float_per_1000_inst"]["mean"]), 4.323)

    def test_fault_rate_recovered_from_elapsed_minflt_majflt(self):
        # Issue #278's second sub-gap, filed alongside float_pct from the same compiler-flag-miner
        # gap report: fault_rate (needed for wspy-archetype --run-guest's allocation_pressure axis)
        # is store.c's (minflt+majflt)/elapsed_seconds, combined from three separate lines' own raw
        # primary values -- unlike every other item-24 case, it's not embedded in any single line's
        # own trailing comment. Real captured values from the issue's own mvermeulen.org/workload
        # 706.stockfish_r sample.
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "elapsed              300.749\n"
                "minflt               32805293       # 109078.75/sec\n"
                "majflt               23             # 0.08/sec\n")

        def fake_list_child_pages(site_url, username, app_password, parent):
            return [{"id": 101, "slug": "run-1", "date": "2026-08-01T00:00:00"}]

        with patch("server.wp_client.find_page", side_effect=self._walk_pages()), \
             patch("server.wp_client.list_child_pages", side_effect=fake_list_child_pages), \
             patch("server.wp_client.fetch_page_raw_content", return_value=preformatted_page(text)):
            rows = server.recover_machine_metrics_from_wordpress(
                FAKE_WP_CFG, "phoronix", "coremark", "default", "amd-395")

        by_metric = {r["metric"]: r for r in rows}
        self.assertAlmostEqual(float(by_metric["fault_rate"]["mean"]), (32805293 + 23) / 300.749)
        # The raw primary values are untouched, same "additive, not a rename" contract as every other
        # item-24 fix.
        self.assertEqual(float(by_metric["minflt"]["mean"]), 32805293.0)
        self.assertEqual(float(by_metric["majflt"]["mean"]), 23.0)

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
