#!/usr/bin/env python3
"""
web/test_counter_text.py -- unit tests for web/counter_text.py's parser (INVESTIGATION.md 4.3 item
21). Fixtures below are trimmed/verbatim excerpts of real counters.txt/ibs.txt output (captured from
an actual cpu2026 run) rather than invented samples, since this format's real quirks (comment shape,
percent suffixes, the ibs.txt sub-list, the non-numeric "counter coverage" trailer) are exactly what
matters here. Not wired into make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by
the C toolchain's test targets" convention as the rest of web/test_*.py -- run standalone:

    python3 web/test_counter_text.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import counter_text

COUNTERS_TXT = """\
elapsed              290.472
on_cpu               0.884          # 21.21 / 24 cores
utime                6157.406
nswap                0              # 0.00/sec
##### pass  0 (mask 0x1) #####################
cpu-cycles           22822740203467 # 3.27 GHz
instructions         54412256231580 # 2.38 IPC
##### pass  1 (mask 0x4) #####################
retiring             52444026804212 # 28.7% (40.6%) low-confidence(50%)
-- ucode             1706753854     #     0.0%
frontend             36264644519310 # 19.9% (28.1%) high
backend              31509439317734 # 17.2% (24.4%)
speculation          8986175920281  #  4.9% ( 7.0%) low
smt-contention       53416184858519 # 29.2% ( 0.0%)
l3 miss              0              # -nan% l3 miss
l2 miss              9819149704     # 3.12% l2 miss
icache miss          1196643001389  #  8.4% icache miss rate
branch misses        116367530656   # 2.65% branch miss
l2 iTLB miss         0               # 0.000 L2 iTLB per 1000 inst
l2 dTLB miss         88087467        # 0.002 L2 dTLB per 1000 inst
L1-dcache miss       500000          # 1.23% L1-dcache miss
counter coverage     57/57 measured
"""

IBS_TXT = """\
ibs_sample_fetch_count     5390
ibs_sample_ic_miss_rate      4.5%
ibs_sample_brn_misp_rate        1.8%          # of 111 branch-retiring ops
ibs_sample_data_src_breakdown (scheme: zen4_ibs_extensions):
  Local L3 or other L1/L2 in CCX                   1.4%
  DRAM                                            58.7%
# note: --ibs-sample rates are computed once at end-of-run
counter coverage     2/2 measured
"""


class ParseCounterTextTest(unittest.TestCase):
    def test_plain_value_no_comment(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        elapsed = next(r for r in records if r["metric"] == "elapsed")
        self.assertEqual(elapsed, {"metric": "elapsed", "value": 290.472, "is_percent": False,
                                    "comment": None})

    def test_value_with_comment(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        on_cpu = next(r for r in records if r["metric"] == "on_cpu")
        self.assertEqual(on_cpu["value"], 0.884)
        self.assertEqual(on_cpu["comment"], "21.21 / 24 cores")
        self.assertFalse(on_cpu["is_percent"])

    def test_dashed_indented_label_kept_verbatim(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        ucode = next(r for r in records if r["metric"] == "-- ucode")
        self.assertEqual(ucode["value"], 1706753854)

    def test_pass_separator_lines_skipped(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        self.assertFalse(any("#####" in r["metric"] for r in records))

    def test_non_numeric_trailer_skipped(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        self.assertFalse(any(r["metric"] == "counter coverage" for r in records))

    def test_nan_comment_value_still_parses(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        l3_miss = next(r for r in records if r["metric"] == "l3 miss")
        self.assertEqual(l3_miss["value"], 0.0)
        self.assertEqual(l3_miss["comment"], "-nan% l3 miss")

    def test_percent_suffixed_value_no_comment(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        rate = next(r for r in records if r["metric"] == "ibs_sample_ic_miss_rate")
        self.assertEqual(rate["value"], 4.5)
        self.assertTrue(rate["is_percent"])
        self.assertIsNone(rate["comment"])

    def test_percent_value_with_comment(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        rate = next(r for r in records if r["metric"] == "ibs_sample_brn_misp_rate")
        self.assertEqual(rate["value"], 1.8)
        self.assertEqual(rate["comment"], "of 111 branch-retiring ops")

    def test_data_src_breakdown_header_and_children_both_skipped(self):
        # Fixed 2026-08-07: this indented sub-list used to parse like any other line, leaking
        # "DRAM"/"Local L3 or other L1/L2 in CCX"/etc. through as if they were stable, comparable
        # metric names -- doc/METRICS.md's own design note says this histogram is "never emitted as
        # CSV -- cross-scheme category names aren't stable enough to be a schema'd column," found
        # live in a real reference-matrix "Other" dump. Both the header and every indented row under
        # it must now be excluded.
        records = counter_text.parse_counter_text(IBS_TXT)
        metrics = {r["metric"]: r for r in records}
        self.assertNotIn("ibs_sample_data_src_breakdown (scheme: zen4_ibs_extensions)", metrics)
        self.assertNotIn("Local L3 or other L1/L2 in CCX", metrics)
        self.assertNotIn("DRAM", metrics)

    def test_line_after_data_src_breakdown_parses_normally(self):
        # The exclusion must end as soon as a non-indented line appears -- it shouldn't swallow
        # everything for the rest of the block.
        text = IBS_TXT + "ibs_sample_lost      0\n"
        records = counter_text.parse_counter_text(text)
        metrics = {r["metric"]: r for r in records}
        self.assertEqual(metrics["ibs_sample_lost"]["value"], 0.0)

    def test_full_line_comment_skipped(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        self.assertFalse(any(r["metric"].startswith("#") for r in records))

    def test_empty_text_returns_empty_list(self):
        self.assertEqual(counter_text.parse_counter_text(""), [])


class ParseCommentRatioTest(unittest.TestCase):
    def test_ipc_style(self):
        self.assertEqual(counter_text.parse_comment_ratio("2.38 IPC"), ("ipc", 2.38, False))

    def test_ipc_with_trailing_high_qualifier_still_slugs_to_plain_ipc(self):
        # topdown.c's print_ipc() appends " high"/" low" when a threshold is crossed -- the metric
        # name must stay stable regardless, or the same ratio would fragment into "ipc"/"ipc_high"
        # depending on which side of the threshold a given run happened to land on.
        self.assertEqual(counter_text.parse_comment_ratio("3.50 IPC high"), ("ipc", 3.50, False))
        self.assertEqual(counter_text.parse_comment_ratio("0.50 IPC low"), ("ipc", 0.50, False))

    def test_percent_with_description(self):
        self.assertEqual(counter_text.parse_comment_ratio("8.4% icache miss rate"),
                          ("icache_miss_rate", 8.4, True))

    def test_per_1000_inst_rate(self):
        self.assertEqual(counter_text.parse_comment_ratio("260.598 icache per 1000 inst"),
                          ("icache_per_1000_inst", 260.598, False))

    def test_ghz_annotation(self):
        self.assertEqual(counter_text.parse_comment_ratio("3.27 GHz"), ("ghz", 3.27, False))

    def test_none_for_empty_comment(self):
        self.assertIsNone(counter_text.parse_comment_ratio(None))
        self.assertIsNone(counter_text.parse_comment_ratio(""))

    def test_none_for_bare_percentage_with_no_description(self):
        self.assertIsNone(counter_text.parse_comment_ratio("0.0%"))

    def test_none_for_two_percentage_topdown_shape(self):
        # Starts with a percentage but the next thing isn't a plain description (it's a second
        # parenthesized percentage) -- must NOT match here; TOPDOWN_SECOND_PERCENT_LABELS handles
        # this shape instead, in extract_derived_ratios().
        self.assertIsNone(counter_text.parse_comment_ratio("28.7% (40.6%) low-confidence(50%)"))

    def test_none_for_non_numeric_comment(self):
        self.assertIsNone(counter_text.parse_comment_ratio("of 111 branch-retiring ops"))
        self.assertIsNone(counter_text.parse_comment_ratio("21.21 / 24 cores"))


class ExtractDerivedRatiosTest(unittest.TestCase):
    def test_ipc_recovered_and_renamed_to_store_feature_name(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        # store.c's SIMPLE_METRIC_FEATURES calls this "ipc_mean", not the bare "ipc" a plain slugify
        # of the "IPC" comment text would produce.
        self.assertNotIn("ipc", derived)
        self.assertEqual(derived["ipc_mean"]["value"], 2.38)
        self.assertFalse(derived["ipc_mean"]["is_percent"])
        self.assertIsNone(derived["ipc_mean"]["comment"])

    def test_topdown_l1_takes_second_percentage_not_first(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["retire_pct"]["value"], 40.6)   # not 28.7 (the first percentage)
        self.assertEqual(derived["frontend_pct"]["value"], 28.1)  # despite trailing " high"
        self.assertEqual(derived["backend_pct"]["value"], 24.4)   # no qualifier at all
        self.assertEqual(derived["speculate_pct"]["value"], 7.0)  # despite trailing " low"

    def test_topdown_l1_also_emitted_under_the_real_csv_column_name(self):
        # Found live 2026-08-07: recover_machine_metrics_from_wordpress() only ever got the
        # "_pct"-suffixed name (needed for wspy-archetype --run-guest's run_snapshot_apply_feature(),
        # which specifically looks for retire_pct/frontend_pct/backend_pct/speculate_pct) -- the
        # reference matrix's own local-store convention uses the bare CSV column name instead
        # (topdown.c's print_topdown() PRINT_CSV branch: retire/frontend/backend/speculate, no "_pct"
        # suffix), so a WordPress-recovered "backend"/"frontend" cell fell back to that label's raw
        # primary value (a giant slot count) instead of ever getting the percentage under that name.
        # Both names must carry the identical value now.
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["retire"]["value"], derived["retire_pct"]["value"])
        self.assertEqual(derived["frontend"]["value"], derived["frontend_pct"]["value"])
        self.assertEqual(derived["backend"]["value"], derived["backend_pct"]["value"])
        self.assertEqual(derived["speculate"]["value"], derived["speculate_pct"]["value"])
        for name in ("retire", "frontend", "backend", "speculate"):
            self.assertTrue(derived[name]["is_percent"])
            self.assertIsNone(derived[name]["comment"])

    def test_icache_also_emitted_under_the_real_csv_column_name(self):
        # Same class of bug as the topdown L1 case above, different mechanism: doc/METRICS.md's own
        # "don't be misled by the name match" warning -- the real "icache" CSV column
        # (icache_miss/icache_access*100, topdown.c print_topdown_fe()) is a percentage, but the
        # block's own separate line literally labeled "icache" (not "icache miss") carries a
        # completely different value as its primary operand (the raw access count). Confirmed live
        # 2026-08-07 against a real reference-matrix page. icache_miss_pct (the archetype-facing
        # name) must be unaffected.
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["icache"]["value"], 8.4)
        self.assertEqual(derived["icache"]["value"], derived["icache_miss_pct"]["value"])

    def test_opcache_miss_overwrites_its_own_raw_primary_value(self):
        # "opcache"/"opcache miss" is a genuine same-label collision between two different real CSV
        # columns in wspy's own C code (print_topdown_op()'s AMD-specific bare "opcache" vs.
        # print_cache()/print_opcache()'s cross-vendor "opcache miss") -- unlike icache, which has no
        # such collision (the cross-vendor pair is "L1-icache"/"L1-icache miss", a different string).
        # Found live 2026-08-07: an earlier version of this fix targeted the bare "opcache" name
        # unconditionally, which silently broke the (more common) cross-vendor case. The correct,
        # unambiguous fix targets "opcache miss" itself -- an identity mapping that overwrites that
        # line's own wrong raw primary value with its own comment's correct percentage.
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "instructions         54412256231580 # 2.38 IPC\n"
                "opcache              1234567         # 4.500 opcache per 1000 inst\n"
                "opcache miss         45678            # 3.70% opcache miss\n")  # print_cache()'s
        # own exact comment shape (no " rate" suffix, unlike the AMD-specific line's own wording).
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["opcache miss"]["value"], 3.7)
        self.assertNotIn("opcache", derived)  # deliberately never populated via this path

    def test_opcache_collision_still_yields_a_correct_percentage_not_the_raw_count(self):
        # If both groups are collected together, the block has two indistinguishable "opcache miss"
        # lines -- parse_counter_text() can't tell them apart from label text alone. This doesn't
        # assert *which* one wins (that's inherently ambiguous), only that the result is some real
        # percentage, never the multi-million-scale raw primary value either line's own operand
        # carries -- the actual bug reported live (a raw count of 1806064670420.0 showing up under
        # "opcache miss").
        text = ("##### pass  0 (mask 0x1) #####################\n"
                "instructions         54412256231580 # 2.38 IPC\n"
                "opcache              1234567         # 4.500 opcache per 1000 inst\n"
                "opcache miss         1806064670420   # 3.70% opcache miss\n"
                "##### pass  1 (mask 0x2) #####################\n"
                "opcache              9876543         # 6.100 opcache per 1000 inst\n"
                "opcache miss         55667788         # 1.20% opcache miss rate\n")
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertIn(derived["opcache miss"]["value"], (3.7, 1.2))

    def test_topdown_l2_and_contention_take_first_percentage(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["retire_ucode_pct"]["value"], 0.0)
        # smt-contention's own second, parenthesized number is a hardcoded literal per topdown.c's
        # own comment, not real data -- must take the first (29.2), not the fake "0.0". Renamed to
        # the real store.c feature name (archetype.c's run_snapshot_apply_feature() only recognizes
        # "smt_contention_pct", not the raw CSV column name "contention_pct").
        self.assertNotIn("contention_pct", derived)
        self.assertEqual(derived["smt_contention_pct"]["value"], 29.2)

    def test_ghz_still_generic_since_no_real_feature_exists_for_it(self):
        # GHz is documented [human-only] in doc/METRICS.md -- not a CSV column at all -- so there's
        # nothing to align it to; it keeps its plain slugified name.
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["ghz"]["value"], 3.27)

    def test_cache_and_branch_comments_renamed_to_store_feature_names(self):
        # INVESTIGATION.md 4.3 item 24's own audit: each of these confirmed directly against
        # store.c's SIMPLE_METRIC_FEATURES table and the topdown.c print function that emits the
        # comment, not guessed from the text alone.
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["dcache_miss_pct"]["value"], 1.23)
        self.assertEqual(derived["icache_miss_pct"]["value"], 8.4)
        self.assertEqual(derived["l2_miss_pct"]["value"], 3.12)
        self.assertEqual(derived["itlb_miss_per1k"]["value"], 0.0)
        self.assertEqual(derived["dtlb_miss_per1k"]["value"], 0.002)
        # branch_mispredict_pct is the one that actually feeds archetype.c's control_flow_style axis
        # (run_snapshot_apply_feature() only recognizes this exact name) -- confirmed this was
        # previously silently dropped as "branch_miss", an unrecognized name, before this fix.
        self.assertEqual(derived["branch_mispredict_pct"]["value"], 2.65)
        # "branch miss" (singular, no "es") is the real CSV column name -- must carry the identical
        # value alongside branch_mispredict_pct, same additive pattern as icache/topdown L1.
        self.assertEqual(derived["branch miss"]["value"], 2.65)
        for old_name in ("l1_dcache_miss", "icache_miss_rate", "l2_miss", "branch_miss",
                          "l2_itlb_per_1000_inst", "l2_dtlb_per_1000_inst"):
            self.assertNotIn(old_name, derived)

    def test_amd_l2_miss_from_l1_label_maps_to_same_real_feature(self):
        # AMD's own branch of print_l2cache() labels the identical l2miss ratio "l2 miss from l1"
        # instead of "l2 miss" (ARM/Intel) -- both must resolve to the same real feature name.
        text = "l2 miss from l1      9819149704     # 3.12% l2 miss\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["l2_miss_pct"]["value"], 3.12)

    def test_generic_itlb_dtlb_miss_renamed_to_real_feature_names(self):
        # issue #281: the generic cross-vendor "iTLB miss"/"dTLB miss" lines (print_itlb()/
        # print_dtlb(), any vendor) used to slugify to plain "itlb_miss"/"dtlb_miss" via the generic
        # tier -- a name archetype.c's run_snapshot_apply_feature() never recognized, so
        # memory_attribution_locus silently lost this signal for WordPress-recovered machines.
        text = ("iTLB                 500  # 5.000 iTLB per 1000 inst\n"
                "iTLB miss            25   # 5.00% iTLB miss\n"
                "dTLB                 500  # 5.000 dTLB per 1000 inst\n"
                "dTLB miss            10   # 2.00% dTLB miss\n")
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["itlb_generic_miss_pct"]["value"], 5.0)
        self.assertEqual(derived["dtlb_generic_miss_pct"]["value"], 2.0)
        self.assertNotIn("itlb_miss", derived)
        self.assertNotIn("dtlb_miss", derived)

    def test_generic_itlb_dtlb_miss_distinct_from_amd_l2_per1k_pair(self):
        # Confirms the new entries don't collide with the pre-existing AMD-only "l2 iTLB miss"/
        # "l2 dTLB miss" pair (itlb_miss_per1k/dtlb_miss_per1k) -- near-identical label text, real
        # store.c feature.
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["itlb_miss_per1k"]["value"], 0.0)
        self.assertEqual(derived["dtlb_miss_per1k"]["value"], 0.002)
        self.assertNotIn("itlb_generic_miss_pct", derived)  # COUNTERS_TXT has no generic tlb group
        self.assertNotIn("dtlb_generic_miss_pct", derived)

    def test_ctxswitch_rate_combines_three_separate_lines_own_primary_values(self):
        # issue #281: ctxswitch_rate = (nvcsw+nivcsw)/elapsed_seconds, store.c:1143 -- the same
        # cross-line shape as fault_rate (issue #278). nvcsw's/nivcsw's own comments are each switch
        # type's share of nvcsw+nivcsw's own sum (voluntary vs. involuntary split), not a rate, so
        # there's no comment to derive this from at all.
        text = "elapsed              300.749\nnvcsw                1000 # 80.00%\nnivcsw               250  # 20.00%\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertAlmostEqual(derived["ctxswitch_rate"]["value"], (1000 + 250) / 300.749)
        self.assertFalse(derived["ctxswitch_rate"]["is_percent"])

    def test_ctxswitch_rate_uses_whichever_switch_column_is_present(self):
        text = "elapsed              100.0\nnvcsw                50   # 100.00%\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["ctxswitch_rate"]["value"], 0.5)

    def test_ctxswitch_rate_absent_without_elapsed_or_without_any_switch_column(self):
        records = counter_text.parse_counter_text("nvcsw                50   # 100.00%\n")
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("ctxswitch_rate", derived_names)
        records = counter_text.parse_counter_text("elapsed              100.0\n")
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("ctxswitch_rate", derived_names)

    def test_float_line_yields_float_pct_at_percent_scale_not_per_mille(self):
        # issue #278: print_float()'s own PRINT_NORMAL line prints a per-1000-inst density ("4.323
        # float per 1000 inst"), but store.c's real float_pct feature (SIMPLE_METRIC_FEATURES, sourced
        # from the CSV "float" column's *100.0 formula) is the same ratio at percent scale -- ten
        # times the printed comment's number. A bare rename would have silently shipped 0.4323 under
        # the name archetype.c's vectorization_density axis expects a real percentage from (its real
        # CPU2026 thresholds run 0.6-34.0), so this must divide by 10, not just relabel.
        text = "instructions         54412256231580 # 4.323 float per 1000 inst\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["float_pct"]["value"], 0.4323)
        self.assertTrue(derived["float_pct"]["is_percent"])
        self.assertIsNone(derived["float_pct"]["comment"])
        # The original per-1000-inst record must survive alongside it, unconverted -- still needed by
        # web/joblib.py's resolve_column_group() (buckets it into the "float" report section).
        self.assertEqual(derived["float_per_1000_inst"]["value"], 4.323)
        self.assertFalse(derived["float_per_1000_inst"]["is_percent"])

    def test_float_width_breakdown_lines_stay_generic_not_converted(self):
        # The AVX-width breakdown lines ("float 512"/"float 256"/...) are a different, per-width
        # density with no real store.c feature of their own -- only the top "instructions" line's
        # aggregate density (float_per_1000_inst) maps to float_pct. These must not pick up a
        # spurious "_pct" conversion just for sharing the "per 1000 inst" comment shape.
        text = "float 512            1234567         # 0.023 AVX-512 per 1000 inst\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["avx_512_per_1000_inst"]["value"], 0.023)
        self.assertNotIn("float_pct", derived)

    def test_fault_rate_combines_three_separate_lines_own_primary_values(self):
        # issue #278's second sub-gap: fault_rate = (minflt+majflt)/elapsed_seconds, computed by
        # store.c from three separate raw metric_values columns, not from any single line's own
        # comment -- minflt's/majflt's own "X.XX/sec" comments don't even match
        # parse_comment_ratio()'s generic shape (no space before "/sec"). Real captured values from
        # the issue's own mvermeulen.org/workload 706.stockfish_r sample.
        text = ("elapsed              300.749\n"
                "minflt               32805293       # 109078.75/sec\n"
                "majflt               23             # 0.08/sec\n")
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertAlmostEqual(derived["fault_rate"]["value"], (32805293 + 23) / 300.749)
        self.assertFalse(derived["fault_rate"]["is_percent"])
        self.assertIsNone(derived["fault_rate"]["comment"])

    def test_fault_rate_uses_whichever_fault_column_is_present(self):
        # Mirrors store.c's own `have_elapsed && (have_minflt || have_majflt)` condition -- either
        # fault source alone is enough, missing one just contributes 0.
        text = "elapsed              100.0\nmajflt               50\n"
        records = counter_text.parse_counter_text(text)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["fault_rate"]["value"], 0.5)

    def test_fault_rate_absent_without_elapsed_or_without_any_fault_column(self):
        records = counter_text.parse_counter_text("minflt               100            # 1.00/sec\n")
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("fault_rate", derived_names)  # no "elapsed" line in this block
        records = counter_text.parse_counter_text("elapsed              100.0\n")
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("fault_rate", derived_names)  # no minflt/majflt line in this block

    def test_nan_and_non_numeric_comments_produce_no_derived_metric(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("l3_miss", derived_names)  # "-nan% l3 miss" -- not a real number
        self.assertNotIn("l3_miss_pct", derived_names)

    def test_ibs_txt_comment_still_extracted(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        # "of 111 branch-retiring ops" has no leading number -- nothing to derive from it, but this
        # must not crash, and the primary value (already a percent) is untouched by this function.
        self.assertNotIn("of_111_branch_retiring_ops", derived)

    def test_empty_records_returns_empty_list(self):
        self.assertEqual(counter_text.extract_derived_ratios([]), [])


class CanonicalMetricNameTest(unittest.TestCase):
    def test_ibs_sampling_rates_renamed_to_store_feature_names(self):
        # INVESTIGATION.md 4.3 item 24's audit: ibs.txt's own rates are primary values (is_percent=
        # True, not comment-derived), so parse_counter_text()/extract_derived_ratios() never touch
        # their names -- canonical_metric_name() is the rename step web/server.py applies instead.
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_dc_miss_rate"), "ibs_dc_miss_pct")
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_dram_rate"), "ibs_dram_pct")
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_dc_l1tlb_miss_rate"),
                          "ibs_dc_l1tlb_miss_pct")
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_dc_l2tlb_miss_rate"),
                          "ibs_dc_l2tlb_miss_pct")
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_remote_node_rate"),
                          "ibs_remote_node_pct")

    def test_onblock_renamed_to_real_rusage_column_name(self):
        # topdown.c's own human-mode label for this rusage field reads "onblock", but the real CSV
        # column (and store.c feature) is "oublock" (ru_oublock) -- doc/METRICS.md documents this
        # quirk explicitly. Found live 2026-08-07 auditing a real reference-matrix "Other" dump.
        self.assertEqual(counter_text.canonical_metric_name("onblock"), "oublock")

    def test_unmapped_name_returned_unchanged(self):
        self.assertEqual(counter_text.canonical_metric_name("ibs_sample_fetch_count"),
                          "ibs_sample_fetch_count")
        self.assertEqual(counter_text.canonical_metric_name("elapsed"), "elapsed")


class ClassifyCounterTextTest(unittest.TestCase):
    def test_counters_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(COUNTERS_TXT), "counters")

    def test_ibs_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(IBS_TXT), "ibs")

    def test_unrecognized_text_returns_none(self):
        self.assertIsNone(counter_text.classify_counter_text("just some random process tree dump\n"))


if __name__ == "__main__":
    unittest.main()
