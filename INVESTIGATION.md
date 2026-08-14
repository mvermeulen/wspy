# wspy Investigation

A rolling roadmap, kept deliberately focused on **forward-looking backlog** — what's actively planned
and the design reasoning behind it. Formerly `INVESTIGATION_4.0.md` — renamed once its content outgrew
a single release. Past releases are recorded only as terse pointer lists (below); their real interfaces
live in `README.md`/`CLAUDE.md`/`doc/METRICS.md`, and the design write-ups/validation narratives behind
every decision live in `doc/INVESTIGATION_ARCHIVE.md` — this document doesn't restate either.

Status (2026-08-09): **4.0, 4.1, 4.2, 4.3, and 4.3.1 are all tagged and released** (v4.2 and v4.3
published as GitHub releases with `wspy-release-notes.4.2.md`/`wspy-release-notes.4.3.md` as their
bodies, v4.3.1's release body written directly — see `scripts/release_prep.sh`). **4.4 is next.** The
open backlog (below) is sorted into three buckets: **"4.4 priorities"** (three named goals —
ease-of-use/one-click web UI flows, GPU support, and Phoronix suite build-out), **"4.5 priorities"**
(lower priority, still wanted, loosely grouped by topic), and **"Deferred indefinitely"** (explicitly
not planned for any numbered release; revisit only on a concrete trigger). A fresh "`<N>` release
closure" bucket gets added back once a cycle's own priorities empty out and it's time to prep that
tag — see `scripts/release_prep.sh`'s own checklist for what that housekeeping covers.

## Purpose
This document captures ideas for improvements focused on making benchmark collection, organization,
and publication easier and more repeatable.

## How to use this document
- **This document is the open backlog, not a changelog.** "What shipped in 4.0"–"4.3" below exist only
  so a name/PR/flag can be traced back to *which release* it landed in — each is a terse pointer list,
  not a feature description. `README.md`/`CLAUDE.md`/`doc/METRICS.md` document current interfaces and
  mechanism; `doc/INVESTIGATION_ARCHIVE.md` holds every design write-up and validation narrative. Don't
  restate either here — link to it.
- **When an item ships:** delete it from its priority bucket and add one line (name the file/tool/flag,
  not the mechanism) to that release's "What shipped in `<N>`" list — creating that section the first
  time a cycle's first item ships. If its design merited a multi-paragraph write-up while it was being
  built, move that write-up to `doc/INVESTIGATION_ARCHIVE.md` rather than leaving it inline — the open
  backlog should only ever contain open work. A rolling "Shipped since `<prior release>`" section is the
  intra-cycle staging area for this (present only while a cycle is actively shipping); fold it into the
  terse "What shipped in `<N>`" form at release-prep time, same as every past cycle.
- **Cross-references are by name, not number.** Item numbers inside a single tier list are fine as a
  local index, but don't reference an item elsewhere in this file (or from `CLAUDE.md`/commit
  messages) as "4.2 #27" — describe it by name instead ("AMD IBS sampling-mode support"). Numbers shift
  every time a tier is reorganized; names don't.
- The open backlog is sorted into three buckets — see the Status line above for what each holds. Add a
  new item to whichever bucket fits, or move an existing one, rather than inventing a parallel table.
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, Intel hybrid/
  counter-grouping, topdown, the preset/configuration/option vocabulary). Each points back at the
  priority-list items it informs, and keeps only its still-open thread — historical background that fed
  an item that's since shipped moves to `doc/INVESTIGATION_ARCHIVE.md`, same rule as everywhere else in
  this document.
- "Open questions" carry a recommendation each; re-open one by editing its entry, not by appending new
  prose elsewhere in the file.

## What shipped in 4.0
Terse pointer list, not a feature log — see `CLAUDE.md` for current interface/mechanism of every named
file/tool, `doc/ARTIFACT_CONTRACT.md` for the manifest/run-index/CSV schema contracts, and
`doc/INVESTIGATION_ARCHIVE.md` for design history and validation narratives.

- Run artifact foundation: manifest + run-index schemas, `wspy-run` (profile-driven launcher),
  `wspy-validate`, `wspy-ledger`, the unified per-run output layout.
- Reproducibility/provenance capture: `coverage.c` (counter capability discovery), `provenance.c`
  (virt role, microcode, BIOS, governor, memory, toolchain).
- Topdown confidence envelope + decomposition sanity checks.
- Zen5/IBS capability-driven probing (`ibs.c`), `ibs-basic`/`ibs-memory-deep` profiles.
- Expanded `getrusage`/`/proc` process telemetry.
- Counter-fit preflight (`wspy --preflight`); interval automatic phase-boundary detection (`phase.c`).
- Portability: arch-neutral ptrace register access (`ptrace_arch.h`, x86_64 + aarch64); collector-plugin
  schema seam (`collector` field, no non-wspy implementation yet).
- AMD GPU dynamic device scan; NVIDIA GPU monitoring (`--gpu-nvidia`). GPU *kernel*-level instrumentation
  (CUDA/Vulkan) was scoped out of this project — see `doc/INVESTIGATION_ARCHIVE.md`.
- Golden-output/capability-matrix testing (`tests/golden_output.sh`/`tests/capability_matrix.sh`).

## What shipped in 4.1
- Normalized SQLite store + reporting: `wspy-store`, `wspy-summary`, `wspy-plot`.
- Multi-pass counter execution: `--passes`/`--multiplex` (`multipass.c`).
- Web launcher & report browser (`web/server.py`): Run/Validate/Store & Summary/Discovery tabs,
  curation studio, multi-run compare view, publish-ready export, `/history`.
- Structured configuration provenance: `--preset-name`/`--config-name`/`--config-option`.
- Job queue: `wspy-queue`, `web/joblib.py`.

## What shipped in 4.2
- Critical-path/synchronization-latency instrumentation: `--tree-futex`/`-io`/`-io-wait`/`-connect`/
  `-nanosleep`/`-wait`/`-poll`/`-schedstat`/`-vmsize`.
- Core/thread affinity control: `--affinity=...` (`affinity.c`), `--list-affinity`.
- AMD IBS hardening + Zen-family preset packs (`zen-portable`/`zen4plus-deep`).
- CPU energy/power: `--power` (`power.c`), package + per-core.
- Hierarchical L1→L2→L3 topdown schema.
- GPU support: ROCm SMI + sysfs fusion layer (`--gpu-metrics`), `gpu-compute` profile, NVIDIA parity.
- PMU-capability-aware comparability warnings (`mixed-pmu`).
- System-wide metrics: `SYSTEM_DISK`/`SYSTEM_MEM`, cgroup v2 identity/limits/throttling (`cgroup.c`).
- Per-core diagnostics: `wspy-core-report`, AMD Zen5/Zen5c core detection, `--per-core-freq`.
- `proctree --json`/`--diff` + interactive web tree viewer.
- ARM64: real topology/topdown/ptrace support, validated on real hardware.
- Local-LLM narrative analysis: `wspy-analyze` (Ollama).
- Feature normalization + archetype scorecard: `run_features`, `wspy-archetype`.

## What shipped in 4.3
- Intel hybrid counter-grouping correctness (Tier 0): six hardware-verified bugs fixed on real Intel
  hybrid hardware, plus full Gracemont E-core raw-event support.
- AMD IBS sampling mode: `--ibs-sample` (`ibs_sample.c`).
- `wspy-summary`/`wspy-archetype` analytics: `--check-regression`, `env_score`/`mixed-env`, per-run IPC
  quantile features, `--phase-topdown`.
- Clustering/nearest-neighbor (Tier 1, now fully shipped): `wspy-archetype --nearest`/`--kmeans`.
- Composite attribution (Tier 2, now fully shipped): `memory_attribution`/`memory_attribution_locus`
  archetype axes, core-class-aware topdown, `wspy-core-report --weight-by`.
- Phoronix suite tooling (Tier 7): `wspy-ledger --phoronix-option-combos`, `wspy-phoronix-import`, web
  Phoronix tab.
- CPU2026 web tab: discovery/registration/build-triggering for SPEC CPU2026 benchmarks.
- Process-tree/profiling depth: `--target=comm=<name>[,cmdline=<substr>]` PID-targeted counter
  attachment, `--symbol-sample`/`wspy-symbolize`.
- `wspy-analyze`: Markdown output (`markdown_lite.py`), IBS-CSV/`on_cpu` prompt fixes.
- Report-page curation: characterization badges, similarity panels, interactive `--interval` timeline
  viewer.
- `wspy-testpoint` pipeline: `select-runs`/`aggregate`/`render`, `wspy-archetype --run-guest`.
- WordPress publishing: `web/wp_client.py`, `wspy-publish`, hierarchy-aware page publishing.
- Benchmark reference matrix: Reference web tab, WordPress-recovered metrics for machines with no local
  database presence, site-wide static-publishing pipeline (`scripts/publish_reference_matrix.py`).
- Release engineering: `scripts/release_prep.sh` outstanding-open-issue review, `doc/CONTRIBUTOR_GUIDE.md`.

Release-prep housekeeping is folded into the list above. `scripts/release_prep.sh --version 4.3`'s full
checklist ran clean; `wspy-release-notes.4.3.md` is the published GitHub release body. v4.3 is tagged
and released.

## What shipped in 4.3.1
- Correctness fix: `web/server.py` never imported the `report_root` module, crashing every default
  `python3 web/server.py` startup (no `--report-root` flag) on first page load — `NameError: name
  'report_root' is not defined` (#226). Regression test: `ResolveReportRootForWebTest` in
  `web/test_reference_matrix.py`.

`scripts/release_prep.sh --version 4.3.1`'s full checklist ran clean. v4.3.1 is tagged and released;
its GitHub release body is the notes at `https://github.com/mvermeulen/wspy/releases/tag/v4.3.1`.

## Shipped since 4.3.1
Intra-cycle staging area for 4.4 — fold into "What shipped in 4.4" at release-prep time.

- `float_pct` `run_features` promotion (`store.c`'s `SIMPLE_METRIC_FEATURES`, `FEATURE_SET_VERSION`
  1.5→1.6): filed as #227 from a `compiler-flag-miner` design discussion (its GCC vector-width flag
  catalog couldn't distinguish an integer memory-bound workload from a genuinely FP-heavy one using
  `resource_dominance`/`memory_attribution` alone). The `float` metric (AMD-only FP-op density,
  `topdown.c:print_float()`) already reached `metric_values` as `[raw]`; this is just the promotion to
  a normalized per-run feature, justified by a reference-matrix data review (SPEC CPU2026 by-machine
  pages, cross-checked against `workload/cpu2026/spec_benchmarks.json`'s intrate/fprate ground truth)
  showing a clean bimodal split with no overlap between the two categories. See "Known gaps" below for
  what that review wasn't enough to justify yet.
- Vision-based topdown-chart analysis: `wspy-analyze --image` narrates a run's `plots/*.topdown.png`
  via a vision-capable Ollama model (default `gemma4:26b`), grounded in a real numeric summary of the
  same CSV data rather than the model's own pixel-reading. CLI and web UI both, wired into the curation
  studio (new `aivision.*`/`AIVISION_RE` naming) the same way the existing text narrative already is.
  See `doc/INVESTIGATION_ARCHIVE.md`'s "Vision-based topdown-chart analysis: live model comparison and
  design" for the live model comparison behind the design and how its open questions resolved.
- `wspy-run` builtin-profile vocabulary refactor (4.4(a) item 1, first slice): the 11 builtin profiles
  (`quick` through `zen4plus-deep`) moved from a hardcoded bash `case` statement
  (`PASS_NAMES`/`PASS_FLAGS` arrays) to data files under `profiles/*.conf` (plain pass lists, reusing
  `wspy-run`'s existing `-c <file>` grammar) and `profiles/*.spec` (composites) — `BUILTIN_PROFILES` is
  now discovered from the file set rather than hand-listed a second time. `web/joblib.py`'s
  `COMPOSITE_PRESET_PROFILES` now reads the same `*.spec` files instead of hand-mirroring `wspy-run`'s
  composition (a real, audit-found drift risk — see 4.4(a)'s own intro). New `wspy --list-groups`
  (`multipass.c`) makes the `--counters=`/`--passes=` group-name vocabulary machine-readable;
  `tests/group_vocab_check.sh` checks `web/joblib.py`'s `ALL_GROUPS` against it so that duplication
  can't silently drift either. `--list`/`--dry-run` output is unchanged (verified byte-identical per
  profile) except `--list`'s own enumeration order, now alphabetical (file-glob-derived) rather than
  hand-authored. Remaining scope (the web UI's own checklist/argv engine) stays open under item 1
  above — this was deliberately scoped as the mechanically-safe slice, not the full three-way
  unification.
- `wspy-run` builtin-profile vocabulary refactor (4.4(a) item 1, ARM group exposure slice): the 3
  ARM-only counter groups (`arm-dcache-mem`/`arm-icache-tlb`/`arm-mem-align-tlb`) are now real
  `web/joblib.py` `ALL_GROUPS` entries, so the web checklist's "Performance counters"/preflight cards can
  select them like any other group — `ALL_GROUPS` is now a full mirror of `multipass.c`'s
  `multipass_group_names[]` rather than missing this one carve-out (`tests/group_vocab_check.sh` stays a
  subset check on principle, not because this gap is still open). `COLUMN_TO_GROUP` also gained the 11
  raw-event CSV columns these 3 groups produce, so a custom plot asking for one of them now correctly
  auto-enables its group (`autofit_checklist_for_custom_plots()`/`build_supplementary_plot_passes()`)
  instead of silently warning. Remaining scope (the web UI's own checklist/argv engine unification onto
  `profiles/*.conf`/`*.spec`) stays open under item 1.
- Default curation: AI vision analysis output (`aivision.*.md`) is now included by the Studio's "Apply
  default curation" button when present, mirroring the existing narrative-analysis treatment —
  previously only `ai_artifact_label()`'s "AI narrative analysis" labels were swept into the fallback
  pass, so `wspy-analyze --image` output was silently skipped even when it existed in the run directory.
  `default_curation.conf` gained an "AI vision analysis" section (rendered-prompt/critique variants
  documented-excluded, same as narrative's); `build_default_curation_blocks()`'s fallback sweep now also
  matches the `"AI vision analysis ("` label prefix (the real per-image model output specifically, not
  its rendered-prompt/critique siblings, which share the same leading phrase).
- Vision analysis: per-plot-type prompt templates + multi-select web UI. `wspy-analyze --image` used to
  always render `prompts/vision_topdown.tmpl` regardless of which plot it was pointed at — fine for
  topdown (the only registered type), wrong wording for any other chart. Now
  `VISION_TEMPLATE_BY_PLOT_TEMPLATE` resolves a dedicated template from the image's own wspy-plot
  template name (`joblib.VISION_PLOT_KINDS`, shared with the web UI so the two vocabularies can't drift);
  two new templates registered — `prompts/vision_system_cpu.tmpl` (CPU busy/idle duty-cycle phases) and
  `prompts/vision_power_vs_frequency.tmpl` (power/frequency coupling, plateaus, wobble during
  transitions) — alongside the original `vision_topdown.tmpl`. An unregistered plot template is a hard
  `ap.error()` (never a silent fallback to mismatched wording); `load_template()`'s version-marker regex
  generalized from one hardcoded `VISION_TOPDOWN_TEMPLATE_VERSION` alternative to any
  `VISION_<NAME>_TEMPLATE_VERSION`, so a future template needs no matching code edit. The report page's
  "AI vision analysis" card replaced its single plot `<select>` with one checkbox per registered plot
  actually present in the run (topdown checked by default for continuity, the two new ones opt-in), still
  one launch button and one shared model selection — `execute_multi_vision_analyze()` runs each checked
  plot as its own `wspy-analyze --image` invocation, sequentially, in one combined SSE log, "done" only
  if every one succeeded. `default_curation.conf` places each plot's own `aivision.*` block immediately
  after that plot (previously the vision-analysis section was grouped near the top, ahead of every plot
  it might narrate) — general policy now, not just for topdown.
- `wspy --list-columns` + mechanized `PROFILE_PLOTTABLE_COLUMNS` (4.4(a) item 1, "extend `wspy.c`'s CLI
  flags"/"revisit `PROFILE_PLOTTABLE_COLUMNS`" slice): new standalone probe, `wspy --list-columns
  [<flags>]` (`wspy.c`, same "no separate workload launch" standing as `--capabilities`/`--preflight`/
  `--list-groups`) — prints the CSV header the given flag combination would produce and exits, by falling
  through the exact single-pass/`--passes` group-construction and header-printing code a real run uses
  (an early return right after each path's own CSV-header block, not a second hand-maintained copy of
  it) rather than dispatching to its own probe function the way the other three do. `web/joblib.py`'s
  hand-maintained `PROFILE_PLOTTABLE_COLUMNS` table (used by `build_supplementary_plot_passes()` to know
  which columns a preset's own passes already cover) is replaced by `profile_plottable_columns()`, a
  lazy, memoized query against this new flag: reads a builtin profile's `profiles/*.conf` (expanding
  `*.spec` composites via the already-shipped `expand_preset_names()`), finds its `--interval` pass(es),
  and asks the real `wspy --list-columns` what CSV header they produce — real per-host device names
  (`net enp2s0`, `disk nvme0n1 ...`) come back as literal columns, so the old `"net *"`/`"disk *"`
  wildcard special-case in `build_supplementary_plot_passes()` is gone too. Deliberately *not* a
  module-level constant computed at import time, unlike `ALL_GROUPS` — `joblib.py`'s own docstring
  requires importing it to never need a built `wspy` binary on PATH, so the query is cached per
  `(wspy_bin, profile token)` the first time it's actually needed instead, with an empty-set degrade on
  any subprocess failure (missing binary, non-zero exit, timeout) matching the old table's own
  "absent entry" default. Found and fixed a real, live drift bug in the process: the old hand-authored
  table claimed `ibs-basic`/`zen-portable` had zero plottable columns, when `ibs-basic.conf`'s pass has
  run `--interval 1 --csv` (producing `ibs_fetch`/`ibs_op`) all along — exactly the class of silent
  drift this item called out as the reason to mechanize it. Remaining scope under item 1: the web UI's
  own checklist/argv engine unification onto `profiles/*.conf`/`*.spec`.

## Known gaps (still open)
Real-hardware/real-scale validation this project's hand-testing hasn't covered yet. Not release
blockers — just don't assume these are confirmed:
- **AMD IBS `fetchlat` threshold minimum unverified:** unlike `ibs_op`'s `ldlat` field (see above,
  now fixed), no host available during that validation exposed a working `fetchlat` sysfs format
  field on `ibs_fetch` to test whether it has an analogous hardware-enforced minimum below
  `IBS_DEFAULT_FETCHLAT_THRESHOLD`'s current value of 120. Worth the same bit-sweep treatment once
  hardware with a live `fetchlat` field is available.
- **Sub-~10ms target processes can read back all-zero/`-nan` counters** — a real perf-subsystem timing
  limitation (the kernel never finishes scheduling the counter onto real hardware before a very
  short-lived child exits), not a wspy bug. Essentially absent on realistic (>0.3s) workloads; no fix
  planned. Root-cause investigation archived in `doc/INVESTIGATION_ARCHIVE.md`.
- **`vectorization_density` archetype axis thresholds not yet set (#227):** `float_pct` (above) is now a
  `run_features` value, and `archetype.c`'s header comment already sketches the one-`SIMPLE_AXES`-entry
  extension path, but the reference-matrix data behind its promotion is single-machine, `n=1` per test,
  one compiler config (`gcc_o3-base`) — a clean intrate/fprate separation (≈0.6–5.6% vs. ≈14.5–34.0%,
  zero overlap, corroborated on a second machine for the tests it has so far), but too thin to commit to
  `low`/`moderate`/`high` boundaries yet, per this doc's own no-thresholds-without-real-data rule. Revisit
  once more machines/compiler configs/repeat runs are in the reference-matrix corpus.

## Track deep-dives
Reasoning that doesn't fit a single backlog line, for tracks with genuinely open work. Deep-dives for
work that's since fully shipped (blocking I/O, schedstat, vmsize, connect/wait/poll/nanosleep, power,
the LLM narrative-analysis design, the vision-based topdown-chart analysis design, the full
critical-path-instrumentation candidate rationale, repeatability policy/confidence metadata, comparison
matrix mode, and symbol-level profiling) have moved to `doc/INVESTIGATION_ARCHIVE.md`. The Zen5/IBS and
Topdown deep-dives below have also each been trimmed to just their one remaining open thread — the
confirmed-platform-behavior background that fed their now-shipped items moved to the archive too.

### Zen5/IBS deep-dive
The confirmed-platform-behavior background that fed 4.2's "Zen-family preset packs"/"PMU-capability-
aware comparability warnings" and 4.3's AMD IBS sampling-mode support (all shipped) has moved to
`doc/INVESTIGATION_ARCHIVE.md`'s "Zen5/IBS platform-behavior findings" now that everything it informed
has landed. One thread remains open:

- Zen5's topdown dispatch baseline shifted from Zen4's 6 slots/cycle to 8 — already implemented
  (`topdown.c`'s `CORE_AMD_ZEN`/`CORE_AMD_ZEN5` slot-multiplier branch). But the finer per-scheduler
  breakdown events AMD introduced alongside that width change aren't in `amd_raw_events[]` yet: split
  ALU/AGU scheduler-stall counters, and op-cache/execution-queue events that would separate `Frontend
  Latency` from `Frontend Bandwidth`. `IBS_LD_L1_DTLB_REFILL_LAT` also isn't named anywhere in the IBS
  capability-probing rows. Tracked as 4.5's "Zen5 fine-grained scheduler-stall counters" item — both are
  also candidate inputs for 4.5's "Platform formula registry" item once Zen5-specific formulas are
  actually versioned there.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Now unblocks 4.3's "IBS-derived memory-path bottleneck decomposition" (shipped, see "What shipped in
4.3"). `--interval`-integrated periodic IBS sampling rates (needs a real poll-loop architectural change)
was deliberately deferred out of that item's scope — see "Deferred indefinitely" above.

### Intel hybrid / counter-grouping deep-dive
Real Intel hybrid hardware became available for the first time this cycle (a Raptor Lake HX host,
codenamed "carlsbad", 2026-07-22) and immediately surfaced a cluster of confirmed, hardware-verified
counter-grouping bugs in `topdown.c` — these predate this investigation entirely (the shared-group
design dates to commit `273e9af`, Dec 2023) and were never caught before because no Intel hardware
existed in this environment to exercise them. All five original findings plus the Gracemont E-core
raw-event gap are now resolved (four fully shipped, one split — the corrupt-percentage half shipped, the
all-zero/`-nan` half is a documented non-actionable perf-subsystem limitation, see "Known gaps" below) —
full root-cause/fix/verification detail moved to `doc/INVESTIGATION_ARCHIVE.md`'s "Intel hybrid /
counter-grouping real-hardware findings and fixes" now that nothing here remains open backlog.

Additional Intel counters worth adding, grounded in the same real-hardware pass (`/sys/bus/
event_source/devices/` enumerated live, not from documentation alone). The GPU item (`i915` GPU PMU) is
tracked in 4.4(b); everything else here is tracked as 4.5's "Intel counter expansion" item:
- **Real DRAM bandwidth** (`COUNTER_MEMORY`, nonexistent for Intel today). `uncore_imc_free_running_0`/
  `_1` expose `data_read`/`data_write`/`data_total` with their own `.scale`/`.unit` sysfs files — the
  exact shape `power.c` already knows how to parse; comparatively low-effort riding on existing code.
- **True LLC/L3 counters.** Today's `l2_request.all`/`.miss` (`COUNTER_L2CACHE`) is genuinely L2, not
  L3 — Intel has no L3-layer entry at all, unlike AMD's `COUNTER_L3CACHE`. `uncore_cbox_0`..`_11` (12
  CBox/LLC-slice PMUs on this host) would give real chip-wide LLC hit/miss/occupancy.
- **Generic `PERF_TYPE_HW_CACHE` coverage is incomplete on this microarchitecture.** Confirmed live on a
  real Coremark run (Raptor Lake HX, 2026-07-22): `l1i-read` (L1I read-access), `iTLB-loads` (ITLB
  read-access), and `dTLB-load-misses` all failed `perf_event_open()` with `EINVAL` while their sibling
  events in the same request (`l1d-read`, `l1i-read-miss`, `dTLB-loads`, `iTLB-load-misses`) succeeded —
  consistent with the well-known Linux-perf reality that several generic `PERF_COUNT_HW_CACHE_*`
  (cache,op,result) combinations simply aren't wired up in the kernel's per-microarchitecture Intel PMU
  mapping table, not a wspy grouping/request bug (ruled out separately: this reproduced with the 4.3 Tier
  0 counter-group budget fix already in place, each event opening as its own correctly-sized group).
  Reinforces the **True LLC/L3 counters**/**Real DRAM bandwidth** items above rather than adding new
  scope: the fix for any specific missing combo is the same one — a real Intel raw/uncore MSR event
  replacing the generic `PERF_TYPE_HW_CACHE` abstraction for that slot, not a wspy-side workaround (there
  isn't one; the kernel refuses the open before wspy sees anything to work around).
- **Intel per-core-domain and iGPU RAPL energy** — a genuinely different discovery shape than AMD's.
  This host has no `power_core` PMU; instead the *same* `power` PMU exposes `energy-cores`/`energy-gpu`
  as additional named events alongside `energy-pkg`. `energy-gpu` is notable on its own: real iGPU
  energy with no GPU vendor build flag needed.
- **`i915` GPU PMU** — an Intel-native busy/frequency alternative to the current AMD-sysfs/NVML-only GPU
  support, `perf_event_open()`-based rather than a vendor SMI/sysfs scrape.
- **C-state residency** (`cstate_core`/`cstate_pkg` PMUs) — idle-state breakdown, useful context for
  `--power`'s energy numbers; AMD has no direct equivalent PMU.
- **PEBS-based precise memory-latency sampling** (`MEM_TRANS_RETIRED.LOAD_LATENCY`-style events) — the
  natural Intel counterpart to AMD IBS sampling-mode support (shipped, see "What shipped in 4.3"),
  comparable in spirit to IBS's `IbsOpData`/`DcMiss`/`NbIbsReqSrc` tag bits. Not investigated in depth
  this pass; worth scoping now that IBS sampling-mode's mmap-ring-buffer/per-sample decode
  infrastructure exists to model this against.

### Topdown deep-dive
Everything this originally tracked has shipped — multiplex-aware confidence/decomposition sanity checks
(4.0), the hierarchical L1→L2→L3 schema (4.2), and phase-aware topdown/cross-signal attribution/hybrid
core-class summaries (4.3) — except one item:

- Platform formula registry — versioned event/formula mapping per CPU family/model, for auditability.
  Tracked as 4.5's "Platform formula registry" item; see the Zen5/IBS deep-dive above for concrete
  candidate inputs once this exists.

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

Cross-cutting goal, now active 4.4(a) scope (see "4.4 priorities" above — promoted 2026-08-07 from "not
yet committed to"): the same preset/configuration/option vocabulary should describe `wspy`'s own CLI
options (today an unstructured flat flag list) and `wspy-run`'s profile format (today hardcoded
`PASS_NAMES`/`PASS_FLAGS` bash arrays in `load_builtin_profile()`), not just the web UI — this is the
vocabulary to design against as that refactor proceeds, so it doesn't independently invent a different
model for the same thing. There is real leeway to adjust existing options/commands toward this if it
produces a cleaner architecture.

### Critical-path / synchronization-latency: what's left
All six originally-scoped syscall-latency candidates (futex, blocking I/O, connect, nanosleep, wait,
poll) have shipped — see "What shipped in 4.2" above and `doc/INVESTIGATION_ARCHIVE.md` for the full
motivation and per-syscall design rationale. What remains open from this track:
- The *general*, table-driven mechanism (`tree_open`'s "syscall name → number → decode →
  log-vs-aggregate" generalization) was deliberately not built — six syscall families were still cheap
  enough as individual `if` branches in `ptrace_loop()`'s dispatch. Tracked in "Deferred indefinitely"
  above; revisit only if a seventh syscall family comes up.
- `ptrace` itself imposes a real stop-the-world cost on every syscall of the traced process, so
  absolute latency numbers collected this way are inflated relative to an untraced run. The *relative*
  split (fraction of wall time in futex-wait vs. read-wait vs. on-CPU) stays informative even when
  absolute numbers are skewed, but this is an inherent limitation of the mechanism — the low-overhead
  tracing alternative to `ptrace` that would fix it is also in "Deferred indefinitely" above, not
  actively planned.

## 4.4 priorities
Goal: refocus away from adding more analysis surface and toward (a) making the large amount of
functionality already shipped easier to actually use, (b) GPU support parity with the CPU side, and
(c) building out Phoronix suite coverage/workflow. Items are grouped by focus, not by dependency tier —
pick items within a group in any order that fits.

**4.4(a) — Ease of use / one-click web UI flows.** Grounded in a 2026-08-07 audit of the actual surface
area rather than guesswork: 16 separate CLI entry points (`wspy`, `wspy-run`, `wspy-validate`,
`wspy-ledger`, `wspy-store`, `wspy-summary`, `wspy-plot`, `wspy-core-report`, `wspy-archetype`,
`wspy-testpoint`, `wspy-publish`, `wspy-queue`, `wspy-sweep`, `wspy-analyze`, `wspy-bundle`,
`wspy-symbolize`) with no unified pipeline or suggested order; the report/studio/reference web pages
have accumulated roughly ten independent manual-trigger actions (generate process-tree views, AI
narrative analysis, characterization badge, similarity panel, apply default curation, add freeform
curation block, export, publish to WordPress, publish test-point report, publish reference matrix,
discover from WordPress), each its own card added the moment its capability shipped, with nothing on
the page indicating order or which are "normally do this" vs. optional; and per-suite flags
(`--phoronix-dest-root`/`--cpu2026-dest-root`) and run-identity conventions (`--run-id` alone vs.
`--hostname`+`--command` vs. `--hostname`+`--run-id`, depending which tool) have drifted apart tool by
tool despite resolving near-identical shapes underneath.

1. Preset/Configuration/Option vocabulary refactor — remaining scope: unify the web UI's own
   checklist-driven argv builder (`build_configuration_passes()`, `web/joblib.py`) onto the same
   `profiles/*.conf`/`*.spec` data `wspy-run` reads, rather than its own independent configuration/
   option model. (The builtin-profile-vocabulary slice, the ARM group-exposure slice, `wspy.c`'s
   `--list-columns` probe, and mechanizing `PROFILE_PLOTTABLE_COLUMNS` off it have all shipped — see
   "Shipped since 4.3.1" above.) See the "Preset / Configuration / Option hierarchy deep-dive" below
   for the full reasoning.
2. One-click end-to-end pipeline. Today a human runs `wspy-run`, then separately has to already know to
   run `wspy-store`'s ingest, `wspy-testpoint select-runs`, `wspy-testpoint render`, and a publish step —
   each its own command or its own web-UI button, in an order nowhere written down for a CLI-only user.
   Chain the common path (a finished run → ingested → selected → rendered → published) into one action,
   with a dry-run/preview step before anything writes or pushes, mirroring the caution
   `scripts/publish_reference_matrix.py`'s web button already applies ("Preview (dry-run)" checked by
   default). Web UI first — it already has every piece as a background-thread/SSE card; a CLI wrapper is
   a natural follow-on once the sequencing is settled, not a prerequisite.
3. Report-page guided flow / progress indicator. Add a lightweight checklist/progress view (Run done /
   Curate — / Characterize — / Publish —) framing the report/studio page's ~10 existing cards as one
   flow rather than an unordered stack discovered by scrolling. Doesn't require merging the actions
   mechanically (the item above covers that) — presentation/sequencing only.
4. CLI flag/identity consistency pass. Two concrete inconsistencies found in the 2026-08-07 audit: (1)
   `--phoronix-dest-root`/`--cpu2026-dest-root` are separate flags even though both suites resolve
   through the identical `find_materialized_*_test_point()` shape — every future suite added this way
   means another bespoke flag pair rather than one generic mechanism; (2) a run is identified three
   different ways depending which tool you're using, for a real reason (`wspy-summary`'s own rationale:
   two runs can share identical command+hostname when one is a redo of the other) but with that rule
   undocumented as a single cross-tool convention anywhere a user would find it before hitting the
   surprise. Audit and, where safe, unify; where a difference is load-bearing, document it once in one
   place rather than re-deriving it per tool.
5. Quickstart guide / guided onboarding path. `README.md` documents all 16 CLI tools each in their own
   section with no suggested order — a new checkout or new machine has to infer the
   run→store→summarize→publish sequence from first principles. Add a single "benchmark X, get a
   published report" walkthrough near the top of `README.md`, and consider surfacing the same sequence
   as a first-run hint in the web UI.
6. Detect and resume interrupted `wspy-run` profiles (raised after a real host crash mid-batch, twice,
   with no way to tell from a report that the run never finished, or to resume without redoing completed
   passes). Two phases, second depends on first:
   - **Phase A — surface incompleteness.** `generate_manifest()` writes the run-level `manifest.json`
     only after every pass finishes, so a mid-loop crash leaves per-pass artifacts but no top-level
     manifest — an unambiguous, already-computable "never finished" signal (distinct from a run that
     finished all passes but whose workload itself failed, already covered by `wspy-validate`). Surface
     on `/report` (an "incomplete — N of M passes ran" banner) and `/history` (a new status value).
   - **Phase B — resume, skipping completed passes.** `wspy-run --resume <existing-run-dir>` reuses the
     existing `RUNROOT`/`RUN_ID`; for each pass, skip re-running only if its own manifest exists with a
     clean exit *and* its recorded configuration exactly matches what this invocation would run now
     (exact-match, via a new `--config-option pass_flags_hash=<hash>` provenance field) — never resumes a
     pass that was itself interrupted mid-execution.
   - Distinct from `wspy-queue`'s job lifecycle and from 4.5's much heavier config-first experiment
     system.
7. Job-browsing view in the web UI. A queued job (`wspy-queue add`, or the Run tab's "Queue instead of
   running it now" checkbox) is visible today only via `wspy-queue list`/`show`, not from the web UI
   itself. Bundle in sharing structured configuration provenance with the job format (`web/joblib.py`'s
   job schema and `manifest.h`'s `configuration_provenance` are designed to be close in shape but aren't
   wired together yet).

**4.4(b) — GPU support:**

8. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
   heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
9. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) — builds on
   4.2's GPU fusion layer (`gpu_fusion.c`, `--gpu-metrics`) for consistent per-metric data.
10. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`, extended
    once GPU runs feed the same index.
11. Intel `i915` GPU PMU — an Intel-native busy/frequency alternative to the current AMD-sysfs/NVML-only
    GPU support, `perf_event_open()`-based rather than a vendor SMI/sysfs scrape. See the Intel hybrid /
    counter-grouping deep-dive for detail (the rest of that deep-dive's counter wishlist is non-GPU,
    tracked in 4.5).

**4.4(c) — Phoronix suite build-out:**

12. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
    CSVs into per-test-case/per-trial datasets by correlating run manifests with PTS results,
    composite.xml, and log timestamps. See
    [phoronix_hook_investigation.md](file:///home/mev/source/wspy/doc/phoronix_hook_investigation.md)
    for design and prototypes. **Capture instrumentation already landed:**
    `scripts/pts_hooks/*.sh`/`scripts/setup_phoronix_hooks.sh` register PTS `result_notifier` hooks and
    capture their output into a per-pass `pts_hooks.log` artifact across every launch path — see
    `doc/INVESTIGATION_ARCHIVE.md`'s "Phoronix `result_notifier` hook capture" write-up. **Still open:**
    teaching `wspy-phoronix-segment.py` to prefer `pts_hooks.log` over composite.xml/log-timestamp
    correlation, and the segmentation tool itself.
13. `wspy-run`-profile-driven batchable equivalent of the single-test-point Phoronix suite flow
    (`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab — see "What shipped in 4.3" for
    what's already landed) — a saved profile or `-c` file, run non-interactively/scriptable/batchable
    across many materialized test points at once. Only the direct wspy/checklist Run tab path (one test
    point, launched by a human clicking Run) exists today.

## 4.5 priorities
Goal: lower priority than 4.4 but still real, wanted work — pick up once 4.4's three focus areas are
substantially done. Not ordered into dependency tiers; a few internal dependencies are called out inline.
Cross-referenced by name, not number, per this doc's own convention — item numbers here will shift the
next time this section is reorganized.

**Report-layer additions on data already collected:**

1. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
   process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
2. System (`--system`) → per-interface network attribution and local-vs-system-pressure attribution,
   plus steal-time capture (user/system/iowait are already captured and printed — this item is the
   missing steal column and the analysis layer on top, not the raw mix itself).
3. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
   `comm`-pattern role tagging).

**Infra:**

4. Collapse `wspy-run`'s builtin profiles onto native `--passes` bin-packing. Most profiles are already
   collapsed as far as `--passes`'s own architectural constraints allow (their remaining separate passes
   use `--interval 1`, hard-fatal'd against `--passes`, or are IBS/`--tree`, excluded the same way); the
   one real remaining candidate is `deep-cpu-intel`, which still hand-authors 4 separate `wspy`
   invocations that don't touch any `--passes`-incompatible flag. Note: collapsing it changes on-disk
   output shape from 4 files to 1, so anything downstream assuming those 4 filenames (external scripts,
   `tests/capability_matrix.sh`) would need checking.
5. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead guardrails —
   needs deterministic micro-workloads plus the normalized store/stats infrastructure already shipped.

**Hardware counter expansion (Intel/AMD, non-GPU):**

6. Intel counter expansion (real DRAM bandwidth via `uncore_imc_free_running_0`/`_1`, true LLC/L3 via
   `uncore_cbox_0`..`_11`, per-core-domain/iGPU RAPL energy via the `power` PMU's `energy-cores`/
   `energy-gpu`, C-state residency via `cstate_core`/`cstate_pkg`, PEBS-based precise memory-latency
   sampling as the Intel counterpart to AMD IBS sampling-mode) — see the Intel hybrid / counter-grouping
   deep-dive's "Additional Intel counters worth adding" list for the full real-hardware-grounded detail
   per counter (that list's one GPU item, `i915` GPU PMU, is tracked in 4.4(b) instead).
7. Zen5 fine-grained scheduler-stall counters (split ALU/AGU scheduler-stall counters, op-cache/
   execution-queue events) and `IBS_LD_L1_DTLB_REFILL_LAT` — see the Zen5/IBS deep-dive's remaining open
   thread for detail.
8. Platform formula registry — versioned event/formula mapping per CPU family/model, for auditability.
   Design against a real third candidate input once one of the two items above lands, rather than
   against the two AMD-only data points available today.

**Heavier/optional pieces:**

9. Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions,
   resumable/selective re-execution) — full version of the lightweight config-file execution already in
   `wspy-run` (4.0); don't build both at once.
10. Process/thread migration diagnostics (did a process's threads actually move between cores during the
    run) — split out of 4.2's "Per-core imbalance/hot-core diagnostics" item; needs new instrumentation
    (periodic `/proc/<pid>/stat` `processor`-field sampling, or scheduler tracepoints) rather than just
    new analysis of data `--per-core` already collects.

## Deferred indefinitely
Explicitly not planned for any numbered release. Revisit only if a concrete need surfaces — don't let
these block or distract from 4.4/4.5 scoping.

- **Collector-plugin implementation** (perf stat / trace-cmd / GPU tools as collectors behind the
  `collector` field, normalization path) — the schema seam shipped in 4.0; no concrete non-wspy collector
  consumer exists to build the actual implementation against yet.
- **Optional dashboard backend** (e.g. Grafana) for exploratory slicing — static-site publishing already
  covers the real need; revisit only if that stops being enough.
- **Optional live TUI** (run progress, interval metrics, throttling/skew warnings) — the web UI already
  covers this; CLI-first model stays primary without a dedicated terminal surface.
- **General, table-driven syscall-level critical-path instrumentation** (`tree_open`'s "syscall name →
  number → decode → log-vs-aggregate" generalization) — six syscall families (futex, blocking I/O,
  connect, nanosleep, wait, poll) are still cheap enough as individual `if` branches in `ptrace_loop()`'s
  dispatch. Revisit only if a seventh syscall family comes up.
- **Low-overhead tracing alternative to `ptrace`** (`ftrace` tracepoints or minimal eBPF) for
  `--tree`/`--tree-open` — real value (`ptrace`'s stop-the-world cost skews I/O-heavy/fork-heavy
  measurements, see the Critical-path deep-dive below) but a genuine architecture change with no
  immediate forcing function. Revisit if the observer-effect problem becomes a concrete blocker on a
  specific workload rather than a standing theoretical caveat. **Already tried once, in `archive/
  wspy2.0/`:** that codebase had a selectable `--processtree-engine ftrace|ptrace|ptrace2|tracecmd`
  (`config.c`), and its `ftrace.c` engine is exactly this idea — enabled `sched_process_{fork,exec,exit}`
  tracepoints via `/sys/kernel/debug/tracing`, hand-parsed `trace_pipe` text lines. It was dropped in
  favor of `ptrace` because raw `ftrace` has no pause: nothing stops the exiting task between the kernel
  writing its trace record and the task actually being reaped, so under load the tree-builder's
  `/proc/<pid>/*` reads sometimes lost the race entirely (the process was already gone by the time the
  consumer got around to it) — worse than a staleness problem, a data-loss one, and it produced visibly
  inaccurate trees. `ptrace`'s `PTRACE_EVENT_EXIT` stop fixed this by construction: the tracee can't
  proceed to actually die until the tracer resumes it, so the read and the process's death can never
  race. This is why any future replacement of the exit-time snapshot (not the per-syscall stepping —
  see the syscall-argument-vs-timing distinction in the deep-dive) needs a synchronous in-kernel
  mechanism (e.g. a kprobe/fentry eBPF program snapshotting task state before returning), not just a
  different notification transport (`perf_event_open` tracepoints included) — swapping `ftrace`'s
  `trace_pipe` for a perf ring buffer would have made the loss less frequent, not eliminated it.
- **Optional deep trace analysis** (Perfetto-compatible export of tree+topdown+interval timelines) —
  depends entirely on the low-overhead tracing backend above; deferred alongside it.
- **Temporal drift detection** (cluster movement across versions/configs/machines) — needs far more
  historical run accumulation across the shipped clustering work than exists today to be meaningful;
  revisit once that history actually exists.
- **AMD IBS sampling-mode: `--interval`-integrated periodic rates** — `ibs_sample.c` only drains the perf
  ring buffer once, at end-of-run; a real per-tick rate needs an actual poll-loop architectural change, a
  different scale of work than the fixed-offset decode work that already shipped. The end-of-run-only
  rate is sufficient for now.
- **Uprobe-based function-argument capture** ("ltrace-style" hooks for hot functions found via
  symbol-level profiling, e.g. recovering GEMM dimensions from a BLAS library's hottest routine) — heavy,
  niche, root-required. Revisit if a real investigation actually hits the "Pareto list of hot symbols
  isn't enough, need the real call arguments" wall this was scoped for.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision. (Several
earlier open questions here have been resolved by shipped work or promoted to active backlog items —
native multi-pass execution, ARM64 support, publication automation, core/thread affinity, minimum
metadata set for publishable, cross-machine comparability scoring (`env_score`/`mixed-env`, shipped 4.3),
the static-vs-interactive-backend question (resolved: static-first, see "Deferred indefinitely" above),
and the `wspy-run` declarative-profile refactor (promoted to 4.4(a) above) — rather than a stale
"resolved" note here for each.)

- **Does wspy's counter-group naming/organization need a separate Intel-focused and AMD-focused split
  (CLI flags, `--counters=` group names, web UI panels), since today's single vocabulary can feel
  AMD-centric on Intel hardware?** Raised 2026-07-22 off a real Intel Coremark run where several
  requested groups (`opcache`, `memory`) silently produced zero columns. Recommendation: no — don't fork
  the vocabulary. The Preset/Configuration/Option deep-dive (now active 4.4(a) scope) already commits to
  one vocabulary shared by the CLI/`wspy-run`/web UI specifically so none of them invents its own mental
  model, and forking by vendor would double that surface while also making `wspy-summary`/`wspy-plot`'s
  cross-run comparisons harder, since those already lean on shared column *identity* across vendors. The
  actual problem is **coverage, not naming**: `intel_raw_events[]` has zero entries for
  `COUNTER_OPCACHE`/`COUNTER_MEMORY` today, so those group names are silent no-ops on Intel with no
  warning — tracked as 4.5's Intel counter expansion item above. A follow-up worth scoping alongside that
  work: making coverage/capability reporting explicit about "not implemented on this vendor" vs.
  "requested but failed to open" — today both look identical (silently zero columns) from the CLI/web UI.

## External brainstorming references
- ReBench — reproducible experiment configuration, resumable execution, explicit benchmark
  parameter tracking: https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) — static-site publication for benchmark trends with an interactive
  frontend model: https://asv.readthedocs.io/en/stable/
- Grafana OSS — optional dashboard-based slicing/templating if the interactive-backend path is
  taken: https://grafana.com/oss/grafana/
- Perfetto — timeline/trace analysis and SQL-based trace queries, relevant to the optional deep
  trace analysis pipeline (4.4): https://perfetto.dev/docs/
- OpenBenchmarking.org — public Phoronix Test Suite result archive; individual result pages expose an
  "Export Benchmark Data: Result File to Test Suite (XML)" link, the seed mechanism for the now-shipped
  "openbenchmarking.org-seeded single-test-point Phoronix suites" item (see "What shipped in 4.3"):
  https://openbenchmarking.org/

Note (2026-07-22): an earlier research pass hit an HTTP 403 fetching OpenBenchmarking.org directly from
this environment and left it unreviewed (see prior revisions of this note); a user-provided result URL
in this same conversation confirmed the export-XML link exists and is usable as a seed, so that blocker
was environment/fetch-specific, not a real access restriction — reviewed manually, not by this
environment's own fetch tooling.
