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
icache miss          1196643001389  #  8.4% icache miss rate
branch misses        116367530656   # 2.65% branch miss
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

    def test_subsection_header_skipped_but_children_parsed(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        metrics = {r["metric"]: r for r in records}
        self.assertNotIn("ibs_sample_data_src_breakdown (scheme: zen4_ibs_extensions)", metrics)
        self.assertEqual(metrics["Local L3 or other L1/L2 in CCX"]["value"], 1.4)
        self.assertEqual(metrics["DRAM"]["value"], 58.7)

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
    def test_ipc_recovered_from_counters_txt(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["ipc"]["value"], 2.38)
        self.assertFalse(derived["ipc"]["is_percent"])
        self.assertIsNone(derived["ipc"]["comment"])

    def test_topdown_l1_takes_second_percentage_not_first(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["retire_pct"]["value"], 40.6)   # not 28.7 (the first percentage)
        self.assertEqual(derived["frontend_pct"]["value"], 28.1)  # despite trailing " high"
        self.assertEqual(derived["backend_pct"]["value"], 24.4)   # no qualifier at all
        self.assertEqual(derived["speculate_pct"]["value"], 7.0)  # despite trailing " low"

    def test_topdown_l2_and_contention_take_first_percentage(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["retire_ucode_pct"]["value"], 0.0)
        # smt-contention's own second, parenthesized number is a hardcoded literal per topdown.c's
        # own comment, not real data -- must take the first (29.2), not the fake "0.0".
        self.assertEqual(derived["contention_pct"]["value"], 29.2)

    def test_generic_miss_rate_and_ghz_also_recovered(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        self.assertEqual(derived["icache_miss_rate"]["value"], 8.4)
        self.assertEqual(derived["ghz"]["value"], 3.27)
        self.assertEqual(derived["branch_miss"]["value"], 2.65)

    def test_nan_and_non_numeric_comments_produce_no_derived_metric(self):
        records = counter_text.parse_counter_text(COUNTERS_TXT)
        derived_names = {d["metric"] for d in counter_text.extract_derived_ratios(records)}
        self.assertNotIn("l3_miss", derived_names)  # "-nan% l3 miss" -- not a real number

    def test_ibs_txt_comment_still_extracted(self):
        records = counter_text.parse_counter_text(IBS_TXT)
        derived = {d["metric"]: d for d in counter_text.extract_derived_ratios(records)}
        # "of 111 branch-retiring ops" has no leading number -- nothing to derive from it, but this
        # must not crash, and the primary value (already a percent) is untouched by this function.
        self.assertNotIn("of_111_branch_retiring_ops", derived)

    def test_empty_records_returns_empty_list(self):
        self.assertEqual(counter_text.extract_derived_ratios([]), [])


class ClassifyCounterTextTest(unittest.TestCase):
    def test_counters_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(COUNTERS_TXT), "counters")

    def test_ibs_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(IBS_TXT), "ibs")

    def test_unrecognized_text_returns_none(self):
        self.assertIsNone(counter_text.classify_counter_text("just some random process tree dump\n"))


if __name__ == "__main__":
    unittest.main()
