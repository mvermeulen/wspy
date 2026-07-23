"""
web/joblib.py -- shared run-building logic for web/server.py and wspy-queue
(INVESTIGATION.md's "What shipped in 4.1", "Deployment/hosting design note").

Everything in here is pure/stdlib-only and has no server-process dependency
(no HTTP, no threading requirement, no in-memory state beyond what a caller
passes in) -- this is what lets wspy-queue process jobs headless, with no
dependency on web/server.py being up, while still sharing the exact same
checklist/preset -> wspy command-line logic the web UI's live preview and
real executors use. Splitting this out of server.py (rather than having
wspy-queue duplicate it, or shell out to the running server) is what keeps
"the one place checklist state becomes flags" (build_configuration_passes(),
below) true even though there are now two independent front ends -- the web
Run tab and wspy-queue -- that both need to turn a configuration into a real
wspy/wspy-run/wspy-plot invocation.

Four groups of things live here:
  1. The configuration/option -> wspy argv builders (ALL_GROUPS through
     build_wspy_run_argv/build_plot_argv) -- moved out of server.py verbatim,
     no behavior change.
  2. The actual run executors (RunState, execute_profile_run,
     execute_custom_run, run_store_ingest_besteffort, write_custom_run_*) --
     also moved out of server.py verbatim. server.py drives these from a
     background thread per HTTP request; wspy-queue drives them synchronously,
     one job at a time, from its own process. Neither needs the other to be
     running.
  3. The job file format (JOB_SCHEMA_VERSION through validate_job) -- new:
     a portable, spec-only JSON document capturing "what should run" in
     close to the same vocabulary #16 (structured configuration provenance)
     will eventually use for "what already ran", so a job and a manifest
     don't drift into separate vocabularies. See build_job()'s docstring for
     the exact shape and the portability rules (no absolute paths, no
     reference to the machine that created it).
  4. Run-directory artifact enumeration/bundling (collect_run_files() through
     build_reproducibility_bundle()) -- moved out of server.py verbatim
     (collect_run_files() backed only the curation studio's "+ add" buttons
     before), plus new bundling logic on top, so wspy-bundle's standalone CLI
     and server.py's own "Download reproducibility bundle" report-page link
     share the identical file list and archive contents instead of drifting
     into two independently-maintained enumerations.
  5. Phoronix single-test-point suite import (parse_openbenchmarking_id()
     through import_phoronix_test_points()) -- INVESTIGATION.md item 26's
     front-end phase: decomposes an already-published OpenBenchmarking
     result or an installed/exported Phoronix suite into one minimal
     single-test-point suite per (test, option-combination), materialized
     under workload/phoronix/ and registered with wspy-ledger --add. Shared
     by wspy-phoronix-import's CLI and web/server.py's Phoronix tab.
"""
import copy
import hashlib
import io
import json
import os
import re
import secrets
import shlex
import shutil
import subprocess
import tarfile
import threading
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
PROFILE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]+$")

# Core/thread affinity control (INVESTIGATION.md's "Core/thread affinity
# control" item, wspy.c's --affinity=<spec>/affinity.h): mirrors the C
# parser's accepted grammar (affinity_parse_spec()) exactly, so a malformed
# spec is rejected here (400) rather than only surfacing as wspy's own
# --affinity warning deep inside a background run's log. "all" is the
# default and never actually reaches a wspy invocation as a flag (see
# build_wspy_run_argv()/build_pass_argv() below), but is still a valid,
# explicit choice in the vocabulary.
AFFINITY_SPEC_RE = re.compile(
    r"^(all|nosmt|thread=\d+|domain=\d+|coretype=\d+|cpuset=\d+(-\d+)?(,\d+(-\d+)?)*)$")


def valid_affinity_spec(spec):
    return bool(spec) and bool(AFFINITY_SPEC_RE.match(spec))

# wspy-run's own unified-layout artifact names (see wspy-run's
# generate_summary()/generate_manifest()) -- shared here since
# execute_profile_run()/execute_custom_run()/write_custom_run_*() below all
# need them regardless of which front end (web UI or wspy-queue) is driving.
LOG_NAME = "launch.log"
PLOTS_DIR_NAME = "plots"
RUN_MANIFEST_NAME = "manifest.json"
SUMMARY_NAME = "summary.txt"

# Item 6's older fixed-configuration launcher's own artifact names (superseded
# on the homepage by item 7/9's unified layout above, but still rendered for
# old reports on disk) -- shared here (not just in server.py) because
# collect_run_files()/classify_bundle_kind() below need to recognize both
# report shapes identically.
CSV_NAME = "amdtopdown.csv"
MANIFEST_NAME = "amdtopdown.manifest.json"
PNG_NAME = "amdtopdown.png"

# The curation studio's own per-run state file (server.py's curation studio) --
# named here too since collect_run_files() below must never offer it as a
# candidate artifact (it's studio-owned metadata, not a run artifact).
CURATION_NAME = "curation.json"


def guess_kind(filename):
    ext = os.path.splitext(filename)[1].lower()
    if ext == ".png":
        return "image"
    if ext == ".csv":
        return "csv"
    if ext == ".json":
        return "json"
    # Unknown extensions are tentatively "text"; server.py's
    # read_text_safely() downgrades to a link-only render if the file
    # doesn't actually decode as UTF-8, rather than requiring every
    # candidate file to be read just to list it.
    return "text"


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


AIANALYSIS_RE = re.compile(r"^aianalysis\.(.+)\.txt$")
AIPROMPT_CRITIQUE_RE = re.compile(r"^aiprompt\.critique\.(.+)\.txt$")


def ai_artifact_label(filename):
    """Friendly (label, ai_generated) for one of wspy-analyze's own output
    files, or None if filename isn't one -- so collect_run_files() below can
    offer something more useful than the bare filename, and so a block built
    from actual model output (aianalysis.*/aiprompt.critique.*, not the
    deterministically-rendered aiprompt.txt itself) carries an AI-generated
    marker from the moment it's added. See INVESTIGATION.md's Ollama
    deep-dive, design decision #7."""
    if filename == "aiprompt.txt":
        return "AI analysis: rendered prompt", False
    m = AIPROMPT_CRITIQUE_RE.match(filename)
    if m:
        return f"AI analysis: prompt critique (model: {m.group(1)})", True
    m = AIANALYSIS_RE.match(filename)
    if m:
        return f"AI narrative analysis (model: {m.group(1)})", True
    return None


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


def collect_run_files(rundir):
    """Every file in a run directory worth offering as a block source, in a
    sensible default order -- wspy-run's own passes (name-labeled) first when
    a run-level manifest exists, else item 6's fixed amdtopdown.* shape, then
    curation.json/wspy-run's own manifest/log, then anything else sitting in
    the directory that neither claims (mirrors render_wspy_run_report's own
    "Other artifacts" scan, generalized for reuse here). Also the enumeration
    build_reproducibility_bundle() below archives, unchanged -- one file list
    for both the curation studio's "+ add" buttons and the reproducibility
    bundle's contents, rather than two independently-drifting scans."""
    run_manifest = read_run_manifest(os.path.join(rundir, RUN_MANIFEST_NAME))
    seen = set()
    items = []

    def add(filename, label, ai_generated=False):
        if not filename or filename in seen:
            return
        if not os.path.isfile(os.path.join(rundir, filename)):
            return
        seen.add(filename)
        items.append({"filename": filename, "kind": guess_kind(filename), "label": label,
                      "ai_generated": ai_generated})

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
        ai_label = ai_artifact_label(f)
        if ai_label is not None:
            label, ai_generated = ai_label
            add(f, label, ai_generated=ai_generated)
        else:
            add(f, f)

    for f in list_plot_pngs(rundir):
        add(f, f"plot: {os.path.basename(f)}")

    return items


# ---------------------------------------------------------------------------
# Reproducibility bundle export (INVESTIGATION.md's 4.2 "Reproducibility
# bundle export" item): given one run directory, produce a single, portable
# tar.gz -- manifest(s) + raw per-pass output + derived summaries/plots/
# curation/AI narrative -- so a run can be archived or handed to someone
# without access to this machine's live output-root/store.db. Scoped to one
# run directory (the same unit every other per-run tool here already
# operates on: wspy-validate, wspy-summary --trace, wspy-analyze --rundir,
# the curation studio) -- bundling a whole sweep/suite is separate future
# work, since a sweep's compare.json lives at the output-root level, not
# inside any one run directory.
# ---------------------------------------------------------------------------

BUNDLE_SCHEMA_VERSION = "1.0"
BUNDLE_MANIFEST_NAME = "bundle_manifest.json"


def classify_bundle_kind(filename):
    """manifest | raw | derived classification for one run-directory file,
    used to label bundle_manifest.json's files[] entries -- "manifest" is
    wspy's own run-identity record, "raw" is a tool's direct output (what
    actually happened), "derived" is something computed from that raw
    output (plots, summaries, curation, AI narrative). Order matters here:
    the manifest check must run before the generic .json catch-all falls
    through to "raw", and the derived filename set before the raw .csv/.txt
    default."""
    base = os.path.basename(filename)
    if base.endswith(".manifest.json") or base == RUN_MANIFEST_NAME:
        return "manifest"
    if (base in (SUMMARY_NAME, CURATION_NAME, PNG_NAME,
                 "process.tree.summary.txt", "process.tree.simple.txt") or
            filename.startswith(PLOTS_DIR_NAME + "/") or
            ai_artifact_label(base) is not None):
        return "derived"
    return "raw"


def build_reproducibility_bundle(rundir, suite, benchmark, run_id):
    """Builds one tar.gz (in memory, returned as bytes) bundling every
    artifact collect_run_files() finds in rundir, plus a new
    bundle_manifest.json index at the tar root: schema_version, suite/
    benchmark/run_id, generated_at, and one {path,kind,sha256,size_bytes}
    entry per file -- sha256 so a recipient can verify the bundle wasn't
    corrupted/tampered with in transit, kind so a reader can tell "what
    happened" (raw) from "what wspy computed" (derived/manifest) without
    guessing from the filename. Returns (tar_bytes, index) -- index is the
    same dict written as bundle_manifest.json, so a caller (wspy-bundle's
    --dry-run, tests) can inspect it without re-parsing the archive.

    A file collect_run_files() lists but that vanishes/becomes unreadable
    between listing and archiving gets kind="missing" and no sha256/
    size_bytes, rather than aborting the whole bundle -- same degrade-don't-
    fail idiom used everywhere else in this codebase."""
    entries = collect_run_files(rundir)
    files_index = []
    buf = io.BytesIO()
    now = int(time.time())
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for entry in entries:
            rel = entry["filename"]
            full = os.path.join(rundir, rel)
            try:
                with open(full, "rb") as f:
                    data = f.read()
            except OSError:
                files_index.append({"path": rel, "kind": "missing"})
                continue
            info = tarfile.TarInfo(name=rel)
            info.size = len(data)
            info.mtime = now
            tar.addfile(info, io.BytesIO(data))
            files_index.append({
                "path": rel,
                "kind": classify_bundle_kind(rel),
                "sha256": hashlib.sha256(data).hexdigest(),
                "size_bytes": len(data),
            })
        index = {
            "schema_version": BUNDLE_SCHEMA_VERSION,
            "suite": suite,
            "benchmark": benchmark,
            "run_id": run_id,
            "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z"),
            "files": files_index,
        }
        index_bytes = json.dumps(index, indent=2).encode("utf-8")
        info = tarfile.TarInfo(name=BUNDLE_MANIFEST_NAME)
        info.size = len(index_bytes)
        info.mtime = now
        tar.addfile(info, io.BytesIO(index_bytes))
    return buf.getvalue(), index


# Fixed staging path scripts/pts_hooks/{pre,post}_test_run.sh write to, if
# Phoronix Test Suite's result_notifier module hooks are registered on this
# host (scripts/setup_phoronix_hooks.sh) -- must match those scripts' own
# default exactly, since PTS invokes them with a replaced environment that
# can't carry this value down (see doc/phoronix_hook_investigation.md).
# wspy-run's own run_pass() relocates it into a per-pass artifact for any
# run launched through wspy-run (execute_profile_run()'s main wspy-run
# invocation); _archive_stale_pts_hooks_log()/_capture_pts_hooks_log() below
# are the Python-side equivalent for the two launch shapes here that invoke
# `wspy` directly instead (execute_custom_run()'s per-configuration passes,
# and execute_profile_run()'s own supplementary plot-data passes) -- without
# this, a Phoronix workload run via the Run tab's checklist mode (rather
# than a preset) would silently lose its hook capture the same way every
# non-wspy-run launch path used to before wspy-run grew this itself.
WSPY_PTS_HOOK_LOG = os.environ.get("WSPY_PTS_HOOK_LOG", "/tmp/wspy_pts_hooks.log")


def _archive_stale_pts_hooks_log(emit):
    """A non-empty staging log at this point predates whatever pass is about
    to run -- either an earlier pass in this same run (each pass fully
    re-executes the workload, so PTS's hooks -- if registered -- fire again
    every time) or a stale leftover from an interrupted earlier run that
    never reached the relocation step below. Mirrors wspy-run's run_pass()
    exactly: move it aside rather than lose or misattribute it."""
    try:
        if os.path.getsize(WSPY_PTS_HOOK_LOG) == 0:
            return
    except OSError:
        return
    stale_path = f"{WSPY_PTS_HOOK_LOG}.stale-{os.getpid()}"
    try:
        os.replace(WSPY_PTS_HOOK_LOG, stale_path)
    except OSError:
        return
    emit(f"[note] found a stale {WSPY_PTS_HOOK_LOG} predating this pass, "
         f"moved aside to {stale_path}")


def _capture_pts_hooks_log(emit, rundir, name):
    """Relocates a non-empty PTS result_notifier hook capture staging log
    into a per-pass artifact (<name>.pts_hooks.log) next to that pass's own
    output/manifest -- the same artifact shape wspy-run's own run_pass()
    produces, so a checklist-driven custom run (or a preset run's
    supplementary plot-data pass) gets identical capture whether or not it
    went through wspy-run itself. Returns the artifact's basename, or None
    if the hooks aren't registered on this host or this pass's workload
    didn't trigger them -- same measured-vs-unavailable idiom used
    throughout this codebase."""
    try:
        if os.path.getsize(WSPY_PTS_HOOK_LOG) == 0:
            return None
    except OSError:
        return None
    dest = os.path.join(rundir, f"{name}.pts_hooks.log")
    try:
        os.replace(WSPY_PTS_HOOK_LOG, dest)
    except OSError:
        return None
    emit(f"[{name}] pts hooks captured -> {os.path.basename(dest)}")
    return os.path.basename(dest)

# ---------------------------------------------------------------------------
# Item 9 (INVESTIGATION.md): the configuration/option checklist that
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
# explicit --no-<name> when left unchecked; the rest are named in one
# --counters=<list> flag when checked (wspy's --counters=<list>, the
# recommended replacement for the individual --<name> boolean flags this
# module used to emit one at a time -- see counter_group_flags() below).
# Mirrors wspy.h's COUNTER_* set and is also the exact token vocabulary
# multipass.c's multipass_group_names[] uses for --passes=<list> (which
# --counters=<list> itself reuses), so the same list drives the plain-flags,
# --counters=-based, and --passes-bin-packed branches below without a second
# table to keep in sync.
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
    (flags, selected_set) -- flags is the minimal flag list needed to reach
    exactly that selection from wspy's own defaults (ipc and software on,
    everything else off): one --counters=<list> naming every selected
    non-default-on group (wspy's own recommended replacement for the
    individual --<name> boolean flags this used to emit one at a time), plus
    a --no-<name> for any default-on group the caller left unchecked. Order
    is ALL_GROUPS' fixed order so the result is deterministic regardless of
    the client's array order."""
    selected = {g for g in (requested_groups or []) if g in GROUP_NAME_SET}
    to_enable = [name for name, default_on in ALL_GROUPS if name in selected and not default_on]
    flags = []
    if to_enable:
        flags.append(f"--counters={','.join(to_enable)}")
    for name, default_on in ALL_GROUPS:
        if name not in selected and default_on:
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
# this host, so it's matched by prefix rather than listed by name; "disk
# <dev> read"/"disk <dev> write"/"disk <dev> time" (system.c's SYSTEM_DISK,
# INVESTIGATION.md's 4.2 Tier 1 "System-wide disk I/O stats" item) are the
# same per-device shape, one device per host, matched by prefix too.
# mem_free_mb/mem_cached_mb/mem_dirty_mb/mem_writeback_mb/swap_free_mb/
# committed_as_mb (system.c's SYSTEM_MEM, INVESTIGATION.md's 4.2 Tier 1
# "System-wide memory pressure stats" item) are 6 fixed column names -- no
# per-device/per-interface variation, so unlike net/disk they're listed here
# directly rather than matched by prefix.
SYSTEM_COLUMN_NAMES = {"load", "runnable", "cpu", "idle", "iowait", "irq", "freq", "cpu_temp",
                        "mem_free_mb", "mem_cached_mb", "mem_dirty_mb", "mem_writeback_mb",
                        "swap_free_mb", "committed_as_mb"}
# --power's own columns (power.c/topdown.c's print_power()) -- same "not an
# ALL_GROUPS entry" reasoning as SYSTEM_COLUMN_NAMES above: --power isn't a
# counter_mask bit, it's a "power" checkbox inside both the "counters" and
# "system" checklist sections (build_configuration_passes() folds it into
# whichever pass it's checked in), so resolve_column_group() reports it via
# its own sentinel rather than either of those two directly.
POWER_COLUMN_NAMES = {"pkg_joules", "pkg_watts"}


def resolve_column_group(column_name):
    """Returns the ALL_GROUPS name (or the "system"/"power" sentinel) whose
    --flag must be enabled to produce column_name in a wspy CSV, or None if
    column_name isn't a column this tool recognizes (a typo, or a
    workload-specific name nothing here can auto-detect)."""
    if column_name in SYSTEM_COLUMN_NAMES or column_name.startswith("net ") or column_name.startswith("disk "):
        return "system"
    if column_name in POWER_COLUMN_NAMES:
        return "power"
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
    needs_power = False
    unresolved = set()

    for cp in (custom_plots or []):
        for col in cp.get("columns", []):
            group = resolve_column_group(col)
            if group is None:
                unresolved.add(col)
            elif group == "system":
                needs_system = True
            elif group == "power":
                needs_power = True
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

    if needs_power:
        # --power has no card of its own -- it's a checkbox inside
        # 'Performance counters' and 'System metrics' (build_configuration_
        # passes() folds it into whichever pass it's checked in, since a
        # separate power-only wspy invocation was never anything more than
        # those same two passes with only --power selected). Prefer
        # 'System metrics' when it's already in play (either this same
        # custom-plot request also needs a system column, or the caller's
        # checklist already has it enabled) -- otherwise fold into
        # 'Performance counters', auto-enabling it same as needed_groups
        # above.
        target_key = "system" if (needs_system or (checklist.get("system") or {}).get("enabled")) \
            else "counters"
        target_label = "System metrics" if target_key == "system" else "Performance counters"
        target = checklist.setdefault(target_key, {})
        if not target.get("enabled"):
            target["enabled"] = True
            notes.append(f"auto-enabled '{target_label}' for a custom plot")
        if not target.get("power"):
            target["power"] = True
            notes.append(f"auto-checked 'power' within '{target_label}' for a custom plot")
        if not str(target.get("interval_secs") or "").strip():
            target["interval_secs"] = "1"
            notes.append(f"auto-set a 1s interval on '{target_label}' so the custom plot has a "
                          "time series to chart")
        target["csv"] = True

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
    # systemtime now also collects --power (wspy-run's load_builtin_profile()),
    # so pkg_joules/pkg_watts land in the same CSV as cpu/freq -- POWER_COLUMN_NAMES
    # here, not just SYSTEM_COLUMN_NAMES, since --power is its own checklist card
    # (see resolve_column_group()'s "power" sentinel above), not a system_mask bit.
    "deep-cpu": SYSTEM_COLUMN_NAMES | POWER_COLUMN_NAMES | {"net *", "disk *",
                 "retire", "frontend", "backend", "speculate", "confidence", "sanity"},
    "deep-cpu-intel": set(),
    # systemtime now also collects --power, matching deep-cpu's own systemtime
    # pass (a pre-existing asymmetry between the two profiles, fixed --
    # INVESTIGATION.md's "What shipped in 4.2"), so POWER_COLUMN_NAMES
    # belongs here too now.
    "deep-gpu": SYSTEM_COLUMN_NAMES | POWER_COLUMN_NAMES | {"net *", "disk *",
                 "retire", "frontend", "backend", "speculate", "confidence", "sanity",
                 "gpu_busy", "gpu_temp", "gpu_activity", "gpu_power", "gpu_freq"},
    "tree-heavy": set(),
    "ibs-basic": set(),
    "ibs-memory-deep": set(),
    # gpu-compute's single pass already runs on --interval 1 (wspy-run's
    # load_builtin_profile()), unlike quick/tree-heavy/ibs-* above -- so
    # unlike those, an absent/empty entry here would be wrong, not just
    # unhelpful: build_supplementary_plot_passes() would wrongly conclude
    # none of its columns are plottable and spin up a redundant duplicate
    # pass collecting data gpu-compute's own pass already produces.
    "gpu-compute": SYSTEM_COLUMN_NAMES | POWER_COLUMN_NAMES | {
                 "net *", "disk *", "retire", "frontend", "backend", "speculate", "confidence", "sanity",
                 "gpu_busy", "gpu_temp", "gpu_activity", "gpu_power", "gpu_freq",
                 "nv_gpu_busy", "nv_vram_used_mb", "nv_vram_total_mb"},
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
            if not (c in covered or (c.startswith("net ") and "net *" in covered) or
                    (c.startswith("disk ") and "disk *" in covered)):
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


def _config_options(section):
    """Turns one checklist category's raw sub-dict (e.g. checklist["counters"])
    into the launcher-vocabulary (name,value) pairs recorded via wspy's
    --config-option (INVESTIGATION.md's "What shipped in 4.1", structured
    configuration provenance) -- the same keys/values the Run tab checklist itself uses,
    not a re-derivation from the wspy flags build_configuration_passes()
    also produces. "enabled" is omitted (implied by the pass existing at
    all); a None/empty value is omitted (nothing was chosen); a list value
    (e.g. counters' "groups") is comma-joined to match --passes=<list>'s own
    syntax; everything else is stringified as-is."""
    options = []
    for key, value in (section or {}).items():
        if key == "enabled" or value is None or value == "":
            continue
        if isinstance(value, (list, tuple)):
            if not value:
                continue
            value = ",".join(str(v) for v in value)
        elif isinstance(value, bool):
            value = "true" if value else "false"
        else:
            value = str(value)
        options.append((key, value))
    return options


# Structured configuration provenance's (INVESTIGATION.md's "What shipped
# in 4.1") launcher-vocabulary category name (recorded
# via --config-name, see build_pass_argv()) back to the Run tab checklist key
# that produced it -- the read-side counterpart to build_configuration_passes()'s
# own tree/counters/system/gpu/ibs dispatch, used by item 17's "customize &
# run again" to figure out which checklist card a given pass's
# configuration_provenance belongs to.
CATEGORY_TO_CHECKLIST_KEY = {
    "process-tree": "tree",
    "performance-counters": "counters",
    "system-metrics": "system",
    "gpu-metrics": "gpu",
    "ibs": "ibs",
    "power": "power",
}

# Which of a checklist section's own option keys are booleans (recorded as
# the literal strings "true"/"false" by _config_options() above) rather than
# plain text/list values -- needed to parse a manifest's recorded
# configuration_provenance.options back into the same {enabled, ...} shape
# buildChecklist() (web/static/app.js) produces client-side. "groups"
# (counters only) is handled separately since it's a comma-joined list, not
# a scalar.
_BOOL_OPTION_KEYS = {
    "tree": {"cmdline", "open", "futex", "io", "io_wait", "schedstat", "vmsize",
             "connect", "wait", "poll", "nanosleep", "software"},
    "counters": {"per_core", "per_core_freq", "rusage", "csv", "power"},
    "system": {"csv", "power"},
    "gpu": {"busy", "metrics", "smi", "csv"},
    "ibs": set(),
    "power": {"csv"},  # legacy: an old manifest's "power" category (before
                        # power was folded into counters/system) still
                        # round-trips through this key for backward compat.
}


def checklist_section_from_options(checklist_key, options):
    """Reverse of _config_options(): turns one pass's recorded
    configuration_provenance.options (name/value string pairs, as written by
    manifest.c's write_config_provenance()) back into that checklist
    category's sub-dict, in the same shape build_configuration_passes()
    consumes and buildChecklist() (app.js) produces. "enabled" is implied
    (the pass exists, so its configuration was on) -- not itself an
    option name recorded in provenance, see _config_options()'s own
    "enabled" skip."""
    section = {"enabled": True}
    bool_keys = _BOOL_OPTION_KEYS.get(checklist_key, set())
    for name, value in options:
        if not name:
            continue
        if checklist_key == "counters" and name == "groups":
            section[name] = [g for g in (value or "").split(",") if g]
        elif name in bool_keys:
            section[name] = (value == "true")
        else:
            section[name] = value
    return section


def checklist_from_pass_provenance(pass_provenances):
    """Aggregates a run's per-pass configuration_provenance records (each a
    {"preset","configuration","options"} dict or None, see server.py's
    read_manifest_config_provenance()) back into whichever launcher state
    actually produced the run: a preset name (wspy-run's builtin profiles,
    item 7 -- --preset-name is only ever set together with a pass's own
    --config-name, so one preset-bearing pass is enough to identify the
    whole run) or a full checklist dict (item 9's checklist-driven custom
    runs, which never set --preset-name -- see build_pass_argv()'s own
    comment). Returns (preset_or_None, checklist_or_None); both None means
    no restorable configuration_provenance was found at all (a report from
    before item 16, or a direct wspy invocation with neither flag given) --
    item 17's "customize & run again" falls back to today's
    workload/suite/benchmark-only prefill in that case."""
    for cp in pass_provenances:
        if cp and cp.get("preset"):
            return cp["preset"], None

    checklist = {}
    for cp in pass_provenances:
        if not cp:
            continue
        key = CATEGORY_TO_CHECKLIST_KEY.get(cp.get("configuration"))
        if not key:
            continue
        checklist[key] = checklist_section_from_options(key, cp.get("options") or [])
    return (None, checklist) if checklist else (None, None)


def build_configuration_passes(rundir, checklist):
    """The one place checklist state (see the item-9 comment above) becomes
    real wspy flags -- used identically by the preview endpoint and the real
    executor, so a preview is never a paraphrase of what actually runs.
    Returns a list of {"name","category","options","flags","csv","timeout"}
    dicts, in the fixed tree/counters/system/gpu/ibs order, one per *enabled
    and non-empty* configuration (an enabled configuration with nothing
    meaningful selected, e.g. "counters" with no groups checked, is silently
    skipped rather than producing a no-op wspy invocation). "category" is
    the launcher-vocabulary configuration name (item 16's structured
    configuration provenance, --config-name) -- stable across the legacy
    "amdtopdown"/"systemtime" pass-name aliases below, unlike "name" (the
    output filename stem), which is not.

    --power has no configuration/pass of its own: it's a "power" checkbox
    inside both the "counters" and "system" sections, folded into whichever
    pass it's checked in rather than issued as a separate wspy invocation --
    a standalone power-only pass was never anything more than one of these
    two passes with only --power selected, and per-core energy (power_core,
    power.c) specifically *needs* --power and --per-core in the same wspy
    process to ever produce core_joules/core_watts at all, which two
    separate passes could never do. Checking it in both sections at once
    just measures power twice (redundant, not incorrect); nothing here
    reconciles that for the caller."""
    checklist = checklist or {}
    passes = []

    tree = checklist.get("tree") or {}
    if tree.get("enabled"):
        flags = ["--tree", os.path.join(rundir, "process.tree.txt")]
        if tree.get("cmdline"):
            flags.append("--tree-cmdline")
        if tree.get("open"):
            flags.append("--tree-open")
        if tree.get("futex"):
            flags.append("--tree-futex")
        if tree.get("io"):
            flags.append("--tree-io")
        if tree.get("io_wait"):
            flags.append("--tree-io-wait")
        if tree.get("schedstat"):
            flags.append("--tree-schedstat")
        if tree.get("vmsize"):
            flags.append("--tree-vmsize")
        if tree.get("connect"):
            flags.append("--tree-connect")
        if tree.get("wait"):
            flags.append("--tree-wait")
        if tree.get("poll"):
            flags.append("--tree-poll")
        if tree.get("nanosleep"):
            flags.append("--tree-nanosleep")
        # software is on by default, so enabling it needs no flag at all;
        # only the off case needs an explicit --no-software (--counters=
        # is purely additive -- there's no way to turn a default-on group
        # off through it, same reasoning as counter_group_flags() below).
        if not tree.get("software", True):
            flags.append("--no-software")
        flags.append("--no-ipc")
        timeout = parse_optional_int(tree.get("timeout_secs"), 1, 86400)
        passes.append({"name": "tree", "category": "process-tree",
                        "options": _config_options(tree),
                        "flags": flags, "csv": False, "timeout": timeout})

    counters = checklist.get("counters") or {}
    if counters.get("enabled"):
        group_flags, selected = counter_group_flags(counters.get("groups"))
        power_wanted = bool(counters.get("power"))
        if selected or power_wanted:
            interval = parse_optional_int(counters.get("interval_secs"), 1, 3600)
            per_core = bool(counters.get("per_core"))
            # --per-core + --interval now produces one CSV row per core per
            # tick (wspy.c's per_core_csv/timer_callback(), fixed after this
            # checkbox originally shipped -- see CLAUDE.md's wspy.c entry),
            # and wspy-core-report already collapses multiple rows per core
            # via a mean, so there's no longer any reason to force aggregate
            # here; the interval field is honored exactly like every other
            # card's.
            per_core_freq = bool(counters.get("per_core_freq")) and per_core
            rusage_on = bool(counters.get("rusage"))
            csv = bool(counters.get("csv", True))
            # --passes rejects --interval/--per-core/--power outright (wspy.c,
            # see CLAUDE.md's wspy.c entry) -- so interval mode, a per-core
            # request, or a power checkbox, always uses plain flags
            # (potentially multiplexed by the kernel across >1 group, same as
            # any ordinary wspy invocation would be); aggregate mode with no
            # per-core/power only needs --passes' bin-packing once >=2 groups
            # are requested, since a single group never multiplexes against
            # itself.
            if interval is None and len(selected) >= 2 and not power_wanted and not per_core:
                ordered = [n for n in GROUP_NAMES if n in selected]
                flags = [f"--passes={','.join(ordered)}"]
            else:
                flags = list(group_flags)
                if interval is not None:
                    flags += ["--interval", str(interval)]
                if per_core:
                    flags.append("--per-core")
                if per_core_freq:
                    flags.append("--per-core-freq")
                if power_wanted:
                    flags.append("--power")
            flags.append("--rusage" if rusage_on else "--no-rusage")
            if csv:
                flags.append("--csv")
            # Reuse the well-known "amdtopdown" name for exactly the case
            # that name always meant elsewhere in this codebase: an
            # interval, CSV, topdown-only sweep (never power-folded, since
            # that name predates this checkbox and shouldn't silently start
            # meaning something wider). wspy-plot (item 12) matches
            # templates against a CSV's header, not its filename, so this
            # naming is now just continuity with older reports, not a
            # requirement for plotting to fire.
            name = "amdtopdown" if (interval is not None and csv and selected == {"topdown"}
                                     and not power_wanted) else "counters"
            passes.append({"name": name, "category": "performance-counters",
                            "options": _config_options(counters),
                            "flags": flags, "csv": csv, "timeout": None})

    system = checklist.get("system") or {}
    if system.get("enabled"):
        interval = parse_optional_int(system.get("interval_secs"), 1, 3600)
        csv = bool(system.get("csv", True))
        power_wanted = bool(system.get("power"))
        flags = ["--system", "--no-ipc", "--no-rusage", "--no-software"]
        if interval is not None:
            flags += ["--interval", str(interval)]
        if power_wanted:
            flags.append("--power")
        if csv:
            flags.append("--csv")
        # Same reasoning as "amdtopdown" above -- kept for continuity with
        # older reports, not because wspy-plot needs this literal filename;
        # never used once power's folded in, same as "amdtopdown" above.
        name = "systemtime" if (interval is not None and csv and not power_wanted) else "system"
        passes.append({"name": name, "category": "system-metrics",
                        "options": _config_options(system),
                        "flags": flags, "csv": csv, "timeout": None})

    gpu = checklist.get("gpu") or {}
    if gpu.get("enabled"):
        backend_flags = []
        if gpu.get("busy"):
            backend_flags.append("--gpu-busy")
        if gpu.get("metrics"):
            backend_flags.append("--gpu-metrics")
        if gpu.get("smi"):
            backend_flags.append("--gpu-smi")
        if gpu.get("nvidia"):
            backend_flags.append("--gpu-nvidia")
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
            passes.append({"name": "gpu", "category": "gpu-metrics",
                            "options": _config_options(gpu),
                            "flags": flags, "csv": csv, "timeout": None})

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
        interval = parse_optional_int(ibs.get("interval_secs"), 1, 3600)
        if interval is not None:
            flags += ["--interval", str(interval)]
        csv = bool(ibs.get("csv", True))
        if csv:
            flags.append("--csv")
        passes.append({"name": "ibs", "category": "ibs",
                        "options": _config_options(ibs),
                        "flags": flags, "csv": csv, "timeout": None})

    return passes


def build_pass_argv(wspy_bin, rundir, p, manifest_on, run_index_path, affinity=None):
    """Full argv for one configuration pass, minus the trailing `-- <workload
    argv>` (appended by the caller, which also decides whether to prefix a
    `timeout <secs>` wrapper) -- mirrors wspy-run's own run_pass() shape:
    <pass-name>.<csv|txt> for output, <pass-name>.manifest.json alongside it
    when manifest recording is on. Also threads p's "category"/"options"
    (see build_configuration_passes()) through as --config-name/
    --config-option -- structured configuration provenance
    (INVESTIGATION.md's "What shipped in 4.1"), the checklist's own vocabulary rather
    than wspy's flags, recorded in the pass's manifest/run-index regardless
    of whether manifest_on/run_index_path are set for *this* pass (it's
    cheap metadata, not gated on those toggles the way the file paths are).
    There's no --preset-name here -- unlike wspy-run's builtin profiles
    (see wspy-run's own run_pass()), a checklist-driven run has no named
    preset by definition; "category" alone is the provenance this path can
    truthfully record."""
    ext = "csv" if p["csv"] else "txt"
    outfile = os.path.join(rundir, f'{p["name"]}.{ext}')
    argv = [wspy_bin] + p["flags"] + ["-o", outfile]
    # Core/thread affinity control: applies to every pass alike (it's a
    # placement decision, not a per-configuration option), same as --manifest/
    # --run-index below. "all" is the implicit default and never needs the flag.
    if affinity and affinity != "all":
        argv += ["--affinity", affinity]
    if p.get("category"):
        argv += ["--config-name", p["category"]]
    for name, value in p.get("options") or []:
        argv += ["--config-option", f"{name}={value}"]
    manifest_path = None
    if manifest_on:
        manifest_path = os.path.join(rundir, f'{p["name"]}.manifest.json')
        argv += ["--manifest", manifest_path]
    if run_index_path:
        argv += ["--run-index", run_index_path]
    return argv, outfile, manifest_path


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


def parse_run_key(key):
    """Parses a "<suite>/<benchmark>/<run_id>" key -- the shape /compare's
    checkbox form (name="r") and the tree-diff view both use to
    identify a run -- into (suite, benchmark, run_id), or None if it isn't
    exactly three valid_segment()-passing components. Factored out of
    render_compare()'s own inline key-splitting so a second call site
    (the tree-diff view) doesn't need a third copy of the same check."""
    segs = key.split("/")
    if len(segs) != 3 or not all(valid_segment(s) for s in segs):
        return None
    return tuple(segs)


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


def valid_profile_spec(spec):
    tokens = spec.split(",")
    return bool(tokens) and all(PROFILE_TOKEN_RE.match(t) for t in tokens)


def build_wspy_run_argv(wspy_run_bin, wspy_bin, output_root, suite, benchmark,
                         run_id, profile, workload_argv, run_index_path=None,
                         affinity=None):
    # wspy-run always writes each pass's manifest into the run directory once
    # --suite/--benchmark select the unified layout (MANIFEST_DIR defaults to
    # RUNROOT unconditionally there's no flag to opt back out) -- so unlike
    # the custom checklist path, there's no "manifest off" toggle to thread
    # through here; only --run-index is actually optional.
    argv = [wspy_run_bin, "--wspy", wspy_bin, "-o", output_root,
            "--suite", suite, "--benchmark", benchmark, "--run-id", run_id]
    if run_index_path:
        argv += ["--run-index", run_index_path]
    # Core/thread affinity control: wspy-run's own --affinity passes the spec
    # through to every pass's wspy invocation (see wspy-run's run_pass()) --
    # this is a single flag on the wspy-run invocation itself, not something
    # that decomposes the preset, so it's compatible with the deep-dive's
    # "a preset stays atomic" rule the same way --manifest-dir/--run-index
    # already are. "all" is the implicit default and never needs the flag.
    if affinity and affinity != "all":
        argv += ["--affinity", affinity]
    argv += [profile, "--"] + workload_argv
    return argv


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


def build_proctree_argv(proctree_bin, tree_txt_path, cmdline=False, futex=False, io=False, io_wait=False,
                         schedstat=False, vmsize=False, connect=False, wait=False, poll=False, nanosleep=False):
    """proctree, applied to a --tree pass's raw process.tree.txt record --
    the same "run the tool automatically" treatment wspy-plot already gets
    for CSVs (see build_plot_argv() above), added once a real ~155K-process
    stress run made clear a raw tree file is too large to eyeball by hand.
    -C mirrors whether this run's tree pass actually requested
    --tree-cmdline (proctree's own default is the *narrower* abbreviated-
    command shape, so the reconstructed tree would otherwise silently drop
    detail the raw file actually carries). -M/-N/-P (vmsize+rss/thread
    count/ppid) are always passed, unconditionally: unlike cmdline, that
    data isn't gated by any wspy flag at all -- /proc/<pid>/stat is dumped
    in full on every exit regardless of any wspy flag, so there's nothing to
    condition on -- the fields are simply always in the raw file, and
    proctree's own defaults just don't print them without asking. -X/-B/-I/
    -D/-R/-K/-J/-L/-Z mirror -C's own conditional treatment, not -M/-N/-P's
    unconditional one: futex/io-wait/io-byte-counter/run-queue-delay/
    peak-RSS-and-RSS-composition-and-swap/connect-latency/wait-blocking-time/
    poll-blocking-time/nanosleep-time data is only in the raw file at all if
    this run's tree pass requested --tree-futex/--tree-io-wait/--tree-io/
    --tree-schedstat/--tree-vmsize/--tree-connect/--tree-wait/--tree-poll/
    --tree-nanosleep respectively (--tree-vmsize used to be a no-op on the
    wspy side -- it now drives -R's data, see wspy.c/topdown.c; -M/-N/-P's
    own vmsize+rss/thread-count/ppid fields come from a different,
    always-present source, /proc/<pid>/stat, and are unaffected by this)."""
    argv = [proctree_bin]
    if cmdline:
        argv.append("-C")
    argv += ["-M", "-N", "-P"]
    if futex:
        argv.append("-X")
    if io_wait:
        argv.append("-B")
    if io:
        argv.append("-I")
    if schedstat:
        argv.append("-D")
    if vmsize:
        argv.append("-R")
    if connect:
        argv.append("-K")
    if wait:
        argv.append("-J")
    if poll:
        argv.append("-L")
    if nanosleep:
        argv.append("-Z")
    argv.append(tree_txt_path)
    return argv


def build_proctree_json_argv(proctree_bin, tree_txt_path):
    """proctree --json (INVESTIGATION.md 4.2 Tier 1, "proctree JSON export +
    interactive viewer + run-to-run diff"): emits one JSON document (tree +
    per-comm summary) instead of the text views build_proctree_argv() above
    produces, for the web viewer's on-demand /api/tree-json endpoint to
    consume directly."""
    return [proctree_bin, "--json", tree_txt_path]


def build_proctree_diff_argv(proctree_bin, json_a_path, json_b_path):
    """proctree --diff --json (run-to-run tree diff, same item): both
    arguments must already be --json-exported files (see
    build_proctree_json_argv() above), not raw process.tree.txt -- JSON is
    the one interchange format both the diff and the viewer consume."""
    return [proctree_bin, "--diff", "--json", json_a_path, json_b_path]


def run_proctree_besteffort(emit, cfg, rundir, cmdline=False, futex=False, io=False, io_wait=False,
                             schedstat=False, vmsize=False, connect=False, wait=False, poll=False, nanosleep=False):
    """Best-effort trailing step mirroring the wspy-plot step (build_plot_argv()
    above) but for --tree's raw process.tree.txt record: renders it into two
    human-readable views -- process.tree.summary.txt (every annotation this
    run's tree pass actually captured, via the cmdline/futex/... kwargs above,
    same as before) and process.tree.simple.txt (proctree's own bare default:
    just cpu=/start=/finish= per process, no other flags -- easier to read as
    a pure process hierarchy once a run's tree pass captures enough
    annotations that the summary view gets visually busy). A no-op (not an
    error) when no --tree pass ran this time, or its output is missing/empty
    (e.g. a --tree pass that timed out before writing anything) -- and never
    fails the run itself, same degrade-don't-fail idiom as the plot step."""
    tree_txt = os.path.join(rundir, "process.tree.txt")
    if not (os.path.isfile(tree_txt) and os.path.getsize(tree_txt) > 0):
        return

    def _run(out_name, argv):
        out_path = os.path.join(rundir, out_name)
        emit("$ " + shell_preview(argv) + f" > {out_name}")
        try:
            with open(out_path, "w") as outf:
                proc = subprocess.run(argv, cwd=REPO_ROOT, stdout=outf,
                                       stderr=subprocess.PIPE, text=True)
            if proc.returncode != 0:
                emit(f"[error] proctree exited {proc.returncode}: {proc.stderr.strip()}")
            else:
                emit(f"[wrote {out_name}]")
        except OSError as e:
            emit(f"[error] failed to launch proctree ({cfg['proctree_bin']}): {e}")

    summary_argv = build_proctree_argv(cfg["proctree_bin"], tree_txt, cmdline=cmdline, futex=futex, io=io,
                                        io_wait=io_wait, schedstat=schedstat, vmsize=vmsize, connect=connect,
                                        wait=wait, poll=poll, nanosleep=nanosleep)
    _run("process.tree.summary.txt", summary_argv)

    # Deliberately not build_proctree_argv() -- it always adds -M/-N/-P
    # unconditionally, which is exactly the annotation this leaner view
    # exists to omit. proctree's own bare invocation (no flags at all)
    # already prints just cpu=/start=/finish= per process (its only
    # default-on field is start=/finish=, everything else defaults off).
    simple_argv = [cfg["proctree_bin"], tree_txt]
    _run("process.tree.simple.txt", simple_argv)


def shell_preview(argv, cwd=None):
    s = shlex.join(argv)
    if cwd:
        return f"(cd {shlex.quote(cwd)} && {s})"
    return s


def run_sync(argv, cwd=None, timeout=120):
    """Runs a short-lived discovery/report command (wspy --capabilities,
    wspy-validate, wspy-store, wspy-summary, phoronix-test-suite info) to
    completion and captures its combined output -- unlike a launched
    workload, none of these have unbounded runtime or need live streaming,
    so a plain synchronous subprocess call (no RunState/SSE machinery) is
    the right amount of plumbing. Returns (returncode_or_None, output_text,
    timed_out). Shared by web/server.py's Discovery-tab checks and
    scripts/estimate_tree_timeout.py (INVESTIGATION.md's "Size wspy-run's
    --tree pass timeout" item)."""
    try:
        proc = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, False
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or ""), True
    except OSError as e:
        return None, f"[error] failed to launch {argv[0]}: {e}", False


# ---------------------------------------------------------------------------
# Phoronix runtime estimation -- originally the web launcher's "Estimated
# runtime display" Check-button feature (INVESTIGATION.md's 4.2 item 18),
# moved here (from server.py) so scripts/estimate_tree_timeout.py can reuse
# the identical, already-validated parsing/estimation logic for a second
# purpose -- sizing wspy-run's --tree pass timeout (INVESTIGATION.md's 4.2
# "Size wspy-run's --tree pass timeout from an actual run-time estimate"
# item) -- rather than a second, independently-drifting reimplementation of
# the same text parsing in bash (wspy-run has never shelled out to an
# external tool/parsed its text output; this stays Python-side instead of
# teaching bash to do it for the first time).
# ---------------------------------------------------------------------------

# Subcommands that take one or more test/suite names as trailing positional
# arguments -- batch-run is what wspy-run/workload/phoronix/run_test.sh
# actually uses; run/benchmark are the same shape for an ad hoc invocation.
PHORONIX_RUN_SUBCOMMANDS = ("batch-run", "run", "benchmark")
PHORONIX_MAX_TESTS_CHECKED = 5  # a handful is plenty for an on-page check; batch-run rarely lists more

# `phoronix-test-suite build-suite` lets a user hand-pick a subset of a real
# test's option combinations into a local suite, by convention (not enforced
# by phoronix-test-suite itself) named "<real-test>-subset" -- see
# workload/phoronix/backlog.txt for the canonical test names this is built
# from. `phoronix-test-suite info` only knows about real OpenBenchmarking
# test profiles, not these local suites, so an unresolved "-subset" name
# reports "no such test" instead of a usable estimate. Stripping the suffix
# resolves it to the real profile's estimate/measured time -- an overestimate
# for the subset (it covers fewer option combinations) but far more useful
# than none at all.
PHORONIX_SUBSET_SUFFIX = "-subset"


def resolve_phoronix_subset_name(name):
    """Returns (name to query via `phoronix-test-suite info`, whether `name`
    was a "-subset" suite resolved to its underlying real test)."""
    if name.endswith(PHORONIX_SUBSET_SUFFIX) and len(name) > len(PHORONIX_SUBSET_SUFFIX):
        return name[:-len(PHORONIX_SUBSET_SUFFIX)], True
    return name, False


def parse_phoronix_test_names(workload):
    """If workload looks like a `phoronix-test-suite <run-subcommand> <test>
    [<test> ...]` invocation, returns the list of test name tokens (argv
    after the subcommand, skipping anything that looks like a flag); else
    []. Best-effort argv parsing via shlex -- an unparseable command string
    (unbalanced quotes) just yields no match rather than raising, since this
    is advisory, not something that gates a run."""
    try:
        tokens = shlex.split(workload or "")
    except ValueError:
        return []
    if len(tokens) < 3:
        return []
    if os.path.basename(tokens[0]) != "phoronix-test-suite":
        return []
    if tokens[1] not in PHORONIX_RUN_SUBCOMMANDS:
        return []
    return [t for t in tokens[2:] if not t.startswith("-")]


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
_DURATION_RE = re.compile(
    r"(?:(\d+)\s*Hours?)?[,\s]*(?:(\d+)\s*Minutes?)?[,\s]*(?:(\d+(?:\.\d+)?)\s*Seconds?)?",
    re.IGNORECASE)


def _parse_duration_seconds(text):
    """'132 Seconds' / '2 Minutes, 12 Seconds' / '1 Hour, 3 Minutes' -> float
    seconds, or None if nothing matched -- phoronix-test-suite's own
    human-readable duration formatting, not a fixed unit."""
    if not text:
        return None
    m = _DURATION_RE.search(text)
    if not m or not any(m.groups()):
        return None
    hours, minutes, seconds = (float(g) if g else 0.0 for g in m.groups())
    return hours * 3600 + minutes * 60 + seconds


def parse_phoronix_info_fields(output):
    """Parses `phoronix-test-suite info`'s "Label: value" text report into a
    dict keyed by label text (ANSI color codes stripped; phoronix
    right-pads values with spaces for column alignment, so both sides are
    stripped). Not a full parse of the whole report (change history,
    OpenBenchmarking stats, ...) -- only the handful of fields this check
    cares about happen to be simple "Label: value" lines, which is all this
    needs."""
    fields = {}
    for raw_line in _ANSI_RE.sub("", output or "").splitlines():
        m = re.match(r"^([A-Za-z][A-Za-z0-9 /\-.]*):\s+(.*)$", raw_line)
        if not m:
            continue
        fields[m.group(1).strip()] = m.group(2).strip()
    return fields


def estimate_phoronix_runtime(fields):
    """Applies the "check" button's runtime-source rule (INVESTIGATION.md
    item 18's original design note): not installed, or installed but never
    run -> the profile's own generic estimate; installed and already run at
    least once on this host -> the host's own measured average, a better
    estimate than the generic one once it exists."""
    installed = fields.get("Test Installed") == "Yes"
    times_run = fields.get("Times Run")
    has_run = installed and times_run not in (None, "", "0")
    if has_run:
        text = fields.get("Average Run-Time") or fields.get("Latest Run-Time")
        return {
            "source": "measured",
            "text": text,
            "seconds": _parse_duration_seconds(text),
            "detail": f"measured average from {times_run} prior run(s) on this host",
        }
    text = fields.get("Estimated Run-Time")
    if installed:
        detail = "installed but never run yet on this host -- using phoronix-test-suite's own estimate"
    else:
        detail = ("not installed yet -- using phoronix-test-suite's own estimate "
                  "(install time not included)")
    return {
        "source": "installed-not-run" if installed else "not-installed",
        "text": text,
        "seconds": _parse_duration_seconds(text),
        "detail": detail,
    }


def estimate_phoronix_workload_seconds(workload, phoronix_bin="phoronix-test-suite",
                                        max_tests=PHORONIX_MAX_TESTS_CHECKED, cwd=None):
    """Given a full workload command string, detects a `phoronix-test-suite
    <run-subcommand> <test...>` invocation and returns
    {"tests": [...], "total_seconds": float_or_None, "truncated": bool} --
    the general-purpose estimation orchestration shared by web/server.py's
    Check-button ("Estimated runtime display") and
    scripts/estimate_tree_timeout.py. "total_seconds" is only populated when
    every checked test's own estimate resolved (a partial sum would be
    misleading, not just incomplete, same as the Check button's own rule);
    each per-test dict mirrors what the Check button already surfaces
    (name/command/estimate/error), so server.py's own JSON response shape
    doesn't need to change when it switches to calling this instead of its
    former inline copy of the same loop."""
    test_names = parse_phoronix_test_names(workload)
    if not test_names:
        return {"tests": [], "total_seconds": None, "truncated": False}

    checked_names = test_names[:max_tests]
    tests = []
    total_seconds = 0.0
    total_known = True
    for name in checked_names:
        query_name, is_subset = resolve_phoronix_subset_name(name)
        argv = [phoronix_bin, "info", query_name]
        rc, output, timed_out = run_sync(argv, cwd=cwd, timeout=30)
        entry = {"name": name, "command": shell_preview(argv)}
        if is_subset:
            entry["queried_name"] = query_name
        if timed_out:
            entry["error"] = "phoronix-test-suite info timed out"
            total_known = False
        elif rc is None:
            entry["error"] = f"failed to launch {phoronix_bin} -- is phoronix-test-suite installed?"
            total_known = False
        else:
            fields = parse_phoronix_info_fields(output)
            if not fields:
                entry["error"] = f"no such test, or unrecognized output (exit {rc})"
                total_known = False
            else:
                estimate = estimate_phoronix_runtime(fields)
                if is_subset:
                    estimate["detail"] = (
                        f"estimate is for the full '{query_name}' test -- '{name}' is a "
                        "build-suite subset of it, so this run should take no longer, "
                        "likely less. " + estimate["detail"])
                if estimate["seconds"] is None:
                    total_known = False
                else:
                    total_seconds += estimate["seconds"]
                entry.update({
                    "installed": fields.get("Test Installed"),
                    "times_run": fields.get("Times Run"),
                    "last_run": fields.get("Last Run"),
                    "estimated_run_time": fields.get("Estimated Run-Time"),
                    "average_run_time": fields.get("Average Run-Time"),
                    "latest_run_time": fields.get("Latest Run-Time"),
                    "estimate": estimate,
                })
        tests.append(entry)

    return {
        "tests": tests,
        "total_seconds": total_seconds if (total_known and tests) else None,
        "truncated": len(test_names) > len(checked_names),
    }


# ---------------------------------------------------------------------------
# Phoronix single-test-point suite import (INVESTIGATION.md item 26's
# front-end phase): decomposes an already-published OpenBenchmarking result,
# a suite XML already on disk, or an installed PTS test-suite into one
# minimal single-test-point suite per (test, option-combination),
# materialized under workload/phoronix/<test>/<options>/ and registered with
# `wspy-ledger --add`. Shared by wspy-phoronix-import's CLI and
# web/server.py's Phoronix tab, same "thin client, no duplicated logic"
# convention as the rest of this file.
#
# Deliberately stops here: it does not copy a materialized suite into
# ~/.phoronix-test-suite/test-suites/local/ or run it, and does not install
# anything -- that's item 26's separately-scoped "runner script" half. A
# per-test "installed" flag (via `phoronix-test-suite info`, same field
# estimate_phoronix_runtime() above already relies on) is surfaced so a
# human knows which materialized points still need `phoronix-test-suite
# install` run by hand.
#
# Two real Phoronix XML shapes feed this, confirmed against files on this
# machine (2026-07-23 investigation) rather than assumed:
#   - suite-definition shape: root <PhoronixTestSuite> with one or more
#     <Execute><Test>pts/name-1.2.3</Test>[<Arguments>...</Arguments>]
#     </Execute> children -- what's installed at
#     ~/.phoronix-test-suite/test-suites/{pts,local}/<name>/
#     suite-definition.xml, and also what OpenBenchmarking's own "Download
#     Suite" result-page export produces.
#   - result/composite shape: root <PhoronixTestSuite> with one or more
#     <Result><Identifier>pts/name-1.2.3</Identifier>
#     [<Arguments>...</Arguments>]...<Data>...</Result> children -- this is
#     composite.xml, phoronix-test-suite's own raw per-run result format.
#     Confirmed live: running `phoronix-test-suite info <id>` (or any
#     [Test Result]-accepting subcommand) against a real OpenBenchmarking
#     result ID transparently downloads and caches this file at
#     ~/.phoronix-test-suite/test-results/<id>/composite.xml as a
#     non-interactive side effect -- no prompts, unlike
#     `result-file-to-suite`'s own interactive suite-building UI.
# Both shapes carry the same information at the granularity this needs (one
# entry per test+option-combination), so parse_phoronix_xml_test_points()
# below just looks for both element types rather than sniffing which shape
# a given source is -- a file only ever has one of the two anyway.
# ---------------------------------------------------------------------------

def phoronix_user_data_dir():
    """~/.phoronix-test-suite, or $PTS_USER_PATH -- same resolution rule as
    web/server.py's phoronix_user_config_path(), duplicated here (rather
    than imported) since this file must stay importable without server.py
    (wspy-queue's own reason for existing). Public (not underscore-
    prefixed): also used directly by wspy-phoronix-import and
    web/server.py to locate the installed test-suites directory."""
    return os.environ.get("PTS_USER_PATH") or os.path.join(os.path.expanduser("~"), ".phoronix-test-suite")


def list_installed_phoronix_suites():
    """Suite names (e.g. "pts/compression-1.1.4") found under
    ~/.phoronix-test-suite/test-suites/{pts,local}/*/suite-definition.xml,
    sorted -- backs both wspy-phoronix-import's --list-installed and the
    Phoronix tab's installed-suite dropdown."""
    base = os.path.join(phoronix_user_data_dir(), "test-suites")
    names = []
    for prefix in ("pts", "local"):
        prefix_dir = os.path.join(base, prefix)
        if not os.path.isdir(prefix_dir):
            continue
        for entry in sorted(os.listdir(prefix_dir)):
            if os.path.isfile(os.path.join(prefix_dir, entry, "suite-definition.xml")):
                names.append(f"{prefix}/{entry}")
    return names


def resolve_installed_phoronix_suite(name):
    """Resolves a bare or prefixed installed-suite name to its
    suite-definition.xml path, searching both pts/ and local/ when no
    prefix is given. Returns (path, matched_name) or (None, None)."""
    base = os.path.join(phoronix_user_data_dir(), "test-suites")
    candidates = [name] if "/" in name else [f"pts/{name}", f"local/{name}"]
    for candidate in candidates:
        path = os.path.join(base, candidate, "suite-definition.xml")
        if os.path.isfile(path):
            return path, candidate
    return None, None


def parse_openbenchmarking_id(result_ref):
    """Accepts either a bare OpenBenchmarking result ID
    ("2607160-PTS-7700X3D886") or a full result URL
    (https://openbenchmarking.org/result/<id>[?...]) and returns the bare
    ID. Not validated against OpenBenchmarking's real ID grammar -- an
    unrecognized string is returned stripped of any query/fragment and just
    fails later, at the fetch step, with a clear error instead of here."""
    ref = (result_ref or "").strip()
    m = re.search(r"openbenchmarking\.org/result/([^/?&#]+)", ref)
    if m:
        return m.group(1)
    return re.split(r"[?&#]", ref, maxsplit=1)[0]


def fetch_openbenchmarking_suite_xml(result_ref, phoronix_bin="phoronix-test-suite", cwd=None, timeout=60):
    """Resolves result_ref (a result URL or bare ID) to suite/result XML
    bytes, trying two paths in order:

    1. Direct fetch of the small "Download Suite" export
       (https://openbenchmarking.org/result/<id>?export=xml-suite) -- fast,
       the same file a browser's "Download Suite" link gives, no
       phoronix-test-suite dependency. Treated as failed (falls through to
       #2) on any network error or a response that doesn't actually look
       like Phoronix XML -- OpenBenchmarking sits behind Cloudflare, whose
       bot-management can return an HTML challenge page instead of the
       export depending on the requesting network's reputation (confirmed
       from this codebase's own dev sandbox, which the challenge blocks
       outright; unconfirmed whether a normal residential/office network
       ever hits it).
    2. `phoronix-test-suite info <id>` -- phoronix-test-suite's own network
       path, confirmed live to transparently download and cache the full
       composite.xml result file (see module-level comment above) as a
       side effect of any [Test Result]-accepting subcommand, non-
       interactively. Slower and a much bigger file than #1, but doesn't
       depend on OpenBenchmarking's export URL/Cloudflare posture and only
       needs phoronix-test-suite itself to reach the network.

    Returns {"xml": bytes, "source_kind": "openbenchmarking-direct" or
    "openbenchmarking-pts-cache", "source_ref": str} on success, or
    {"error": str} if both paths fail."""
    result_id = parse_openbenchmarking_id(result_ref)
    if not result_id:
        return {"error": f"could not determine an OpenBenchmarking result ID from {result_ref!r}"}

    direct_url = f"https://openbenchmarking.org/result/{result_id}?export=xml-suite"
    try:
        req = urllib.request.Request(direct_url, headers={"User-Agent": "wspy-phoronix-import"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
        stripped = body.lstrip()
        if stripped.startswith(b"<?xml") or stripped.startswith(b"<PhoronixTestSuite"):
            return {"xml": body, "source_kind": "openbenchmarking-direct", "source_ref": direct_url}
    except (urllib.error.URLError, OSError):
        pass  # fall through to the phoronix-test-suite path below

    argv = [phoronix_bin, "info", result_id]
    rc, output, timed_out = run_sync(argv, cwd=cwd, timeout=timeout)
    if timed_out:
        return {"error": f"direct fetch failed, and {shell_preview(argv)} timed out"}
    if rc is None:
        return {"error": f"direct fetch failed, and failed to launch {phoronix_bin} -- "
                          "is phoronix-test-suite installed?"}
    composite_path = os.path.join(phoronix_user_data_dir(), "test-results", result_id, "composite.xml")
    if not os.path.isfile(composite_path):
        return {"error": f"direct fetch failed, and {shell_preview(argv)} (exit {rc}) did not "
                          f"produce {composite_path} -- check the result ID; output: "
                          f"{output.strip()[:300]}"}
    with open(composite_path, "rb") as f:
        return {"xml": f.read(), "source_kind": "openbenchmarking-pts-cache", "source_ref": composite_path}


def parse_phoronix_xml_test_points(xml_bytes):
    """Decomposes suite-definition or result/composite Phoronix XML (see
    the module-level comment above for both shapes) into a deduped, order-
    preserving list of {"test_id": "pts/name-1.2.3", "arguments": "..."}
    dicts, one per distinct (test, option-combination) pair. Raises
    xml.etree.ElementTree.ParseError on unparseable input -- callers decide
    how to surface that."""
    root = ET.fromstring(xml_bytes)
    points = []
    seen = set()

    def add(test_id, arguments):
        test_id = (test_id or "").strip()
        arguments = (arguments or "").strip()
        if not test_id:
            return
        key = (test_id, arguments)
        if key in seen:
            return
        seen.add(key)
        points.append({"test_id": test_id, "arguments": arguments})

    for execute in root.iter("Execute"):
        test_el = execute.find("Test")
        args_el = execute.find("Arguments")
        add(test_el.text if test_el is not None else None,
            args_el.text if args_el is not None else None)

    for result in root.iter("Result"):
        # .find() (not .iter()) only looks at Result's direct children, so
        # this doesn't pick up the unrelated per-system-hardware
        # <Data><Entry><Identifier> nested two levels deeper in the same
        # <Result> block.
        id_el = result.find("Identifier")
        args_el = result.find("Arguments")
        add(id_el.text if id_el is not None else None,
            args_el.text if args_el is not None else None)

    return points


def _phoronix_looks_like_version(s):
    """Mirrors ledger.c's looks_like_version(): digits and '.' only, at
    least one digit."""
    if not s:
        return False
    has_digit = False
    for ch in s:
        if ch == ".":
            continue
        if not ch.isdigit():
            return False
        has_digit = True
    return has_digit


def phoronix_bare_test_name(test_id):
    """"pts/blender-1.2.1" -> "blender", "system/selenium-1.0.47" ->
    "selenium" -- strips the suite-namespace prefix (pts/, system/, ...)
    and the trailing "-<version>" every PTS test-profile directory carries,
    mirroring ledger.c's strip_version_suffix() (same rule -- only strip
    the text after the last '-' if it actually looks like a version --
    just in Python, since strip_version_suffix() itself isn't exposed
    outside ledger.c)."""
    name = test_id.split("/", 1)[1] if "/" in test_id else test_id
    last_dash = name.rfind("-")
    if last_dash != -1 and _phoronix_looks_like_version(name[last_dash + 1:]):
        name = name[:last_dash]
    return name


_PHORONIX_SLUG_RE = re.compile(r"[^a-z0-9]+")
_PHORONIX_SLUG_MAX = 60
_PHORONIX_SLUG_HASH_LEN = 8


def slugify_phoronix_arguments(arguments):
    """Filesystem/ledger-safe slug for a test point's Arguments string --
    "" (a test with no options) becomes "default"; anything else is
    lowercased with every run of non-alphanumeric characters collapsed to a
    single '-'. Not reversible -- the verbatim Arguments string is always
    additionally kept in the generated suite-definition.xml and
    source.json, so this is only ever a directory name, never the source
    of truth.

    A slug longer than _PHORONIX_SLUG_MAX gets a short hash of the
    *untruncated* text appended rather than being silently cut off --
    confirmed live (2026-07-23) against a real OpenVINO result that a
    plain truncation collapses two genuinely different option combinations
    into one slug when they share a long common prefix and differ only
    near the end: OpenVINO's "-hint throughput" vs "-hint latency"
    variants share an ~83-character "-m models/intel/<model>/..." prefix,
    so a bare 60-char cut discarded exactly the "throughput"/"latency"
    text that distinguishes them, silently dropping half of every
    OpenVINO test point materialize_phoronix_test_point() saw ("already
    exists" instead of a second real directory). The hash guarantees two
    different Arguments strings never collide here, regardless of where
    their difference falls."""
    text = (arguments or "").strip()
    if not text:
        return "default"
    slug = _PHORONIX_SLUG_RE.sub("-", text.lower()).strip("-")
    if not slug:
        return "default"
    if len(slug) <= _PHORONIX_SLUG_MAX:
        return slug
    digest = hashlib.sha1(text.encode("utf-8", errors="replace")).hexdigest()[:_PHORONIX_SLUG_HASH_LEN]
    prefix_len = _PHORONIX_SLUG_MAX - _PHORONIX_SLUG_HASH_LEN - 1
    return f"{slug[:prefix_len].strip('-')}-{digest}"


def _build_phoronix_suite_xml(identity, test_id, arguments, source_ref):
    root = ET.Element("PhoronixTestSuite")
    info = ET.SubElement(root, "SuiteInformation")
    ET.SubElement(info, "Title").text = identity
    ET.SubElement(info, "Version").text = "1.0.0"
    ET.SubElement(info, "TestType").text = "Processor"
    ET.SubElement(info, "Description").text = (
        f"Single-test-point suite generated by wspy-phoronix-import from {source_ref}.")
    ET.SubElement(info, "Maintainer").text = "wspy"
    execute = ET.SubElement(root, "Execute")
    ET.SubElement(execute, "Test").text = test_id
    if arguments:
        ET.SubElement(execute, "Arguments").text = arguments
    ET.indent(root, space="  ")
    return b'<?xml version="1.0"?>\n' + ET.tostring(root, encoding="utf-8") + b"\n"


def materialize_phoronix_test_point(point, dest_root, source_kind, source_ref, installed=None):
    """Writes one minimal single-test-point suite-definition.xml (plus a
    source.json provenance sidecar) for `point` (a {"test_id",
    "arguments"} dict from parse_phoronix_xml_test_points()) under
    dest_root/<bare-test-name>/<options-slug>/ -- the layout
    INVESTIGATION.md item 26 specifies. Returns a dict:
      {"test_id", "arguments", "bare_name", "options_slug", "identity",
       "dir", "status": "created" or "exists", "installed"}

    "identity" ("<bare_name>-<options_slug>") is deliberately also what a
    future copy-into-~/.phoronix-test-suite/test-suites/local/ step would
    name the runnable local suite, so wspy-ledger's substring-against-run-
    index-command matching (ledger.c's command_matches()) lines up
    automatically once that later phase exists, with no renaming.

    Idempotent/additive: if <dir>/suite-definition.xml already exists, this
    leaves it untouched and reports "exists" instead of overwriting -- the
    "additive across sessions" reuse check item 26's own text calls for,
    the same idiom item 24's resume-check design note uses."""
    test_id = point["test_id"]
    arguments = point.get("arguments", "")
    bare_name = phoronix_bare_test_name(test_id)
    options_slug = slugify_phoronix_arguments(arguments)
    identity = f"{bare_name}-{options_slug}"
    out_dir = os.path.join(dest_root, bare_name, options_slug)
    suite_path = os.path.join(out_dir, "suite-definition.xml")

    result = {
        "test_id": test_id, "arguments": arguments, "bare_name": bare_name,
        "options_slug": options_slug, "identity": identity, "dir": out_dir,
        "installed": installed,
    }
    if os.path.isfile(suite_path):
        result["status"] = "exists"
        return result

    os.makedirs(out_dir, exist_ok=True)
    with open(suite_path, "wb") as f:
        f.write(_build_phoronix_suite_xml(identity, test_id, arguments, source_ref))
    with open(os.path.join(out_dir, "source.json"), "w") as f:
        json.dump({
            "schema_version": 1,
            "source_kind": source_kind,
            "source_ref": source_ref,
            "test_id": test_id,
            "arguments": arguments,
            "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "installed": installed,
        }, f, indent=2)
        f.write("\n")
    result["status"] = "created"
    return result


_PHORONIX_README_DETAIL_FIELDS = [
    "Test Type", "Software Type", "License Type", "Supported Platforms",
    "Project Web-Site", "OpenBenchmarking.org Test Profile",
]


def _build_phoronix_test_readme(bare_name, test_id, fields, source_ref):
    """Renders `phoronix-test-suite info <test_id>`'s Description plus a
    handful of other high-level fields into README.md text for
    dest_root/<bare_name>/ -- one per bare test name, shared across all of
    its option-combination subdirectories, so someone browsing the
    materialized suite library sees what the test actually measures
    without having to run `phoronix-test-suite info` themselves."""
    lines = [f"# {bare_name}", ""]
    description = fields.get("Description")
    if description:
        lines += [description, ""]
    detail_lines = [f"- **{name}:** {fields[name]}" for name in _PHORONIX_README_DETAIL_FIELDS if fields.get(name)]
    if detail_lines:
        lines += detail_lines + [""]
    lines += [f"Source test profile: `{test_id}`", "",
              f"Generated by `wspy-phoronix-import` from {source_ref}.", ""]
    return "\n".join(lines)


def write_phoronix_test_readme(bare_name, dest_root, test_id, fields, source_ref):
    """Idempotently writes dest_root/<bare_name>/README.md -- same
    "additive, don't overwrite" convention as
    materialize_phoronix_test_point()'s suite-definition.xml, so a human's
    own edits to a previously-generated README survive a later re-import.
    Returns {"bare_name", "path", "status"} where status is
    "created"/"exists"/"skipped" ("skipped" when fields is falsy -- the
    `phoronix-test-suite info` lookup failed or --no-check-installed
    disabled it, so there's nothing to render)."""
    out_dir = os.path.join(dest_root, bare_name)
    path = os.path.join(out_dir, "README.md")
    if os.path.isfile(path):
        return {"bare_name": bare_name, "path": path, "status": "exists"}
    if not fields:
        return {"bare_name": bare_name, "path": path, "status": "skipped"}
    os.makedirs(out_dir, exist_ok=True)
    with open(path, "w") as f:
        f.write(_build_phoronix_test_readme(bare_name, test_id, fields, source_ref))
    return {"bare_name": bare_name, "path": path, "status": "created"}


def add_phoronix_test_point_to_ledger(identity, list_path, ledger_bin="wspy-ledger", cwd=None, timeout=15):
    """Thin subprocess wrapper around `wspy-ledger --add <identity> --list
    <list_path>` (ledger.c's add_to_list() -- idempotent, so calling this
    again for an identity already in the list is safe and just reports
    "already in ... not added"). Returns {"command", "exit_code", "output",
    "timed_out"}, the same shape run_sync()'s other callers already
    surface to their own JSON responses."""
    argv = [ledger_bin, "--add", identity, "--list", list_path]
    rc, output, timed_out = run_sync(argv, cwd=cwd, timeout=timeout)
    return {"command": shell_preview(argv), "exit_code": rc,
            "output": (output or "").strip(), "timed_out": timed_out}


def fetch_phoronix_info_fields(test_id, phoronix_bin="phoronix-test-suite", cwd=None, timeout=30):
    """Runs `phoronix-test-suite info <test_id>` once and returns
    parse_phoronix_info_fields()'s dict, or None if the subprocess itself
    failed (timeout, phoronix-test-suite missing) -- the shared subprocess
    call behind both check_phoronix_test_installed() (just the "Test
    Installed" field) and the README generation below (Description and a
    few other fields), so a caller needing both only pays for one `info`
    invocation per test_id (see import_phoronix_test_points()'s cache)."""
    argv = [phoronix_bin, "info", test_id]
    rc, output, timed_out = run_sync(argv, cwd=cwd, timeout=timeout)
    if timed_out or rc is None:
        return None
    return parse_phoronix_info_fields(output)


def check_phoronix_test_installed(test_id, phoronix_bin="phoronix-test-suite", cwd=None, timeout=30):
    """Returns fetch_phoronix_info_fields()'s "Test Installed" field as
    True/False, or None if the check itself failed or the field was
    unrecognized -- "unknown", not "not installed", since this is advisory
    information for a human, not a gate on anything."""
    fields = fetch_phoronix_info_fields(test_id, phoronix_bin=phoronix_bin, cwd=cwd, timeout=timeout)
    value = (fields or {}).get("Test Installed")
    if value not in ("Yes", "No"):
        return None
    return value == "Yes"


def import_phoronix_test_points(xml_bytes, dest_root, source_kind, source_ref,
                                 phoronix_bin="phoronix-test-suite", ledger_bin="wspy-ledger",
                                 ledger_list_path=None, cwd=None, dry_run=False,
                                 check_installed=True, add_to_ledger=True):
    """Top-level orchestration shared by wspy-phoronix-import and
    web/server.py's Phoronix tab: parse -> materialize each test point ->
    (unless dry_run/add_to_ledger is False) register with `wspy-ledger
    --add`. Returns {"points": [...], "readmes": [...], "error": str or
    None} -- each "points" entry is materialize_phoronix_test_point()'s own
    dict (or its dry-run equivalent, status "would-create" instead of
    "created") plus a "ledger" key (add_phoronix_test_point_to_ledger()'s
    result, or None when skipped). "readmes" has one entry per distinct
    bare test name touched by this call (write_phoronix_test_readme()'s own
    dict, or its dry-run equivalent) -- dest_root/<bare_name>/README.md
    sits one level above the <test>/<options>/ suite directories, since the
    description applies to the whole test regardless of which option
    combination.

    check_installed calls `phoronix-test-suite info` once per *unique*
    test_id (option combinations of the same test share one lookup) --
    still one subprocess per distinct test, so a big result (dozens of
    tests) is noticeably slower with this on; callers that don't need the
    installed flag (or are previewing a huge result) can pass False. The
    same `info` output backs README generation, so check_installed=False
    also means no new README gets written (status "skipped" -- no
    Description available) rather than a second subprocess call just for
    that.

    dry_run computes and reports everything, including what wspy-ledger
    *would* be called with, without writing any file or invoking
    wspy-ledger -- lets a caller preview a big result's decomposition
    before committing it to workload/phoronix/."""
    try:
        raw_points = parse_phoronix_xml_test_points(xml_bytes)
    except ET.ParseError as e:
        return {"points": [], "error": f"could not parse source XML: {e}"}
    if not raw_points:
        return {"points": [], "error": "no <Execute>/<Result> test points found in source XML"}

    list_path = ledger_list_path or os.path.join(dest_root, "backlog.txt")
    info_cache = {}

    def get_info_fields(test_id):
        if not check_installed:
            return None
        if test_id not in info_cache:
            info_cache[test_id] = fetch_phoronix_info_fields(test_id, phoronix_bin=phoronix_bin, cwd=cwd)
        return info_cache[test_id]

    out_points = []
    readmes = {}
    for raw in raw_points:
        fields = get_info_fields(raw["test_id"])
        value = (fields or {}).get("Test Installed")
        installed = (value == "Yes") if value in ("Yes", "No") else None
        bare_name = phoronix_bare_test_name(raw["test_id"])
        if bare_name not in readmes:
            readme_path = os.path.join(dest_root, bare_name, "README.md")
            if dry_run:
                if os.path.isfile(readme_path):
                    readme_status = "exists"
                elif not check_installed:
                    readme_status = "skipped"
                else:
                    readme_status = "would-create"
                readmes[bare_name] = {"bare_name": bare_name, "path": readme_path, "status": readme_status}
            else:
                readmes[bare_name] = write_phoronix_test_readme(bare_name, dest_root, raw["test_id"],
                                                                  fields, source_ref)
        if dry_run:
            options_slug = slugify_phoronix_arguments(raw["arguments"])
            identity = f"{bare_name}-{options_slug}"
            out_dir = os.path.join(dest_root, bare_name, options_slug)
            already = os.path.isfile(os.path.join(out_dir, "suite-definition.xml"))
            entry = {
                **raw, "bare_name": bare_name, "options_slug": options_slug, "identity": identity,
                "dir": out_dir, "status": "exists" if already else "would-create",
                "installed": installed, "ledger": None,
            }
        else:
            entry = materialize_phoronix_test_point(raw, dest_root, source_kind, source_ref, installed=installed)
            entry["ledger"] = (add_phoronix_test_point_to_ledger(entry["identity"], list_path,
                                                                  ledger_bin=ledger_bin, cwd=cwd)
                                if add_to_ledger else None)
        out_points.append(entry)
    return {"points": out_points, "readmes": list(readmes.values()), "error": None}


def list_materialized_phoronix_test_points(dest_root):
    """Inventory of already-materialized test points under dest_root/<test>/
    <options>/ -- backs the Phoronix tab's inventory table and
    wspy-phoronix-import --list-materialized. Reads each source.json
    sidecar materialize_phoronix_test_point() wrote (a directory with a
    suite-definition.xml but no readable source.json is skipped rather
    than erroring -- best-effort, matching this codebase's "degrade,
    don't fail" convention for filesystem scans elsewhere, e.g.
    scan_phoronix_dependencies() in ledger.c). Each entry also lists any
    linked runs (link_phoronix_test_point_run()'s own <dir>/runs/<run_id>
    symlinks): {run_id, suite, benchmark}, decoded from the symlink's own
    target path (.../<suite>/<benchmark>/<run_id>, the last 3 components)
    rather than re-deriving them some other way -- a dangling symlink
    (target since deleted) is skipped. "installed" reflects whatever
    materialize_phoronix_test_point() observed at materialize time (True/
    False/None for unknown) -- this is not re-checked here, so it can go
    stale (installed after materializing, or vice versa); re-materializing
    the same point (check_installed=True) refreshes it. Returns a list of
    {test_id, bare_name, options_slug, identity, dir, arguments,
    source_kind, source_ref, generated_at, installed, runs}, newest
    generated_at first."""
    entries = []
    if not os.path.isdir(dest_root):
        return entries
    for bare_name in sorted(os.listdir(dest_root)):
        test_dir = os.path.join(dest_root, bare_name)
        if not os.path.isdir(test_dir):
            continue
        for options_slug in sorted(os.listdir(test_dir)):
            point_dir = os.path.join(test_dir, options_slug)
            source_path = os.path.join(point_dir, "source.json")
            if not os.path.isfile(os.path.join(point_dir, "suite-definition.xml")):
                continue
            try:
                with open(source_path) as f:
                    source = json.load(f)
            except (OSError, ValueError):
                continue

            runs = []
            runs_dir = os.path.join(point_dir, "runs")
            if os.path.isdir(runs_dir):
                for run_id in sorted(os.listdir(runs_dir)):
                    link_path = os.path.join(runs_dir, run_id)
                    target = os.path.realpath(link_path)
                    if not os.path.isdir(target):
                        continue  # dangling symlink -- target run directory is gone
                    parts = os.path.normpath(target).split(os.sep)
                    if len(parts) < 3:
                        continue
                    real_run_id, benchmark, suite = parts[-1], parts[-2], parts[-3]
                    runs.append({"run_id": real_run_id, "suite": suite, "benchmark": benchmark})

            entries.append({
                "test_id": source.get("test_id", ""),
                "bare_name": bare_name,
                "options_slug": options_slug,
                "identity": f"{bare_name}-{options_slug}",
                "dir": point_dir,
                "arguments": source.get("arguments", ""),
                "source_kind": source.get("source_kind", ""),
                "source_ref": source.get("source_ref", ""),
                "generated_at": source.get("generated_at", ""),
                "installed": source.get("installed"),
                "runs": runs,
            })
    entries.sort(key=lambda e: e["generated_at"], reverse=True)
    return entries


def resolve_phoronix_test_point_dir(dest_root, raw_dir):
    """Validates raw_dir (untrusted -- comes from a request body) is a real
    materialized test point: resolves under dest_root (guards against a
    stray '..'/typo pointing outside workload/phoronix/, the same
    "local tool, guard against mistakes not adversaries" posture
    valid_segment()/valid_relpath() already use elsewhere in this file)
    and actually has a suite-definition.xml. Returns the realpath, or None
    if either check fails."""
    if not raw_dir:
        return None
    real_dest = os.path.realpath(dest_root)
    real_dir = os.path.realpath(raw_dir)
    if os.path.commonpath([real_dest, real_dir]) != real_dest:
        return None
    if not os.path.isfile(os.path.join(real_dir, "suite-definition.xml")):
        return None
    return real_dir


def copy_phoronix_test_point_to_local_suite(test_point_dir, identity, user_data_dir=None):
    """Copies <test_point_dir>/suite-definition.xml to
    ~/.phoronix-test-suite/test-suites/local/<identity>/suite-definition.xml
    (or under user_data_dir if given, e.g. a test's own PTS_USER_PATH
    override) -- the minimum needed for `phoronix-test-suite batch-run
    local/<identity>` to actually find the suite. Always overwrites
    (idempotent refresh, not a one-time copy, so a later edit to the
    materialized suite -- or a stale prior copy -- doesn't linger stale).
    Returns the destination path."""
    base = user_data_dir or phoronix_user_data_dir()
    dest_dir = os.path.join(base, "test-suites", "local", identity)
    os.makedirs(dest_dir, exist_ok=True)
    dest_path = os.path.join(dest_dir, "suite-definition.xml")
    shutil.copy2(os.path.join(test_point_dir, "suite-definition.xml"), dest_path)
    return dest_path


def link_phoronix_test_point_run(test_point_dir, run_id, rundir):
    """Best-effort: symlinks <test_point_dir>/runs/<run_id> -> rundir (an
    absolute path), so a run launched against a materialized test point is
    still browsable as a subdirectory of that test point's own directory
    -- purely a filesystem-browsing convenience layered on top of the run,
    which otherwise lives entirely under the normal --output-root
    unchanged (report page/compare/bundle/history all keep working
    against the real location; nothing here changes where a run's own
    files are written). Catches OSError and returns False rather than
    raising -- a run that already started successfully shouldn't fail
    just because e.g. the test point directory was deleted out from under
    it since the Run tab was populated. Idempotent: replaces an existing
    symlink at the same path (e.g. a re-run with an explicit run_id)."""
    try:
        runs_dir = os.path.join(test_point_dir, "runs")
        os.makedirs(runs_dir, exist_ok=True)
        link_path = os.path.join(runs_dir, run_id)
        if os.path.islink(link_path) or os.path.exists(link_path):
            os.remove(link_path)
        os.symlink(os.path.abspath(rundir), link_path)
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Run execution -- shared by web/server.py's background-thread executors and
# wspy-queue's synchronous, one-job-at-a-time processing. `state` needs only
# an .append(line) method (log a line) and a .finish(status, report_url)
# method (record the terminal status) -- RunState below is the reference
# implementation server.py also uses for its SSE relay; wspy-queue uses it
# too (its .cond/.lines are harmless overhead with no SSE listener attached).
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
                         supp_passes=None, manifest_on=False, affinity=None):
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
                                         run_index_path=run_index_path,
                                         affinity=affinity)
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
                                                          manifest_on, run_index_path,
                                                          affinity=affinity)
        full_argv = argv + ["--"] + workload_argv
        if p["timeout"]:
            full_argv = ["timeout", str(p["timeout"])] + full_argv
        emit(f"[{p['name']}] $ " + shell_preview(full_argv))
        _archive_stale_pts_hooks_log(emit)
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
        # Not recorded anywhere in manifest.json (that file is wspy-run's own
        # generate_manifest(), which knows nothing about these supplementary
        # passes) -- the artifact just lands in rundir like the pass's own
        # output/manifest, picked up by render_wspy_run_report()'s existing
        # "Other artifacts" scan for anything no passes[] entry claims.
        _capture_pts_hooks_log(emit, rundir, p["name"])

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

    # Each of these builtin profiles' own --tree pass flags are fixed in
    # wspy-run's load_builtin_profile() -- not discoverable from here without
    # shelling out and parsing wspy-run's own bash config, so this mirrors
    # those fixed choices directly (tree-heavy: --tree-cmdline only;
    # gpu-compute: the syscall-latency set, no cmdline). Update alongside
    # load_builtin_profile() if either ever changes, or a future profile adds
    # its own --tree pass.
    profile_names = profile.split(",")
    if "tree-heavy" in profile_names:
        run_proctree_besteffort(emit, cfg, rundir, cmdline=True)
    elif "gpu-compute" in profile_names:
        run_proctree_besteffort(emit, cfg, rundir, futex=True, io_wait=True,
                                 connect=True, wait=True, poll=True, nanosleep=True)

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
            {"name": p["name"], "output": p["output"], "manifest": p["manifest"],
             "pts_hooks_log": p.get("pts_hooks_log"), "status": p["status"]}
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
                        custom_plots=None, only_custom=False, autofit_notes=None,
                        affinity=None):
    """Item 9's "customized away from a preset" path: runs each enabled
    configuration (see build_configuration_passes()) as its own sequential
    wspy invocation into this run directory -- the direct-command-lines
    fallback the deep-dive's own rule calls for once a preset's checklist has
    been touched. Ends by writing a wspy-run-shaped manifest.json/summary.txt
    (see write_custom_run_manifest()/write_custom_run_summary() above) so the
    existing report/curation/compare machinery needs no new code path.

    checklist has already been through autofit_checklist_for_custom_plots()
    by the caller -- autofit_notes is only threaded through here to surface
    what was auto-enabled in the live log, not to redo the autofit (that
    would find nothing left to change)."""
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
                                                         manifest_on, run_index_path,
                                                         affinity=affinity)
        full_argv = argv + ["--"] + workload_argv
        if p["timeout"]:
            full_argv = ["timeout", str(p["timeout"])] + full_argv
        emit(f"[{p['name']}] $ " + shell_preview(full_argv))
        _archive_stale_pts_hooks_log(emit)
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
        pts_hooks_log = _capture_pts_hooks_log(emit, rundir, p["name"])
        pass_records.append({
            "name": p["name"],
            "output": os.path.basename(outfile),
            "manifest": os.path.basename(manifest_path) if manifest_path else None,
            "pts_hooks_log": pts_hooks_log,
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

    tree_pass = next((p for p in passes if p["name"] == "tree"), None)
    if tree_pass:
        run_proctree_besteffort(emit, cfg, rundir,
                                 cmdline="--tree-cmdline" in tree_pass["flags"],
                                 futex="--tree-futex" in tree_pass["flags"],
                                 io="--tree-io" in tree_pass["flags"],
                                 io_wait="--tree-io-wait" in tree_pass["flags"],
                                 schedstat="--tree-schedstat" in tree_pass["flags"],
                                 vmsize="--tree-vmsize" in tree_pass["flags"],
                                 connect="--tree-connect" in tree_pass["flags"],
                                 wait="--tree-wait" in tree_pass["flags"],
                                 poll="--tree-poll" in tree_pass["flags"],
                                 nanosleep="--tree-nanosleep" in tree_pass["flags"])

    write_custom_run_summary(rundir, pass_records)
    write_custom_run_manifest(rundir, suite, benchmark, run_id, workload_argv, pass_records)
    emit(f"[wrote {RUN_MANIFEST_NAME}]")

    if store_ingest:
        run_store_ingest_besteffort(emit, cfg, run_index_path)

    logf.close()
    status = "done" if not any_failed and plot_rc == 0 else "error"
    state.finish(status, None)


# ---------------------------------------------------------------------------
# Job files (INVESTIGATION.md's "What shipped in 4.1", "Deployment/hosting design note").
#
# A job is a spec-only JSON document -- "what should run", captured before
# any run directory or output exists -- built from exactly the same
# preset/checklist configuration + workload command + suite/benchmark
# identity build_configuration_passes()/build_wspy_run_argv() above already
# consume. It deliberately carries no reference to the machine that created
# it: no absolute --output-root, no path into that machine's --run-index or
# store.db. That's what makes a job portable -- copy the file to a second
# machine that also has wspy checked out, drop it in that machine's own
# jobs/pending/ directory, and `wspy-queue run` there works it against that
# machine's own independent output tree, with no shared/synced state between
# the two.
#
# wspy-queue (repo root, alongside wspy-run) owns the actual pending ->
# running -> done/failed lifecycle (moving a job file between
# <jobs-dir>/<state>/ subdirectories); this module only knows how to build
# and validate the job document itself, so web/server.py's Run tab (job
# *creator*, via POST /api/enqueue-job) and wspy-queue (job *runner*) share
# one definition of what a valid job looks like.
# ---------------------------------------------------------------------------

JOB_SCHEMA_VERSION = "1.0.0"
JOB_STATES = ("pending", "running", "done", "failed")


def make_job_id():
    """Same shape as make_run_id() but a distinct value -- a job's identity
    (which file in jobs/<state>/ this is) is independent of the run_id the
    run it eventually produces will have; see build_job()'s "run_id" note."""
    now = datetime.now(timezone.utc)
    ms = now.microsecond // 1000
    return f"job-{now.strftime('%Y%m%dT%H%M%S')}.{ms:03d}-{secrets.token_hex(4)}"


def resolve_toggles(cfg, toggles):
    """The manifest/run-index/store-ingest toggle chips (item 9's mockup-
    feedback item, see INVESTIGATION.md line ~336), resolved against a
    cfg dict's run_index_file path -- shared by server.py's Run tab
    (Handler._parse_toggles(), wrapping this around body["toggles"]) and
    wspy-queue (wrapping it around a job's own "toggles" object), so both
    front ends apply store_ingest's "requires run_index" rule identically.
    Returns (manifest_on, run_index_path_or_None, store_ingest)."""
    toggles = toggles or {}
    manifest_on = bool(toggles.get("manifest", True))
    run_index_on = bool(toggles.get("run_index", True))
    store_ingest = bool(toggles.get("store_ingest", True)) and run_index_on
    run_index_path = cfg["run_index_file"] if run_index_on else None
    return manifest_on, run_index_path, store_ingest


def build_job(workload_argv, suite, benchmark, mode, profile=None, checklist=None,
              custom_plots=None, only_custom=False, toggles=None, run_id=None, notes=None,
              affinity=None):
    """Builds a portable job document. mode is "preset" (profile is a
    wspy-run BUILTIN_PROFILES spec, e.g. "deep-cpu,tree-heavy") or "custom"
    (checklist is the same object build_configuration_passes() consumes).
    run_id is normally left None -- wspy-queue assigns a fresh one (via
    make_run_id()) at process time, since a job may be processed on a
    different machine than the one that created it and each processing is
    its own distinct run; give an explicit run_id only when correlating a
    specific run_id across machines actually matters to the caller.
    toggles mirrors the Run tab's manifest/run_index/store_ingest chips
    (server.py's _parse_toggles()), defaulting the same way: all on except
    store_ingest, which also requires run_index. affinity is a validated
    --affinity=<spec> string (or None/"all" for the default) -- a placement
    choice, portable across machines the same as everything else in the job
    document, since it names CPUs by topology-relative id (thread=<id>/
    domain=<id>) or an explicit cpuset, not anything host-specific beyond
    that (a job replayed on a machine with fewer CPUs/domains than the one
    that created it will fail loudly at wspy's own --affinity resolution,
    same as any other under-provisioned replay target)."""
    toggles = toggles or {}
    return {
        "job_schema_version": JOB_SCHEMA_VERSION,
        "job_id": make_job_id(),
        "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z"),
        "workload": list(workload_argv),
        "suite": suite,
        "benchmark": benchmark,
        "run_id": run_id,
        "mode": mode,
        "profile": profile,
        "checklist": checklist,
        "custom_plots": custom_plots or [],
        "only_custom": bool(only_custom),
        "toggles": {
            "manifest": bool(toggles.get("manifest", True)),
            "run_index": bool(toggles.get("run_index", True)),
            "store_ingest": bool(toggles.get("store_ingest", True)),
        },
        "affinity": affinity or None,
        "notes": notes or "",
    }


def validate_job(job):
    """Returns a list of error strings (empty if valid). Deliberately mirrors
    server.py's _parse_workload_and_ids()/_parse_toggles()/_parse_custom_plots()
    validation, since a job file may have been hand-copied or hand-edited on
    a second machine and never passed through the web UI's own form
    validation at all -- a job dropped into jobs/pending/ gets exactly the
    same checks a web-submitted one already got."""
    errors = []
    if not isinstance(job, dict):
        return ["job must be a JSON object"]

    workload = job.get("workload")
    if not isinstance(workload, list) or not workload or not all(isinstance(w, str) for w in workload):
        errors.append("workload must be a non-empty list of strings")

    suite = job.get("suite")
    benchmark = job.get("benchmark")
    if not isinstance(suite, str) or not valid_segment(suite):
        errors.append("suite must be non-empty and contain only letters, digits, '.', '_', '-'")
    if not isinstance(benchmark, str) or not valid_segment(benchmark):
        errors.append("benchmark must be non-empty and contain only letters, digits, '.', '_', '-'")

    run_id = job.get("run_id")
    if run_id is not None and not valid_segment(run_id):
        errors.append("run_id, if given, must contain only letters, digits, '.', '_', '-'")

    mode = job.get("mode")
    if mode == "preset":
        profile = job.get("profile")
        if not isinstance(profile, str) or not profile or not valid_profile_spec(profile):
            errors.append("profile is required for mode=preset and must be a comma-separated list "
                           "of letters/digits/'-'/'_' (e.g. deep-cpu or deep-cpu,tree-heavy)")
    elif mode == "custom":
        checklist = job.get("checklist")
        if checklist is not None and not isinstance(checklist, dict):
            errors.append("checklist must be an object")
    else:
        errors.append('mode must be "preset" or "custom"')

    custom_plots = job.get("custom_plots") or []
    if not isinstance(custom_plots, list):
        errors.append("custom_plots must be a list")
    else:
        for item in custom_plots:
            if not isinstance(item, dict) or not item.get("name") or not item.get("columns"):
                errors.append("each custom_plots entry needs a name and at least one column")
                break

    affinity = job.get("affinity")
    if affinity is not None and not valid_affinity_spec(affinity):
        errors.append("affinity, if given, must be all/nosmt/thread=<id>/domain=<id>/cpuset=<c0,c1,...>")

    return errors
