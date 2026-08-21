"""
web/counter_text.py -- parses wspy's human-readable (PRINT_NORMAL) counter/IBS text output
(`counters.txt`/`ibs.txt`, `topdown.c`'s `print_topdown()`/`print_ibs()` et al. in non-CSV mode) into
structured `{metric, value, is_percent, comment}` records.

INVESTIGATION.md 4.3 item 21: these files, not the two time-series CSVs (`amdtopdown.csv`/
`systemtime.csv`, which stay file-only), are what a curated "full"-depth block publishes verbatim to
WordPress as a plain `<pre class="wp-block-preformatted">` block -- this module is the other half of
recovering real metric values from an already-published page for a machine with no direct
file/SSH-reachable copy of the original run data. Nothing in this codebase parsed this text format
before now; `wspy-analyze` (the only other reader) hands it to an LLM as unstructured prose.

Deliberately best-effort, not strict: a line this module doesn't recognize (a section separator, a
sub-heading, a trailing non-numeric summary like "57/57 measured") is silently skipped rather than
raising -- this text was designed for human eyes, not machine parsing, and the reference-matrix
caller only cares about the numeric fields it *can* recover, not a guarantee of completeness.
"""
import re

_LINE_RE = re.compile(
    r"""^(?P<metric>\S.*?)          # label -- non-greedy so it stops at the value gap
        \s{2,}                      # 2+ spaces separate label from value in every real sample
        (?P<sign>-)?(?P<value>\d+(?:\.\d+)?)
        (?P<percent>%)?
        \s*
        (?:\#\s*(?P<comment>.*))?   # optional trailing "# ..." comment
        $""",
    re.VERBOSE,
)


def parse_counter_text(text):
    """Returns a list of {"metric", "value" (float), "is_percent" (bool), "comment" (str or None)}
    dicts, one per recognized line, in file order. Skips: blank lines; full-line comments
    (`# note: ...`); `topdown.c`'s `##### pass N (mask 0x..) #####...` section separators;
    sub-section headers with no value of their own; any line whose trailing content isn't a plain
    (optionally `%`-suffixed) number (e.g. "counter coverage     57/57 measured"); and the entire
    indented sub-list under an `ibs_sample_data_src_breakdown (scheme: ...):` header -- unlike other
    indented sub-lists (which parse like any other line, their label just happening to have leading
    whitespace), this one is deliberately excluded: doc/METRICS.md's own design note says this
    histogram is "never emitted as CSV -- cross-scheme category names aren't stable enough to be a
    schema'd column," but this parser had no such exclusion, so entries like "DRAM"/
    "MMIO/Config/PCI/APIC" were leaking through as if they were stable, comparable metric names
    (found live in a real reference-matrix "Other" dump, 2026-08-07)."""
    records = []
    in_data_src_breakdown = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("ibs_sample_data_src_breakdown"):
            in_data_src_breakdown = True
            continue
        if in_data_src_breakdown:
            if raw_line[:1].isspace():
                continue  # still inside the indented breakdown block
            in_data_src_breakdown = False  # a non-indented line ends it
        if line.startswith("#") or line.startswith("##### "):
            continue
        m = _LINE_RE.match(line)
        if not m:
            continue
        value = float(m.group("value"))
        if m.group("sign"):
            value = -value
        records.append({
            "metric": m.group("metric").strip(),
            "value": value,
            "is_percent": bool(m.group("percent")),
            "comment": m.group("comment").strip() if m.group("comment") else None,
        })
    return records


def _strip_qualifiers(comment):
    """Strips `topdown.c`'s own trailing "high"/"low" classification word (`print_ipc()`/
    `print_topdown()` append this after the number when a threshold is crossed) so the same ratio
    slugifies to the same metric name regardless of which side of the threshold this particular run
    landed on -- e.g. "2.38 IPC" and "3.50 IPC high" must both become metric "ipc", not "ipc"/
    "ipc_high"."""
    return re.sub(r"\s+(?:high|low)\s*$", "", comment)


_GENERIC_COMMENT_RE = re.compile(
    r"""^(?P<sign>-)?(?P<value>\d+(?:\.\d+)?)
        (?P<percent>%)?
        \s+
        (?P<desc>[A-Za-z][^()]*?)
        \s*$""",
    re.VERBOSE,
)


def _slugify(text):
    return re.sub(r"[^a-z0-9]+", "_", text.strip().lower()).strip("_")


def parse_comment_ratio(comment):
    """Best-effort extraction of a single derived (metric_name, value, is_percent) from one line's
    comment (as captured by parse_counter_text()) -- most of topdown.c's PRINT_NORMAL comments are
    self-describing enough that a generic "<number>[%] <description>" parse recovers a usable derived
    metric without per-counter-group knowledge (e.g. "8.4% icache miss rate" -> ("icache_miss_rate",
    8.4, True); "260.598 icache per 1000 inst" -> ("icache_per_1000_inst", 260.598, False)).

    Returns None for a comment this generic shape can't handle -- a bare percentage with no
    description to name a metric from (topdown.c's "-- ucode"/etc. L2 lines, smt-contention), or a
    comment starting with something other than a plain number (topdown.c's four L1 lines print
    "27.6% (47.0%) ..." -- two percentages, no bare leading description). Both of those need
    TOPDOWN_SECOND_PERCENT_LABELS/TOPDOWN_FIRST_PERCENT_LABELS' explicit label-based handling instead
    (extract_derived_ratios() below), not a generic parse.

    The slugified name is a best-effort guess, not guaranteed to match wspy-summary's own CSV column
    name for the same ratio -- extract_derived_ratios() below applies GENERIC_LABEL_NAME_OVERRIDES/
    GENERIC_SLUG_NAME_OVERRIDES on top of this function's raw output for every case confirmed against
    store.c's real feature vocabulary (item 24's audit); anything still unmapped keeps this
    function's own slug, correct in value but not yet cross-referenceable by name."""
    if not comment:
        return None
    m = _GENERIC_COMMENT_RE.match(_strip_qualifiers(comment.strip()))
    if not m:
        return None
    name = _slugify(m.group("desc"))
    if not name:
        return None
    value = float(m.group("value"))
    if m.group("sign"):
        value = -value
    return name, value, bool(m.group("percent"))


# topdown.c's print_topdown(): these labels' own comments need special handling a generic
# "<number> <description>" parse can't do. Real names/formulas confirmed directly against
# topdown.c's own PRINT_CSV branch and doc/METRICS.md, not guessed from PRINT_NORMAL text alone.
#
# retiring/frontend/backend/speculation print TWO percentages, "27.6% (47.0%) ..." -- the first is
# value/slots*100 (share of all pipeline slots, contention included), the SECOND, parenthetical one
# is value/slots_no_contention*100 -- confirmed directly against topdown.c's PRINT_CSV branch
# (print_topdown(), retiring_pct/frontend_pct/backend_pct/speculation_pct local variables), which
# prints that exact second percentage as the CSV columns retire/frontend/backend/speculate
# (%4.1f, no "_pct" suffix -- that suffix belongs to a *different* name: store.c's own
# SIMPLE_METRIC_FEATURES table promotes those same CSV columns into the run_features table under
# retire_pct/frontend_pct/backend_pct/speculate_pct, which is the name archetype.c's
# run_snapshot_apply_feature() actually recognizes for `wspy-archetype --run-guest` scoring
# (wspy-testpoint's collect_wordpress_archetype_scorecards()). Two real, different, legitimately-
# needed names for the same value -- TOPDOWN_SECOND_PERCENT_LABELS below is the archetype-facing
# one, TOPDOWN_CSV_COLUMN_NAMES is the reference-matrix-facing one (extract_derived_ratios() emits a
# derived record under both). Getting this backwards was a real bug (found live, 2026-08-07):
# recover_machine_metrics_from_wordpress() only ever emitted the "_pct" name, so a WordPress-
# recovered machine's reference-matrix cell for "backend"/"frontend" silently fell back to the raw
# primary value from the line's own operand (a giant accumulated slot count, not a percentage) --
# local wspy-store runs never had that problem since their "backend"/"frontend" CSV columns were
# never anything but the percentage in the first place. Note the CSV column is "speculate" but the
# printed label is "speculation".
TOPDOWN_SECOND_PERCENT_LABELS = {
    "retiring": "retire_pct", "frontend": "frontend_pct", "backend": "backend_pct",
    "speculation": "speculate_pct",
}
TOPDOWN_CSV_COLUMN_NAMES = {
    "retiring": "retire", "frontend": "frontend", "backend": "backend", "speculation": "speculate",
}
# Their L2 children plus smt-contention print one bare percentage with no description text to
# slugify a name from at all -- smt-contention's own comment ("29.2% ( 0.0%)") does have a second,
# parenthetical number, but topdown.c's own comment (see print_topdown()) says outright it's a
# hardcoded literal, not data, so this bucket always takes the FIRST percentage found, unlike the
# bucket above.
TOPDOWN_FIRST_PERCENT_LABELS = {
    "-- ucode": "retire_ucode_pct", "-- fastpath": "retire_fastpath_pct",
    "-- latency": "frontend_latency_pct", "-- bandwidth": "frontend_bandwidth_pct",
    "-- cpu": "backend_cpu_pct", "-- memory": "backend_memory_pct",
    "-- branch mispredict": "spec_branch_pct", "-- pipeline restart": "spec_pipeline_pct",
    # store.c's SIMPLE_METRIC_FEATURES promotes this raw CSV column ("contention_pct") under the
    # feature name "smt_contention_pct" (the name archetype.c's run_snapshot_apply_feature() actually
    # recognizes) -- confirmed by reading both files directly, not guessed; a prior version of this
    # table used the raw CSV name here instead, which archetype.c silently ignored as unrecognized.
    "smt-contention": "smt_contention_pct",
}
_PERCENT_RE = re.compile(r"(-?\d+(?:\.\d+)?)%")

# INVESTIGATION.md 4.3 item 24's own residual: labels whose comment is already correctly parsed by
# the generic "<number>[%] <description>" case above (parse_comment_ratio()) -- a plain single-number
# parse, no special extraction needed -- but whose slugified description doesn't match store.c's real
# SIMPLE_METRIC_FEATURES name for the same ratio. Confirmed entry by entry directly against store.c's
# SIMPLE_METRIC_FEATURES table and the topdown.c print function that emits each comment (not guessed
# from sample output) -- see the print function named in each comment below for the exact format
# string this was read from.
GENERIC_LABEL_NAME_OVERRIDES = {
    # print_cache() (shared cross-vendor helper), via print_dcache()'s own "L1-dcache" name.
    "L1-dcache miss": "dcache_miss_pct",
    # topdown.c's AMD-only print_topdown_fe() (the --topdown-optlb group) prints its own "icache"/
    # "icache miss" pair, distinct from print_icache()'s cross-vendor "L1-icache"/"L1-icache miss" --
    # store.c's icache_miss_pct is sourced from the *former* raw CSV column ("icache") specifically;
    # print_icache()'s own "L1-icache miss" isn't in SIMPLE_METRIC_FEATURES at all today, so it's
    # deliberately left un-renamed below (nothing to align it to yet).
    "icache miss": "icache_miss_pct",
    # Unlike icache, "opcache"/"opcache miss" is a genuine *naming collision in wspy's own C code*,
    # not just a text-parsing wrinkle: print_topdown_op() (AMD-only --topdown-optlb) and print_cache()
    # via print_opcache() (cross-vendor --opcache/cache group, topdown.c:3389) both use the identical
    # bare name "opcache" for their own, *different* real CSV columns ("opcache" vs "opcache miss"
    # respectively) -- unlike every other print_cache() caller, which each get their own distinct
    # prefixed name (print_icache()'s cross-vendor pair is "L1-icache"/"L1-icache miss", genuinely
    # distinct from print_topdown_op()'s bare "icache"/"icache miss", so no collision there). A block
    # with both groups collected has two indistinguishable "opcache miss" lines -- parse_counter_text()
    # has no positional/pass-aware context to tell them apart. Rather than guess which one a given
    # "opcache miss" line came from (found live 2026-08-07: guessing wrong here previously mapped the
    # far-more-common cross-vendor case to the wrong name, silently breaking it -- a real bug), this
    # targets the cross-vendor case's own name ("opcache miss", itself -- an identity mapping that
    # overwrites its own line's wrong raw primary value with its own comment's correct percentage,
    # same trick TOPDOWN_CSV_COLUMN_NAMES/GENERIC_LABEL_ALSO_CSV_COLUMN use elsewhere) and deliberately
    # does *not* attempt the AMD-specific bare "opcache" name at all via this recovery path -- an
    # unreliable value under a plausible-looking name is worse than no value.
    "opcache miss": "opcache miss",
    # print_l2cache(): ARM/Intel label this line "l2 miss"; AMD's own branch (a composite ratio
    # combining demand-miss and prefetch-miss sources, see topdown.c's own comment there) labels the
    # same ratio "l2 miss from l1" instead -- both comments carry the identical real l2miss value.
    "l2 miss": "l2_miss_pct",
    "l2 miss from l1": "l2_miss_pct",
    # print_l3cache() (AMD only).
    "l3 miss": "l3_miss_pct",
    # print_branch(): CSV column is literally "branch miss" (with a space) -- store.c's own real
    # feature name is branch_mispredict_pct, not the "branch_miss" a bare slugify produces. This is
    # the one that fed archetype.c's control_flow_style axis, and was verifiably wrong before this
    # entry existed (confirmed live: control_flow_style came back "unknown" for a WordPress-recovered
    # machine that had real branch data, because run_snapshot_apply_feature() didn't recognize
    # "branch_miss" as a feature name at all).
    "branch misses": "branch_mispredict_pct",
    # topdown.c's AMD-only print_topdown_fe()/print_topdown_op() groups -- the L2 (itlb2/dtlb2) rate,
    # distinct from the L1 (itlb1/dtlb1) rate on the sibling "l1 iTLB miss"/"l1 dTLB miss" lines, which
    # aren't in SIMPLE_METRIC_FEATURES and are left un-renamed.
    "l2 iTLB miss": "itlb_miss_per1k",
    "l2 dTLB miss": "dtlb_miss_per1k",
    # issue #281: the *generic*, cross-vendor pair (print_itlb()/print_dtlb() via the shared
    # print_cache() helper, --tlb group, any vendor) -- a completely different real feature from the
    # AMD-only per1k pair immediately above, despite the near-identical label text ("iTLB miss"/
    # "dTLB miss" here, no leading "l2 " or "l1 "). store.c's SIMPLE_METRIC_FEATURES sources
    # itlb_generic_miss_pct/dtlb_generic_miss_pct from CSV columns literally named "iTLB miss"/
    # "dTLB miss" -- but before this entry, the generic tier's own slugify produced "itlb_miss"/
    # "dtlb_miss" instead (confirmed live), a name archetype.c's run_snapshot_apply_feature() (the
    # memory_attribution_locus axis) never recognized -- the same "silently missing signal" shape
    # branch_mispredict_pct's own gap had before its entry above existed.
    "iTLB miss": "itlb_generic_miss_pct",
    "dTLB miss": "dtlb_generic_miss_pct",
}

# A GENERIC_LABEL_NAME_OVERRIDES entry gives one line's comment its real store.c feature name --
# but "icache miss" needs a *second* name too: topdown.c's AMD-only print_topdown_fe() (--topdown-
# optlb) has its own real CSV column literally called "icache" (icache_miss/icache_access*100,
# topdown.c:2987), matching aggregate_reference_matrix_cell()'s local-store convention -- but the
# *primary* (raw operand) value of the block's own separate line literally labeled "icache" is the
# raw access count, not this percentage (topdown.c:2994's "icache <count> # X.XXX icache per 1000
# inst" line, a different row from "icache miss" entirely). Without this, nothing ever overwrites
# that wrong raw value under the bare "icache" name -- the same class of bug TOPDOWN_CSV_COLUMN_NAMES
# above fixes for backend/frontend/retire/speculate, confirmed live against the same real report
# (2026-08-07) doc/METRICS.md's own "don't be misled by the name match" warning already called out.
# opcache doesn't get an entry here -- see GENERIC_LABEL_NAME_OVERRIDES's "opcache miss" comment
# above for why the AMD-specific bare "opcache" name is deliberately never targeted via this path at
# all (a genuine same-label collision with the cross-vendor "opcache miss" CSV column, not just an
# icache-style two-different-rows situation this table's own mechanism could safely resolve).
#
# "branch misses" (raw primary label, plural, print_branch()) is the same icache-style situation:
# the real CSV column is "branch miss" (singular, no collision with any other function -- confirmed
# only print_branch() ever emits that exact label), but nothing previously targeted it, so the
# bare-name gap matched retire/speculate's own gap before TOPDOWN_CSV_COLUMN_NAMES: not a wrong
# value under the bare name, just a *missing* one (found auditing a real "Other" dump, 2026-08-07).
GENERIC_LABEL_ALSO_CSV_COLUMN = {
    "icache miss": "icache",
    "branch misses": "branch miss",
}

# Same idea as GENERIC_LABEL_NAME_OVERRIDES above, but keyed by the *slugified description* instead
# of the line's label -- needed specifically for IPC, since its comment lives on an "instructions"
# line (print_ipc()'s own PRINT_NORMAL format), a label multiple other counter groups also reuse with
# entirely different comments (l2/l3 access rate, float density, ...), so the label alone can't
# safely identify which comment this is. "ipc" itself is a specific enough slug that this is safe.
GENERIC_SLUG_NAME_OVERRIDES = {
    "ipc": "ipc_mean",
}

# issue #278's first sub-gap: print_float()'s own PRINT_NORMAL line ("instructions ... # X.XXX float
# per 1000 inst", topdown.c, AMD-only) slugifies to "float_per_1000_inst" via the generic tier above --
# same "instructions" label collision GENERIC_SLUG_NAME_OVERRIDES's own comment already calls out
# ("float density" is this exact line). Unlike every GENERIC_SLUG_NAME_OVERRIDES entry, this can't be
# a bare rename: the printed comment is a per-1000-instruction density (topdown.c's non-csvflag branch:
# safe_div(float_all,instructions)*1000.0), while store.c's real float_pct feature
# (SIMPLE_METRIC_FEATURES, sourced from the CSV "float" column) is a *percentage* of instructions
# (topdown.c's csvflag branch, one line above: *100.0, not *1000.0) -- the identical ratio, but ten
# times the printed comment's scale. A plain name override here would silently ship a value 10x too
# small under a name archetype.c's run_snapshot_apply_feature()/vectorization_density axis trusts as a
# real percentage (confirmed live from compiler-flag-miner, which hit this gap scoring
# vectorization_density from a WordPress-recovered machine's counters.txt). The original
# "float_per_1000_inst" record is left alone (still needed by web/joblib.py's resolve_column_group(),
# which buckets it into the "float" report section for display) -- this table adds a *second*,
# percent-scaled record alongside it rather than replacing it.
GENERIC_SLUG_PER_MILLE_TO_PERCENT_NAMES = {
    "float_per_1000_inst": "float_pct",
}

# INVESTIGATION.md 4.3 item 24: ibs.txt's own sampling-rate lines are already correctly parsed as
# *primary* values (is_percent=True, not comment-derived at all -- see parse_counter_text()'s own
# docstring), but store.c's SIMPLE_METRIC_FEATURES promotes them under shorter feature names than
# their raw ibs_sample_* CSV column text (confirmed directly against store.c, not guessed). Applied
# by web/server.py's recover_machine_metrics_from_wordpress() as a final rename over every metric
# name (raw values and derived ratios alike), not here -- this dict lives alongside the others since
# it's the same underlying alignment problem, just against a primary value instead of a comment.
IBS_FEATURE_NAME_OVERRIDES = {
    "ibs_sample_dc_miss_rate": "ibs_dc_miss_pct",
    "ibs_sample_dram_rate": "ibs_dram_pct",
    "ibs_sample_dc_l1tlb_miss_rate": "ibs_dc_l1tlb_miss_pct",
    "ibs_sample_dc_l2tlb_miss_rate": "ibs_dc_l2tlb_miss_pct",
    "ibs_sample_remote_node_rate": "ibs_remote_node_pct",
}

# doc/METRICS.md documents this exact label quirk: rusage's real CSV column is "oublock"
# (ru_oublock), but topdown.c's own human-mode PRINT_NORMAL text labels that same line "onblock"
# instead. Found auditing a real reference-matrix "Other" dump (2026-08-07): the WordPress-recovery
# path was keeping this value only under the mislabeled "onblock" name, matching nothing a local
# wspy-store run would ever show under that name -- same underlying alignment problem
# IBS_FEATURE_NAME_OVERRIDES fixes for a different primary-value case, kept as its own table since
# it's not IBS-specific.
PRIMARY_LABEL_OVERRIDES = {
    "onblock": "oublock",
}


def _primary_value(records, label):
    """Returns the first record's raw primary value for the exact label `label`, or None if no such
    line is present in this block. Used only by _derive_fault_rate()/_derive_ctxswitch_rate() below,
    for the derived metrics that combine two or more separate lines' primary values rather than a
    single line's own trailing comment."""
    for r in records:
        if r["metric"] == label:
            return r["value"]
    return None


# doc/METRICS.md / store.c's SIMPLE_METRIC_FEATURES-adjacent upsert: fault_rate =
# (minflt+majflt)/elapsed_seconds. Issue #278's second sub-gap -- unlike every case
# extract_derived_ratios() otherwise handles, this ratio isn't embedded in any one line's own trailing
# comment at all: store.c's own fault_rate computation (see its "fault_rate / ctxswitch_rate" comment)
# combines THREE separate lines' raw primary values -- elapsed, minflt, majflt (all printed in the same
# rusage PRINT_NORMAL block, topdown.c:2299-2342) -- each on its own line with no cross-referencing
# comment tying them together. minflt's/majflt's own comments ("X.XX/sec", topdown.c:2339-2342) *look*
# like they'd hand this straight over, but they don't even match parse_comment_ratio()'s generic
# "<number>[%] <description>" shape (no separating whitespace before "/sec", and "/" isn't a letter) --
# confirmed live, parse_comment_ratio() returns None for both. Rather than teach the generic per-line
# parser a one-off "<number>/sec" shape just for this (inblock/onblock/nswap print the identical
# comment shape with no real feature of their own to attach it to), _derive_fault_rate() instead reads
# store.c's real formula straight off the three lines' own primary values -- the same numbers store.c
# itself sums (sum_metric_value() over the "elapsed"/"minflt"/"majflt" metric_values columns) -- exact
# parity with the local-store computation, not an approximation via re-summing two independently
# %4.2f-rounded per-second comments. Mirrors store.c's own `have_elapsed && (have_minflt ||
# have_majflt)` condition: either fault source alone is enough (a machine that only ever swaps, or
# only ever majflts, still has a real rate) -- only elapsed==0 (never a real run) or neither fault
# column present at all skips emission entirely.
def _derive_fault_rate(records):
    elapsed = _primary_value(records, "elapsed")
    minflt = _primary_value(records, "minflt")
    majflt = _primary_value(records, "majflt")
    if not elapsed or (minflt is None and majflt is None):
        return []
    return [{"metric": "fault_rate", "value": ((minflt or 0.0) + (majflt or 0.0)) / elapsed,
             "is_percent": False, "comment": None}]


# issue #281: store.c's ctxswitch_rate = (nvcsw+nivcsw)/elapsed_seconds (store.c:1143) -- the exact
# same cross-line shape fault_rate above is, and for the same reason: nvcsw's/nivcsw's own comments
# ("X.XX%", topdown.c:2329-2332) aren't a rate at all, they're each switch type's *share of
# nvcsw+nivcsw's own sum* (voluntary vs. involuntary split) -- a real ratio, just not this one, so
# there's no comment anywhere in the block this value could be recovered from even if the generic
# parser were taught to read it. Reads elapsed/nvcsw/nivcsw straight off their own primary values,
# same store.c-parity approach _derive_fault_rate() uses, and mirrors its own `have_elapsed &&
# (have_nvcsw || have_nivcsw)` condition -- either switch type alone is a real rate.
def _derive_ctxswitch_rate(records):
    elapsed = _primary_value(records, "elapsed")
    nvcsw = _primary_value(records, "nvcsw")
    nivcsw = _primary_value(records, "nivcsw")
    if not elapsed or (nvcsw is None and nivcsw is None):
        return []
    return [{"metric": "ctxswitch_rate", "value": ((nvcsw or 0.0) + (nivcsw or 0.0)) / elapsed,
             "is_percent": False, "comment": None}]


def canonical_metric_name(raw_metric_name):
    """Maps a primary (not comment-derived) metric name -- ibs.txt's sampling rates
    (IBS_FEATURE_NAME_OVERRIDES) and rusage's "onblock"/"oublock" label quirk
    (PRIMARY_LABEL_OVERRIDES) -- to its real store.c/topdown.c CSV name, the same alignment
    GENERIC_LABEL_NAME_OVERRIDES/GENERIC_SLUG_NAME_OVERRIDES already give comment-derived ratios.
    Anything not in either table is returned unchanged."""
    if raw_metric_name in PRIMARY_LABEL_OVERRIDES:
        return PRIMARY_LABEL_OVERRIDES[raw_metric_name]
    return IBS_FEATURE_NAME_OVERRIDES.get(raw_metric_name, raw_metric_name)


def extract_derived_ratios(records):
    """Given parse_counter_text()'s own records for one counters.txt/ibs.txt block, returns
    additional {metric, value, is_percent, comment} records for ratios embedded in each line's
    *comment* rather than its primary value (INVESTIGATION.md 4.3 item 24) -- topdown.c's human-text
    mode prints both the raw operand (as the line's value) and a normalized ratio/rate (as a trailing
    comment), and it's the ratio that's actually comparable across differently-scaled/multiplexed
    runs (doc/METRICS.md: "this is why we store ipc and not raw instructions/cpu-cycles separately").
    Every returned record has comment=None (it IS the derived value, nothing further to attach).

    Three cases per record, checked in this order: (1) its metric label is one of
    TOPDOWN_SECOND_PERCENT_LABELS -- take the second (parenthetical) percentage found in the
    comment, and emit it under *both* that label's archetype-facing "_pct" name and its
    TOPDOWN_CSV_COLUMN_NAMES reference-matrix-facing name (see that table's own comment for why
    both are real, independently-needed names for the same value); (2) its label is one of
    TOPDOWN_FIRST_PERCENT_LABELS -- take the first; (3) otherwise, try parse_comment_ratio()'s
    generic "<number>[%] <description>" parse. If the parsed slug is one of
    GENERIC_SLUG_PER_MILLE_TO_PERCENT_NAMES (issue #278: a per-1000-inst density printed where the
    real store.c feature is the same ratio as a percentage, currently only print_float()'s "float"
    line), also emit that /10-converted percentage under its own name, alongside -- not instead of --
    the generic parse's own record. Then apply GENERIC_SLUG_NAME_OVERRIDES (by the parsed slug) and
    GENERIC_LABEL_NAME_OVERRIDES (by the line's own label, which wins if both apply) to the generic
    parse's own record -- item 24's audited name alignment for every generic-tier case confirmed
    against store.c's real feature vocabulary. Never raises on an unexpected comment shape -- skips
    it, same best-effort contract as parse_counter_text() itself.

    After the per-record pass, also runs _derive_fault_rate()/_derive_ctxswitch_rate() once each over
    the whole block (issues #278/#281) -- neither is one line's comment at all, each is a combination
    of separate lines' own primary values (elapsed+minflt+majflt, elapsed+nvcsw+nivcsw respectively),
    so neither can be produced by the per-record loop above no matter which of its three cases
    applies."""
    derived = []
    for r in records:
        comment = r.get("comment")
        if not comment:
            continue
        metric = r["metric"]
        if metric in TOPDOWN_SECOND_PERCENT_LABELS:
            matches = _PERCENT_RE.findall(comment)
            if len(matches) >= 2:
                value = float(matches[1])
                derived.append({"metric": TOPDOWN_SECOND_PERCENT_LABELS[metric],
                                 "value": value, "is_percent": True, "comment": None})
                derived.append({"metric": TOPDOWN_CSV_COLUMN_NAMES[metric],
                                 "value": value, "is_percent": True, "comment": None})
            continue
        if metric in TOPDOWN_FIRST_PERCENT_LABELS:
            matches = _PERCENT_RE.findall(comment)
            if matches:
                derived.append({"metric": TOPDOWN_FIRST_PERCENT_LABELS[metric],
                                 "value": float(matches[0]), "is_percent": True, "comment": None})
            continue
        parsed = parse_comment_ratio(comment)
        if parsed:
            name, value, is_percent = parsed
            pct_name = GENERIC_SLUG_PER_MILLE_TO_PERCENT_NAMES.get(name)
            if pct_name:
                derived.append({"metric": pct_name, "value": value / 10.0,
                                 "is_percent": True, "comment": None})
            name = GENERIC_SLUG_NAME_OVERRIDES.get(name, name)
            name = GENERIC_LABEL_NAME_OVERRIDES.get(metric, name)
            derived.append({"metric": name, "value": value, "is_percent": is_percent, "comment": None})
            if metric in GENERIC_LABEL_ALSO_CSV_COLUMN:
                derived.append({"metric": GENERIC_LABEL_ALSO_CSV_COLUMN[metric], "value": value,
                                 "is_percent": is_percent, "comment": None})
    derived.extend(_derive_fault_rate(records))
    derived.extend(_derive_ctxswitch_rate(records))
    return derived


def classify_counter_text(text):
    """Best-effort guess at which curated artifact `text` came from, by content shape rather than an
    explicit filename tag -- full-depth curated blocks carry no filename/note at all once published
    (only summary/excerpt depth does, per export_block_content()). Returns "counters", "ibs", or None
    (unrecognized -- e.g. a process-tree dump or some other curated text file, not a counter dump at
    all) so a caller can skip blocks that aren't either format rather than mis-parsing them."""
    if re.search(r"^ibs_sample_", text, re.MULTILINE):
        return "ibs"
    if re.search(r"^#####\s+pass\s+\d+", text, re.MULTILINE):
        return "counters"
    return None
