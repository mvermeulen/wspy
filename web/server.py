#!/usr/bin/env python3
"""
wspy web launcher + report browser (INVESTIGATION_4.0.md 4.1 Tier 2, items 6-7).

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
import copy
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
PNG_NAME = "amdtopdown.png"  # legacy root-level plot name from the retired gnuplot.sh; old
                              # reports on disk from before item 12 may still have one there.
LOG_NAME = "launch.log"
PLOTS_DIR_NAME = "plots"      # wspy-plot's own output directory (item 12), relative to a run dir
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
# Item 9 (INVESTIGATION_4.0.md): the configuration/option checklist that
# generalizes item 7's preset-only wspy-run launcher into the real
# preset/configuration/option hierarchy the "deep-dive" section describes.
# A "configuration" here is one of the doc's own examples (a process tree, an
# interval measurement of performance counters, an interval measurement of
# other system metrics, an overall performance-counters measurement, an
# overall system measurement) plus the mockup feedback's explicit GPU/IBS
# extensions -- five independently toggleable rows, each with its own
# sub-options, each becoming its own separate `wspy` invocation/pass (mirrors
# wspy-run's own one-pass-per-configuration shape, just assembled from
# checklist state instead of a hardcoded PASS_NAMES/PASS_FLAGS pair).
#
# Deliberately NOT attempted: decomposing a named preset (BUILTIN_PROFILES)
# into equivalent checklist state, or detecting "this checklist state happens
# to equal preset X". The deep-dive's own rule is that a preset names a
# configuration+option combination wspy-run already knows how to run -- the
# moment it's customized it leaves that space and becomes direct wspy command
# lines -- so presets stay atomic (picked from a dropdown, run via wspy-run
# exactly as item 7 already did) and are simply mutually exclusive with the
# checklist rather than reverse-engineered from it. That keeps the "live
# indicator" honest and simple: a preset is selected, or the checklist is in
# play -- never a fuzzy "close enough" match.
# ---------------------------------------------------------------------------

# (group name, default_on) -- default_on groups (ipc/software) need an
# explicit --no-<name> when left unchecked; the rest need an explicit
# --<name> when checked. Mirrors wspy.h's COUNTER_* set and is also the exact
# token vocabulary multipass.c's multipass_group_names[] uses for
# --passes=<list>, so the same list drives both the plain-flags and
# --passes-bin-packed branches below without a second table to keep in sync.
ALL_GROUPS = [
    ("ipc", True),
    ("topdown", False),
    ("topdown2", False),
    ("topdown-frontend", False),
    ("topdown-backend", False),
    ("topdown-optlb", False),
    ("branch", False),
    ("cache1", False),
    ("cache2", False),
    ("cache3", False),
    ("dcache", False),
    ("icache", False),
    ("tlb", False),
    ("memory", False),
    ("opcache", False),
    ("software", True),
    ("float", False),
]
GROUP_NAMES = [name for name, _ in ALL_GROUPS]
GROUP_NAME_SET = set(GROUP_NAMES)
GROUP_DEFAULT_ON = {name for name, default_on in ALL_GROUPS if default_on}


def counter_group_flags(requested_groups):
    """Whitelist-filters requested_groups against ALL_GROUPS and returns
    (flags, selected_set) -- flags is the minimal --<name>/--no-<name> list
    needed to reach exactly that selection from wspy's own defaults (ipc and
    software on, everything else off), in ALL_GROUPS' fixed order so the
    result is deterministic regardless of the client's array order."""
    selected = {g for g in (requested_groups or []) if g in GROUP_NAME_SET}
    flags = []
    for name, default_on in ALL_GROUPS:
        if name in selected and not default_on:
            flags.append(f"--{name}")
        elif name not in selected and default_on:
            flags.append(f"--no-{name}")
    return flags, selected


# ---------------------------------------------------------------------------
# Custom plots (item 12's wspy-plot --plot/--only-custom, Run tab section):
# a custom plot's column list is otherwise completely decoupled from which
# counter groups are actually being collected, so a column can be requested
# that the run simply never produces -- wspy-plot degrades gracefully (skips
# the missing column, warns), but silently producing an empty or partial
# plot is a worse experience than making sure the right groups (and
# --interval, without which there's no "time" column for wspy-plot to chart
# at all) are turned on in the first place. COLUMN_TO_GROUP maps a column's
# literal CSV header text (topdown.c's/system.c's own PRINT_CSV_HEADER
# strings -- see CLAUDE.md's plot.c entry for the same column-identity
# convention) to the ALL_GROUPS name whose --flag produces it.
#
# Deliberately excludes two ALL_GROUPS entries: "topdown2" duplicates
# "topdown"'s own column names verbatim (both call print_topdown()), so
# resolving a topdown column to "topdown2" instead of "topdown" would be an
# arbitrary choice -- "topdown" is the canonical resolution and auto-
# enabling it is sufficient; "cache1" (--cache1) is a dead flag (topdown.c's
# setup_counter_groups() never wires COUNTER_L1CACHE into any group
# constructor), so it produces zero CSV columns on any vendor and there's
# nothing a column could ever resolve to it for.
COLUMN_TO_GROUP = {
    "ipc": "ipc",
    "retire": "topdown", "frontend": "topdown", "backend": "topdown",
    "speculate": "topdown", "confidence": "topdown", "sanity": "topdown",
    "icache": "topdown-frontend", "itlb1": "topdown-frontend",
    "itlb2": "topdown-frontend", "tlbflush": "topdown-frontend",
    "l1_bound": "topdown-backend", "l2_bound": "topdown-backend",
    "l3_bound": "topdown-backend", "dram_bound": "topdown-backend",
    "store_bound": "topdown-backend",
    "opcache": "topdown-optlb", "dtlb1": "topdown-optlb", "dtlb2": "topdown-optlb",
    "branch miss": "branch",
    "l2miss": "cache2",
    "l3miss": "cache3",
    "L1-dcache miss": "dcache",
    "L1-icache miss": "icache",
    "dTLB miss": "tlb", "iTLB miss": "tlb",
    "bandwidth": "memory",
    "opcache miss": "opcache",
    "float": "float",
    "cpu-clock": "software", "task-clock": "software", "page faults": "software",
    "context switches": "software", "cpu migrations": "software",
    "major page faults": "software", "minor page faults": "software",
    "alignment faults": "software", "emulation faults": "software",
}
# --system's own columns (system.c) -- not an ALL_GROUPS/counter_mask entry,
# so resolve_column_group() reports these via the "system" sentinel instead,
# toggling the checklist's separate "system" configuration rather than a
# counters group. "net <iface>" is one column per interface discovered on
# this host, so it's matched by prefix rather than listed by name.
SYSTEM_COLUMN_NAMES = {"load", "runnable", "cpu", "idle", "iowait", "irq"}


def resolve_column_group(column_name):
    """Returns the ALL_GROUPS name (or the "system" sentinel) whose --flag
    must be enabled to produce column_name in a wspy CSV, or None if
    column_name isn't a column this tool recognizes (a typo, or a
    workload-specific name nothing here can auto-detect)."""
    if column_name in SYSTEM_COLUMN_NAMES or column_name.startswith("net "):
        return "system"
    return COLUMN_TO_GROUP.get(column_name)


def autofit_checklist_for_custom_plots(checklist, custom_plots):
    """Makes sure every custom plot's requested columns will actually be
    collected: auto-enables 'Performance counters' (and the specific
    group(s) needed) and/or 'System metrics', and auto-sets a 1-second
    --interval wherever one isn't already given, since a custom plot has
    nothing to chart without a "time" column. Returns (new_checklist,
    notes) -- a deep copy with whatever was missing turned on, plus a
    human-readable note per change made (never a silent one). A column
    that doesn't resolve to a known group (resolve_column_group() ->
    None) is left alone and reported separately, since there's nothing
    to auto-enable for it."""
    checklist = copy.deepcopy(checklist) if checklist else {}
    notes = []
    needed_groups = set()
    needs_system = False
    unresolved = set()

    for cp in (custom_plots or []):
        for col in cp.get("columns", []):
            group = resolve_column_group(col)
            if group is None:
                unresolved.add(col)
            elif group == "system":
                needs_system = True
            else:
                needed_groups.add(group)

    if needed_groups:
        counters = checklist.setdefault("counters", {})
        if not counters.get("enabled"):
            counters["enabled"] = True
            notes.append("auto-enabled 'Performance counters' for a custom plot")
        existing_groups = set(counters.get("groups") or [])
        new_groups = needed_groups - existing_groups
        if new_groups:
            counters["groups"] = sorted(existing_groups | needed_groups)
            notes.append(f"auto-checked counter group(s) {', '.join(sorted(new_groups))} for a custom plot")
        if not str(counters.get("interval_secs") or "").strip():
            counters["interval_secs"] = "1"
            notes.append("auto-set a 1s interval on 'Performance counters' so the custom plot has "
                          "a time series to chart")
        counters["csv"] = True

    if needs_system:
        system = checklist.setdefault("system", {})
        if not system.get("enabled"):
            system["enabled"] = True
            notes.append("auto-enabled 'System metrics' for a custom plot")
        if not str(system.get("interval_secs") or "").strip():
            system["interval_secs"] = "1"
            notes.append("auto-set a 1s interval on 'System metrics' so the custom plot has a "
                          "time series to chart")
        system["csv"] = True

    if unresolved:
        notes.append("column(s) " + ", ".join(sorted(unresolved)) +
                      " aren't a recognized wspy output column -- wspy-plot will skip them at run "
                      "time unless your workload/counter selection actually produces them")

    return checklist, notes


# Best-effort column coverage per BUILTIN_PROFILES entry -- which CSV
# columns actually land in a *time-series* (--interval) CSV wspy-plot can
# chart at all, derived by hand from wspy-run's own load_builtin_profile()
# PASS_FLAGS. A convenience hint, not the enforcement point; wspy-run's own
# PASS_FLAGS remain authoritative, so keep this in sync by hand if a builtin
# profile's passes change. Most profiles' passes never use --interval at all
# (multi-pass/aggregate/no-CSV), so they produce nothing wspy-plot can chart
# regardless of which counter groups they collect -- only deep-cpu/deep-gpu
# have any --interval passes today. build_supplementary_plot_passes() below
# uses this table to find what a preset's own passes are missing, not just to
# warn about it.
PROFILE_PLOTTABLE_COLUMNS = {
    "quick": set(),
    "deep-cpu": SYSTEM_COLUMN_NAMES | {"net *",
                 "retire", "frontend", "backend", "speculate", "confidence", "sanity"},
    "deep-cpu-intel": set(),
    "deep-gpu": SYSTEM_COLUMN_NAMES | {"net *",
                 "retire", "frontend", "backend", "speculate", "confidence", "sanity",
                 "gpu_busy", "gpu_temp", "gpu_activity", "gpu_power", "gpu_freq"},
    "tree-heavy": set(),
    "ibs-basic": set(),
    "ibs-memory-deep": set(),
}


def build_supplementary_plot_passes(rundir, profile_spec, custom_plots):
    """Preset-mode counterpart to autofit_checklist_for_custom_plots(): a
    preset's own wspy-run passes stay atomic (the deep-dive's own rule --
    never decomposed or edited), but a custom plot asking for column(s) none
    of the preset's passes will ever produce (per PROFILE_PLOTTABLE_COLUMNS)
    would otherwise just warn and leave wspy-plot with no time series to
    chart. Instead, resolve exactly the missing column(s) the same way
    autofit_checklist_for_custom_plots() would (against an empty checklist,
    so nothing the preset already covers is duplicated), and turn the result
    into one or two extra, ordinary `wspy` passes -- named with a
    'plotdata-' prefix so they can never collide with a builtin profile's own
    pass filenames (e.g. deep-cpu's 'amdtopdown.csv'). These run alongside
    wspy-run's own invocation, not instead of it, and land in the same run
    directory, so wspy-plot's whole-directory CSV scan (and
    render_wspy_run_report()'s "Other artifacts" listing) picks them up with
    no further plumbing. profile_spec may comma-compose more than one
    builtin profile (wspy-run's own convention), so coverage is the union
    across every token. Returns (passes, notes) -- passes is empty (with a
    plain per-column warning note, same wording as before) when a missing
    column doesn't resolve to any known group; notes is empty entirely if
    every requested column is already covered or custom_plots is empty."""
    if not custom_plots:
        return [], []
    covered = set()
    for token in (profile_spec or "").split(","):
        covered |= PROFILE_PLOTTABLE_COLUMNS.get(token.strip(), set())
    missing = set()
    for cp in custom_plots:
        for c in cp.get("columns", []):
            if not (c in covered or (c.startswith("net ") and "net *" in covered)):
                missing.add(c)
    if not missing:
        return [], []

    synthetic = [{"name": "_missing", "columns": sorted(missing)}]
    checklist, autofit_notes = autofit_checklist_for_custom_plots({}, synthetic)
    passes = build_configuration_passes(rundir, checklist)
    for p in passes:
        p["name"] = "plotdata-" + p["name"]

    notes = []
    if passes:
        notes.append(f"note: preset '{profile_spec}' doesn't collect column(s) "
                      f"{', '.join(sorted(missing))} needed by your custom plot(s) -- added "
                      f"supplementary pass(es) {', '.join(p['name'] for p in passes)} alongside "
                      f"the preset to collect them")
    for note in autofit_notes:
        if note.startswith("column(s) "):
            notes.append("warning: " + note)
    return passes, notes


def parse_optional_int(value, lo, hi):
    """Returns an int in [lo,hi] parsed from value, or None if value is
    blank/absent -- the checklist's numeric fields (interval seconds, IBS
    thresholds, ...) are all optional, and "not given" is meaningfully
    different from 0."""
    if value is None:
        return None
    s = str(value).strip()
    if not s:
        return None
    try:
        n = int(s)
    except ValueError:
        return None
    if n < lo or n > hi:
        return None
    return n


def build_configuration_passes(rundir, checklist):
    """The one place checklist state (see the item-9 comment above) becomes
    real wspy flags -- used identically by the preview endpoint and the real
    executor, so a preview is never a paraphrase of what actually runs.
    Returns a list of {"name","flags","csv","timeout"} dicts, in the fixed
    tree/counters/system/gpu/ibs order, one per *enabled and non-empty*
    configuration (an enabled configuration with nothing meaningful selected,
    e.g. "counters" with no groups checked, is silently skipped rather than
    producing a no-op wspy invocation)."""
    checklist = checklist or {}
    passes = []

    tree = checklist.get("tree") or {}
    if tree.get("enabled"):
        flags = ["--tree", os.path.join(rundir, "process.tree.txt")]
        if tree.get("cmdline"):
            flags.append("--tree-cmdline")
        if tree.get("open"):
            flags.append("--tree-open")
        if tree.get("vmsize"):
            flags.append("--tree-vmsize")
        flags.append("--software" if tree.get("software", True) else "--no-software")
        flags.append("--no-ipc")
        timeout = parse_optional_int(tree.get("timeout_secs"), 1, 86400)
        passes.append({"name": "tree", "flags": flags, "csv": False, "timeout": timeout})

    counters = checklist.get("counters") or {}
    if counters.get("enabled"):
        group_flags, selected = counter_group_flags(counters.get("groups"))
        if selected:
            interval = parse_optional_int(counters.get("interval_secs"), 1, 3600)
            per_core = bool(counters.get("per_core")) and interval is not None
            rusage_on = bool(counters.get("rusage"))
            csv = bool(counters.get("csv", True))
            # --passes rejects --interval/--per-core outright (wspy.c, see
            # CLAUDE.md's wspy.c entry) -- so interval mode always uses plain
            # flags (potentially multiplexed by the kernel across >1 group,
            # same as any ordinary wspy invocation would be); aggregate mode
            # only needs --passes' bin-packing once >=2 groups are requested,
            # since a single group never multiplexes against itself.
            if interval is None and len(selected) >= 2:
                ordered = [n for n in GROUP_NAMES if n in selected]
                flags = [f"--passes={','.join(ordered)}"]
            else:
                flags = list(group_flags)
                if interval is not None:
                    flags += ["--interval", str(interval)]
                if per_core:
                    flags.append("--per-core")
            flags.append("--rusage" if rusage_on else "--no-rusage")
            if csv:
                flags.append("--csv")
            # Reuse the well-known "amdtopdown" name for exactly the case
            # that name always meant elsewhere in this codebase: an
            # interval, CSV, topdown-only sweep. wspy-plot (item 12) matches
            # templates against a CSV's header, not its filename, so this
            # naming is now just continuity with older reports, not a
            # requirement for plotting to fire.
            name = "amdtopdown" if (interval is not None and csv and selected == {"topdown"}) else "counters"
            passes.append({"name": name, "flags": flags, "csv": csv, "timeout": None})

    system = checklist.get("system") or {}
    if system.get("enabled"):
        interval = parse_optional_int(system.get("interval_secs"), 1, 3600)
        csv = bool(system.get("csv", True))
        flags = ["--system", "--no-ipc", "--no-rusage", "--no-software"]
        if interval is not None:
            flags += ["--interval", str(interval)]
        if csv:
            flags.append("--csv")
        # Same reasoning as "amdtopdown" above -- kept for continuity with
        # older reports, not because wspy-plot needs this literal filename.
        name = "systemtime" if (interval is not None and csv) else "system"
        passes.append({"name": name, "flags": flags, "csv": csv, "timeout": None})

    gpu = checklist.get("gpu") or {}
    if gpu.get("enabled"):
        backend_flags = []
        if gpu.get("busy"):
            backend_flags.append("--gpu-busy")
        if gpu.get("metrics"):
            backend_flags.append("--gpu-metrics")
        if gpu.get("smi"):
            backend_flags.append("--gpu-smi")
        if backend_flags:
            flags = list(backend_flags)
            device = parse_optional_int(gpu.get("device"), 0, 63)
            if device is not None:
                flags += ["--gpu-device", str(device)]
            interval = parse_optional_int(gpu.get("interval_secs"), 1, 3600)
            if interval is not None:
                flags += ["--interval", str(interval)]
            flags += ["--no-ipc", "--no-rusage", "--no-software"]
            csv = bool(gpu.get("csv", True))
            if csv:
                flags.append("--csv")
            passes.append({"name": "gpu", "flags": flags, "csv": csv, "timeout": None})

    ibs = checklist.get("ibs") or {}
    if ibs.get("enabled"):
        profile = ibs.get("profile") if ibs.get("profile") in ("basic", "memory-deep") else "basic"
        flags = ["--ibs-basic" if profile == "basic" else "--ibs-memory-deep", "--no-ipc"]
        maxcnt = parse_optional_int(ibs.get("maxcnt"), 1, 10 ** 9)
        ldlat = parse_optional_int(ibs.get("ldlat"), 0, 10 ** 6)
        fetchlat = parse_optional_int(ibs.get("fetchlat"), 0, 10 ** 6)
        if maxcnt is not None:
            flags += ["--ibs-maxcnt", str(maxcnt)]
        if ldlat is not None:
            flags += ["--ibs-ldlat", str(ldlat)]
        if fetchlat is not None:
            flags += ["--ibs-fetchlat", str(fetchlat)]
        csv = bool(ibs.get("csv", True))
        if csv:
            flags.append("--csv")
        passes.append({"name": "ibs", "flags": flags, "csv": csv, "timeout": None})

    return passes


def build_pass_argv(wspy_bin, rundir, p, manifest_on, run_index_path):
    """Full argv for one configuration pass, minus the trailing `-- <workload
    argv>` (appended by the caller, which also decides whether to prefix a
    `timeout <secs>` wrapper) -- mirrors wspy-run's own run_pass() shape:
    <pass-name>.<csv|txt> for output, <pass-name>.manifest.json alongside it
    when manifest recording is on."""
    ext = "csv" if p["csv"] else "txt"
    outfile = os.path.join(rundir, f'{p["name"]}.{ext}')
    argv = [wspy_bin] + p["flags"] + ["-o", outfile]
    manifest_path = None
    if manifest_on:
        manifest_path = os.path.join(rundir, f'{p["name"]}.manifest.json')
        argv += ["--manifest", manifest_path]
    if run_index_path:
        argv += ["--run-index", run_index_path]
    return argv, outfile, manifest_path


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


def valid_relpath(s):
    """Like valid_segment(), but allows one or more "/"-separated
    components (e.g. "plots/amdtopdown.topdown.png", item 12's plot PNGs
    living one directory level under a run dir) -- every component must
    still individually pass valid_segment(), so "..", a leading/trailing/
    doubled "/", and any character outside NAME_RE's whitelist are all
    rejected the same as they always were for a single segment."""
    if not s or s.startswith("/") or s.endswith("/") or "//" in s:
        return False
    return all(valid_segment(part) for part in s.split("/"))


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


def build_plot_argv(wspy_plot_bin, rundir, custom_plots=None, only_custom=False):
    """wspy-plot (item 12, "shared plotting templates") over the whole run
    directory: it scans every *.csv itself and matches each against the
    shared template table, so -- unlike the old gnuplot.sh, which only knew
    two literal filenames -- this one command line covers any counter-group
    combination a run happened to produce, with no "did this produce
    amdtopdown.csv?" gate needed before calling it.

    custom_plots (the Run tab's "Custom plots" section, validated by
    _parse_custom_plots()) becomes one --plot NAME=col1,col2,... per entry
    -- wspy-plot's own escape hatch for grouping specific counters onto one
    plot regardless of the built-in templates' groupings; only_custom adds
    --only-custom, which renders exactly those spec(s) and skips every
    built-in template and fallback plot."""
    argv = [wspy_plot_bin, "--rundir", rundir, "--quiet"]
    for cp in (custom_plots or []):
        argv += ["--plot", f"{cp['name']}={','.join(cp['columns'])}"]
    if only_custom:
        argv.append("--only-custom")
    return argv


def list_plot_pngs(rundir):
    """Every *.png wspy-plot wrote into <rundir>/plots/, as filenames
    relative to rundir (e.g. "plots/amdtopdown.topdown.png") -- the shape
    collect_run_files()/render_wspy_run_report()'s "other artifacts" scan
    and render_fixed_report() all offer plot images in."""
    plots_dir = os.path.join(rundir, PLOTS_DIR_NAME)
    try:
        names = sorted(f for f in os.listdir(plots_dir)
                        if f.endswith(".png") and os.path.isfile(os.path.join(plots_dir, f)))
    except OSError:
        return []
    return [f"{PLOTS_DIR_NAME}/{f}" for f in names]


def valid_profile_spec(spec):
    tokens = spec.split(",")
    return bool(tokens) and all(PROFILE_TOKEN_RE.match(t) for t in tokens)


def build_wspy_run_argv(wspy_run_bin, wspy_bin, output_root, suite, benchmark,
                         run_id, profile, workload_argv, run_index_path=None):
    # wspy-run always writes each pass's manifest into the run directory once
    # --suite/--benchmark select the unified layout (MANIFEST_DIR defaults to
    # RUNROOT unconditionally there's no flag to opt back out) -- so unlike
    # the custom checklist path, there's no "manifest off" toggle to thread
    # through here; only --run-index is actually optional.
    argv = [wspy_run_bin, "--wspy", wspy_bin, "-o", output_root,
            "--suite", suite, "--benchmark", benchmark, "--run-id", run_id]
    if run_index_path:
        argv += ["--run-index", run_index_path]
    argv += [profile, "--"] + workload_argv
    return argv


def shell_preview(argv, cwd=None):
    s = shlex.join(argv)
    if cwd:
        return f"(cd {shlex.quote(cwd)} && {s})"
    return s


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


def run_store_ingest_besteffort(emit, cfg, run_index_path):
    """Best-effort trailing step shared by every run path (item 9's
    defaults-on "ingest into store" toggle chip): re-runs wspy-store against
    the shared run-index file so the normalized store (Tier 1, store.c) stays
    current without a separate manual step. Never fails the run itself --
    same degrade-don't-fail idiom as the plot generation step above."""
    if not run_index_path:
        emit("[skipping store ingest: run index was not recorded for this run]")
        return
    argv = [cfg["wspy_store_bin"], "--db", cfg["store_db"], "--run-index", run_index_path]
    emit("$ " + shell_preview(argv))
    try:
        proc = subprocess.Popen(argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        for line in proc.stdout:
            emit(line.rstrip("\n"))
        rc = proc.wait()
        emit(f"[wspy-store exited {rc}]")
    except OSError as e:
        emit(f"[error] failed to launch wspy-store ({cfg['wspy_store_bin']}): {e}")


def execute_profile_run(state, cfg, rundir, suite, benchmark, run_id, profile,
                         workload_argv, run_index_path=None, store_ingest=False,
                         custom_plots=None, only_custom=False, preset_notes=None,
                         supp_passes=None, manifest_on=False):
    """Item 7: invoke wspy-run itself (rather than wspy directly) for one of
    its builtin profiles, then -- mirroring workload/phoronix/run_test.sh's
    own hand-written pattern -- best-effort run wspy-plot (item 12) over the
    whole run directory afterward. Unlike the old gnuplot.sh, wspy-plot
    matches its shared templates against whatever CSV(s) the chosen profile
    actually produced, so there's no "did this profile make amdtopdown.csv?"
    gate needed first -- deep-cpu-intel/quick/tree-heavy/ibs-* now get
    whatever plots their own CSVs support instead of none. Item 9 adds the
    optional trailing run-index/store-ingest steps (the "preset" side of the
    Run tab's toggle chips); manifest recording has no toggle here for
    wspy-run's own passes, since its unified layout always writes one per
    pass regardless -- but it does apply to supp_passes below, which are
    plain `wspy` invocations like any custom-mode pass.

    supp_passes (build_supplementary_plot_passes()) are extra, ordinary
    `wspy` passes run after wspy-run's own invocation finishes and before
    wspy-plot, purely to collect column(s) a custom plot needs that the
    preset's own passes don't produce -- wspy-run's own invocation is never
    modified, so the preset itself stays atomic. A supplementary pass
    failing doesn't fail the run (same degrade-don't-fail idiom as the
    wspy-plot step below); its CSV/manifest just won't exist for wspy-plot
    or the report to find."""
    log_path = os.path.join(rundir, LOG_NAME)
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    for note in (preset_notes or []):
        emit(f"[note] {note}")

    wspy_run_argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"],
                                         cfg["output_root"], suite, benchmark,
                                         run_id, profile, workload_argv,
                                         run_index_path=run_index_path)
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

    for p in (supp_passes or []):
        argv, outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                          manifest_on, run_index_path)
        full_argv = argv + ["--"] + workload_argv
        if p["timeout"]:
            full_argv = ["timeout", str(p["timeout"])] + full_argv
        emit(f"[{p['name']}] $ " + shell_preview(full_argv))
        try:
            supp_proc = subprocess.Popen(full_argv, cwd=REPO_ROOT,
                                          stdout=subprocess.PIPE,
                                          stderr=subprocess.STDOUT,
                                          text=True, bufsize=1)
            for line in supp_proc.stdout:
                emit(line.rstrip("\n"))
            supp_rc = supp_proc.wait()
        except OSError as e:
            emit(f"[error] failed to launch wspy for supplementary pass "
                 f"'{p['name']}' ({cfg['wspy_bin']}): {e}")
            supp_rc = 1
        emit(f"[{p['name']}] exited {supp_rc} -> {os.path.basename(outfile)}")

    plot_argv = build_plot_argv(cfg["wspy_plot_bin"], rundir, custom_plots, only_custom)
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
        emit(f"[error] failed to launch wspy-plot ({cfg['wspy_plot_bin']}): {e}")
        plot_rc = 1

    if store_ingest:
        run_store_ingest_besteffort(emit, cfg, run_index_path)

    logf.close()
    status = "done" if wspy_run_rc == 0 and plot_rc == 0 else "error"
    state.finish(status, None)


def write_custom_run_manifest(rundir, suite, benchmark, run_id, workload_argv, pass_records):
    """Same shape as wspy-run's own generate_manifest() (see wspy-run's
    comment there) -- writing the identical layout_version/suite/benchmark/
    run_id/command/passes[] fields means render_wspy_run_report() renders a
    checklist-driven custom run exactly like a wspy-run profile run, with no
    extra branching needed in the report layer for item 9's new run shape."""
    data = {
        "layout_version": "1.0.0",
        "suite": suite,
        "benchmark": benchmark,
        "run_id": run_id,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z"),
        "command": workload_argv,
        "passes": [
            {"name": p["name"], "output": p["output"], "manifest": p["manifest"], "status": p["status"]}
            for p in pass_records
        ],
    }
    with open(os.path.join(rundir, RUN_MANIFEST_NAME), "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def write_custom_run_summary(rundir, pass_records):
    """Mirrors wspy-run's generate_summary(): concatenates every non-CSV,
    non-tree pass's text output into one summary.txt."""
    chunks = []
    for p in pass_records:
        if p["output"].endswith(".csv") or p["kind"] == "tree":
            continue
        path = os.path.join(rundir, p["output"])
        try:
            with open(path) as f:
                content = f.read()
        except OSError:
            continue
        chunks.append(f"=== {p['name']} ===\n{content}\n")
    if not chunks:
        return
    with open(os.path.join(rundir, SUMMARY_NAME), "w") as f:
        f.write("".join(chunks))


def execute_custom_run(state, cfg, rundir, suite, benchmark, run_id, workload_argv,
                        checklist, manifest_on, run_index_path, store_ingest,
                        custom_plots=None, only_custom=False, autofit_notes=None):
    """Item 9's "customized away from a preset" path: runs each enabled
    configuration (see build_configuration_passes()) as its own sequential
    wspy invocation into this run directory -- the direct-command-lines
    fallback the deep-dive's own rule calls for once a preset's checklist has
    been touched. Ends by writing a wspy-run-shaped manifest.json/summary.txt
    (see write_custom_run_manifest()/write_custom_run_summary() above) so the
    existing report/curation/compare machinery needs no new code path.

    checklist has already been through autofit_checklist_for_custom_plots()
    by the caller (_start_custom_run) -- autofit_notes is only threaded
    through here to surface what was auto-enabled in the live log, not to
    redo the autofit (that would find nothing left to change)."""
    log_path = os.path.join(rundir, LOG_NAME)
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    for note in (autofit_notes or []):
        emit(f"[note] {note}")

    passes = build_configuration_passes(rundir, checklist)
    if not passes:
        emit("[error] no configuration was enabled (or every enabled configuration had "
             "nothing selected within it) -- nothing to run")
        logf.close()
        state.finish("error", None)
        return

    pass_records = []
    any_failed = False
    for p in passes:
        argv, outfile, manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                         manifest_on, run_index_path)
        full_argv = argv + ["--"] + workload_argv
        if p["timeout"]:
            full_argv = ["timeout", str(p["timeout"])] + full_argv
        emit(f"[{p['name']}] $ " + shell_preview(full_argv))
        try:
            proc = subprocess.Popen(full_argv, cwd=REPO_ROOT,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
            for line in proc.stdout:
                emit(line.rstrip("\n"))
            rc = proc.wait()
        except OSError as e:
            emit(f"[error] failed to launch wspy for pass '{p['name']}' ({cfg['wspy_bin']}): {e}")
            rc = 1
        status = "ok" if rc == 0 else "wspy-error"
        any_failed = any_failed or rc != 0
        emit(f"[{p['name']}] exited {rc} -> {os.path.basename(outfile)}")
        pass_records.append({
            "name": p["name"],
            "output": os.path.basename(outfile),
            "manifest": os.path.basename(manifest_path) if manifest_path else None,
            "status": status,
            "kind": "tree" if p["name"] == "tree" else "other",
        })

    plot_argv = build_plot_argv(cfg["wspy_plot_bin"], rundir, custom_plots, only_custom)
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
        emit(f"[error] failed to launch wspy-plot ({cfg['wspy_plot_bin']}): {e}")
        plot_rc = 1

    write_custom_run_summary(rundir, pass_records)
    write_custom_run_manifest(rundir, suite, benchmark, run_id, workload_argv, pass_records)
    emit(f"[wrote {RUN_MANIFEST_NAME}]")

    if store_ingest:
        run_store_ingest_besteffort(emit, cfg, run_index_path)

    logf.close()
    status = "done" if not any_failed and plot_rc == 0 else "error"
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


# ---------------------------------------------------------------------------
# Historical run index browser/search (item 11, INVESTIGATION_4.0.md 4.1 Tier
# 2) -- discover_reports() above is the homepage's cheap, mtime-only recent
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


def run_sync(argv, cwd=None, timeout=120):
    """Runs a short-lived discovery/report command (wspy --capabilities,
    wspy-validate, wspy-store, wspy-summary) to completion and captures its
    combined output -- unlike a launched workload, none of these have
    unbounded runtime or need live streaming, so a plain synchronous
    subprocess call (no RunState/SSE machinery) is the right amount of
    plumbing. Returns (returncode_or_None, output_text, timed_out)."""
    try:
        proc = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, False
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or ""), True
    except OSError as e:
        return None, f"[error] failed to launch {argv[0]}: {e}", False


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

    for f in list_plot_pngs(rundir):
        add(f, f"plot: {os.path.basename(f)}")

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
# Publish-ready export (item 10, INVESTIGATION_4.0.md): renders #8's curated
# block sequence into a format ready to paste elsewhere, rather than a bulk
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


def _export_blocks(rundir):
    curation = load_curation(rundir)
    if not curation:
        return []
    return [b for b in curation.get("blocks", []) if b.get("depth", "none") != "none"]


def render_export_markdown(rundir, base_url, title, blocks):
    parts = [f"# {title}\n"]
    for b in blocks:
        parts.append(f"## {b.get('title') or '(untitled)'}\n")
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


def render_export_html(rundir, base_url, title, blocks):
    body_parts = [f'<h1 style="font-family:sans-serif;">{html.escape(title)}</h1>']
    for b in blocks:
        body_parts.append(
            f'<h2 style="font-family:sans-serif;margin-top:2em;">{html.escape(b.get("title") or "(untitled)")}</h2>')
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


def render_export_wordpress(rundir, base_url, title, blocks):
    parts = [_wp_block("heading", f"<h1>{html.escape(title)}</h1>", {"level": 1})]
    for b in blocks:
        parts.append(_wp_block("heading", f'<h2>{html.escape(b.get("title") or "(untitled)")}</h2>',
                                {"level": 2}))
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


def render_export(rundir, base_url, title, fmt, blocks):
    if fmt == "markdown":
        return render_export_markdown(rundir, base_url, title, blocks)
    if fmt == "html":
        return render_export_html(rundir, base_url, title, blocks)
    return render_export_wordpress(rundir, base_url, title, blocks)


def render_export_page(rundir, base_url, suite, benchmark, run_id, fmt):
    studio_url = f"/studio/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    title = f"{suite} / {benchmark} / {run_id}"
    blocks = _export_blocks(rundir)

    if not blocks:
        body = (f'<section class="panel"><h1>Export: {html.escape(benchmark)}/{html.escape(run_id)}</h1>'
                f'<p class="muted">No curated blocks yet &mdash; '
                f'<a href="{studio_url}">curate this report</a> first, then come back here.</p></section>')
        return page(f"export: {benchmark}/{run_id}", body)

    rendered = render_export(rundir, base_url, title, fmt, blocks)
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
        '<code>INVESTIGATION_4.0.md</code> item 10/12).</p>'
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
    preset_options = "".join(f'<option value="{html.escape(p)}">{html.escape(p)}</option>'
                              for p in BUILTIN_PROFILES)
    counters_groups_html = render_group_checkboxes("counters", checked_by_default=("topdown",))

    return f"""
<section class="panel">
  <h1>Run</h1>
  <p class="config-label">Pick a named <code>wspy-run</code> preset, or build a custom run from the
     configuration checklist below. Per the preset/configuration/option hierarchy
     (<code>INVESTIGATION_4.0.md</code>): a preset is atomic -- picking one runs it exactly as
     <code>wspy-run</code> defines it and the checklist below is ignored; set preset back to
     "(custom)" to compose configurations directly instead. Each enabled configuration below becomes
     its own separate <code>wspy</code> invocation into the same run directory.</p>
  <form id="run-form">
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
        <label class="config-toggle"><input type="checkbox" id="tree_enabled"> <strong>Process tree</strong></label>
        <div class="config-options">
          <label><input type="checkbox" id="tree_cmdline"> full command lines <code>--tree-cmdline</code></label>
          <label><input type="checkbox" id="tree_open"> record <code>open()</code> calls <code>--tree-open</code></label>
          <label><input type="checkbox" id="tree_vmsize"> vmsize samples <code>--tree-vmsize</code></label>
          <label><input type="checkbox" id="tree_software" checked> software counters too <code>--software</code></label>
          <label>Timeout seconds <input type="text" id="tree_timeout" placeholder="(none)"></label>
        </div>
      </div>

      <div class="config-card" data-config="counters">
        <label class="config-toggle"><input type="checkbox" id="counters_enabled"> <strong>Performance counters</strong></label>
        <div class="config-options">
          <div class="group-grid">{counters_groups_html}</div>
          <div class="row">
            <label>Interval seconds <input type="text" id="counters_interval" placeholder="(aggregate)"></label>
            <label class="inline-check"><input type="checkbox" id="counters_per_core"> per-core (interval only)</label>
            <label class="inline-check"><input type="checkbox" id="counters_rusage"> include rusage</label>
            <label class="inline-check"><input type="checkbox" id="counters_csv" checked> CSV output</label>
          </div>
          <p class="muted">2+ groups with no interval given automatically bin-pack via native
             multi-pass execution (<code>--passes</code>, wspy's own PMU-fit arithmetic); giving an
             interval always uses plain flags for a single re-execution (per-core available then;
             <code>--passes</code> rejects <code>--interval</code>/<code>--per-core</code> outright).</p>
        </div>
      </div>

      <div class="config-card" data-config="system">
        <label class="config-toggle"><input type="checkbox" id="system_enabled"> <strong>System metrics</strong></label>
        <div class="config-options">
          <label>Interval seconds <input type="text" id="system_interval" placeholder="(aggregate)"></label>
          <label class="inline-check"><input type="checkbox" id="system_csv" checked> CSV output</label>
        </div>
      </div>

      <div class="config-card" data-config="gpu">
        <label class="config-toggle"><input type="checkbox" id="gpu_enabled"> <strong>GPU metrics</strong>
          <span class="muted">(needs an AMDGPU=1 build; otherwise wspy warns and continues)</span></label>
        <div class="config-options">
          <label class="inline-check"><input type="checkbox" id="gpu_busy"> busy % <code>--gpu-busy</code></label>
          <label class="inline-check"><input type="checkbox" id="gpu_metrics"> extended metrics <code>--gpu-metrics</code></label>
          <label class="inline-check"><input type="checkbox" id="gpu_smi"> ROCm SMI <code>--gpu-smi</code></label>
          <div class="row">
            <label>Device index <input type="text" id="gpu_device" placeholder="(default)"></label>
            <label>Interval seconds <input type="text" id="gpu_interval" placeholder="(aggregate)"></label>
            <label class="inline-check"><input type="checkbox" id="gpu_csv" checked> CSV output</label>
          </div>
        </div>
      </div>

      <div class="config-card" data-config="ibs">
        <label class="config-toggle"><input type="checkbox" id="ibs_enabled"> <strong>AMD IBS</strong>
          <span class="muted">(AMD only)</span></label>
        <div class="config-options">
          <label>Profile
            <select id="ibs_profile">
              <option value="basic">basic (unfiltered ibs_fetch+ibs_op)</option>
              <option value="memory-deep">memory-deep (l3missonly+ldlat filtering)</option>
            </select>
          </label>
          <div class="row">
            <label><code>--ibs-maxcnt</code> <input type="text" id="ibs_maxcnt" placeholder="(default)"></label>
            <label><code>--ibs-ldlat</code> <input type="text" id="ibs_ldlat" placeholder="(default)"></label>
            <label><code>--ibs-fetchlat</code> <input type="text" id="ibs_fetchlat" placeholder="(default)"></label>
          </div>
        </div>
      </div>

      <div class="config-card config-reserved">
        <label class="config-toggle"><input type="checkbox" disabled> <strong>/proc extras</strong>
          <span class="muted">(reserved for 4.2 Tier 3 &mdash; not implemented yet)</span></label>
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

    <div class="chips">
      <label class="chip"><input type="checkbox" id="toggle_manifest" checked> Write manifest</label>
      <label class="chip"><input type="checkbox" id="toggle_run_index" checked> Append to run index</label>
      <label class="chip"><input type="checkbox" id="toggle_store_ingest" checked> Ingest into store after run</label>
    </div>

    <fieldset class="preview">
      <legend>Command(s) about to run (copy/paste-able)</legend>
      <pre id="preview">(fill in a workload command above)</pre>
      <p id="preview-notes" class="muted"></p>
    </fieldset>
    <button type="submit" id="run-button">Run</button>
  </form>
  <pre id="live-output" class="live-output" hidden></pre>
  <p id="run-result"></p>
</section>
"""


def render_validate_tab(cfg):
    manifests = discover_manifest_paths(cfg["output_root"])
    chips = "".join(
        f'<button type="button" class="add-manifest-chip" data-path="{html.escape(m["path"])}">'
        f'+ {html.escape(m["rel"])}</button>'
        for m in manifests
    ) or '<span class="muted">no manifests found under the output root yet</span>'
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
    <label class="chip"><input type="checkbox" id="summary-strict"> --strict</label>
  </div>
  <button type="button" id="summary-run">Run wspy-summary</button>
  <pre id="summary-cmdline" class="muted" hidden></pre>
  <pre id="summary-output" class="live-output" hidden></pre>
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
        '<button type="submit">Compare selected</button>'
        "</form>" if rows else "<p class=\"muted\">No runs yet.</p>"
    )

    body = f"""
<nav class="tabs">
  <button type="button" class="tab-btn active" data-tab="run">Run</button>
  <button type="button" class="tab-btn" data-tab="validate">Validate</button>
  <button type="button" class="tab-btn" data-tab="store">Store &amp; Summary</button>
  <button type="button" class="tab-btn" data-tab="discovery">Discovery</button>
</nav>
<div class="tab-panel" id="tab-run">{render_run_tab(prefill, cfg)}</div>
<div class="tab-panel" id="tab-validate" hidden>{render_validate_tab(cfg)}</div>
<div class="tab-panel" id="tab-store" hidden>{render_store_tab(cfg)}</div>
<div class="tab-panel" id="tab-discovery" hidden>{render_discovery_tab()}</div>
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
        '<button type="submit">Compare selected</button>'
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
    export_url = f"/export/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}"
    curated_html = render_curated_section(rundir, base_url, suite, benchmark, run_id)
    if curated_html:
        return (f'<p><a href="{studio_url}">Edit curation</a> &middot; '
                f'<a href="{export_url}">Export</a></p>'
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
  <p><a href="/report/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}">Back to report</a>
     &middot; <a href="/export/{_urlescape(suite)}/{_urlescape(benchmark)}/{_urlescape(run_id)}">Export</a></p>
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
                raw.append(f'<li><a href="{base}/{_urlescape(f)}">{html.escape(f)}</a></li>')
        raw.append("</ul>")

    parts.append(_studio_link_and_curated(rundir, base, suite, benchmark, run_id, "".join(raw)))

    body = "<section class=\"panel\">" + "".join(parts) + "</section>"
    return page(f"wspy report: {benchmark}/{run_id}", body)


def _urlescape(s):
    from urllib.parse import quote
    # safe="/" so a relative filename like "plots/foo.png" (item 12's plot
    # PNGs, one directory level under a run dir) keeps its literal "/" as a
    # path separator -- nothing on the receiving end (do_GET's routing
    # below) unquotes the path, so a %2F here would arrive as a literal,
    # unmatchable "%2F" in the filename instead of a directory separator.
    return quote(s, safe="/")


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
            # profile_workload/profile_suite/profile_benchmark are legacy
            # aliases from when the Run/wspy-run-profile forms were separate
            # (item 7); "customize & run again" on an older report may still
            # link with them, so they're accepted as synonyms rather than
            # broken by item 9's single-form merge.
            prefill = {}
            for key, aliases in (("workload", ("workload", "profile_workload")),
                                  ("suite", ("suite", "profile_suite")),
                                  ("benchmark", ("benchmark", "profile_benchmark"))):
                for alias in aliases:
                    if alias in qs:
                        prefill[key] = qs[alias][0]
                        break
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
            blocks = _export_blocks(rundir)
            if not blocks:
                self._send(400, "no curated blocks to export")
                return
            base = f"/files/{suite}/{benchmark}/{run_id}"
            title = f"{suite} / {benchmark} / {run_id}"
            rendered = render_export(rundir, base, title, fmt, blocks)
            filename = f"{benchmark}-{run_id}-{fmt}.{EXPORT_FORMAT_EXTENSIONS[fmt]}"
            self._send(200, rendered, content_type=EXPORT_FORMAT_CONTENT_TYPES[fmt],
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

        if path == "/history":
            self._send(200, render_history(cfg, qs))
            return

        m = re.match(r"^/files/([^/]+)/([^/]+)/([^/]+)/(.+)$", path)
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

        POST_HANDLERS = {
            "/api/run": self._start_run,
            "/api/run-profile": self._start_profile_run,
            "/api/run-custom": self._start_custom_run,
            "/api/preview": self._preview,
            "/api/discovery/capabilities": self._discovery_capabilities,
            "/api/discovery/preflight": self._discovery_preflight,
            "/api/discovery/validate": self._discovery_validate,
            "/api/discovery/store-ingest": self._discovery_store_ingest,
            "/api/discovery/summary": self._discovery_summary,
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
        ingest -- item 9's mockup-feedback item, see INVESTIGATION_4.0.md
        line ~336). Returns (manifest_on, run_index_path_or_None, store_ingest)."""
        toggles = body.get("toggles") or {}
        manifest_on = bool(toggles.get("manifest", True))
        run_index_on = bool(toggles.get("run_index", True))
        store_ingest = bool(toggles.get("store_ingest", True)) and run_index_on
        run_index_path = cfg["run_index_file"] if run_index_on else None
        return manifest_on, run_index_path, store_ingest

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
                                             run_id, profile, workload_argv,
                                             run_index_path=run_index_path)
        supp_passes, preset_notes = build_supplementary_plot_passes(rundir, profile, custom_plots)

        t = threading.Thread(target=execute_profile_run, args=(
            state, cfg, rundir, suite, benchmark, run_id, profile, workload_argv,
            run_index_path, store_ingest, custom_plots, only_custom, preset_notes,
            supp_passes, manifest_on,
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
        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        command_lines = []
        for p in passes:
            argv, _outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                               manifest_on, run_index_path)
            full_argv = argv + ["--"] + workload_argv
            if p["timeout"]:
                full_argv = ["timeout", str(p["timeout"])] + full_argv
            command_lines.append(shell_preview(full_argv))

        t = threading.Thread(target=execute_custom_run, args=(
            state, cfg, rundir, suite, benchmark, run_id, workload_argv,
            checklist, manifest_on, run_index_path, store_ingest,
            custom_plots, only_custom, autofit_notes,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "commands": command_lines,
            "notes": autofit_notes,
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
        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        plot_argv = build_plot_argv(cfg["wspy_plot_bin"], rundir, custom_plots, only_custom)

        preset = (body.get("preset") or "").strip()
        if preset:
            if not valid_profile_spec(preset):
                self._send_json(400, {"error": "invalid preset spec"})
                return
            argv = build_wspy_run_argv(cfg["wspy_run_bin"], cfg["wspy_bin"], cfg["output_root"],
                                        suite, benchmark, run_id, preset, workload_argv,
                                        run_index_path=run_index_path)
            supp_passes, preset_notes = build_supplementary_plot_passes(rundir, preset, custom_plots)
            lines = [shell_preview(argv)]
            for p in supp_passes:
                pargv, _outfile, _manifest_path = build_pass_argv(cfg["wspy_bin"], rundir, p,
                                                                    _manifest_on, run_index_path)
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
                                                              _manifest_on, run_index_path)
            full_argv = argv + ["--"] + workload_argv
            if p["timeout"]:
                full_argv = ["timeout", str(p["timeout"])] + full_argv
            lines.append(shell_preview(full_argv))
            if "--passes=" in " ".join(p["flags"]):
                notes.append(f"'{p['name']}' uses native multi-pass execution (--passes) to bin-pack "
                              f"its groups into as few re-executions of the workload as fit the PMU budget.")
        if passes:
            lines.append(shell_preview(plot_argv))
            notes.append("wspy-plot will run afterward, best-effort, matching every CSV this run "
                          "produces against the shared plot templates (see CLAUDE.md's plot.c entry) "
                          "and writing whatever fires into plots/.")
        self._send_json(200, {"mode": "custom", "lines": lines, "notes": notes,
                               "resolved_checklist": checklist})

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
        if body.get("strict"):
            argv.append("--strict")
        rc, output, timed_out = run_sync(argv, cwd=REPO_ROOT, timeout=60)
        self._send_json(200, {"command": shell_preview(argv), "exit_code": rc,
                               "output": output, "timed_out": timed_out, "csv": csv})

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
    ap.add_argument("--wspy-plot", default=os.path.join(REPO_ROOT, "wspy-plot"),
                     help="path to the wspy-plot binary (item 12's shared plotting templates; "
                          "default: repo root's ./wspy-plot)")
    ap.add_argument("--output-root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs"),
                     help="directory root for <suite>/<benchmark>/<run-id>/ run output "
                          "(default: web/runs)")
    ap.add_argument("--wspy-validate", default=os.path.join(REPO_ROOT, "wspy-validate"),
                     help="path to the wspy-validate binary (default: repo root's ./wspy-validate)")
    ap.add_argument("--wspy-store", default=os.path.join(REPO_ROOT, "wspy-store"),
                     help="path to the wspy-store binary (default: repo root's ./wspy-store)")
    ap.add_argument("--wspy-summary", default=os.path.join(REPO_ROOT, "wspy-summary"),
                     help="path to the wspy-summary binary (default: repo root's ./wspy-summary)")
    ap.add_argument("--run-index-file",
                     help="shared --run-index file every launched run appends to when the "
                          "'record run index' toggle chip is on (default: <output-root>/run_index.jsonl)")
    ap.add_argument("--store-db",
                     help="normalized-store database used by the Store & Summary tab and the "
                          "Run tab's 'ingest into store' toggle chip (default: <output-root>/store.db)")
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
                         ("wspy-summary", args.wspy_summary)):
        if not os.path.isfile(path):
            print(f"warning: {label} not found at {path} (the Validate/Store & Summary tab "
                  f"will fail until it's built -- see CLAUDE.md's Build & Test section)",
                  file=sys.stderr)
    if not os.path.isfile(args.wspy_plot):
        print(f"warning: wspy-plot not found at {args.wspy_plot} (best-effort plot generation "
              f"after a run will fail until it's built -- see CLAUDE.md's Build & Test section)",
              file=sys.stderr)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.wspy_cfg = {
        "wspy_bin": os.path.abspath(args.wspy),
        "wspy_run_bin": os.path.abspath(args.wspy_run),
        "wspy_plot_bin": os.path.abspath(args.wspy_plot),
        "output_root": output_root,
        "wspy_validate_bin": os.path.abspath(args.wspy_validate),
        "wspy_store_bin": os.path.abspath(args.wspy_store),
        "wspy_summary_bin": os.path.abspath(args.wspy_summary),
        "run_index_file": run_index_file,
        "store_db": store_db,
    }
    print(f"wspy web launcher listening on http://{args.host}:{args.port}  "
          f"(output root: {output_root})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
