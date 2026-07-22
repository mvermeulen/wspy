# wspy Investigation

A rolling roadmap: what's shipped, what's actively planned, and the design reasoning behind both.
Formerly `INVESTIGATION_4.0.md` — renamed once its content outgrew a single release (4.0, 4.1, and
now 4.2 are done; see below). Full design write-ups and validation narratives for work that's fully
shipped now live in `doc/INVESTIGATION_ARCHIVE.md`, out of the way of the open backlog.

Status (2026-07-21): **4.0 and 4.1 are released and done. 4.2's full scope has shipped** (see "What
shipped in 4.2" below), `./run_tests.sh` passes cleanly as the final release-prep check, and
`wspy-release-notes.4.2.md` is ready as the v4.2 GitHub release body — nothing was carried forward
as open backlog; tagging/publishing the release itself is the one remaining manual step (see
`scripts/release_prep.sh`). This document was slimmed down for the 4.3 cycle (2026-07-21): "What
shipped in 4.0"/"4.1"/"4.2" are pointer lists only, with design write-ups and validation narratives
moved to `doc/INVESTIGATION_ARCHIVE.md`. 4.3 and 4.4 are planned, not started.

## Purpose
This document captures ideas for improvements focused on making benchmark collection, organization,
and publication easier and more repeatable.

## How to use this document
- "What shipped in 4.0" / "What shipped in 4.1" / "What shipped in 4.2" are pointer lists, not feature
  logs — `CLAUDE.md` documents each module's actual behavior in detail, `doc/INVESTIGATION_ARCHIVE.md`
  holds full design/validation write-ups for fully-shipped work, and `git log` has history. Don't
  restate mechanism here, link to it.
- **When an item ships:** move it out of its phase's open backlog and into that phase's "Shipped"
  rollup as a one- or two-sentence pointer (name the file/tool, not the mechanism). If its design
  merited a multi-paragraph write-up while it was being built, move that write-up to
  `doc/INVESTIGATION_ARCHIVE.md` rather than leaving it inline — the open backlog should only ever
  contain open work. Once a phase's backlog empties out entirely, fold its rolling "Shipped since
  <prior phase>" section into a proper "What shipped in <this phase>" section (as happened for 4.2) —
  this is real editorial judgment done at release-prep time, not automatic the moment the last item lands.
- **Cross-references are by name, not number.** Item numbers inside a single tier list are fine as a
  local index, but don't reference an item elsewhere in this file (or from `CLAUDE.md`/commit
  messages) as "4.2 #27" — describe it by name instead ("AMD IBS sampling-mode support"). Numbers shift
  every time a tier is reorganized; names don't.
- "4.3 priorities" / "4.4 priorities" are ordered backlogs, one per phase, grouped into dependency
  tiers (earlier tiers unlock later ones within the same phase). Add or reorder an item there rather
  than inventing a parallel table.
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, topdown, the
  preset/configuration/option vocabulary). Each points back at the priority-list items it informs.
  Deep-dives for work that has since fully shipped live in `doc/INVESTIGATION_ARCHIVE.md`, not here.
- "Open questions" carry a recommendation each; re-open one by editing its entry, not by appending new
  prose elsewhere in the file.

## What shipped in 4.0
Grouped by the same track names `CLAUDE.md` and PR history use, so existing cross-references still
resolve. This is a pointer list — see `CLAUDE.md`'s entry for each named file for actual behavior.

**Run artifact foundation:** run manifest + SemVer schema (`manifest.c`, `MANIFEST_SCHEMA_VERSION`);
run index (`run_index.c`, JSONL, `RUN_INDEX_SCHEMA_VERSION`); profile-driven launcher (`wspy-run`:
builtin profiles including `deep-cpu-intel`, `-c <file>` config execution, comma-composed profiles,
per-pass timeouts); pre-publish validation (`wspy-validate`/`validate.c`); coverage ledger
(`wspy-ledger`/`ledger.c`); unified output layout (`wspy-run --suite/--benchmark`, `doc/
ARTIFACT_CONTRACT.md`'s "Unified output layout" section); `workload/cpu2017`, `workload/phoronix`,
and `workload/pbbsbench` migrated to call `wspy-run` instead of hand-rolling per-suite invocations.

**Reproducibility, comparability, statistics:** counter capability discovery + coverage reporting
(`coverage.c`, `wspy --capabilities`); environment/provenance capture (`provenance.c` — virt role,
microcode, BIOS, governor, memory, toolchain).

**Topdown quality:** confidence envelope + decomposition sanity checks on the level-1 breakdown
(`topdown.c`'s `print_topdown()`).

**Zen5 / IBS:** capability-driven IBS probing (`ibs.c`, readdir-driven sysfs discovery); `ibs-basic`/
`ibs-memory-deep` collection profiles with sampling-skew/quality annotations.

**Process / `getrusage` / `/proc` telemetry:** CSV/normal output parity fix for `print_usage()`;
expanded `getrusage` coverage (`maxrss`/`minflt`/`majflt`/`nswap`).

**Existing-capability extensions:** counter-fit preflight (`preflight.c`, `wspy --preflight`);
interval automatic phase-boundary detection (`phase.c`, `phase` CSV column + boundary summary).

**Portability and robustness:** opt-in child exit status propagation (`--exit-with-child`);
arch-neutral `ptrace` register access (`ptrace_arch.h` — both x86_64 and `__aarch64__` branches
are fully verified and validated on real hardware); run-index schema validation on ingest
(`wspy-ledger`); collector-plugin schema seam (`manifest.h`/`run_index.h`'s `collector` field,
default `"wspy"` — no non-wspy collector implementation exists yet, that's real 4.3+ scope).

**AMD GPU track:** dynamic GPU path scan (`amd_sysfs_scan_devices()`, replacing the old `card1`
hardcode); `--gpu-device=<idx>` override + full multi-GPU enumeration across both the sysfs and SMI
backends. GPU *kernel*-level instrumentation (CUDA/Vulkan profiling — tracing individual compute
kernels/shaders, not point-in-time busy%/VRAM monitoring) is **cut from this roadmap** (project scope
decision, 2026-07-08) — this codebase has no CUDA/Vulkan profiling code; revisit only if the project's
mission changes to include GPU kernel-level profiling.
**Revised 2026-07-18:** that decision was specifically about kernel-level instrumentation, not
cross-vendor GPU *monitoring* — the narrower, AMD-parity capability (busy%/VRAM via a vendor
management API, exactly what `amd_smi.c`/`amd_sysfs.c` already do for AMD) shipped as `--gpu-nvidia`
(`nvidia_nvml.c`, `NVIDIA=1` build flag): NVML is `dlopen()`d at runtime rather than linked at build
time, so unlike the AMD path there's no ROCm-equivalent header/toolkit dependency at build time. See
`CLAUDE.md`'s "GPU support" section and `nvidia_nvml.c`'s entry in the Architecture list for detail.

**Testing and documentation:** golden output-contract tests + capability-matrix smoke tests
(`tests/golden_output.sh`, `tests/capability_matrix.sh`) — building these surfaced and fixed five
independent, pre-existing output-contract bugs and one crash (see `CLAUDE.md`'s "Build & Test" for
specifics, not repeated here); `doc/ARTIFACT_CONTRACT.md` artifact-contract doc + troubleshooting
runbook. `--per-core` CSV column-count mismatch fixed: `wspy.c`'s `per_core_csv` re-architects the
aflag/csv print flow into one row per active core (a `core` column identifies which), so header and
row column counts now match like any other flag combination; `--per-core` combined with `--interval`
still keeps the old, separately-caused mismatch (`timer_callback()` never reads per-core counters —
see `wspy.c`'s `per_core_csv` comment and `doc/ARTIFACT_CONTRACT.md`'s CSV section).

## What shipped in 4.1
Grouped the same way as "What shipped in 4.0" above — a pointer list, not a feature log. See
`CLAUDE.md`'s entry for each named file/tool for actual behavior, and git history for how each piece
evolved.

**Normalized store & reporting:** canonical SQLite schema + idempotent ingest (`wspy-store`/`store.c`,
`STORE_SCHEMA_VERSION` — run catalog plus a long/tall `metric_values` table parsed from each run's
CSV, covering aggregate/`--interval`/`--per-core` shapes uniformly); summary table generator
(`wspy-summary`/`summary.c` — min/max/mean/median/stddev/outlier flags per `(group,metric)` bucket,
grouped by command/hostname/`cpu_vendor`, `--show-runs`/`--trace` traceability back to
manifest/CSV/tree/plot artifacts); shared plotting templates (`wspy-plot`/`plot.c` — gnuplot-rendered
PNGs from any wspy CSV via column-identity matching, replacing the old per-suite `gnuplot.sh` script,
plus `--plot`/`--only-custom` for hand-picked groupings).

**Multi-pass counter execution:** `wspy --passes=<groups>` (`multipass.c`) bin-packs requested
counter groups into automatically-sized passes and merges the result into one CSV/manifest/run-index
record instead of requiring N separate `wspy` invocations; `--multiplex` trades that bin-packing for
a single oversubscribed pass. Paired with a correctness fix: `read_counters()` (`topdown.c`) now
scales every counter's raw *value* (not just its confidence envelope) by that read's multiplex ratio,
so an oversubscribed run no longer silently undercounts.

**Web launcher & report browser (`web/server.py`):** Run/Validate/Store & Summary/Discovery tabs; a
preset-dropdown-or-configuration-checklist launcher with live SSE-streamed output and an
always-shown, copy/paste-able command line; a report page with a curation studio (reorderable,
per-block + whole-report commentary, `none`/`summary`/`excerpt`/`full` inclusion depth) and a
multi-run compare view; publish-ready export (WordPress block markup / self-contained HTML /
Markdown); a historical run browser/search page (`/history`); an estimated-runtime "Check" button
(perf/`nmi_watchdog` sysctls, Phoronix runtime estimates, and a real AMD IBS `perf_event_open()`
probe before launching).

**Structured configuration provenance:** `--preset-name`/`--config-name`/`--config-option <k>=<v>`
(`wspy.c`, metadata-only) record which named preset/configuration/option a front end chose, threaded
through `manifest.h`/`run_index.h`'s `configuration_provenance` object
(`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` `1.4.0` → `1.5.0`); the report browser's
"customize & run again" reads it back to restore exact launcher state (preset or full checklist) from
a report instead of re-parsing a flat command line.

**Deployment / job queue:** a portable, spec-only **job** file format (`web/joblib.py`'s
`build_job()`/`validate_job()`) captured before any run directory exists, processed through a
`pending`/`running`/`done`/`failed` lifecycle by `wspy-queue` (standalone CLI, headless, no
dependency on the web server) or created from the Run tab's "Queue instead of running it now"
checkbox; a job file can be copied to a second machine with wspy checked out and processed there.

## What shipped in 4.2
4.2's full scope has shipped — nothing was carried forward as open backlog. Grouped the same way as
"What shipped in 4.0"/"What shipped in 4.1" above — a pointer list, not a feature log. Full design and
validation detail for every item below lives in `doc/INVESTIGATION_ARCHIVE.md`.

**Critical-path/synchronization-latency instrumentation:** all six originally-scoped syscall-latency
candidates (`--tree-futex`, `--tree-io`/`--tree-io-wait`, `--tree-connect`, `--tree-nanosleep`,
`--tree-wait`, `--tree-poll`), plus `--tree-schedstat` (run-delay/timeslice) and `--tree-vmsize` (peak
RSS/composition/swap) — together giving a degraded interval phase a three-way explanation: blocked in
the kernel, runnable but not scheduled, or a genuine on-CPU stall.

**Core/thread affinity control:** `--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|
cpuset=<c0,...>` (`affinity.c`) — topology discovery, `--list-affinity`, manifest/run-index provenance,
`wspy-run`/web launcher/`wspy-queue` wiring. Shipped ahead of its originally-planned phase.

**AMD IBS:** real-Zen5-hardware validation fixed a MaxCnt/`sample_period` bug (4.1) and a
below-hardware-minimum `ldlat` default (4.2); `ibs-basic`/`ibs-memory-deep` default to `--interval 1`
with real gnuplot PNGs; `zen-portable`/`zen4plus-deep` builtin preset packs; the web launcher's Check
button gained a live `perf_event_open()` probe.

**CPU energy/power:** `--power`/`--no-power` (`power.c`) reports package `pkg_joules`/`pkg_watts`, plus
per-core `core_joules`/`core_watts` under `--power --per-core` — dedicated web launcher card,
custom-plot column autofit, a live `EACCES`-aware Check-button probe.

**Hierarchical topdown schema (L1→L2→L3):** `print_topdown()`'s L2 breakdown reaches CSV as 9 new
trailing columns; `--topdown-backend`'s L3 detail (`print_topdown_be()`) joins the same denominator via
5 more. `TOPDOWN_FORMULA_VERSION` recorded in the manifest/run-index; matching `wspy-plot` templates.

**GPU support:** ROCm SMI + sysfs fusion layer (`--gpu-metrics` merges both backends, per-metric source
tracking); GPU telemetry provenance in the manifest/run-index; `gpu-compute` builtin profile (tree +
system + power + both GPU backends + topdown on one `--interval` timeline); CPU temperature
(`SYSTEM_TEMP`); GPU-aware plot templates; multi-device enumeration validated on a real AMD+NVIDIA host.

**PMU-capability-aware comparability warnings:** `wspy-summary`'s repeatability verdict gains a
`mixed-pmu` reason, flagging a bucket whose contributing runs differ in CPU vendor or
requested/measured counter coverage.

**System-wide metrics:** `SYSTEM_DISK` (per-block-device read/write/time) and `SYSTEM_MEM` (host-wide
free/cached/dirty/writeback/swap/committed) — both default-on under `--system`; cgroup v2 identity,
resource limits, and `cpu.stat` throttling deltas in the manifest/run-index (`cgroup.c`).

**Per-core diagnostics:** `wspy-core-report` (cross-core min/max/mean/stddev/CV, hot/cold core,
core-class breakdown) plus AMD Zen5/Zen5c core detection (`cpu_info.c`) so that class breakdown
actually fires on hybrid AMD parts; `--per-core-freq` for live per-core cpufreq reading.

**`proctree` JSON export + interactive viewer + diff:** `proctree --json`/`--diff`, a web tree viewer
and tree-diff page.

**ARM64:** real CPU topology/topdown/ptrace support (not just register-access prep), validated on real
ARM64 hardware.

**Local LLM (Ollama) narrative analysis:** `wspy-analyze` turns already-computed/validated numbers into
prose via a local model — versioned prompt templates, multi-model sweep + critique, curation-studio
integration with an always-visible "AI-generated" marker, comparative mode.

**Feature normalization + archetype scorecard:** `wspy-store` derives a coverage-aware feature
vocabulary (`run_features`); `wspy-archetype` classifies a run along four axes (resource dominance,
parallelism shape, control-flow style, runtime stability) with a confidence level, grounded in prior
workload-clustering research.

**Comparison matrix mode:** `wspy-summary --group-by`/`--group-by-option`; the new `wspy-sweep` tool
cross-products `--affinity` values against workloads.

**Compare-view curation (Phase 1):** `GET /compare` gained an optional overview/per-row annotation
layer (`compare.json`).

**Release engineering & documentation tooling:** `scripts/release_prep.sh` (repeatable release
checklist), `tests/doc_version_check.sh` (doc/version drift check, wired into `run_tests.sh`),
`doc/PROFILE_COOKBOOK.md` (verdict/confidence/phase interpretation guide), `wspy-bundle` (checksummed
reproducibility-bundle export), and `wspy-run`'s `--tree` pass timeout sized from an actual Phoronix
run-time estimate.

**Testing:** `wspy-store`'s schema-migration/idempotency coverage (`test_store.c`).

**Correctness fixes found via real use:** `wspy-ledger` no longer permanently misreports a workload's
status after its output directory is deleted; a `--gpu-smi --interval` CSV column-count gap; an
`--interval` tail-print/last-tick `SIGALRM` race; `deep-gpu` missing `--power`; the web launcher's
custom GPU checklist missing an NVIDIA checkbox; an `--capabilities` AMD sysfs device-selection marker
bug found on a real multi-GPU host; a below-hardware-minimum AMD IBS `ldlat` default.

**Dropped, not deferred:** "Deeper Phoronix Test Suite awareness in the web UI" — its article-scraping
sub-item conflicts with Phoronix's site use policy; the rest of its motivation is covered by
`wspy-ledger`'s existing workload-coverage tracking.

## Known gaps (still open)
Real-hardware/real-scale validation this project's hand-testing hasn't covered yet. Not release
blockers — just don't assume these are confirmed:
- **AMD IBS `fetchlat` threshold minimum unverified:** unlike `ibs_op`'s `ldlat` field (see above,
  now fixed), no host available during that validation exposed a working `fetchlat` sysfs format
  field on `ibs_fetch` to test whether it has an analogous hardware-enforced minimum below
  `IBS_DEFAULT_FETCHLAT_THRESHOLD`'s current value of 120. Worth the same bit-sweep treatment once
  hardware with a live `fetchlat` field is available.

## Track deep-dives
Reasoning that doesn't fit a single backlog line, for tracks with genuinely open work. Deep-dives for
work that's since fully shipped (blocking I/O, schedstat, vmsize, connect/wait/poll/nanosleep, power,
the LLM narrative-analysis design, the full critical-path-instrumentation candidate rationale,
repeatability policy/confidence metadata, and comparison matrix mode) have moved to
`doc/INVESTIGATION_ARCHIVE.md`.

### Zen5/IBS deep-dive
What appears confirmed from current Linux perf/PMU behavior for AMD Family 1Ah (Zen5):
1. Zen5-specific IBS load-latency filtering enables L3-miss-only filtering via a Zen5 feature check.
2. Generic `PERF_COUNT_HW_*` mapping on Family 1Ah still follows the Zen4 event-map path in current
   kernel PMU logic — there isn't yet a distinct "Zen5-only" generic hardware-event map.
3. IBS capability extensions (L3-miss-only, load-latency/fetch-latency filters, richer memory-source
   decoding) are the strongest near-term source of additional signal.
4. L3-miss-only filtering is documented to skew sampling-period behavior — runs using it need
   explicit annotation (shipped — see `topdown.c`'s `print_ibs()`).
5. Zen5's topdown dispatch baseline shifted from Zen4's 6 slots/cycle to 8 — already implemented
   (`topdown.c`'s `CORE_AMD_ZEN`/`CORE_AMD_ZEN5` slot-multiplier branch). But the finer per-scheduler
   breakdown events AMD introduced alongside that width change aren't in `amd_raw_events[]` yet:
   split ALU/AGU scheduler-stall counters, and op-cache/execution-queue events that would separate
   `Frontend Latency` from `Frontend Bandwidth`. `IBS_LD_L1_DTLB_REFILL_LAT` also isn't named
   anywhere in the IBS capability-probing rows. Both are candidate inputs for a future "platform
   formula registry" (see the Topdown deep-dive's item 8) once Zen5-specific formulas are actually
   versioned there — no standalone backlog item yet.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Informed 4.2's "Zen-family preset packs" and "PMU-capability-aware comparability warnings" (both
shipped). Also informs 4.3's "AMD IBS sampling-mode support" (moved to the front of 4.3, see that
section — icache/TLB/dcache/L2/L3/branch rate estimates decoded from real per-sample tag data, not just
counting-mode sample counts) and that same phase's "IBS-derived memory-path bottleneck decomposition,"
which depends on it existing first.

### Topdown deep-dive
Advancements worth adopting, in priority order for `wspy` specifically:
1. ~~Multiplex-aware confidence~~ — shipped (see "What shipped in 4.0").
2. ~~Decomposition consistency/sanity checks~~ — shipped alongside #1.
3. ~~Hierarchical (L1→L2→L3) parent/child schema with explicit raw-vs-contention-adjusted
   denominators~~ — shipped, including L3 (folding `--topdown-backend`'s own detail in), see
   "What shipped in 4.2"'s "Hierarchical topdown schema".
4. ~~SMT/contention-aware normalization — publish both denominators, document which one drives
   classification~~ — shipped alongside #3 (`contention_pct` CSV column; every other percentage
   documented as a fraction of `slots_no_contention`).
5. Phase-aware topdown (warmup/steady/degraded) — depends on interval phase segmentation (shipped,
   `phase.c`).
6. Hybrid/heterogeneous core-class summaries — don't mix Atom+Core topdown into one headline number.
7. Cross-signal attribution (topdown + cache/TLB/IBS) — composite bottleneck rules over single-
   counter heuristics.
8. Platform formula registry — versioned event/formula mapping per CPU family/model, for
   auditability.

**MVP acceptance criteria** (still the right bar):
- ≥95% of topdown fields in standard profiles include confidence metadata. **Met** for the level-1
  breakdown (shipped).
- Reports clearly mark low-confidence topdown rows and avoid strong claims from them. **Met.**
- One benchmark run demonstrates phase-specific topdown shifts in generated summary output. **Not
  met** — phase detection exists (`phase.c`) but nothing correlates it with topdown output yet
  (items 5/6 above, and 4.3's "Phase-aware topdown" entry).

→ Items 3-8 map to 4.2's "Hierarchical topdown schema" (shipped) and 4.3's "Phase-aware topdown,"
"Composite attribution," and "Core-class-aware topdown" (moved to 4.3, blocked on hardware access).

### Preset / Configuration / Option hierarchy deep-dive
A three-level vocabulary for describing what wspy can be asked to do, surfaced while iterating the
4.1 web-interface mockup (2026-07-11) — the goal is for the CLI, `wspy-run`, and the web UI to
describe the same thing the same way, rather than each inventing its own mental model.

- **Configuration** — one thing wspy can be asked to measure, typically corresponding to a single
  wspy run/operation (though a run can combine more than one). Examples: a process tree, an interval
  measurement of performance counters, an interval measurement of other system metrics, an overall
  (non-interval) system measurement, an overall performance-counters measurement. Each configuration
  has a natural output representation — a table, a gnuplot, a tree diagram — which is part of what
  makes it a distinct configuration rather than a variant of another one.
- **Option** — a way to customize a configuration. Options apply to specific configurations, not
  universally: an interval-seconds option only makes sense on an interval configuration;
  `--tree-cmdline` only makes sense on a process-tree configuration.
- **Preset** — a configuration, or combination of configurations, common enough to deserve a name.
  Presets are exactly the things that showed up hand-rolled in `workload/*/run_test.sh` before 4.0 and
  are now `wspy-run --profile` entries. Presets can be hierarchical — a preset can itself specify
  particular configurations at particular option values (`deep-cpu` selects several configurations at
  fixed options). Not every reachable configuration/option combination has, or needs, a preset name;
  presets are the well-worn paths, not the full space.

The load-bearing rule this implies: **a preset names a configuration+option combination `wspy-run`
already knows how to run; the moment a preset's options are customized away from what it names, the
result has left the set of things `wspy-run --profile` can express, and has to run as one or more
direct `wspy` command lines instead.** This isn't a new rule invented for the web UI — it's the same
fatal-combination behavior the real CLI already has (`--passes`, which is what `wspy-run`'s profiles
bin-pack onto, rejects `--interval`/`--per-core`/`--tree`/IBS/GPU flags outright; see `CLAUDE.md`'s
`wspy.c` entry). The mockup's "customize a checkbox → separate command lines with an explanatory note"
fallback (shipped in the web launcher's Run tab) is this rule, discovered bottom-up from the real
constraints before being named top-down here. Worth treating as the general rule going forward rather
than a fact specific to counters/tree/interval, since it will recur every time a new configuration or
option is added.

Realized as shipped 4.1 features (see "What shipped in 4.1" above): the web launcher's preset-first
framing with a live "customized away from preset" indicator, and structured configuration provenance
recording which preset/configuration/option choice actually produced a run so a report can say "this
was `deep-cpu`, with the TLB group swapped for L3" rather than re-deriving it from a flat argv.

Cross-cutting goal, not yet committed to: the same preset/configuration/option vocabulary should
eventually describe `wspy`'s own CLI options (today an unstructured flat flag list) and `wspy-run`'s
profile format (today hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in `load_builtin_profile()`), not
just the web UI. Nothing here commits to that refactor — see "Open questions for prioritization" below
— but this is the vocabulary to design against as any later CLI/`wspy-run` restructuring proceeds, so
it doesn't independently invent a different model for the same thing. There is real leeway to adjust
existing options/commands toward this if it produces a cleaner architecture.

### Critical-path / synchronization-latency: what's left
All six originally-scoped syscall-latency candidates (futex, blocking I/O, connect, nanosleep, wait,
poll) have shipped — see "What shipped in 4.2" above and `doc/INVESTIGATION_ARCHIVE.md` for the full
motivation and per-syscall design rationale. What remains open from this track:
- The *general*, table-driven mechanism (`tree_open`'s "syscall name → number → decode →
  log-vs-aggregate" generalization, 4.3's "General syscall-level critical-path instrumentation" entry)
  was deliberately not built — six syscall families were still cheap enough as individual `if`
  branches in `ptrace_loop()`'s dispatch. Revisit only if a seventh syscall family comes up.
- `ptrace` itself imposes a real stop-the-world cost on every syscall of the traced process, so
  absolute latency numbers collected this way are inflated relative to an untraced run. The *relative*
  split (fraction of wall time in futex-wait vs. read-wait vs. on-CPU) stays informative even when
  absolute numbers are skewed, but this is an inherent limitation of the mechanism — 4.3's "Low-overhead
  tracing alternative to ptrace" entry is the eventual fix, not a documentation note.

## 4.3 priorities
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and a lower-overhead tracing backend.

**Tier 0 — Intel counter-grouping correctness bugs (moved to the front of 4.3, 2026-07-22): real Intel
hybrid hardware became available (a Raptor Lake HX host, "carlsbad") and immediately surfaced a cluster
of confirmed, hardware-verified bugs in how `topdown.c` groups Intel `perf_event_open()` calls — these
predate this investigation (the shared-group design dates to commit `273e9af`, Dec 2023) and were never
caught because no Intel hardware was available to exercise them until now. Ahead of the IBS work below
since these are outright incorrect-data/broken-feature bugs on a whole vendor, not new capability:**

0.1. **[FIXED, uncommitted on `master`] `--per-core` on Intel silently measured only the first core.**
   `topdown.c`'s `intel_group_id` (a module-static "current Intel perf-group leader fd") was scoped to
   outlive a single `setup_counters()` call, reused across every subsequent call until `close_counters()`
   resets it — fine for the single-pass aggregate path (one call, ever), but `--per-core`'s setup loop
   (`wspy.c` `main()`, ~line 1718) calls `setup_counters()` once per eligible core, back to back, with no
   `close_counters()` in between (core fds all need to stay open simultaneously so `start_counters()` can
   start them together). Every core after the first therefore tried to open its own hardware counters as
   members of a perf group led by a *different CPU's* fd (core 0's) — the kernel requires group members to
   share their leader's cpu/task target, so every one of those opens failed with `EINVAL`. Confirmed live:
   `./wspy --per-core --csv -- sleep 1` on the 13950HX measured real IPC on core 0 only; cores 1-15
   (the P-core set `core_is_per_core_eligible()` allows) all read `-nan`, `counters_measured=2/32`
   (2 = core 0's own 2 counters; every other core's 2 attempts failed and were never re-counted). This
   silently broke **every currently-possible Intel `--per-core` invocation** — `core_is_per_core_eligible()`
   only allows `CORE_INTEL_CORE`, which only the 5 hybrid-capable models `cpu_info.c` recognizes ever get
   classified as, so there was no Intel host configuration where `--per-core` + more than one core actually
   worked. Fix (already applied to `topdown.c`, verified against real hardware — all 16 P-cores now report
   real per-core IPC, and `test_wspy`/full `make test` still pass): reset `intel_group_id = -1` at the top
   of every `setup_counters()` call, so each call's list forms its own self-contained group instead of
   inheriting a leader fd bound to a different call's cpu/task target. Bonus side effect confirmed live:
   this incidentally also fixed `--power --per-core`'s package-energy reading, which was hitting the same
   bleed from the opposite direction (see 0.3 below) — needs a decision on whether to land as-is on
   `master` (contained, one-file diff, `make test` green) or move to a `feature/` branch per the normal
   workflow given its severity/urgency.
0.2. **[FIXED, second commit on the same branch] Topdown/topdown2 (`--topdown`/`--topdown2`, now
   `--counters=topdown[2]`) silently reported all-zero percentages whenever any other Intel counter
   opened first.** Intel's "Perf Metrics" fixed-counter feature (`slots` + the four/eight
   `core.topdown-*` sub-events) is a genuine kernel-enforced special case: those sub-events are only
   valid as members of a group whose *literal* leader is the `slots` event — the kernel returns `EINVAL`
   for any of them opened under a different leader. Because `setup_counters()` funneled every Intel
   raw/hw-cache/hardware group into one shared `intel_group_id` regardless of which group's counter
   happened to open first, and `ipc` (default-on, list order puts it ahead of `topdown`) opens its own
   `instructions` event first, `slots`'s sub-metrics ended up trying to join a group led by
   `instructions` — not `slots` — and failed. Confirmed live in exactly the *default*, most common
   invocation: `./wspy --csv --topdown -- true` measured only 3/7 counters (ipc's 2 + `slots` itself,
   which tolerates a foreign leader) and printed `retire=0.0, frontend=0.0, backend=0.0, speculate=0.0`
   — indistinguishable from a genuinely idle workload unless the stderr `errno=22` lines are read.
   `--no-ipc --topdown` (or any invocation where nothing else opened first) correctly measured 5/5 with
   real values, confirming the diagnosis. **Fix:** a second, dedicated group-leader variable
   (`intel_topdown_group_id`, mirroring `intel_group_id`'s own per-call reset from 0.1) scoped to exactly
   the groups whose `cgroup->mask` includes `COUNTER_TOPDOWN`/`COUNTER_TOPDOWN2` — every other Intel raw/
   hardware/hw-cache group still shares the general `intel_group_id` unchanged. Verified live: `--topdown`
   with default `ipc` now measures 7/7 with real values, reliably across 5 repeated runs; `--per-core
   --topdown` together (both this fix and 0.1's stacked) measures real, distinct topdown data on all 16
   P-cores. **Residual, separate issue found while verifying this fix:** `--topdown2` (and, less
   consistently, `--topdown-backend`) still intermittently print all-zero/`-nan` despite the manifest/
   coverage showing full measurement (e.g. `11/11`) — this is not the group-leader bug (leadership is
   now always correct and coverage is always complete) and reproduces in plain single-pass aggregate mode,
   not just under `--passes` as originally scoped in 0.5 below — see that item, revised to reflect the
   broader scope. One `--topdown2` run also printed an obviously-corrupt value
   (`spec_pipeline_pct=72407003082176.4`), suggesting a separate read/compute bug in that path worth its
   own investigation.
0.3. **The single-shared-Intel-group design cascades into wholesale counter loss once the combined group
   exceeds real hardware PMU capacity, and cannot mix counters across different underlying PMUs.** Two
   confirmed manifestations beyond topdown specifically:
   - A realistic multi-group combo (`--counters=dcache,icache,tlb,branch,cache2`, closer to what
     `deep-cpu-intel`-scale profiles request) measured only 10/19 counters — not partial degradation
     spread across groups, but *one whole group* (`cache`, 9 counters) failing 0/9 outright once the
     shared group's real hardware-slot budget was exhausted by earlier groups. A perf event *group*
     requires every member to be simultaneously schedulable (no within-group multiplexing by design);
     once that's impossible the kernel refuses further members with `EINVAL` rather than degrading to
     multiplexing. `preflight.c`'s own warning text ("an oversized group can get little or no scheduled
     time at all rather than degrading gracefully") already anticipated something like this but
     undersells it — in practice it's outright counter-open failure for entire groups, not reduced
     fidelity, and the 6-slot budget `preflight.c` estimates doesn't match what was actually observed
     (~10 succeeded here). `wspy --capabilities` (`COUNTER_ALL`) makes this maximally visible: 20/48
     available, with entire groups (`cache`, most of `topdown`/`topdown2`) reading 0.
   - `--power` (aggregate, no `--per-core`) tries to put RAPL's `energy-pkg` event (a *different* dynamic
     PMU, `type=35` on this host — not the general-purpose `cpu`/`cpu_core` PMU) into the same shared
     group as whatever hardware/raw event opened first. A perf group cannot span two different PMUs, so
     `energy-pkg` fails with `EINVAL` whenever anything else is set up in the same call — confirmed live,
     `./wspy --csv --power -- true` reports `pkg_joules=0.000,pkg_watts=0.000` with an explicit
     `unable to create power performance counter` error, even with `--no-ipc` (still fails — see 0.4,
     a second, independent bug also affecting this same flag). `--power --per-core` happens to dodge this
     one as a side effect of the 0.1 fix, since power's systemwide group is now set up in its own isolated
     `setup_counters()` call, separate from the per-core groups.
   Proper fix likely means moving Intel away from "one shared group across every requested counter" and
   toward something closer to AMD's model: either leave unrelated general-purpose events ungrouped
   (letting the kernel multiplex independently-scheduled events the normal way, restoring graceful
   degradation) with only the topdown Perf-Metrics family kept as its own small dedicated group (see 0.2),
   or route grouping decisions through `preflight.c`'s own already-more-accurate budget/bin-packing logic
   (already built for `--passes`, see `multipass.c`) instead of a single ungated shared leader. Worth
   scoping as its own design pass rather than a one-line fix, given how many failure shapes trace back to
   this one assumption.
0.4. **RAPL/`energy-pkg` opened with the wrong scope (`pid=0` instead of `pid=-1`) on Intel.** Even fully
   isolated (`--power --no-ipc`, nothing else competing for group leadership — rules out 0.3), `--power`
   alone still fails with `errno=22`. `setup_counters()`'s dispatch only special-cases system-wide/uncore
   PMU semantics (`perf_event_open(&pe,-1,0,group_id,0)`, i.e. "any process on this cpu") for
   `pe.type == PERF_TYPE_L3` specifically; every other counter — including RAPL's `power` PMU, whose Linux
   driver sets `task_ctx_nr = perf_invalid_context` and rejects task-scoped (`pid=0`) opens outright — falls
   through to the generic per-process branch (`pid=0,cpu=-1`). `CLAUDE.md`'s own power.c notes document
   that `--power` was validated "on real hardware" — but on the author's AMD dev host, the `power` PMU's
   dynamic type apparently *coincidentally* equalled `PERF_TYPE_L3`'s sentinel (`0xe`/14), routing it
   through the correct branch by accident (that coincidence is independently documented in `manifest.c`'s
   own entry, in a different context — `run_capabilities_probe()`'s duplicate-`perf_event_open()` bug).
   On this Intel host `power`'s real type is 35, doesn't collide, and takes the wrong branch. **This bug
   is not Intel-specific** — any AMD (or future Intel) host where the `power` PMU's dynamic type doesn't
   happen to equal 14 would hit the identical failure; it was only ever masked by chance. Fix: give
   `power_counter_group()`/`ibs_counter_group()` (and any future system-wide dynamic-PMU group) an explicit
   "needs pid=-1" marker rather than relying on an incidental type-value match against `PERF_TYPE_L3`.
0.5. **Intel topdown-family counters intermittently read back as zero/`-nan` despite full counter
   coverage — broader than `--passes`, revised after fixing 0.2.** Originally scoped (and reproduced) as
   a `--passes`-specific bug: `--passes=ipc,topdown -- sleep 1` measured both passes correctly (manifest
   `passes[]` showed 2/2 and 5/5) roughly 2 times out of 3, but occasionally printed `ipc=-nan` and every
   topdown percentage as `0.0` in the same run despite full coverage. After landing 0.2's group-leader
   fix, the *same symptom* reproduced in plain single-pass aggregate `--topdown2` (no `--passes` involved
   at all — `11/11` measured, all percentages `0.0`, roughly 1 run in 3) and once in `--topdown-backend`
   too, so this is not primarily a multi-pass merge/print bug and doesn't share 0.1's cross-call root
   cause either (each pass's `counter_group` list is a fresh, independently-`calloc()`'d allocation with
   no possible cross-pass field reuse). The common thread across every observed case is the Intel
   Perf Metrics/fixed-counter family (`slots` and its sub-events) specifically — plain hardware/raw groups
   (`ipc` alone, `branch`, `cache`) haven't shown this in any repeated test. Likely candidates for a
   follow-up investigation: a race between `PERF_EVENT_IOC_ENABLE` and the fixed-counter MSR actually
   starting to accumulate, or a `read()`-time issue specific to how the kernel computes `PERF_METRICS`
   sub-event values relative to their `slots` leader's own enable/disable state. One `--topdown2` run
   also printed a corrupt `spec_pipeline_pct` value (`72407003082176.4`), suggesting a related read/compute
   bug, possibly a divide against a near-zero or transiently-uninitialized denominator. Not root-caused —
   flagged as a real, reproduced-live, Intel-specific reliability gap in both the single-pass and
   `--passes` topdown paths; `strace`/`perf record` across a failing run is the obvious next step.

**Study: additional counters worth adding, grounded in this same real-hardware pass (13950HX / Raptor
Lake HX, `/sys/bus/event_source/devices/` enumerated live, not from documentation alone):**
- **Per-core-type-aware Intel raw event tables.** `cpu_core`'s dynamic PMU type is `4` on this host
  (which happens to equal the numeric value of `PERF_TYPE_RAW` itself — the likely reason
  `intel_raw_events[]`'s hardcoded `PERF_TYPE_RAW` has silently "worked" for P-cores despite never doing
  a real per-core PMU-type lookup the way `cpu_info.c` already does for ARM via
  `discover_arm_pmu_topology()`/`mark_cpus_for_pmu()`); `cpu_atom`'s type is `10` — a different value, not
  guaranteed stable across hosts/kernel versions. Every raw event in `intel_raw_events[]` (topdown,
  branch, l2cache encodings) is P-core-only-correct (Golden/Raptor Cove); Gracemont E-cores have
  different encodings entirely, which is *why* `core_is_per_core_eligible()` currently excludes
  `CORE_INTEL_ATOM` outright (see item 9 below, now unblocked). Real fix needs both a second,
  E-core-specific raw event table *and* per-core dynamic-PMU-type resolution (mirroring the ARM
  `pmu_type` field on `struct cpu_core_info`) rather than the hardcoded `PERF_TYPE_RAW` constant.
- **Real DRAM bandwidth (`COUNTER_MEMORY`, currently nonexistent for Intel — `amd_raw_events[]` has it,
  `intel_raw_events[]` doesn't).** `uncore_imc_free_running_0`/`_1` expose `data_read`/`data_write`/
  `data_total` events with their own `.scale`/`.unit` sysfs files — the *exact* shape `power.c` already
  knows how to parse (event lookup + scale/unit conversion, no config bit-math needed since these are
  "free running," always-on counters). This is a comparatively low-effort net-new capability riding on
  code that already exists, not a new discovery pattern.
- **True LLC/L3 counters.** Today's `l2_request.all`/`l2_request.miss` (`COUNTER_L2CACHE`, printed as
  `--cache2`/`-c`) is genuinely L2, not L3 — Intel has no L3-layer entry in `intel_raw_events[]` at all,
  unlike AMD's `COUNTER_L3CACHE`. `uncore_cbox_0`..`uncore_cbox_11` (12 CBox/LLC-slice PMUs on this host)
  would give real chip-wide LLC hit/miss/occupancy, complementing (not replacing) a per-core
  `LONGEST_LAT_CACHE.MISS`/`.REFERENCE`-style general-purpose-PMU event if one's wanted at the per-core
  granularity `l2_request.*` already reports at.
- **Intel per-core-domain and iGPU RAPL energy**, a genuinely different discovery shape than AMD's.
  `power.c`'s per-core energy support (`power.h`, "Per-core energy support," shipped 4.2) is built around
  AMD's model: package energy and per-core energy are *two separate PMU devices* (`power`/`power_core`).
  This host has no `power_core` PMU at all — instead the *same* `power` PMU exposes `energy-cores`
  (plural) and `energy-gpu` as additional named events alongside `energy-pkg`. Wiring up Intel per-core
  energy needs a second discovery path in `power_probe()`, not just a differently-named sysfs directory.
  `energy-gpu` is also notable on its own: real iGPU energy with no GPU vendor build flag needed at all.
- **`i915` GPU PMU** — an Intel-native alternative to this codebase's current AMD-sysfs/NVML-only GPU
  support (`amd_sysfs.c`/`nvidia_nvml.c`), exposing busy/frequency directly via `perf_event_open()`
  rather than a vendor SMI/sysfs scrape. Would need its own build axis (`INTEL_GPU=1`?) alongside
  `AMDGPU`/`NVIDIA` per the existing pattern, or could piggyback on the always-built default path since
  it's `perf_event_open()`-based rather than needing a vendor library.
- **C-state residency (`cstate_core`/`cstate_pkg` PMUs)** — idle-state time breakdown, useful context for
  interpreting `--power`'s energy numbers (low `pkg_watts` could mean genuinely idle vs. throttled) and
  for `system.c`'s existing power/frequency reporting; no code anywhere in this tree reads these today.
  AMD has no direct equivalent PMU (its C-state info comes from different sysfs paths), so this would be
  an Intel-only addition, same shape as `power_core`'s "not every counter exists on every vendor."
- **Intel PEBS-based precise memory-latency sampling** — the natural Intel-side counterpart to 4.3 Tier 1's
  already-planned AMD IBS sampling-mode item (item 1 above): `MEM_TRANS_RETIRED.LOAD_LATENCY`-style PEBS
  events give per-sample tagged memory-access latency/source data on Intel, comparable in spirit to IBS's
  `IbsOpData`/`DcMiss`/`NbIbsReqSrc` tag bits. Not investigated in depth this pass (no PEBS-specific sysfs
  probing was done here) — flagged as a parallel track worth scoping once IBS sampling-mode ships and its
  mmap-ring-buffer/per-sample decode infrastructure exists to model this against.

**Tier 1 — AMD IBS sampling-mode support (moved to the front of 4.3, 2026-07-20; see below):**

1. AMD IBS *sampling*-mode support: mmap'ing the perf ring buffer and requesting `PERF_SAMPLE_RAW`
   so each individual IBS sample's tagged register data is available, not just a count of how many
   fired — a genuinely new capability, not an extension of the counting-mode `ibs-basic`/
   `ibs-memory-deep` profiles. Nothing in wspy today reads a perf mmap ring buffer at all; every
   existing counter group (including IBS as currently implemented) is `read()`-based counting. Each
   `IbsOpData`/`IbsOpData2`/`IbsOpData3`/`IbsDcLinAd` (op samples) and `IbsFetchCtl`/`IbsFetchLinAd`
   (fetch samples) record carries tag bits this item would decode into per-tick rate estimates
   comparable to (but independently sourced from) the equivalent hardware-counter groups already
   reported: `DcMiss`/`L2Miss`/`NbIbsReqSrc` → dcache/L2/L3 miss and memory-source-of-fill rates;
   `DcL1TlbMiss`/`DcL2TlbMiss` → data-TLB miss rates; fetch-side `IcMiss`/`L1TlbMiss`/`L2TlbMiss` →
   icache/iTLB miss rates; `BrnMisp` (on branch ops) → branch misprediction rate. Valuable
   specifically because it's a second, independently-sampled measurement of the same phenomena the
   counting-mode groups already report — a real disagreement between the two is itself a signal (PMU
   multiplexing skew, a counting-mode blind spot, or an IBS sampling-rate artifact), not just a fifth
   way to get the same number. Format/sysfs-field discovery already exists (`ibs.c`'s `ibs_probe()`)
   and generalizes directly; the new work is the mmap/ring-buffer read path itself, per-sample record
   parsing, and the rate-aggregation/report layer built on top. Feeds this same phase's own
   "IBS-derived memory-path bottleneck decomposition" item (Tier 3 below). Moved here (was the last
   item in 4.2 Tier 1) rather than left at the tail of 4.2: it's a large, genuinely new capability on
   its own, not a small extension squeezed in after the rest of 4.2 wrapped up, and it has no
   dependency on anything else in 4.2's own remaining work (characterization prerequisites,
   launcher/infra, docs/testing/release) — only on already-shipped
   4.0/4.2 IBS capability-discovery work (`ibs.c`), so it's equally startable as 4.3's own first item.

**Tier 2 — needs 4.1's normalized store/history:**

2. Baselines and regression/anomaly detection.
3. Machine/environment comparability scoring — depends on provenance capture (shipped, `provenance.c`)
   existing across enough runs to score against. Broader than 4.2's (shipped) "PMU-capability-aware
   comparability warnings": that item is a narrow, immediate per-bucket exact-match check on
   `(cpu_vendor,counters_requested,counters_measured)`; this item is the deferred, scored version across
   the fuller provenance surface (BIOS, microcode, governor, memory, virtualization, etc.).
4. Distribution-first reporting (quantiles, clustering prep).
5. Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace
   only when data coverage differs).

**Tier 3 — topdown/attribution, needs 4.2's hierarchical schema + phase detection (both shipped) +
this phase's own IBS sampling mode (Tier 1 above):**

6. Phase-aware topdown (warmup/steady/degraded segmentation, drift signal).
7. Composite attribution (topdown + cache/TLB/IBS signals) — the "no blocking-syscall activity" vs.
   "heavy blocking-syscall activity" split from the critical-path work (shipped, see "Shipped since
   4.1") is a direct input here, alongside topdown/cache/TLB/IBS.
8. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) — needs this phase's
   own IBS sampling-mode support first (Tier 1 above); today's counting-mode IBS has no per-sample tag
   data to decompose.
9. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — **blocked on hardware
   access**: moved out of 4.2 (2026-07-19) since real Intel hybrid (Alder Lake/Raptor Lake) or ARM
   big.LITTLE hardware isn't reachable in the environment doing this work, and every line of
   core-type-specific raw-event/PMU-type code would ship unverified. Investigation before deferring
   turned up more missing infrastructure than the one-line backlog description implied, worth keeping
   so a future pass doesn't have to re-derive it: `wspy.c`'s `--per-core` counter setup (`main()`,
   ~line 1327) explicitly excludes `CORE_INTEL_ATOM` from the per-core loop today — on a real hybrid
   system, `--per-core` silently collects zero counters from Atom (E-core) cores, so there's no
   existing per-core Atom topdown data to weight-aggregate over yet. In the default (non-`--per-core`)
   aggregate mode, a process-wide perf counter sums across whatever cores the OS scheduled the thread
   on — on a hybrid CPU that's an uncontrolled Atom/Core mix with no visibility into how much of each,
   which is the concrete case the "don't mix Atom+Core topdown into one headline number" caveat
   (Topdown deep-dive item 6, above) is about. `--affinity=coretype=<id>` already exists (`affinity.c`)
   but is ARM-only (clusters by the ARM-specific `MIDR_EL1` register); x86 hybrid detection for that
   same grouping is a separate, already-documented gap in that file. A reporting-only v1 (detect
   `cpu_info->is_hybrid` + topdown counters requested, warn when the collection mode can't account for
   the mix, no new raw-event code) was scoped and ready to implement when this was deferred — revisit
   that scope first if picked back up before hardware becomes available, since it needs no hardware to
   validate. **Update (2026-07-20):** AMD Zen5/Zen5c per-core classification shipped (see "Shipped
   since 4.1" above, real hardware was available for that vendor) and `--per-core` already collects
   Zen5c counters (unlike the Atom exclusion above), so the "no existing per-core data to weight-
   aggregate over" blocker is gone specifically for AMD — but the weighted-aggregate composite-topdown
   analysis itself is still unimplemented for every vendor including AMD, and `--affinity=coretype=<id>`
   is still ARM-only (x86 hybrid detection for that grouping remains the separate gap `affinity.c`
   already documents). This item stays open; AMD is just no longer blocked on hardware access the way
   Intel/ARM hybrid still are. **Update (2026-07-22):** Intel hybrid hardware is now available (a
   Raptor Lake HX host, "carlsbad") — this item is no longer hardware-blocked either. First real-hardware
   pass on it turned up correctness bugs more fundamental than the E-core-exclusion gap this item was
   originally scoped around (see 4.3 Tier 0, above): `--per-core` was silently measuring only the first
   P-core regardless of E-cores (fixed), and topdown/topdown2 report all-zero on Intel by default even on
   the P-cores this item already knew were supported. Recommend sequencing Tier 0's fixes before resuming
   this item — a weighted P-core/E-core aggregate is meaningless to build on top of per-core topdown data
   that isn't being measured correctly yet even for the cores already included.

**Tier 4 — publishing/reporting expansion, needs 4.1's report studio:**

10. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1's per-run curation studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats (WordPress/HTML/Markdown,
   4.1) rather than inventing a fourth.
11. Characterization badges + similarity panels in reports — a new block type in 4.1's curation studio
    once 4.2's archetype scorecard exists to draw a badge from, not a separate report surface.
12. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1's
    static inclusion-depth mechanism (none/summary/excerpt/full) for the tree/interval blocks
    specifically; that mechanism stays the right default for a published, non-interactive report even
    once this exists.

**Tier 5 — report-layer additions on data already collected in 4.0:**

13. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
    process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
14. System (`--system`) → per-interface network attribution and local-vs-system-pressure
    attribution, plus steal-time capture (user/system/iowait are already captured and printed —
    `system.c`'s existing `/proc/stat` parsing — this item is the missing steal column and the
    analysis layer on top of what's already there, not the raw mix itself).
15. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
    `comm`-pattern role tagging).

**Tier 6 — GPU deeper profiling:**

16. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
    heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
17. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) —
    depends on 4.2's GPU fusion layer providing consistent per-metric data first.
18. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`,
    extended once GPU runs feed the same index.
19. Fold into general environment-comparability scoring (power cap, memory clock, thermal state,
    driver version) — no separate "GPU comparability score" needed; one scoring mechanism, not two.

**Tier 7 — infra:**

20. Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for
    `--tree`/`--tree-open` — `ptrace` context-switches on every syscall entry/exit, which skews the
    very counters being measured for I/O-heavy or fork-heavy workloads. Also the eventual fix for the
    observer-effect caveat noted under "Critical-path / synchronization-latency: what's left" above.
21. Collector-plugin implementation (perf stat / trace-cmd / GPU tools as collectors behind the
    `collector` field, normalization path) — the schema seam shipped in 4.0; this is the actual
    implementation of wrapping a non-wspy collector.
22. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
    CSVs into per-test-case/per-trial datasets by correlating run manifests with PTS results,
    composite.xml, and log timestamps. See
    [phoronix_hook_investigation.md](file:///home/mev/source/wspy/doc/phoronix_hook_investigation.md)
    for design and prototypes. **Capture instrumentation landed ahead of the full item:**
    `scripts/pts_hooks/*.sh`/`scripts/setup_phoronix_hooks.sh` register PTS `result_notifier` hooks and
    capture their output into a per-pass `pts_hooks.log` artifact across every launch path (`wspy-run`,
    the web launcher's custom path, `wspy-queue`); real-host testing found and fixed a registration bug
    on our side and surfaced/patched an upstream PTS crash bug (filed/fixed upstream:
    phoronix-test-suite/phoronix-test-suite#924/#925) — see `doc/INVESTIGATION_ARCHIVE.md`'s "Phoronix
    `result_notifier` hook capture: real-host findings" for the full story. **Still open:** teaching
    `wspy-phoronix-segment.py` to prefer `pts_hooks.log` over the composite.xml/log-timestamp
    correlation it uses today, and the segmentation tool itself.
23. Collapse `wspy-run`'s builtin profiles onto native `--passes` bin-packing. Low value relative to
    everything else on the 4.3 board, no dependents, safe to leave alone indefinitely. Most profiles
    are already collapsed as far as they can go: `deep-cpu`/`deep-gpu` folded their pure-counter middle
    pass onto `--passes=...` back in 4.1; their remaining separate passes all use `--interval 1`, which
    is hard-fatal'd against `--passes` (no defined multi-pass merge semantics for periodic ticks) — a
    real architectural constraint, not a missed collapse. `tree-heavy`/`gpu-compute` (`--tree`) and
    `ibs-basic`/`ibs-memory-deep` (IBS) are excluded from `--passes` the same way; `quick` is already
    one pass; `zen-portable`/`zen4plus-deep` just compose other profiles. The only real remaining
    candidate is `deep-cpu-intel`, which still hand-authors 4 separate `wspy` invocations that don't
    touch any `--passes`-incompatible flag — collapsing it to one pass is the entire remaining scope.
    Note: this changes on-disk output shape from 4 files to 1, so anything downstream assuming those 4
    filenames (external scripts, `tests/capability_matrix.sh`) would need checking.
24. Detect and resume interrupted `wspy-run` profiles (raised after a real host crash mid-batch, twice,
    with no way to tell from a report that the run never finished, or to resume without redoing
    completed passes). Two phases, second depends on first:
    - **Phase A — surface incompleteness.** `generate_manifest()` writes the run-level `manifest.json`
      only after every pass finishes, so a mid-loop crash leaves per-pass artifacts but no top-level
      manifest — an unambiguous, already-computable "never finished" signal (distinct from a run that
      finished all passes but whose workload itself failed, already covered by `wspy-validate`).
      Surface on `/report` (an "incomplete — N of M passes ran" banner) and `/history` (a new status
      value).
    - **Phase B — resume, skipping completed passes.** `wspy-run --resume <existing-run-dir>` reuses the
      existing `RUNROOT`/`RUN_ID`; for each pass, skip re-running only if its own manifest exists with a
      clean exit *and* its recorded configuration exactly matches what this invocation would run now
      (exact-match, via a new `--config-option pass_flags_hash=<hash>` provenance field) — never resumes
      a pass that was itself interrupted mid-execution; that pass is simply discarded and rerun.
    - Distinct from `wspy-queue`'s job lifecycle (whole-job scheduling/retry, not resuming partway
      through one multi-pass invocation's own internal passes) and from 4.4's much heavier config-first
      experiment system.
25. Phoronix per-test option-combination count, surfaced ahead of running — a real, recurring pain
    point is discovering *after* a long `batch-run` sweep that a test's full option matrix takes far
    longer than expected. Confirmed live against a real test profile (`blender-1.2.1`) that
    `<TestSettings>/<Option>/<Menu>/<Entry>` in `test-definition.xml` names the exact shape needed
    (blender's own profile is a 5×2=10-combination full sweep) — purely static parsing, no need to run
    anything, following the same "deliberately not a real XML parser" convention `ledger.c`'s
    `scan_phoronix_dependencies()` already established for this file. Natural home is `wspy-ledger`
    (alongside that existing scanning logic) as a new annotation/warning column next to a workload's
    done/skipped/unsupported/needs-tool-support status. Future input to the (shipped) `--tree` pass
    timeout item's `BATCH_RUN_MULTIPLIER` — a real combination count would replace that item's blind
    5.0 guess with a grounded number, once both exist.

**Tier 8 — testing:**

26. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
26. Contributor guide for adding a collector/metric/schema bump safely.

## 4.4 priorities
Goal: optional/heavier pieces that shouldn't block the rest, in priority order:
1. Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions,
   resumable/selective re-execution) — full version of the lightweight config-file execution
   already in `wspy-run` (4.0); don't build both at once.
2. Optional deep trace analysis (Perfetto-compatible export of tree+topdown+interval timelines) —
   advanced companion path for difficult workloads, needs 4.3's lower-overhead tracing backend to
   feed it.
3. Temporal drift detection (cluster movement across versions/configs/machines) — needs 4.3's
   clustering plus enough history to detect movement; treat as an investigation trigger, not a
   standalone feature.
4. Optional dashboard backend (e.g. Grafana) for exploratory slicing — explicitly optional/coexists
   with static-first publishing; doesn't block 4.0-4.3.
5. Optional live TUI (run progress, interval metrics, throttling/skew warnings) — a terminal-side
   surface, unrelated to and not superseded by 4.1's web interface work; nice-to-have, CLI-first model
   stays primary.
6. Process/thread migration diagnostics (did a process's threads actually move between cores during
   the run) — split out of 4.2's "Per-core imbalance/hot-core diagnostics" item, since it needs new
   instrumentation (periodic `/proc/<pid>/stat` `processor`-field sampling, or scheduler tracepoints)
   rather than just new analysis of data `--per-core` already collects. Natural pairing with 4.3's
   lower-overhead tracing backend if that lands first, but not a hard dependency.
7. Job-browsing view in the web UI — pushed out of 4.2 (2026-07-20). A queued job (`wspy-queue add`,
   or the Run tab's "Queue instead of running it now" checkbox) is visible today only via
   `wspy-queue list`/`show`, not from the web UI itself. Bundle in sharing structured configuration
   provenance with the job format (`web/joblib.py`'s job schema and `manifest.h`'s
   `configuration_provenance` are designed to be close in shape but aren't wired together yet).

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision. (Several
earlier open questions here — native multi-pass execution, ARM64 support, publication automation,
core/thread affinity, minimum metadata set for publishable — have been resolved by shipped work; see
"What shipped in 4.1" and "What shipped in 4.2" above rather than a stale "resolved" note here.)

- **Should `wspy-run`'s builtin profiles be refactored to be declaratively defined (as
  configurations+options) instead of today's hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in
  `load_builtin_profile()`?** Opened by the preset/configuration/option deep-dive above.
  Recommendation: not yet — let the web UI's preset/configuration/option model (shipped in 4.1)
  stabilize against real feedback first, then decide whether `wspy-run` itself should be rebuilt on the
  same vocabulary. Premature to commit to a CLI/`wspy-run` restructure before the vocabulary has been
  used for anything real; there's real leeway to make this change later if it produces a cleaner
  architecture, but no reason to rush it ahead of the UI work that motivated it.
- **Is cross-machine comparability a hard requirement for the first round?** Still open.
  Recommendation: no. Provenance fields are captured (4.0); defer comparability *scoring* to 4.3 —
  scoring needs enough historical runs across machines to be meaningful, which doesn't exist yet.
- **Should the website stay static-only, or add an interactive backend?** Still open. Recommendation:
  static-first through 4.3, keep an optional Grafana-style backend as a 4.4 nice-to-have. Non-goal:
  don't let the interactive-backend question block 4.3's static-site work.

## External brainstorming references
- ReBench — reproducible experiment configuration, resumable execution, explicit benchmark
  parameter tracking: https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) — static-site publication for benchmark trends with an interactive
  frontend model: https://asv.readthedocs.io/en/stable/
- Grafana OSS — optional dashboard-based slicing/templating if the interactive-backend path is
  taken: https://grafana.com/oss/grafana/
- Perfetto — timeline/trace analysis and SQL-based trace queries, relevant to the optional deep
  trace analysis pipeline (4.4): https://perfetto.dev/docs/

Note: OpenBenchmarking.org returned HTTP 403 in the environment used for this research; not
reviewed.
