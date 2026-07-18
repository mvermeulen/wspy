"""
web/joblib.py -- shared run-building logic for web/server.py and wspy-queue
(INVESTIGATION_4.0.md's "What shipped in 4.1", "Deployment/hosting design note").

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

Three groups of things live here:
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
"""
import copy
import json
import os
import re
import secrets
import shlex
import subprocess
import threading
from datetime import datetime, timezone

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
PROFILE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]+$")

# Core/thread affinity control (INVESTIGATION_4.0.md's "Core/thread affinity
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
SYSTEM_COLUMN_NAMES = {"load", "runnable", "cpu", "idle", "iowait", "irq", "freq"}
# --power's own columns (power.c/topdown.c's print_power()) -- same "not an
# ALL_GROUPS entry" reasoning as SYSTEM_COLUMN_NAMES above: --power isn't a
# counter_mask bit build_configuration_passes()'s "counters" section
# handles, it's its own checklist card (see that section below).
POWER_COLUMN_NAMES = {"pkg_joules", "pkg_watts"}


def resolve_column_group(column_name):
    """Returns the ALL_GROUPS name (or the "system"/"power" sentinel) whose
    --flag must be enabled to produce column_name in a wspy CSV, or None if
    column_name isn't a column this tool recognizes (a typo, or a
    workload-specific name nothing here can auto-detect)."""
    if column_name in SYSTEM_COLUMN_NAMES or column_name.startswith("net "):
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
        power = checklist.setdefault("power", {})
        if not power.get("enabled"):
            power["enabled"] = True
            notes.append("auto-enabled 'CPU power' for a custom plot")
        if not str(power.get("interval_secs") or "").strip():
            power["interval_secs"] = "1"
            notes.append("auto-set a 1s interval on 'CPU power' so the custom plot has a "
                          "time series to chart")
        power["csv"] = True

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


def _config_options(section):
    """Turns one checklist category's raw sub-dict (e.g. checklist["counters"])
    into the launcher-vocabulary (name,value) pairs recorded via wspy's
    --config-option (INVESTIGATION_4.0.md's "What shipped in 4.1", structured
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


# Structured configuration provenance's (INVESTIGATION_4.0.md's "What shipped
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
    "counters": {"per_core", "rusage", "csv"},
    "system": {"csv"},
    "gpu": {"busy", "metrics", "smi", "csv"},
    "ibs": set(),
    "power": {"csv"},
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
    dicts, in the fixed tree/counters/system/gpu/ibs/power order, one per *enabled
    and non-empty* configuration (an enabled configuration with nothing
    meaningful selected, e.g. "counters" with no groups checked, is silently
    skipped rather than producing a no-op wspy invocation). "category" is
    the launcher-vocabulary configuration name (item 16's structured
    configuration provenance, --config-name) -- stable across the legacy
    "amdtopdown"/"systemtime" pass-name aliases below, unlike "name" (the
    output filename stem), which is not."""
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
        flags.append("--software" if tree.get("software", True) else "--no-software")
        flags.append("--no-ipc")
        timeout = parse_optional_int(tree.get("timeout_secs"), 1, 86400)
        passes.append({"name": "tree", "category": "process-tree",
                        "options": _config_options(tree),
                        "flags": flags, "csv": False, "timeout": timeout})

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
            passes.append({"name": name, "category": "performance-counters",
                            "options": _config_options(counters),
                            "flags": flags, "csv": csv, "timeout": None})

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

    power = checklist.get("power") or {}
    if power.get("enabled"):
        interval = parse_optional_int(power.get("interval_secs"), 1, 3600)
        csv = bool(power.get("csv", True))
        flags = ["--power", "--no-ipc", "--no-rusage", "--no-software"]
        if interval is not None:
            flags += ["--interval", str(interval)]
        if csv:
            flags.append("--csv")
        passes.append({"name": "power", "category": "power",
                        "options": _config_options(power),
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
    (INVESTIGATION_4.0.md's "What shipped in 4.1"), the checklist's own vocabulary rather
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


def run_proctree_besteffort(emit, cfg, rundir, cmdline=False, futex=False, io=False, io_wait=False,
                             schedstat=False, vmsize=False, connect=False, wait=False, poll=False, nanosleep=False):
    """Best-effort trailing step mirroring the wspy-plot step (build_plot_argv()
    above) but for --tree's raw process.tree.txt record: renders it into a
    human-readable process.tree.summary.txt via proctree. A no-op (not an
    error) when no --tree pass ran this time, or its output is missing/empty
    (e.g. a --tree pass that timed out before writing anything) -- and never
    fails the run itself, same degrade-don't-fail idiom as the plot step."""
    tree_txt = os.path.join(rundir, "process.tree.txt")
    if not (os.path.isfile(tree_txt) and os.path.getsize(tree_txt) > 0):
        return
    summary_path = os.path.join(rundir, "process.tree.summary.txt")
    argv = build_proctree_argv(cfg["proctree_bin"], tree_txt, cmdline=cmdline, futex=futex, io=io, io_wait=io_wait,
                                schedstat=schedstat, vmsize=vmsize, connect=connect, wait=wait, poll=poll, nanosleep=nanosleep)
    emit("$ " + shell_preview(argv) + f" > {os.path.basename(summary_path)}")
    try:
        with open(summary_path, "w") as outf:
            proc = subprocess.run(argv, cwd=REPO_ROOT, stdout=outf,
                                   stderr=subprocess.PIPE, text=True)
        if proc.returncode != 0:
            emit(f"[error] proctree exited {proc.returncode}: {proc.stderr.strip()}")
        else:
            emit(f"[wrote {os.path.basename(summary_path)}]")
    except OSError as e:
        emit(f"[error] failed to launch proctree ({cfg['proctree_bin']}): {e}")


def shell_preview(argv, cwd=None):
    s = shlex.join(argv)
    if cwd:
        return f"(cd {shlex.quote(cwd)} && {s})"
    return s


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

    # tree-heavy is currently the only builtin profile with a --tree pass, and
    # its flags are fixed in wspy-run's load_builtin_profile() (--tree-cmdline)
    # -- not discoverable from here without shelling out and parsing
    # wspy-run's own bash config, so this mirrors that fixed choice directly.
    # Update alongside load_builtin_profile() if that ever changes.
    if "tree-heavy" in profile.split(","):
        run_proctree_besteffort(emit, cfg, rundir, cmdline=True)

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
# Job files (INVESTIGATION_4.0.md's "What shipped in 4.1", "Deployment/hosting design note").
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
    feedback item, see INVESTIGATION_4.0.md line ~336), resolved against a
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
