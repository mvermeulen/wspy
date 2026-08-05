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
