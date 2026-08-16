#!/usr/bin/env python3
"""
scripts/publish_reference_matrix.py -- INVESTIGATION.md 4.3 Tier 3 item 2's site-wide publishing
pipeline. Materializes the benchmark reference-matrix database (item 5) as real content on
mvermeulen.org/workload, replacing today's empty auto-created suite-level stub pages.

Fetching the real /perf/workloads/<suite>/ pages during scoping showed their actual shape: rows=test
points, columns=metrics, one machine per page -- not a cross-machine table. That's exactly
render_reference_by_machine()'s shape (web/server.py, PR #212), so this script is mostly publishing
plumbing over data that already exists, not new analysis. Three page kinds:

  1. Per-(suite, machine) wide table at <suite>/by-machine-<machine>/ -- the real numeric content.
     Sourced from joblib.aggregate_reference_matrix_cell() for machines with local wspy-store
     presence, merged with server.recover_machine_metrics_from_wordpress() (item 21) for machines
     that are WordPress-only -- discovered via server.discover_wordpress_matrix_rows() (item 22), not
     just item 21's per-row gap-filling, since a WordPress-only machine has no local runs.json at all
     for enumerate_reference_matrix_cells() to find in the first place. A machine with local data for
     some test points and WordPress-only data for others gets both merged into one page -- local
     always wins per test point when both exist, same "real data over recovered" precedent
     render_reference_test_point_detail() already established. WordPress-recovered rows carry no
     wspy-summary reliability verdict, marked with `*` rather than silently blended in as equally
     trustworthy.
  2. Per-suite index at <suite>/ (today an empty auto-created stub) -- one row per test point with
     per-machine coverage (local run count, or "WordPress only"), plus links to every machine's own
     wide-table page from (1).
  3. Root rollup, listing every suite x machine page (1) actually exists for -- sourced from scanning
     what this script has already generated into the report-root (<suite>/by-machine/*.md), not a
     fresh computation, so it stays accurate regardless of which --suite/--machine this particular
     invocation touched. Same "always freshly regenerated from what's actually there" precedent
     publish_machine_page.py's own catalog index already uses.

Imports server.py directly for discover_wordpress_matrix_rows()/recover_machine_metrics_from_wordpress()
-- same precedent wspy-testpoint already set (`import server as web_export`) for reusing server.py's
functions from a separate CLI tool, rather than duplicating them.

Can't reuse render_reference_by_machine()'s own HTML (local-only /reference/ links, CSS classes
WordPress doesn't have) -- generates its own report-root markdown + WP Gutenberg block markup from the
same underlying row data instead, the same two-representations-from-one-source-of-truth precedent
publish_cpu2026_benchmarks.py/publish_machine_page.py already use.

WordPress discovery (item 22) is a real crawl cost (~50s/suite, confirmed live during that item's own
development) -- included by default, since finding WordPress-only machines is this script's whole
point, but skippable via --skip-wordpress-discovery for a fast local-only run.

Confirmed with the author (2026-08-07): generated-only, no hand-maintained content on the new site to
protect (the suite-level pages are empty stubs today). The old mvermeulen.org/perf/workloads/ site is
untouched by this script -- retiring its hand-maintenance in favor of this pipeline, once the new site
has real authority, is a deliberate future follow-on, not done here.

Usage:
  publish_reference_matrix.py --dry-run                       # preview everything, touches nothing
  publish_reference_matrix.py --suite cpu2026                  # just one suite
  publish_reference_matrix.py --suite cpu2026 --machine amd-370-64gb  # just one page, for review
  publish_reference_matrix.py                                  # every suite, every machine
  publish_reference_matrix.py --publish                        # also flip each leaf page to published
  publish_reference_matrix.py --skip-wordpress-discovery       # local wspy-store machines only, fast
"""
import argparse
import functools
import html
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "web"))
import joblib  # noqa: E402
import report_root  # noqa: E402
import wp_client  # noqa: E402
from wp_client import load_config  # noqa: E402
import server as web_export  # noqa: E402

SUITES = web_export.REFERENCE_MATRIX_SUITES
ROOT_SLUG = "reference-matrix"
ROOT_TITLE = "Reference matrix"

# ---------------------------------------------------------------------------
# Table interactivity (INVESTIGATION.md follow-up to Tier 3 item 2): the wspy WordPress service
# account is deliberately scoped without unfiltered_html (see CLAUDE.md's wp_client.py entry), so
# <script>/<style> embedded directly in REST-published post content gets stripped by wp_kses_post()
# on save. Rather than fight that, generated pages only ever carry plain <table>/<th>/<td> markup --
# data-col-kind/data-col-group attributes per column and a data-desc-bearing info span -- and all the
# actual search/sort/column-group-toggle/row-toggle/tooltip behavior lives in
# scripts/wp-refmatrix-assets.php, a small site-wide plugin (same install pattern as
# scripts/wp-auth-bridge.php) that decorates any <table class="wspy-refmatrix"> it finds. Confirmed
# with the author (2026-08-07).
# ---------------------------------------------------------------------------
METRICS_MD_PATH = os.path.join(REPO_ROOT, "doc", "METRICS.md")

# Metric names whose reference-matrix column is an absolute accumulated count/duration rather than
# something already normalized (a percentage, a per-1000-instruction rate, IPC, bandwidth, ...) --
# these are real numbers but not comparable across differently-sized runs the way a ratio is, so the
# plugin's "show raw counts" toolbar checkbox hides them by default. Curated by hand against
# doc/METRICS.md's own derivations rather than inferred from its [raw]/[feature]/... tags -- those
# track *database promotion status*, not "is this a ratio" (e.g. itlb1/dtlb1/tlbflush are tagged
# [raw] there but are already per-1000-instruction rates, not raw counts). Anything NOT in this set
# defaults to visible, including a future metric this list hasn't caught up with yet -- silently
# hiding an unclassified column is a worse failure than showing one that could arguably be hidden.
RAW_COUNT_METRICS = {
    # rusage (topdown.c:print_usage()) -- accumulated absolute counts/durations
    "nvcsw", "nivcsw", "inblock", "oublock", "maxrss", "minflt", "majflt", "nswap", "utime", "stime",
    # software counters -- raw event counts over the run, not rates
    "page faults", "context switches", "cpu migrations", "major page faults", "minor page faults",
    "alignment faults", "emulation faults", "cpu-clock", "task-clock", "float",
    # branch raw counts (AMD/ARM extras)
    "near_return", "near_return_mispredicted", "indirect_branch_mispredicted",
    "br_immed_retired", "br_return_retired", "br_pred", "br_mis_pred",
    # ARM-only raw dumps (topdown.c:print_arm_dcache_mem()/print_arm_icache_tlb()/
    # print_arm_mem_align_tlb()) -- event counts, no headline ratio
    "l1d_cache_refill", "l1d_tlb_refill", "l2d_cache_refill", "l2d_tlb_refill",
    "l1i_cache_refill", "l1i_tlb_refill", "l2i_tlb_refill", "dtlb_walk", "itlb_walk",
    "ld_align_lat", "st_align_lat",
    # power
    "pkg_joules",
    # AMD IBS -- raw sampled-event counts (ibs_op_accepted_ratio and the *_rate columns stay "ratio")
    "ibs_fetch", "ibs_op", "ibs_op_unfiltered",
    "ibs_sample_fetch_count", "ibs_sample_op_count", "ibs_sample_lost",
    # GPU -- absolute memory sizes, not comparable across differently-sized runs
    "gpu_vram_used", "gpu_vram_total", "nv_vram_used_mb", "nv_vram_total_mb",
    # counter coverage -- running tallies, not a workload characteristic
    "counters_measured", "counters_requested",
    # WordPress-recovery-only raw primary values (web/counter_text.py's parse_counter_text()) --
    # never real local CSV columns at all, just the on-screen operand of a human-text line whose
    # *comment* carries the actually-comparable ratio under a different name (already classified
    # above/below). Found auditing a real reference-matrix "Other" dump (2026-08-07): with no
    # RAW_COUNT_METRICS entry, these defaulted to "ratio" (visible), which meant the mostly-raw
    # "Other" group never defaulted hidden the way every other all-raw group does.
    "instructions", "cpu-cycles", "slots", "retiring", "speculation", "smt-contention",
    "near return", "branches", "branch misses", "conditional", "indirect", "indirect mispredict",
    "l1 iTLB miss", "l1 dTLB miss", "l2 iTLB miss", "l2 dTLB miss", "tlb flush",
    "l2 hit from l1", "l2 hit from l2 pf", "l2 miss from l1", "l3 access", "l3 miss",
    "l3 hit from l2 pf", "l3 miss from l2 pf", "icache miss",
    "float 128", "float 256", "float 512", "float MMX", "float scalar",
    "-- ucode", "-- fastpath", "-- latency", "-- bandwidth", "-- cpu", "-- memory",
    "-- branch mispredict", "-- pipeline restart",
    # Unconfirmed exact provenance (some other comment's bare unit-word slug, most likely) but
    # clearly not a real named metric either way -- same "raw and uninteresting" treatment.
    "seconds", "mb",
}


def metric_col_kind(metric):
    """"raw" or "ratio" for the data-col-kind attribute build_machine_page() emits -- see
    RAW_COUNT_METRICS above."""
    return "raw" if metric in RAW_COUNT_METRICS else "ratio"


# joblib.resolve_column_group() already maps a wspy CSV column name to the ALL_GROUPS/--counters
# token that produces it (or the "system"/"power" sentinel) -- reused here as the primary source for
# the plugin's per-group "Columns" checkboxes, rather than a second classification table duplicating
# it. It only covers columns gated by a --counters/--system/--power flag, though: rusage ("always
# emitted regardless of counter_mask" per doc/METRICS.md), AMD IBS, GPU, counter-coverage, and the
# topdown L2/backend-deep-dive splits aren't in its COLUMN_TO_GROUP table, so this fills those in and
# falls back to "other" for anything neither covers -- every metric ends up in some group, so the
# checkbox list is never silently incomplete for an unclassified column.
SUPPLEMENTARY_COLUMN_GROUPS = {
    # rusage/process (topdown.c:print_usage())
    "elapsed": "process", "utime": "process", "stime": "process", "on_cpu": "process",
    "nvcsw": "process", "nivcsw": "process", "inblock": "process", "oublock": "process",
    "maxrss": "process", "minflt": "process", "majflt": "process", "nswap": "process",
    # topdown quality indicators + L2 splits (share the top-level "topdown" bucket)
    "confidence": "topdown", "sanity": "topdown", "contention_pct": "topdown",
    "retire_ucode_pct": "topdown", "retire_fastpath_pct": "topdown",
    "frontend_latency_pct": "topdown", "frontend_bandwidth_pct": "topdown",
    "backend_cpu_pct": "topdown", "backend_memory_pct": "topdown",
    "spec_branch_pct": "topdown", "spec_pipeline_pct": "topdown",
    # topdown backend deep-dive (--topdown-backend) -- own group, doesn't roll up to "topdown"
    "l1_bound": "topdown-backend", "l2_bound": "topdown-backend", "l3_bound": "topdown-backend",
    "dram_bound": "topdown-backend", "store_bound": "topdown-backend",
    "l1_bound_slots_pct": "topdown-backend", "l2_bound_slots_pct": "topdown-backend",
    "l3_bound_slots_pct": "topdown-backend", "dram_bound_slots_pct": "topdown-backend",
    "store_bound_slots_pct": "topdown-backend",
    # branch raw extras (AMD/ARM)
    "near_return": "branch", "near_return_mispredicted": "branch",
    "indirect_branch_mispredicted": "branch", "br_immed_retired": "branch",
    "br_return_retired": "branch", "br_pred": "branch", "br_mis_pred": "branch",
    "branches per 1000 inst": "branch", "conditional per 1000 inst": "branch",
    "indirect per 1000 inst": "branch",
    # ARM-only raw dumps -- no headline ratio of their own, closest existing bucket is TLB
    "l1d_cache_refill": "tlb", "l1d_tlb_refill": "tlb", "l2d_cache_refill": "tlb",
    "l2d_tlb_refill": "tlb", "l1i_cache_refill": "tlb", "l1i_tlb_refill": "tlb",
    "l2i_tlb_refill": "tlb", "dtlb_walk": "tlb", "itlb_walk": "tlb",
    "ld_align_lat": "tlb", "st_align_lat": "tlb",
    # power extras beyond joblib's POWER_COLUMN_NAMES (pkg_joules/pkg_watts only)
    "core_joules": "power", "core_watts": "power",
    # AMD IBS
    "ibs_fetch": "ibs", "ibs_op": "ibs", "ibs_op_unfiltered": "ibs", "ibs_op_accepted_ratio": "ibs",
    "ibs_l3missonly": "ibs", "ibs_ldlat_threshold": "ibs", "ibs_fetchlat_threshold": "ibs",
    "ibs_sample_fetch_count": "ibs", "ibs_sample_ic_miss_rate": "ibs",
    "ibs_sample_l1tlb_miss_rate": "ibs", "ibs_sample_l2tlb_miss_rate": "ibs",
    "ibs_sample_op_count": "ibs", "ibs_sample_dc_miss_rate": "ibs",
    "ibs_sample_dc_l1tlb_miss_rate": "ibs", "ibs_sample_dc_l2tlb_miss_rate": "ibs",
    "ibs_sample_brn_misp_rate": "ibs", "ibs_sample_lost": "ibs",
    "ibs_sample_dram_rate": "ibs", "ibs_sample_remote_node_rate": "ibs",
    # GPU (all three backends, plus system.c's own generic gpu_busy column)
    "gpu_busy": "gpu", "gpu_busy_percent": "gpu", "temp_gfx": "gpu", "gfx_activity": "gpu", "gfx_power": "gpu",
    "gfxclk_freq": "gpu", "gpu_temp": "gpu", "gpu_activity": "gpu", "gpu_power": "gpu",
    "gpu_freq": "gpu", "gpu_vram_used": "gpu", "gpu_vram_total": "gpu", "gpu_temp_source": "gpu",
    "gpu_activity_source": "gpu", "nv_gpu_busy": "gpu", "nv_vram_used_mb": "gpu",
    "nv_vram_total_mb": "gpu",
    # counter coverage -- measurement quality, not a workload characteristic
    "counters_measured": "coverage", "counters_requested": "coverage",

    # WordPress-recovery-only derived ratios/rates (web/counter_text.py's extract_derived_ratios()
    # generic-slug fallback, or its archetype-facing "_pct" names) -- never real local CSV columns,
    # but genuine comparable ratios all the same, so they get a real group instead of falling
    # through to "other" alongside the raw leftovers above. Found auditing a real reference-matrix
    # "Other" dump (2026-08-07), which is why grouping these was the whole point of that exercise.
    "ghz": "ipc", "ipc_mean": "ipc",
    "backend_pct": "topdown", "frontend_pct": "topdown", "retire_pct": "topdown",
    "speculate_pct": "topdown", "smt_contention_pct": "topdown",
    "icache_miss_pct": "topdown", "icache_per_1000_inst": "topdown",
    "opcache_per_1000_inst": "topdown", "tlb_flush_per_1000_inst": "topdown",
    "itlb_miss_per1k": "topdown", "dtlb_miss_per1k": "topdown",
    # L1 counterparts of the L2 itlb_miss_per1k/dtlb_miss_per1k pair above -- topdown.c's own
    # "l1 iTLB miss"/"l1 dTLB miss" comment ("X.XXX L1 iTLB per 1000 inst") had no
    # GENERIC_LABEL_NAME_OVERRIDES entry, so extract_derived_ratios() fell through to a plain
    # slugify with no group assigned; found live (2026-08-08) sitting unclassified in "Other".
    "l1_itlb_per_1000_inst": "topdown", "l1_dtlb_per_1000_inst": "topdown",
    "branch_mispredict_pct": "branch", "branches_per_1000_inst": "branch",
    "conditional_branches_per_1000_inst": "branch", "indirect_branches_per_1000_inst": "branch",
    "indirect_branch_mispredict_rate": "branch", "near_return_per_1000_inst": "branch",
    "near_return_mispredict_rate": "branch",
    "l2_miss_pct": "cache2", "l2_access_per_1000_inst": "cache2",
    "l3_access_per_1000_inst": "cache3",
    "avx_128_per_1000_inst": "float", "avx_256_per_1000_inst": "float",
    "avx_512_per_1000_inst": "float", "mmx_per_1000_inst": "float",
    "scalar_per_1000_inst": "float", "float_per_1000_inst": "float",
    "ibs_dc_miss_pct": "ibs", "ibs_dc_l1tlb_miss_pct": "ibs", "ibs_dc_l2tlb_miss_pct": "ibs",
    "ibs_dram_pct": "ibs", "ibs_remote_node_pct": "ibs",
}


# joblib.resolve_column_group()/SUPPLEMENTARY_COLUMN_GROUPS above return a finer-grained group
# token than the plugin's Columns panel should actually show as separate checkboxes -- this maps
# each such token to the coarser one it should present as instead. Two different reasons a merge
# ends up here, both author calls made live-testing the Columns panel (2026-08-07/08):
#   - Genuine duplicates for one methodology, split only by which --topdown-* CLI flag collected
#     which column: the main L1 split ("topdown"/"topdown2", both print_topdown()), the AMD-only
#     backend deep-dive ("topdown-backend", print_topdown_be() -- l1_bound/l2_bound/...), and the
#     AMD-only frontend/op-cache deep-dive ("topdown-frontend"/"topdown-optlb", print_topdown_fe()/
#     print_topdown_op() -- icache/itlb1/opcache/dtlb1/...). One mental model ("topdown"), not four
#     same-looking-but-not-quite-matching checkboxes.
#   - A group too narrow to earn its own checkbox: "opcache" (joblib's own token for the single
#     cross-vendor "opcache miss" column) folds into "other"; "cache3" (AMD-only "l3miss") folds
#     into "cache2" (renamed "cache" here, since it's no longer just L2) since the author found L2
#     and L3 cache miss rate useful to browse together rather than as two separate checkboxes.
GROUP_ALIASES = {
    "topdown2": "topdown", "topdown-frontend": "topdown", "topdown-backend": "topdown",
    "topdown-optlb": "topdown",
    "opcache": "other",
    "cache2": "cache", "cache3": "cache",
}


def metric_col_group(metric):
    """The --counters/--system/--power group token (or a SUPPLEMENTARY_COLUMN_GROUPS bucket, or
    "other" as a last resort) for the data-col-group attribute -- what the plugin's per-group
    "Columns" checkboxes filter on. See GROUP_ALIASES above for the merges applied on top of the
    raw group token."""
    group = joblib.resolve_column_group(metric) or SUPPLEMENTARY_COLUMN_GROUPS.get(metric, "other")
    return GROUP_ALIASES.get(group, group)


# WordPress's own server-side sanitize_title() converts the "." in a cpu2026 bench identity
# ("706.stockfish_r", scripts/publish_cpu2026_benchmarks.py's own submitted slug, matching
# CPU2026_BENCHMARKS' real dot-form keys) into "-" when actually creating that benchmark's page
# ("706-stockfish_r") -- confirmed live (2026-08-08) from a real publish log's own WordPress-crawl
# output ("scanning cpu2026/881-neutron_s..."). joblib.discover_wordpress_matrix_rows() reads back
# that literal WP slug as `test`, needed as-is for its own later find_page() lookups by that exact
# slug (recover_machine_metrics_from_wordpress()'s WP-hierarchy walk) -- normalizing it there would
# fix this lookup but break that one, since the real WP page genuinely only exists under the dash
# form. So the fix lives narrowly here instead: only the benchmark's numeric-prefix separator is
# ever affected (no cpu2026 benchmark name itself contains a hyphen), so reversing just that one
# substitution as a fallback is precise, not a guess.
_CPU2026_WP_SLUG_DASH_RE = re.compile(r"^(\d+)-")


def row_group_for_test(test):
    """cpu2026's own benchmark-suite category (intrate/intspeed/fprate/fpspeed --
    joblib.CPU2026_BENCHMARKS, keyed by the exact bench name resolve_test_identity() already uses as
    this row's `test` value for a local cell) for the data-row-group attribute -- what the plugin's
    Rows panel groups checkboxes by, the row-axis counterpart to metric_col_group() on the column
    axis. Falls back to _CPU2026_WP_SLUG_DASH_RE's dash-to-dot substitution when the direct lookup
    misses -- see that regex's own comment for why a WordPress-recovered row's `test` needs it.
    Returns None for a Phoronix test (no such categorization exists) or a genuinely unrecognized
    cpu2026 bench (a future benchmark this table hasn't caught up with yet) -- the plugin already
    falls back to a flat, ungrouped row-checkbox list whenever fewer than two rows carry this
    attribute at all, so leaving it off degrades gracefully rather than needing a placeholder
    value."""
    info = joblib.CPU2026_BENCHMARKS.get(test)
    if info is None:
        info = joblib.CPU2026_BENCHMARKS.get(_CPU2026_WP_SLUG_DASH_RE.sub(r"\1.", test, count=1))
    return (info or {}).get("suite")


# Priority order for the plugin's Columns panel -- fundamental/commonly-read groups first (ipc and
# topdown are what most people reach for first), niche/vendor-specific ones last, "other" always the
# final catch-all. Anything not listed here (there shouldn't be any -- every joblib.ALL_GROUPS/
# SUPPLEMENTARY_COLUMN_GROUPS token is covered) sorts just before "other" rather than disappearing.
COLUMN_GROUP_ORDER = [
    "ipc", "topdown", "branch", "dcache", "icache", "cache", "tlb", "memory",
    "process", "software", "float", "system", "power", "ibs", "gpu", "coverage", "other",
]


# The optional "(?:[^*\n]*?:\s+)?" prefix tolerates a leading label like "AMD extras: " or
# "ARM extras: " before the bold name(s) -- doc/METRICS.md's branch-prediction section uses exactly
# that shape ("- AMD extras: **near_return**, **near_return_mispredicted**, ... -- ..."), which this
# regex used to reject outright (requiring bold names to start immediately after "- "), silently
# dropping tooltips for every name in those two bullets. Found live (2026-08-08): the reference
# matrix's branch-prediction columns for those seven AMD/ARM raw counts had no tooltip at all.
_METRIC_DESC_BULLET_RE = re.compile(r'^-\s+(?:[^*\n]*?:\s+)?((?:\*\*[^*]+\*\*,?\s*)+)—\s*(.*)$')
_METRIC_DESC_TAG_RE = re.compile(r'^(\s*`\[[a-z-]+\]`[,\s]*)+')


@functools.lru_cache(maxsize=1)
def load_metric_descriptions(metrics_md_path=METRICS_MD_PATH):
    """{metric_name: one-line description} parsed from doc/METRICS.md's own `- **name** -- ...`
    bullets -- that file is already "the single index of every metric wspy produces" (CLAUDE.md), so
    this reads tooltip text from there instead of hand-duplicating descriptions that would drift out
    of sync. A bullet naming several metrics at once (e.g. "**l1_bound**, **l2_bound** -- ...") maps
    all of them to the same description. Best-effort and cached (doc/METRICS.md doesn't change mid-run
    across the many machine pages this script generates in one invocation) -- returns {} if the file
    is missing rather than failing the whole publish run over a tooltip."""
    try:
        with open(metrics_md_path) as f:
            raw_lines = f.readlines()
    except OSError:
        return {}

    # Markdown wraps a single bullet across several physical lines (this file is hand-wrapped at
    # ~100 columns) -- rejoin any line that isn't itself a new bullet/heading/blank into the bullet
    # it continues, so a description isn't truncated at an arbitrary mid-sentence line break.
    bullets = []
    for raw in raw_lines:
        line = raw.rstrip("\n")
        stripped = line.strip()
        if stripped.startswith("- "):
            bullets.append(stripped)
        elif stripped and not stripped.startswith("#") and bullets:
            bullets[-1] += " " + stripped
        # blank lines / headings / non-continuation lines just end the current bullet implicitly --
        # the next "- " line starts a fresh one.

    descriptions = {}
    for bullet in bullets:
        m = _METRIC_DESC_BULLET_RE.match(bullet)
        if not m:
            continue
        names = re.findall(r'\*\*([^*]+)\*\*', m.group(1))
        desc = _METRIC_DESC_TAG_RE.sub("", m.group(2)).replace("`", "").replace("**", "").strip()
        # Cut at the end of the first sentence where there's a clean one, else hard-wrap -- either
        # way this is tooltip text (title attribute), not the full doc/METRICS.md entry.
        first_sentence = re.split(r'(?<=[.!?])\s+', desc, maxsplit=1)[0]
        desc = first_sentence if 20 <= len(first_sentence) <= 200 else desc
        if len(desc) > 200:
            desc = desc[:197].rsplit(" ", 1)[0] + "…"
        for name in names:
            descriptions.setdefault(name.strip(), desc)
    return descriptions


def resolve_report_root(args):
    if args.report_root:
        return args.report_root
    cfg = load_config()
    if cfg and cfg.get("report_root"):
        return cfg["report_root"]
    return report_root.DEFAULT_REPORT_ROOT


def local_cells_for_suite(report_root_path, suite):
    all_cells = joblib.enumerate_reference_matrix_cells(report_root_path, web_export.dest_roots())
    return [c for c in all_cells if c["suite"] == suite]


def wordpress_rows_for_suite(wp_cfg, suite, skip):
    if skip or not wp_cfg:
        return []
    print("  crawling WordPress for %s (this can take a while)..." % suite, file=sys.stderr)
    return web_export.discover_wordpress_matrix_rows(
        wp_cfg, suite, progress=lambda msg: print("    %s" % msg, file=sys.stderr))


def build_machine_rows(agg_cfg, wp_cfg, suite, machine, local_cells, wp_rows):
    """Returns a sorted list of (test, test_point, rows, source) for one machine -- rows is
    aggregate_reference_matrix_cell()'s shape for source="local", or
    recover_machine_metrics_from_wordpress()'s shape for source="wordpress". Local always wins per
    test point when both a local cell and a WordPress-only row exist for the same (test, test_point)."""
    local_by_tp = {(c["test"], c["test_point"]): c for c in local_cells if c["machine"] == machine}
    wp_tps = {(r["test"], r["test_point"]) for r in wp_rows if r["machine"] == machine}

    entries = []
    for (test, test_point), c in local_by_tp.items():
        rows = joblib.aggregate_reference_matrix_cell(
            agg_cfg["wspy_testpoint_bin"], agg_cfg["db"], suite, c["benchmark"], machine,
            report_root_path=agg_cfg["report_root_path"])
        if rows:
            entries.append((test, test_point, rows, "local"))
    for test, test_point in wp_tps - set(local_by_tp):
        rows = web_export.recover_machine_metrics_from_wordpress(wp_cfg, suite, test, test_point, machine)
        if rows:
            entries.append((test, test_point, rows, "wordpress"))
    return sorted(entries, key=lambda e: (e[0], e[1]))


def cell_text(rows, metric, source):
    for r in rows:
        if r["metric"] == metric:
            mean, n = r.get("mean", ""), r.get("n", "")
            marker = ""
            if source == "wordpress":
                marker = "*"
            elif r.get("verdict") and r["verdict"] != "PASS":
                marker = "†"
            return "%s (n=%s)%s" % (mean, n, marker)
    return "—"


def build_machine_page(suite, machine, entries):
    """Returns (markdown, wp_html) for one machine's wide table across every test point in `suite`
    it has data for, or (None, None) if there's nothing to show. Cell text mirrors
    render_reference_by_machine()'s own "mean (n=N)" shape; `*` marks a WordPress-recovered cell (no
    reliability verdict behind it), `†` marks a non-PASS wspy-summary verdict on a real local
    cell -- footnotes only appear when at least one cell actually uses that marker."""
    metrics = sorted({r["metric"] for _, _, rows, _ in entries for r in rows})
    if not metrics:
        return None, None

    has_wp = any(source == "wordpress" for _, _, _, source in entries)
    has_warn = any(r.get("verdict") not in (None, "PASS")
                   for _, _, rows, source in entries if source == "local" for r in rows)

    kinds = [metric_col_kind(m) for m in metrics]
    groups = [metric_col_group(m) for m in metrics]
    descriptions = load_metric_descriptions()

    md_lines = ["# %s -- %s" % (machine, suite), "",
                "| test point | " + " | ".join(metrics) + " |",
                "|---|" + "|".join("---" for _ in metrics) + "|"]
    body_rows_html = []
    for test, test_point, rows, source in entries:
        label = "%s / %s" % (test, test_point)
        cells = [cell_text(rows, m, source) for m in metrics]
        md_lines.append("| %s | %s |" % (label, " | ".join(cells)))
        cells_html = "".join(
            '<td data-col-kind="%s" data-col-group="%s">%s</td>' % (kind, group, html.escape(c))
            for c, kind, group in zip(cells, kinds, groups))
        row_group = row_group_for_test(test)
        row_attr = ' data-row-group="%s"' % row_group if row_group else ""
        body_rows_html.append("<tr%s><td>%s</td>%s</tr>" % (row_attr, html.escape(label), cells_html))

    md_lines.append("")
    if has_wp:
        md_lines.append("`*` -- recovered from an already-published WordPress page, not this "
                         "machine's local wspy-store; no reliability verdict behind it.")
    if has_warn:
        md_lines.append("`†` -- carries a non-PASS wspy-summary verdict "
                         "(thin/noisy/mixed-pmu/mixed-env).")
    markdown = "\n".join(md_lines) + "\n"

    footnote_html = ""
    if has_wp:
        footnote_html += (
            '<!-- wp:paragraph --><p><em>`*` -- recovered from an already-published WordPress page, '
            "not this machine's local wspy-store; no reliability verdict behind it.</em></p>"
            "<!-- /wp:paragraph -->\n\n")
    if has_warn:
        footnote_html += (
            '<!-- wp:paragraph --><p><em>`†` -- carries a non-PASS wspy-summary verdict '
            "(thin/noisy/mixed-pmu/mixed-env).</em></p><!-- /wp:paragraph -->\n\n")

    def th(metric, kind, group):
        desc = descriptions.get(metric)
        # data-desc, not title -- WordPress's post-content sanitizer (wp_kses_post(), see the
        # module docstring) allows data-* attributes by default but not a bare title on <span>, so
        # a title="..." here would silently get stripped on save while surviving fine as data-desc.
        # scripts/wp-refmatrix-assets.php applies it as a real title attribute client-side instead.
        info = (' <span class="wspy-info" data-desc="%s">ⓘ</span>' % html.escape(desc)
                if desc else "")
        # data-metric duplicates the visible label -- the plugin's per-column checkbox panel reads
        # it directly rather than scraping the info-span out of the <th>'s text content.
        return ('<th data-col-kind="%s" data-col-group="%s" data-metric="%s">%s%s</th>'
                % (kind, group, html.escape(metric), html.escape(metric), info))

    header_html = "".join(th(m, k, g) for m, k, g in zip(metrics, kinds, groups))
    # data-group-order on the table itself, not hardcoded a second time in wp-refmatrix-assets.php --
    # COLUMN_GROUP_ORDER above is the single source of truth for the plugin's Columns-panel ordering.
    group_order_attr = html.escape(",".join(COLUMN_GROUP_ORDER))
    wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>%s -- %s</h1>\n<!-- /wp:heading -->\n\n'
        '%s'
        '<!-- wp:table --><figure class="wp-block-table">'
        '<table class="wspy-refmatrix" data-group-order="%s"><thead><tr>'
        '<th>test point</th>%s</tr></thead><tbody>%s</tbody></table></figure><!-- /wp:table -->'
        % (html.escape(machine), html.escape(suite), footnote_html, group_order_attr, header_html,
           "".join(body_rows_html))
    )
    return markdown, wp_html


def build_suite_index(report_root_path, suite, local_cells, wp_rows, by_machine_slugs):
    """(markdown, wp_html) for the suite-level index page -- one row per (test, test_point) covering
    either a local cell or a WordPress-only discovery hit, plus links to every by-machine wide-table
    page this run knows about for this suite."""
    rows_by_tp = {}
    for c in local_cells:
        runs_json_path = os.path.join(report_root_path, suite, c["test"], c["test_point"],
                                       c["machine"], "runs.json")
        n = joblib.count_stats_pool_runs(runs_json_path)
        rows_by_tp.setdefault((c["test"], c["test_point"]), {})[c["machine"]] = "%d run(s)" % n
    for r in wp_rows:
        key = (r["test"], r["test_point"])
        rows_by_tp.setdefault(key, {}).setdefault(r["machine"], "WordPress only")

    md_lines = ["# %s -- reference matrix" % suite, ""]
    tp_rows_html = []
    if rows_by_tp:
        md_lines += ["| test | test point | machines |", "|---|---|---|"]
        for (test, test_point), by_machine in sorted(rows_by_tp.items()):
            machines_text = ", ".join("%s (%s)" % (m, s) for m, s in sorted(by_machine.items()))
            md_lines.append("| %s | %s | %s |" % (test, test_point, machines_text))
            row_group = row_group_for_test(test)
            row_attr = ' data-row-group="%s"' % row_group if row_group else ""
            tp_rows_html.append("<tr%s><td>%s</td><td>%s</td><td>%s</td></tr>" % (
                row_attr, html.escape(test), html.escape(test_point), html.escape(machines_text)))
    else:
        md_lines.append("No test points yet.")

    md_lines.append("")
    md_lines.append("## By machine")
    for machine in sorted(by_machine_slugs):
        md_lines.append("- [%s](by-machine-%s/)" % (machine, machine))
    markdown = "\n".join(md_lines) + "\n"

    # wspy-refmatrix class gets this table the plugin's search/sort toolbar too (scripts/
    # wp-refmatrix-assets.php) -- no data-col-kind attributes here since none of these columns are
    # metrics, so the "show raw counts" checkbox correctly stays hidden for this table.
    tp_table_html = (
        '<!-- wp:table --><figure class="wp-block-table"><table class="wspy-refmatrix"><thead><tr>'
        '<th>test</th><th>test point</th><th>machines</th></tr></thead><tbody>%s</tbody>'
        '</table></figure><!-- /wp:table -->\n\n' % "".join(tp_rows_html)
        if tp_rows_html else
        '<!-- wp:paragraph --><p>No test points yet.</p><!-- /wp:paragraph -->\n\n'
    )
    by_machine_items_html = "".join(
        '<!-- wp:list-item --><li><a href="%s/">%s</a></li><!-- /wp:list-item -->\n'
        % (html.escape("by-machine-" + m), html.escape(m))
        for m in sorted(by_machine_slugs)
    )
    wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>%s -- reference matrix</h1>\n<!-- /wp:heading -->\n\n'
        '%s'
        '<!-- wp:heading {"level":2} -->\n<h2>By machine</h2>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:list --><ul>%s</ul><!-- /wp:list -->'
        % (html.escape(suite), tp_table_html, by_machine_items_html)
    )
    return markdown, wp_html


def scan_existing_by_machine_pages(report_root_path):
    """{suite: [machine, ...]} from <report-root>/<suite>/by-machine/*.md already generated by this
    script (any prior run, or this one) -- not a fresh computation, so the root rollup always
    reflects what's actually been published regardless of which --suite/--machine this particular
    invocation touched."""
    result = {}
    for suite in SUITES:
        by_machine_dir = os.path.join(report_root_path, suite, "by-machine")
        if not os.path.isdir(by_machine_dir):
            continue
        machines = sorted(fn[:-3] for fn in os.listdir(by_machine_dir) if fn.endswith(".md"))
        if machines:
            result[suite] = machines
    return result


def build_root_rollup(by_suite):
    md_lines = ["# Reference matrix", ""]
    for suite in sorted(by_suite):
        md_lines.append("## %s" % suite)
        for machine in by_suite[suite]:
            md_lines.append("- [%s](%s/by-machine-%s/)" % (machine, suite, machine))
        md_lines.append("")
    markdown = "\n".join(md_lines) + "\n"

    sections_html = []
    for suite in sorted(by_suite):
        items_html = "".join(
            '<!-- wp:list-item --><li><a href="/%s/by-machine-%s/">%s</a></li><!-- /wp:list-item -->\n'
            % (html.escape(suite), html.escape(m), html.escape(m))
            for m in by_suite[suite]
        )
        sections_html.append(
            '<!-- wp:heading {"level":2} -->\n<h2>%s</h2>\n<!-- /wp:heading -->\n\n'
            '<!-- wp:list --><ul>%s</ul><!-- /wp:list -->\n\n' % (html.escape(suite), items_html)
        )
    wp_html = ('<!-- wp:heading {"level":1} -->\n<h1>Reference matrix</h1>\n<!-- /wp:heading -->\n\n'
               + "".join(sections_html))
    return markdown, wp_html


def write_generated_file(report_root_path, rel_path, content, dry_run, new_rel_paths):
    path = os.path.join(report_root_path, rel_path)
    if not dry_run:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
    new_rel_paths.append(rel_path)


def publish_leaf(wp, levels, content, args, label):
    if args.dry_run:
        print("  WP %s: would publish_page_at_path(levels=%r, content=%d bytes, publish=%s)"
              % (label, levels, len(content), args.publish))
        return True
    try:
        page, created = wp_client.publish_page_at_path(
            wp["site_url"], wp["username"], wp["app_password"], levels, content,
            do_publish=args.publish, force=args.force)
    except wp_client.WPContentDriftError as e:
        print("  WP %s: FAILED (drift) -- %s: %s" % (label, e, e.link), file=sys.stderr)
        return False
    except wp_client.WPError as e:
        print("  WP %s: FAILED (status=%s code=%s): %s" % (label, e.status, e.code, e),
              file=sys.stderr)
        return False
    print("  WP %s: %s (id=%s, status=%s) link=%s"
          % (label, "created" if created else "found/updated", page["id"], page.get("status"),
             page.get("link")))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", action="append", default=None, choices=list(SUITES),
                     help="restrict to this suite (repeatable); default: all of %s" % (SUITES,))
    ap.add_argument("--machine", action="append", default=None,
                     help="restrict to this machine slug (repeatable); default: every machine found")
    ap.add_argument("--skip-wordpress-discovery", action="store_true",
                     help="skip the WordPress crawl (item 22) -- local wspy-store machines only, "
                          "much faster")
    ap.add_argument("--skip-local-store", action="store_true",
                     help="ignore local wspy-store data entirely -- every test point is sourced from "
                          "WordPress recovery instead (normally local always wins per test point when "
                          "both exist). For testing the WordPress-recovery path itself in isolation, "
                          "without local data silently masking it.")
    ap.add_argument("--report-root", default=None, help="override the report-root path")
    ap.add_argument("--report-root-remote", default=report_root.DEFAULT_REPORT_ROOT_REMOTE,
                     help="report-root git remote, for clone-if-missing")
    ap.add_argument("--db", default=os.path.join(REPO_ROOT, "web", "runs", "store.db"),
                     help="normalized wspy-store database (default: %(default)s)")
    ap.add_argument("--wspy-testpoint-bin", default=os.path.join(REPO_ROOT, "wspy-testpoint"),
                     help="wspy-testpoint binary (default: %(default)s)")
    ap.add_argument("--publish", action="store_true",
                     help="flip each WP page to status=publish (default: leave as draft)")
    ap.add_argument("--force", action="store_true",
                     help="overwrite a leaf page even if its live content has drifted since wspy "
                          "last published it (default: refuse, since that usually means a hand-edit)")
    ap.add_argument("--dry-run", action="store_true",
                     help="print what would be written/committed/published, touch nothing")
    args = ap.parse_args()

    suites = args.suite or list(SUITES)
    machine_filter = set(args.machine) if args.machine else None

    report_root_path = resolve_report_root(args)
    if not args.dry_run:
        ok, msg = report_root.ensure_report_root(report_root_path, args.report_root_remote)
        print("publish_reference_matrix: %s" % msg, file=sys.stderr)
        if not ok:
            return 1

    wp = None
    wp_cfg = load_config()
    if not args.dry_run:
        wp = (wp_cfg or {}).get("wordpress")
        if not wp:
            print("publish_reference_matrix: no WordPress config -- run `wspy-publish configure` "
                  "first", file=sys.stderr)
            return 1

    # --dry-run must touch nothing, including the network -- WordPress discovery (item 22) is
    # read-only but still a real, slow (~50s/suite) live crawl, not something a "preview only" flag
    # should silently trigger. skip_wordpress_discovery alone still governs a real (non-dry-run) run.
    skip_discovery = args.skip_wordpress_discovery or args.dry_run
    if args.dry_run and not args.skip_wordpress_discovery:
        print("publish_reference_matrix: --dry-run also skips WordPress discovery (a real network "
              "crawl, not just a preview) -- pass --skip-wordpress-discovery explicitly if you want "
              "that noted, or drop --dry-run to actually crawl", file=sys.stderr)

    agg_cfg = {"wspy_testpoint_bin": args.wspy_testpoint_bin, "db": args.db,
               "report_root_path": report_root_path}

    new_rel_paths = []
    failures = 0
    touched_by_suite = {}

    for suite in suites:
        print("suite %s:" % suite, file=sys.stderr)
        local_cells = [] if args.skip_local_store else local_cells_for_suite(report_root_path, suite)
        wp_rows = wordpress_rows_for_suite(wp_cfg, suite, skip_discovery)

        machines = {c["machine"] for c in local_cells} | {r["machine"] for r in wp_rows}
        if machine_filter:
            machines &= machine_filter
        machines = sorted(machines)

        suite_machine_pages = []
        for machine in machines:
            entries = build_machine_rows(agg_cfg, wp_cfg, suite, machine, local_cells, wp_rows)
            markdown, wp_html = build_machine_page(suite, machine, entries)
            if markdown is None:
                print("  %s/%s: no data, skipping" % (suite, machine), file=sys.stderr)
                continue
            rel_path = os.path.join(suite, "by-machine", machine + ".md")
            write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
            print("  %s: wrote %s" % (machine, rel_path))
            levels = [(suite, suite), ("by-machine-" + machine, machine)]
            if not publish_leaf(wp, levels, wp_html, args, "%s/%s" % (suite, machine)):
                failures += 1
            else:
                suite_machine_pages.append(machine)

        if not machine_filter:
            # The suite index links every by-machine page this run touched -- only meaningful once
            # every machine in the suite has been considered, not a --machine-filtered subset.
            markdown, wp_html = build_suite_index(
                report_root_path, suite, local_cells, wp_rows, suite_machine_pages)
            rel_path = os.path.join(suite, "README.md")
            write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
            print("  suite index: wrote %s" % rel_path)
            if not publish_leaf(wp, [(suite, suite)], wp_html, args, suite):
                failures += 1

        if suite_machine_pages:
            touched_by_suite[suite] = suite_machine_pages

    if not args.suite and not machine_filter:
        # Merge in what this run itself just (or would have) generated -- under --dry-run,
        # write_generated_file() never actually writes, so a fresh report-root's disk scan alone
        # would under-report what this same invocation is previewing.
        by_suite = scan_existing_by_machine_pages(report_root_path)
        for suite, machines in touched_by_suite.items():
            by_suite[suite] = sorted(set(by_suite.get(suite, [])) | set(machines))
        markdown, wp_html = build_root_rollup(by_suite)
        rel_path = "README.md"
        write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
        print("root rollup: wrote %s" % rel_path, file=sys.stderr)
        if not publish_leaf(wp, [(ROOT_SLUG, ROOT_TITLE)], wp_html, args, "root rollup"):
            failures += 1

    if new_rel_paths:
        message = "Update reference-matrix pages: %s" % ", ".join(sorted(set(new_rel_paths)))
        if args.dry_run:
            print("publish_reference_matrix: --dry-run -- would commit %d file(s)"
                  % len(new_rel_paths), file=sys.stderr)
        else:
            ok, msg = report_root.commit_paths(report_root_path, new_rel_paths, message)
            print("publish_reference_matrix: %s" % msg, file=sys.stderr)
            if not ok:
                return 1

    if failures:
        print("publish_reference_matrix: %d page(s) failed" % failures, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
