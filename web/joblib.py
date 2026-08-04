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
import csv
import hashlib
import html
import io
import json
import os
import re
import secrets
import shlex
import shutil
import socket
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
COMMAND_TXT_NAME = "command.txt"  # wspy-run's own plain-text companion to manifest.json's "command"
                                   # array (INVESTIGATION.md 4.3 Tier 3 item 2's "additional artifacts"
                                   # work) -- only present for wspy-run-launched runs, same as
                                   # RUN_MANIFEST_NAME itself.

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

# server.py's characterization-badge generator (INVESTIGATION.md 4.3 Tier 3 item 3) writes this file
# once a human clicks "Generate characterization badge" in the studio -- a small wspy-archetype --run
# scorecard formatted as markdown. From then on it's an ordinary artifact: collect_run_files() below
# offers it, guess_kind()'s ".md" -> "markdown" rule renders it through the existing markdown artifact
# pipeline unchanged, same "external tool output becomes a curatable file" precedent
# aianalysis.<model>.md already established for wspy-analyze -- no new block kind needed.
ARCHETYPE_BADGE_NAME = "archetype_badge.md"

# Item 3's other half -- server.py's similarity-panel generator writes this file once a human clicks
# "Generate similarity panel" in the studio: a wspy-archetype --nearest neighbor table formatted as
# markdown. Same "external tool output becomes a curatable file" precedent as ARCHETYPE_BADGE_NAME
# above, for the same reason (avoids threading a live wspy-archetype dependency through every render/
# export path, several of which wspy-testpoint also calls with no wspy-archetype config available).
ARCHETYPE_SIMILAR_NAME = "archetype_similar.md"


def escape_like(s):
    """Escapes %, _, and \\ for safe interpolation into a SQL LIKE pattern (ESCAPE '\\') -- benchmark
    names routinely contain underscores (e.g. "707.ntest_r-gcc_O3-base"), which LIKE would otherwise
    treat as a single-character wildcard and silently over-match."""
    return s.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")


def resolve_store_pass_rows(cur, hostname, suite, benchmark, run_id):
    """Returns [(store_run_id, config_name), ...] for one run directory's actual rows in wspy-store's
    `runs` table. A run directory's own name (wspy-run's naming, one manifest.json per directory) is
    *not* a store run_id -- wspy-store's `runs` table is keyed by each underlying wspy invocation's own
    run-index run_id, generated independently per collection pass (run_index.c). wspy-run's unified
    --suite/--benchmark layout always writes every pass's manifest/output files under
    <output_root>/<suite>/<benchmark>/<run_id>/, and that path is preserved verbatim in the store's own
    output_path/manifest_path columns (store.c) regardless of what --output-root prefix the ingesting
    host used -- so matching on the "/<suite>/<benchmark>/<run_id>/" suffix identifies every pass row
    belonging to this exact run directory without needing --output-root here at all. Also tries an exact
    (hostname, run_id) match, for a bare wspy run dropped into this layout by hand (a directory named
    after the run-index record's own run_id directly) -- so both a real wspy-run multi-pass directory
    and a single hand-placed run resolve correctly. Shared by wspy-testpoint (aggregate/render's --run-id
    resolution) and server.py's characterization-badge generator (wspy-archetype --run needs one real
    store run_id, not the directory name)."""
    pattern = "%/" + escape_like(suite) + "/" + escape_like(benchmark) + "/" + escape_like(run_id) + "/%"
    cur.execute(
        "SELECT run_id, config_name FROM runs WHERE hostname = ? AND "
        "(manifest_path LIKE ? ESCAPE '\\' OR output_path LIKE ? ESCAPE '\\')",
        (hostname, pattern, pattern))
    rows = dict(cur.fetchall())
    cur.execute("SELECT run_id, config_name FROM runs WHERE hostname = ? AND run_id = ?",
                (hostname, run_id))
    rows.update(cur.fetchall())
    return list(rows.items())


def resolve_store_run_directory(cur, hostname, store_run_id):
    """The reverse of resolve_store_pass_rows() above: given a store run_id (as `wspy-archetype
    --nearest`'s neighbor rows name them, not a run directory's own name), returns the
    (suite, benchmark, run_id) of the run directory it belongs to, or None if the row doesn't exist or
    its recorded path doesn't look like wspy-run's unified <suite>/<benchmark>/<run_id>/ layout. Reads
    whichever of output_path/manifest_path is set and takes the dirname's last 3 path components -- same
    "path preserved verbatim regardless of --output-root prefix" property resolve_store_pass_rows()
    relies on, just walked in the other direction. Used by server.py's similarity-panel generator to
    turn a bare neighbor identity into a /report/<suite>/<benchmark>/<run_id> link."""
    cur.execute("SELECT output_path, manifest_path FROM runs WHERE hostname = ? AND run_id = ?",
                (hostname, store_run_id))
    row = cur.fetchone()
    if not row:
        return None
    for path in row:
        if not path:
            continue
        parts = os.path.normpath(os.path.dirname(path)).split(os.sep)
        if len(parts) >= 3 and all(parts[-3:]):
            return tuple(parts[-3:])
    return None


def pick_counters_pass_id(pass_rows):
    """Given [(store_run_id, config_name), ...] (resolve_store_pass_rows() above), prefers the
    "counters" pass -- the widest single counter set (IPC/topdown/cache/branch/software) and the
    closest match to what wspy-archetype's feature extraction was built against -- falling back to
    whichever pass id resolved first if none is tagged "counters" (e.g. a profile that never collects
    one). `wspy-archetype --run` takes exactly one store run_id, unlike a bulk aggregate call that can
    take one per pass."""
    for store_run_id, config_name in pass_rows:
        if config_name == "counters":
            return store_run_id
    return pass_rows[0][0]


def guess_kind(filename):
    ext = os.path.splitext(filename)[1].lower()
    if ext == ".png":
        return "image"
    if ext == ".csv":
        return "csv"
    if ext == ".json":
        return "json"
    if ext == ".md":
        return "markdown"
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


AIANALYSIS_RE = re.compile(r"^aianalysis\.(.+)\.(?:txt|md)$")
AIPROMPT_CRITIQUE_RE = re.compile(r"^aiprompt\.critique\.(.+)\.(?:txt|md)$")

# wspy-analyze wrote these as .txt through 4.3; it writes .md now (the
# content was always markdown-flavored prose -- gpt-oss:20b et al. write
# markdown by default, primed by the prompt template's own markdown
# headers -- .md just makes that honest, and lets the report/export
# renderers convert it to real HTML instead of dumping it verbatim into a
# <pre>). Both regexes above match either extension so an old run's .txt
# narrative keeps being recognized/labeled/curated exactly as before --
# guess_kind() is what decides the actual rendering treatment (only ".md"
# gets kind="markdown"; old ".txt" files stay kind="text", still readable,
# just not reformatted).


def ai_artifact_label(filename):
    """Friendly (label, ai_generated) for one of wspy-analyze's own output
    files, or None if filename isn't one -- so collect_run_files() below can
    offer something more useful than the bare filename, and so a block built
    from actual model output (aianalysis.*/aiprompt.critique.*, not the
    deterministically-rendered aiprompt.txt/.md itself) carries an
    AI-generated marker from the moment it's added. See INVESTIGATION.md's
    Ollama deep-dive, design decision #7."""
    if filename in ("aiprompt.txt", "aiprompt.md"):
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


# Dimension columns a wspy --interval/--csv header can carry -- never plotted as a metric
# themselves, the same three names store.c's ingest_csv_metrics() and plot.c's template matcher
# both already special-case (a column named exactly one of these is a dimension, every other
# non-empty-named column is a metric). Shared here so server.py's interval-timeline viewer treats
# a CSV's columns identically to how the C toolchain already does.
INTERVAL_DIMENSION_COLUMNS = ("time", "core", "phase")

# GPU utilization-percentage columns plot.c already groups onto their own dedicated chart
# (templates[], "GPU busy/activity percentages") -- the same set the interval-timeline viewer
# overlays on a secondary axis. Other GPU columns (temp/power/freq/vram) are different units with
# their own separate plots already; not part of this overlay.
INTERVAL_GPU_COLUMNS = ("gpu_busy", "gpu_activity", "nv_gpu_busy")

# Row cap for the interval-timeline viewer's JSON payload -- a multi-hour --interval 1 soak run
# could have tens of thousands of rows, more than a hand-rolled SVG chart (or a browser tab) wants
# to render point-by-point. parse_interval_csv() below stride-decimates down to this cap rather
# than truncating, so a long run still shows its whole shape, just at lower resolution.
INTERVAL_VIEWER_MAX_ROWS = 5000


def csv_has_time_column(path):
    """True if path's CSV header names a "time" column -- the same eligibility rule plot.c already
    uses ("Only CSVs with a 'time' column ... are plottable time series"), i.e. this CSV was
    produced with --interval. Reads only the first line, not the whole file, so it's cheap enough
    to call once per pass artifact while rendering a report page."""
    try:
        with open(path, newline="") as f:
            header = next(csv.reader(f), None)
    except OSError:
        return False
    return bool(header) and "time" in header


def parse_interval_csv(path, max_rows=INTERVAL_VIEWER_MAX_ROWS):
    """Parses an --interval CSV into the shape web/static/interval_viewer.js charts: {"columns",
    "dimension_columns", "gpu_columns", "series": {col: [values...]}, "row_count", "decimated"}.
    Every non-dimension column is cast to float (invalid/empty cells become None, same "missing
    counter reads as absent, not zero" convention the rest of this codebase follows); "phase" (if
    present) is kept as its raw string values (warmup/steady/degraded) for the viewer's own
    run-length grouping. Uniformly stride-decimates down to max_rows if the file has more rows than
    that -- simple "don't lose the shape entirely" downsampling, not a precision feature, matching
    INTERVAL_VIEWER_MAX_ROWS's own reasoning."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, [])
        rows = list(reader)

    dimension_columns = [c for c in INTERVAL_DIMENSION_COLUMNS if c in header]
    gpu_columns = [c for c in INTERVAL_GPU_COLUMNS if c in header]

    row_count = len(rows)
    decimated = row_count > max_rows
    if decimated:
        stride = -(-row_count // max_rows)  # ceil division -- guarantees len(rows) <= max_rows
        rows = rows[::stride]

    series = {col: [] for col in header if col}
    for row in rows:
        for col, cell in zip(header, row):
            if not col:
                continue
            if col == "phase":
                series[col].append(cell)
                continue
            try:
                series[col].append(float(cell))
            except ValueError:
                series[col].append(None)

    return {
        "columns": [c for c in header if c],
        "dimension_columns": dimension_columns,
        "gpu_columns": gpu_columns,
        "series": series,
        "row_count": row_count,
        "decimated": decimated,
    }


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
        add(COMMAND_TXT_NAME, "command line")
        add(RUN_MANIFEST_NAME, "wspy-run run manifest")
        add(LOG_NAME, "launch log")
    else:
        add(PNG_NAME, "topdown plot")
        add(CSV_NAME, "amdtopdown.csv")
        add(MANIFEST_NAME, "manifest")
        add(LOG_NAME, "launch log")

    # Independent of run shape (unlike the two branches above) -- a no-op via add()'s own
    # os.path.isfile() check until a human actually generates one.
    add(ARCHETYPE_BADGE_NAME, "Workload characterization badge")
    add(ARCHETYPE_SIMILAR_NAME, "Nearest-neighbor similarity panel")

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
    if base.endswith(".manifest.json") or base in (RUN_MANIFEST_NAME, COMMAND_TXT_NAME):
        return "manifest"
    if (base in (SUMMARY_NAME, CURATION_NAME, PNG_NAME,
                 "process.tree.summary.txt", "process.tree.simple.txt",
                 "process.tree.top.txt", "process.tree.top1pct.txt") or
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

# (group name, default_on) -- the one default_on group (ipc; wspy.c's own
# `counter_mask = COUNTER_IPC` default) needs an explicit --no-<name> when
# left unchecked; the rest are named in one --counters=<list> flag when
# checked (wspy's --counters=<list>, the recommended replacement for the
# individual --<name> boolean flags this module used to emit one at a time
# -- see counter_group_flags() below). "software" used to be marked
# default_on here too, which was simply wrong -- COUNTER_SOFTWARE is never on
# by default in wspy -- so checking it here silently emitted nothing (only
# the unchecked case emitted a no-op --no-software); fixed once real testing
# of item 10's --target (which attaches to whatever counter_mask this pass's
# wspy invocation has active) surfaced it via a --target run with nothing to
# attach. Mirrors wspy.h's COUNTER_* set and is also the exact token
# vocabulary multipass.c's multipass_group_names[] uses for --passes=<list>
# (which --counters=<list> itself reuses), so the same list drives the
# plain-flags, --counters=-based, and --passes-bin-packed branches below
# without a second table to keep in sync.
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
    ("software", False),
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
             "connect", "wait", "poll", "nanosleep", "symbol_sample"},
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
        if name == "groups" and checklist_key in ("counters", "tree"):
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
        # Performance counters for this pass -- reuses "counters"' own
        # counter_group_flags() helper rather than a second table, so this
        # card's selector behaves identically (same --counters=<list>/
        # --no-<default-on-group> logic, same ALL_GROUPS vocabulary). This is
        # also what --target (below) attaches to for its pid-scoped matches:
        # one wspy process, one counter_mask, shared by the whole-subtree
        # aggregate this pass reports and any --target match. Previously
        # hardcoded to always --no-ipc plus a single ad hoc "software"
        # checkbox that (via a since-fixed ALL_GROUPS bug -- see the item-10
        # Run-tab entry under "Shipped since 4.2") silently never actually
        # enabled software counters; default empty, a quiet tree-structure-
        # only pass unless the user opts into counter groups here.
        group_flags, _selected = counter_group_flags(tree.get("groups"))
        flags += group_flags
        # --target=comm=<name>[,cmdline=<substr>] (INVESTIGATION.md 4.4
        # priorities item 10): a free-form spec string, not a checkbox --
        # wspy's own target_parse_spec() does the real validation (warns and
        # ignores a malformed spec rather than failing the run), so this
        # just passes a non-empty value through. Only ever emitted alongside
        # --tree above, since this whole block is gated on tree["enabled"],
        # so --target's "requires --tree" fatal check can never fire from
        # this UI path.
        target_spec = (tree.get("target") or "").strip()
        if target_spec:
            flags.append(f"--target={target_spec}")
        # --symbol-sample/--symbol-sample-event=<event> (INVESTIGATION.md 4.4
        # priorities item 9, "Symbol-level profiling"): only meaningful
        # alongside --target above (wspy itself fatals otherwise) -- this UI
        # doesn't enforce that client-side, same "wspy's own validation is
        # the source of truth" posture --target's own spec string already
        # has above; a --symbol-sample checked without a target spec just
        # surfaces as a real, clear wspy fatal error via the run's own log.
        if tree.get("symbol_sample"):
            flags.append("--symbol-sample")
            event = (tree.get("symbol_sample_event") or "").strip()
            if event:
                flags.append(f"--symbol-sample-event={event}")
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


def build_symbolize_argv(symbolize_bin, proctree_bin, tree_txt_path, pid=None, comm=None):
    """wspy-symbolize --json (INVESTIGATION.md 4.4 priorities item 9's (b)
    half, "Symbol-level profiling" -- address-to-symbol resolution for
    --symbol-sample profiling data): mirrors build_proctree_json_argv()'s
    role for the web viewer's on-demand /api/symbolize endpoint, but for the
    separate wspy-symbolize tool. --proctree passes through this server's
    own configured proctree binary (wspy-symbolize shells `proctree --json`
    itself internally, same as this server's own tree-viewer endpoints do)
    rather than letting it fall back to searching PATH. Exactly one of
    pid/comm must be given -- caller's responsibility, matching
    wspy-symbolize's own --pid/--comm mutually-exclusive-required CLI
    contract."""
    argv = [symbolize_bin, "--tree-file", tree_txt_path, "--proctree", proctree_bin, "--json"]
    if pid is not None:
        argv += ["--pid", str(pid)]
    else:
        argv += ["--comm", comm]
    return argv


# Top-process/pruned-tree derived views (INVESTIGATION.md 4.3 Tier 3 item 2's
# "additional artifacts" work): a starting default the author expects to
# tune once used on more runs, not a fixed policy -- top_fraction=0.01 (the
# "top 1%" the item was scoped around) alone would pick 0-1 processes on a
# small run and 1500+ on the ~155K-process stress run doc/ARTIFACT_CONTRACT.md
# mentions, so it's clamped to [PROCESS_TREE_TOP_MIN, PROCESS_TREE_TOP_MAX]
# to stay a readable-sized artifact either way.
PROCESS_TREE_TOP_FRACTION = 0.01
PROCESS_TREE_TOP_MIN = 5
PROCESS_TREE_TOP_MAX = 50


def _process_cpu_seconds(node):
    return (node.get("utime_seconds") or 0) + (node.get("stime_seconds") or 0)


def _flatten_tree_nodes(node, out):
    out.append(node)
    for child in node.get("children", []):
        _flatten_tree_nodes(child, out)


def _top_process_count(process_count, top_fraction=PROCESS_TREE_TOP_FRACTION,
                        top_min=PROCESS_TREE_TOP_MIN, top_max=PROCESS_TREE_TOP_MAX):
    if not process_count:
        return top_min
    return max(top_min, min(top_max, round(process_count * top_fraction)))


def render_top_processes_text(tree_json, top_fraction=PROCESS_TREE_TOP_FRACTION,
                               top_min=PROCESS_TREE_TOP_MIN, top_max=PROCESS_TREE_TOP_MAX):
    """Flat list of the individual *processes* (pids) with the highest CPU
    time (utime_seconds+stime_seconds) from a proctree --json document --
    distinct from proctree's own text summary view, which rolls every pid
    up by comm and never ranks individual processes. Ranked by actual CPU
    time (always present in the raw tree file regardless of which --tree-*
    flags a run used, per proctree's own --help text), not wall-clock
    start/finish span, since a process that merely waited a long time
    isn't what "top" should mean here."""
    root = tree_json.get("tree")
    if not root:
        return "(no tree data)\n"
    nodes = []
    _flatten_tree_nodes(root, nodes)
    process_count = tree_json.get("process_count", len(nodes))
    n = _top_process_count(process_count, top_fraction, top_min, top_max)
    ranked = sorted(nodes, key=_process_cpu_seconds, reverse=True)[:n]

    lines = ["Top %d of %d processes by CPU time (utime+stime):" % (len(ranked), process_count), ""]
    for node in ranked:
        lines.append("%9.3fs  pid=%-8s ppid=%-8s %s" % (
            _process_cpu_seconds(node), node.get("pid", "?"), node.get("ppid", "?"),
            node.get("cmdline") or node.get("comm") or "?"))
    return "\n".join(lines) + "\n"


def render_top1pct_tree_text(tree_json, top_fraction=PROCESS_TREE_TOP_FRACTION,
                              top_min=PROCESS_TREE_TOP_MIN, top_max=PROCESS_TREE_TOP_MAX):
    """The full process tree pruned to only the highest-CPU-time processes
    (same selection as render_top_processes_text()) plus whatever ancestor
    chain keeps them structurally connected to the root -- every other
    subtree collapses into a one-line "N more process(es) omitted" marker
    rather than disappearing silently, so the shape of what got hidden
    stays visible even though its contents don't."""
    root = tree_json.get("tree")
    if not root:
        return "(no tree data)\n"
    nodes = []
    _flatten_tree_nodes(root, nodes)
    process_count = tree_json.get("process_count", len(nodes))
    n = _top_process_count(process_count, top_fraction, top_min, top_max)
    kept_pids = {node.get("pid") for node in sorted(nodes, key=_process_cpu_seconds, reverse=True)[:n]}

    # One bottom-up pass marking which subtrees contain a kept pid at all --
    # naive repeated subtree scans during the print walk below would be
    # O(process_count^2), prohibitive on the ~155K-process runs
    # doc/ARTIFACT_CONTRACT.md mentions as a real stress case.
    has_kept = {}

    def mark(node):
        found = node.get("pid") in kept_pids
        for child in node.get("children", []):
            if mark(child):
                found = True
        has_kept[id(node)] = found
        return found

    mark(root)

    def count_subtree(node):
        total = 1
        for child in node.get("children", []):
            total += count_subtree(child)
        return total

    lines = ["Process tree pruned to the top %d of %d processes by CPU time (utime+stime), "
             "plus their ancestor chain:" % (len(kept_pids), process_count), ""]

    def walk(node, depth):
        marker = "*" if node.get("pid") in kept_pids else " "
        lines.append("%s%s %6.3fs %s (pid=%s)" % (
            "  " * depth, marker, _process_cpu_seconds(node), node.get("comm") or "?", node.get("pid", "?")))
        omitted = 0
        for child in node.get("children", []):
            if has_kept.get(id(child)):
                walk(child, depth + 1)
            else:
                omitted += count_subtree(child)
        if omitted:
            lines.append("%s  ... %d more process(es) omitted (below top %.0f%%)" % (
                "  " * depth, omitted, top_fraction * 100))

    walk(root, 0)
    return "\n".join(lines) + "\n"


def run_proctree_besteffort(emit, cfg, rundir, cmdline=False, futex=False, io=False, io_wait=False,
                             schedstat=False, vmsize=False, connect=False, wait=False, poll=False, nanosleep=False):
    """Best-effort trailing step mirroring the wspy-plot step (build_plot_argv()
    above) but for --tree's raw process.tree.txt record: renders it into
    human-readable views -- process.tree.summary.txt (every annotation this
    run's tree pass actually captured, via the cmdline/futex/... kwargs above,
    same as before), process.tree.simple.txt (proctree's own bare default:
    just cpu=/start=/finish= per process, no other flags -- easier to read as
    a pure process hierarchy once a run's tree pass captures enough
    annotations that the summary view gets visually busy), and
    process.tree.top.txt/process.tree.top1pct.txt (render_top_processes_text()/
    render_top1pct_tree_text() above -- individual processes ranked by CPU
    time, not proctree's own per-comm rollup). A no-op (not an error) when
    no --tree pass ran this time, or its output is missing/empty (e.g. a
    --tree pass that timed out before writing anything) -- and never fails
    the run itself, same degrade-don't-fail idiom as the plot step."""
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

    json_argv = build_proctree_json_argv(cfg["proctree_bin"], tree_txt)
    emit("$ " + shell_preview(json_argv))
    try:
        proc = subprocess.run(json_argv, cwd=REPO_ROOT, capture_output=True, text=True)
    except OSError as e:
        emit(f"[error] failed to launch proctree ({cfg['proctree_bin']}): {e}")
        return
    if proc.returncode != 0:
        emit(f"[error] proctree --json exited {proc.returncode}: {proc.stderr.strip()}")
        return
    try:
        tree_json = json.loads(proc.stdout)
    except ValueError as e:
        emit(f"[error] proctree --json produced invalid JSON: {e}")
        return

    for out_name, render in (("process.tree.top.txt", render_top_processes_text),
                              ("process.tree.top1pct.txt", render_top1pct_tree_text)):
        out_path = os.path.join(rundir, out_name)
        try:
            with open(out_path, "w") as f:
                f.write(render(tree_json))
            emit(f"[wrote {out_name}]")
        except OSError as e:
            emit(f"[error] failed to write {out_name}: {e}")


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


def resolve_phoronix_local_suite_test_ids(names, dest_root):
    """Maps each of `names` that looks like `local/<identity>` (the
    "Use in Run tab" workload string copy_phoronix_test_point_to_local_suite()
    builds, e.g. `local/build-linux-kernel-defconfig`) to the real pinned
    test_id (e.g. `pts/build-linux-kernel-1.18.0`) of the materialized test
    point under dest_root whose identity matches -- or leaves it unmapped if
    no match is found (a hand-installed `local/` suite unrelated to
    wspy-phoronix-import, or a stale/deleted materialized point).

    `local/<identity>` is only ever a wspy-generated wrapper *suite* around
    one real test -- `phoronix-test-suite info local/<identity>` doesn't
    carry a "Test Installed"/"Times Run" field the way `info
    pts/<test>-<version>` does (a suite isn't itself "installed"), so
    without this resolution estimate_phoronix_workload_seconds() reported
    "no such test, or unrecognized output" for every test point run this
    way, even when the real underlying test was installed and had run
    history on this host. Mirrors resolve_phoronix_subset_name()'s own
    "resolve a wrapper name back to the real profile it estimates for"
    idiom, just for our own local/ wrapper instead of PTS's own -subset
    suffix. Returns a dict {name: test_id} covering only the names that
    resolved; never raises (list_materialized_phoronix_test_points()
    already degrades to [] for a missing/unreadable dest_root)."""
    local_names = {n for n in names if n.startswith("local/")}
    if not local_names:
        return {}
    by_identity = {e["identity"]: e["test_id"] for e in list_materialized_phoronix_test_points(dest_root)}
    return {n: by_identity[n[len("local/"):]] for n in local_names if n[len("local/"):] in by_identity}


def estimate_phoronix_workload_seconds(workload, phoronix_bin="phoronix-test-suite",
                                        max_tests=PHORONIX_MAX_TESTS_CHECKED, cwd=None, dest_root=None):
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
    former inline copy of the same loop.

    dest_root (default REPO_ROOT/workload/phoronix) is only used to resolve
    `local/<identity>` names -- see resolve_phoronix_local_suite_test_ids()
    -- against our own materialized test points; it's irrelevant for a
    workload naming real PTS test/suite ids directly."""
    test_names = parse_phoronix_test_names(workload)
    if not test_names:
        return {"tests": [], "total_seconds": None, "truncated": False}

    checked_names = test_names[:max_tests]
    local_test_ids = resolve_phoronix_local_suite_test_ids(
        checked_names, dest_root or os.path.join(REPO_ROOT, "workload", "phoronix"))
    tests = []
    total_seconds = 0.0
    total_known = True
    for name in checked_names:
        local_test_id = local_test_ids.get(name)
        if local_test_id is not None:
            query_name, is_subset, is_local = local_test_id, False, True
        else:
            query_name, is_subset = resolve_phoronix_subset_name(name)
            is_local = False
        argv = [phoronix_bin, "info", query_name]
        rc, output, timed_out = run_sync(argv, cwd=cwd, timeout=30)
        entry = {"name": name, "command": shell_preview(argv)}
        if is_subset:
            entry["queried_name"] = query_name
            entry["queried_name_reason"] = "subset"
        elif is_local:
            entry["queried_name"] = query_name
            entry["queried_name_reason"] = "local-suite"
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
                elif is_local:
                    estimate["detail"] = (
                        f"'{name}' is a wspy-materialized single-test-point local suite wrapping "
                        f"the real test '{query_name}' -- installed/run-history reflects that "
                        "underlying test. " + estimate["detail"])
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
    preserving list of {"test_id": "pts/name-1.2.3", "arguments": "...",
    "description": "..."} dicts, one per distinct (test, option-
    combination) pair. "description" (a Result's sibling <Description>,
    e.g. "Build: defconfig" -- distinct from a suite's own top-level
    <SuiteInformation><Description>) is captured alongside <Arguments>
    because materialize_phoronix_test_point() needs both: a real PTS
    install (pts_test_suite.php's own suite parser, confirmed live
    2026-07-23) silently ignores a custom suite's <Arguments> and instead
    batch-runs *every* option in a test's menu whenever a test has
    configurable options and its <Execute> block has no <Description> --
    <Arguments> alone is not enough to pin a specific option combination.
    Raises xml.etree.ElementTree.ParseError on unparseable input -- callers
    decide how to surface that."""
    root = ET.fromstring(xml_bytes)
    points = []
    seen = set()

    def add(test_id, arguments, description):
        test_id = (test_id or "").strip()
        arguments = (arguments or "").strip()
        description = (description or "").strip()
        if not test_id:
            return
        key = (test_id, arguments)
        if key in seen:
            return
        seen.add(key)
        points.append({"test_id": test_id, "arguments": arguments, "description": description})

    for execute in root.iter("Execute"):
        test_el = execute.find("Test")
        args_el = execute.find("Arguments")
        desc_el = execute.find("Description")
        add(test_el.text if test_el is not None else None,
            args_el.text if args_el is not None else None,
            desc_el.text if desc_el is not None else None)

    for result in root.iter("Result"):
        # .find() (not .iter()) only looks at Result's direct children, so
        # this doesn't pick up the unrelated per-system-hardware
        # <Data><Entry><Identifier> nested two levels deeper in the same
        # <Result> block.
        id_el = result.find("Identifier")
        args_el = result.find("Arguments")
        desc_el = result.find("Description")
        add(id_el.text if id_el is not None else None,
            args_el.text if args_el is not None else None,
            desc_el.text if desc_el is not None else None)

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


def phoronix_pinned_version(test_id):
    """The "-<version>" suffix test_id itself carries (e.g. "1.17.1" for
    "pts/build-linux-kernel-1.17.1"), or None if test_id has no recognizable
    version suffix -- same detection phoronix_bare_test_name() already uses,
    just returning the piece that function discards."""
    name = test_id.split("/", 1)[1] if "/" in test_id else test_id
    last_dash = name.rfind("-")
    if last_dash != -1 and _phoronix_looks_like_version(name[last_dash + 1:]):
        return name[last_dash + 1:]
    return None


def _phoronix_version_key(version):
    return [int(part) if part.isdigit() else part for part in version.split(".")]


def list_installed_phoronix_test_versions(test_id, user_data_dir=None):
    """Installed version strings for test_id's bare test name, sorted --
    e.g. ["1.17.1", "1.18.0"] if both happen to be installed under
    ~/.phoronix-test-suite/installed-tests/pts/build-linux-kernel-*/.
    Deliberately distinct from check_phoronix_test_installed(): that asks
    phoronix-test-suite whether the *exact pinned* test_id is installed;
    this instead answers "is any version of this test installed, and if so
    which" by scanning installed-tests/<namespace>/ directly (no
    subprocess). The two can disagree when a materialized test point pins
    an older version (e.g. imported from an OpenBenchmarking result) than
    whatever's actually installed on this host -- confirmed live
    (2026-07-23): a build-linux-kernel test point pinned to 1.17.1 reported
    "not installed" and failed at wspy-run time even though 1.18.0 was
    already installed, because phoronix-test-suite treats each version as a
    wholly separate installed/uninstalled test. The Phoronix tab's
    inventory table uses this to show that mismatch instead of a flat
    yes/no, and offers a re-pin action (repin_phoronix_test_point()) built
    on top of it. Returns [] if the namespace directory doesn't exist or
    nothing matches -- never raises. user_data_dir overrides
    phoronix_user_data_dir(), same testability escape hatch
    copy_phoronix_test_point_to_local_suite() already uses."""
    bare_name = phoronix_bare_test_name(test_id)
    installed_root = os.path.join(user_data_dir or phoronix_user_data_dir(), "installed-tests")
    try:
        namespaces = os.listdir(installed_root)
    except OSError:
        return []

    primary_ns = test_id.split("/", 1)[0] if "/" in test_id else "pts"
    if primary_ns in namespaces:
        namespaces.remove(primary_ns)
        namespaces.insert(0, primary_ns)

    prefix = bare_name + "-"
    versions = set()
    for ns in namespaces:
        base = os.path.join(installed_root, ns)
        try:
            entries = os.listdir(base)
        except OSError:
            continue
        for entry in entries:
            if entry.startswith(prefix) and _phoronix_looks_like_version(entry[len(prefix):]):
                versions.add(entry[len(prefix):])

    sorted_versions = sorted(list(versions), key=_phoronix_version_key)
    return sorted_versions


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


def _build_phoronix_suite_xml(identity, test_id, arguments, source_ref, description=""):
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
    # A real PTS install ignores <Arguments> entirely -- silently
    # batch-running every option in the test's menu instead of just
    # this one -- unless <Execute> also carries a non-empty
    # <Description> (pts_test_suite.php's own BATCH-mode fallback,
    # confirmed live 2026-07-23 against a build-linux-kernel test
    # point: PTS ran both "defconfig" and "allmodconfig" when only
    # <Arguments>defconfig</Arguments> was present). This applies equally
    # when <Arguments> is empty (e.g. default option): without <Description>,
    # PTS batch-mode runs every option menu entry. The *content* doesn't
    # matter to PTS, only presence -- fall back to the arguments string
    # itself when the source XML had no real Description to carry over.
    desc = description or arguments
    if desc:
        ET.SubElement(execute, "Description").text = desc
    ET.indent(root, space="  ")
    return b'<?xml version="1.0"?>\n' + ET.tostring(root, encoding="utf-8") + b"\n"


def materialize_phoronix_test_point(point, dest_root, source_kind, source_ref, installed=None):
    """Writes one minimal single-test-point suite-definition.xml (plus a
    source.json provenance sidecar) for `point` (a {"test_id",
    "arguments", "description"} dict from parse_phoronix_xml_test_points())
    under dest_root/<bare-test-name>/<options-slug>/ -- the layout
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
    description = point.get("description", "")
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
        f.write(_build_phoronix_suite_xml(identity, test_id, arguments, source_ref, description=description))
    with open(os.path.join(out_dir, "source.json"), "w") as f:
        json.dump({
            "schema_version": 1,
            "source_kind": source_kind,
            "source_ref": source_ref,
            "test_id": test_id,
            "arguments": arguments,
            "description": description,
            "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "installed": installed,
        }, f, indent=2)
        f.write("\n")
    result["status"] = "created"
    return result


def repin_phoronix_test_point(test_point_dir, new_version, user_data_dir=None):
    """Rewrites <test_point_dir>/suite-definition.xml's <Execute><Test> to
    point at new_version instead of whatever version it was originally
    materialized with, and updates source.json to match -- the explicit,
    opt-in counterpart to the Phoronix tab's version-mismatch badge
    (list_installed_phoronix_test_versions()): that badge only *shows* a
    materialized point is pinned to a version this host no longer has
    installed, it doesn't fix it, because auto-re-pinning would silently
    defeat the reproducibility guarantee that pinning to an exact
    OpenBenchmarking-result version exists for in the first place (a test
    profile's metric definition can change between versions). This
    function is that fix, called only when a human clicks "re-pin."

    Keeps the namespace prefix ("pts/", "system/", ...) and bare test name
    unchanged when the new version is installed under that namespace; if
    the new version is installed under a different namespace (e.g. pts/ vs
    system/), updates the namespace prefix to match where the version is
    actually installed. Bare_name/options_slug/identity/directory layout
    (all derived from the bare name, never the version) are untouched; a
    re-pin never moves or renames a test point. source.json's original
    "test_id"/"generated_at" are preserved as "previous_test_id"/
    "generated_at" for provenance; a new "repinned_at" timestamp and
    "installed": True are set, since a re-pin is only ever offered for a
    version this host has confirmed installed.

    Returns {"old_test_id", "new_test_id", "dir"}, or raises FileNotFoundError
    if suite-definition.xml/source.json are missing, or ValueError if the
    XML has no <Execute><Test> to rewrite -- both indicate test_point_dir
    isn't actually a materialized test point, which callers are expected to
    have already validated (e.g. via resolve_phoronix_test_point_dir())
    before ever getting here."""
    suite_path = os.path.join(test_point_dir, "suite-definition.xml")
    source_path = os.path.join(test_point_dir, "source.json")
    tree = ET.parse(suite_path)
    test_el = tree.find("./Execute/Test")
    if test_el is None or not (test_el.text or "").strip():
        raise ValueError(f"no <Execute><Test> found in {suite_path}")
    old_test_id = test_el.text.strip()
    old_namespace = old_test_id.split("/", 1)[0] if "/" in old_test_id else ""
    bare_name = phoronix_bare_test_name(old_test_id)

    installed_root = os.path.join(user_data_dir or phoronix_user_data_dir(), "installed-tests")
    target_dirname = f"{bare_name}-{new_version}"
    namespace = old_namespace
    if old_namespace and not os.path.isdir(os.path.join(installed_root, old_namespace, target_dirname)):
        try:
            for ns in os.listdir(installed_root):
                if os.path.isdir(os.path.join(installed_root, ns, target_dirname)):
                    namespace = ns
                    break
        except OSError:
            pass
    elif not old_namespace and not os.path.isdir(os.path.join(installed_root, "pts", target_dirname)):
        try:
            for ns in os.listdir(installed_root):
                if os.path.isdir(os.path.join(installed_root, ns, target_dirname)):
                    namespace = ns
                    break
        except OSError:
            pass

    new_test_id = f"{namespace}/{bare_name}-{new_version}" if namespace else f"{bare_name}-{new_version}"

    test_el.text = new_test_id
    with open(suite_path, "wb") as f:
        f.write(b'<?xml version="1.0"?>\n' + ET.tostring(tree.getroot(), encoding="utf-8") + b"\n")

    with open(source_path) as f:
        source = json.load(f)
    source["previous_test_id"] = old_test_id
    source["test_id"] = new_test_id
    source["installed"] = True
    source["repinned_at"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    with open(source_path, "w") as f:
        json.dump(source, f, indent=2)
        f.write("\n")

    return {"old_test_id": old_test_id, "new_test_id": new_test_id, "dir": test_point_dir}


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


def find_materialized_phoronix_test_point(dest_root, identity):
    """The full entry from list_materialized_phoronix_test_points() whose "identity" matches, or
    None if no such materialized test point exists. Separate from resolve_test_identity() (which
    only returns the split test/test_point names) since some callers need the full metadata --
    test_id/arguments -- not just where the page nests, e.g. giving an auto-created test-point WP
    page real content instead of leaving it empty (web/server.py's "Publish to WordPress" flow)."""
    for entry in list_materialized_phoronix_test_points(dest_root):
        if entry["identity"] == identity:
            return entry
    return None


def test_point_wp_content(entry):
    """Hand-built WP block markup for one materialized Phoronix test point's page, matching
    cpu2026's own wp_content() pattern (scripts/publish_cpu2026_benchmarks.py) -- arguments text is
    unpredictable (raw paths/flags/model filenames) and doesn't need markdown_lite's heading/list/bold
    support, so there's no benefit to routing it through that module. Shared by
    scripts/publish_phoronix_pages.py's batch pass and web/server.py's "Publish to WordPress" flow
    (which uses it to give an auto-created test-point stub real content at publish time, instead of
    an empty page only a later separate script run would fill in)."""
    title = html.escape(entry["options_slug"])
    test_id = html.escape(entry["test_id"])
    arguments = html.escape(entry["arguments"] or "(none)")
    return (
        '<!-- wp:heading {"level":1} -->\n<h1>%s</h1>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:paragraph -->\n<p>Test point identity: <code>%s</code></p>\n<!-- /wp:paragraph -->\n\n'
        '<!-- wp:paragraph -->\n<p>Arguments: <code>%s</code></p>\n<!-- /wp:paragraph -->'
        % (title, test_id, arguments)
    )


def parse_summary_csv(text):
    """Parses wspy-summary --csv --quiet output (header row + one row per (group,metric) bucket) into
    a list of {column_name: value} dicts using its own header row, rather than hardcoding the column
    order -- stays correct if wspy-summary's own CSV columns are ever reordered/extended. --quiet
    suppresses the trailing summary line so every remaining line is a real data row; the row-width
    check below is a second guard against any other stray non-CSV line. Moved here from
    wspy-testpoint (its original sole caller, cmd_render) so INVESTIGATION.md 4.3 Tier 3 item 5's
    reference-matrix database can reuse it too, aggregating cells by shelling out to `wspy-testpoint
    aggregate --csv` the same way wspy-testpoint's own render subcommand shells out to
    wspy-summary."""
    rows = list(csv.reader(io.StringIO(text)))
    if not rows:
        return []
    header = rows[0]
    return [dict(zip(header, row)) for row in rows[1:] if len(row) == len(header)]


def resolve_test_identity(suite, benchmark, phoronix_dest_root, cpu2026_dest_root=None):
    """Returns (test, test_point, warning_or_None). For suite "phoronix", `benchmark` (wspy-run's own
    naming) already equals a materialized test point's "identity" (bare_name-options_slug,
    materialize_phoronix_test_point() above) -- split it back into (bare_name, options_slug) via
    find_materialized_phoronix_test_point(), rather than re-deriving it some other way. For suite
    "cpu2026", `benchmark` similarly equals a registered point's "identity" (bench-tag-tune,
    list_materialized_cpu2026_points() above) when cpu2026_dest_root is given -- split back into
    (bench, "tag-tune") via find_materialized_cpu2026_point(). cpu2026_dest_root defaults to None
    (rather than a real default path the way phoronix_dest_root doesn't) so existing callers that
    never resolve cpu2026 identities (e.g. wspy-testpoint, which only handles Phoronix today) keep
    their exact prior behavior without having to pass anything new. Any other suite, a phoronix
    benchmark that doesn't match any materialized point, or a cpu2026 benchmark with no
    cpu2026_dest_root given/no match, falls back to test=benchmark, test_point="default" per
    doc/REPORT_HIERARCHY.md's own convention for suites with no option axis -- degrade, don't fail,
    same idiom used throughout this codebase."""
    if suite == "phoronix":
        entry = find_materialized_phoronix_test_point(phoronix_dest_root, benchmark)
        if entry:
            return entry["bare_name"], entry["options_slug"], None
        return benchmark, "default", (
            "no materialized Phoronix test point found with identity %r under %r -- "
            "falling back to test=%r, test_point=\"default\"" % (benchmark, phoronix_dest_root, benchmark))
    if suite == "cpu2026" and cpu2026_dest_root:
        entry = find_materialized_cpu2026_point(cpu2026_dest_root, benchmark)
        if entry:
            return entry["bench"], f"{entry['tag']}-{entry['tune']}", None
        return benchmark, "default", (
            "no materialized cpu2026 benchmark point found with identity %r under %r -- "
            "falling back to test=%r, test_point=\"default\"" % (benchmark, cpu2026_dest_root, benchmark))
    return benchmark, "default", None


def enumerate_reference_matrix_cells(report_root_path, phoronix_dest_root, cpu2026_dest_root):
    """INVESTIGATION.md 4.3 Tier 3 item 5's reference-matrix database row/column enumeration: every
    (suite, test, test_point, machine) combination that has both a materialized test point (Phoronix
    or cpu2026 -- resolve_test_identity()'s own two suites) and an already-written
    <report-root>/<suite>/<test>/<test_point>/<machine>/runs.json (wspy-testpoint select-runs). A
    materialized test point with no runs.json yet for a given machine contributes no cell for that
    machine -- there is nothing to aggregate -- matching item 5's design choice to reuse
    wspy-testpoint's already-curated stats-pool role selection rather than re-deriving a run set
    directly from the store. Returns a list of {suite, test, test_point, machine, benchmark} dicts
    (benchmark is the --benchmark identity a caller needs for `wspy-testpoint aggregate`/`render`),
    sorted by (suite, test, test_point, machine)."""
    points = []
    if phoronix_dest_root:
        points += [("phoronix", e["identity"], e["bare_name"], e["options_slug"])
                   for e in list_materialized_phoronix_test_points(phoronix_dest_root)]
    if cpu2026_dest_root:
        points += [("cpu2026", e["identity"], e["bench"], f"{e['tag']}-{e['tune']}")
                   for e in list_materialized_cpu2026_points(cpu2026_dest_root)]

    cells = []
    for suite, identity, test, test_point in points:
        test_point_dir = os.path.join(report_root_path, suite, test, test_point)
        if not os.path.isdir(test_point_dir):
            continue
        for machine in sorted(os.listdir(test_point_dir)):
            if os.path.isfile(os.path.join(test_point_dir, machine, "runs.json")):
                cells.append({"suite": suite, "test": test, "test_point": test_point,
                              "machine": machine, "benchmark": identity})
    return sorted(cells, key=lambda c: (c["suite"], c["test"], c["test_point"], c["machine"]))


def aggregate_reference_matrix_cell(wspy_testpoint_bin, db, suite, benchmark, machine,
                                     report_root_path=None, report_root_remote=None, timeout=30):
    """Runs `wspy-testpoint aggregate --csv --quiet` for one enumerate_reference_matrix_cells() cell
    and returns parse_summary_csv()-shaped rows ({metric, n, min, max, mean, stddev, cv_percent,
    verdict}, one per counter-group metric), or None on any failure (missing store rows since the
    last select-runs, a --strict violation, a launch/timeout failure) -- callers render a "no data"
    cell rather than raising, since one cell failing to aggregate shouldn't take down the whole
    matrix. Shells out rather than importing wspy-testpoint directly (a hyphenated script name, not a
    plain importable module) -- same subprocess-reuse precedent web/server.py's
    execute_testpoint_publish() already established for select-runs/render."""
    argv = [wspy_testpoint_bin, "aggregate", "--suite", suite, "--benchmark", benchmark,
            "--machine", machine, "--db", db, "--csv", "--quiet"]
    if report_root_path:
        argv += ["--report-root", report_root_path]
    if report_root_remote:
        argv += ["--report-root-remote", report_root_remote]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return parse_summary_csv(proc.stdout)


def read_phoronix_test_description(dest_root, bare_name):
    """Returns the Description paragraph write_phoronix_test_readme() put
    in dest_root/<bare_name>/README.md (its second non-blank line -- the
    first is the "# <bare_name>" heading), or None if no README exists yet
    (--no-check-installed, or the `phoronix-test-suite info` lookup
    failed, or the info output itself had no Description field, in which
    case the second non-blank line is a detail bullet or the "Source test
    profile:" line instead, not prose). Lets the Phoronix tab's inventory
    show a one-line summary of each test group without re-running
    `phoronix-test-suite info`."""
    path = os.path.join(dest_root, bare_name, "README.md")
    try:
        with open(path) as f:
            lines = [line.strip() for line in f]
    except OSError:
        return None
    non_blank = [line for line in lines if line]
    if len(non_blank) < 2:
        return None
    candidate = non_blank[1]
    if candidate.startswith("-") or candidate.startswith("Source test profile:"):
        return None
    return candidate


def group_materialized_phoronix_points_by_test(points):
    """Groups list_materialized_phoronix_test_points()'s flat, recency-
    ordered list into one entry per bare test name, alphabetically -- the
    <test> -> <options> hierarchy the Phoronix tab's inventory renders.
    Recency order is useful for "what did I just import" but scales badly
    once a host has accumulated hundreds of points across many imports and
    a human is instead trying to find one specific test among them.
    Each group's own points are re-sorted by options_slug (recovering the
    ordering list_materialized_phoronix_test_points() had before its own
    final recency sort). Returns a list of {"bare_name", "points",
    "total_count", "installed_count", "run_status"}, sorted by bare_name.
    "run_status" is "all"/"some"/"none" depending on how many of the
    group's points have at least one linked run -- a quick per-test
    coverage signal (green/yellow/red dot in the Phoronix tab) distinct
    from "installed_count", since a point can be installed but never run,
    or (via a symlinked run from elsewhere) run without wspy-phoronix-import
    itself having checked it installed."""
    groups = {}
    for p in points:
        groups.setdefault(p["bare_name"], []).append(p)
    result = []
    for bare_name in sorted(groups):
        pts = sorted(groups[bare_name], key=lambda p: p["options_slug"])
        points_with_runs = sum(1 for p in pts if p.get("runs"))
        if points_with_runs == 0:
            run_status = "none"
        elif points_with_runs == len(pts):
            run_status = "all"
        else:
            run_status = "some"
        result.append({
            "bare_name": bare_name,
            "points": pts,
            "total_count": len(pts),
            "installed_count": sum(1 for p in pts if p.get("installed") is True),
            "run_status": run_status,
        })
    return result


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
# CPU2026 workload-suite tab (INVESTIGATION.md 4.3 Tier 6 "CPU2026
# workload-suite web tab") -- a SPEC CPU2026 counterpart to the Phoronix
# tab above, but structurally simpler: a CPU2026 benchmark and its config
# file both already exist on disk the moment a suite is installed, so
# there's nothing to fetch/materialize from a remote result the way a
# Phoronix test point is. This section's job is discovery (what benchmarks/
# configs exist), registration (pairing a benchmark with a config under
# workload/cpu2026/<bench>/<tag>/, mirroring workload/phoronix/<test>/
# <options>/), and building the runcpu/wspy invocations the tab's Build/
# Use-in-Run-tab buttons trigger.
# ---------------------------------------------------------------------------

# Static catalog for display/grouping only (name, suite component, source
# language) -- from https://spec.org/cpu2026/docs/overview.html#benchmarks.
# What's actually runnable on this host is discovered live from
# $SPECDIR/benchspec/CPU/ (discover_installed_cpu2026_benchmarks()
# below), not this table -- same "static table for labels, filesystem for
# truth" split the Phoronix tab uses between phoronix.tests.txt and live
# `phoronix-test-suite info` calls.
CPU2026_BENCHMARKS = {
    # SPECrate 2026 Integer
    "706.stockfish_r": {"suite": "intrate", "lang": "C++"},
    "707.ntest_r": {"suite": "intrate", "lang": "C++"},
    "708.sqlite_r": {"suite": "intrate", "lang": "C"},
    "710.omnetpp_r": {"suite": "intrate", "lang": "C++, C"},
    "714.cpython_r": {"suite": "intrate", "lang": "C"},
    "721.gcc_r": {"suite": "intrate", "lang": "C++, C"},
    "723.llvm_r": {"suite": "intrate", "lang": "C++, C"},
    "727.cppcheck_r": {"suite": "intrate", "lang": "C++"},
    "729.abc_r": {"suite": "intrate", "lang": "C++, C"},
    "734.vpr_r": {"suite": "intrate", "lang": "C++, C"},
    "735.gem5_r": {"suite": "intrate", "lang": "C++, C"},
    "750.sealcrypto_r": {"suite": "intrate", "lang": "C++, C"},
    "753.ns3_r": {"suite": "intrate", "lang": "C++"},
    "777.zstd_r": {"suite": "intrate", "lang": "C"},
    # SPECspeed 2026 Integer
    "801.xz_s": {"suite": "intspeed", "lang": "C++, C"},
    "807.ntest_s": {"suite": "intspeed", "lang": "C++"},
    "817.flac_s": {"suite": "intspeed", "lang": "C++, C"},
    "821.gcc_s": {"suite": "intspeed", "lang": "C++, C"},
    "823.llvm_s": {"suite": "intspeed", "lang": "C++, C"},
    "827.cppcheck_s": {"suite": "intspeed", "lang": "C++"},
    "829.abc_s": {"suite": "intspeed", "lang": "C++, C"},
    "834.vpr_s": {"suite": "intspeed", "lang": "C++, C"},
    "835.gem5_s": {"suite": "intspeed", "lang": "C++, C"},
    "838.diamond_s": {"suite": "intspeed", "lang": "C++, C"},
    "846.minizinc_s": {"suite": "intspeed", "lang": "C++, C"},
    "853.ns3_s": {"suite": "intspeed", "lang": "C++"},
    "854.graph500_s": {"suite": "intspeed", "lang": "C"},
    # SPECrate 2026 Floating Point
    "709.cactus_r": {"suite": "fprate", "lang": "C++, C"},
    "722.palm_r": {"suite": "fprate", "lang": "Fortran"},
    "731.astcenc_r": {"suite": "fprate", "lang": "C++"},
    "736.ocio_r": {"suite": "fprate", "lang": "C++"},
    "737.gmsh_r": {"suite": "fprate", "lang": "C++, C"},
    "748.flightdm_r": {"suite": "fprate", "lang": "C++"},
    "765.roms_r": {"suite": "fprate", "lang": "Fortran"},
    "766.femflow_r": {"suite": "fprate", "lang": "C++"},
    "767.nest_r": {"suite": "fprate", "lang": "C++"},
    "772.marian_r": {"suite": "fprate", "lang": "C++"},
    "782.lbm_r": {"suite": "fprate", "lang": "C"},
    # SPECspeed 2026 Floating Point
    "800.pot3d_s": {"suite": "fpspeed", "lang": "Fortran"},
    "803.sph_exa_s": {"suite": "fpspeed", "lang": "C++"},
    "809.cactus_s": {"suite": "fpspeed", "lang": "C++, C"},
    "811.tealeaf_s": {"suite": "fpspeed", "lang": "C"},
    "816.nab_s": {"suite": "fpspeed", "lang": "C"},
    "820.cloverleaf_s": {"suite": "fpspeed", "lang": "Fortran"},
    "822.palm_s": {"suite": "fpspeed", "lang": "Fortran"},
    "849.fotonik3d_s": {"suite": "fpspeed", "lang": "Fortran"},
    "857.namd_s": {"suite": "fpspeed", "lang": "C++"},
    "865.roms_s": {"suite": "fpspeed", "lang": "Fortran"},
    "867.nest_s": {"suite": "fpspeed", "lang": "C++"},
    "872.marian_s": {"suite": "fpspeed", "lang": "C++"},
    "881.neutron_s": {"suite": "fpspeed", "lang": "C"},
}


def cpu2026_shrc_path(specdir):
    return os.path.join(specdir, "shrc")


def cpu2026_suite_installed(specdir):
    """True if specdir looks like a real SPEC CPU2026 install (has a shrc
    to source) -- the tab's install-sanity check, same role
    check_phoronix_batch_config() plays for Phoronix."""
    return os.path.isfile(cpu2026_shrc_path(specdir))


def discover_installed_cpu2026_benchmarks(specdir):
    """Benchmark directory names actually present under
    $SPECDIR/benchspec/CPU/, sorted -- the live truth backing the tab's
    inventory, independent of CPU2026_BENCHMARKS above (a benchmark
    installed under a name this static table doesn't know about still
    shows up, just without suite/language labels). Note: despite the
    "CPU2026" suite name, the installed subdirectory is literally named
    "CPU" (confirmed against a real install; CPU2017 uses the same
    "benchspec/CPU/" convention, not "benchspec/CPU2017/")."""
    root = os.path.join(specdir, "benchspec", "CPU")
    if not os.path.isdir(root):
        return []
    return sorted(
        name for name in os.listdir(root)
        if os.path.isdir(os.path.join(root, name)) and re.match(r"^\d{3}\.", name)
    )


def discover_cpu2026_configs(specdir):
    """Config tags (filename minus ".cfg") under $SPECDIR/config/*.cfg,
    sorted -- one config can build any benchmark, so this is discovered
    independently of any particular benchmark, unlike Phoronix's per-test
    option combinations."""
    root = os.path.join(specdir, "config")
    if not os.path.isdir(root):
        return []
    return sorted(
        name[:-4] for name in os.listdir(root)
        if name.endswith(".cfg") and os.path.isfile(os.path.join(root, name))
    )


def cpu2026_benchmark_built(specdir, bench, tag, tune="base"):
    """Best-effort "already built" check: does
    $SPECDIR/benchspec/CPU/<bench>/exe/ contain any file naming this
    config tag and tune? SPEC's exe naming carries machine/OS-specific
    suffixes (confirmed against a real install, e.g.
    "leela_s_base.mev-aocc-7800x3d" / "leela_s_peak.mev-aocc-7800x3d"), so
    this is a substring match against "_<tune>.<tag>" rather than an exact
    reconstructed filename -- cheap disk check now, correctness
    re-verified for real when Build/Use-in-Run-tab actually runs, same
    posture list_installed_phoronix_test_versions() documents for its own
    installed-tests scan. tune-aware so a base-only build doesn't get
    reported as "built" for a peak registration of the same tag, or vice
    versa."""
    exe_dir = os.path.join(specdir, "benchspec", "CPU", bench, "exe")
    if not os.path.isdir(exe_dir):
        return False
    needle = f"_{tune}.{tag}"
    return any(needle in name for name in os.listdir(exe_dir))


def cpu2026_host_specdir(source, hostname=None):
    """Resolves the calling host's own specdir out of a source.json dict's "hosts" map
    (register_cpu2026_point() below) -- a point registered by a different host, or not
    yet registered by this host at all, resolves to "" rather than falling back to
    another host's specdir, which may not exist on this filesystem or (worse) happen to
    resolve to some unrelated directory. Shared by list_materialized_cpu2026_points()
    and web/server.py's Build/Use-in-Run-tab handlers, which read source.json directly."""
    hostname = hostname or socket.gethostname()
    return source.get("hosts", {}).get(hostname, {}).get("specdir", "")


def register_cpu2026_point(dest_root, specdir, bench, tag, tune="base", hostname=None):
    """Pairs an already-installed benchmark with an already-discovered
    config tag under dest_root/<bench>/<tag>/<tune>/ (mirroring
    materialize_phoronix_test_point()'s dest_root/<test>/<options>/), by
    writing a source.json provenance sidecar plus a README.md built from
    the static CPU2026_BENCHMARKS labels (no subprocess/network call
    needed -- unlike Phoronix's `phoronix-test-suite info`, there's no
    live lookup for a SPEC benchmark's description). The <tune> level
    exists because base and peak are separate runcpu builds of the same
    bench+config -- nesting under <tag>/ rather than folding tune into the
    tag name keeps a single config's base/peak pair visually grouped.

    dest_root is this repo's checked-in workload/cpu2026/ (web/server.py's
    CPU2026_DEST_ROOT) -- shared across every host that clones it, unlike a
    per-host runtime cache. specdir is an absolute, host-local SPEC install
    path, so it's recorded per-hostname under a "hosts" map rather than as a
    single flat field: a flat field would silently belong to whichever host
    registered first, leaving every other host's "built" check
    (list_materialized_cpu2026_points()) and Build/Use-in-Run-tab actions
    (web/server.py) resolving a specdir that isn't theirs.

    Idempotent/additive at both levels, same "additive across sessions"
    convention materialize_phoronix_test_point() documents: a hostname
    already present in "hosts" is left untouched (re-registering with a
    different path doesn't silently repoint it -- unregister/re-register
    to do that intentionally); a new hostname is added without touching
    other hosts' entries. Returns {"bench", "tag", "tune", "identity",
    "dir", "status"} where status is "created" (brand-new point),
    "host_added" (existing point, this host's first registration), or
    "exists" (this host already registered)."""
    hostname = hostname or socket.gethostname()
    identity = f"{bench}-{tag}-{tune}"
    out_dir = os.path.join(dest_root, bench, tag, tune)
    source_path = os.path.join(out_dir, "source.json")
    result = {"bench": bench, "tag": tag, "tune": tune, "identity": identity, "dir": out_dir}

    if os.path.isfile(source_path):
        with open(source_path) as f:
            source = json.load(f)
        hosts = source.setdefault("hosts", {})
        if hostname in hosts:
            result["status"] = "exists"
            return result
        hosts[hostname] = {
            "specdir": specdir,
            "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        }
        source["schema_version"] = 2
        with open(source_path, "w") as f:
            json.dump(source, f, indent=2)
            f.write("\n")
        result["status"] = "host_added"
        return result

    os.makedirs(out_dir, exist_ok=True)
    with open(source_path, "w") as f:
        json.dump({
            "schema_version": 2,
            "bench": bench,
            "config_file": f"{tag}.cfg",
            "tune": tune,
            "hosts": {
                hostname: {
                    "specdir": specdir,
                    "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                },
            },
        }, f, indent=2)
        f.write("\n")

    readme_path = os.path.join(dest_root, bench, "README.md")
    if not os.path.isfile(readme_path):
        info = CPU2026_BENCHMARKS.get(bench, {})
        lines = [f"# {bench}", ""]
        if info:
            lines.append(f"SPEC CPU2026 {info['suite']} benchmark, {info['lang']}.")
        else:
            lines.append("SPEC CPU2026 benchmark (not in the built-in catalog table).")
        lines.append("")
        with open(readme_path, "w") as f:
            f.write("\n".join(lines) + "\n")

    result["status"] = "created"
    return result


def resolve_cpu2026_point_dir(dest_root, raw_dir):
    """Same "don't trust client-supplied identity strings" validation as
    resolve_phoronix_test_point_dir(): resolves under dest_root and
    requires a real source.json, or returns None."""
    if not raw_dir:
        return None
    real_dest = os.path.realpath(dest_root)
    real_dir = os.path.realpath(raw_dir)
    if os.path.commonpath([real_dest, real_dir]) != real_dest:
        return None
    if not os.path.isfile(os.path.join(real_dir, "source.json")):
        return None
    return real_dir


def unregister_cpu2026_point(dest_root, point_dir):
    """Removes a registered dest_root/<bench>/<tag>/<tune>/ point --
    counterpart to register_cpu2026_point(). Only removes the
    registration (source.json, README.md, and the runs/ symlinks that
    point at real run directories elsewhere under output_root/cpu2026/)
    -- the actual run data a symlink targets is never touched, so this is
    safe even for a point with recorded runs. Re-validates point_dir the
    same "don't trust client-supplied paths" way resolve_cpu2026_point_dir()
    does (this is a delete, so the check is load-bearing here, not just
    defensive) rather than assuming the caller already resolved it.
    Additionally cleans up now-empty <tag>/ and <bench>/ directories
    (including a bench-level README.md with no config left to document)
    so an unregister doesn't leave an empty shell behind. Returns True if
    something was removed, False if point_dir didn't resolve to a real
    registered point."""
    real_dest = os.path.realpath(dest_root)
    real_dir = os.path.realpath(point_dir)
    if os.path.commonpath([real_dest, real_dir]) != real_dest:
        return False
    if not os.path.isfile(os.path.join(real_dir, "source.json")):
        return False

    shutil.rmtree(real_dir)

    tag_dir = os.path.dirname(real_dir)
    if os.path.isdir(tag_dir) and not os.listdir(tag_dir):
        os.rmdir(tag_dir)

    bench_dir = os.path.dirname(tag_dir)
    if os.path.isdir(bench_dir):
        remaining = os.listdir(bench_dir)
        if remaining in ([], ["README.md"]):
            readme_path = os.path.join(bench_dir, "README.md")
            if os.path.isfile(readme_path):
                os.remove(readme_path)
            os.rmdir(bench_dir)

    return True


def list_materialized_cpu2026_points(dest_root, hostname=None):
    """Inventory of registered (bench, tag, tune) triples under
    dest_root/<bench>/<tag>/<tune>/ -- same shape/role as
    list_materialized_phoronix_test_points(). "built" is recomputed live
    via cpu2026_benchmark_built() against hostname's (defaults to
    socket.gethostname()) own recorded specdir (not cached at registration
    time the way Phoronix's "installed" is) since a Build action changes
    that state after registration, and staleness here would just be wrong
    rather than merely a re-check-later warning. A point registered by a
    different host (or not registered by this one at all) reports
    specdir="" and built=False here rather than resolving some other
    host's path -- see cpu2026_host_specdir()/register_cpu2026_point().
    Returns a list of {bench, tag, tune, identity, dir, config_file,
    specdir, generated_at, built, hosts, runs} -- "hosts" is the raw
    per-hostname map for cross-host visibility, "specdir"/"generated_at"
    are hostname's own entry out of it."""
    hostname = hostname or socket.gethostname()
    entries = []
    if not os.path.isdir(dest_root):
        return entries
    for bench in sorted(os.listdir(dest_root)):
        bench_dir = os.path.join(dest_root, bench)
        if not os.path.isdir(bench_dir):
            continue
        for tag in sorted(os.listdir(bench_dir)):
            tag_dir = os.path.join(bench_dir, tag)
            if not os.path.isdir(tag_dir):
                continue
            for tune in sorted(os.listdir(tag_dir)):
                point_dir = os.path.join(tag_dir, tune)
                source_path = os.path.join(point_dir, "source.json")
                if not os.path.isfile(source_path):
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
                            continue
                        parts = os.path.normpath(target).split(os.sep)
                        if len(parts) < 3:
                            continue
                        real_run_id, run_benchmark, run_suite = parts[-1], parts[-2], parts[-3]
                        runs.append({"run_id": real_run_id, "suite": run_suite, "benchmark": run_benchmark})

                hosts = source.get("hosts", {})
                point_specdir = cpu2026_host_specdir(source, hostname)
                entries.append({
                    "bench": bench,
                    "tag": tag,
                    "tune": tune,
                    "identity": f"{bench}-{tag}-{tune}",
                    "dir": point_dir,
                    "config_file": source.get("config_file", f"{tag}.cfg"),
                    "specdir": point_specdir,
                    "generated_at": hosts.get(hostname, {}).get("generated_at", ""),
                    "hosts": hosts,
                    "built": cpu2026_benchmark_built(point_specdir, bench, tag, tune) if point_specdir else False,
                    "runs": runs,
                })
    return entries


def find_materialized_cpu2026_point(dest_root, identity):
    """The full entry from list_materialized_cpu2026_points() whose "identity" matches, or None if
    no such registered point exists -- same shape/role as find_materialized_phoronix_test_point()."""
    for entry in list_materialized_cpu2026_points(dest_root):
        if entry["identity"] == identity:
            return entry
    return None


def cpu2026_test_point_wp_content(entry):
    """Hand-built WP block markup for one materialized cpu2026 benchmark point's page -- mirrors
    test_point_wp_content()'s Phoronix shape, using this suite's own config_file/tune/built fields
    (list_materialized_cpu2026_points()) instead of Phoronix's test_id/arguments. Used by
    web/server.py's "Publish to WordPress" flow to give an auto-created test-point stub (e.g.
    cpu2026/706.stockfish_r/gcc_O3-base/) real content at publish time."""
    title = html.escape(f"{entry['tag']}-{entry['tune']}")
    config_file = html.escape(entry["config_file"])
    tune = html.escape(entry["tune"])
    built = "yes" if entry["built"] else "no"
    return (
        '<!-- wp:heading {"level":1} -->\n<h1>%s</h1>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:paragraph -->\n<p>Config file: <code>%s</code></p>\n<!-- /wp:paragraph -->\n\n'
        '<!-- wp:paragraph -->\n<p>Tune: <code>%s</code> &mdash; Built: <code>%s</code></p>\n'
        '<!-- /wp:paragraph -->'
        % (title, config_file, tune, built)
    )


def group_materialized_cpu2026_points_by_bench(points):
    """Re-buckets list_materialized_cpu2026_points()'s flat list into one
    entry per benchmark, alphabetically -- the <bench> -> <tag> hierarchy
    the CPU2026 tab's inventory renders. Same shape as
    group_materialized_phoronix_points_by_test(): {"bench", "points",
    "total_count", "built_count", "run_status"}."""
    groups = {}
    for p in points:
        groups.setdefault(p["bench"], []).append(p)
    result = []
    for bench in sorted(groups):
        pts = sorted(groups[bench], key=lambda p: (p["tag"], p["tune"]))
        points_with_runs = sum(1 for p in pts if p.get("runs"))
        if points_with_runs == 0:
            run_status = "none"
        elif points_with_runs == len(pts):
            run_status = "all"
        else:
            run_status = "some"
        result.append({
            "bench": bench,
            "points": pts,
            "total_count": len(pts),
            "built_count": sum(1 for p in pts if p.get("built")),
            "run_status": run_status,
        })
    return result


def link_cpu2026_point_run(point_dir, run_id, rundir):
    """Symlinks <point_dir>/runs/<run_id> -> rundir, identical idiom to
    link_phoronix_test_point_run() -- best-effort, never raises."""
    try:
        runs_dir = os.path.join(point_dir, "runs")
        os.makedirs(runs_dir, exist_ok=True)
        link_path = os.path.join(runs_dir, run_id)
        if os.path.islink(link_path) or os.path.exists(link_path):
            os.remove(link_path)
        os.symlink(os.path.abspath(rundir), link_path)
        return True
    except OSError:
        return False


def build_cpu2026_shell_argv(specdir, inner_cmd):
    """Wraps inner_cmd (a shell command string, e.g. a `runcpu ...`
    invocation) in `bash -c "cd $SPECDIR && source shrc && ulimit -s
    unlimited && <inner_cmd>"` -- runcpu needs SPEC's shrc sourced for its
    environment (SPEC_ROOT/PATH/LD_LIBRARY_PATH etc, see
    workload/cpu2017/run_test.sh's identical preamble), but neither the
    Build action's Popen() nor the Run tab's `workload` field (parsed by
    shlex.split() with no shell, see server.py's _parse_workload_and_ids())
    invoke a shell on their own -- so the sourcing has to be baked into a
    single bash -c argv rather than assumed from ambient environment. `cd`
    MUST happen before `source shrc`, not after: shrc finds its own SPEC
    root by walking up from `$PWD` looking for `bin/harness/runcpu` (see
    its `TEMPSPEC=$(pwd)` loop), not from its own script path -- sourcing
    it while still in the caller's cwd (e.g. the wspy repo, since Popen()
    here sets no `cwd=`) makes it search upward from the wrong directory
    and fail with "Can't find the top of your SPEC tree"."""
    shrc = cpu2026_shrc_path(specdir)
    full = f"cd {shlex.quote(specdir)} && source {shlex.quote(shrc)} && ulimit -s unlimited && {inner_cmd}"
    return ["bash", "-c", full]


def build_cpu2026_build_argv(specdir, config_file, bench, tune="base"):
    """The `runcpu --action=build` invocation behind the tab's Build
    button -- compiles without running, same split cpu2017/run_test.sh's
    own build-then-validate two-step already makes."""
    inner = f"runcpu --config {shlex.quote(config_file)} --action=build --tune {shlex.quote(tune)} {shlex.quote(bench)}"
    return build_cpu2026_shell_argv(specdir, inner)


def build_cpu2026_run_workload(specdir, config_file, bench, tune="base", iterations=1):
    """The Run tab's prefilled `workload` command for "Use in Run tab" --
    `runcpu --action=validate --nobuild` (build already happened via the
    Build button), same action choice workload/cpu2017/run_test.sh makes
    for its own counter-collected pass. Returns a single shell-quoted
    string (shlex.join(), matching shell_preview()'s own quoting) so
    re-parsing it with shlex.split() on submit reconstructs exactly the
    3-token ["bash", "-c", "..."] argv build_cpu2026_shell_argv() built."""
    inner = (f"runcpu --config {shlex.quote(config_file)} --action=validate "
              f"--tune {shlex.quote(tune)} --iterations {iterations} --nobuild {shlex.quote(bench)}")
    return shlex.join(build_cpu2026_shell_argv(specdir, inner))


def execute_cpu2026_build(state, specdir, config_file, bench, tag, point_dir, tune="base"):
    """Runs the Build action as a background subprocess, relaying output
    through the same RunState/SSE machinery execute_analyze() uses --
    builds can run minutes, so this needs a live tail, not run_sync()'s
    bounded synchronous call. Writes a durable build.<tag>.log into the
    registry directory (point_dir) as well as the live relay, since a
    build's own compile output is worth keeping around after the tab is
    closed, same reasoning execute_run() writes a log file for a real
    wspy run."""
    log_path = os.path.join(point_dir, f"build.{tag}.log")
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    argv = build_cpu2026_build_argv(specdir, config_file, bench, tune=tune)
    emit("$ " + shell_preview(argv))
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
    except OSError as e:
        emit(f"[error] failed to launch bash: {e}")
        logf.close()
        state.finish("error", None)
        return

    for line in proc.stdout:
        emit(line.rstrip("\n"))
    rc = proc.wait()
    emit(f"[runcpu build exited {rc}]")
    logf.close()
    state.finish("done" if rc == 0 else "error", None)


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


# wspy-run's zen-portable/zen4plus-deep are themselves composed from other
# builtin profiles via wspy-run's own load_profiles() (hand-derived here,
# not otherwise discoverable from Python without parsing wspy-run's bash) --
# the web launcher only ever submits the single top-level preset name
# ("zen4plus-deep"), never wspy-run's own expanded "deep-cpu,ibs-sample,
# tree-heavy" comma list. Lives here (not just server.py, which also uses
# it for its own IBS/power probe tables) because execute_profile_run() below
# needs it too: its own tree-heavy/gpu-compute detection was checking the
# raw, unexpanded profile string, so a composite preset's embedded tree pass
# never triggered the process-tree-views post-processing step below.
COMPOSITE_PRESET_PROFILES = {
    "zen-portable": ("quick", "ibs-basic"),
    "zen4plus-deep": ("deep-cpu", "ibs-sample", "tree-heavy"),
}


def expand_preset_names(preset):
    """preset.split(',') with each composite name (see COMPOSITE_PRESET_
    PROFILES) expanded to its constituent builtin profiles -- so logic keyed
    on wspy-run's own profile names (e.g. "ibs-sample", "deep-cpu", "tree-
    heavy") sees them even when the request only named the composite."""
    names = []
    for name in (n.strip() for n in preset.split(",")):
        names.extend(COMPOSITE_PRESET_PROFILES.get(name, (name,)))
    return names


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
            full_argv = ["timeout", "--foreground", str(p["timeout"])] + full_argv
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
    # its own --tree pass. expand_preset_names() (not a bare .split(",")) so
    # a composite preset composed of tree-heavy/gpu-compute (zen4plus-deep,
    # future composites) is recognized too -- the web launcher only ever
    # submits the composite's own name, never its expanded pass list.
    profile_names = expand_preset_names(profile)
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
            full_argv = ["timeout", "--foreground", str(p["timeout"])] + full_argv
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

JOB_SCHEMA_VERSION = "1.1.0"
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
              affinity=None, phoronix_test_point=None):
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
    same as any other under-provisioned replay target). phoronix_test_point,
    if given, is a path *relative to workload/phoronix/* (e.g.
    "coremark/default") identifying the materialized test point this run
    was launched against -- deliberately not an absolute path, so the job
    stays portable across machines the same way everything else here does;
    server.py's Handler._phoronix_test_point_identity() computes it and
    wspy-queue's process_job() re-resolves it under the *processing*
    machine's own workload/phoronix/ before calling
    link_phoronix_test_point_run()."""
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
        "phoronix_test_point": phoronix_test_point or None,
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

    phoronix_test_point = job.get("phoronix_test_point")
    if phoronix_test_point is not None and not isinstance(phoronix_test_point, str):
        errors.append("phoronix_test_point, if given, must be a string")

    return errors
