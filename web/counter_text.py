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
    sub-section headers with no value of their own (e.g. `ibs_sample_data_src_breakdown (scheme:
    ...):"); and any line whose trailing content isn't a plain (optionally `%`-suffixed) number (e.g.
    "counter coverage     57/57 measured"). An indented sub-list under a header (`ibs.txt`'s
    `data_src_breakdown`) parses like any other line -- its label just happens to have leading
    whitespace, stripped like any other label."""
    records = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("##### "):
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
# is value/slots_no_contention*100, which is what the real retire_pct/frontend_pct/backend_pct/
# speculate_pct CSV columns actually store (doc/METRICS.md's own documented wrinkle). Note the CSV
# column is "speculate" but the printed label is "speculation".
TOPDOWN_SECOND_PERCENT_LABELS = {
    "retiring": "retire_pct", "frontend": "frontend_pct", "backend": "backend_pct",
    "speculation": "speculate_pct",
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
}

# Same idea as GENERIC_LABEL_NAME_OVERRIDES above, but keyed by the *slugified description* instead
# of the line's label -- needed specifically for IPC, since its comment lives on an "instructions"
# line (print_ipc()'s own PRINT_NORMAL format), a label multiple other counter groups also reuse with
# entirely different comments (l2/l3 access rate, float density, ...), so the label alone can't
# safely identify which comment this is. "ipc" itself is a specific enough slug that this is safe.
GENERIC_SLUG_NAME_OVERRIDES = {
    "ipc": "ipc_mean",
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


def canonical_metric_name(raw_metric_name):
    """Maps a primary (not comment-derived) metric name -- currently just ibs.txt's sampling rates,
    IBS_FEATURE_NAME_OVERRIDES above -- to its real store.c SIMPLE_METRIC_FEATURES name, the same
    alignment GENERIC_LABEL_NAME_OVERRIDES/GENERIC_SLUG_NAME_OVERRIDES already give comment-derived
    ratios. Anything not in the table is returned unchanged."""
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
    TOPDOWN_SECOND_PERCENT_LABELS -- take the second (parenthetical) percentage found in the comment;
    (2) its label is one of TOPDOWN_FIRST_PERCENT_LABELS -- take the first; (3) otherwise, try
    parse_comment_ratio()'s generic "<number>[%] <description>" parse, then apply
    GENERIC_SLUG_NAME_OVERRIDES (by the parsed slug) and GENERIC_LABEL_NAME_OVERRIDES (by the line's
    own label, which wins if both apply) -- item 24's audited name alignment for every generic-tier
    case confirmed against store.c's real feature vocabulary. Never raises on an unexpected comment
    shape -- skips it, same best-effort contract as parse_counter_text() itself."""
    derived = []
    for r in records:
        comment = r.get("comment")
        if not comment:
            continue
        metric = r["metric"]
        if metric in TOPDOWN_SECOND_PERCENT_LABELS:
            matches = _PERCENT_RE.findall(comment)
            if len(matches) >= 2:
                derived.append({"metric": TOPDOWN_SECOND_PERCENT_LABELS[metric],
                                 "value": float(matches[1]), "is_percent": True, "comment": None})
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
            name = GENERIC_SLUG_NAME_OVERRIDES.get(name, name)
            name = GENERIC_LABEL_NAME_OVERRIDES.get(metric, name)
            derived.append({"metric": name, "value": value, "is_percent": is_percent, "comment": None})
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
