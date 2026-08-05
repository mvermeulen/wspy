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
l3 miss              0              # -nan% l3 miss
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


class ClassifyCounterTextTest(unittest.TestCase):
    def test_counters_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(COUNTERS_TXT), "counters")

    def test_ibs_txt_shape(self):
        self.assertEqual(counter_text.classify_counter_text(IBS_TXT), "ibs")

    def test_unrecognized_text_returns_none(self):
        self.assertIsNone(counter_text.classify_counter_text("just some random process tree dump\n"))


if __name__ == "__main__":
    unittest.main()
