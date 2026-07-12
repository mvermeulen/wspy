#!/usr/bin/env python3
"""
wspy web launcher + report browser (INVESTIGATION_4.0.md 4.1 Tier 2, items 6-7).

Two launchers on one page:

- Item 6, the fixed-configuration slice: always runs the "amdtopdown" pass
  wspy-run's deep-cpu/deep-gpu profiles already use --

      wspy --csv --interval 1 --topdown --no-rusage --no-software --no-ipc

  -- followed by workload/phoronix/gnuplot.sh's amdtopdown.csv -> amdtopdown.png
  plot block. No preset picker, no configuration/option checklist.
- Item 7, the wspy-run profile launcher: a thin form over wspy-run's own
  existing surface (builtin profile(s) + suite/benchmark/workload), so real
  varied runs exist to browse before #8's curation studio is built. Mirrors
  workload/phoronix/run_test.sh's own pattern -- call wspy-run, then
  best-effort run gnuplot.sh only if the chosen profile produced
  amdtopdown.csv.

Both are thin clients: every run launches exactly the command line(s) a user
could type by hand, shown before running them. No preset/configuration/option
checklist yet for either (that's #9), no wspy-validate/wspy-store/wspy-summary
coverage (also #9). The report browser keeps no state of its own -- it reads
whatever landed in a run directory straight off disk, dispatching on whether
wspy-run's own run-level manifest.json is present.

Usage:
    web/server.py [--host HOST] [--port PORT] [--wspy PATH] [--wspy-run PATH]
                   [--gnuplot-script PATH] [--output-root DIR]

Stdlib only, by design (see CLAUDE.md's web/ entry for the reasoning).
"""
import argparse
import html
import json
import os
import queue
import re
import secrets
import shlex
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

# The one fixed configuration item 6 knows about -- matches wspy-run's
# deep-cpu/deep-gpu "amdtopdown" pass exactly (wspy-run, load_builtin_profile()).
WSPY_FIXED_ARGS = ["--csv", "--interval", "1", "--topdown",
                    "--no-rusage", "--no-software", "--no-ipc"]
CSV_NAME = "amdtopdown.csv"
MANIFEST_NAME = "amdtopdown.manifest.json"
PNG_NAME = "amdtopdown.png"
LOG_NAME = "launch.log"
ARTIFACT_FILES = (CSV_NAME, MANIFEST_NAME, PNG_NAME, LOG_NAME)

# wspy-run's own unified-layout artifacts (item 7) -- see wspy-run's
# generate_summary()/generate_manifest(). RUN_MANIFEST_NAME's presence in a
# run directory is what distinguishes an item-7 (wspy-run) report from an
# item-6 (fixed-config) one -- the two never collide since item 6 never
# writes a bare "manifest.json" (its own manifest is amdtopdown.manifest.json).
RUN_MANIFEST_NAME = "manifest.json"
SUMMARY_NAME = "summary.txt"
TOPLEVEL_MARKER_FILES = ARTIFACT_FILES + (RUN_MANIFEST_NAME, SUMMARY_NAME)

# wspy-run's own builtin profile catalog (wspy-run, BUILTIN_PROFILES) --
# offered as a datalist in the UI; wspy-run itself is still the source of
# truth and rejects anything else, so this list is a convenience, not a gate.
BUILTIN_PROFILES = ("quick", "deep-cpu", "deep-cpu-intel", "deep-gpu",
                     "tree-heavy", "ibs-basic", "ibs-memory-deep")

NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
PROFILE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]+$")

# ---------------------------------------------------------------------------
# Run registry: in-memory only, purely to relay a run's live log lines to an
# SSE stream while it's in flight. Nothing here is authoritative -- once a
# run finishes, the report page reads its directory off disk like any other,
# so a server restart mid-run loses only the live tail, not the artifacts.
# ---------------------------------------------------------------------------

class RunState:
    def __init__(self, rundir):
        self.rundir = rundir
        self.lines = []
        self.status = "running"  # running | done | error
        self.report_url = None
        self.cond = threading.Condition()

    def append(self, line):
        with self.cond:
            self.lines.append(line)
            self.cond.notify_all()

    def finish(self, status, report_url):
        with self.cond:
            self.status = status
            self.report_url = report_url
            self.cond.notify_all()


RUNS = {}
RUNS_LOCK = threading.Lock()


def run_key(suite, benchmark, run_id):
    return (suite, benchmark, run_id)


def valid_segment(s):
    return bool(s) and bool(NAME_RE.match(s)) and s not in (".", "..")


def make_run_id():
    # Same shape as wspy-run's own <timestamp>.<ms>-<suffix> run ids (see
    # run_index.c's format_run_id()), but this server is long-running so a
    # per-request pid isn't unique the way wspy-run's own "$$" is -- a short
    # random hex suffix stands in for it instead.
    now = datetime.now(timezone.utc)
    ms = now.microsecond // 1000
    return f"{now.strftime('%Y%m%dT%H%M%S')}.{ms:03d}-{secrets.token_hex(4)}"


def default_benchmark_from_workload(argv):
    base = os.path.basename(argv[0]) if argv else "workload"
    base = re.sub(r"[^A-Za-z0-9_.-]", "_", base)
    return base or "workload"


# ---------------------------------------------------------------------------
# Command construction -- the same strings are used to (a) show the user what
# will run, and (b) actually run it, so the preview is never a lie.
# ---------------------------------------------------------------------------

def build_wspy_argv(wspy_bin, rundir, workload_argv):
    csv_path = os.path.join(rundir, CSV_NAME)
    manifest_path = os.path.join(rundir, MANIFEST_NAME)
    return ([wspy_bin] + WSPY_FIXED_ARGS +
            ["-o", csv_path, "--manifest", manifest_path, "--"] + workload_argv)


def build_gnuplot_argv(gnuplot_script):
    return ["bash", gnuplot_script]


def valid_profile_spec(spec):
    tokens = spec.split(",")
    return bool(tokens) and all(PROFILE_TOKEN_RE.match(t) for t in tokens)


def build_wspy_run_argv(wspy_run_bin, wspy_bin, output_root, suite, benchmark,
                         run_id, profile, workload_argv):
    return ([wspy_run_bin, "--wspy", wspy_bin, "-o", output_root,
              "--suite", suite, "--benchmark", benchmark, "--run-id", run_id,
              profile, "--"] + workload_argv)


def shell_preview(argv, cwd=None):
    s = shlex.join(argv)
    if cwd:
        return f"(cd {shlex.quote(cwd)} && {s})"
    return s


# ---------------------------------------------------------------------------
# Run execution
# ---------------------------------------------------------------------------

def execute_run(state, wspy_bin, gnuplot_script, rundir, workload_argv):
    log_path = os.path.join(rundir, LOG_NAME)
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    wspy_argv = build_wspy_argv(wspy_bin, rundir, workload_argv)
    emit("$ " + shell_preview(wspy_argv))
    try:
        proc = subprocess.Popen(wspy_argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
    except OSError as e:
        emit(f"[error] failed to launch wspy ({wspy_bin}): {e}")
        logf.close()
        state.finish("error", None)
        return

    for line in proc.stdout:
        emit(line.rstrip("\n"))
    wspy_rc = proc.wait()
    emit(f"[wspy exited {wspy_rc}]")

    csv_path = os.path.join(rundir, CSV_NAME)
    ok = wspy_rc == 0 and os.path.exists(csv_path) and os.path.getsize(csv_path) > 0

    if not ok:
        emit("[skipping gnuplot: no usable CSV output]")
        logf.close()
        state.finish("error", None)
        return

    gnuplot_argv = build_gnuplot_argv(gnuplot_script)
    emit("$ " + shell_preview(gnuplot_argv, cwd=rundir))
    try:
        proc = subprocess.Popen(gnuplot_argv, cwd=rundir,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        for line in proc.stdout:
            emit(line.rstrip("\n"))
        gnuplot_rc = proc.wait()
        emit(f"[gnuplot exited {gnuplot_rc}]")
    except OSError as e:
        emit(f"[error] failed to launch gnuplot script ({gnuplot_script}): {e}")
        gnuplot_rc = 1

    logf.close()
    status = "done" if gnuplot_rc == 0 else "error"
    state.finish(status, None)


def execute_profile_run(state, cfg, rundir, suite, benchmark, run_id, profile, workload_argv):
    """Item 7: invoke wspy-run itself (rather than wspy directly) for one of
    its builtin profiles, then -- mirroring workload/phoronix/run_test.sh's
    own hand-written pattern -- best-effort run gnuplot.sh afterward only if
    the chosen profile actually produced amdtopdown.csv (true for deep-cpu/
    deep-gpu, false for e.g. deep-cpu-intel/quick/tree-heavy/ibs-*)."""
    log_path = os.path.join(rundir, LOG_NAME)
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    wspy_run_argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"],
                                         cfg["output_root"], suite, benchmark,
                                         run_id, profile, workload_argv)
    emit("$ " + shell_preview(wspy_run_argv))
    try:
        proc = subprocess.Popen(wspy_run_argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
    except OSError as e:
        emit(f"[error] failed to launch wspy-run ({cfg['wspy_run_bin']}): {e}")
        logf.close()
        state.finish("error", None)
        return

    for line in proc.stdout:
        emit(line.rstrip("\n"))
    wspy_run_rc = proc.wait()
    emit(f"[wspy-run exited {wspy_run_rc}]")

    csv_path = os.path.join(rundir, CSV_NAME)
    has_topdown_csv = os.path.exists(csv_path) and os.path.getsize(csv_path) > 0

    if not has_topdown_csv:
        emit("[skipping gnuplot: chosen profile did not produce amdtopdown.csv]")
        logf.close()
        state.finish("done" if wspy_run_rc == 0 else "error", None)
        return

    gnuplot_argv = build_gnuplot_argv(cfg["gnuplot_script"])
    emit("$ " + shell_preview(gnuplot_argv, cwd=rundir))
    try:
        proc = subprocess.Popen(gnuplot_argv, cwd=rundir,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        for line in proc.stdout:
            emit(line.rstrip("\n"))
        gnuplot_rc = proc.wait()
        emit(f"[gnuplot exited {gnuplot_rc}]")
    except OSError as e:
        emit(f"[error] failed to launch gnuplot script ({cfg['gnuplot_script']}): {e}")
        gnuplot_rc = 1

    logf.close()
    status = "done" if wspy_run_rc == 0 and gnuplot_rc == 0 else "error"
    state.finish(status, None)


# ---------------------------------------------------------------------------
# Report discovery -- scans OUTPUT_ROOT for suite/benchmark/run_id
# directories holding at least one known artifact, newest first. This is
# what lets a report be found without knowing its path by heart.
# ---------------------------------------------------------------------------

def discover_reports(output_root, limit=50):
    reports = []
    if not os.path.isdir(output_root):
        return reports
    for suite in sorted(os.listdir(output_root)):
        suite_dir = os.path.join(output_root, suite)
        if not os.path.isdir(suite_dir):
            continue
        for benchmark in sorted(os.listdir(suite_dir)):
            bench_dir = os.path.join(suite_dir, benchmark)
            if not os.path.isdir(bench_dir):
                continue
            for run_id in sorted(os.listdir(bench_dir)):
                run_dir = os.path.join(bench_dir, run_id)
                if not os.path.isdir(run_dir):
                    continue
                if not any(os.path.exists(os.path.join(run_dir, f)) for f in TOPLEVEL_MARKER_FILES):
                    continue
                mtime = os.path.getmtime(run_dir)
                reports.append({
                    "suite": suite, "benchmark": benchmark, "run_id": run_id,
                    "mtime": mtime,
                })
    reports.sort(key=lambda r: r["mtime"], reverse=True)
    return reports[:limit]


def read_manifest_workload(manifest_path):
    """Best-effort: pull the workload command back out of a manifest.json's
    command.argv (manifest.c writes just the workload's own argv there, not
    wspy's own flags -- see wspy.c main()'s command_line_argv), so 'customize
    & run again' can prefill the launcher without re-parsing anything else."""
    try:
        with open(manifest_path) as f:
            data = json.load(f)
        argv = data.get("command", {}).get("argv", [])
        return argv or None
    except (OSError, json.JSONDecodeError, ValueError):
        pass
    return None


def read_run_manifest(run_manifest_path):
    """Parse wspy-run's own run-level manifest.json (wspy-run's
    generate_manifest() -- a different, simpler shape than a per-pass
    --manifest: top-level command is a bare argv array, not {"argv": [...]},
    and passes[] lists name/output/manifest/status for each pass wspy-run ran."""
    try:
        with open(run_manifest_path) as f:
            data = json.load(f)
        if not isinstance(data.get("passes"), list):
            return None
        return data
    except (OSError, json.JSONDecodeError, ValueError):
        return None


def guess_content_type(filename):
    ext = os.path.splitext(filename)[1].lower()
    return {
        ".csv": "text/csv",
        ".json": "application/json",
        ".png": "image/png",
        ".txt": "text/plain",
        ".log": "text/plain",
    }.get(ext, "application/octet-stream")


# ---------------------------------------------------------------------------
# Curation studio (item 8, INVESTIGATION_4.0.md): review every artifact a run
# collected on one page and curate a subset of it -- select, reorder, and
# annotate per configuration -- into a block sequence that #10's export will
# later consume. State lives in <rundir>/curation.json, one more file the run
# directory holds alongside the artifacts it curates; nothing server-owned
# that isn't reconstructible from disk, same principle #6/#7 already follow.
#
# A block is either "artifact" (backed by one file already in the run
# directory -- the same file can back more than one block instance, each
# under its own title, e.g. separate "AMD results"/"Intel results" sections
# built from the same counters.txt) or "freeform" (commentary only, no
# artifact). "depth" is the single inclusion control per the spec's own
# framing ("not just an all-or-nothing toggle"): "none" excludes the block
# from the curated/exported view (it stays in the studio, not deleted);
# images and freeform text only support none/full (there's no meaningful
# partial rendering of either); csv/text/json also support summary (a short
# peek) and excerpt (first N lines, N user-configurable) for artifacts too
# large to sensibly ship in full -- a process tree is the concrete case the
# spec calls out, but the same logic applies uniformly to any text-shaped
# artifact rather than special-casing tree files specifically.
# ---------------------------------------------------------------------------

CURATION_NAME = "curation.json"
CURATION_SCHEMA_VERSION = "1.0"
DEFAULT_EXCERPT_LINES = 40
MAX_INLINE_BYTES = 5 * 1024 * 1024  # don't embed a pathologically large file even at depth=full

DEPTH_OPTIONS_BY_KIND = {
    "image": ("none", "full"),
    "csv": ("none", "summary", "excerpt", "full"),
    "text": ("none", "summary", "excerpt", "full"),
    "json": ("none", "summary", "excerpt", "full"),
    "binary": ("none", "full"),
    "freeform": ("none", "full"),
}


def guess_kind(filename):
    ext = os.path.splitext(filename)[1].lower()
    if ext == ".png":
        return "image"
    if ext == ".csv":
        return "csv"
    if ext == ".json":
        return "json"
    # Unknown extensions are tentatively "text"; read_text_safely() downgrades
    # to a link-only render if the file doesn't actually decode as UTF-8,
    # rather than requiring every candidate file to be read just to list it.
    return "text"


def allowed_depths(block):
    if block.get("kind") == "freeform":
        return DEPTH_OPTIONS_BY_KIND["freeform"]
    return DEPTH_OPTIONS_BY_KIND.get(block.get("source_kind"), ("none", "full"))


def new_block(kind, source_file=None, source_kind=None, title=None):
    depths = DEPTH_OPTIONS_BY_KIND["freeform"] if kind == "freeform" else \
        DEPTH_OPTIONS_BY_KIND.get(source_kind, ("none", "full"))
    return {
        "id": secrets.token_hex(4),
        "kind": kind,  # "artifact" | "freeform"
        "source_file": source_file,
        "source_kind": source_kind,
        "title": title or (source_file or "New section"),
        "depth": "full" if "full" in depths else depths[-1],
        "excerpt_lines": DEFAULT_EXCERPT_LINES,
        "commentary": "",
    }


def collect_run_files(rundir):
    """Every file in a run directory worth offering as a block source, in a
    sensible default order -- wspy-run's own passes (name-labeled) first when
    a run-level manifest exists, else item 6's fixed amdtopdown.* shape, then
    curation.json/wspy-run's own manifest/log, then anything else sitting in
    the directory that neither claims (mirrors render_wspy_run_report's own
    "Other artifacts" scan, generalized for reuse here)."""
    run_manifest = read_run_manifest(os.path.join(rundir, RUN_MANIFEST_NAME))
    seen = set()
    items = []

    def add(filename, label):
        if not filename or filename in seen:
            return
        if not os.path.isfile(os.path.join(rundir, filename)):
            return
        seen.add(filename)
        items.append({"filename": filename, "kind": guess_kind(filename), "label": label})

    if run_manifest is not None:
        for p in run_manifest.get("passes", []):
            name = p.get("name", "?")
            if p.get("output"):
                add(p["output"], f"{name}: {p['output']}")
            if p.get("manifest"):
                add(p["manifest"], f"{name}: manifest")
        add(SUMMARY_NAME, "summary (concatenated pass output)")
        add(RUN_MANIFEST_NAME, "wspy-run run manifest")
        add(LOG_NAME, "launch log")
    else:
        add(PNG_NAME, "topdown plot")
        add(CSV_NAME, "amdtopdown.csv")
        add(MANIFEST_NAME, "manifest")
        add(LOG_NAME, "launch log")

    try:
        extras = sorted(
            f for f in os.listdir(rundir)
            if f not in seen and f != CURATION_NAME and os.path.isfile(os.path.join(rundir, f))
        )
    except OSError:
        extras = []
    for f in extras:
        add(f, f)

    return items


def load_curation(rundir):
    try:
        with open(os.path.join(rundir, CURATION_NAME)) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return None
    if not isinstance(data.get("blocks"), list):
        return None
    return data


def save_curation(rundir, data):
    data["schema_version"] = CURATION_SCHEMA_VERSION
    data["updated"] = datetime.now(timezone.utc).isoformat()
    data.setdefault("created", data["updated"])
    path = os.path.join(rundir, CURATION_NAME)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def read_text_safely(path):
    """Returns (text, size) if the file decodes as UTF-8, else (None, size)
    so callers can fall back to a link-only render for binary artifacts."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return None, 0
    try:
        return raw.decode("utf-8"), size
    except UnicodeDecodeError:
        return None, size


def render_block_content(rundir, base_url, block):
    """Renders one block's artifact at its chosen depth. Empty string at
    depth=none or for a freeform block (whose only content is its own
    commentary, rendered by the caller, not here)."""
    depth = block.get("depth", "none")
    if depth == "none" or block.get("kind") == "freeform":
        return ""

    filename = block.get("source_file")
    if not filename:
        return '<p class="muted">(missing source)</p>'
    path = os.path.join(rundir, filename)
    if not os.path.isfile(path):
        return f'<p class="muted">{html.escape(filename)} not found</p>'
    url = f"{base_url}/{_urlescape(filename)}"
    kind = block.get("source_kind")

    if kind == "image":
        return f'<img class="plot" src="{url}" alt="{html.escape(filename)}">' if depth == "full" else ""

    text, size = read_text_safely(path)
    if text is None:
        return (f'<p class="muted"><a href="{url}">{html.escape(filename)}</a> '
                f'({size} bytes, binary &mdash; linked, not embedded)</p>')

    lines = text.splitlines()
    n = len(lines)
    full_link = f'<p class="muted"><a href="{url}">full file</a></p>'

    if depth == "summary":
        peek = "\n".join(lines[:3])
        extra = f" &middot; {len(lines[0].split(','))} columns" if kind == "csv" and n else ""
        if kind == "json":
            try:
                obj = json.loads(text)
                if isinstance(obj, dict):
                    extra = " &middot; keys: " + ", ".join(list(obj.keys())[:12])
            except ValueError:
                pass
        return (f'<p class="muted">{n} lines &middot; {size} bytes{extra}</p>'
                f'<pre>{html.escape(peek)}</pre>{full_link}')

    if depth == "excerpt":
        excerpt_n = block.get("excerpt_lines") or DEFAULT_EXCERPT_LINES
        shown = "\n".join(lines[:excerpt_n])
        note = (f'<p class="muted">showing first {min(excerpt_n, n)} of {n} lines &middot; '
                f'<a href="{url}">full file</a></p>') if n > excerpt_n else ""
        return f'<pre>{html.escape(shown)}</pre>{note}'

    # depth == "full"
    if size > MAX_INLINE_BYTES:
        return (f'<p class="muted">{size} bytes, too large to embed inline &mdash; '
                f'<a href="{url}">full file</a></p>')
    return f'<pre>{html.escape(text)}</pre>'


def render_curated_section(rundir, base_url, suite, benchmark, run_id):
    curation = load_curation(rundir)
    if not curation:
        return ""
    included = [b for b in curation.get("blocks", []) if b.get("depth", "none") != "none"]
    if not included:
        return ""
    parts = ["<h2>Curated view</h2>"]
    for b in included:
        parts.append('<div class="block">')
        parts.append(f"<h3>{html.escape(b.get('title') or '(untitled)')}</h3>")
        if b.get("commentary"):
            parts.append(f'<p class="commentary">{html.escape(b["commentary"])}</p>')
        parts.append(render_block_content(rundir, base_url, b))
        parts.append("</div>")
    return "".join(parts)


# ---------------------------------------------------------------------------
# HTML rendering
# ---------------------------------------------------------------------------

def page(title, body):
    return f"""<!doctype html>
<html><head><meta charset="utf-8">
<title>{html.escape(title)}</title>
<link rel="stylesheet" href="/static/style.css">
</head><body>
<header><a href="/" class="brand">wspy web</a></header>
<main>{body}</main>
</body></html>"""


def render_index(output_root, prefill, wspy_bin, wspy_run_bin, gnuplot_script):
    reports = discover_reports(output_root)
    rows = []
    for r in reports:
        ts = datetime.fromtimestamp(r["mtime"]).strftime("%Y-%m-%d %H:%M:%S")
        url = f"/report/{r['suite']}/{r['benchmark']}/{r['run_id']}"
        key = f"{r['suite']}/{r['benchmark']}/{r['run_id']}"
        rows.append(
            f'<tr><td><input type="checkbox" name="r" value="{html.escape(key)}"></td>'
            f"<td>{html.escape(ts)}</td>"
            f"<td>{html.escape(r['suite'])}</td>"
            f"<td>{html.escape(r['benchmark'])}</td>"
            f"<td><a href=\"{html.escape(url)}\">{html.escape(r['run_id'])}</a></td></tr>"
        )
    reports_html = (
        '<form method="get" action="/compare">'
        "<table class=\"reports\"><thead><tr><th></th><th>when</th><th>suite</th>"
        "<th>benchmark</th><th>run</th></tr></thead><tbody>" + "".join(rows) +
        "</tbody></table>"
        '<button type="submit">Compare selected</button>'
        "</form>" if rows else "<p class=\"muted\">No runs yet.</p>"
    )

    w_workload = html.escape(prefill.get("workload", ""))
    w_suite = html.escape(prefill.get("suite", "manual"))
    w_benchmark = html.escape(prefill.get("benchmark", ""))

    p_workload = html.escape(prefill.get("profile_workload", ""))
    p_suite = html.escape(prefill.get("profile_suite", "manual"))
    p_benchmark = html.escape(prefill.get("profile_benchmark", ""))
    profile_options = "".join(f'<option value="{html.escape(p)}">' for p in BUILTIN_PROFILES)

    body = f"""
<section class="panel">
  <h1>Launcher: fixed configuration</h1>
  <p class="config-label">Configuration: <code>amdtopdown</code>
     (fixed for this slice &mdash; the preset/configuration picker lands here later)</p>
  <form id="run-form" data-output-root="{html.escape(output_root)}"
        data-wspy="{html.escape(wspy_bin)}" data-gnuplot="{html.escape(gnuplot_script)}">
    <label>Workload command
      <input type="text" id="workload" name="workload" value="{w_workload}"
             placeholder="e.g. sleep 5" required>
    </label>
    <div class="row">
      <label>Suite
        <input type="text" id="suite" name="suite" value="{w_suite}">
      </label>
      <label>Benchmark
        <input type="text" id="benchmark" name="benchmark" value="{w_benchmark}"
               placeholder="(defaults to workload's program name)">
      </label>
      <label>Run id
        <input type="text" id="run_id" name="run_id" placeholder="(auto)">
      </label>
    </div>
    <fieldset class="preview">
      <legend>Commands about to run (copy/paste-able)</legend>
      <pre id="preview-wspy"></pre>
      <pre id="preview-gnuplot"></pre>
    </fieldset>
    <button type="submit" id="run-button">Run</button>
  </form>
  <pre id="live-output" class="live-output" hidden></pre>
  <p id="run-result"></p>
</section>
<section class="panel">
  <h1>Launcher: wspy-run profile</h1>
  <p class="config-label">Runs a named <code>wspy-run</code> profile (or comma-composed list) against
     a workload &mdash; no ad-hoc counter/option checklist yet, just <code>wspy-run</code>'s own
     existing presets.</p>
  <form id="profile-run-form" data-output-root="{html.escape(output_root)}"
        data-wspy="{html.escape(wspy_bin)}" data-wspy-run="{html.escape(wspy_run_bin)}"
        data-gnuplot="{html.escape(gnuplot_script)}">
    <label>Profile(s) <span class="muted">(comma-separated, e.g. <code>deep-cpu,tree-heavy</code>)</span>
      <input type="text" id="p_profile" name="profile" list="profile-list"
             placeholder="e.g. deep-cpu" required>
      <datalist id="profile-list">{profile_options}</datalist>
    </label>
    <label>Workload command
      <input type="text" id="p_workload" name="workload" value="{p_workload}"
             placeholder="e.g. sleep 5" required>
    </label>
    <div class="row">
      <label>Suite
        <input type="text" id="p_suite" name="suite" value="{p_suite}">
      </label>
      <label>Benchmark
        <input type="text" id="p_benchmark" name="benchmark" value="{p_benchmark}"
               placeholder="(defaults to workload's program name)">
      </label>
      <label>Run id
        <input type="text" id="p_run_id" name="run_id" placeholder="(auto)">
      </label>
    </div>
    <fieldset class="preview">
      <legend>Command about to run (copy/paste-able)</legend>
      <pre id="p-preview-wspy"></pre>
      <p class="muted">gnuplot.sh runs afterward, best-effort, only if the chosen profile produces
         amdtopdown.csv (e.g. deep-cpu/deep-gpu).</p>
    </fieldset>
    <button type="submit" id="p-run-button">Run</button>
  </form>
  <pre id="p-live-output" class="live-output" hidden></pre>
  <p id="p-run-result"></p>
</section>
<section class="panel">
  <h2>Recent reports</h2>
  {reports_html}
</section>
<script src="/static/app.js"></script>
"""
    return page("wspy web launcher", body)


def apply_studio_post(rundir, form):
    """Reconstructs the block list from a studio form submission (in DOM/
    submission order, which is the current on-page order) and applies the one
    op the clicked submit button carries. Every submit button lives inside
    the same <form> as every block's editable fields, so a reorder/add/
    delete click also persists whatever title/depth/commentary edits were
    pending on the other blocks -- there's no separate "save" step to forget."""
    ids = form.get("id", [])
    kinds = form.get("kind", [])
    source_files = form.get("source_file", [])
    source_kinds = form.get("source_kind", [])
    titles = form.get("title", [])
    depths = form.get("depth", [])
    excerpt_lines = form.get("excerpt_lines", [])
    commentaries = form.get("commentary", [])
    op = (form.get("op", [""])[0] or "save")

    blocks = []
    for i in range(len(ids)):
        try:
            excerpt_n = max(1, min(5000, int(excerpt_lines[i])))
        except (IndexError, ValueError):
            excerpt_n = DEFAULT_EXCERPT_LINES
        blocks.append({
            "id": ids[i],
            "kind": kinds[i] if i < len(kinds) else "artifact",
            "source_file": (source_files[i] if i < len(source_files) else "") or None,
            "source_kind": (source_kinds[i] if i < len(source_kinds) else "") or None,
            "title": titles[i] if i < len(titles) else "",
            "depth": depths[i] if i < len(depths) else "none",
            "excerpt_lines": excerpt_n,
            "commentary": commentaries[i] if i < len(commentaries) else "",
        })

    if op.startswith("up:") or op.startswith("down:"):
        bid = op.split(":", 1)[1]
        idx = next((j for j, b in enumerate(blocks) if b["id"] == bid), None)
        if idx is not None:
            other = idx - 1 if op.startswith("up:") else idx + 1
            if 0 <= other < len(blocks):
                blocks[idx], blocks[other] = blocks[other], blocks[idx]
    elif op.startswith("delete:"):
        bid = op.split(":", 1)[1]
        blocks = [b for b in blocks if b["id"] != bid]
    elif op.startswith("add:"):
        _, src_kind, filename = op.split(":", 2)
        available_names = {item["filename"] for item in collect_run_files(rundir)}
        if filename in available_names:
            blocks.append(new_block("artifact", source_file=filename, source_kind=src_kind, title=filename))
    elif op == "add-freeform":
        blocks.append(new_block("freeform", title="New section"))

    for b in blocks:
        allowed = allowed_depths(b)
        if b["depth"] not in allowed:
            b["depth"] = allowed[-1] if allowed else "none"

    save_curation(rundir, {"blocks": blocks})


def _studio_link_and_curated(rundir, base_url, suite, benchmark, run_id, raw_html):
    """Shared tail assembly for both report shapes: the curated block
    sequence (when curation.json has at least one included block) plus the
    studio link, with the raw artifact listing collapsed underneath via
    <details> once a curated view exists to lead with -- open by default
    when there's no curation yet, since the raw listing is then the only
    content this report has."""
    studio_url = f"/studio/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    curated_html = render_curated_section(rundir, base_url, suite, benchmark, run_id)
    if curated_html:
        return (f'<p><a href="{studio_url}">Edit curation</a></p>'
                f'{curated_html}'
                f'<details><summary>Raw artifacts</summary>{raw_html}</details>')
    return (f'<p><a href="{studio_url}">Curate this report</a> '
            f'<span class="muted">(select/reorder/annotate the artifacts below)</span></p>'
            f'{raw_html}')


def render_studio(rundir, suite, benchmark, run_id):
    curation = load_curation(rundir) or {"blocks": []}
    blocks = curation.get("blocks", [])
    available = collect_run_files(rundir)
    action = f"/studio/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"

    cards = []
    if not blocks:
        cards.append('<p class="muted">No blocks yet &mdash; add one of the artifacts below, '
                      'or a freeform section, to start curating.</p>')
    for i, b in enumerate(blocks):
        depths = allowed_depths(b)
        depth_opts = "".join(
            f'<option value="{d}"{" selected" if d == b.get("depth") else ""}>{d}</option>'
            for d in depths
        )
        source_note = (f'<span class="muted">source: {html.escape(b["source_file"])} '
                        f'({html.escape(b.get("source_kind") or "?")})</span>'
                        if b.get("kind") == "artifact" else '<span class="muted">freeform section</span>')
        show_excerpt = "excerpt" in depths
        cards.append(f"""
<div class="block-card">
  <input type="hidden" name="id" value="{html.escape(b['id'])}">
  <input type="hidden" name="kind" value="{html.escape(b.get('kind', 'artifact'))}">
  <input type="hidden" name="source_file" value="{html.escape(b.get('source_file') or '')}">
  <input type="hidden" name="source_kind" value="{html.escape(b.get('source_kind') or '')}">
  <div class="block-card-head">
    <label>Title
      <input type="text" name="title" value="{html.escape(b.get('title') or '')}">
    </label>
    {source_note}
    <div class="block-card-ops">
      <button type="submit" name="op" value="up:{html.escape(b['id'])}"{' disabled' if i == 0 else ''}>Move up</button>
      <button type="submit" name="op" value="down:{html.escape(b['id'])}"{' disabled' if i == len(blocks) - 1 else ''}>Move down</button>
      <button type="submit" name="op" value="delete:{html.escape(b['id'])}" class="danger">Remove</button>
    </div>
  </div>
  <div class="row">
    <label>Inclusion depth
      <select name="depth">{depth_opts}</select>
    </label>
    <label{' class="hidden-field"' if not show_excerpt else ''}>Excerpt lines
      <input type="text" name="excerpt_lines" value="{b.get('excerpt_lines', DEFAULT_EXCERPT_LINES)}">
    </label>
  </div>
  <label>Commentary <span class="muted">(what does this configuration tell us?)</span>
    <textarea name="commentary" rows="3">{html.escape(b.get('commentary') or '')}</textarea>
  </label>
</div>""")

    add_buttons = "".join(
        f'<button type="submit" name="op" value="add:{html.escape(item["kind"])}:{html.escape(item["filename"])}">'
        f'+ {html.escape(item["label"])}</button>'
        for item in available
    )

    body = f"""
<section class="panel">
  <h1>Curation studio: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>
  <p class="config-label">Select, reorder, and annotate this run's artifacts into a curated block
     sequence. Changes save when you click any button below (moving/removing/adding a block also
     saves any edits you've made to the others).</p>
  <p><a href="/report/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}">Back to report</a></p>
  <form method="post" action="{action}">
    <div class="block-list">{"".join(cards)}</div>
    <fieldset class="preview">
      <legend>Add a block</legend>
      <div class="add-buttons">{add_buttons or '<span class="muted">no artifacts found in this run directory</span>'}</div>
      <button type="submit" name="op" value="add-freeform">+ Freeform section (no artifact)</button>
    </fieldset>
    <button type="submit" name="op" value="save" class="primary">Save</button>
  </form>
</section>
"""
    return page(f"curation studio: {benchmark}/{run_id}", body)


def render_report(output_root, suite, benchmark, run_id):
    rundir = os.path.join(output_root, suite, benchmark, run_id)
    if not os.path.isdir(rundir):
        return None

    # A wspy-run-produced run directory (item 7) always carries wspy-run's
    # own manifest.json (generate_manifest(), unconditional for the unified
    # --suite/--benchmark layout); item 6's fixed-config path never writes a
    # bare "manifest.json" (its own is amdtopdown.manifest.json), so this is
    # an unambiguous discriminator between the two report shapes.
    run_manifest = read_run_manifest(os.path.join(rundir, RUN_MANIFEST_NAME))
    if run_manifest is not None:
        return render_wspy_run_report(rundir, suite, benchmark, run_id, run_manifest)
    return render_fixed_report(rundir, suite, benchmark, run_id)


def render_fixed_report(rundir, suite, benchmark, run_id):
    csv_path = os.path.join(rundir, CSV_NAME)
    manifest_path = os.path.join(rundir, MANIFEST_NAME)
    png_path = os.path.join(rundir, PNG_NAME)
    log_path = os.path.join(rundir, LOG_NAME)
    base = f"/files/{suite}/{benchmark}/{run_id}"

    workload = read_manifest_workload(manifest_path)
    workload_str = shlex.join(workload) if workload else None

    parts = [f"<h1>Report: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>"]

    if workload_str:
        parts.append(f"<p>Workload: <code>{html.escape(workload_str)}</code></p>")
        rerun_url = ("/?" +
                     "workload=" + _urlescape(workload_str) +
                     "&suite=" + _urlescape(suite) +
                     "&benchmark=" + _urlescape(benchmark))
        parts.append(f'<p><a href="{rerun_url}">Customize &amp; run again</a></p>')
    else:
        parts.append('<p class="muted">No manifest found; can\'t restore workload command.</p>')

    raw = ["<h2>Artifacts</h2><ul class=\"artifacts\">"]
    if os.path.exists(png_path):
        raw.append(f'<li>Topdown plot:<br><img class="plot" src="{base}/{PNG_NAME}" alt="amdtopdown plot"></li>')
    else:
        raw.append('<li class="muted">amdtopdown.png not generated</li>')
    if os.path.exists(csv_path):
        raw.append(f'<li><a href="{base}/{CSV_NAME}">{CSV_NAME}</a> (raw CSV)</li>')
    else:
        raw.append('<li class="muted">amdtopdown.csv missing</li>')
    if os.path.exists(manifest_path):
        raw.append(f'<li><a href="{base}/{MANIFEST_NAME}">{MANIFEST_NAME}</a> (raw manifest)</li>')
    else:
        raw.append('<li class="muted">amdtopdown.manifest.json missing</li>')
    if os.path.exists(log_path):
        raw.append(f'<li><a href="{base}/{LOG_NAME}">{LOG_NAME}</a> (launch log)</li>')
    raw.append("</ul>")

    parts.append(_studio_link_and_curated(rundir, base, suite, benchmark, run_id, "".join(raw)))

    body = "<section class=\"panel\">" + "".join(parts) + "</section>"
    return page(f"wspy report: {benchmark}/{run_id}", body)


def render_wspy_run_report(rundir, suite, benchmark, run_id, run_manifest):
    base = f"/files/{suite}/{benchmark}/{run_id}"
    workload = run_manifest.get("command") or None
    workload_str = shlex.join(workload) if workload else None

    parts = [f"<h1>Report: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>",
             '<p class="muted">Produced by the wspy-run profile launcher (item 7).</p>']

    if workload_str:
        parts.append(f"<p>Workload: <code>{html.escape(workload_str)}</code></p>")
        # No structured record of which profile(s) produced this run yet
        # (that's #16, structured configuration provenance) -- prefill the
        # profile launcher's workload/suite/benchmark and leave the profile
        # field for the user to re-pick.
        rerun_url = ("/?" +
                     "profile_workload=" + _urlescape(workload_str) +
                     "&profile_suite=" + _urlescape(suite) +
                     "&profile_benchmark=" + _urlescape(benchmark))
        parts.append(f'<p><a href="{rerun_url}">Customize &amp; run again</a> '
                      f'<span class="muted">(re-pick the profile; workload/suite/benchmark are prefilled)</span></p>')
    else:
        parts.append('<p class="muted">No workload command recorded in manifest.json.</p>')

    accounted_for = {RUN_MANIFEST_NAME}
    raw = []

    raw.append("<h2>Passes</h2><ul class=\"artifacts\">")
    for p in run_manifest.get("passes", []):
        name = p.get("name", "?")
        output = p.get("output")
        pass_manifest = p.get("manifest")
        status = p.get("status", "?")
        status_class = "" if status == "ok" else ' class="muted"'
        raw.append(f"<li><strong>{html.escape(name)}</strong> "
                   f"<span{status_class}>[{html.escape(status)}]</span><br>")
        if output:
            accounted_for.add(output)
            output_path = os.path.join(rundir, output)
            if output.endswith(".png") and os.path.isfile(output_path):
                raw.append(f'<img class="plot" src="{base}/{_urlescape(output)}" alt="{html.escape(name)}">')
            elif os.path.isfile(output_path):
                raw.append(f'<a href="{base}/{_urlescape(output)}">{html.escape(output)}</a>')
            else:
                raw.append(f'<span class="muted">{html.escape(output)} (missing)</span>')
        if pass_manifest:
            accounted_for.add(pass_manifest)
            if os.path.isfile(os.path.join(rundir, pass_manifest)):
                raw.append(f' &middot; <a href="{base}/{_urlescape(pass_manifest)}">manifest</a>')
        raw.append("</li>")
    raw.append("</ul>")

    raw.append("<h2>Run-level artifacts</h2><ul class=\"artifacts\">")
    if os.path.isfile(os.path.join(rundir, SUMMARY_NAME)):
        accounted_for.add(SUMMARY_NAME)
        raw.append(f'<li><a href="{base}/{SUMMARY_NAME}">{SUMMARY_NAME}</a> '
                   f'(concatenated non-CSV pass output)</li>')
    raw.append(f'<li><a href="{base}/{RUN_MANIFEST_NAME}">{RUN_MANIFEST_NAME}</a> (wspy-run run manifest)</li>')
    if os.path.isfile(os.path.join(rundir, LOG_NAME)):
        accounted_for.add(LOG_NAME)
        raw.append(f'<li><a href="{base}/{LOG_NAME}">{LOG_NAME}</a> (launch log)</li>')
    raw.append("</ul>")

    # Anything else regular file sitting in the run directory that no pass
    # claimed -- concretely, gnuplot.sh's own systemtime.png (produced
    # whenever systemtime.csv is present, alongside amdtopdown.png) plus our
    # own post-hoc amdtopdown.png/csv/manifest from item 6's plot step,
    # neither of which wspy-run's own passes[] list knows about. Scanned
    # rather than hardcoded so a different profile's gnuplot output, or a
    # future #12 plotting-template addition, shows up automatically.
    accounted_for.add(CURATION_NAME)
    try:
        extra = sorted(
            f for f in os.listdir(rundir)
            if f not in accounted_for and os.path.isfile(os.path.join(rundir, f))
        )
    except OSError:
        extra = []
    if extra:
        raw.append("<h2>Other artifacts</h2><ul class=\"artifacts\">")
        for f in extra:
            if f.endswith(".png"):
                raw.append(f'<li>{html.escape(f)}:<br>'
                          f'<img class="plot" src="{base}/{_urlescape(f)}" alt="{html.escape(f)}"></li>')
            else:
                raw.append(f'<li><a href="{base}/{_urlescape(f)}">{html.escape(f)}</a></li>')
        raw.append("</ul>")

    parts.append(_studio_link_and_curated(rundir, base, suite, benchmark, run_id, "".join(raw)))

    body = "<section class=\"panel\">" + "".join(parts) + "</section>"
    return page(f"wspy report: {benchmark}/{run_id}", body)


def _urlescape(s):
    from urllib.parse import quote
    return quote(s, safe="")


def render_compare(output_root, keys):
    """Item 8's compare view: sweep 2+ runs side by side. Deliberately raw
    (not curation-aware) and filename-aligned rather than block-aligned --
    curation is a per-run editorial layer (title/commentary/depth), while
    this view's job is spotting differences across runs' actual artifacts,
    which works whether or not either run has been curated yet."""
    runs = []
    seen = set()
    for key in keys:
        segs = key.split("/")
        if len(segs) != 3 or not all(valid_segment(s) for s in segs) or key in seen:
            continue
        seen.add(key)
        suite, benchmark, run_id = segs
        rundir = os.path.join(output_root, suite, benchmark, run_id)
        if not os.path.isdir(rundir):
            continue
        runs.append({"suite": suite, "benchmark": benchmark, "run_id": run_id, "rundir": rundir})

    if len(runs) < 2:
        body = ('<section class="panel"><h1>Compare runs</h1>'
                '<p class="muted">Select at least two runs from the homepage report list to '
                'compare them side by side.</p><p><a href="/">Back to launcher</a></p></section>')
        return page("wspy compare", body)

    for r in runs:
        r["base"] = f"/files/{r['suite']}/{r['benchmark']}/{r['run_id']}"
        r["files"] = {item["filename"]: item for item in collect_run_files(r["rundir"])}
        run_manifest = read_run_manifest(os.path.join(r["rundir"], RUN_MANIFEST_NAME))
        if run_manifest is not None:
            workload = run_manifest.get("command") or None
        else:
            workload = read_manifest_workload(os.path.join(r["rundir"], MANIFEST_NAME))
        r["workload_str"] = shlex.join(workload) if workload else None

    filenames = []
    filenames_seen = set()
    for r in runs:
        for item in collect_run_files(r["rundir"]):
            if item["filename"] not in filenames_seen:
                filenames_seen.add(item["filename"])
                filenames.append(item["filename"])

    header_cells = "".join(
        f'<th><a href="/report/{_urlescape(r["suite"])}/{_urlescape(r["benchmark"])}/{_urlescape(r["run_id"])}">'
        f'{html.escape(r["suite"])}/{html.escape(r["benchmark"])}<br>{html.escape(r["run_id"])}</a>'
        f'{"<br><code>" + html.escape(r["workload_str"]) + "</code>" if r["workload_str"] else ""}</th>'
        for r in runs
    )

    rows = []
    for filename in filenames:
        cells = []
        for r in runs:
            item = r["files"].get(filename)
            if item is None:
                cells.append('<td class="muted">&mdash;</td>')
                continue
            url = f'{r["base"]}/{_urlescape(filename)}'
            if item["kind"] == "image":
                cells.append(f'<td><img class="plot compare-plot" src="{url}" alt="{html.escape(filename)}"></td>')
            else:
                try:
                    size = os.path.getsize(os.path.join(r["rundir"], filename))
                except OSError:
                    size = None
                size_note = f' <span class="muted">({size} bytes)</span>' if size is not None else ""
                cells.append(f'<td><a href="{url}">{html.escape(filename)}</a>{size_note}</td>')
        rows.append(f'<tr><th class="row-label">{html.escape(filename)}</th>{"".join(cells)}</tr>')

    body = f"""
<section class="panel">
  <h1>Compare {len(runs)} runs</h1>
  <p><a href="/">Back to launcher</a></p>
  <div class="compare-scroll">
    <table class="compare">
      <thead><tr><th></th>{header_cells}</tr></thead>
      <tbody>{"".join(rows)}</tbody>
    </table>
  </div>
</section>
"""
    return page("wspy compare", body)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "wspy-web/0.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, body, content_type="text/html; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_json(self, code, obj):
        self._send(code, json.dumps(obj), content_type="application/json")

    def do_GET(self):
        parsed = urlsplit(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        cfg = self.server.wspy_cfg

        if path == "/":
            prefill_keys = ("workload", "suite", "benchmark",
                             "profile_workload", "profile_suite", "profile_benchmark")
            prefill = {k: v[0] for k, v in qs.items() if k in prefill_keys}
            self._send(200, render_index(cfg["output_root"], prefill,
                                          cfg["wspy_bin"], cfg["wspy_run_bin"],
                                          cfg["gnuplot_script"]))
            return

        if path.startswith("/static/"):
            self._serve_static(path[len("/static/"):])
            return

        m = re.match(r"^/report/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid report path")
                return
            out = render_report(cfg["output_root"], suite, benchmark, run_id)
            if out is None:
                self._send(404, "no such report")
            else:
                self._send(200, out)
            return

        m = re.match(r"^/studio/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isdir(rundir):
                self._send(404, "no such report")
                return
            self._send(200, render_studio(rundir, suite, benchmark, run_id))
            return

        if path == "/compare":
            keys = qs.get("r", [])
            self._send(200, render_compare(cfg["output_root"], keys))
            return

        m = re.match(r"^/files/([^/]+)/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id, filename = m.groups()
            self._serve_artifact(cfg["output_root"], suite, benchmark, run_id, filename)
            return

        m = re.match(r"^/api/run/([^/]+)/([^/]+)/([^/]+)/events$", path)
        if m:
            self._stream_events(*m.groups())
            return

        self._send(404, "not found")

    def do_POST(self):
        parsed = urlsplit(self.path)
        cfg = self.server.wspy_cfg

        m = re.match(r"^/studio/([^/]+)/([^/]+)/([^/]+)$", parsed.path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isdir(rundir):
                self._send(404, "no such report")
                return
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode("utf-8", errors="replace")
            form = parse_qs(raw, keep_blank_values=True)
            apply_studio_post(rundir, form)
            self.send_response(303)
            self.send_header("Location", f"/studio/{suite}/{benchmark}/{run_id}")
            self.end_headers()
            return

        if parsed.path not in ("/api/run", "/api/run-profile"):
            self._send(404, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._send_json(400, {"error": "invalid JSON body"})
            return
        if parsed.path == "/api/run":
            self._start_run(cfg, body)
        else:
            self._start_profile_run(cfg, body)

    def _serve_static(self, rel):
        if not valid_segment(rel):
            self._send(404, "not found")
            return
        path = os.path.join(STATIC_DIR, rel)
        if not os.path.isfile(path):
            self._send(404, "not found")
            return
        ctype = "text/css" if rel.endswith(".css") else \
                "application/javascript" if rel.endswith(".js") else \
                "application/octet-stream"
        with open(path, "rb") as f:
            self._send(200, f.read(), content_type=ctype)

    def _serve_artifact(self, output_root, suite, benchmark, run_id, filename):
        # No fixed filename whitelist: wspy-run's chosen profile determines
        # what lands in a run directory (systemtime.csv, process.tree.txt,
        # ibs.csv, ...), so any single path segment that both passes
        # valid_segment() (no "/", no "..") and actually exists inside this
        # specific, already-validated run directory is safe to serve.
        if not all(valid_segment(x) for x in (suite, benchmark, run_id, filename)):
            self._send(400, "invalid path")
            return
        path = os.path.join(output_root, suite, benchmark, run_id, filename)
        if not os.path.isfile(path):
            self._send(404, "not found")
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), content_type=guess_content_type(filename))

    def _start_run(self, cfg, body):
        workload_str = (body.get("workload") or "").strip()
        if not workload_str:
            self._send_json(400, {"error": "workload command is required"})
            return
        try:
            workload_argv = shlex.split(workload_str)
        except ValueError as e:
            self._send_json(400, {"error": f"could not parse workload command: {e}"})
            return
        if not workload_argv:
            self._send_json(400, {"error": "workload command is required"})
            return

        suite = (body.get("suite") or "manual").strip()
        benchmark = (body.get("benchmark") or "").strip() or default_benchmark_from_workload(workload_argv)
        run_id = (body.get("run_id") or "").strip() or make_run_id()

        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            self._send_json(400, {"error": "suite/benchmark/run_id must be non-empty and "
                                            "contain only letters, digits, '.', '_', '-'"})
            return

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return
        os.makedirs(rundir)

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        wspy_argv = build_wspy_argv(cfg["wspy_bin"], rundir, workload_argv)
        gnuplot_argv = build_gnuplot_argv(cfg["gnuplot_script"])

        t = threading.Thread(target=execute_run, args=(
            state, cfg["wspy_bin"], cfg["gnuplot_script"], rundir, workload_argv,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "wspy_command": shell_preview(wspy_argv),
            "gnuplot_command": shell_preview(gnuplot_argv, cwd=rundir),
        })

    def _start_profile_run(self, cfg, body):
        profile = (body.get("profile") or "").strip()
        if not profile or not valid_profile_spec(profile):
            self._send_json(400, {"error": "profile is required and must be a comma-separated list "
                                            "of letters/digits/'-'/'_' (e.g. deep-cpu or "
                                            "deep-cpu,tree-heavy)"})
            return

        workload_str = (body.get("workload") or "").strip()
        if not workload_str:
            self._send_json(400, {"error": "workload command is required"})
            return
        try:
            workload_argv = shlex.split(workload_str)
        except ValueError as e:
            self._send_json(400, {"error": f"could not parse workload command: {e}"})
            return
        if not workload_argv:
            self._send_json(400, {"error": "workload command is required"})
            return

        suite = (body.get("suite") or "manual").strip()
        benchmark = (body.get("benchmark") or "").strip() or default_benchmark_from_workload(workload_argv)
        run_id = (body.get("run_id") or "").strip() or make_run_id()

        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            self._send_json(400, {"error": "suite/benchmark/run_id must be non-empty and "
                                            "contain only letters, digits, '.', '_', '-'"})
            return

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return
        os.makedirs(rundir)

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        wspy_run_argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"],
                                             cfg["output_root"], suite, benchmark,
                                             run_id, profile, workload_argv)

        t = threading.Thread(target=execute_profile_run, args=(
            state, cfg, rundir, suite, benchmark, run_id, profile, workload_argv,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "wspy_run_command": shell_preview(wspy_run_argv),
        })

    def _stream_events(self, suite, benchmark, run_id):
        key = run_key(suite, benchmark, run_id)
        with RUNS_LOCK:
            state = RUNS.get(key)
        if state is None:
            self._send(404, "no such run")
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        sent = 0
        try:
            while True:
                with state.cond:
                    state.cond.wait_for(
                        lambda: len(state.lines) > sent or state.status != "running",
                        timeout=15,
                    )
                    pending = state.lines[sent:]
                    sent = len(state.lines)
                    status = state.status
                    report_url = state.report_url
                for line in pending:
                    self.wfile.write(f"event: log\ndata: {json.dumps(line)}\n\n".encode())
                if not pending:
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
                if status != "running" and sent >= len(state.lines):
                    report_url = f"/report/{suite}/{benchmark}/{run_id}"
                    self.wfile.write(
                        f"event: done\ndata: {json.dumps({'status': status, 'report_url': report_url})}\n\n".encode()
                    )
                    self.wfile.flush()
                    break
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--wspy", default=os.path.join(REPO_ROOT, "wspy"),
                     help="path to the wspy binary (default: repo root's ./wspy)")
    ap.add_argument("--wspy-run", default=os.path.join(REPO_ROOT, "wspy-run"),
                     help="path to the wspy-run script (default: repo root's ./wspy-run)")
    ap.add_argument("--gnuplot-script",
                     default=os.path.join(REPO_ROOT, "workload", "phoronix", "gnuplot.sh"),
                     help="path to the amdtopdown.csv -> amdtopdown.png plot script")
    ap.add_argument("--output-root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs"),
                     help="directory root for <suite>/<benchmark>/<run-id>/ run output "
                          "(default: web/runs)")
    args = ap.parse_args()

    output_root = os.path.abspath(args.output_root)
    os.makedirs(output_root, exist_ok=True)

    if not os.path.isfile(args.wspy):
        print(f"warning: wspy binary not found at {args.wspy} (build it with `make` first; "
              f"runs will fail until it exists)", file=sys.stderr)
    if not os.path.isfile(args.wspy_run):
        print(f"warning: wspy-run not found at {args.wspy_run}", file=sys.stderr)
    if not os.path.isfile(args.gnuplot_script):
        print(f"warning: gnuplot script not found at {args.gnuplot_script}", file=sys.stderr)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.wspy_cfg = {
        "wspy_bin": os.path.abspath(args.wspy),
        "wspy_run_bin": os.path.abspath(args.wspy_run),
        "gnuplot_script": os.path.abspath(args.gnuplot_script),
        "output_root": output_root,
    }
    print(f"wspy web launcher listening on http://{args.host}:{args.port}  "
          f"(output root: {output_root})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
