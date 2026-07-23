#!/usr/bin/env python3
"""
wspy web launcher + report browser (INVESTIGATION.md's "What shipped in 4.1").

Two launchers on one page:

- Item 6, the fixed-configuration slice: always runs the "amdtopdown" pass
  wspy-run's deep-cpu/deep-gpu profiles already use --

      wspy --csv --interval 1 --topdown --no-rusage --no-software --no-ipc

  -- followed by wspy-plot (item 12's shared plotting templates,
  workload/phoronix/gnuplot.sh's generalized replacement) rendering
  <rundir>/plots/*.png. No preset picker, no configuration/option checklist.
- Item 7, the wspy-run profile launcher: a thin form over wspy-run's own
  existing surface (builtin profile(s) + suite/benchmark/workload), so real
  varied runs exist to browse before #8's curation studio is built. Mirrors
  workload/phoronix/run_test.sh's own pattern -- call wspy-run, then
  best-effort run wspy-plot over the whole run directory, whatever CSVs the
  chosen profile produced.

Both are thin clients: every run launches exactly the command line(s) a user
could type by hand, shown before running them. No preset/configuration/option
checklist yet for either (that's #9), no wspy-validate/wspy-store/wspy-summary
coverage (also #9). The report browser keeps no state of its own -- it reads
whatever landed in a run directory straight off disk, dispatching on whether
wspy-run's own run-level manifest.json is present.

Usage:
    web/server.py [--host HOST] [--port PORT] [--wspy PATH] [--wspy-run PATH]
                   [--wspy-plot PATH] [--output-root DIR]

Stdlib only, by design (see CLAUDE.md's web/ entry for the reasoning).
"""
import argparse
import hashlib
import html
import json
import os
import queue
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joblib  # noqa: E402 -- see joblib.py's own docstring; shared with wspy-queue

REPO_ROOT = joblib.REPO_ROOT
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

# Re-exported from joblib.py (see its docstring) so the rest of this file --
# and any external code importing server.py's own names -- keeps working
# unchanged; joblib.py is the canonical definition, shared with wspy-queue.
from joblib import (  # noqa: E402,F401
    GROUP_NAMES, counter_group_flags, autofit_checklist_for_custom_plots,
    build_supplementary_plot_passes, parse_optional_int, build_configuration_passes,
    build_pass_argv, valid_segment, valid_relpath, make_run_id,
    default_benchmark_from_workload, valid_profile_spec, build_wspy_run_argv,
    build_plot_argv, shell_preview, RunState, run_store_ingest_besteffort,
    execute_profile_run, execute_custom_run, write_custom_run_manifest,
    write_custom_run_summary, LOG_NAME, PLOTS_DIR_NAME, RUN_MANIFEST_NAME, SUMMARY_NAME,
    NAME_RE, resolve_toggles, checklist_from_pass_provenance, valid_affinity_spec,
    parse_run_key, build_proctree_json_argv, build_proctree_diff_argv,
    run_sync, parse_phoronix_test_names, estimate_phoronix_workload_seconds,
    CSV_NAME, MANIFEST_NAME, PNG_NAME, CURATION_NAME, guess_kind, read_run_manifest,
    ai_artifact_label, list_plot_pngs, collect_run_files,
    build_reproducibility_bundle, BUNDLE_MANIFEST_NAME,
)

# The one fixed configuration item 6 knows about -- matches wspy-run's
# deep-cpu/deep-gpu "amdtopdown" pass exactly (wspy-run, load_builtin_profile()).
WSPY_FIXED_ARGS = ["--csv", "--interval", "1", "--counters=topdown",
                    "--no-rusage", "--no-software", "--no-ipc"]
# CSV_NAME/MANIFEST_NAME/PNG_NAME/LOG_NAME/RUN_MANIFEST_NAME/SUMMARY_NAME/
# CURATION_NAME/NAME_RE/PROFILE_TOKEN_RE come from joblib.py (import block above).
TREE_TXT_NAME = "process.tree.txt"  # fixed filename every --tree pass writes (joblib.py)
ARTIFACT_FILES = (CSV_NAME, MANIFEST_NAME, PNG_NAME, LOG_NAME)

# RUN_MANIFEST_NAME's presence in a run directory is what distinguishes an
# item-7 (wspy-run) report from an item-6 (fixed-config) one -- the two
# never collide since item 6 never writes a bare "manifest.json" (its own
# manifest is amdtopdown.manifest.json).
TOPLEVEL_MARKER_FILES = ARTIFACT_FILES + (RUN_MANIFEST_NAME, SUMMARY_NAME)

# wspy-run's own builtin profile catalog (wspy-run, BUILTIN_PROFILES) --
# offered as a datalist in the UI; wspy-run itself is still the source of
# truth and rejects anything else, so this list is a convenience, not a gate.
BUILTIN_PROFILES = ("quick", "deep-cpu", "deep-cpu-intel", "deep-gpu",
                     "tree-heavy", "ibs-basic", "ibs-memory-deep", "gpu-compute")

# ALL_GROUPS/counter_group_flags/COLUMN_TO_GROUP/resolve_column_group/
# autofit_checklist_for_custom_plots/PROFILE_PLOTTABLE_COLUMNS/
# build_supplementary_plot_passes/parse_optional_int/build_configuration_passes/
# build_pass_argv (item 9's checklist -> wspy-argv machinery, see joblib.py's
# own docstring for the full "Item 9" design rationale) now live in
# joblib.py, imported above -- shared with wspy-queue.
#
# ---------------------------------------------------------------------------
# Run registry: in-memory only, purely to relay a run's live log lines to an
# SSE stream while it's in flight. Nothing here is authoritative -- once a
# run finishes, the report page reads its directory off disk like any other,
# so a server restart mid-run loses only the live tail, not the artifacts.
# ---------------------------------------------------------------------------

# RunState (see joblib.py, imported above) is used identically here for the
# SSE relay -- the .lines/.cond machinery is only actually watched when a
# GET /api/run/.../events listener is attached.
RUNS = {}
RUNS_LOCK = threading.Lock()

# Separate registry for the report page's "AI narrative analysis" button
# (wspy-analyze, see execute_analyze() below): keyed the same way (suite,
# benchmark, run_id), but kept apart from RUNS so triggering an analysis
# against an already-finished run's report page never overwrites that run's
# own launch state (RUNS[key] may already hold a "done"/"error" RunState
# from the run that produced this report directory in the first place).
ANALYZE_RUNS = {}
ANALYZE_RUNS_LOCK = threading.Lock()


def run_key(suite, benchmark, run_id):
    return (suite, benchmark, run_id)


# valid_segment/valid_relpath/make_run_id/default_benchmark_from_workload
# come from joblib.py (import block above).

# ---------------------------------------------------------------------------
# Command construction -- the same strings are used to (a) show the user what
# will run, and (b) actually run it, so the preview is never a lie.
# ---------------------------------------------------------------------------

def build_wspy_argv(wspy_bin, rundir, workload_argv):
    csv_path = os.path.join(rundir, CSV_NAME)
    manifest_path = os.path.join(rundir, MANIFEST_NAME)
    return ([wspy_bin] + WSPY_FIXED_ARGS +
            ["-o", csv_path, "--manifest", manifest_path, "--"] + workload_argv)


# build_plot_argv/list_plot_pngs come from joblib.py (import block above).

# valid_profile_spec/build_wspy_run_argv/shell_preview come from joblib.py
# (import block above).

# ---------------------------------------------------------------------------
# Run execution
# ---------------------------------------------------------------------------

def execute_run(state, wspy_bin, wspy_plot_bin, rundir, workload_argv,
                 custom_plots=None, only_custom=False):
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
        emit("[skipping plot generation: no usable CSV output]")
        logf.close()
        state.finish("error", None)
        return

    plot_argv = build_plot_argv(wspy_plot_bin, rundir, custom_plots, only_custom)
    emit("$ " + shell_preview(plot_argv))
    try:
        proc = subprocess.Popen(plot_argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        for line in proc.stdout:
            emit(line.rstrip("\n"))
        plot_rc = proc.wait()
        emit(f"[wspy-plot exited {plot_rc}]")
    except OSError as e:
        emit(f"[error] failed to launch wspy-plot ({wspy_plot_bin}): {e}")
        plot_rc = 1

    logf.close()
    status = "done" if plot_rc == 0 else "error"
    state.finish(status, None)


def execute_analyze(state, argv, suite, benchmark, run_id):
    """Runs wspy-analyze (the report page's "AI narrative analysis" button)
    as a background subprocess, relaying its stderr progress lines through
    the same RunState/SSE machinery a launched workload uses (state lives in
    ANALYZE_RUNS, not RUNS -- see that registry's own comment above). Unlike
    execute_run(), there's no separate launch-log file to write: wspy-analyze
    already writes its own aiprompt.txt/aianalysis.<model>.txt artifacts
    straight into rundir, so this thread's only job is relaying live
    progress -- losing the tail on a server restart mid-analysis loses
    nothing not already recoverable by just re-running it, same as RUNS'
    own "nothing here is authoritative" note above. A multi-model (or
    --all-models) query can run for minutes against a real Ollama daemon, so
    this gets a live SSE tail rather than the Discovery tab's bounded
    run_sync()."""
    def emit(line):
        state.append(line)

    emit("$ " + shell_preview(argv))
    try:
        proc = subprocess.Popen(argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
    except OSError as e:
        emit(f"[error] failed to launch wspy-analyze ({argv[0]}): {e}")
        state.finish("error", None)
        return

    for line in proc.stdout:
        emit(line.rstrip("\n"))
    rc = proc.wait()
    emit(f"[wspy-analyze exited {rc}]")
    state.finish("done" if rc == 0 else "error", f"/report/{suite}/{benchmark}/{run_id}")


# run_store_ingest_besteffort/execute_profile_run/write_custom_run_manifest/
# write_custom_run_summary/execute_custom_run come from joblib.py (import
# block above) -- shared with wspy-queue's own job processing.
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


def discover_manifest_paths(output_root, limit=100):
    """Every *.manifest.json (per-pass) or bare manifest.json (wspy-run's own
    run-level index -- not a wspy-validate target itself, but harmless to
    list; validate.c just reports schema_version/passes-shaped files as a
    parse failure like any other malformed input) under output_root, newest
    first -- offered as "+ add" chips on the Validate tab so a manifest can
    be picked without typing its path by hand."""
    found = []
    if not os.path.isdir(output_root):
        return found
    for dirpath, _dirnames, filenames in os.walk(output_root):
        for f in filenames:
            if f.endswith(".manifest.json") or f == RUN_MANIFEST_NAME:
                path = os.path.join(dirpath, f)
                try:
                    mtime = os.path.getmtime(path)
                except OSError:
                    continue
                found.append({"path": path, "rel": os.path.relpath(path, output_root), "mtime": mtime})
    found.sort(key=lambda r: r["mtime"], reverse=True)
    return found[:limit]


def csv_has_core_column(path):
    """True if path's header row has a literal "core" column -- i.e. a
    --per-core wspy CSV, wspy-core-report's one required input. Matches
    core_report.c's own definition (read_percore_csv() requires exactly
    this column), so callers never offer/link a CSV that would just fail
    with "no core column" if actually run through the tool."""
    try:
        with open(path, "r", newline="") as fh:
            header = fh.readline()
    except OSError:
        return False
    cols = [c.strip() for c in header.strip().split(",")]
    return "core" in cols


def discover_percore_csv_paths(output_root, limit=100):
    """Every *.csv under output_root with a "core" header column, newest
    first, offered as "+ add" chips on the Validate tab's per-core class
    comparison section so a path never has to be typed by hand."""
    found = []
    if not os.path.isdir(output_root):
        return found
    for dirpath, _dirnames, filenames in os.walk(output_root):
        for f in filenames:
            if not f.endswith(".csv"):
                continue
            path = os.path.join(dirpath, f)
            if not csv_has_core_column(path):
                continue
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            found.append({"path": path, "rel": os.path.relpath(path, output_root), "mtime": mtime})
    found.sort(key=lambda r: r["mtime"], reverse=True)
    return found[:limit]


def core_report_link(csv_path):
    """A 'Compare cores' link for a --per-core CSV artifact, landing on the
    Validate tab's per-core class comparison section with this path
    prefilled (do_GET()'s core_report_csv query param, render_index()'s
    active_tab). Empty string if the file isn't actually a --per-core CSV,
    same degrade-quietly idiom as every other artifact-presence check in
    the report renderers -- a plain CSV/systemtime.csv artifact just won't
    get the link."""
    if not csv_has_core_column(csv_path):
        return ""
    url = "/?core_report_csv=" + _urlescape(csv_path)
    return f' &middot; <a href="{html.escape(url)}">Compare cores</a>'


# ---------------------------------------------------------------------------
# Historical run index browser/search (INVESTIGATION.md's "What shipped in
# 4.1") -- discover_reports() above is the homepage's cheap, mtime-only recent
# list; this is the fuller searchable index over every run directory. Reads
# straight off disk (directory scan + each run's own manifest(s)), not
# wspy-store's normalized `runs` table, since store ingestion is an opt-in
# per-run toggle and this browser should cover every run whether or not that
# toggle was on for it -- matching this tier's "no server-owned state that
# isn't also derivable from files already being produced" design principle.
# ---------------------------------------------------------------------------

HISTORY_PAGE_SIZE = 25


def read_json_file(path):
    """Best-effort JSON load -- None on any I/O/parse failure rather than
    raising, so one unreadable/malformed manifest degrades that one run's
    metadata instead of breaking the whole history scan."""
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return None


def run_status_from_exit_status(exit_status):
    """Mirrors wspy-validate's own "clean exit" definition (validate.c):
    known+exited+exit_code==0 is ok, known+(signaled or nonzero) is failed,
    not known at all is unknown -- never fatal to the browser either way."""
    if not exit_status or not exit_status.get("known"):
        return "unknown"
    if exit_status.get("signaled"):
        return "failed"
    if exit_status.get("exited") and exit_status.get("exit_code") == 0:
        return "ok"
    return "failed"


def run_status_from_passes(passes):
    if not passes:
        return "unknown"
    return "ok" if all(p.get("status") == "ok" for p in passes) else "failed"


def load_run_history_entry(output_root, suite, benchmark, run_id, mtime):
    """Best-effort metadata for one run directory: workload command, overall
    status, and (from a representative per-process manifest -- wspy-run's own
    run-level manifest.json carries none of this) hostname/cpu_vendor/
    start_time/elapsed_seconds. Every field degrades to None/"unknown"
    independently rather than failing the whole entry, the same "measured vs
    unavailable" idiom coverage.c/provenance.c use for a single run's own
    fields, applied here across a run's identifying metadata instead."""
    rundir = os.path.join(output_root, suite, benchmark, run_id)
    run_manifest = read_run_manifest(os.path.join(rundir, RUN_MANIFEST_NAME))
    host_manifest = None
    if run_manifest is not None:
        workload = run_manifest.get("command") or None
        status = run_status_from_passes(run_manifest.get("passes"))
        for p in run_manifest.get("passes", []):
            pass_manifest = p.get("manifest")
            if pass_manifest:
                host_manifest = read_json_file(os.path.join(rundir, pass_manifest))
                if host_manifest:
                    break
    else:
        host_manifest = read_json_file(os.path.join(rundir, MANIFEST_NAME))
        workload = (host_manifest.get("command", {}).get("argv") if host_manifest else None) or None
        status = run_status_from_exit_status(host_manifest.get("exit_status") if host_manifest else None)

    host = (host_manifest or {}).get("host") or {}
    timing = (host_manifest or {}).get("timing") or {}

    return {
        "suite": suite, "benchmark": benchmark, "run_id": run_id, "mtime": mtime,
        "workload_str": shlex.join(workload) if workload else None,
        "status": status,
        "hostname": host.get("hostname"),
        "cpu_vendor": host.get("cpu_vendor"),
        "start_time": timing.get("start_time"),
        "elapsed_seconds": timing.get("elapsed_seconds"),
    }


def discover_run_history(output_root, filters, page=1, page_size=HISTORY_PAGE_SIZE):
    """Every run directory under output_root (no 50-cap like
    discover_reports() -- this view's whole point is searching further back
    than the homepage's recent list), filtered and paginated server-side.
    Returns (page_of_entries, total_matching)."""
    entries = []
    if os.path.isdir(output_root):
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
                    entries.append(load_run_history_entry(output_root, suite, benchmark, run_id, mtime))

    def matches(e):
        if filters.get("q") and filters["q"] not in (e["workload_str"] or "").lower():
            return False
        if filters.get("suite") and filters["suite"] not in e["suite"].lower():
            return False
        if filters.get("benchmark") and filters["benchmark"] not in e["benchmark"].lower():
            return False
        if filters.get("hostname") and filters["hostname"] not in (e["hostname"] or "").lower():
            return False
        if filters.get("cpu_vendor") and filters["cpu_vendor"].lower() != (e["cpu_vendor"] or "").lower():
            return False
        if filters.get("status") and filters["status"] != e["status"]:
            return False
        date_key = (e["start_time"] or "")[:10]
        if filters.get("date_from") and (not date_key or date_key < filters["date_from"]):
            return False
        if filters.get("date_to") and (not date_key or date_key > filters["date_to"]):
            return False
        return True

    filtered = [e for e in entries if matches(e)]

    def sort_key(e):
        if e["start_time"]:
            return e["start_time"]
        return datetime.fromtimestamp(e["mtime"], tz=timezone.utc).isoformat()

    filtered.sort(key=sort_key, reverse=True)

    total = len(filtered)
    page = max(1, page)
    start = (page - 1) * page_size
    return filtered[start:start + page_size], total


def _rundir_triple_for_path(output_root, path):
    """If path falls inside output_root at the <suite>/<benchmark>/<run_id>/...
    depth the unified output layout uses, returns (suite, benchmark, run_id,
    relpath-within-rundir); otherwise None. Used by _resolve_trace_links()
    (item 14) to turn a store-recorded absolute path back into a /report or
    /files URL -- only possible for runs this server's own --output-root
    produced (or was pointed at), not an absolute path ingested from a
    different host's run-index (doc/ARTIFACT_CONTRACT.md's "Normalized
    store" section notes that's the common cross-host case, and it's
    expected to just not resolve here)."""
    if not path:
        return None
    try:
        rel = os.path.relpath(os.path.abspath(path), os.path.abspath(output_root))
    except ValueError:
        return None  # e.g. different drive on Windows; never true on this project's Linux target
    if rel == os.pardir or rel.startswith(os.pardir + os.sep):
        return None
    parts = rel.split(os.sep)
    if len(parts) < 4:
        return None
    suite, benchmark, run_id = parts[0], parts[1], parts[2]
    if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
        return None
    filename = "/".join(parts[3:])
    if not valid_relpath(filename):
        return None
    return suite, benchmark, run_id, filename


def _resolve_trace_links(output_root, fields):
    """Best-effort companion to _discovery_trace(): turns wspy-summary
    --trace's raw {manifest,output,tree_output}_path fields into clickable
    /report + /files links wherever they resolve under this server's own
    output_root, degrading to "no link" (the caller still has the raw path
    text from `fields`) rather than failing the whole lookup when they
    don't -- same idiom as everywhere else in this tier."""
    links = {}
    for path_field, kind in (("manifest_path", "manifest"), ("output_path", "output"),
                              ("tree_output_path", "tree")):
        triple = _rundir_triple_for_path(output_root, fields.get(path_field))
        if not triple:
            continue
        suite, benchmark, run_id, filename = triple
        links.setdefault("report_url", f"/report/{suite}/{benchmark}/{run_id}")
        links[f"{kind}_url"] = f"/files/{suite}/{benchmark}/{run_id}/{filename}"
    return links




# Core/thread affinity control's discovery counterpart (INVESTIGATION.md's
# "Core/thread affinity control" item): parses `wspy --list-affinity`'s
# output (affinity.c's affinity_print_report() -- a small CSV-shaped per-cpu
# table plus L3-domain and core-type summaries, see CLAUDE.md's affinity.c
# entry) into structured JSON the Run tab's affinity card can render
# checkboxes/domain/coretype buttons from, without needing a JSON library on
# the C side for a report this simple.
_AFFINITY_CPU_ROW_RE = re.compile(r"^(\d+),(-?\d+),(-?\d+),([01]),(-?\d+),(-?\d+)$")
_AFFINITY_DOMAIN_RE = re.compile(r"^l3domain (\d+): cpus (\S+) \(([\d.]+) MiB\)$")
_AFFINITY_CORETYPE_RE = re.compile(r"^coretype (\d+): implementer=(0x[0-9a-f]+) part=(0x[0-9a-f]+) cpus (\S+)$")
_AFFINITY_CORETYPE_VENDOR_RE = re.compile(r"^coretype (\d+): vendor=(\S+) cpus (\S+)$")


def parse_affinity_topology_output(output):
    """Returns {"cpus": [{"id","core_id","package_id","primary_thread",
    "l3_domain","core_type"}...], "domains": [{"id","cpus","size_mib"}...],
    "core_types": [{"id","cpus","implementer","part"}...] (ARM MIDR-derived)
    or [{"id","cpus","vendor"}...] (x86 hybrid fallback, e.g. Intel P-core/
    E-core or AMD Zen5/Zen5c) -- entries are one shape or the other, never
    mixed, see affinity.c's affinity_print_report()}, or None if output
    doesn't look like a --list-affinity report at all (e.g. wspy itself
    failed to launch). core_types is [] on a genuinely homogeneous host --
    affinity.c never populates it there, not a parsing gap."""
    cpus = []
    domains = []
    core_types = []
    for line in output.splitlines():
        line = line.strip()
        m = _AFFINITY_CPU_ROW_RE.match(line)
        if m:
            cpus.append({
                "id": int(m.group(1)), "core_id": int(m.group(2)),
                "package_id": int(m.group(3)), "primary_thread": m.group(4) == "1",
                "l3_domain": int(m.group(5)), "core_type": int(m.group(6)),
            })
            continue
        m = _AFFINITY_DOMAIN_RE.match(line)
        if m:
            domains.append({"id": int(m.group(1)), "cpus": m.group(2), "size_mib": float(m.group(3))})
            continue
        m = _AFFINITY_CORETYPE_RE.match(line)
        if m:
            core_types.append({"id": int(m.group(1)), "implementer": m.group(2),
                                "part": m.group(3), "cpus": m.group(4)})
            continue
        m = _AFFINITY_CORETYPE_VENDOR_RE.match(line)
        if m:
            core_types.append({"id": int(m.group(1)), "vendor": m.group(2), "cpus": m.group(3)})
    if not cpus:
        return None
    return {"cpus": cpus, "domains": domains, "core_types": core_types}


# ---------------------------------------------------------------------------
# Item 18: Run tab "Check" button -- perf-counter-access sysctls plus, for
# phoronix-test-suite workloads specifically, an estimated/actual runtime
# looked up via `phoronix-test-suite info <test>`. Deliberately optional and
# separate from the run itself (matching the design note's "quickly check
# ... before launching" framing) -- nothing here blocks or alters a run,
# it's read-only discovery like the Discovery tab's capabilities/preflight
# checks, just surfaced next to the Run button since that's the moment this
# information is actually useful. No estimator exists yet for cpu2017/
# pbbsbench/arbitrary commands (INVESTIGATION.md's "What shipped in 4.1",
# estimated runtime display's own scoping note), so those degrade to "no
# estimate available" rather than guessing.
# ---------------------------------------------------------------------------

PERF_PARANOID_PATH = "/proc/sys/kernel/perf_event_paranoid"
NMI_WATCHDOG_PATH = "/proc/sys/kernel/nmi_watchdog"
PERF_PARANOID_MAX_OK = 1   # CLAUDE.md/scripts/setup_perf.sh's documented minimum
NMI_WATCHDOG_DESIRED = 0


def _read_sysctl_int(path):
    """Best-effort single-integer /proc/sys read -- returns (value_or_None,
    error_or_None), never raises. A missing file (non-Linux, or a sysctl
    this kernel doesn't expose) and a permission error both degrade to
    "unknown" rather than failing the whole check, the same "measured vs
    unavailable" idiom coverage.c/provenance.c use for hardware-side
    unavailability."""
    try:
        with open(path) as f:
            return int(f.read().strip()), None
    except FileNotFoundError:
        return None, "not present on this system"
    except (OSError, ValueError) as e:
        return None, str(e)


def check_perf_access():
    """Reads perf_event_paranoid/nmi_watchdog directly -- the same two
    sysctls scripts/setup_perf.sh already checks/adjusts -- and reports each
    as ok/warn/unknown against wspy's documented minimum requirement, no
    wspy invocation needed since these are plain /proc/sys reads."""
    results = {}
    paranoid, perr = _read_sysctl_int(PERF_PARANOID_PATH)
    if paranoid is None:
        results["perf_event_paranoid"] = {"value": None, "status": "unknown", "detail": perr}
    else:
        ok = paranoid <= PERF_PARANOID_MAX_OK
        results["perf_event_paranoid"] = {
            "value": paranoid, "status": "ok" if ok else "warn",
            "detail": f"wspy needs <= {PERF_PARANOID_MAX_OK} for unprivileged counter access "
                      f"(root, or CAP_SYS_PTRACE, also works regardless of this value)",
        }
    nmi, nerr = _read_sysctl_int(NMI_WATCHDOG_PATH)
    if nmi is None:
        results["nmi_watchdog"] = {"value": None, "status": "unknown", "detail": nerr}
    else:
        ok = nmi == NMI_WATCHDOG_DESIRED
        results["nmi_watchdog"] = {
            "value": nmi, "status": "ok" if ok else "warn",
            "detail": "an active watchdog reserves one hardware counter system-wide "
                      "(wspy still runs, just with one fewer slot)",
        }
    return results


def check_tooling():
    """Checks for external binaries wspy's own tools shell out to at runtime
    (not build time, so nothing catches a missing one until the shellout
    itself fails) -- currently just gnuplot, which wspy-plot always
    popen()s literally as "gnuplot" (plot.c) with no configurable path.
    execute_profile_run()/execute_custom_run() run wspy-plot best-effort
    after every launch, so a missing gnuplot doesn't fail the run -- it
    just silently produces no plots, discoverable today only by noticing
    an empty plots/ directory after the fact. A real incident: a run
    finished, was published, and the missing plots weren't noticed until
    someone went looking for them."""
    gnuplot = shutil.which("gnuplot")
    return {
        "gnuplot": {
            "status": "ok" if gnuplot else "warn",
            "path": gnuplot,
            "detail": ("found on PATH" if gnuplot else
                       "not found on PATH -- the plots step (wspy-plot, run best-effort "
                       "after every launch) will fail silently and produce no PNGs"),
        },
    }


# IBS's PMUs can be present in sysfs (so `wspy --capabilities`/`ibs_probe()`
# report them as supported) yet still have perf_event_open() reject every
# counter with -EINVAL at runtime -- a MaxCnt/sample_period mismatch has
# caused exactly this before, and produced a report full of zeros with no
# warning until you went digging in its manifest. Only an actual open
# attempt catches that class of failure, so when this run would use IBS,
# the Check button also launches a real (trivial, ~2s)
# `wspy --ibs-basic|--ibs-memory-deep -- true` probe rather
# than only re-deriving sysfs presence. topdown.c:setup_counters()'s error()
# text is identical regardless of which counter group failed to open, so
# this same regex also parses probe_power()'s failures below.
COUNTER_UNAVAILABLE_RE = re.compile(
    r"unable to create \S+ performance counter, name=(\S+), errno=(\d+) - (.+)")
IBS_UNSUPPORTED_TEXT = "AMD IBS not supported on this host/kernel"


def ibs_probes_for_request(cfg, preset, checklist):
    """Returns [(label, flags)] for every IBS profile this request would
    actually invoke -- a composite preset's ibs-basic/ibs-memory-deep
    entries, or the Run tab's IBS checklist row in custom mode -- so the
    probe below tests exactly what would run, not a guess. [] when IBS
    isn't in play for this request at all."""
    preset = (preset or "").strip()
    probes = []
    if preset:
        for name in [p.strip() for p in preset.split(",")]:
            if name == "ibs-basic":
                probes.append(("ibs-basic", ["--ibs-basic", "--no-ipc", "--csv"]))
            elif name == "ibs-memory-deep":
                probes.append(("ibs-memory-deep", ["--ibs-memory-deep", "--no-ipc", "--csv"]))
        return probes

    ibs = (checklist or {}).get("ibs") or {}
    if not ibs.get("enabled"):
        return []
    label = "ibs-memory-deep" if ibs.get("profile") == "memory-deep" else "ibs-basic"
    # Placeholder rundir: build_configuration_passes() only ever uses it to
    # spell a --tree pass's output path into that pass's flags text -- never
    # touches the filesystem -- so this is safe purely to get the ibs pass's
    # real flags (profile + --ibs-maxcnt/ldlat/fetchlat overrides), matching
    # exactly what a real custom run would invoke.
    placeholder_rundir = os.path.join(cfg["output_root"], "<check>", "<check>", "<check>")
    for p in build_configuration_passes(placeholder_rundir, checklist):
        if p["category"] == "ibs":
            probes.append((label, p["flags"]))
    return probes


def probe_ibs(wspy_bin, label, flags):
    """Actually runs `wspy <flags> -- true` (a trivial, always-successful
    workload) and reports whether every requested IBS counter opened. Never
    raises -- a probe failure degrades to status "unknown", the same
    measured-vs-unavailable idiom used throughout the C side."""
    argv = [wspy_bin] + list(flags) + ["--", "true"]
    entry = {"profile": label, "command": shell_preview(argv)}
    rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=15)
    if timed_out:
        entry.update(status="unknown", detail="probe timed out after 15s")
        return entry
    if rc is None:
        entry.update(status="unknown", detail=f"failed to launch {wspy_bin}")
        return entry
    if IBS_UNSUPPORTED_TEXT in output:
        entry.update(status="warn",
                      detail=f"{IBS_UNSUPPORTED_TEXT} (ibs_fetch/ibs_op PMUs not present)")
        return entry

    measured = requested = None
    lines = output.splitlines()
    for i, line in enumerate(lines):
        if "counters_measured" in line and "counters_requested" in line and i + 1 < len(lines):
            header = [c.strip() for c in line.split(",")]
            data = [c.strip() for c in lines[i + 1].split(",")]
            try:
                measured = int(data[header.index("counters_measured")])
                requested = int(data[header.index("counters_requested")])
            except (ValueError, IndexError):
                measured = requested = None
            break

    failures = COUNTER_UNAVAILABLE_RE.findall(output)
    if requested is None:
        entry.update(status="unknown", detail="could not parse counter coverage from probe output")
    elif requested == 0:
        entry.update(status="unknown", detail="probe requested 0 IBS counters unexpectedly")
    elif measured == requested and not failures:
        entry.update(status="ok", detail=f"{measured}/{requested} IBS counter(s) opened successfully")
    else:
        detail = "; ".join(f"{name}: errno={errnum} ({reason})" for name, errnum, reason in failures) \
            or f"only {measured}/{requested} IBS counter(s) opened"
        entry.update(status="warn", detail=detail)
    entry["measured"] = measured
    entry["requested"] = requested
    return entry


# power/energy-pkg's own permission story is stricter than the general
# perf_event_paranoid gate check_perf_access() already covers -- confirmed
# live: at perf_event_paranoid=1 (wspy's own documented minimum,
# scripts/setup_perf.sh's default target), --ibs-basic opens its counters
# fine but --power gets EACCES, because RAPL/power-PMU access needs
# CAP_PERFMON or root specifically (the Platypus-CVE-era kernel hardening),
# not just a permissive-enough paranoid value. --capabilities' sysfs-only
# discovery can't see this either (it happily reports the PMU as
# "supported" from sysfs alone) -- only an actual open attempt catches it,
# same reasoning as the IBS probe above.
POWER_UNSUPPORTED_TEXT = "CPU power/energy (power/energy-pkg) not supported on this host/kernel"
# EACCES specifically -- the exact errno RAPL's CAP_PERFMON/root requirement
# produces, distinct from e.g. EINVAL on a malformed config -- gets an
# actionable remediation hint appended, not just "insufficient permission".
POWER_EACCES_ERRNO = "13"
POWER_EACCES_HINT = (
    "power/energy-pkg needs root or the CAP_PERFMON capability, stricter than "
    "perf_event_paranoid alone covers (confirmed: --ibs-basic opens fine at the same "
    "paranoid level that denies this) -- either run wspy under sudo, or run "
    "`scripts/setup_perf.sh` (it now checks/grants CAP_PERFMON on the wspy binary "
    "alongside its existing sysctl checks -- note the grant is tied to that exact binary "
    "file and needs re-running after every rebuild)"
)


# Preset names whose own wspy-run passes open --power (hand-derived from
# load_builtin_profile() in wspy-run, same "not otherwise discoverable from
# Python without parsing wspy-run's bash" reasoning as PROFILE_PLOTTABLE_COLUMNS
# above). deep-cpu's systemtime pass carries --power (see wspy-run's own
# comment there: "--power rides along on systemtime... since systemtime
# already opens zero hardware counters, --power is a genuinely free
# addition"), and gpu-compute's single pass does too. deep-gpu's systemtime
# pass now carries it as well (INVESTIGATION.md's "What shipped in 4.2" --
# fixed a pre-existing asymmetry between deep-cpu and deep-gpu, not a
# deliberate difference). This table used to be empty (the docstring below
# claimed "no preset uses --power"), which silently skipped the power probe
# for every deep-cpu run despite deep-cpu having used --power all along.
POWER_PRESET_NAMES = {"deep-cpu", "deep-gpu", "gpu-compute"}


def power_probes_for_request(cfg, preset, checklist):
    """Returns [(label, flags)] -- at most one entry, since --power has no
    profile variants the way IBS does -- for whether this request would
    actually invoke --power: a preset in POWER_PRESET_NAMES (or a composite
    list containing one), or the Run tab's "package power" checkbox
    (checked under either "Performance counters" or "System metrics" --
    it has no card of its own, see build_configuration_passes()'s own
    comment) in custom mode. [] when power isn't in play for this request
    at all."""
    preset = (preset or "").strip()
    if preset:
        names = {p.strip() for p in preset.split(",")}
        if names & POWER_PRESET_NAMES:
            return [("power", ["--power", "--no-ipc", "--csv"])]
        return []

    counters = (checklist or {}).get("counters") or {}
    system = (checklist or {}).get("system") or {}
    wants_power = (counters.get("enabled") and counters.get("power")) or \
        (system.get("enabled") and system.get("power"))
    if not wants_power:
        return []
    # Placeholder rundir: build_configuration_passes() only ever uses it to
    # spell a --tree pass's output path into that pass's flags text -- never
    # touches the filesystem -- so this is safe purely to get the real pass
    # flags, matching exactly what a real custom run would invoke (whichever
    # of "counters"/"system" power actually got folded into).
    placeholder_rundir = os.path.join(cfg["output_root"], "<check>", "<check>", "<check>")
    for p in build_configuration_passes(placeholder_rundir, checklist):
        if "--power" in p["flags"]:
            return [("power", p["flags"])]
    return []


def probe_power(wspy_bin, label, flags):
    """Actually runs `wspy <flags> -- true` (a trivial, always-successful
    workload) and reports whether the pkg_joules counter opened -- catches
    the RAPL-specific CAP_PERFMON/root requirement described above, which
    sysfs-presence-only discovery can't. Never raises -- a probe failure
    degrades to status "unknown", same idiom as probe_ibs()."""
    argv = [wspy_bin] + list(flags) + ["--", "true"]
    entry = {"profile": label, "command": shell_preview(argv)}
    rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=15)
    if timed_out:
        entry.update(status="unknown", detail="probe timed out after 15s")
        return entry
    if rc is None:
        entry.update(status="unknown", detail=f"failed to launch {wspy_bin}")
        return entry
    if POWER_UNSUPPORTED_TEXT in output:
        entry.update(status="warn",
                      detail=f"{POWER_UNSUPPORTED_TEXT} (power/energy-pkg PMU not present in sysfs)")
        return entry

    measured = requested = None
    lines = output.splitlines()
    for i, line in enumerate(lines):
        if "counters_measured" in line and "counters_requested" in line and i + 1 < len(lines):
            header = [c.strip() for c in line.split(",")]
            data = [c.strip() for c in lines[i + 1].split(",")]
            try:
                measured = int(data[header.index("counters_measured")])
                requested = int(data[header.index("counters_requested")])
            except (ValueError, IndexError):
                measured = requested = None
            break

    failures = COUNTER_UNAVAILABLE_RE.findall(output)
    if requested is None:
        entry.update(status="unknown", detail="could not parse counter coverage from probe output")
    elif requested == 0:
        entry.update(status="unknown", detail="probe requested 0 power counter(s) unexpectedly")
    elif measured == requested and not failures:
        entry.update(status="ok", detail=f"{measured}/{requested} power counter(s) opened successfully")
    else:
        detail = "; ".join(f"{name}: errno={errnum} ({reason})" for name, errnum, reason in failures) \
            or f"only {measured}/{requested} power counter(s) opened"
        if any(errnum == POWER_EACCES_ERRNO for _, errnum, _ in failures):
            detail += " -- " + POWER_EACCES_HINT
        entry.update(status="warn", detail=detail)
    entry["measured"] = measured
    entry["requested"] = requested
    return entry


# GPU backends are a build-time link (AMDGPU=1 for amd_smi/amd_sysfs,
# NVIDIA=1 for NVML), unlike IBS/power which are always compiled in and
# gated only by runtime perf access. A GPU flag on a build lacking its
# backend doesn't fail or warn loudly -- topdown.c/wspy.c print one
# "GPU support not built (rebuild with AMDGPU=1/NVIDIA=1): --gpu-foo ignored"
# line and continue, so the run "succeeds" with every GPU column silently
# absent or zero, discoverable today only by noticing empty GPU data in the
# report afterward (confirmed: this is exactly how a real yquake2 run's
# gpu.csv ended up all-zero, unnoticed until looked at directly). The Check
# button catches this up front the same way probe_ibs()/probe_power() catch
# their own runtime-only failure modes, just with a build check instead of
# an open attempt, since a not-built backend needs a rebuild, not root/setcap.
GPU_BUILD_UNSUPPORTED_RE = re.compile(
    r"GPU support not built \(rebuild with (AMDGPU=1|NVIDIA=1)\): (\S+) ignored")

# Preset name -> the --gpu-* flags its own wspy-run passes use (hand-derived
# from load_builtin_profile(), same reasoning as POWER_PRESET_NAMES/
# PROFILE_PLOTTABLE_COLUMNS above -- a preset's own flags aren't otherwise
# discoverable from Python without parsing wspy-run's bash). deep-gpu's
# gpu_busy/gpu_metrics passes use the AMD sysfs backend only; gpu-compute's
# single pass uses AMD sysfs + NVML both.
PRESET_GPU_FLAGS = {
    "deep-gpu": ["--gpu-busy", "--gpu-metrics"],
    "gpu-compute": ["--gpu-busy", "--gpu-metrics", "--gpu-nvidia"],
}


def gpu_flags_for_request(preset, checklist):
    """Returns the --gpu-* flags this request would actually invoke -- a
    composite preset's own known GPU flags (PRESET_GPU_FLAGS), or the Run
    tab's GPU checklist row in custom mode. [] when no GPU flag is in play."""
    preset = (preset or "").strip()
    if preset:
        flags = []
        for name in [p.strip() for p in preset.split(",")]:
            for flag in PRESET_GPU_FLAGS.get(name, []):
                if flag not in flags:
                    flags.append(flag)
        return flags

    gpu = (checklist or {}).get("gpu") or {}
    if not gpu.get("enabled"):
        return []
    flags = []
    if gpu.get("busy"):
        flags.append("--gpu-busy")
    if gpu.get("metrics"):
        flags.append("--gpu-metrics")
    if gpu.get("smi"):
        flags.append("--gpu-smi")
    if gpu.get("nvidia"):
        flags.append("--gpu-nvidia")
    return flags


def check_gpu_build(wspy_bin, flags):
    """Runs `wspy <flags> -- true` once (a trivial, always-successful
    workload -- no hardware GPU needed, this only checks whether the binary
    itself was linked with AMDGPU=1/NVIDIA=1) and reports, per requested
    flag, ok/warn/unknown. [] when no GPU flag is in play for this request."""
    if not flags:
        return []
    argv = [wspy_bin] + list(flags) + ["--", "true"]
    entry_base = {"command": shell_preview(argv)}
    rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=15)
    results = []
    if timed_out or rc is None:
        detail = "probe timed out after 15s" if timed_out else f"failed to launch {wspy_bin}"
        for flag in flags:
            results.append({**entry_base, "flag": flag, "status": "unknown", "detail": detail})
        return results
    unsupported = {flag: macro for macro, flag in GPU_BUILD_UNSUPPORTED_RE.findall(output or "")}
    for flag in flags:
        if flag in unsupported:
            results.append({**entry_base, "flag": flag, "status": "warn",
                             "detail": f"GPU support not built -- rebuild wspy with {unsupported[flag]}"})
        else:
            results.append({**entry_base, "flag": flag, "status": "ok",
                             "detail": "built with GPU support (hardware/driver availability not checked here)"})
    return results


# `phoronix-test-suite batch-run` doesn't take flags for any of this -- it
# reads user-config.xml's <BatchMode> block, written by the interactive
# `phoronix-test-suite batch-setup` wizard, and falls back to prompting on
# stdin for anything that block leaves unanswered. A real incident:
# RunAllTestCombinations left at its FALSE default (the wizard's own
# suggested default, not something wspy-run itself ever sets) made a
# background wspy-run invocation pause indefinitely asking which of a
# test's option combinations to run, with nowhere for that prompt to go --
# not discovered until much later, since wspy itself doesn't time out.
# PTS_USER_PATH is the same env var phoronix-test-suite itself honors to
# relocate this file.
PHORONIX_BATCH_MODE_REQUIRED = {
    "Configured": "TRUE",
    "RunAllTestCombinations": "TRUE",
    "PromptForTestIdentifier": "FALSE",
    "PromptForTestDescription": "FALSE",
    "PromptSaveName": "FALSE",
}
PHORONIX_BATCH_MODE_HINTS = {
    "Configured": "the batch-setup wizard has never been run/completed",
    "RunAllTestCombinations": "prompts to pick one test option combination instead of "
                              "running all of them",
    "PromptForTestIdentifier": "prompts for a test identifier before each run",
    "PromptForTestDescription": "prompts for a test description before each run",
    "PromptSaveName": "prompts for a save name before each run",
}


def phoronix_user_config_path():
    base = os.environ.get("PTS_USER_PATH") or os.path.join(os.path.expanduser("~"),
                                                             ".phoronix-test-suite")
    return os.path.join(base, "user-config.xml")


def check_phoronix_batch_config():
    """Reads user-config.xml's <BatchMode> block and warns about any setting
    that makes `phoronix-test-suite batch-run` pause waiting on stdin for
    input a background/scripted invocation has no way to supply. Best-effort
    text/XML read against phoronix-test-suite's own config file format, not
    validated against its source -- an unreadable or unexpected file
    degrades to "unknown" rather than failing the whole check, same
    measured-vs-unavailable idiom used throughout this codebase."""
    path = phoronix_user_config_path()
    if not os.path.isfile(path):
        return {"status": "warn", "path": path, "settings": None,
                "detail": "no user-config.xml found -- phoronix-test-suite will run its "
                          "interactive batch-setup wizard on first batch-run and pause "
                          "waiting for input; run `phoronix-test-suite batch-setup` once first"}
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        return {"status": "unknown", "path": path, "settings": None,
                "detail": f"could not parse {path}: {e}"}
    # Real-world files nest this under <Options> (<PhoronixTestSuite><Options>
    # <BatchMode>...), confirmed against a live phoronix-test-suite v10.8.6
    # install -- search anywhere in the tree rather than assuming a fixed
    # depth, since that nesting isn't documented anywhere and could shift
    # across versions.
    batch_mode = root.find(".//BatchMode")
    if batch_mode is None:
        return {"status": "warn", "path": path, "settings": None,
                "detail": "no <BatchMode> section -- run `phoronix-test-suite batch-setup` "
                          "once to configure unattended batch-run behavior"}
    settings = {}
    problems = []
    for key, wanted in PHORONIX_BATCH_MODE_REQUIRED.items():
        el = batch_mode.find(key)
        value = el.text.strip() if el is not None and el.text else None
        settings[key] = value
        if (value or "").upper() != wanted:
            problems.append(f"{key}={value!r} (needs {wanted}) -- {PHORONIX_BATCH_MODE_HINTS[key]}")
    if problems:
        return {"status": "warn", "path": path, "settings": settings,
                "detail": "batch-run will pause waiting for input: " + "; ".join(problems)}
    return {"status": "ok", "path": path, "settings": settings,
            "detail": "configured for unattended batch-run"}


# The phoronix-test-suite launcher script itself hardcodes this as PTS_DIR
# on every non-macOS install (it's not exported into our environment since
# we invoke it as a subprocess, not source the script) -- see that script's
# own "Full path to root directory of the actual Phoronix Test Suite code"
# comment. --phoronix-pts-dir overrides this for a non-standard install.
PHORONIX_PTS_DEFAULT_DIR = "/usr/share/phoronix-test-suite"


def phoronix_result_notifier_settings_path():
    """Directory name must be the underscored "result_notifier" -- PTS's own
    module lookup (pts_module::read_option()) resolves it from the module's
    literal PHP class name, not a hyphenated form. An earlier version of
    scripts/setup_phoronix_hooks.sh got this wrong (see
    doc/phoronix_hook_investigation.md's "Real-Host Findings" section) --
    checking only this, correct path is deliberate: a stale hyphenated
    directory from that bug is never read by PTS either, so it's not a
    "hooks are registered" signal worth checking."""
    base = os.environ.get("PTS_USER_PATH") or os.path.join(os.path.expanduser("~"),
                                                             ".phoronix-test-suite")
    return os.path.join(base, "modules-data", "result_notifier", "module-settings.ini")


def phoronix_result_notifier_hooks_registered():
    """True if pre_test_run_process or post_test_run_process is set to a
    non-empty value in result_notifier's own module-settings.ini -- the
    precondition for phoronix-test-suite's own result_notifier.php bug (see
    check_phoronix_result_notifier_bug() below) to actually matter. Without
    a real hook script configured, the buggy code path is never reached at
    all, so surfacing the bug check unconditionally on every Phoronix run
    would be alarming noise for the common case of not using this feature."""
    try:
        with open(phoronix_result_notifier_settings_path()) as f:
            text = f.read()
    except OSError:
        return False
    for key in ("pre_test_run_process", "post_test_run_process"):
        m = re.search(rf"^{key}\s*=\s*(.+)$", text, re.MULTILINE)
        if m and m.group(1).strip():
            return True
    return False


# Two independent bugs found live in phoronix-test-suite's own bundled
# result_notifier.php (doc/phoronix_hook_investigation.md's "Real-Host
# Findings" section; reported/fixed upstream at
# phoronix-test-suite/phoronix-test-suite#924 and #925): it unconditionally
# dereferences a test_result_buffer that's null both at pre-run time (no
# trial has happened yet) and, for a simple single-way test, even at
# post-run time, and it calls a get_result() method that doesn't exist on
# pts_test_result at all (the real accessor is ->active->get_result()).
# Either one is an uncaught PHP fatal error the instant a real hook script
# is configured -- not a wspy bug, but wspy's own scripts/pts_hooks/*.sh are
# exactly what would configure one on this host, so catching it here (before
# a real benchmark run crashes with zero results) is the point of this
# check. Detected by simple text search rather than executing PHP -- this
# server has no PHP dependency and the exact buggy/fixed lines are known
# verbatim from reproducing and patching the bug directly.
PHORONIX_RESULT_NOTIFIER_BUGGY_MARKER = "test_result_buffer->get_count() + 1;"
PHORONIX_RESULT_NOTIFIER_FIXED_MARKER = "has_result_buffer"


def check_phoronix_result_notifier_bug(pts_dir):
    """Best-effort text search of PTS's own bundled result_notifier.php for
    the known-buggy vs. known-fixed pattern. Degrades to "unknown" (not
    "warn") when the file can't be found/read, or contains neither marker
    (e.g. a future upstream rewrite this project hasn't seen) -- absence of
    proof isn't proof of a crash, same idiom as every other probe here."""
    path = os.path.join(pts_dir, "pts-core", "modules", "result_notifier.php")
    try:
        with open(path) as f:
            text = f.read()
    except OSError:
        return {"status": "unknown", "path": path,
                "detail": f"could not read {path} -- is phoronix-test-suite installed under "
                          f"{pts_dir}? (override with --phoronix-pts-dir)"}
    if PHORONIX_RESULT_NOTIFIER_FIXED_MARKER in text:
        return {"status": "ok", "path": path,
                "detail": "patched -- matches phoronix-test-suite/phoronix-test-suite#924's fix"}
    if PHORONIX_RESULT_NOTIFIER_BUGGY_MARKER in text:
        return {"status": "warn", "path": path,
                "detail": "unpatched -- this install will crash phoronix-test-suite with a fatal "
                          "PHP error (\"Call to a member function get_count() on null\" or "
                          "\"Call to undefined method pts_test_result::get_result()\") as soon as "
                          "the configured hook script fires, producing zero results instead of a "
                          "real run. See phoronix-test-suite/phoronix-test-suite#924/#925 for the "
                          "fix, or apply it locally to this file until it ships in a release."}
    return {"status": "unknown", "path": path,
            "detail": "neither the known-buggy nor known-fixed pattern was found -- this may be a "
                      "version this check hasn't seen; can't confirm whether hooks will crash"}


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


def read_manifest_config_provenance(manifest_path):
    """Best-effort: pull a manifest.json's structured configuration
    provenance (INVESTIGATION.md's "What shipped in 4.1", manifest.c's
    write_config_provenance()) back out -- {"preset": str|None,
    "configuration": str|None, "options": [(name,value), ...]} -- or None if
    the manifest is missing/unreadable, or the run was never launched with
    --preset-name/--config-name (a plain direct wspy invocation, or an item-6
    fixed-config run -- see execute_run(), which never sets either flag).
    This is item 17's read side of item 16: relating a report's artifacts
    back to the preset/configuration/option choices that produced them."""
    try:
        with open(manifest_path) as f:
            data = json.load(f)
        cp = data.get("configuration_provenance")
        if not isinstance(cp, dict):
            return None
        preset = cp.get("preset")
        configuration = cp.get("configuration")
        if preset is None and configuration is None:
            return None
        options = [(o.get("name"), o.get("value")) for o in cp.get("options", [])
                   if isinstance(o, dict)]
        return {"preset": preset, "configuration": configuration, "options": options}
    except (OSError, json.JSONDecodeError, ValueError, AttributeError):
        return None


def format_config_provenance(cp):
    """Human-readable one-liner for a single pass's configuration_provenance
    (item 17: showing a report's artifacts alongside the preset/
    configuration/option choices that produced them), e.g.
    'preset=deep-cpu; config=amdtopdown' or 'config=performance-counters;
    options=groups=topdown,cache2, interval_secs=1, csv=true'. None in, None
    out (nothing to show)."""
    if not cp:
        return None
    bits = []
    if cp.get("preset"):
        bits.append(f"preset={cp['preset']}")
    if cp.get("configuration"):
        bits.append(f"config={cp['configuration']}")
    opts = cp.get("options") or []
    if opts:
        bits.append("options=" + ", ".join(f"{n}={v}" for n, v in opts if n))
    return "; ".join(bits) if bits else None


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
# Curation studio (INVESTIGATION.md's "What shipped in 4.1"): review every artifact a run
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
#
# Item 15 (report commentary/annotation) adds one more field alongside
# "blocks": a single "overview_note" for the report as a whole, distinct
# from each block's own per-configuration commentary ("what does *this*
# configuration tell us" vs. "what does the report as a whole tell us") --
# the doc is explicit that this needs to be both, not one collapsed into
# the other. It lives in the same curation.json file rather than the
# normalized store, matching this tier's "no server-owned state that isn't
# reconstructible from the run directory" principle that #8 already
# established for per-block commentary.
# ---------------------------------------------------------------------------

# CURATION_NAME comes from joblib.py (import block above).
CURATION_SCHEMA_VERSION = "1.1"
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


def allowed_depths(block):
    if block.get("kind") == "freeform":
        return DEPTH_OPTIONS_BY_KIND["freeform"]
    return DEPTH_OPTIONS_BY_KIND.get(block.get("source_kind"), ("none", "full"))


def new_block(kind, source_file=None, source_kind=None, title=None, ai_generated=False):
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
        # wspy-analyze's own output (aianalysis.<model>.txt / aiprompt.critique.<model>.txt,
        # see collect_run_files() below) is model-written prose, not human commentary --
        # this flag rides along through studio edits/saves/export so it stays labeled
        # AI-generated even after its text is copied into this block's own commentary
        # field (INVESTIGATION.md's Ollama deep-dive, design decision #7: "never
        # silently substituted as the human's own curated commentary in an export").
        "ai_generated": ai_generated,
    }


# AIANALYSIS_RE/AIPROMPT_CRITIQUE_RE/ai_artifact_label/collect_run_files come
# from joblib.py (import block above) -- shared with build_reproducibility_bundle()
# there, which archives the identical file list this function's callers browse.


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


# ---------------------------------------------------------------------------
# Compare-view curation (INVESTIGATION.md's "Give the report compare view
# its own curation/annotation layer" item). Unlike curation.json above,
# this state describes a *set* of runs, not one run directory -- there's no
# existing precedent for that in this file (run_index.jsonl/store.db are
# the closest, and those are flat per-run logs, not relationships between
# specific runs), so it lives in its own directory at the output-root level,
# the same level run_index.jsonl/store.db already occupy for the same
# "spans more than one run" reason.
#
# Phase 1 scope only (a fuller "manually align two differently-named files
# as the same measurement" mode was considered and deferred -- see this
# item's own INVESTIGATION.md entry): an overview note for the comparison
# as a whole, plus one commentary note per filename row, using exactly
# today's filename-based row identity (render_compare()'s own union-of-
# filenames list) rather than inventing a new alignment concept.
# ---------------------------------------------------------------------------

COMPARE_CURATION_DIR = "compares"
COMPARE_SCHEMA_VERSION = "1.0"


def compare_id_for_keys(run_keys):
    """Deterministic id for a comparison, from the *sorted* set of
    "<suite>/<benchmark>/<run_id>" keys -- order-independent (so
    ?r=A&r=B and ?r=B&r=A resolve to the same curation) and exact-match
    (a different run set, even one run added/removed, gets a different id
    and starts uncurated -- same "don't guess at approximate equivalence"
    idiom summary.c's mixed-pmu check already uses elsewhere in this
    project, rather than fuzzy-matching a "close enough" prior comparison)."""
    joined = "\n".join(sorted(set(run_keys)))
    return hashlib.sha256(joined.encode("utf-8")).hexdigest()[:16]


def load_compare_curation(output_root, compare_id):
    try:
        with open(os.path.join(output_root, COMPARE_CURATION_DIR, f"{compare_id}.json")) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return None
    if not isinstance(data.get("row_notes"), dict):
        return None
    return data


def save_compare_curation(output_root, compare_id, data):
    data["schema_version"] = COMPARE_SCHEMA_VERSION
    data["updated"] = datetime.now(timezone.utc).isoformat()
    data.setdefault("created", data["updated"])
    compare_dir = os.path.join(output_root, COMPARE_CURATION_DIR)
    os.makedirs(compare_dir, exist_ok=True)
    path = os.path.join(compare_dir, f"{compare_id}.json")
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
    overview_note = curation.get("overview_note", "")
    if not included and not overview_note:
        return ""
    parts = ["<h2>Curated view</h2>"]
    if overview_note:
        parts.append(f'<p class="overview-note">{html.escape(overview_note)}</p>')
    for b in included:
        parts.append('<div class="block">')
        title_html = html.escape(b.get('title') or '(untitled)')
        if b.get("ai_generated"):
            title_html += ' <span class="badge ai-badge">AI-generated</span>'
        parts.append(f"<h3>{title_html}</h3>")
        if b.get("commentary"):
            parts.append(f'<p class="commentary">{html.escape(b["commentary"])}</p>')
        parts.append(render_block_content(rundir, base_url, b))
        parts.append("</div>")
    return "".join(parts)


# ---------------------------------------------------------------------------
# Publish-ready export (INVESTIGATION.md's "What shipped in 4.1"): renders
# the curation studio's curated block sequence into a format ready to paste elsewhere, rather than a bulk
# data dump. Three targets, in the doc's own recommended-default order:
#
#   - WordPress block markup (Gutenberg comment format) -- the default, since
#     pasting it into the block editor produces separately-editable native
#     blocks (heading/image/preformatted/paragraph) instead of one opaque
#     blob.
#   - Self-contained inline-styled HTML -- for a "Custom HTML" block or any
#     CMS that just wants raw markup; easiest to paste, hardest to edit again.
#   - Markdown -- portable, for anywhere that takes it directly or as a
#     conversion source.
#
# All three are thin wrappers over export_block_content(), which mirrors
# render_block_content()'s depth handling (summary/excerpt/full) but returns
# structured data (image URL, or preformatted text, or nothing, plus an
# optional plain-text note) instead of an HTML string, since each format
# spells out "image"/"preformatted text" differently. A curated image block
# is exported as a URL pointing back at this server's /files/... endpoint --
# real image *hosting* for WordPress/HTML is a documented gap (the doc's own
# words: "a real gap between mockup and implementation, not a detail to gloss
# over"), so the export page surfaces that explicitly rather than silently
# producing a link that will 404 once this server stops running.
# ---------------------------------------------------------------------------

EXPORT_FORMATS = ("wordpress", "html", "markdown")
EXPORT_FORMAT_LABELS = {
    "wordpress": "WordPress blocks (Gutenberg)",
    "html": "Self-contained HTML",
    "markdown": "Markdown",
}
EXPORT_FORMAT_EXTENSIONS = {"wordpress": "html", "html": "html", "markdown": "md"}
EXPORT_FORMAT_CONTENT_TYPES = {
    "wordpress": "text/html; charset=utf-8",
    "html": "text/html; charset=utf-8",
    "markdown": "text/markdown; charset=utf-8",
}


def export_block_content(rundir, base_url, block):
    """Structured form of one curated block's artifact content, shared by
    every export renderer. Returns (content_kind, payload, note):

      content_kind: "none" | "image" | "pre"
      payload: absolute image URL (content_kind == "image"), or preformatted
        text (content_kind == "pre"), else None
      note: an optional plain-text annotation (line/byte counts, "too large
        to embed", ...) for the renderer to show alongside the content --
        always plain text, never HTML, so each format escapes it itself.
    """
    depth = block.get("depth", "none")
    if depth == "none" or block.get("kind") == "freeform":
        return "none", None, None

    filename = block.get("source_file")
    if not filename:
        return "none", None, "(missing source)"
    path = os.path.join(rundir, filename)
    if not os.path.isfile(path):
        return "none", None, f"{filename} not found"
    url = f"{base_url}/{_urlescape(filename)}"
    kind = block.get("source_kind")

    if kind == "image":
        return ("image", url, None) if depth == "full" else ("none", None, None)

    text, size = read_text_safely(path)
    if text is None:
        return "none", None, f"{filename} ({size} bytes, binary -- not embedded, full file: {url})"

    lines = text.splitlines()
    n = len(lines)

    if depth == "summary":
        peek = "\n".join(lines[:3])
        extra = f", {len(lines[0].split(','))} columns" if kind == "csv" and n else ""
        return "pre", peek, f"{n} lines, {size} bytes{extra} -- full file: {url}"

    if depth == "excerpt":
        excerpt_n = block.get("excerpt_lines") or DEFAULT_EXCERPT_LINES
        shown = "\n".join(lines[:excerpt_n])
        note = (f"showing first {min(excerpt_n, n)} of {n} lines -- full file: {url}"
                if n > excerpt_n else None)
        return "pre", shown, note

    # depth == "full"
    if size > MAX_INLINE_BYTES:
        return "none", None, f"{size} bytes, too large to embed -- full file: {url}"
    return "pre", text, None


def _export_data(rundir):
    """(overview_note, included_blocks) for the export renderers -- one
    read of curation.json shared by every format and by the download
    endpoint, so the report-level note and the block sequence never fall
    out of sync with each other."""
    curation = load_curation(rundir)
    if not curation:
        return "", []
    blocks = [b for b in curation.get("blocks", []) if b.get("depth", "none") != "none"]
    return curation.get("overview_note", ""), blocks


def render_export_markdown(rundir, base_url, title, overview_note, blocks):
    parts = [f"# {title}\n"]
    if overview_note:
        parts.append(f"{overview_note}\n")
    for b in blocks:
        heading = b.get('title') or '(untitled)'
        if b.get("ai_generated"):
            heading += " _(AI-generated)_"
        parts.append(f"## {heading}\n")
        if b.get("commentary"):
            parts.append(f"{b['commentary']}\n")
        content_kind, payload, note = export_block_content(rundir, base_url, b)
        if content_kind == "image":
            parts.append(f"![{b.get('title') or ''}]({payload})\n")
        elif content_kind == "pre":
            fence = "````" if "```" in payload else "```"
            parts.append(f"{fence}\n{payload}\n{fence}\n")
        if note:
            parts.append(f"*{note}*\n")
    return "\n".join(parts) + "\n"


def render_export_html(rundir, base_url, title, overview_note, blocks):
    body_parts = [f'<h1 style="font-family:sans-serif;">{html.escape(title)}</h1>']
    if overview_note:
        body_parts.append(f'<p style="font-family:sans-serif;">{html.escape(overview_note)}</p>')
    for b in blocks:
        heading_html = html.escape(b.get("title") or "(untitled)")
        if b.get("ai_generated"):
            heading_html += ' <span style="font-size:0.6em;color:#666;">(AI-generated)</span>'
        body_parts.append(f'<h2 style="font-family:sans-serif;margin-top:2em;">{heading_html}</h2>')
        if b.get("commentary"):
            body_parts.append(
                f'<p style="font-family:sans-serif;">{html.escape(b["commentary"])}</p>')
        content_kind, payload, note = export_block_content(rundir, base_url, b)
        if content_kind == "image":
            body_parts.append(f'<img src="{payload}" alt="{html.escape(b.get("title") or "")}" '
                               f'style="max-width:100%;height:auto;">')
        elif content_kind == "pre":
            body_parts.append(
                f'<pre style="background:#f5f5f5;padding:12px;overflow-x:auto;'
                f'font-family:monospace;font-size:0.9em;">{html.escape(payload)}</pre>')
        if note:
            body_parts.append(
                f'<p style="color:#666;font-family:sans-serif;font-size:0.85em;">{html.escape(note)}</p>')
    return ("<!doctype html><html><head><meta charset=\"utf-8\">"
            f"<title>{html.escape(title)}</title></head>"
            f'<body style="max-width:800px;margin:2em auto;">{"".join(body_parts)}</body></html>')


def _wp_block(name, inner_html, attrs=None):
    attrs_json = f" {json.dumps(attrs)}" if attrs else ""
    return f"<!-- wp:{name}{attrs_json} -->\n{inner_html}\n<!-- /wp:{name} -->"


def render_export_wordpress(rundir, base_url, title, overview_note, blocks):
    parts = [_wp_block("heading", f"<h1>{html.escape(title)}</h1>", {"level": 1})]
    if overview_note:
        parts.append(_wp_block("paragraph", f'<p>{html.escape(overview_note)}</p>'))
    for b in blocks:
        heading_html = html.escape(b.get("title") or "(untitled)")
        if b.get("ai_generated"):
            heading_html += ' <em>(AI-generated)</em>'
        parts.append(_wp_block("heading", f'<h2>{heading_html}</h2>', {"level": 2}))
        if b.get("commentary"):
            parts.append(_wp_block("paragraph", f'<p>{html.escape(b["commentary"])}</p>'))
        content_kind, payload, note = export_block_content(rundir, base_url, b)
        if content_kind == "image":
            parts.append(_wp_block(
                "image", f'<figure class="wp-block-image"><img src="{payload}" '
                         f'alt="{html.escape(b.get("title") or "")}"/></figure>'))
        elif content_kind == "pre":
            parts.append(_wp_block(
                "preformatted", f'<pre class="wp-block-preformatted">{html.escape(payload)}</pre>'))
        if note:
            parts.append(_wp_block("paragraph", f'<p><em>{html.escape(note)}</em></p>'))
    return "\n\n".join(parts) + "\n"


def render_export(rundir, base_url, title, fmt, overview_note, blocks):
    if fmt == "markdown":
        return render_export_markdown(rundir, base_url, title, overview_note, blocks)
    if fmt == "html":
        return render_export_html(rundir, base_url, title, overview_note, blocks)
    return render_export_wordpress(rundir, base_url, title, overview_note, blocks)


def render_export_page(rundir, base_url, suite, benchmark, run_id, fmt):
    studio_url = f"/studio/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    title = f"{suite} / {benchmark} / {run_id}"
    overview_note, blocks = _export_data(rundir)

    if not blocks and not overview_note:
        body = (f'<section class="panel"><h1>Export: {html.escape(benchmark)}/{html.escape(run_id)}</h1>'
                f'<p class="muted">No curated blocks yet &mdash; '
                f'<a href="{studio_url}">curate this report</a> first, then come back here.</p></section>')
        return page(f"export: {benchmark}/{run_id}", body)

    rendered = render_export(rundir, base_url, title, fmt, overview_note, blocks)
    has_image = any(b.get("source_kind") == "image" and b.get("depth") == "full" for b in blocks)

    nav = "".join(
        f'<a class="tab-btn{" active" if f == fmt else ""}" href="?format={f}">{EXPORT_FORMAT_LABELS[f]}</a>'
        for f in EXPORT_FORMATS
    )
    download_url = (f"/export/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}/download"
                    f"?format={fmt}")
    image_note = (
        '<p class="muted">This export references an image by its URL on this server '
        f'(<code>{base_url}</code>), which only resolves while this server keeps running at this '
        'address. For WordPress or another CMS, re-upload the image to that platform\'s media '
        'library first and swap in the resulting URL before publishing (see '
        '<code>INVESTIGATION.md</code>\'s "What shipped in 4.1").</p>'
    ) if has_image else ""

    body = f"""
<section class="panel">
  <h1>Export: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>
  <p class="config-label">Renders this report's curated block sequence
     (<a href="{studio_url}">edit curation</a>) into a format ready to paste elsewhere.</p>
  <nav class="tabs">{nav}</nav>
  {image_note}
  <textarea readonly rows="28" style="width:100%;font-family:monospace;font-size:0.85em;"
    >{html.escape(rendered)}</textarea>
  <p><a href="{download_url}">Download as .{EXPORT_FORMAT_EXTENSIONS[fmt]}</a></p>
</section>
"""
    return page(f"export: {benchmark}/{run_id}", body)


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


def render_group_checkboxes(id_prefix, checked_by_default=()):
    return "".join(
        f'<label class="group-check"><input type="checkbox" id="{id_prefix}_{name}" value="{name}"'
        f'{" checked" if name in checked_by_default else ""}> <code>{name}</code></label>'
        for name in GROUP_NAMES
    )


def render_run_tab(prefill, cfg):
    w_workload = html.escape(prefill.get("workload", ""))
    w_suite = html.escape(prefill.get("suite", "manual"))
    w_benchmark = html.escape(prefill.get("benchmark", ""))

    # Item 17: "Customize & run again" restores not just workload/suite/
    # benchmark but, when the report's configuration_provenance resolved to
    # one (checklist_from_pass_provenance(), called from build_rerun_url()'s
    # callers), the exact preset selection or checklist state that produced
    # it -- prefill["preset"]/prefill["checklist"], parsed and validated in
    # do_GET("/"). Absent (a fresh visit to "/", or a report with no
    # restorable provenance), prefill_checklist is {} and every field below
    # falls back to its pre-item-17 hardcoded default.
    prefill_preset = prefill.get("preset") or ""
    prefill_checklist = prefill.get("checklist") or {}

    def sec(key):
        section = prefill_checklist.get(key)
        return section if isinstance(section, dict) else {}

    def chk(cond):
        return " checked" if cond else ""

    def chk_default(cat, key, default_when_absent):
        """A checkbox's checked state: the prefilled value when this
        category is part of a restored checklist (explicit, even if False --
        see checklist_section_from_options()'s comment on why every boolean
        option is always recorded), else the pre-item-17 hardcoded default
        for a fresh form."""
        if cat in prefill_checklist:
            return bool(sec(cat).get(key, False))
        return default_when_absent

    def val(cat, key):
        v = sec(cat).get(key)
        return html.escape(str(v)) if v not in (None, "") else ""

    preset_options = "".join(
        f'<option value="{html.escape(p)}"{" selected" if p == prefill_preset else ""}>'
        f'{html.escape(p)}</option>'
        for p in BUILTIN_PROFILES
    )
    default_groups = set(sec("counters").get("groups") or []) if "counters" in prefill_checklist \
        else {"topdown"}
    counters_groups_html = render_group_checkboxes("counters", checked_by_default=default_groups)

    return f"""
<section class="panel">
  <h1>Run</h1>
  <p class="config-label">Pick a named <code>wspy-run</code> preset, or build a custom run from the
     configuration checklist below. Per the preset/configuration/option hierarchy
     (<code>INVESTIGATION.md</code>): a preset is atomic -- picking one runs it exactly as
     <code>wspy-run</code> defines it and the checklist below is ignored; set preset back to
     "(custom)" to compose configurations directly instead. Each enabled configuration below becomes
     its own separate <code>wspy</code> invocation into the same run directory.</p>
  <form id="run-form">
    <input type="hidden" id="phoronix_test_point" name="phoronix_test_point"
           value="{html.escape(prefill.get('phoronix_test_point', ''))}">
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

    <label>Preset
      <select id="preset">
        <option value="">(custom &mdash; use the checklist below)</option>
        {preset_options}
      </select>
    </label>
    <p id="mode-indicator" class="mode-indicator"></p>

    <div id="checklist">
      <div class="config-card" data-config="tree">
        <label class="config-toggle"><input type="checkbox" id="tree_enabled"{chk(sec('tree').get('enabled'))}> <strong>Process tree</strong></label>
        <div class="config-options">
          <div class="option-group-label">General</div>
          <div class="option-grid">
            <label class="group-check"><input type="checkbox" id="tree_cmdline"{chk(chk_default('tree', 'cmdline', False))}> full command lines <code>--tree-cmdline</code></label>
            <label class="group-check"><input type="checkbox" id="tree_software"{chk(chk_default('tree', 'software', True))}> software counters too (on by default; unchecking adds <code>--no-software</code>)</label>
          </div>
          <div class="option-group-label">Latency (blocking-time measurements)</div>
          <div class="option-grid">
            <label class="group-check"><input type="checkbox" id="tree_futex"{chk(chk_default('tree', 'futex', False))}> record blocking futex waits <code>--tree-futex</code></label>
            <label class="group-check"><input type="checkbox" id="tree_io_wait"{chk(chk_default('tree', 'io_wait', False))}> record blocking I/O wait time <code>--tree-io-wait</code></label>
            <label class="group-check"><input type="checkbox" id="tree_connect"{chk(chk_default('tree', 'connect', False))}> record connect() latency <code>--tree-connect</code></label>
            <label class="group-check"><input type="checkbox" id="tree_wait"{chk(chk_default('tree', 'wait', False))}> record wait4/waitid blocking time <code>--tree-wait</code></label>
            <label class="group-check"><input type="checkbox" id="tree_poll"{chk(chk_default('tree', 'poll', False))}> record poll/select/epoll_wait blocking time <code>--tree-poll</code></label>
            <label class="group-check"><input type="checkbox" id="tree_nanosleep"{chk(chk_default('tree', 'nanosleep', False))}> record nanosleep time <code>--tree-nanosleep</code></label>
          </div>
          <div class="option-group-label">Information (facts, no latency)</div>
          <div class="option-grid">
            <label class="group-check"><input type="checkbox" id="tree_open"{chk(chk_default('tree', 'open', False))}> record <code>open()</code> calls <code>--tree-open</code></label>
            <label class="group-check"><input type="checkbox" id="tree_io"{chk(chk_default('tree', 'io', False))}> record I/O byte counters <code>--tree-io</code></label>
            <label class="group-check"><input type="checkbox" id="tree_schedstat"{chk(chk_default('tree', 'schedstat', False))}> record run-queue delay <code>--tree-schedstat</code></label>
            <label class="group-check"><input type="checkbox" id="tree_vmsize"{chk(chk_default('tree', 'vmsize', False))}> record peak RSS + anon/file/shmem RSS + swap <code>--tree-vmsize</code></label>
          </div>
          <label>Timeout seconds <input type="text" id="tree_timeout" value="{val('tree', 'timeout_secs')}" placeholder="(none)"></label>
        </div>
      </div>

      <div class="config-card" data-config="counters">
        <label class="config-toggle"><input type="checkbox" id="counters_enabled"{chk(sec('counters').get('enabled'))}> <strong>Performance counters</strong></label>
        <div class="config-options">
          <div class="group-grid">{counters_groups_html}</div>
          <div class="row">
            <label>Interval seconds <input type="text" id="counters_interval" value="{val('counters', 'interval_secs')}" placeholder="(aggregate)"></label>
            <label class="inline-check"><input type="checkbox" id="counters_per_core"{chk(chk_default('counters', 'per_core', False))}> per-core <code>--per-core</code></label>
            <label class="inline-check"><input type="checkbox" id="counters_per_core_freq"{chk(chk_default('counters', 'per_core_freq', False))}> + live per-core frequency <code>--per-core-freq</code>
              <span class="muted">(needs "per-core" checked too)</span></label>
            <label class="inline-check"><input type="checkbox" id="counters_rusage"{chk(chk_default('counters', 'rusage', False))}> include rusage</label>
            <label class="inline-check"><input type="checkbox" id="counters_csv"{chk(chk_default('counters', 'csv', True))}> CSV output</label>
            <label class="inline-check"><input type="checkbox" id="counters_power"{chk(chk_default('counters', 'power', False))}> package power <code>--power</code>
              <span class="muted">(<code>power</code>/<code>energy-pkg</code> dynamic PMU, RAPL-equivalent -- needs root or CAP_PERFMON)</span></label>
          </div>
          <p class="muted">2+ groups with no interval given automatically bin-pack via native
             multi-pass execution (<code>--passes</code>, wspy's own PMU-fit arithmetic); giving an
             interval, or checking "per-core" or "package power", always uses plain flags for a
             single re-execution instead (<code>--passes</code> rejects <code>--interval</code>/
             <code>--per-core</code>/<code>--power</code> outright). "per-core" combines with an
             interval fine -- <code>--per-core --interval</code> produces one CSV row per core per
             tick, which <code>wspy-core-report</code> (and the plotting templates) already handle by
             averaging each core's rows. Check "package power" <strong>here</strong> (rather than under
             "System metrics" below) specifically to combine it with per-core in the same run --
             that's the only way to get per-core energy (<code>core_joules</code>/<code>core_watts</code>)
             at all, since <code>--power</code> and <code>--per-core</code> only combine when given to
             the same <code>wspy</code> invocation.</p>
        </div>
      </div>

      <div class="config-card" data-config="system">
        <label class="config-toggle"><input type="checkbox" id="system_enabled"{chk(sec('system').get('enabled'))}> <strong>System metrics</strong></label>
        <div class="config-options">
          <label>Interval seconds <input type="text" id="system_interval" value="{val('system', 'interval_secs')}" placeholder="(aggregate)"></label>
          <label class="inline-check"><input type="checkbox" id="system_csv"{chk(chk_default('system', 'csv', True))}> CSV output</label>
          <label class="inline-check"><input type="checkbox" id="system_power"{chk(chk_default('system', 'power', False))}> package power <code>--power</code></label>
          <p class="muted">"package power" here folds <code>--power</code> into this same
             system-wide pass instead of a separate re-execution of the workload -- use this
             one unless you also want per-core energy, which needs the checkbox under
             "Performance counters" instead (see that card's note).</p>
        </div>
      </div>

      <div class="config-card" data-config="gpu">
        <label class="config-toggle"><input type="checkbox" id="gpu_enabled"{chk(sec('gpu').get('enabled'))}> <strong>GPU metrics</strong>
          <span class="muted">(AMD rows need an AMDGPU=1 build, NVIDIA needs NVIDIA=1; otherwise wspy warns and continues)</span></label>
        <div class="config-options">
          <label class="inline-check"><input type="checkbox" id="gpu_busy"{chk(chk_default('gpu', 'busy', False))}> busy % <code>--gpu-busy</code></label>
          <label class="inline-check"><input type="checkbox" id="gpu_metrics"{chk(chk_default('gpu', 'metrics', False))}> extended metrics <code>--gpu-metrics</code></label>
          <label class="inline-check"><input type="checkbox" id="gpu_smi"{chk(chk_default('gpu', 'smi', False))}> ROCm SMI <code>--gpu-smi</code></label>
          <label class="inline-check"><input type="checkbox" id="gpu_nvidia"{chk(chk_default('gpu', 'nvidia', False))}> NVIDIA NVML <code>--gpu-nvidia</code></label>
          <div class="row">
            <label>Device index <input type="text" id="gpu_device" value="{val('gpu', 'device')}" placeholder="(default)"></label>
            <label>Interval seconds <input type="text" id="gpu_interval" value="{val('gpu', 'interval_secs')}" placeholder="(aggregate)"></label>
            <label class="inline-check"><input type="checkbox" id="gpu_csv"{chk(chk_default('gpu', 'csv', True))}> CSV output</label>
          </div>
          <p class="muted">Device index applies to the AMD backends only (<code>--gpu-device</code>) --
             NVIDIA always uses its own default device (<code>--gpu-nvidia-device</code> has no
             checklist field yet).</p>
        </div>
      </div>

      <div class="config-card" data-config="ibs">
        <label class="config-toggle"><input type="checkbox" id="ibs_enabled"{chk(sec('ibs').get('enabled'))}> <strong>AMD IBS</strong>
          <span class="muted">(AMD only)</span></label>
        <div class="config-options">
          <label>Profile
            <select id="ibs_profile">
              <option value="basic"{" selected" if sec('ibs').get('profile') != 'memory-deep' else ""}>basic (unfiltered ibs_fetch+ibs_op)</option>
              <option value="memory-deep"{" selected" if sec('ibs').get('profile') == 'memory-deep' else ""}>memory-deep (l3missonly+ldlat filtering)</option>
            </select>
          </label>
          <label>Interval seconds <input type="text" id="ibs_interval" value="{val('ibs', 'interval_secs')}" placeholder="(aggregate)"></label>
          <div class="row">
            <label><code>--ibs-maxcnt</code> <input type="text" id="ibs_maxcnt" value="{val('ibs', 'maxcnt')}" placeholder="(default)"></label>
            <label><code>--ibs-ldlat</code> <input type="text" id="ibs_ldlat" value="{val('ibs', 'ldlat')}" placeholder="(default)"></label>
            <label><code>--ibs-fetchlat</code> <input type="text" id="ibs_fetchlat" value="{val('ibs', 'fetchlat')}" placeholder="(default)"></label>
          </div>
        </div>
      </div>

      <div class="config-card config-reserved">
        <label class="config-toggle"><input type="checkbox" disabled> <strong>/proc extras</strong>
          <span class="muted">(reserved for a future release &mdash; not implemented yet)</span></label>
      </div>
    </div>

    <div class="config-card" id="custom-plots-card">
      <label class="config-toggle"><strong>Custom plots</strong>
        <span class="muted">(runs regardless of preset vs. custom above)</span></label>
      <div class="config-options">
        <p class="muted">Group specific CSV columns onto a plot of their own, alongside the
           default plot templates &mdash; useful when a counter you care about doesn't share a
           scale with any built-in grouping (<code>wspy-plot --list-templates</code> lists those).
           Column names are the literal CSV header text a counter group produces (e.g. topdown's
           <code>retire,frontend,backend,speculate</code>; see <code>CLAUDE.md</code>'s
           <code>plot.c</code> entry for more).</p>
        <div id="custom-plots-list"></div>
        <div class="add-buttons">
          <button type="button" id="add-custom-plot">+ add custom plot</button>
        </div>
        <label class="inline-check"><input type="checkbox" id="only_custom">
          Only render these custom plots (skip the built-in templates)</label>
      </div>
    </div>

    <div class="config-card" id="affinity-card">
      <label class="config-toggle"><strong>CPU affinity</strong>
        <span class="muted">(runs regardless of preset vs. custom above)</span></label>
      <div class="config-options">
        <p class="muted">Pin the workload to selected CPUs (<code>wspy --affinity</code>) --
           e.g. avoid an SMT sibling, stay on one L3-sharing core-complex, pick only the
           "big" or "little" cores of a heterogeneous (e.g. ARM big.LITTLE) part, or
           enumerate cores by hand. Applies to every pass alike, same as the toggle chips below.</p>
        <div class="row">
          <label><input type="radio" name="affinity_mode" value="all" id="affinity_mode_all" checked>
            All CPUs (default)</label>
          <label><input type="radio" name="affinity_mode" value="nosmt" id="affinity_mode_nosmt">
            No SMT <span class="muted">(one thread per core)</span></label>
          <label><input type="radio" name="affinity_mode" value="domain" id="affinity_mode_domain">
            One L3 domain</label>
          <label><input type="radio" name="affinity_mode" value="coretype" id="affinity_mode_coretype">
            One core type <span class="muted">(e.g. big.LITTLE, ARM only)</span></label>
          <label><input type="radio" name="affinity_mode" value="cpuset" id="affinity_mode_cpuset">
            Explicit CPUs</label>
        </div>
        <div class="row">
          <button type="button" id="affinity-discover">Discover CPUs/domains</button>
          <span id="affinity-discover-status" class="muted"></span>
        </div>
        <div id="affinity-domain-picker" hidden>
          <label>L3 domain <select id="affinity_domain_select"></select></label>
        </div>
        <div id="affinity-coretype-picker" hidden>
          <label>Core type <select id="affinity_coretype_select"></select></label>
        </div>
        <div id="affinity-cpuset-picker" hidden>
          <div id="affinity-cpu-checkboxes" class="row"></div>
        </div>
      </div>
    </div>

    <div class="chips">
      <label class="chip"><input type="checkbox" id="toggle_manifest" checked> Write manifest</label>
      <label class="chip"><input type="checkbox" id="toggle_run_index" checked> Append to run index</label>
      <label class="chip"><input type="checkbox" id="toggle_store_ingest" checked> Ingest into store after run</label>
    </div>

    <label class="inline-check">
      <input type="checkbox" id="queue_job">
      Queue this instead of running it now
      <span class="muted">(writes a job file to <code>{html.escape(cfg["jobs_dir"])}/pending/</code>
        &mdash; process it later with <code>wspy-queue run</code>, from this machine, cron, or after
        copying the job file to another machine with wspy checked out; see
        <code>INVESTIGATION.md</code>'s "What shipped in 4.1")</span>
    </label>

    <fieldset class="preview">
      <legend>Command(s) about to run (copy/paste-able)</legend>
      <pre id="preview">(fill in a workload command above)</pre>
      <p id="preview-notes" class="muted"></p>
    </fieldset>
    <div class="button-row">
      <button type="submit" id="run-button">Run</button>
      <button type="button" id="check-button">Check</button>
    </div>
    <p class="muted">"Check" (item 18) is optional and doesn't launch anything: it reports whether
       <code>perf_event_paranoid</code>/<code>nmi_watchdog</code> are set for unprivileged counter
       access, and -- for a <code>phoronix-test-suite batch-run</code>/<code>run</code>/
       <code>benchmark</code> workload specifically -- whether the test is installed and an
       estimated (not installed, or installed but never run) or measured (already run on this host)
       single-run time.</p>
  </form>
  <div id="check-results" class="check-results" hidden></div>
  <pre id="live-output" class="live-output" hidden></pre>
  <p id="run-result"></p>
</section>
"""


def render_validate_tab(cfg, prefill=None):
    prefill = prefill or {}
    manifests = discover_manifest_paths(cfg["output_root"])
    chips = "".join(
        f'<button type="button" class="add-manifest-chip" data-path="{html.escape(m["path"])}">'
        f'+ {html.escape(m["rel"])}</button>'
        for m in manifests
    ) or '<span class="muted">no manifests found under the output root yet</span>'
    core_report_csvs = discover_percore_csv_paths(cfg["output_root"])
    core_report_chips = "".join(
        f'<button type="button" class="add-core-report-csv-chip" data-path="{html.escape(c["path"])}">'
        f'+ {html.escape(c["rel"])}</button>'
        for c in core_report_csvs
    ) or '<span class="muted">no --per-core CSVs found under the output root yet</span>'
    core_report_path = html.escape(prefill.get("core_report_path", ""))
    return f"""
<section class="panel">
  <h1>Validate</h1>
  <p class="config-label">Runs <code>wspy-validate</code> against one or more manifests: required
     output files present, output CSV well-formed and non-empty, workload exit status, counter
     coverage, sanity ranges on numeric CSV columns.</p>
  <div class="add-buttons">{chips}</div>
  <label>Manifest path(s), one per line
    <textarea id="validate-paths" rows="4" placeholder="/path/to/run.manifest.json"></textarea>
  </label>
  <div class="chips">
    <label class="chip"><input type="checkbox" id="validate-strict"> --strict</label>
    <label class="chip"><input type="checkbox" id="validate-quiet"> --quiet</label>
  </div>
  <button type="button" id="validate-run">Run wspy-validate</button>
  <pre id="validate-cmdline" class="muted" hidden></pre>
  <pre id="validate-output" class="live-output" hidden></pre>

  <h2>Per-core class comparison</h2>
  <p class="config-label">Runs <code>wspy-core-report</code> against an existing <code>--per-core
     --csv</code> wspy output file: cross-core min/max/mean/stddev/coefficient-of-variation for
     every metric column, naming the "hot"/"cold" core by index. On a heterogeneous host (ARM
     big.LITTLE, Intel Atom+Core, AMD Zen5/Zen5c) it also breaks the same stats down by core
     class -- e.g. Zen5 vs. Zen5c IPC/topdown. Re-detects <em>this</em> host's core classes
     fresh, so it must be run on the same host that collected the CSV (or one with identical
     topology).</p>
  <div class="add-buttons">{core_report_chips}</div>
  <label>Per-core CSV path
    <input type="text" id="core-report-path" value="{core_report_path}" placeholder="/path/to/percore.csv">
  </label>
  <label>Metric filter(s), comma-separated
    <input type="text" id="core-report-metrics" placeholder="(all, e.g. ipc,retire,frontend,backend)">
  </label>
  <div class="chips">
    <label class="chip"><input type="checkbox" id="core-report-csv"> --csv output</label>
  </div>
  <button type="button" id="core-report-run">Run wspy-core-report</button>
  <pre id="core-report-cmdline" class="muted" hidden></pre>
  <pre id="core-report-output" class="live-output" hidden></pre>
</section>
"""


def render_store_tab(cfg):
    db = html.escape(cfg["store_db"])
    run_index = html.escape(cfg["run_index_file"])
    return f"""
<section class="panel">
  <h1>Store &amp; Summary</h1>
  <p class="config-label">Ingest <code>--run-index</code> file(s) into the normalized store
     (<code>wspy-store</code>), then query it for a min/max/mean/median/stddev/outlier-flag summary
     table grouped by workload command, hostname, or CPU vendor (<code>wspy-summary</code>).</p>

  <h2>Ingest</h2>
  <div class="row">
    <label>Database path <input type="text" id="store-db" value="{db}"></label>
    <label>Run-index file(s), one per line
      <textarea id="store-run-index" rows="2">{run_index}</textarea>
    </label>
  </div>
  <div class="chips">
    <label class="chip"><input type="checkbox" id="store-no-manifest-enrich"> skip manifest enrich</label>
    <label class="chip"><input type="checkbox" id="store-no-metrics-ingest"> skip CSV metrics ingest</label>
    <label class="chip"><input type="checkbox" id="store-strict"> --strict</label>
  </div>
  <button type="button" id="store-ingest-run">Run wspy-store</button>
  <pre id="store-ingest-cmdline" class="muted" hidden></pre>
  <pre id="store-ingest-output" class="live-output" hidden></pre>

  <h2>Summary query</h2>
  <div class="row">
    <label>Database path <input type="text" id="summary-db" value="{db}"></label>
    <label>Command filter <input type="text" id="summary-command" placeholder="(substring, optional)"></label>
    <label>Hostname filter <input type="text" id="summary-hostname" placeholder="(optional)"></label>
  </div>
  <div class="row">
    <label>Metric filter(s), comma-separated <input type="text" id="summary-metrics" placeholder="(all)"></label>
    <label>Group by
      <select id="summary-group-by">
        <option value="command">command</option>
        <option value="hostname">hostname</option>
        <option value="cpu_vendor">cpu_vendor</option>
      </select>
    </label>
  </div>
  <div class="row">
    <label>Outlier stddev <input type="text" id="summary-outlier" placeholder="2.0"></label>
    <label>Min runs <input type="text" id="summary-min-runs" placeholder="1"></label>
    <label class="chip"><input type="checkbox" id="summary-csv"> CSV output</label>
    <label class="chip"><input type="checkbox" id="summary-show-runs"> show contributing runs</label>
    <label class="chip"><input type="checkbox" id="summary-strict"> --strict</label>
  </div>
  <button type="button" id="summary-run">Run wspy-summary</button>
  <pre id="summary-cmdline" class="muted" hidden></pre>
  <pre id="summary-output" class="live-output" hidden></pre>

  <h2>Trace a run</h2>
  <p class="config-label">Traceability links (summary row &rarr; manifest &rarr; raw CSV &rarr; plots
     &rarr; tree artifacts): paste a <code>hostname:run_id</code> from the "show contributing runs"
     column above (or from a run-index record) to resolve it back to its command line, manifest,
     raw CSV, tree file, and plots &mdash; <code>wspy-summary --trace</code>.</p>
  <div class="row">
    <label>Database path <input type="text" id="trace-db" value="{db}"></label>
    <label>hostname:run_id <input type="text" id="trace-key" placeholder="host1:1699999999-1234"></label>
  </div>
  <button type="button" id="trace-run">Trace run</button>
  <pre id="trace-cmdline" class="muted" hidden></pre>
  <div id="trace-output" hidden></div>
</section>
"""


def render_discovery_tab():
    preflight_groups_html = render_group_checkboxes("preflight", checked_by_default=("ipc",))
    return f"""
<section class="panel">
  <h1>Discovery</h1>
  <p class="config-label">Discovery-only commands &mdash; no workload, just a report.
     <code>wspy --capabilities</code> probes what this host/kernel/build actually supports;
     <code>wspy --preflight</code> checks whether a chosen counter selection fits the available
     hardware PMU budget without multiplexing.</p>

  <h2>Capabilities</h2>
  <button type="button" id="capabilities-run">Run wspy --capabilities</button>
  <pre id="capabilities-cmdline" class="muted" hidden></pre>
  <pre id="capabilities-output" class="live-output" hidden></pre>

  <h2>Preflight</h2>
  <div class="group-grid">{preflight_groups_html}</div>
  <button type="button" id="preflight-run">Run wspy --preflight</button>
  <pre id="preflight-cmdline" class="muted" hidden></pre>
  <pre id="preflight-output" class="live-output" hidden></pre>
</section>
"""


def render_phoronix_inventory_groups(dest_root):
    points = joblib.list_materialized_phoronix_test_points(dest_root)
    groups = joblib.group_materialized_phoronix_points_by_test(points)
    blocks = []
    for g in groups:
        description = joblib.read_phoronix_test_description(dest_root, g["bare_name"])
        summary_bits = [f'{g["total_count"]} point{"s" if g["total_count"] != 1 else ""}']
        if g["installed_count"]:
            summary_bits.append(f'{g["installed_count"]} installed')
        summary_extra = " &mdash; " + html.escape(description) if description else ""
        search_attr = html.escape(f'{g["bare_name"]} {description or ""}'.lower())
        run_status = g["run_status"]
        run_dot_title = {
            "all": "every test point has at least one run",
            "some": "some test points have at least one run",
            "none": "no test points have any runs yet",
        }[run_status]
        run_dot_html = (f'<span class="phoronix-run-dot phoronix-run-{run_status}" '
                         f'title="{html.escape(run_dot_title)}"></span>')

        group_test_id = g["points"][0]["test_id"] if g["points"] else ""
        installed_versions = joblib.list_installed_phoronix_test_versions(group_test_id) if group_test_id else []

        rows = []
        for p in g["points"]:
            if p["runs"]:
                runs_html = ", ".join(
                    f'<a href="/report/{html.escape(r["suite"])}/{html.escape(r["benchmark"])}/'
                    f'{html.escape(r["run_id"])}">{html.escape(r["run_id"])}</a>'
                    for r in p["runs"])
            else:
                runs_html = '<span class="muted">none yet</span>'
            installed_text = {True: "yes", False: "no"}.get(p.get("installed"), "?")
            pinned_version = joblib.phoronix_pinned_version(p["test_id"])
            repin_html = ""
            if pinned_version and installed_versions and pinned_version not in installed_versions:
                other_versions = installed_versions
                mismatch_title = (f'pinned v{pinned_version} is not installed, but v'
                                   f'{", v".join(other_versions)} {"is" if len(other_versions) == 1 else "are"} '
                                   f'-- the run will fail until v{pinned_version} is installed')
                installed_text += (f' <span class="phoronix-version-mismatch" title="{html.escape(mismatch_title)}">'
                                    f'&#9888; v{html.escape(pinned_version)} pinned, '
                                    f'v{html.escape(", v".join(other_versions))} installed</span>')
                version_options = "".join(f'<option value="{html.escape(v)}">v{html.escape(v)}</option>'
                                           for v in other_versions)
                repin_html = (f'<select class="phoronix-repin-version">{version_options}</select> '
                               f'<button type="button" class="phoronix-repin" '
                               f'data-dir="{html.escape(p["dir"])}" '
                               f'title="Rewrite this test point\'s pinned version to the one selected -- '
                               f'leaves other test points alone; a re-pin only ever moves forward to an '
                               f'installed version, never automatic">Re-pin</button>'
                               )
            rows.append(
                f'<tr data-phoronix-options="{html.escape(p["options_slug"].lower())}">'
                f'<td>{html.escape(p["options_slug"])}</td>'
                f'<td>{installed_text}</td><td>{runs_html}</td>'
                f'<td><button type="button" class="phoronix-use-in-run" '
                f'data-dir="{html.escape(p["dir"])}">Use in Run tab</button> {repin_html}</td></tr>'
            )

        blocks.append(
            f'<details class="phoronix-test-group" data-phoronix-search="{search_attr}">'
            f'<summary>{run_dot_html}<strong>{html.escape(g["bare_name"])}</strong> '
            f'<span class="muted">({", ".join(summary_bits)}){summary_extra}</span></summary>'
            f'<table class="reports"><thead><tr><th>Options</th><th>Installed</th>'
            f'<th>Runs</th><th></th></tr></thead><tbody>{"".join(rows)}</tbody></table>'
            f'</details>'
        )
    return "".join(blocks), len(points), len(groups)


def render_phoronix_tab(cfg):
    dest_root = os.path.join(REPO_ROOT, "workload", "phoronix")
    installed_options = "".join(
        f'<option value="{html.escape(name)}">{html.escape(name)}</option>'
        for name in joblib.list_installed_phoronix_suites()
    ) or '<option value="" disabled selected>(none found)</option>'
    inventory_groups_html, point_count, test_count = render_phoronix_inventory_groups(dest_root)
    if inventory_groups_html:
        inventory_html = f"""
    <div class="row">
      <label>Filter <input type="text" id="phoronix-filter"
             placeholder="test name, description, or options substring"></label>
      <button type="button" id="phoronix-expand-all">Expand all</button>
      <button type="button" id="phoronix-collapse-all">Collapse all</button>
    </div>
    <p class="muted" id="phoronix-inventory-count">{point_count} test point(s) across {test_count} test(s)</p>
    <div id="phoronix-inventory-groups">{inventory_groups_html}</div>
    <p class="muted" id="phoronix-filter-empty" hidden>No tests match this filter.</p>
"""
    else:
        inventory_html = '<p class="muted">No test points materialized yet.</p>'
    return f"""
<section class="panel">
  <h1>Phoronix</h1>
  <p class="config-label">Decomposes an already-published Phoronix result or suite into one
     minimal single-test-point suite per (test, option-combination) &mdash; materialized under
     <code>workload/phoronix/&lt;test&gt;/&lt;options&gt;/</code> and registered with
     <code>wspy-ledger --add</code> (INVESTIGATION.md item 26's front-end phase). The INSTALLED
     column tells you which points still need <code>phoronix-test-suite install</code> run by
     hand &mdash; nothing here installs or runs anything on its own, except that "Use in Run tab"
     copies a test point's suite into <code>~/.phoronix-test-suite/test-suites/local/</code> so the
     Run tab's prefilled command is immediately runnable.</p>

  <h2>Inventory</h2>
  {inventory_html}

  <h2>Materialize new test points</h2>
  <div class="chips">
    <label class="chip"><input type="radio" name="phoronix-source" value="result" checked> OpenBenchmarking result</label>
    <label class="chip"><input type="radio" name="phoronix-source" value="file"> XML file on disk</label>
    <label class="chip"><input type="radio" name="phoronix-source" value="installed"> Installed suite</label>
  </div>

  <label id="phoronix-result-row">Result URL or ID
    <input type="text" id="phoronix-result" placeholder="https://openbenchmarking.org/result/2607160-PTS-7700X3D886">
  </label>
  <label id="phoronix-file-row" hidden>XML file path
    <input type="text" id="phoronix-file" placeholder="/home/you/Downloads/result-suite.xml">
  </label>
  <label id="phoronix-installed-row" hidden>Installed suite
    <select id="phoronix-installed">{installed_options}</select>
  </label>

  <label>Destination root
    <input type="text" id="phoronix-dest" value="{html.escape(dest_root)}">
  </label>
  <label>wspy-ledger list path (blank = &lt;destination&gt;/backlog.txt)
    <input type="text" id="phoronix-ledger-list" placeholder="workload/phoronix/backlog.txt">
  </label>
  <div class="chips">
    <label class="chip"><input type="checkbox" id="phoronix-dry-run" checked> Dry run</label>
    <label class="chip"><input type="checkbox" id="phoronix-no-ledger"> Skip wspy-ledger --add</label>
    <label class="chip"><input type="checkbox" id="phoronix-no-check-installed"> Skip installed check (faster)</label>
  </div>
  <button type="button" id="phoronix-run">Materialize</button>
  <pre id="phoronix-cmdline" class="muted" hidden></pre>
  <p id="phoronix-error" class="muted" hidden></p>
  <div id="phoronix-results" hidden>
    <table class="reports" id="phoronix-table">
      <thead><tr><th>Test</th><th>Options</th><th>Installed</th><th>Status</th><th>Ledger</th></tr></thead>
      <tbody id="phoronix-table-body"></tbody>
    </table>
  </div>
  <p id="phoronix-use-in-run-error" class="muted" hidden></p>
  <p id="phoronix-repin-error" class="muted" hidden></p>
</section>
"""


def render_index(cfg, prefill):
    output_root = cfg["output_root"]
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
        '<button type="submit">Compare selected</button> '
        '<button type="submit" formaction="/tree-diff">Tree diff selected</button>'
        "</form>" if rows else "<p class=\"muted\">No runs yet.</p>"
    )

    # Normally the Run tab is what a page load lands on; a report page's
    # "Compare cores" link (core_report_csv query param, do_GET() above)
    # instead wants the Validate tab active with its CSV path prefilled --
    # active_tab picks which one starts visible, purely a server-side
    # render-time choice (wireTabs() in app.js only wires click handlers,
    # it never inspects initial state).
    active_tab = prefill.get("active_tab") or "run"
    def tab_btn(name, label):
        cls = "tab-btn active" if name == active_tab else "tab-btn"
        return f'<button type="button" class="{cls}" data-tab="{name}">{label}</button>'
    def tab_hidden(name):
        return "" if name == active_tab else " hidden"

    body = f"""
<nav class="tabs">
  {tab_btn("run", "Run")}
  {tab_btn("validate", "Validate")}
  {tab_btn("store", "Store &amp; Summary")}
  {tab_btn("discovery", "Discovery")}
  {tab_btn("phoronix", "Phoronix")}
</nav>
<div class="tab-panel" id="tab-run"{tab_hidden("run")}>{render_run_tab(prefill, cfg)}</div>
<div class="tab-panel" id="tab-validate"{tab_hidden("validate")}>{render_validate_tab(cfg, prefill)}</div>
<div class="tab-panel" id="tab-store"{tab_hidden("store")}>{render_store_tab(cfg)}</div>
<div class="tab-panel" id="tab-discovery"{tab_hidden("discovery")}>{render_discovery_tab()}</div>
<div class="tab-panel" id="tab-phoronix"{tab_hidden("phoronix")}>{render_phoronix_tab(cfg)}</div>
<section class="panel">
  <h2>Recent reports</h2>
  {reports_html}
  <p><a href="/history">Browse &amp; search all runs &rarr;</a></p>
</section>
<script src="/static/app.js"></script>
"""
    return page("wspy web launcher", body)


def render_history(cfg, qs):
    """Item 11's dedicated page: search/filter/paginate over every run
    directory (discover_run_history()), not just the homepage's 50-newest
    quick list. Plain GET with query-string filters -- bookmarkable/
    shareable and reloadable without resubmission, same pattern /compare
    already uses, no JS required."""
    output_root = cfg["output_root"]

    def qparam(name):
        return (qs.get(name, [""])[0] or "").strip()

    filters = {
        "q": qparam("q").lower(),
        "suite": qparam("suite").lower(),
        "benchmark": qparam("benchmark").lower(),
        "hostname": qparam("hostname").lower(),
        "cpu_vendor": qparam("cpu_vendor"),
        "status": qparam("status"),
        "date_from": qparam("date_from"),
        "date_to": qparam("date_to"),
    }
    try:
        page_num = max(1, int(qparam("page") or "1"))
    except ValueError:
        page_num = 1

    results, total = discover_run_history(output_root, filters, page_num)

    def field(name, label, kind="text"):
        value = qparam(name)
        return (f'<label>{html.escape(label)}<br>'
                f'<input type="{kind}" name="{name}" value="{html.escape(value)}"></label>')

    status_value = filters["status"]
    status_options = "".join(
        f'<option value="{v}"{" selected" if v == status_value else ""}>{l}</option>'
        for v, l in (("", "any"), ("ok", "ok"), ("failed", "failed"), ("unknown", "unknown"))
    )

    filter_form = f"""
<form method="get" action="/history" class="history-filters">
  <div class="row">
    {field("q", "Command contains")}
    {field("suite", "Suite")}
    {field("benchmark", "Benchmark")}
  </div>
  <div class="row">
    {field("hostname", "Hostname contains")}
    {field("cpu_vendor", "CPU vendor")}
    <label>Status<br><select name="status">{status_options}</select></label>
  </div>
  <div class="row">
    {field("date_from", "From date", kind="date")}
    {field("date_to", "To date", kind="date")}
  </div>
  <button type="submit">Search</button>
  <a href="/history">Clear filters</a>
</form>
"""

    rows = []
    for r in results:
        ts = r["start_time"] or datetime.fromtimestamp(r["mtime"]).strftime("%Y-%m-%dT%H:%M:%S")
        key = f"{r['suite']}/{r['benchmark']}/{r['run_id']}"
        url = f"/report/{_urlescape(r['suite'])}/{_urlescape(r['benchmark'])}/{_urlescape(r['run_id'])}"
        if r["workload_str"]:
            shown = r["workload_str"] if len(r["workload_str"]) <= 60 else r["workload_str"][:60] + "…"
            workload_cell = f'<code title="{html.escape(r["workload_str"])}">{html.escape(shown)}</code>'
        else:
            workload_cell = '<span class="muted">&mdash;</span>'
        elapsed_cell = (f'{r["elapsed_seconds"]:.1f}s'
                         if isinstance(r["elapsed_seconds"], (int, float)) else "—")
        rows.append(
            "<tr>"
            f'<td><input type="checkbox" name="r" value="{html.escape(key)}"></td>'
            f"<td>{html.escape(ts)}</td>"
            f"<td>{html.escape(r['suite'])}</td>"
            f"<td>{html.escape(r['benchmark'])}</td>"
            f'<td><a href="{url}">{html.escape(r["run_id"])}</a></td>'
            f"<td>{workload_cell}</td>"
            f'<td><span class="status-{html.escape(r["status"])}">{html.escape(r["status"])}</span></td>'
            f'<td>{html.escape(r["hostname"] or "—")}</td>'
            f'<td>{html.escape(r["cpu_vendor"] or "—")}</td>'
            f"<td>{elapsed_cell}</td>"
            "</tr>"
        )

    table_html = (
        '<form method="get" action="/compare">'
        '<table class="reports history"><thead><tr><th></th><th>when</th><th>suite</th>'
        "<th>benchmark</th><th>run</th><th>workload</th><th>status</th><th>host</th>"
        "<th>vendor</th><th>elapsed</th></tr></thead><tbody>" + "".join(rows) +
        "</tbody></table>"
        '<button type="submit">Compare selected</button> '
        '<button type="submit" formaction="/tree-diff">Tree diff selected</button>'
        "</form>"
    ) if rows else '<p class="muted">No runs match these filters.</p>'

    last_page = max(1, (total + HISTORY_PAGE_SIZE - 1) // HISTORY_PAGE_SIZE)

    def page_link(p, label):
        parts = [f"{k}={_urlescape(v)}" for k, v in filters.items() if v]
        parts.append(f"page={p}")
        return f'<a href="/history?{"&".join(parts)}">{label}</a>'

    pagination = []
    if page_num > 1:
        pagination.append(page_link(page_num - 1, "&laquo; Prev"))
    pagination.append(f"Page {page_num} of {last_page} ({total} runs)")
    if page_num < last_page:
        pagination.append(page_link(page_num + 1, "Next &raquo;"))

    body = f"""
<section class="panel">
  <h1>Run history</h1>
  <p class="config-label">Search and browse every collected run directory under the output root
     (item 11, historical run index browser/search) &mdash; a directory/manifest scan, not a
     dependency on <code>wspy-store</code> ingestion, so it covers every run whether or not that
     toggle was on for it.</p>
  {filter_form}
  <p>{" &middot; ".join(pagination)}</p>
  {table_html}
  <p><a href="/">Back to launcher</a></p>
</section>
"""
    return page("wspy run history", body)


def apply_studio_post(rundir, form):
    """Reconstructs the block list from a studio form submission (in DOM/
    submission order, which is the current on-page order) and applies the one
    op the clicked submit button carries. Every submit button lives inside
    the same <form> as every block's editable fields and the report-level
    overview note, so a reorder/add/delete click also persists whatever
    title/depth/commentary/overview edits were pending -- there's no
    separate "save" step to forget."""
    overview_note = form.get("overview_note", [""])[0]
    ids = form.get("id", [])
    kinds = form.get("kind", [])
    source_files = form.get("source_file", [])
    source_kinds = form.get("source_kind", [])
    titles = form.get("title", [])
    depths = form.get("depth", [])
    excerpt_lines = form.get("excerpt_lines", [])
    commentaries = form.get("commentary", [])
    ai_generated_flags = form.get("ai_generated", [])
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
            "ai_generated": i < len(ai_generated_flags) and ai_generated_flags[i] == "1",
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
        available = {item["filename"]: item for item in collect_run_files(rundir)}
        item = available.get(filename)
        if item is not None:
            blocks.append(new_block("artifact", source_file=filename, source_kind=src_kind,
                                     title=item["label"], ai_generated=item.get("ai_generated", False)))
    elif op == "add-freeform":
        blocks.append(new_block("freeform", title="New section"))
    elif op.startswith("copy-commentary:"):
        # Design decision #7 in the Ollama deep-dive: wspy-analyze's narrative
        # output is meant to become editable report commentary, not just sit
        # embedded as a raw artifact. This is a plain copy (not a link) --
        # the block keeps its ai_generated flag regardless, so the copied
        # text still renders with an AI-generated marker in the studio and
        # every export format, even after a human edits it down.
        bid = op.split(":", 1)[1]
        for b in blocks:
            if b["id"] == bid and b.get("ai_generated") and b.get("source_file"):
                text, _size = read_text_safely(os.path.join(rundir, b["source_file"]))
                if text is not None:
                    b["commentary"] = text.strip()
                break

    for b in blocks:
        allowed = allowed_depths(b)
        if b["depth"] not in allowed:
            b["depth"] = allowed[-1] if allowed else "none"

    save_curation(rundir, {"blocks": blocks, "overview_note": overview_note})


def render_analyze_card(suite, benchmark, run_id):
    """Report-page "AI narrative analysis" card: triggers wspy-analyze
    against this run directory (see CLAUDE.md's wspy-analyze entry) without
    leaving the browser. Model discovery is a live Ollama call
    (POST /api/discovery/ollama-models), so it's opt-in via a button rather
    than done at page-render time -- the same "advisory, only when asked"
    treatment the Run tab's Check button gives its own live probes, since a
    report page shouldn't hang (or fail) on Ollama not being installed/
    running just to render. Model names picked from that discovery call are
    added as chips into the free-text field below, mirroring the Validate
    tab's "+ add manifest" chip pattern (renderValidateTab()) rather than a
    dropdown, since --model is repeatable and a user may want more than one.
    A model query can run for minutes, so this streams progress over SSE the
    same way a launched workload's live log does (wireRunTab() in app.js),
    not the Discovery tab's bounded run_sync(); wireAnalyzeForm() in app.js
    is the client-side counterpart."""
    analyze_url = f"/api/analyze/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    return f"""
<h2>AI narrative analysis</h2>
<p class="muted">Runs <code>wspy-analyze</code> against this run directory's already-computed,
   already-validated data via a locally running Ollama daemon -- narration, not a re-derived
   verdict. Output lands back in this run directory as <code>aianalysis.&lt;model&gt;.txt</code>
   and shows up as an "+ add" candidate in the curation studio once this finishes (reload the
   page).</p>
<form id="analyze-form">
  <div class="row">
    <label>Model(s) <span class="muted">(comma-separated Ollama model names -- leave blank to use
      wspy-analyze's own default model if it's installed, e.g. gpt-oss:20b)</span>
      <input type="text" id="analyze-models" placeholder="e.g. llama3.1:8b (blank = default model)">
    </label>
    <button type="button" id="analyze-discover-models">Discover installed models</button>
  </div>
  <div id="analyze-model-chips" class="add-buttons"></div>
  <div class="row" style="margin-top: 0.5rem;">
    <label>Prompt Template
      <select id="analyze-template">
        <option value="perf_analysis.tmpl">Default (perf_analysis.tmpl)</option>
        <option value="perf_analysis2.tmpl">Structured (perf_analysis2.tmpl)</option>
      </select>
    </label>
  </div>
  <div class="chips">
    <label class="chip"><input type="checkbox" id="analyze-all-models"> query every installed model (--all-models)</label>
    <label class="chip"><input type="checkbox" id="analyze-critique"> also ask for prompt critique (--critique)</label>
  </div>
  <button type="button" id="analyze-run" data-analyze-url="{html.escape(analyze_url)}">Run AI analysis</button>
  <pre id="analyze-log" class="live-output" hidden></pre>
  <div id="analyze-result"></div>
</form>
"""


def _studio_link_and_curated(rundir, base_url, suite, benchmark, run_id, raw_html):
    """Shared tail assembly for both report shapes: the curated block
    sequence (when curation.json has at least one included block) plus the
    studio link, with the raw artifact listing collapsed underneath via
    <details> once a curated view exists to lead with -- open by default
    when there's no curation yet, since the raw listing is then the only
    content this report has."""
    studio_url = f"/studio/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    export_url = f"/export/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    bundle_url = f"/bundle/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}/download"
    bundle_link = (f'<a href="{bundle_url}">Download reproducibility bundle</a> '
                   f'<span class="muted">(manifest + raw + derived, .tar.gz)</span>')
    curated_html = render_curated_section(rundir, base_url, suite, benchmark, run_id)
    if curated_html:
        return (f'<p><a href="{studio_url}">Edit curation</a> &middot; '
                f'<a href="{export_url}">Export</a> &middot; {bundle_link}</p>'
                f'{curated_html}'
                f'<details><summary>Raw artifacts</summary>{raw_html}</details>')
    return (f'<p><a href="{studio_url}">Curate this report</a> '
            f'<span class="muted">(select/reorder/annotate the artifacts below)</span></p>'
            f'<p>{bundle_link}</p>'
            f'{raw_html}')


def render_studio(rundir, suite, benchmark, run_id):
    curation = load_curation(rundir) or {"blocks": []}
    blocks = curation.get("blocks", [])
    overview_note = curation.get("overview_note", "")
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
        ai_badge = ' <span class="badge ai-badge">AI-generated</span>' if b.get("ai_generated") else ""
        # Offered only for an artifact block whose source is model output (not every
        # ai_generated block -- a freeform block can't reach this state) -- see design
        # decision #7: the point is turning wspy-analyze's prose into editable
        # commentary, not just displaying it as another embedded artifact.
        copy_button = (
            f'<button type="submit" name="op" value="copy-commentary:{html.escape(b["id"])}">'
            f'Copy analysis into commentary</button>'
            if b.get("ai_generated") and b.get("kind") == "artifact" else ""
        )
        show_excerpt = "excerpt" in depths
        cards.append(f"""
<div class="block-card">
  <input type="hidden" name="id" value="{html.escape(b['id'])}">
  <input type="hidden" name="kind" value="{html.escape(b.get('kind', 'artifact'))}">
  <input type="hidden" name="source_file" value="{html.escape(b.get('source_file') or '')}">
  <input type="hidden" name="source_kind" value="{html.escape(b.get('source_kind') or '')}">
  <input type="hidden" name="ai_generated" value="{'1' if b.get('ai_generated') else ''}">
  <div class="block-card-head">
    <label>Title
      <input type="text" name="title" value="{html.escape(b.get('title') or '')}">
    </label>
    {source_note}{ai_badge}
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
  {copy_button}
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
  <p><a href="/report/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}">Back to report</a>
     &middot; <a href="/export/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}">Export</a></p>
  <form method="post" action="{action}">
    <div class="block-card overview-card">
      <label>Report overview <span class="muted">(one note for the report as a whole, separate from
        each block's own commentary below)</span>
        <textarea name="overview_note" rows="3">{html.escape(overview_note)}</textarea>
      </label>
    </div>
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
    # Item 17: item 6's fixed-config launcher never sets --preset-name/
    # --config-name (see execute_run()), so this is almost always None --
    # kept here (rather than skipped) so a manifest hand-annotated with
    # --config-name outside this UI, or a future change to execute_run(),
    # is picked up automatically instead of needing this function touched.
    config_provenance = read_manifest_config_provenance(manifest_path)
    rerun_preset, rerun_checklist = checklist_from_pass_provenance([config_provenance])

    parts = [f"<h1>Report: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>"]

    if workload_str:
        parts.append(f"<p>Workload: <code>{html.escape(workload_str)}</code></p>")
        cp_text = format_config_provenance(config_provenance)
        if cp_text:
            parts.append(f'<p class="muted">Configuration: {html.escape(cp_text)}</p>')
        rerun_url = build_rerun_url(workload_str, suite, benchmark, rerun_preset, rerun_checklist)
        parts.append(f'<p><a href="{rerun_url}">Customize &amp; run again</a></p>')
    else:
        parts.append('<p class="muted">No manifest found; can\'t restore workload command.</p>')

    if os.path.isfile(os.path.join(rundir, TREE_TXT_NAME)):
        tree_url = f"/tree-viewer/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
        parts.append(f'<p><a href="{tree_url}">Interactive tree viewer</a></p>')

    parts.append(render_analyze_card(suite, benchmark, run_id))

    raw = ["<h2>Artifacts</h2><ul class=\"artifacts\">"]
    plot_pngs = list_plot_pngs(rundir)
    if os.path.exists(png_path):
        # Legacy root-level plot from a run predating item 12 (the retired
        # gnuplot.sh wrote directly into rundir, not rundir/plots/).
        plot_pngs = [PNG_NAME] + plot_pngs
    if plot_pngs:
        for f in plot_pngs:
            raw.append(f'<li>Plot ({html.escape(f)}):<br>'
                       f'<img class="plot" src="{base}/{_urlescape(f)}" alt="{html.escape(f)}"></li>')
    else:
        raw.append('<li class="muted">no plots generated</li>')
    if os.path.exists(csv_path):
        raw.append(f'<li><a href="{base}/{CSV_NAME}">{CSV_NAME}</a> (raw CSV)'
                   f'{core_report_link(csv_path)}</li>')
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

    body = ("<section class=\"panel\">" + "".join(parts) + "</section>"
            '<script src="/static/app.js"></script>')
    return page(f"wspy report: {benchmark}/{run_id}", body)


def render_wspy_run_report(rundir, suite, benchmark, run_id, run_manifest):
    base = f"/files/{suite}/{benchmark}/{run_id}"
    workload = run_manifest.get("command") or None
    workload_str = shlex.join(workload) if workload else None

    # INVESTIGATION.md's "What shipped in 4.1", "Browse-reports": relate
    # this report's artifacts back to the preset/configuration/option choices
    # that produced them (structured configuration provenance's
    # configuration_provenance, recorded per-pass since each pass is its own
    # wspy invocation) -- read every pass's own manifest once up front so
    # both the per-pass display below and the aggregated rerun link can use
    # it without re-parsing.
    passes = run_manifest.get("passes", [])
    pass_provenance = [
        (read_manifest_config_provenance(os.path.join(rundir, p["manifest"]))
         if p.get("manifest") else None)
        for p in passes
    ]
    rerun_preset, rerun_checklist = checklist_from_pass_provenance(pass_provenance)

    parts = [f"<h1>Report: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>",
             '<p class="muted">Produced by the wspy-run profile launcher (item 7) or the Run tab\'s '
             'configuration checklist (item 9).</p>']

    if workload_str:
        parts.append(f"<p>Workload: <code>{html.escape(workload_str)}</code></p>")
        rerun_url = build_rerun_url(workload_str, suite, benchmark, rerun_preset, rerun_checklist)
        if rerun_preset:
            note = f"(preset &ldquo;{html.escape(rerun_preset)}&rdquo; prefilled)"
        elif rerun_checklist:
            note = "(configuration checklist prefilled from this run)"
        else:
            note = "(no structured configuration recorded for this run -- re-pick it by hand; " \
                   "only workload/suite/benchmark are prefilled)"
        parts.append(f'<p><a href="{rerun_url}">Customize &amp; run again</a> '
                      f'<span class="muted">{note}</span></p>')
    else:
        parts.append('<p class="muted">No workload command recorded in manifest.json.</p>')

    if os.path.isfile(os.path.join(rundir, TREE_TXT_NAME)):
        tree_url = f"/tree-viewer/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
        parts.append(f'<p><a href="{tree_url}">Interactive tree viewer</a></p>')

    parts.append(render_analyze_card(suite, benchmark, run_id))

    accounted_for = {RUN_MANIFEST_NAME}
    raw = []

    raw.append("<h2>Passes</h2><ul class=\"artifacts\">")
    for p, cp in zip(passes, pass_provenance):
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
                raw.append(f'<a href="{base}/{_urlescape(output)}">{html.escape(output)}</a>'
                           f'{core_report_link(output_path)}')
            else:
                raw.append(f'<span class="muted">{html.escape(output)} (missing)</span>')
        if pass_manifest:
            accounted_for.add(pass_manifest)
            if os.path.isfile(os.path.join(rundir, pass_manifest)):
                raw.append(f' &middot; <a href="{base}/{_urlescape(pass_manifest)}">manifest</a>')
        cp_text = format_config_provenance(cp)
        if cp_text:
            raw.append(f'<br><span class="muted">{html.escape(cp_text)}</span>')
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
    # claimed, plus every *.png wspy-plot (item 12) wrote into plots/ --
    # neither of which wspy-run's own passes[] list knows about. Scanned
    # rather than hardcoded so any counter-group combination's plots show up
    # automatically, whatever templates happened to match.
    accounted_for.add(CURATION_NAME)
    try:
        extra = sorted(
            f for f in os.listdir(rundir)
            if f not in accounted_for and os.path.isfile(os.path.join(rundir, f))
        )
    except OSError:
        extra = []
    extra += list_plot_pngs(rundir)
    if extra:
        raw.append("<h2>Other artifacts</h2><ul class=\"artifacts\">")
        for f in extra:
            if f.endswith(".png"):
                raw.append(f'<li>{html.escape(f)}:<br>'
                          f'<img class="plot" src="{base}/{_urlescape(f)}" alt="{html.escape(f)}"></li>')
            else:
                raw.append(f'<li><a href="{base}/{_urlescape(f)}">{html.escape(f)}</a>'
                           f'{core_report_link(os.path.join(rundir, f))}</li>')
        raw.append("</ul>")

    parts.append(_studio_link_and_curated(rundir, base, suite, benchmark, run_id, "".join(raw)))

    body = ("<section class=\"panel\">" + "".join(parts) + "</section>"
            '<script src="/static/app.js"></script>')
    return page(f"wspy report: {benchmark}/{run_id}", body)


def _urlescape(s):
    from urllib.parse import quote
    # safe="/" so a relative filename like "plots/foo.png" (item 12's plot
    # PNGs, one directory level under a run dir) keeps its literal "/" as a
    # path separator -- nothing on the receiving end (do_GET's routing
    # below) unquotes the path, so a %2F here would arrive as a literal,
    # unmatchable "%2F" in the filename instead of a directory separator.
    return quote(s, safe="/")


def build_rerun_url(workload_str, suite, benchmark, preset=None, checklist=None):
    """Builds the '/?' URL item 17's "customize & run again" links use.
    Always restores workload/suite/benchmark (pre-item-17 behavior); when
    checklist_from_pass_provenance() found a restorable preset or checklist
    for the report, it also rides along as a single 'config' query
    parameter (JSON -- a checklist is a nested structure the older flat
    per-field query-param scheme can't express). preset/checklist are
    mutually exclusive by construction; passing neither just omits the
    parameter, identical to a pre-item-17 rerun link."""
    from urllib.parse import quote
    url = ("/?" +
           "workload=" + _urlescape(workload_str) +
           "&suite=" + _urlescape(suite) +
           "&benchmark=" + _urlescape(benchmark))
    state = {}
    if preset:
        state["preset"] = preset
    elif checklist:
        state["checklist"] = checklist
    if state:
        url += "&config=" + quote(json.dumps(state))
    return url


def resolve_compare_runs(output_root, keys):
    """Parses/dedupes/validates a list of "<suite>/<benchmark>/<run_id>"
    keys into run dicts (with rundir/base/files/workload_str populated),
    shared by render_compare() and the compare-curation edit form so the
    two can never resolve a different run set for what's meant to be the
    same comparison. Returns [] if fewer than 2 keys resolve to a real run
    directory -- callers treat that as "nothing to compare"."""
    runs = []
    seen = set()
    for key in keys:
        segs = parse_run_key(key)
        if segs is None or key in seen:
            continue
        seen.add(key)
        suite, benchmark, run_id = segs
        rundir = os.path.join(output_root, suite, benchmark, run_id)
        if not os.path.isdir(rundir):
            continue
        runs.append({"suite": suite, "benchmark": benchmark, "run_id": run_id, "rundir": rundir})

    if len(runs) < 2:
        return []

    for r in runs:
        r["base"] = f"/files/{r['suite']}/{r['benchmark']}/{r['run_id']}"
        r["files"] = {item["filename"]: item for item in collect_run_files(r["rundir"])}
        run_manifest = read_run_manifest(os.path.join(r["rundir"], RUN_MANIFEST_NAME))
        if run_manifest is not None:
            workload = run_manifest.get("command") or None
        else:
            workload = read_manifest_workload(os.path.join(r["rundir"], MANIFEST_NAME))
        r["workload_str"] = shlex.join(workload) if workload else None
    return runs


def compare_run_keys(runs):
    """The canonical "<suite>/<benchmark>/<run_id>" key list for an already-
    resolved run set, reconstructed from the parsed/validated segments
    rather than the raw query string -- so compare_id_for_keys() is stable
    against whitespace/encoding differences in how the URL happened to be
    typed, not just key order (already handled by compare_id_for_keys()'s
    own sort)."""
    return [f"{r['suite']}/{r['benchmark']}/{r['run_id']}" for r in runs]


def compare_filenames(runs):
    """Union of filenames across all runs' collect_run_files(), first-seen
    order -- the row identity both the raw table and the curation layer
    key off (Phase 1 scope: filename rows only, no cross-run alignment --
    see this item's own INVESTIGATION.md entry for why that's deferred)."""
    filenames = []
    seen = set()
    for r in runs:
        for item in collect_run_files(r["rundir"]):
            if item["filename"] not in seen:
                seen.add(item["filename"])
                filenames.append(item["filename"])
    return filenames


def compare_query_string(run_keys):
    """Renders run_keys back into repeated r=... query params for a link/
    form action -- the same identity render_compare()/resolve_compare_runs()
    parse back out of a request's own qs.get("r", [])."""
    return "&".join(f"r={_urlescape(k)}" for k in run_keys)


def render_compare(output_root, keys):
    """Item 8's compare view: sweep 2+ runs side by side, filename-aligned
    (not block-aligned -- see this item's own INVESTIGATION.md entry for why
    cross-run alignment is deferred past Phase 1). Always shows the raw
    per-file table regardless of curation state; additionally shows an
    overview note and any per-row notes from compare.json when present,
    mirroring render_report()'s own "curated content leads, raw listing
    always available" precedent, just without a <details> collapse here
    since Phase 1 annotates the *same* table rather than producing a
    second, differently-shaped view of it."""
    runs = resolve_compare_runs(output_root, keys)
    if not runs:
        body = ('<section class="panel"><h1>Compare runs</h1>'
                '<p class="muted">Select at least two runs from the homepage report list to '
                'compare them side by side.</p><p><a href="/">Back to launcher</a></p></section>')
        return page("wspy compare", body)

    run_keys = compare_run_keys(runs)
    compare_id = compare_id_for_keys(run_keys)
    curation = load_compare_curation(output_root, compare_id) or {}
    overview_note = curation.get("overview_note", "")
    row_notes = curation.get("row_notes", {}) or {}
    curate_url = f"/compare/curate?{compare_query_string(run_keys)}"

    filenames = compare_filenames(runs)

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
        note = row_notes.get(filename)
        note_html = f'<div class="row-note">{html.escape(note)}</div>' if note else ""
        rows.append(f'<tr><th class="row-label">{html.escape(filename)}{note_html}</th>{"".join(cells)}</tr>')

    overview_html = (f'<p class="compare-overview">{html.escape(overview_note)}</p>' if overview_note else "")

    body = f"""
<section class="panel">
  <h1>Compare {len(runs)} runs</h1>
  <p><a href="/">Back to launcher</a> &middot; <a href="{curate_url}">
     {"Edit this comparison" if (overview_note or row_notes) else "Annotate this comparison"}</a></p>
  {overview_html}
  <div class="compare-scroll">
    <table class="compare">
      <thead><tr><th></th>{header_cells}</tr></thead>
      <tbody>{"".join(rows)}</tbody>
    </table>
  </div>
</section>
"""
    return page("wspy compare", body)


def render_compare_curate_form(output_root, keys):
    """Edit page for a comparison's overview note + per-filename-row notes
    (Phase 1 scope -- see render_compare()'s own docstring). A separate
    page from /compare itself, mirroring the studio/report split rather
    than an inline-edit toggle, since that's this codebase's established
    view/edit pattern (render_studio() vs. render_report())."""
    runs = resolve_compare_runs(output_root, keys)
    if not runs:
        body = ('<section class="panel"><h1>Annotate comparison</h1>'
                '<p class="muted">Select at least two runs from the homepage report list to '
                'compare them side by side.</p><p><a href="/">Back to launcher</a></p></section>')
        return page("wspy compare", body)

    run_keys = compare_run_keys(runs)
    compare_id = compare_id_for_keys(run_keys)
    curation = load_compare_curation(output_root, compare_id) or {}
    overview_note = curation.get("overview_note", "")
    row_notes = curation.get("row_notes", {}) or {}
    action = f"/compare/curate?{compare_query_string(run_keys)}"
    back_url = f"/compare?{compare_query_string(run_keys)}"

    filenames = compare_filenames(runs)
    rows = "".join(f"""
<div class="block-card">
  <label>{html.escape(filename)}
    <textarea name="row_note__{html.escape(filename)}" rows="2">{html.escape(row_notes.get(filename, ""))}</textarea>
  </label>
</div>""" for filename in filenames)

    body = f"""
<section class="panel">
  <h1>Annotate comparison: {len(runs)} runs</h1>
  <p class="config-label">One note for the comparison as a whole, plus an optional note per
     artifact row -- filename-aligned, same rows as the comparison table itself. Clearing a note's
     text and saving removes it.</p>
  <p><a href="{back_url}">Back to comparison</a></p>
  <form method="post" action="{action}">
    <div class="block-card overview-card">
      <label>Comparison overview
        <textarea name="overview_note" rows="3">{html.escape(overview_note)}</textarea>
      </label>
    </div>
    <div class="block-list">{rows or '<p class="muted">No artifacts in common to annotate.</p>'}</div>
    <button type="submit" class="primary">Save</button>
  </form>
</section>
"""
    return page("annotate comparison", body)


def apply_compare_curate_post(output_root, keys, form):
    """Saves the edit form above. Returns the run_keys the comparison
    resolved to (for the caller's redirect back to /compare), or None if
    the key set no longer resolves to >=2 real run directories (e.g. a run
    was deleted between loading the form and submitting it)."""
    runs = resolve_compare_runs(output_root, keys)
    if not runs:
        return None
    run_keys = compare_run_keys(runs)
    compare_id = compare_id_for_keys(run_keys)

    overview_note = form.get("overview_note", [""])[0]
    row_notes = {}
    prefix = "row_note__"
    for field_name, values in form.items():
        if not field_name.startswith(prefix) or not values:
            continue
        note = values[0].strip()
        if note:
            row_notes[field_name[len(prefix):]] = note

    save_compare_curation(output_root, compare_id,
                           {"run_keys": run_keys, "overview_note": overview_note, "row_notes": row_notes})
    return run_keys


def render_tree_viewer(suite, benchmark, run_id):
    """Item 3's interactive tree viewer page: a thin HTML shell -- the
    actual rendering (collapsible tree, search/filter, column toggles)
    happens client-side in proctree_viewer.js, which fetches this run's
    tree via /api/tree-json/<suite>/<benchmark>/<run_id> on load. Kept as
    its own static file rather than folded into the shared app.js so every
    other page doesn't pay for this page-specific JS (see CLAUDE.md's web/
    entry)."""
    json_url = f"/api/tree-json/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    report_url = f"/report/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    body = f"""
<section class="panel">
  <h1>Process tree: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>
  <p><a href="{report_url}">Back to report</a></p>
  <div id="ptv-controls"></div>
  <div id="ptv-root"><p class="muted">Loading tree...</p></div>
</section>
<script>window.PTV_CONFIG = {json.dumps({"mode": "single", "jsonUrl": json_url})};</script>
<script src="/static/proctree_viewer.js"></script>
"""
    return page(f"process tree: {benchmark}/{run_id}", body)


def render_tree_diff(cfg, keys):
    """Item 3's run-to-run tree diff view: mirrors render_compare()'s own
    "select at least two runs" fallback shape, but needs exactly two runs
    (a structural tree diff, unlike the N-way artifact-list compare above)
    and both must have a process.tree.txt to diff at all."""
    output_root = cfg["output_root"]
    runs = []
    seen = set()
    for key in keys:
        segs = parse_run_key(key)
        if segs is None or key in seen:
            continue
        seen.add(key)
        suite, benchmark, run_id = segs
        rundir = os.path.join(output_root, suite, benchmark, run_id)
        if not os.path.isfile(os.path.join(rundir, TREE_TXT_NAME)):
            continue
        runs.append({"suite": suite, "benchmark": benchmark, "run_id": run_id, "key": key})

    if len(runs) != 2:
        body = ('<section class="panel"><h1>Tree diff</h1>'
                '<p class="muted">Select exactly two runs with a process tree '
                '(<code>process.tree.txt</code>, i.e. run with the process-tree '
                'configuration/preset enabled) from the homepage report list to diff '
                'them.</p><p><a href="/">Back to launcher</a></p></section>')
        return page("wspy tree diff", body)

    json_url = (f"/api/tree-diff-json?a={_urlescape(runs[0]['key'])}&b={_urlescape(runs[1]['key'])}")
    labels = [f"{r['suite']}/{r['benchmark']}/{r['run_id']}" for r in runs]
    body = f"""
<section class="panel">
  <h1>Tree diff</h1>
  <p>A: <code>{html.escape(labels[0])}</code> vs B: <code>{html.escape(labels[1])}</code></p>
  <p><a href="/">Back to launcher</a></p>
  <div id="ptv-controls"></div>
  <div id="ptv-root"><p class="muted">Loading diff...</p></div>
</section>
<script>window.PTV_CONFIG = {json.dumps({"mode": "diff", "jsonUrl": json_url})};</script>
<script src="/static/proctree_viewer.js"></script>
"""
    return page("wspy tree diff", body)


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
            # profile_workload/profile_suite/profile_benchmark are legacy
            # aliases from when the Run/wspy-run-profile forms were separate
            # (item 7); "customize & run again" on an older report may still
            # link with them, so they're accepted as synonyms rather than
            # broken by item 9's single-form merge.
            prefill = {}
            for key, aliases in (("workload", ("workload", "profile_workload")),
                                  ("suite", ("suite", "profile_suite")),
                                  ("benchmark", ("benchmark", "profile_benchmark")),
                                  ("phoronix_test_point", ("phoronix_test_point",))):
                for alias in aliases:
                    if alias in qs:
                        prefill[key] = qs[alias][0]
                        break
            # Item 17: a report's "Customize & run again" link carries the
            # preset/configuration/option state build_rerun_url() resolved
            # from that report's own configuration_provenance, as a single
            # JSON 'config' param (a checklist is nested, so the flat
            # per-field scheme above can't express it). Untrusted input
            # (a hand-edited URL) -- validated defensively before it
            # reaches render_run_tab(), same as any other query param here.
            if "config" in qs:
                try:
                    state = json.loads(qs["config"][0])
                except (ValueError, TypeError):
                    state = None
                if isinstance(state, dict):
                    preset = state.get("preset")
                    if isinstance(preset, str) and preset in BUILTIN_PROFILES:
                        prefill["preset"] = preset
                    checklist = state.get("checklist")
                    if isinstance(checklist, dict):
                        prefill["checklist"] = {
                            k: v for k, v in checklist.items()
                            if k in ("tree", "counters", "system", "gpu", "ibs", "power")
                            and isinstance(v, dict)
                        }
            # A report page's "Compare cores" link on a --per-core CSV
            # artifact -- lands on the Validate tab's per-core class
            # comparison section instead of the Run tab, with that CSV's
            # path prefilled so nothing has to be typed by hand.
            if "core_report_csv" in qs:
                prefill["active_tab"] = "validate"
                prefill["core_report_path"] = qs["core_report_csv"][0]
            self._send(200, render_index(cfg, prefill))
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

        m = re.match(r"^/tree-viewer/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isfile(os.path.join(rundir, TREE_TXT_NAME)):
                self._send(404, f"no {TREE_TXT_NAME} in this run directory")
                return
            self._send(200, render_tree_viewer(suite, benchmark, run_id))
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

        m = re.match(r"^/export/([^/]+)/([^/]+)/([^/]+)/download$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isdir(rundir):
                self._send(404, "no such report")
                return
            fmt = qs.get("format", ["wordpress"])[0]
            if fmt not in EXPORT_FORMATS:
                self._send(400, "invalid format")
                return
            overview_note, blocks = _export_data(rundir)
            if not blocks and not overview_note:
                self._send(400, "no curated blocks to export")
                return
            base = f"/files/{suite}/{benchmark}/{run_id}"
            title = f"{suite} / {benchmark} / {run_id}"
            rendered = render_export(rundir, base, title, fmt, overview_note, blocks)
            filename = f"{benchmark}-{run_id}-{fmt}.{EXPORT_FORMAT_EXTENSIONS[fmt]}"
            self._send(200, rendered, content_type=EXPORT_FORMAT_CONTENT_TYPES[fmt],
                       headers={"Content-Disposition": f'attachment; filename="{filename}"'})
            return

        m = re.match(r"^/bundle/([^/]+)/([^/]+)/([^/]+)/download$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isdir(rundir):
                self._send(404, "no such report")
                return
            tar_bytes, _index = build_reproducibility_bundle(rundir, suite, benchmark, run_id)
            filename = f"{suite}-{benchmark}-{run_id}.reproducibility.tar.gz"
            self._send(200, tar_bytes, content_type="application/gzip",
                       headers={"Content-Disposition": f'attachment; filename="{filename}"'})
            return

        m = re.match(r"^/export/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
            if not os.path.isdir(rundir):
                self._send(404, "no such report")
                return
            fmt = qs.get("format", ["wordpress"])[0]
            if fmt not in EXPORT_FORMATS:
                fmt = "wordpress"
            base = f"/files/{suite}/{benchmark}/{run_id}"
            self._send(200, render_export_page(rundir, base, suite, benchmark, run_id, fmt))
            return

        if path == "/compare":
            keys = qs.get("r", [])
            self._send(200, render_compare(cfg["output_root"], keys))
            return

        if path == "/compare/curate":
            keys = qs.get("r", [])
            self._send(200, render_compare_curate_form(cfg["output_root"], keys))
            return

        if path == "/history":
            self._send(200, render_history(cfg, qs))
            return

        if path == "/tree-diff":
            keys = qs.get("r", [])
            self._send(200, render_tree_diff(cfg, keys))
            return

        m = re.match(r"^/files/([^/]+)/([^/]+)/([^/]+)/(.+)$", path)
        if m:
            suite, benchmark, run_id, filename = m.groups()
            self._serve_artifact(cfg["output_root"], suite, benchmark, run_id, filename)
            return

        m = re.match(r"^/api/tree-json/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            self._api_tree_json(cfg, *m.groups())
            return

        if path == "/api/tree-diff-json":
            self._api_tree_diff_json(cfg, qs)
            return

        m = re.match(r"^/api/run/([^/]+)/([^/]+)/([^/]+)/events$", path)
        if m:
            self._stream_events(*m.groups())
            return

        m = re.match(r"^/api/analyze/([^/]+)/([^/]+)/([^/]+)/events$", path)
        if m:
            self._stream_events(*m.groups(), registry=ANALYZE_RUNS, lock=ANALYZE_RUNS_LOCK)
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

        if parsed.path == "/compare/curate":
            keys = parse_qs(parsed.query).get("r", [])
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode("utf-8", errors="replace")
            form = parse_qs(raw, keep_blank_values=True)
            run_keys = apply_compare_curate_post(cfg["output_root"], keys, form)
            if run_keys is None:
                self._send(400, "comparison no longer resolves to at least two runs")
                return
            self.send_response(303)
            self.send_header("Location", f"/compare?{compare_query_string(run_keys)}")
            self.end_headers()
            return

        m = re.match(r"^/api/analyze/([^/]+)/([^/]+)/([^/]+)$", parsed.path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid path")
                return
            length = int(self.headers.get("Content-Length", "0"))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                self._send_json(400, {"error": "invalid JSON body"})
                return
            self._start_analyze(cfg, suite, benchmark, run_id, body)
            return

        POST_HANDLERS = {
            "/api/run": self._start_run,
            "/api/run-profile": self._start_profile_run,
            "/api/run-custom": self._start_custom_run,
            "/api/preview": self._preview,
            "/api/enqueue-job": self._enqueue_job,
            "/api/check-run": self._check_run,
            "/api/discovery/capabilities": self._discovery_capabilities,
            "/api/discovery/preflight": self._discovery_preflight,
            "/api/discovery/affinity-topology": self._discovery_affinity_topology,
            "/api/discovery/validate": self._discovery_validate,
            "/api/discovery/core-report": self._discovery_core_report,
            "/api/discovery/store-ingest": self._discovery_store_ingest,
            "/api/discovery/summary": self._discovery_summary,
            "/api/discovery/trace": self._discovery_trace,
            "/api/discovery/ollama-models": self._discovery_ollama_models,
            "/api/phoronix/materialize": self._phoronix_materialize,
            "/api/phoronix/use-in-run": self._phoronix_use_in_run,
            "/api/phoronix/repin": self._phoronix_repin,
        }
        handler = POST_HANDLERS.get(parsed.path)
        if handler is None:
            self._send(404, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._send_json(400, {"error": "invalid JSON body"})
            return
        handler(cfg, body)

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
        # ibs.csv, ...), so any path that both passes valid_relpath() (no
        # "..", no leading/trailing/doubled "/", every component filesystem-
        # safe) and actually exists inside this specific, already-validated
        # run directory is safe to serve. filename may be one level nested
        # (e.g. "plots/amdtopdown.topdown.png", item 12's plot PNGs).
        if not all(valid_segment(x) for x in (suite, benchmark, run_id)) or not valid_relpath(filename):
            self._send(400, "invalid path")
            return
        path = os.path.join(output_root, suite, benchmark, run_id, *filename.split("/"))
        if not os.path.isfile(path):
            self._send(404, "not found")
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), content_type=guess_content_type(filename))

    def _api_tree_json(self, cfg, suite, benchmark, run_id):
        """Item 3's on-demand JSON endpoint: runs proctree --json against
        this run's process.tree.txt and returns the parsed result. No
        artifact is written to disk (per the confirmed design decision) --
        this always reflects the current process.tree.txt, computed fresh
        on each request, same synchronous-subprocess shape as the
        _discovery_* endpoints (run_sync -> _send_json), just reached via
        GET with path segments (like /files, /report) rather than POST with
        a JSON body, since there's nothing to post."""
        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            self._send_json(400, {"error": "invalid path"})
            return
        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        tree_txt = os.path.join(rundir, TREE_TXT_NAME)
        if not (os.path.isfile(tree_txt) and os.path.getsize(tree_txt) > 0):
            self._send_json(404, {"error": f"no {TREE_TXT_NAME} in this run directory"})
            return
        argv = build_proctree_json_argv(cfg["proctree_bin"], tree_txt)
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        if timed_out:
            self._send_json(504, {"error": "proctree --json timed out", "command": shell_preview(argv)})
            return
        if rc != 0:
            self._send_json(500, {"error": f"proctree exited {rc}", "command": shell_preview(argv),
                                   "output": output})
            return
        try:
            data = json.loads(output)
        except ValueError as e:
            self._send_json(500, {"error": f"proctree --json produced invalid JSON: {e}",
                                   "command": shell_preview(argv)})
            return
        self._send_json(200, {"command": shell_preview(argv), "data": data})

    def _api_tree_diff_json(self, cfg, qs):
        """Item 3's tree-diff counterpart to _api_tree_json above: resolves
        both runs' process.tree.txt, runs proctree --json for each into a
        temp file (proctree --diff itself only accepts already-exported
        JSON, per proctree.c's own design -- see CLAUDE.md), then runs
        proctree --diff --json against the two temp files and returns its
        output the same way."""
        a_key = qs.get("a", [None])[0]
        b_key = qs.get("b", [None])[0]
        a_segs = parse_run_key(a_key) if a_key else None
        b_segs = parse_run_key(b_key) if b_key else None
        if a_segs is None or b_segs is None:
            self._send_json(400, {"error": "both a and b must be <suite>/<benchmark>/<run_id>"})
            return

        output_root = cfg["output_root"]
        tree_txt_a = os.path.join(output_root, *a_segs, TREE_TXT_NAME)
        tree_txt_b = os.path.join(output_root, *b_segs, TREE_TXT_NAME)
        for label, path in (("a", tree_txt_a), ("b", tree_txt_b)):
            if not (os.path.isfile(path) and os.path.getsize(path) > 0):
                self._send_json(404, {"error": f"no {TREE_TXT_NAME} in run {label}"})
                return

        tmp_a = tmp_b = None
        try:
            commands = []
            jsons = {}
            for label, tree_txt in (("a", tree_txt_a), ("b", tree_txt_b)):
                argv = build_proctree_json_argv(cfg["proctree_bin"], tree_txt)
                commands.append(shell_preview(argv))
                rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
                if timed_out or rc != 0:
                    self._send_json(500, {"error": f"proctree --json (run {label}) failed",
                                           "command": shell_preview(argv), "output": output})
                    return
                jsons[label] = output

            tmp_a = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
            tmp_a.write(jsons["a"])
            tmp_a.close()
            tmp_b = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
            tmp_b.write(jsons["b"])
            tmp_b.close()

            diff_argv = build_proctree_diff_argv(cfg["proctree_bin"], tmp_a.name, tmp_b.name)
            commands.append(shell_preview(diff_argv))
            rc, output, timed_out = run_sync(diff_argv, cwd=REPO_ROOT, timeout=60)
            if timed_out:
                self._send_json(504, {"error": "proctree --diff timed out", "command": commands})
                return
            # proctree --diff exits 1 (not an error) when the two trees differ --
            # only a missing/crashed binary (no stdout at all) is a real failure.
            try:
                data = json.loads(output)
            except ValueError as e:
                self._send_json(500, {"error": f"proctree --diff produced invalid JSON: {e}",
                                       "command": commands, "output": output})
                return
            self._send_json(200, {"command": commands, "data": data})
        finally:
            for tmp in (tmp_a, tmp_b):
                if tmp is not None:
                    try:
                        os.unlink(tmp.name)
                    except OSError:
                        pass

    @staticmethod
    def _parse_workload_and_ids(body):
        """Shared workload/suite/benchmark/run_id parsing + validation for
        every run-starting endpoint. Returns (workload_argv, suite, benchmark,
        run_id, error_dict) -- error_dict is None on success."""
        workload_str = (body.get("workload") or "").strip()
        if not workload_str:
            return None, None, None, None, {"error": "workload command is required"}
        try:
            workload_argv = shlex.split(workload_str)
        except ValueError as e:
            return None, None, None, None, {"error": f"could not parse workload command: {e}"}
        if not workload_argv:
            return None, None, None, None, {"error": "workload command is required"}

        suite = (body.get("suite") or "manual").strip()
        benchmark = (body.get("benchmark") or "").strip() or default_benchmark_from_workload(workload_argv)
        run_id = (body.get("run_id") or "").strip() or make_run_id()
        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            return None, None, None, None, {"error": "suite/benchmark/run_id must be non-empty and "
                                                       "contain only letters, digits, '.', '_', '-'"}
        return workload_argv, suite, benchmark, run_id, None

    @staticmethod
    def _parse_toggles(cfg, body):
        """The Run tab's defaults-on toggle chips (manifest/run-index/store-
        ingest -- a mockup-feedback item, see INVESTIGATION.md's "What
        shipped in 4.1"). Returns (manifest_on, run_index_path_or_None, store_ingest).
        Thin wrapper over joblib.resolve_toggles() (shared with wspy-queue)."""
        return resolve_toggles(cfg, body.get("toggles"))

    @staticmethod
    def _parse_custom_plots(body):
        """The Run tab's "Custom plots" section (item 12's wspy-plot --plot/
        --only-custom exposed in the UI): validates body["custom_plots"] (a
        list of {"name","columns"}) and body["only_custom"], mirroring
        wspy-plot's own --plot NAME=col1,col2,... validation so a malformed
        spec is rejected here (400) rather than only surfacing as wspy-plot's
        own exit-2 deep inside a background run. Returns (custom_plots,
        only_custom, error_dict) -- error_dict is None on success. A row the
        user hasn't finished filling in (blank name or no columns) is
        silently dropped rather than treated as an error, since the UI's "+
        add custom plot" button starts a row empty."""
        raw = body.get("custom_plots") or []
        if not isinstance(raw, list):
            return None, False, {"error": "custom_plots must be a list"}
        custom_plots = []
        for item in raw:
            if not isinstance(item, dict):
                return None, False, {"error": "each custom_plots entry must be an object"}
            name = str(item.get("name") or "").strip()
            columns = [c.strip() for c in (item.get("columns") or [])
                       if isinstance(c, str) and c.strip()]
            if not name and not columns:
                continue
            if not name or not columns:
                return None, False, {"error": "each custom plot needs both a name and at least one column"}
            if not NAME_RE.match(name):
                return None, False, {"error": f"custom plot name '{name}' must contain only "
                                               "letters, digits, '.', '_', '-'"}
            if any("," in c for c in columns):
                return None, False, {"error": f"custom plot '{name}': column names cannot contain a comma"}
            custom_plots.append({"name": name, "columns": columns})
        only_custom = bool(body.get("only_custom"))
        if only_custom and not custom_plots:
            return None, False, {"error": "\"only render custom plots\" requires at least one "
                                           "custom plot with a name and columns"}
        return custom_plots, only_custom, None

    @staticmethod
    def _parse_affinity(body):
        """The Run tab's CPU affinity card (INVESTIGATION.md's "Core/
        thread affinity control" item, wspy.c's --affinity=<spec>): validates
        body["affinity"] against the same grammar affinity.h's C parser
        accepts, mirroring _parse_custom_plots()'s "reject here, not deep
        inside a background run" idiom. Returns (spec_or_None, error_dict) --
        None (not "all") is what every builder function below treats as "no
        --affinity flag at all", since "all" is the implicit default."""
        spec = (body.get("affinity") or "").strip()
        if not spec or spec == "all":
            return None, None
        if not valid_affinity_spec(spec):
            return None, {"error": "affinity must be all/nosmt/thread=<id>/domain=<id>/"
                                    "cpuset=<c0,c1,...>"}
        return spec, None

    @staticmethod
    def _link_phoronix_test_point(body, rundir, run_id):
        """Shared by _start_run()/_start_profile_run()/_start_custom_run():
        if this run was started via the Phoronix tab's "Use in Run tab"
        (body["phoronix_test_point"] set, either by that button's own JS or
        by a bookmarked prefill URL -- see do_GET("/")), re-validates the
        path server-side (never trust a client-supplied path blindly, even
        one this same server handed out a moment ago) and symlinks the run
        back under that test point directory. Best-effort: a missing/
        invalid/deleted test point just means no symlink, never a failed
        run -- see joblib.link_phoronix_test_point_run()'s own docstring."""
        raw = (body.get("phoronix_test_point") or "").strip()
        if not raw:
            return
        dest = os.path.join(REPO_ROOT, "workload", "phoronix")
        test_point_dir = joblib.resolve_phoronix_test_point_dir(dest, raw)
        if test_point_dir:
            joblib.link_phoronix_test_point_run(test_point_dir, run_id, rundir)

    def _start_run(self, cfg, body):
        workload_argv, suite, benchmark, run_id, err = self._parse_workload_and_ids(body)
        if err:
            self._send_json(400, err)
            return

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return
        os.makedirs(rundir)
        self._link_phoronix_test_point(body, rundir, run_id)

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        wspy_argv = build_wspy_argv(cfg["wspy_bin"], rundir, workload_argv)
        plot_argv = build_plot_argv(cfg["wspy_plot_bin"], rundir)

        t = threading.Thread(target=execute_run, args=(
            state, cfg["wspy_bin"], cfg["wspy_plot_bin"], rundir, workload_argv,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "wspy_command": shell_preview(wspy_argv),
            "plot_command": shell_preview(plot_argv),
        })

    def _start_profile_run(self, cfg, body):
        profile = (body.get("profile") or "").strip()
        if not profile or not valid_profile_spec(profile):
            self._send_json(400, {"error": "profile is required and must be a comma-separated list "
                                            "of letters/digits/'-'/'_' (e.g. deep-cpu or "
                                            "deep-cpu,tree-heavy)"})
            return

        workload_argv, suite, benchmark, run_id, err = self._parse_workload_and_ids(body)
        if err:
            self._send_json(400, err)
            return
        manifest_on, run_index_path, store_ingest = self._parse_toggles(cfg, body)
        custom_plots, only_custom, err = self._parse_custom_plots(body)
        if err:
            self._send_json(400, err)
            return
        affinity, err = self._parse_affinity(body)
        if err:
            self._send_json(400, err)
            return

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return
        os.makedirs(rundir)
        self._link_phoronix_test_point(body, rundir, run_id)

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        wspy_run_argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"],
                                             cfg["output_root"], suite, benchmark,
                                             run_id, profile, workload_argv,
                                             run_index_path=run_index_path,
                                             affinity=affinity)
        supp_passes, preset_notes = build_supplementary_plot_passes(rundir, profile, custom_plots)

        t = threading.Thread(target=execute_profile_run, args=(
            state, cfg, rundir, suite, benchmark, run_id, profile, workload_argv,
            run_index_path, store_ingest, custom_plots, only_custom, preset_notes,
            supp_passes, manifest_on, affinity,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "wspy_run_command": shell_preview(wspy_run_argv),
            "notes": preset_notes,
        })

    def _start_custom_run(self, cfg, body):
        """Item 9's checklist-driven fallback (see build_configuration_passes()/
        execute_custom_run()): runs whatever configurations are enabled in
        body["checklist"] as separate sequential wspy invocations."""
        workload_argv, suite, benchmark, run_id, err = self._parse_workload_and_ids(body)
        if err:
            self._send_json(400, err)
            return
        checklist = body.get("checklist") or {}
        manifest_on, run_index_path, store_ingest = self._parse_toggles(cfg, body)
        custom_plots, only_custom, err = self._parse_custom_plots(body)
        if err:
            self._send_json(400, err)
            return
        affinity, err = self._parse_affinity(body)
        if err:
            self._send_json(400, err)
            return
        checklist, autofit_notes = autofit_checklist_for_custom_plots(checklist, custom_plots)

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return

        passes = build_configuration_passes(rundir, checklist)
        if not passes:
            self._send_json(400, {"error": "no configuration is enabled (or every enabled "
                                            "configuration has nothing selected within it)"})
            return

        os.makedirs(rundir)
        self._link_phoronix_test_point(body, rundir, run_id)
        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        command_lines = []
        for p in passes:
            argv, _outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                               manifest_on, run_index_path,
                                                               affinity=affinity)
            full_argv = argv + ["--"] + workload_argv
            if p["timeout"]:
                full_argv = ["timeout", str(p["timeout"])] + full_argv
            command_lines.append(shell_preview(full_argv))

        t = threading.Thread(target=execute_custom_run, args=(
            state, cfg, rundir, suite, benchmark, run_id, workload_argv,
            checklist, manifest_on, run_index_path, store_ingest,
            custom_plots, only_custom, autofit_notes, affinity,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "commands": command_lines,
            "notes": autofit_notes,
        })

    def _start_analyze(self, cfg, suite, benchmark, run_id, body):
        """Report page's "AI narrative analysis" button (render_analyze_card()):
        runs wspy-analyze against an existing run directory, streamed over SSE
        via ANALYZE_RUNS the same way a launched workload's live log is (see
        execute_analyze()'s own comment for why this doesn't reuse RUNS).
        Neither models nor all_models is required: an empty models list with
        all_models=False is passed straight through as a plain `wspy-analyze
        --rundir <dir>` invocation with no --model/--all-models at all, so
        wspy-analyze's own --default-model fallback (gpt-oss:20b if
        installed) applies -- if that's also not installed, wspy-analyze
        itself errors out and that error surfaces in the streamed log, same
        as any other wspy-analyze failure."""
        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if not os.path.isdir(rundir):
            self._send_json(404, {"error": "no such report"})
            return

        models = [m.strip() for m in (body.get("models") or [])
                  if isinstance(m, str) and m.strip()]
        all_models = bool(body.get("all_models"))
        critique = bool(body.get("critique"))
        template = body.get("template")

        argv = [cfg["wspy_analyze_bin"], "--rundir", rundir]
        for m in models:
            argv += ["--model", m]
        if all_models:
            argv.append("--all-models")
        if critique:
            argv.append("--critique")
        if template:
            if template in ("perf_analysis.tmpl", "perf_analysis2.tmpl"):
                template_path = os.path.join(os.path.dirname(cfg["wspy_analyze_bin"]), "prompts", template)
                argv += ["--prompt-template", template_path]

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with ANALYZE_RUNS_LOCK:
            ANALYZE_RUNS[key] = state

        t = threading.Thread(target=execute_analyze, args=(
            state, argv, suite, benchmark, run_id,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/analyze/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "studio_url": f"/studio/{suite}/{benchmark}/{run_id}",
            "command": shell_preview(argv),
        })

    def _preview(self, cfg, body):
        """Source of truth for the Run tab's command-line preview -- shares
        build_wspy_run_argv()/build_configuration_passes()/build_pass_argv()
        with the real execute_*_run() paths, so what's shown here is never a
        paraphrase of what /api/run-profile or /api/run-custom will actually
        run. Tolerant of blank workload/suite/benchmark/run_id (placeholder
        text stands in), since this fires on every keystroke while the form
        is still being filled in -- it never touches the filesystem."""
        workload_str = (body.get("workload") or "").strip()
        workload_argv = shlex.split(workload_str) if workload_str else ["<workload command>"]
        suite = (body.get("suite") or "").strip() or "manual"
        benchmark = (body.get("benchmark") or "").strip() or "<benchmark>"
        run_id = (body.get("run_id") or "").strip() or "<auto>"
        _manifest_on, run_index_path, _store_ingest = self._parse_toggles(cfg, body)
        custom_plots, only_custom, err = self._parse_custom_plots(body)
        if err:
            self._send_json(400, err)
            return
        affinity, err = self._parse_affinity(body)
        if err:
            self._send_json(400, err)
            return
        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        plot_argv = build_plot_argv(cfg["wspy_plot_bin"], rundir, custom_plots, only_custom)

        preset = (body.get("preset") or "").strip()
        if preset:
            if not valid_profile_spec(preset):
                self._send_json(400, {"error": "invalid preset spec"})
                return
            argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"], cfg["output_root"],
                                        suite, benchmark, run_id, preset, workload_argv,
                                        run_index_path=run_index_path, affinity=affinity)
            supp_passes, preset_notes = build_supplementary_plot_passes(rundir, preset, custom_plots)
            lines = [shell_preview(argv)]
            for p in supp_passes:
                pargv, _outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                                    _manifest_on, run_index_path,
                                                                    affinity=affinity)
                full_pargv = pargv + ["--"] + workload_argv
                if p["timeout"]:
                    full_pargv = ["timeout", str(p["timeout"])] + full_pargv
                lines.append(shell_preview(full_pargv))
            lines.append(shell_preview(plot_argv))
            self._send_json(200, {"mode": "preset", "lines": lines, "notes": preset_notes})
            return

        checklist = body.get("checklist") or {}
        checklist, autofit_notes = autofit_checklist_for_custom_plots(checklist, custom_plots)
        try:
            passes = build_configuration_passes(rundir, checklist)
        except (TypeError, ValueError):
            self._send_json(400, {"error": "invalid checklist"})
            return

        notes = list(autofit_notes)
        lines = []
        if not passes:
            notes.append("No configuration enabled yet -- check one below to build a custom run.")
        for p in passes:
            argv, outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                              _manifest_on, run_index_path,
                                                              affinity=affinity)
            full_argv = argv + ["--"] + workload_argv
            if p["timeout"]:
                full_argv = ["timeout", str(p["timeout"])] + full_argv
            lines.append(shell_preview(full_argv))
            if "--passes=" in " ".join(p["flags"]):
                notes.append(f"'{p['name']}' uses native multi-pass execution (--passes) to bin-pack "
                              f"its groups into as few re-executions of the workload as fit the PMU budget.")
            if "--per-core" in p["flags"] and p.get("category") == "performance-counters" \
                    and (checklist.get("counters") or {}).get("interval_secs"):
                notes.append(f"'{p['name']}' ignores the interval field: per-core mode always runs in "
                              f"aggregate (--per-core --interval together can't produce a wspy CSV "
                              f"'wspy-core-report' can read -- see CLAUDE.md's wspy.c per_core_csv note).")
        if passes:
            lines.append(shell_preview(plot_argv))
            notes.append("wspy-plot will run afterward, best-effort, matching every CSV this run "
                          "produces against the shared plot templates (see CLAUDE.md's plot.c entry) "
                          "and writing whatever fires into plots/.")
        self._send_json(200, {"mode": "custom", "lines": lines, "notes": notes,
                               "resolved_checklist": checklist})

    def _enqueue_job(self, cfg, body):
        """Item 13 (INVESTIGATION.md, "Deployment/hosting design note"),
        use case (a): the Run tab's "queue instead of run now" toggle. Builds
        the exact same portable job document `wspy-queue add` itself creates
        (joblib.build_job()/validate_job()) from the same preset/checklist/
        workload/toggles state _preview()/_start_profile_run()/
        _start_custom_run() already consume, and writes it into
        <jobs-dir>/pending/ instead of launching anything -- this server
        never processes a job itself (see joblib.py's "Job files" section
        for why: `wspy-queue run` is the one place job execution happens,
        headless, so a job queued here works the same whether or not this
        server is still running when it's picked up)."""
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
        run_id = (body.get("run_id") or "").strip() or None
        if not valid_segment(suite) or not valid_segment(benchmark) or (run_id and not valid_segment(run_id)):
            self._send_json(400, {"error": "suite/benchmark/run_id must be non-empty and contain "
                                             "only letters, digits, '.', '_', '-'"})
            return

        custom_plots, only_custom, err = self._parse_custom_plots(body)
        if err:
            self._send_json(400, err)
            return
        affinity, err = self._parse_affinity(body)
        if err:
            self._send_json(400, err)
            return
        toggles = body.get("toggles") or {}
        notes = (body.get("notes") or "").strip()

        preset = (body.get("preset") or "").strip()
        if preset:
            if not valid_profile_spec(preset):
                self._send_json(400, {"error": "invalid preset spec"})
                return
            job = joblib.build_job(workload_argv, suite, benchmark, "preset", profile=preset,
                                    custom_plots=custom_plots, only_custom=only_custom,
                                    toggles=toggles, run_id=run_id, notes=notes, affinity=affinity)
        else:
            checklist = body.get("checklist") or {}
            # Placeholder rundir: build_configuration_passes() only ever uses
            # it to spell a --tree pass's output path into that pass's flags
            # text -- it never touches the filesystem -- so this is safe to
            # call purely to check "is anything enabled" before queuing.
            if not build_configuration_passes(
                    os.path.join(cfg["output_root"], suite, benchmark, "<job>"), checklist):
                self._send_json(400, {"error": "no configuration is enabled (or every enabled "
                                                "configuration has nothing selected within it)"})
                return
            job = joblib.build_job(workload_argv, suite, benchmark, "custom", checklist=checklist,
                                    custom_plots=custom_plots, only_custom=only_custom,
                                    toggles=toggles, run_id=run_id, notes=notes, affinity=affinity)

        errors = joblib.validate_job(job)
        if errors:
            self._send_json(400, {"error": "; ".join(errors)})
            return

        pending_dir = os.path.join(cfg["jobs_dir"], "pending")
        os.makedirs(pending_dir, exist_ok=True)
        path = os.path.join(pending_dir, f'{job["job_id"]}.json')
        with open(path, "w") as f:
            json.dump(job, f, indent=2)
            f.write("\n")

        self._send_json(202, {
            "job_id": job["job_id"], "path": path,
            "message": "queued -- run `wspy-queue run` (on this machine, via cron, or after "
                       "copying the job file to another machine) to actually launch it",
        })

    # -----------------------------------------------------------------
    # Item 18: Run tab "Check" button. Synchronous like the Discovery tab
    # family below (no workload launched, nothing to stream live) but kept
    # in the Run tab family since that's the button it sits next to.
    # -----------------------------------------------------------------

    def _check_run(self, cfg, body):
        workload = (body.get("workload") or "").strip()
        result = {"perf": check_perf_access(), "tools": check_tooling()}

        ibs_probes = ibs_probes_for_request(cfg, body.get("preset"), body.get("checklist"))
        result["ibs"] = [probe_ibs(cfg["wspy_bin"], label, flags) for label, flags in ibs_probes]

        power_probes = power_probes_for_request(cfg, body.get("preset"), body.get("checklist"))
        result["power"] = [probe_power(cfg["wspy_bin"], label, flags) for label, flags in power_probes]

        gpu_flags = gpu_flags_for_request(body.get("preset"), body.get("checklist"))
        result["gpu_build"] = check_gpu_build(cfg["wspy_bin"], gpu_flags)

        test_names = parse_phoronix_test_names(workload)
        if not test_names:
            result["phoronix"] = {
                "detected": False,
                "note": "not a phoronix-test-suite command -- no runtime estimator for this "
                        "workload yet (INVESTIGATION.md's estimated runtime display)",
            }
            self._send_json(200, result)
            return

        # batch-run is the only one of PHORONIX_RUN_SUBCOMMANDS that reads
        # user-config.xml's <BatchMode> block for unattended operation --
        # `run`/`benchmark` already prompt interactively by design, so
        # there's nothing check_phoronix_batch_config() would add for those.
        is_batch_run = shlex.split(workload)[1] == "batch-run"
        estimate = estimate_phoronix_workload_seconds(workload, phoronix_bin=cfg["phoronix_bin"],
                                                       cwd=REPO_ROOT)
        result["phoronix"] = {
            "detected": True,
            **estimate,
            "batch_mode": check_phoronix_batch_config() if is_batch_run else None,
            "result_notifier": (check_phoronix_result_notifier_bug(cfg["phoronix_pts_dir"])
                                 if phoronix_result_notifier_hooks_registered() else None),
        }
        self._send_json(200, result)

    # -----------------------------------------------------------------
    # Discovery tab family (item 9): wspy-validate, wspy-store/wspy-summary,
    # wspy --capabilities/--preflight. All synchronous (run_sync()) -- none
    # of these launch a workload, so there's no unbounded runtime to stream
    # live and no run directory/report page involved; the result is just
    # rendered straight into the calling tab.
    # -----------------------------------------------------------------

    def _discovery_capabilities(self, cfg, body):
        argv = [cfg["wspy_bin"], "--capabilities"]
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out})

    def _discovery_affinity_topology(self, cfg, body):
        """Backs the Run tab's affinity card 'Discover CPUs' button: runs
        `wspy --list-affinity` (no privileges needed, pure sysfs reads --
        same standing as --preflight) and returns both the raw text (for the
        same command/output display every other Discovery tab action shows)
        and the parsed topology the card's JS uses to render domain buttons
        and per-CPU checkboxes."""
        argv = [cfg["wspy_bin"], "--list-affinity"]
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=30)
        topology = parse_affinity_topology_output(output) if rc == 0 else None
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out, "topology": topology})

    def _discovery_preflight(self, cfg, body):
        group_flags, selected = counter_group_flags(body.get("groups"))
        if not selected:
            self._send_json(400, {"error": "select at least one counter group to preflight-check"})
            return
        argv = [cfg["wspy_bin"], "--preflight"] + group_flags
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=30)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out})

    def _discovery_validate(self, cfg, body):
        paths = [p.strip() for p in (body.get("paths") or []) if isinstance(p, str) and p.strip()]
        if not paths:
            self._send_json(400, {"error": "at least one manifest path is required"})
            return
        missing = [p for p in paths if not os.path.isfile(p)]
        if missing:
            self._send_json(400, {"error": "manifest file(s) not found: " + ", ".join(missing)})
            return
        argv = [cfg["wspy_validate_bin"]]
        if body.get("strict"):
            argv.append("--strict")
        if body.get("quiet"):
            argv.append("--quiet")
        argv += paths
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out})

    def _discovery_core_report(self, cfg, body):
        path = (body.get("path") or "").strip()
        if not path:
            self._send_json(400, {"error": "a --per-core CSV path is required"})
            return
        if not os.path.isfile(path):
            self._send_json(400, {"error": f"CSV file not found: {path}"})
            return
        argv = [cfg["wspy_core_report_bin"]]
        if body.get("csv"):
            argv.append("--csv")
        for m in (body.get("metrics") or []):
            if isinstance(m, str) and m.strip():
                argv += ["--metric", m.strip()]
        argv.append(path)
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out})

    def _discovery_store_ingest(self, cfg, body):
        db = (body.get("db") or "").strip() or cfg["store_db"]
        run_index_paths = [p.strip() for p in (body.get("run_index") or []) if isinstance(p, str) and p.strip()]
        if not run_index_paths:
            run_index_paths = [cfg["run_index_file"]]
        argv = [cfg["wspy_store_bin"], "--db", db]
        for p in run_index_paths:
            argv += ["--run-index", p]
        if body.get("no_manifest_enrich"):
            argv.append("--no-manifest-enrich")
        if body.get("no_metrics_ingest"):
            argv.append("--no-metrics-ingest")
        if body.get("strict"):
            argv.append("--strict")
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=180)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out})

    def _phoronix_materialize(self, cfg, body):
        """Backs the Phoronix tab's "Materialize" button: resolves whichever
        of result/file/installed_suite the tab's source radio selected, then
        calls joblib.import_phoronix_test_points() -- the exact same
        function wspy-phoronix-import's CLI calls -- in-process (there's no
        external-binary dependency the way wspy --capabilities has, so this
        matches execute_profile_run()'s "call the shared function directly"
        pattern rather than run_sync()-around-a-real-binary). Source
        resolution mirrors wspy-phoronix-import's own resolve_source()."""
        source_kind_req = (body.get("source") or "").strip()
        dest = (body.get("dest") or "").strip() or os.path.join(REPO_ROOT, "workload/phoronix")
        ledger_list = (body.get("ledger_list") or "").strip() or None
        dry_run = bool(body.get("dry_run"))
        add_to_ledger = not body.get("no_ledger")
        check_installed = not body.get("no_check_installed")

        if source_kind_req == "result":
            result_ref = (body.get("result") or "").strip()
            if not result_ref:
                self._send_json(400, {"error": "a result URL or ID is required"})
                return
            source = joblib.fetch_openbenchmarking_suite_xml(result_ref, phoronix_bin=cfg["phoronix_bin"],
                                                               cwd=REPO_ROOT)
        elif source_kind_req == "file":
            path = (body.get("file") or "").strip()
            if not path:
                self._send_json(400, {"error": "an XML file path is required"})
                return
            if not os.path.isfile(path):
                self._send_json(400, {"error": f"no such file: {path}"})
                return
            with open(path, "rb") as f:
                source = {"xml": f.read(), "source_kind": "file", "source_ref": os.path.abspath(path)}
        elif source_kind_req == "installed":
            name = (body.get("installed_suite") or "").strip()
            if not name:
                self._send_json(400, {"error": "an installed suite name is required"})
                return
            path, matched_name = joblib.resolve_installed_phoronix_suite(name)
            if not path:
                self._send_json(400, {"error": f"no installed suite matching {name!r}"})
                return
            with open(path, "rb") as f:
                source = {"xml": f.read(), "source_kind": "installed-suite", "source_ref": matched_name}
        else:
            self._send_json(400, {"error": "source must be one of result/file/installed"})
            return

        if "error" in source:
            self._send_json(400, {"error": source["error"]})
            return

        result = joblib.import_phoronix_test_points(
            source["xml"], dest, source["source_kind"], source["source_ref"],
            phoronix_bin=cfg["phoronix_bin"], ledger_bin=cfg["wspy_ledger_bin"],
            ledger_list_path=ledger_list, cwd=REPO_ROOT, dry_run=dry_run,
            check_installed=check_installed, add_to_ledger=add_to_ledger)
        if result["error"]:
            self._send_json(400, {"error": result["error"]})
            return
        self._send_json(200, {"source_kind": source["source_kind"], "source_ref": source["source_ref"],
                               "dest": dest, "dry_run": dry_run, "points": result["points"],
                               "readmes": result.get("readmes", [])})

    def _phoronix_use_in_run(self, cfg, body):
        """Backs the Phoronix tab inventory's "Use in Run tab" button:
        copies the given test point's suite-definition.xml into
        ~/.phoronix-test-suite/test-suites/local/<identity>/ (so the
        returned workload command is immediately runnable -- see
        joblib.copy_phoronix_test_point_to_local_suite()'s own docstring)
        and returns the workload/suite/benchmark the Run tab should
        prefill, plus phoronix_test_point (echoed back into the Run tab's
        hidden field) so a subsequent /api/run-profile or /api/run-custom
        can symlink its run directory back under this test point via
        joblib.link_phoronix_test_point_run() -- see that function's own
        call sites below."""
        dest = os.path.join(REPO_ROOT, "workload", "phoronix")
        test_point_dir = joblib.resolve_phoronix_test_point_dir(dest, (body.get("dir") or "").strip())
        if not test_point_dir:
            self._send_json(400, {"error": "not a materialized test point directory"})
            return
        # bare_name/options_slug/identity are recomputed from the path
        # components rather than trusted from the request body, matching
        # resolve_phoronix_test_point_dir()'s own "don't trust client-
        # supplied identity strings" posture.
        options_slug = os.path.basename(test_point_dir)
        bare_name = os.path.basename(os.path.dirname(test_point_dir))
        identity = f"{bare_name}-{options_slug}"
        joblib.copy_phoronix_test_point_to_local_suite(test_point_dir, identity)
        self._send_json(200, {
            "workload": f"phoronix-test-suite batch-run local/{identity}",
            "suite": "phoronix", "benchmark": identity,
            "phoronix_test_point": test_point_dir,
        })

    def _phoronix_repin(self, cfg, body):
        """Backs the Phoronix tab inventory's per-row "Re-pin" button (only
        shown when the version-mismatch badge is showing): rewrites the
        given test point's pinned version to whichever installed version
        the user picked, via joblib.repin_phoronix_test_point(). Explicit
        and per-point by design -- see that function's own docstring for
        why this isn't done automatically. Re-validates new_version against
        list_installed_phoronix_test_versions() server-side (not just
        trusting the dropdown the client rendered) so a stale page can't
        pin to a version that isn't actually installed."""
        dest = os.path.join(REPO_ROOT, "workload", "phoronix")
        test_point_dir = joblib.resolve_phoronix_test_point_dir(dest, (body.get("dir") or "").strip())
        if not test_point_dir:
            self._send_json(400, {"error": "not a materialized test point directory"})
            return
        new_version = (body.get("version") or "").strip()
        if not new_version:
            self._send_json(400, {"error": "a version is required"})
            return
        try:
            with open(os.path.join(test_point_dir, "source.json")) as f:
                current_test_id = json.load(f).get("test_id", "")
        except (OSError, ValueError):
            self._send_json(400, {"error": "could not read source.json for this test point"})
            return
        installed_versions = joblib.list_installed_phoronix_test_versions(current_test_id)
        if new_version not in installed_versions:
            self._send_json(400, {"error": f"v{new_version} is not among the versions installed on this "
                                            f"host ({', '.join(installed_versions) or 'none'})"})
            return
        try:
            result = joblib.repin_phoronix_test_point(test_point_dir, new_version)
        except (FileNotFoundError, ValueError) as e:
            self._send_json(400, {"error": str(e)})
            return
        self._send_json(200, result)

    def _discovery_ollama_models(self, cfg, body):
        """Backs the report page's "Discover installed models" button
        (render_analyze_card()): runs `wspy-analyze --list-models`, which
        just queries Ollama's /api/tags and prints one model name per line
        -- bounded like the rest of the Discovery-tab family (unlike an
        actual analysis run, listing models is quick), so this stays
        synchronous via run_sync() rather than the SSE path _start_analyze()
        uses."""
        argv = [cfg["wspy_analyze_bin"], "--list-models"]
        host = (body.get("ollama_host") or "").strip()
        if host:
            argv += ["--ollama-host", host]
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=30)
        models = [line.strip() for line in output.splitlines() if line.strip()] if rc == 0 else []
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out, "models": models})

    def _discovery_summary(self, cfg, body):
        db = (body.get("db") or "").strip() or cfg["store_db"]
        argv = [cfg["wspy_summary_bin"], "--db", db]
        command_filter = (body.get("command") or "").strip()
        if command_filter:
            argv += ["--command", command_filter]
        hostname_filter = (body.get("hostname") or "").strip()
        if hostname_filter:
            argv += ["--hostname", hostname_filter]
        for m in (body.get("metrics") or []):
            if isinstance(m, str) and m.strip():
                argv += ["--metric", m.strip()]
        group_by = body.get("group_by") or "command"
        if group_by not in ("command", "hostname", "cpu_vendor"):
            self._send_json(400, {"error": "group_by must be command, hostname, or cpu_vendor"})
            return
        argv += ["--group-by", group_by]
        # outlier-stddev is a float CLI option; parse_optional_int() (used
        # for min_runs below) is int-only, so validate/format it separately.
        outlier_raw = body.get("outlier_stddev")
        if outlier_raw not in (None, ""):
            try:
                argv += ["--outlier-stddev", str(float(outlier_raw))]
            except (TypeError, ValueError):
                self._send_json(400, {"error": "invalid outlier_stddev"})
                return
        min_runs = parse_optional_int(body.get("min_runs"), 1, 100000)
        if body.get("min_runs") not in (None, "") and min_runs is None:
            self._send_json(400, {"error": "invalid min_runs"})
            return
        if min_runs is not None:
            argv += ["--min-runs", str(min_runs)]
        csv = bool(body.get("csv"))
        if csv:
            argv.append("--csv")
        if body.get("show_runs"):
            argv.append("--show-runs")
        if body.get("strict"):
            argv.append("--strict")
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out, "csv": csv})

    # Item 14 "Traceability links (summary row -> manifest -> raw CSV ->
    # plots -> tree artifacts)": resolves one hostname:run_id (as printed by
    # the "show contributing runs" checkbox above) via `wspy-summary --trace`
    # to its artifact paths, then -- best-effort, only when those paths
    # happen to live under this server's own --output-root, the common case
    # for runs this same server launched -- derives a suite/benchmark/run_id
    # triple so the response can offer real links into /report and /files
    # instead of just bare filesystem paths a browser can't open.
    def _discovery_trace(self, cfg, body):
        db = (body.get("db") or "").strip() or cfg["store_db"]
        key = (body.get("key") or "").strip()
        if ":" not in key or key.startswith(":") or key.endswith(":"):
            self._send_json(400, {"error": "expected hostname:run_id"})
            return
        argv = [cfg["wspy_summary_bin"], "--db", db, "--trace", key]
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=30)
        fields = {}
        for line in output.splitlines():
            k, sep, v = line.partition("=")
            if sep:
                fields[k] = v
        result = {"command": shell_preview(argv), "exit_code": rc, "output": output,
                   "timed_out": timed_out, "found": rc == 0, "fields": fields}
        if rc == 0:
            result["links"] = _resolve_trace_links(cfg["output_root"], fields)
        self._send_json(200, result)

    def _stream_events(self, suite, benchmark, run_id, registry=RUNS, lock=RUNS_LOCK):
        key = run_key(suite, benchmark, run_id)
        with lock:
            state = registry.get(key)
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
    ap.add_argument("--wspy-plot", default=os.path.join(REPO_ROOT, "wspy-plot"),
                     help="path to the wspy-plot binary (item 12's shared plotting templates; "
                          "default: repo root's ./wspy-plot)")
    ap.add_argument("--proctree", default=os.path.join(REPO_ROOT, "proctree"),
                     help="path to the proctree binary, run best-effort after a --tree pass "
                          "the same way wspy-plot runs after a CSV-producing pass "
                          "(default: repo root's ./proctree)")
    ap.add_argument("--output-root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs"),
                     help="directory root for <suite>/<benchmark>/<run-id>/ run output "
                          "(default: web/runs)")
    ap.add_argument("--wspy-validate", default=os.path.join(REPO_ROOT, "wspy-validate"),
                     help="path to the wspy-validate binary (default: repo root's ./wspy-validate)")
    ap.add_argument("--wspy-store", default=os.path.join(REPO_ROOT, "wspy-store"),
                     help="path to the wspy-store binary (default: repo root's ./wspy-store)")
    ap.add_argument("--wspy-summary", default=os.path.join(REPO_ROOT, "wspy-summary"),
                     help="path to the wspy-summary binary (default: repo root's ./wspy-summary)")
    ap.add_argument("--wspy-core-report", default=os.path.join(REPO_ROOT, "wspy-core-report"),
                     help="path to the wspy-core-report binary (Validate tab's per-core class "
                          "comparison section, e.g. Zen5 vs. Zen5c; default: repo root's "
                          "./wspy-core-report)")
    ap.add_argument("--wspy-analyze", default=os.path.join(REPO_ROOT, "wspy-analyze"),
                     help="path to the wspy-analyze script (report page's 'AI narrative "
                          "analysis' button; requires a locally running Ollama daemon; "
                          "default: repo root's ./wspy-analyze)")
    ap.add_argument("--wspy-ledger", default=os.path.join(REPO_ROOT, "wspy-ledger"),
                     help="path to the wspy-ledger binary (Phoronix tab's 'materialize' action "
                          "registers each new test point with this via --add; default: repo "
                          "root's ./wspy-ledger)")
    ap.add_argument("--run-index-file",
                     help="shared --run-index file every launched run appends to when the "
                          "'record run index' toggle chip is on (default: <output-root>/run_index.jsonl)")
    ap.add_argument("--store-db",
                     help="normalized-store database used by the Store & Summary tab and the "
                          "Run tab's 'ingest into store' toggle chip (default: <output-root>/store.db)")
    ap.add_argument("--jobs-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "jobs"),
                     help="root of the pending/running/done/failed job directories the Run tab's "
                          "'queue instead of run now' toggle writes into (item 13; default: web/jobs, "
                          "the same default wspy-queue itself uses) -- jobs are processed by the "
                          "separate wspy-queue tool, not by this server")
    ap.add_argument("--phoronix-test-suite", default="phoronix-test-suite", dest="phoronix_test_suite",
                     help="phoronix-test-suite binary/command the Run tab's 'Check' button (item 18) "
                          "uses to look up a Phoronix test's install status and estimated/measured "
                          "runtime (default: resolved via PATH, like wspy-plot's gnuplot dependency)")
    ap.add_argument("--phoronix-pts-dir", default=PHORONIX_PTS_DEFAULT_DIR, dest="phoronix_pts_dir",
                     help="root directory of the actual phoronix-test-suite install (its own "
                          "'phoronix-test-suite' launcher script hardcodes this as PTS_DIR) -- used "
                          "by the Check button's result_notifier.php bug check to find "
                          "pts-core/modules/result_notifier.php (default: %(default)s)")
    args = ap.parse_args()

    output_root = os.path.abspath(args.output_root)
    os.makedirs(output_root, exist_ok=True)
    run_index_file = os.path.abspath(args.run_index_file) if args.run_index_file else \
        os.path.join(output_root, "run_index.jsonl")
    store_db = os.path.abspath(args.store_db) if args.store_db else \
        os.path.join(output_root, "store.db")

    if not os.path.isfile(args.wspy):
        print(f"warning: wspy binary not found at {args.wspy} (build it with `make` first; "
              f"runs will fail until it exists)", file=sys.stderr)
    if not os.path.isfile(args.wspy_run):
        print(f"warning: wspy-run not found at {args.wspy_run}", file=sys.stderr)
    for label, path in (("wspy-validate", args.wspy_validate), ("wspy-store", args.wspy_store),
                         ("wspy-summary", args.wspy_summary),
                         ("wspy-core-report", args.wspy_core_report),
                         ("wspy-ledger", args.wspy_ledger)):
        if not os.path.isfile(path):
            print(f"warning: {label} not found at {path} (the Validate/Store & Summary tab "
                  f"will fail until it's built -- see CLAUDE.md's Build & Test section)",
                  file=sys.stderr)
    if not os.path.isfile(args.wspy_plot):
        print(f"warning: wspy-plot not found at {args.wspy_plot} (best-effort plot generation "
              f"after a run will fail until it's built -- see CLAUDE.md's Build & Test section)",
              file=sys.stderr)
    if not os.path.isfile(args.proctree):
        print(f"warning: proctree not found at {args.proctree} (best-effort tree reconstruction "
              f"after a --tree pass will fail until it's built -- see CLAUDE.md's Build & Test section)",
              file=sys.stderr)
    if not os.path.isfile(args.wspy_analyze):
        print(f"warning: wspy-analyze not found at {args.wspy_analyze} (the report page's "
              f"'AI narrative analysis' button will fail until it's present)", file=sys.stderr)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.wspy_cfg = {
        "wspy_bin": os.path.abspath(args.wspy),
        "wspy_run_bin": os.path.abspath(args.wspy_run),
        "wspy_plot_bin": os.path.abspath(args.wspy_plot),
        "proctree_bin": os.path.abspath(args.proctree),
        "output_root": output_root,
        "wspy_validate_bin": os.path.abspath(args.wspy_validate),
        "wspy_store_bin": os.path.abspath(args.wspy_store),
        "wspy_summary_bin": os.path.abspath(args.wspy_summary),
        "wspy_core_report_bin": os.path.abspath(args.wspy_core_report),
        "wspy_analyze_bin": os.path.abspath(args.wspy_analyze),
        "wspy_ledger_bin": os.path.abspath(args.wspy_ledger),
        "run_index_file": run_index_file,
        "store_db": store_db,
        "jobs_dir": os.path.abspath(args.jobs_dir),
        "phoronix_bin": args.phoronix_test_suite,
        "phoronix_pts_dir": args.phoronix_pts_dir,
    }
    print(f"wspy web launcher listening on http://{args.host}:{args.port}  "
          f"(output root: {output_root})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
