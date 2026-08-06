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

    The slugified name is NOT guaranteed to match wspy-summary's own CSV column name for the same
    ratio (e.g. this produces "branch_miss", not the real `branch_mispredict_pct` column -- see
    doc/METRICS.md for authoritative names where exact alignment matters); the numeric value itself
    is still correct either way, just not yet cross-referenceable by name against store-based data."""
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
    "smt-contention": "contention_pct",
}
_PERCENT_RE = re.compile(r"(-?\d+(?:\.\d+)?)%")


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
    parse_comment_ratio()'s generic "<number>[%] <description>" parse. Never raises on an unexpected
    comment shape -- skips it, same best-effort contract as parse_counter_text() itself."""
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
