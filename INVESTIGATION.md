# wspy Investigation

A rolling roadmap: what's shipped, what's actively planned, and the design reasoning behind both.
Formerly `INVESTIGATION_4.0.md` — renamed once its content outgrew a single release (4.0, 4.1, and
now 4.2 are done; see below). Full design write-ups and validation narratives for work that's fully
shipped now live in `doc/INVESTIGATION_ARCHIVE.md`, out of the way of the open backlog.

Status (2026-07-22): **4.0, 4.1, and 4.2 are released and done** (v4.2 tagged and published as a
GitHub release, `wspy-release-notes.4.2.md` as its body — see `scripts/release_prep.sh`). **4.3 is now
underway.** Real Intel hybrid hardware became available for
the first time this cycle (a Raptor Lake HX host, "carlsbad") and an Intel counter-grouping
correctness pass found five confirmed hardware bugs plus one coverage gap (Gracemont E-core raw
events); all six are now resolved (five shipped, see "Shipped since 4.2" below; one is a documented
non-actionable perf-subsystem limitation, see "Known gaps") — nothing remains open from that pass.
This document was slimmed down for the 4.3 cycle (2026-07-21):
"What shipped in 4.0"/"4.1"/"4.2" are pointer lists only, with design write-ups and validation
narratives moved to `doc/INVESTIGATION_ARCHIVE.md`. A "Shipped since 4.2" rolling section (same idiom
described below) now tracks 4.3 progress until its backlog empties out and it folds into a proper
"What shipped in 4.3" section.

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
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, Intel hybrid/
  counter-grouping, topdown, the preset/configuration/option vocabulary). Each points back at the
  priority-list items it informs. Deep-dives for work that has since fully shipped live in
  `doc/INVESTIGATION_ARCHIVE.md`, not here.
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

## Shipped since 4.2
Rolling pointer list for the active 4.3 cycle (see "How to use this document" above) — folds into a
proper "What shipped in 4.3" section once 4.3's backlog empties out, the same way this same section
folded into "What shipped in 4.2" once that cycle finished. Grouped the same way as prior phases'
shipped lists; full root-cause detail lives in the Intel hybrid/counter-grouping deep-dive below, not
repeated here.

**Intel counter-grouping correctness fixes:** two hardware-verified bugs found on real Intel hybrid
hardware, both in `topdown.c` (PR #129) — `--per-core` silently measuring only the first core
(`intel_group_id`, a shared perf-group-leader fd, bled across back-to-back `setup_counters()` calls);
`--topdown`/`--topdown2` reporting all-zero whenever any other Intel counter opened first (Intel's Perf
Metrics `slots` sub-events require a literal `slots`-led group, and `ipc`'s default-on event usually
won leadership instead). Verified against real Raptor Lake HX hardware.

**Intel counter-group budget chunking:** the third originally-found bug (`topdown.c`) — Intel's
single-shared-group design funneled every non-topdown counter into one perf event group with no size
limit, cascading into wholesale `EINVAL` loss once a combined group exceeded real hardware PMU capacity
or spanned two different underlying PMUs. `cache_counter_group()`/`raw_counter_group()` now chunk Intel
counters into hardware-budget-respecting groups (`is_group_leader` every `available_counters`/
`num_counters_available` counters, same as AMD/ARM already did) instead of one unbounded shared group,
while the topdown/topdown2 Perf Metrics family stays exactly one dedicated group regardless of size (a
kernel-enforced "literal `slots` leader" requirement, not a PMC-budget one). This also let
`setup_counters()` drop the `intel_group_id`/`intel_topdown_group_id` module statics entirely in favor
of the same `is_group_leader`-driven local `group_id` AMD/ARM already used — structurally removing the
class of cross-call state-leak bug the first fix above had to patch.

**x86 hybrid core-type detection:** `--affinity=coretype=<id>`/`--list-affinity` (`affinity.c`) now
detect Intel P-core/E-core and AMD Zen5/Zen5c core-type groups on x86 by reusing `cpu_info.c`'s
existing per-core vendor classification when the ARM-only `MIDR_EL1` pass finds nothing — previously
x86 always reported 0 core types. Verified against a real 32-thread Intel P-core/E-core host (16+16
threads, both `coretype=0|1` resolving correctly); the web UI's discovery endpoint carries the new
vendor-tagged core types too.

**Intel L2 topdown unsigned-underflow fix (`topdown.c`, Intel counter-grouping correctness track):**
`print_topdown()`'s `VENDOR_INTEL` branch computed its four L2 splits (`backend_cpu`, `speculation_pipeline`,
`frontend_bandwidth`, `retire_fastpath`) as plain `unsigned long` subtraction of two independently-read,
independently-multiplexed perf counters — the exact bug class `safe_sub()` was introduced to fix on
AMD's side (see that function's own comment), just never applied when the Intel branch was written.
When a "child" counter reads fractionally above its "parent" (routine measurement noise), the
subtraction wraps to near-`ULONG_MAX` and the resulting percentage explodes — reproduced live on
carlsbad with a branch-heavy workload (`spec_pipeline_pct=173616230410.5`, same signature as the
originally-logged `72407003082176.4`). Fix: route all four through the already-existing `safe_sub()`.
Regression test (`test_intel_topdown_l2_underflow`) confirmed to fail pre-fix and pass post-fix.

**RAPL/`energy-pkg` wrong-scope fix (`cpu_info.h`/`topdown.c`/`power.c`/`ibs.c`, Intel counter-grouping
correctness track):** `setup_counters()` special-cased system-wide (`pid=-1`) opens only for
`pe.type == PERF_TYPE_L3` specifically — an incidental type-value match, not a real marker. RAPL's
`power` PMU (whose driver sets `task_ctx_nr = perf_invalid_context`, rejecting task-scoped opens
outright) fell through to the generic per-process (`pid=0`) branch on any host where its dynamic PMU
type didn't happen to numerically collide with `PERF_TYPE_L3`'s sentinel value (14) — true on this
Intel host (`power`'s real type is 35), not Intel-specific (any AMD host without the same lucky
collision hits it too). Fix: a new explicit `requires_system_wide` bit (`struct counter_info`,
`cpu_info.h`) set by `power_counter_group()` (RAPL), `ibs_counter_group()` (AMD IBS, documented
system-wide-only), and `raw_counter_group()`'s AMD L3 entries; `setup_counters()`'s dispatch now checks
this marker directly instead of the `PERF_TYPE_L3` coincidence, which it no longer references at all.
Verified live on carlsbad: `perf_event_open()`'s failure mode changed from `EINVAL` (wrong pid/cpu args,
rejected regardless of privilege) to `EACCES` (correct args, just needs `CAP_PERFMON`/root) — exactly
the signature this bug's own diagnosis predicted.

**Per-core-type-aware Intel raw event tables — Gracemont E-core support (`cpu_info.c`/`topdown.c`/
`wspy.c`, Intel counter-grouping correctness track):** `intel_raw_events[]` hardcoded `PERF_TYPE_RAW` as
every raw event's `device_type` — silently correct for P-cores only because `cpu_core`'s real dynamic
PMU type (4) happens to numerically equal `PERF_TYPE_RAW`'s own enum value; `cpu_atom`'s real type is
10, and Gracemont E-cores needed entirely different raw event *encodings*, not just a different
`device_type`. `core_is_per_core_eligible()` (`wspy.c`) excluded `CORE_INTEL_ATOM` outright to avoid
silently mismeasuring E-cores with P-core-only-correct events. Every encoding below was read directly
off carlsbad's live `cpu_atom` PMU (`/sys/devices/cpu_atom/events/`, `perf stat -e cpu_atom/<name>/ -vv`
for the resolved raw config), not guessed, and cross-checked against real counter runs. Two genuine
hardware differences from P-cores, not just a `device_type` substitution: (1) **Gracemont has no
`slots`/fixed-counter "Perf Metrics" register at all** (`cpu_core` exposes 20 named perf events
including `slots`; `cpu_atom` exposes 14, none named `slots`) — its 4 topdown events
(`topdown-retiring`/`-bad-spec`/`-fe-bound`/`-be-bound`) are ordinary counting events
(`event=0xc2/0x73/0x71/0x74`), not members of any kernel-enforced "literal slots leader" group;
`print_topdown()`'s `VENDOR_INTEL` case now synthesizes a slots denominator from `cpu-cycles * 5`
(mirroring AMD's `cpu-cycles * width` pattern) when no real `slots` counter is found — the width was
measured empirically: `(retiring+bad-spec+fe-bound+be-bound)/cpu-cycles` across 4 independent real runs
on this host measured 4.9997 every time. No L2 topdown breakdown exists on Gracemont at all (confirmed
absent from both sysfs and `perf list`) — those columns stay `0.0` for E-core rows via the same "vendor
doesn't populate this" convention ARM already relies on. (2) `l2_request.all`/`.miss` and the
`br_inst_retired.*`/`br_misp_retired.*` family exist on `cpu_atom` too, but with different umask
encodings than P-core for the same event numbers (e.g. `l2_request.all`: P-core `umask=0xff` vs.
Gracemont `umask=0x0`). Fix: `cpu_info.c`'s existing Raptor/Alder Lake hybrid-detection block now also
resolves each core's real dynamic PMU type via `/sys/devices/<pmu>/type` (reusing the same vendor-
agnostic `mark_cpus_for_pmu()` helper `discover_arm_pmu_topology()` already used for ARM) instead of
leaving `pmu_type` at its never-resolved `PERF_TYPE_RAW` default; a new `intel_atom_raw_events[]`
(`topdown.c`) carries the verified Gracemont encodings; `raw_counter_group()`/`setup_counter_groups()`
gained a `core_class` parameter to select it (`CORE_INTEL_ATOM`) instead of `intel_raw_events[]`, and
skip the P-core-only forced-single-group chunking rule for it. `core_is_per_core_eligible()` no longer
excludes `CORE_INTEL_ATOM`. `COUNTER_TOPDOWN_BE` (`--topdown-backend`'s `exe_activity.*`/
`memory_activity.*`) has no confirmed Gracemont equivalent — deliberately left as zero coverage rather
than guessed, same philosophy as the rest of this fix. Verified live on carlsbad end to end, with
`CAP_PERFMON`: `--per-core --topdown --affinity=coretype=1` on real E-cores reports sane, real values
(e.g. `retire=13.7,frontend=28.7,backend=44.2,speculate=13.3%,confidence=1.00,sanity=99.8%`,
`224/224 counters_measured`); `--per-core --topdown2` shows real non-zero L2 breakdown on P-core rows
(unchanged) alongside the correct, honest `0.0` L2 columns on E-core rows in the *same* run;
`--per-core --branch --cache2` reports real, plausible, vendor-distinct branch-miss/L2-miss rates on
both core types. `strace` additionally confirmed E-core `perf_event_open()` calls use `type=0xa` (10,
`cpu_atom`'s real dynamic type) with Gracemont's own configs
(`0xc0`/`0x3c`/`0xc2`/`0x73`/`0x71`/`0x74` for instructions/cycles/topdown,
`0xc4`/`0xc5`/`0x7ec4`/`0xebc4` for branch, `0x24`/`0x124` for L2).

**Phoronix per-test option-combination count (`ledger.c`, Tier 7):** `wspy-ledger --phoronix-option-combos`
scans the same `--phoronix-profiles-dir` tree `--unavailable-deps` already reads and reports each matching
workload's full option-matrix size — the product of every `<TestSettings>/<Option>`'s `<Menu>` `<Entry>`
count in its `test-definition.xml` — as a new `option_combinations`/`option_combinations_note` pair (CSV) or
a bracketed suffix (human report), surfaced up front instead of discovered partway through a long
batch-run sweep. Verified against real installed profiles: blender-1.2.1's own test-definition.xml
(`system/blender-1.2.1`) resolves to exactly the confirmed 5×2=10. An `<Option>` with no `<Menu>` at all
(a free-form input like fio's "Disk Target" or iperf's "Server Address") can't be enumerated, so it's
excluded from the product and the count is reported as a lower bound (`combo_has_freeform`) rather than
silently undercounted; if a workload name matches more than one installed profile (different suites or
kept versions) and they disagree on the count, `combo_ambiguous` flags it rather than silently picking
one. Purely static `test-definition.xml` parsing, same "deliberately not a real XML parser" convention
`scan_phoronix_dependencies()` already established for this file — no need to run anything. Independent of
`--unavailable-deps`; both share `resolve_profiles_dir()`'s profiles-dir resolution and may be given
together. Feeding this count into the (shipped) `--tree` pass timeout item's `BATCH_RUN_MULTIPLIER` —
replacing that item's blind 5.0 guess with a grounded number — remains a follow-up, not yet wired up.

**openbenchmarking.org-seeded single-test-point Phoronix suites, front-end phase
(`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab, Tier 7):** decomposes an already-published
OpenBenchmarking result or an installed/exported Phoronix suite into one minimal single-test-point suite
per (test, option-combination), materialized under `workload/phoronix/<test>/<options>/` and registered
with `wspy-ledger --add`. Three source methods (a result URL/ID, an XML file already on disk, or an
installed PTS suite under `~/.phoronix-test-suite/test-suites/`) all reduce to the same
`(test_id, arguments)` pair list, parsed from either of the two real Phoronix XML shapes (suite-definition
`<Execute>` elements or composite/result-file `<Result>` elements) — verified against real files on a live
PTS install, including a real fetched OpenBenchmarking result. Idempotent/additive re-runs (an existing
`<test>/<options>/` directory is left untouched and reported `exists`), and a per-test `installed` flag
(`phoronix-test-suite info`) surfaces which materialized points still need `phoronix-test-suite install`
run by hand. The directory-name slug for a long `Arguments` string (a common failure mode: two option
combinations sharing a long prefix, e.g. a model file path, differing only near the end) gets a short
content hash appended whenever it would otherwise be truncated, rather than silently truncating and
colliding two different combinations into one slug — confirmed live against a real OpenVINO result
where every one of 12 model/precision combinations' `-hint throughput`/`-hint latency` pair collided
under a plain 60-character truncation before this fix.

The Phoronix tab also shows a live inventory of already-materialized test points
(`joblib.list_materialized_phoronix_test_points()`), each with a "Use in Run tab" button that copies its
suite into `~/.phoronix-test-suite/test-suites/local/<identity>/` (`joblib.
copy_phoronix_test_point_to_local_suite()`) and prefills the Run tab's workload/suite/benchmark fields —
closing most of the gap to the "runner script" half this item originally deferred, without the invasive
change a literal `--output-root` override would have needed (the web launcher's report page, `/compare`,
and bundle export all hard-assume one fixed `--output-root`). A run launched that way still writes its
real files under the normal `--output-root` (`suite=phoronix`, `benchmark=<identity>` — every existing
route keeps working unchanged) and additionally gets symlinked at
`workload/phoronix/<test>/<options>/runs/<run-id>/` (`joblib.link_phoronix_test_point_run()`, best-effort)
purely so it's still browsable as a subdirectory of the test point directory. Because each generated
suite is exactly one test point, its wspy capture is already segmented at the source — no post-hoc
composite.xml/log-timestamp correlation needed for runs built this way (doesn't replace
`wspy-phoronix-segment` for suites run the ordinary multi-test-point way, but sidesteps the problem
entirely for anything built through this path). Longer-term, once enough `<test-name>/<options-info>`
directories accumulate this way they form a pre-profiled library keyed on real Phoronix test identity —
a natural feed for Tier 2's clustering/nearest-neighbor work once that lands, and a cheaper way to grow
`wspy-ledger`'s workload coverage than hand-authoring one-off `wspy-run` invocations per benchmark.

Also writes one `workload/phoronix/<test>/README.md` per bare test name (one directory level above the
`<options>/` subdirectories, since it describes the test regardless of option combination), rendered from
the same `phoronix-test-suite info <test_id>` call `installed` already used (`fetch_phoronix_info_fields()`/
`write_phoronix_test_readme()`, `joblib.py`) — the test's Description plus a handful of other high-level
fields (type, license, supported platforms, project/OpenBenchmarking.org links). Same idempotent/additive
convention as `suite-definition.xml`: an existing README is left untouched on re-import. Skipped (not an
error) when `--no-check-installed` disabled the `info` lookup or the lookup itself failed.

The inventory table itself is now grouped by test rather than shown as one flat, recency-ordered list of
option combinations — `list_materialized_phoronix_test_points()`'s own ordering (newest `generated_at`
first) is the right default for "what did I just import" but stopped scaling once a host accumulates
hundreds of points across many imports and a human is instead trying to find one specific test.
`joblib.group_materialized_phoronix_points_by_test()` re-buckets that flat list into
`<test>` → `<options>` groups sorted alphabetically by bare test name (each group's own points re-sorted by
options slug); `joblib.read_phoronix_test_description()` pulls each group's one-line summary straight back
out of the README.md above, so the inventory doesn't need a second `phoronix-test-suite info` round-trip
just to label a collapsed group. Each test renders as a `<details>` block (`render_phoronix_inventory_groups()`,
`web/server.py`) with a text filter above matching test name/description/option-slug substrings
(`wirePhoronixTab()`, `web/static/app.js` — client-side only, no new endpoint), plus expand-all/collapse-all
buttons.

**`cache_counter_group()`'s "instructions" entry opened at the wrong PMU type (`topdown.c`) —
vendor-agnostic, not Intel-specific, though surfaced by the same real Coremark run above.** The
synthetic `"instructions"` entry `cache_events[]` carries alongside its `PERF_TYPE_HW_CACHE` rows (a
genuine `PERF_TYPE_HARDWARE`/`PERF_COUNT_HW_INSTRUCTIONS` event, used only as `print_cache()`'s "N per
1000 inst" denominator) never had a `device_type` set, so `setup_counters()` opened it at the whole
group's fixed `PERF_TYPE_HW_CACHE` type instead — and `PERF_COUNT_HW_INSTRUCTIONS` (`1`) numerically
collides with L1I-read-access's own `PERF_TYPE_HW_CACHE` encoding (also `1`, confirmed against
`<linux/perf_event.h>`), so it silently requested a duplicate of `l1i-read` rather than real instruction
retirement, on every vendor. Fix: `cache_counter_group()` now records each `cache_events[]` entry's real
`type_id` per-counter; `setup_counters()`'s `pe.type` resolution now also honors it for
`PERF_TYPE_HW_CACHE` groups (previously that per-counter override only applied to `PERF_TYPE_RAW`
groups).

**AMD IBS sampling-mode support (`ibs_sample.c`/`ibs_sample.h`, PRs #143/#144) — done for this cycle,
removed from the open backlog.** `--ibs-sample` mmaps the perf ring buffer with `PERF_SAMPLE_RAW`
(nothing in wspy read a perf mmap ring buffer before this) and decodes each sample's fixed-offset
fields into end-of-run rate estimates: op-side `dc_miss`/`dc_l1tlb_miss`/`dc_l2tlb_miss`/`op_brn_misp`
(of branch-retiring ops) plus `IbsOpData2`'s two scheme-independent signals (`dram_rate`,
`remote_node_rate`); fetch-side `ic_miss`/`l1tlb_miss`/`l2tlb_miss`. Draining only happens once, at
end-of-run (never from `timer_callback()`'s `SIGALRM` handler — ring parsing isn't async-signal-safe
and wspy has no poll loop), so `--ibs-sample` + `--interval` zeroes periodic rows and populates only
the final tail row — documented, not silent. `IbsOpData2`'s full named category breakdown (its meaning
differs between pre-Zen4 and Zen4+/`zen4_ibs_extensions` hardware, confirmed against the kernel's own
decoder) is decoded scheme-agnostically but only *named* in human-readable output, never baked into a
permanent CSV column. The remaining raw-address fields (`IbsBrTarget`/`IbsDcLinAd`/`IbsDcPhysAd`) and
`IbsOpData4` (no documented bitfield layout) are deliberately out of scope permanently, not deferred —
see the Zen5/IBS deep-dive's item 6 for the full reasoning. `--interval`-integrated periodic rates
(needs a real poll-loop architectural change) is split out as its own 4.4 priorities item.

**`--ibs-sample` wired into a real `wspy-run` profile, plus its own `CAP_PERFMON` permission
requirement discovered and fixed.** New `ibs-sample` builtin profile (`wspy-run`: `--ibs-sample
--no-ipc`, deliberately no `--csv`/`--interval` — the named breakdown is human-readable-only and the
ring buffer only drains at end-of-run); `zen4plus-deep` now composes `deep-cpu,ibs-sample,tree-heavy`
in place of the counting-mode `ibs-memory-deep` it originally shipped with (that profile's own
composition, "Shipped since 4.2" above), with `--no-rusage` added to just its own `ibs` pass since
`deep-cpu`'s `counters` pass already captures the same elapsed/utime/stime block. Real-host testing
found `--ibs-sample` (like `--ibs-basic`/`--ibs-memory-deep`) needs root or `CAP_PERFMON` — IBS is
system-wide-only monitoring, and `perfmon_capable()` gates that identically to `--power`'s RAPL access;
confirmed live end-to-end (not just the `EACCES` symptom): granting `cap_perfmon=ep` to the `wspy`
binary took `--ibs-sample` from `EACCES` on every counter to real sample counts.
`scripts/setup_perf.sh`'s existing `CAP_PERFMON` grant (previously described as `--power`-only) now
documents covering IBS too — one grant, no script changes needed beyond the description. Also fixed a
real bug this surfaced in the web launcher's Check button: `probe_ibs()` only recognized counting-mode's
`counters_measured`/`counters_requested` CSV trailer, so a genuine `--ibs-sample` permission failure was
masked as "could not parse counter coverage" instead of the actionable `CAP_PERFMON` hint — fixed by
checking for open failures before attempting to parse coverage, and by probing with `--csv` regardless
of the real pass's own flags (that trailer is generic across every counter group, not counting-mode-
specific).

**Baselines and regression/anomaly detection (`wspy-summary --check-regression`, PR #148) — done for
this cycle, removed from the open backlog.** New standalone `wspy-summary --check-regression
<hostname>:<run_id>` mode (mutually exclusive with `--trace`) compares a run's own per-metric values
against a *rolling* baseline — every strictly-earlier run sharing the same `--group-by`/
`--group-by-option` bucket the target run itself belongs to, reusing `compute_stats()`/`compute_ci95()`/
`compute_verdict()` verbatim rather than inventing new statistics. No new store schema: "which
historical runs count as comparable" is defined identically to the existing group-by summary table, not
by a separately invented matching-key set. A metric outside the baseline's 95% CI is classified
`above`/`below` — deliberately direction-neutral, not asserted as "regression," since this codebase has
no per-metric higher-is-better/lower-is-better table anywhere and inventing one now would repeat the
same cross-scheme category-mapping risk `--ibs-sample`'s `IbsOpData2` decode already avoided (see above)
— the tool reports the CI deviation and its direction, leaving "is this actually bad" to a human or a
future per-metric semantics table. Verified against real `wspy-store` data end-to-end, including a
genuine flagged deviation from a zero-variance baseline.

**Machine/environment comparability scoring (`env_score`/`mixed-env`, PR #149) — done for this cycle,
removed from the open backlog.** The deferred, *scored* version of 4.2's exact-match "mixed-pmu" bucket
check, extended across `run_environment`'s fuller provenance surface (BIOS, microcode, governor, memory,
virtualization) rather than just PMU setup — no diff/score between two runs' environments existed
anywhere in the codebase before this. Extends the existing `struct bucket`/`compute_verdict()` machinery
rather than adding a new CLI mode: `env_score` is a default-on column on every bucket row, in both the
main group-by table and `--check-regression`'s baseline rows. Unweighted fraction of 8 tracked fields
(`virt_role`, `hypervisor_vendor`, `microcode_version`, `bios_vendor`/`bios_version`/`bios_date`,
`cpu_governor`, `memory_total_kb`) that agreed across a bucket's contributing runs — deliberately not a
hand-tuned per-field point scheme, the same reasoning that kept IBS sampling-mode's cross-scheme category
mapping out of a permanent CSV schema. `memory_total_kb` uses a 5% tolerance (routine firmware/DIMM
jitter); every other field is exact match; a field only counts once mutually comparable
(`hypervisor_vendor` self-excludes on host runs with zero special-casing). A bucket with zero comparable
fields gets an explicit no-data sentinel, never a fabricated 0%/100% — absence of provenance data is not
evidence of a mismatch. New `--min-env-score` flag, default 0.8. Also fixed a real pre-existing bug found
while researching this: `upsert_run_environment()` (`store.c`) silently collapsed a genuine JSON `null`
for `memory_total_kb`/`cpu_governor_uniform` to `0`/`false`, indistinguishable from a real measurement —
every text provenance field already preserved `null` correctly via `bind_text_or_null()`, these two
numeric/bool fields had no equivalent. Verified against real `wspy-store` data end-to-end.

**Distribution-first reporting: per-run IPC quantile features (`ipc_p10`/`ipc_p90`/`ipc_iqr`, PR #150)
— done for this cycle, removed from the open backlog.** Extends `extract_run_features()` (`store.c`) —
the feature vocabulary `wspy-archetype` already consumes and this doc calls the clustering groundwork —
with per-run distribution-shape features derived from a single run's own `--interval` tick data, rather
than today's per-metric means. Clustering-prep feature-vocabulary richness, not clustering itself (the
next Tier 1 item). Scoped to `ipc` only for this first pass (the most universally-collected metric,
already the primary signal `phase.c`/`print_topdown()` use elsewhere) via a per-metric helper
(`extract_quantile_features_for_metric()`) that's a one-line extension per additional metric later.
Linear-interpolation percentiles (R-7/numpy-default method); requires >=4 `--interval` ticks
(`QUANTILE_MIN_TICKS`) or all three features record unavailable rather than a fabricated value from too
few points. The extraction query's `core IS NULL` guard is load-bearing, not decorative:
`metric_values.row_index` means a genuine tick ordinal for `--interval` runs but one core's snapshot for
`--per-core` runs — without it, a `--per-core` run's cross-core spread would be silently quantiled as if
it were a time series, colliding with `parallelism_proxy`'s already-covered signal under a different
feature name. `FEATURE_SET_VERSION` 1.1→1.2 (informational only). Verified against a real `--interval`
run end-to-end: sorted IPC ticks `[0.15,0.15,0.17,0.23,0.87,0.93]` produced `p10=0.15`/`p90=0.9`/
`iqr=0.555`, matching hand-computed expected values exactly.

**Nearest-neighbor search (`wspy-archetype --nearest`, PR #152) — the nearest-neighbor slice of Tier 1
item 1 below; K-means clustering and cluster profile cards remain open, rescoped there.** New standalone
`wspy-archetype --nearest <host>:<run_id> [--k N]` mode: given a target run, ranks every other run in the
store by a coverage-aware distance computed only over the `run_features` both runs actually have
`measured` — root-*mean*-square (dividing by the shared-feature count, not taking a root-sum-square) over
z-score-standardized differences, so a pair sharing 6 features isn't penalized purely for sharing less
than a pair sharing 18. Population mean/stddev for standardization are computed once across every loaded
candidate, not per-pair, so every pairwise distance shares the same scale; a zero-variance feature
(identical value across every candidate) is excluded rather than causing a divide-by-zero. The
`compared_features` shared-dimension count is always reported alongside each distance — coverage
transparency, matching the `env_score`/baseline-`n` convention already established elsewhere in this
codebase, rather than silently hidden. Mirrors `--run`'s colon-parsing and not-found exit-code
conventions; `--command`/`--hostname` reused as candidate-pool filters. Clustering/cluster-profile-cards
deliberately left out of this pass: K-means with a common-subspace distance has no clean textbook answer
for how a centroid should be computed when cluster members don't all share the same available features —
real, separate design work, not wasted effort building this distance function first. Verified via 8 new
`test_archetype.c` cases (basic ranking, common-subspace RMS normalization, zero-shared-feature exclusion,
zero-variance no-crash, `--k` limiting, `--command`/`--hostname` filters, target-not-found,
target-with-no-measured-features) plus the full `run_tests.sh` matrix.

**K-means clustering + cluster profile cards (`wspy-archetype --kmeans`) — closes out Tier 1's clustering item,
the last item in Tier 1; Tier 1 is now fully shipped for 4.3.** New standalone `wspy-archetype --kmeans
<n> [--seed <n>] [--iterations <n>]` mode partitions every candidate run into `n` clusters over the same
coverage-aware z-standardized distance `--nearest` uses, then prints one row per member (grouped by
cluster, closest-to-centroid first) carrying a "profile card" of the `KMEANS_TOP_FEATURES` dimensions
where that cluster's centroid sits furthest from the population mean. Resolves the design question this
item was deferred over: a centroid, per dimension, is the *available-case mean* — averaged only over
whichever cluster members actually measured that dimension, so a dimension no member measured simply
doesn't appear in the centroid rather than requiring a fabricated value. This is the standard "partial
distance strategy" for clustering under missing data (Dixon 1979; also underlies Hathaway & Bezdek's
fuzzy-c-means-with-missing-data extension), not a novel invention — just not previously wired up here,
and it mirrors `nearest_neighbor_distance()`'s own "only shared dimensions count" idiom on the assignment
side too (`centroid_distance()`, same RMS-over-shared-dimensions/zero-variance-exclusion shape).
Initialization is k-means++ (Arthur & Vassilvitskii 2007), seeded from real data points rather than a
fabricated average point (a real point is always well-defined on its own measured dimensions, sidestepping
the missing-data question for the one spot it would otherwise bite hardest: a centroid with zero members
yet to average over); `--seed` makes a given `(data,k)` pair reproducible via `rand_r()`-based sampling
independent of any other code's RNG state. Empty clusters (a real Lloyd's-algorithm failure mode) are
healed each iteration by stealing the single worst-fit point from a cluster with more than one member,
rather than letting a cluster's centroid drift somewhere no point wants it. Verified via 5 new
`test_archetype.c` cases (well-separated groups land in distinct clusters, heterogeneous-coverage
centroid computation doesn't crash, `k` greater than candidate count returns the standard "insufficient
data" exit code, same-seed determinism, and `k` larger than the natural group count still yields `k`
non-empty clusters) plus the full `run_tests.sh` matrix.

**Phase-aware topdown (`wspy-summary --phase-topdown`) — closes 4.3 Tier 2's phase-aware topdown
item.** New standalone
`wspy-summary --phase-topdown <hostname>:<run_id>` mode: breaks one run's own topdown output down by
`--interval` phase (warmup/steady/degraded, `phase.c`'s per-tick classification). store.c's
`ingest_csv_metrics()` already tagged every `metric_values` row with its tick's phase label the moment
`phase.c` shipped (the CSV's `phase` column is carried straight through, no schema change needed here)
— this is simply the first thing that actually reads that column back out and correlates it with
topdown specifically, closing the topdown deep-dive's long-deferred MVP criterion ("one benchmark run
demonstrates phase-specific topdown shifts in generated summary output"). Reports each topdown column's
per-phase mean/n (blank, never fabricated as 0, when a phase has no data for that column) plus a
`drift_pct` (largest phase-to-phase swing for that column) and a trailing note naming the single
largest drifter overall; deliberately scoped to topdown's own CSV columns
(`retire`/`frontend`/`backend`/`speculate`/`contention_pct` plus the L2 splits — the literal raw CSV
column names `metric_values.metric_name` stores, *not* the `retire_pct`-style `run_features` feature-
vocabulary names `archetype.c` reads from a separate table), not a generic phase-vs-every-metric report.
A run collected without phase data (aggregate, `--per-core`, or no `--interval`) degrades to an
explicit notice rather than a silently-empty table, same convention as `--nearest`'s "target has zero
measured features" case. Verified end-to-end against a real AMD Zen5 host run (`--interval --topdown`,
piped through `--run-index`/`wspy-store`) in addition to 6 new `test_summary.c` cases (full 3-phase
drift, largest-drift trailing note, no-phase-data graceful degradation, target-not-found, single-
phase-observed, and a metric present in only one of two observed phases) plus the full `run_tests.sh`
matrix.

**Composite attribution (`wspy-archetype`'s new `memory_attribution` axis) — the topdown+cache/TLB/IBS
cross-referencing half of 4.3 Tier 2's composite attribution item (the blocking-syscall-split modifier
that closes it out entirely is its own separate "Shipped since 4.2" entry below).**
A fifth `wspy-archetype` axis, alongside `resource_dominance`/`parallelism_shape`/`control_flow_style`/
`runtime_stability`: cross-references topdown's own `backend_pct` (the same L1 category
`resource_dominance` already ranks) against every independently-measured cache/TLB/IBS signal a run
collected — `dcache_miss_pct`/`l2_miss_pct`/`l3_miss_pct`, `itlb_miss_per1k`/`dtlb_miss_per1k` (AMD-only,
`--topdown-optlb`) and the newly-promoted `itlb_generic_miss_pct`/`dtlb_generic_miss_pct` (cross-vendor,
`--tlb` — added specifically for this item after real-hardware testing showed the AMD-only pair almost
never fires in practice, since `--topdown-optlb` is far less commonly collected than plain `--tlb`),
`smt_contention_pct`, and the newly-promoted `ibs_dc_miss_pct`/`ibs_dram_pct` (AMD IBS sampling-mode,
shipped 4.3). A "memory-bound" topdown read (`backend_pct` at/above a 20% floor) corroborated by at
least one of these independently-measured signals is materially higher-confidence than topdown alone;
one with *zero* corroboration from any signal that *was* actually collected is flagged `uncorroborated`
— genuinely new information a single-counter heuristic can't produce, since it suggests the true
bottleneck may be something not measured yet (cross-NUMA latency, lock contention masquerading as a
backend stall) rather than a straightforward capacity/miss-rate problem. `unknown` covers both
`backend_pct` never having been measured and `backend_pct` being significant but zero corroborating
signals having been collected at all (kept as two states via `.available`, even though both currently
render the same label). Deliberately does *not* attempt to rank *which* cache level a stall concentrates
in — that's a different, stall-cycle-attribution question only `--topdown-backend`'s own dedicated
`l1_bound_slots_pct`/etc. group can honestly answer (a miss *rate* and a stall-cycle *attribution* are
related but distinct measurements); conflating the two would overclaim precision this signal set doesn't
have, so this axis stays a corroboration check, not a hierarchy-level diagnosis. `memory_attribution_reasons`
lists exactly which signal(s) fired (`corroborated`) or were checked and found unremarkable
(`uncorroborated`) as `name=value`/`checked:name` pairs. Thresholds (`MEMORY_ATTRIBUTION_FLOOR_PCT=20.0`,
per-signal `*_ELEVATED_*` constants) are the same "deliberately simple v1 starting point, not derived
from a formal study" caveat `DOMINANCE_*_MARGIN`/`phase.c`'s own thresholds already carry. Verified via
11 new `test_archetype.c`/`test_store.c` cases (all four label outcomes directly, plus end-to-end through
`score_runs()`'s CSV pipeline and the two newly-promoted feature-extraction paths) and against a real AMD
Zen5 host run (`--topdown --dcache --cache2 --cache3 --tlb`, piped through `wspy-store`) — real IBS
sampling itself needs root this session couldn't grant, so the IBS corroboration path is unit-tested
only, not yet confirmed against a live IBS sample; the generic-vs-AMD-only TLB feature split above was
itself a direct finding from that real-hardware run, not anticipated in the original design.

**Core-class-aware topdown — closes 4.3 Tier 2's core-class-aware topdown item.** Two pieces, both
surfaced by re-examining what
"core-class-aware" actually requires rather than assuming the existing per-core-type raw event tables
(shipped 4.2) already covered it end to end:
1. **A real correctness gap in default (non-`--per-core`) mode on Intel hybrid hosts, now warned
   about.** Research into this item's own "weighted aggregate" framing turned up something more
   fundamental than a missing summary feature: `setup_counters()`'s default (non-`--per-core`) path
   opens raw events with `device_type` left at the literal `PERF_TYPE_RAW` constant, never patched to
   the real per-host dynamic `cpu_core`/`cpu_atom` PMU type the way `bind_core_counter_groups()`
   (`wspy.c`) already does inside the `--per-core` loop. On current hardware this happens to still bind
   correctly to the P-core PMU only because `cpu_core`'s real sysfs type coincidentally equals
   `PERF_TYPE_RAW`'s own enum value (4) — not by design. Any execution the scheduler places on an
   E-core simply can't be counted by that mismatched-PMU-type event: no error, no dropped-sample
   marker, just a silently under-counted total, with zero indication anything was missed. `topdown.c`'s
   `setup_raw_events()` now warns on `cpu_info->is_hybrid && !aflag`, mirroring the existing ARM
   `mixed_pmu_types` warning exactly (that warning turns out to guard the identical underlying
   mechanism — raw events bound to one PMU type silently not counting on a task migrated to a
   different-PMU-type core — just phrased more softly). AMD Zen5/Zen5c doesn't need the same warning:
   confirmed there's no per-core-class raw table split on that vendor yet (`raw_counter_group()`'s own
   comment), so there's no PMU-type mismatch to warn about.
2. **The weighted-aggregate/summary-presentation work itself: `wspy-core-report --weight-by <metric>`.**
   New optional flag weights each core's contribution to the existing "Cross-core stats" and "Core-class
   summary" mean/stddev/cv by that same core's own value of a chosen metric column (e.g. `ipc` or
   `cpu-cycles`) — an "activity-weighted" combination rather than counting every core equally regardless
   of how much work it actually did. min/max/hot/cold stay exactly the raw per-core values either way
   (weighting only changes how per-core values combine into one number, not which core was highest/
   lowest). A core lacking data for the weight metric is excluded from every weighted result entirely,
   never silently zero- or unity-weighted (`gather_core_values()`'s new `weight_metric` parameter).
   Population-style weighted variance (divides by `sum(weights)`, not a bias-corrected denominator) —
   same "deliberately simple, not a formal derivation" spirit `DOMINANCE_*_MARGIN`/`phase.c`'s own
   thresholds already carry. Opt-in and fully backward-compatible: default output (no `--weight-by`) is
   byte-identical to before. Verified against a real per-core `--topdown` capture on this AMD host: a
   single-busy-process workload's unweighted cross-core `ipc` mean (0.69, diluted by 31 idle cores) vs.
   `--weight-by ipc`'s activity-weighted mean (1.94, correctly dominated by the one core actually doing
   the work) — a concrete demonstration of exactly the problem this item asked to fix. 8 new
   `test_core_report.c` cases (weighted-mean correctness, raw-value min/max/hot/cold unaffected by
   weighting, zero-weights fallback, missing-weight-data exclusion) plus the full `run_tests.sh` matrix;
   the Intel hybrid warning itself has no dedicated unit test (no existing precedent for asserting on
   `warning()` text anywhere in this codebase, including the ARM warning it mirrors) but is a 3-line
   conditional gated behind a flag (`is_hybrid`) that's false on every test host today, so zero behavior
   change for any existing test.

**Composite attribution's blocking-syscall-split modifier — closes 4.3 Tier 2's composite attribution
item entirely.** `wspy-archetype`'s `memory_attribution` axis (shipped) now folds in the
"no blocking-syscall activity" vs. "heavy blocking-syscall activity" vs. "runnable but not scheduled"
three-way split from the 4.1 critical-path work, ahead of the cache/TLB/IBS corroboration checks it
already did: a "memory-bound" topdown read on a phase/run dominated by kernel-blocking (futex/io-wait)
or scheduler run-delay isn't a genuine hardware stall at all, so asking whether cache/TLB/IBS
"corroborate" it would be misleading regardless of what they show. Two new labels, checked before the
existing `corroborated`/`uncorroborated` logic: `blocked` (heavy futex/io-wait — waiting on the kernel
is the story) and `oversubscribed` (heavy scheduler run-delay with low blocking-wait — runnable but not
given the CPU, an affinity/placement problem, not a counter-chasing one); `blocked` wins when both fire,
matching the critical-path work's own stated priority.

The real, separate plumbing work this item was waiting on: this signal lived only in `--tree`'s own raw
text output before now, never ingested into the normalized store. `store.c`'s new
`scan_tree_blocking_stats()` scans that raw `--tree` file directly for its `futex`/`io_wait`/`schedstat`
lines (topdown.c's own fixed fprintf formats, confirmed against the emission sites) and sums them into
whole-run totals — deliberately *not* reusing or shelling out to `proctree.c`'s own parser, since this
needs only a run-wide sum, not per-pid/tree-shape reconstruction (proctree.c has no such run-wide total
today either, in raw text or its `--json` export, so a new pass was needed regardless of ingestion
approach). Promoted into two new run_features, normalized by `elapsed_seconds` the same way every other
`_pct` feature is: `blocking_wait_pct` = (futex + io-wait seconds) / elapsed × 100,
`sched_rundelay_pct` = run-delay seconds / elapsed × 100. Both `unavailable` (not fabricated as 0) for
the common case of a run that never used `--tree` at all. Verified via 10 new
`test_store.c`/`test_archetype.c` cases (scanner correctness, missing-file/no-matching-lines
degradation, feature promotion end to end, all four priority-ordering outcomes including "both blocked
and oversubscribed signals fire") and against a real 32-process oversubscription capture on this host
(`--tree --tree-io-wait --tree-schedstat` against 32 busy-spin children on a 32-core host, correctly
producing `sched_rundelay_pct≈32%`) piped through `wspy-store`.

**IBS-derived memory-path bottleneck decomposition (`wspy-archetype`'s new `memory_attribution_locus`
field) — closes 4.3 Tier 2's last remaining item.** `memory_attribution` (shipped, above) explicitly
declined to rank *which* cache level a memory-bound stall concentrates in, pointing at
`--topdown-backend`'s L1/L2/L3/DRAM stall-cycle chain (`print_topdown_be()`) as the right tool for that —
but that chain is Intel/ARM-only (`print_topdown_be()` returns immediately on `VENDOR_AMD`), and IBS is
also AMD-only, so this builds the AMD equivalent from a fundamentally different kind of data: IBS's
per-sample tags are request-outcome hits (did *this* sample miss L1D / cross to DRAM / cross a NUMA hop),
not stall-cycle attribution.

New `locus`/`locus_reasons` fields on `struct memory_attribution_result`, populated only when the axis's
existing classification already landed on `"corroborated"` *and* the newly-promoted `backend_memory_pct`
feature (the L2 memory-portion-of-backend split, a more precise anchor than the coarser `backend_pct` the
rest of the axis gates on) clears the same 20% floor `memory_attribution` itself uses. Two precision
tiers, `locus_reasons` always prefixed `tier=ibs-sample`/`tier=cache-counter` so one is never mistaken for
the other:
1. IBS tier (needs `ibs_dc_miss_pct`): `ibs_remote_node_pct` at/above 10% wins first (a cross-socket hop
   dominates regardless of where else the access resolved) → `"remote-numa"`; else `ibs_dram_pct` at/above
   10% → `"dram"`; else, only when `ibs_dram_pct` was itself measured, the residual
   `ibs_dc_miss_pct − ibs_dram_pct` at/above 10% → `"l2-l3"` (missed L1D, resolved before DRAM); else a
   plain `ibs_dc_miss_pct` at/above 10% → `"l1"`. `ibs_dc_l1tlb_miss_pct`/`ibs_dc_l2tlb_miss_pct` elevated
   appends a `tlb-cofire` tag rather than competing for the label — address translation is an independent
   hop from the data-miss chain. Falls through to tier 2 if IBS data exists but nothing in it clears its
   threshold, since `memory_attribution`'s own corroboration may have come from a signal (e.g.
   `smt_contention_pct`) with no hierarchy-position information at all.
2. Cache-counter tier (no IBS data this run): same shape over `l3_miss_pct`/`l2_miss_pct`/`dcache_miss_pct`
   at the same `CACHE_MISS_ELEVATED_PCT` cutoff `memory_attribution`'s own signal table already uses for
   these features (not a new threshold), `dtlb_generic_miss_pct` as the TLB co-firing tag.

`store.c`'s `SIMPLE_METRIC_FEATURES` table gained four new one-line rows (`ibs_dc_l1tlb_miss_pct`,
`ibs_dc_l2tlb_miss_pct`, `ibs_remote_node_pct`, `backend_memory_pct`); `FEATURE_SET_VERSION` bumped to
1.5. Deliberately out of scope for v1: fetch-side/icache decomposition (a different topdown category,
`frontend_bandwidth_pct`, with different semantics — conflating it here would repeat the exact
overclaiming-precision mistake `memory_attribution`'s own header warns against) and the raw
`op_data_src_count[13]` histogram / `IbsBrTarget`/`IbsDcLinAd`/`IbsDcPhysAd` (same scheme-ambiguity/
no-stable-bitfield reasons `ibs_sample.h` already excludes them from CSV for).

Verified via 11 new `test_archetype.c` cases (both precision tiers, the `tlb-cofire` tag, tier-2
fallthrough, the `"corroborated"`-only gate, the `backend_memory_pct` availability/floor gates) plus an
end-to-end `run_features` → CSV pipeline test, and 1 new `test_store.c` feature-promotion case, plus the
full `run_tests.sh` matrix (93 golden-output checks, 62 capability-matrix bundles including the NVIDIA
build rerun). Caught one real bug during testing: an unmeasured `ibs_dram_pct` was initially treated as
`0` when computing the L2/L3 residual, which silently misclassified a run as `"l2-l3"` when DRAM
residency was actually unmeasured rather than genuinely zero — fixed by only computing the residual when
`ibs_dram_pct` was itself actually collected, falling back to a plain `"l1"` read otherwise.

**CPU2026 workload-suite web tab (new `web/server.py`/`web/joblib.py` tab, alongside Run/Validate/
Store & Summary/Discovery/Phoronix) — closes 4.3 Tier 6's CPU2026 backlog item.** A SPEC CPU2026
counterpart to the Phoronix tab, structurally simpler: unlike a Phoronix test point, a CPU2026
benchmark and its config file both already exist locally the moment a suite is installed, so there's
nothing to fetch/materialize from a remote result — the tab's job is discovery, registration, and
action-triggering, not import. `CPU2026_BENCHMARKS` (`joblib.py`) is a static 52-benchmark catalog
(`706.stockfish_r` etc., from https://spec.org/cpu2026/docs/overview.html#benchmarks, grouped
intrate/intspeed/fprate/fpspeed) for display/grouping only — what's actually runnable is discovered live
via `discover_installed_cpu2026_benchmarks()`/`discover_cpu2026_configs()` scanning
`$SPECDIR/benchspec/CPU2026/*/` and `$SPECDIR/config/*.cfg` on disk, same "static table for labels,
filesystem for truth" split the Phoronix tab uses between `phoronix.tests.txt` and live
`phoronix-test-suite info` calls — a benchmark installed under a name the static table doesn't know
still shows up, just without suite/language labels.

Two-level inventory hierarchy (benchmark → config-tag, mirroring `workload/phoronix/<test>/<options>/`):
config tags (filename minus `.cfg`) are discovered independently of any benchmark since one SPEC config
can build any benchmark, then a benchmark×config *pairing* gets registered (not "materialized" — no
ledger, no XML) via `register_cpu2026_point()` under `workload/cpu2026/<bench>/<tag>/` (a `source.json`
provenance sidecar + one `README.md` per bench built from the static catalog's suite/language labels,
no subprocess/network round-trip needed the way Phoronix's `phoronix-test-suite info` does — idempotent,
an existing `source.json` is left untouched on re-register). `runs/<run_id>` symlinks back to real run
dirs via `link_cpu2026_point_run()`, same idiom as `link_phoronix_test_point_run()`.
`resolve_cpu2026_point_dir()` mirrors `resolve_phoronix_test_point_dir()`'s "resolve under dest_root,
require a real `source.json`" validation rather than trusting a client-supplied path. `$SPECDIR`
(default `/home/mev/cpu2026`, new `--cpu2026-dir` flag) is changeable from the tab's "Suite directory"
field via a plain GET re-render, no server restart — `cpu2026_suite_installed()` checking for
`$SPECDIR/shrc` is the install-sanity check, same role `check_phoronix_batch_config()` plays for
Phoronix. Inventory table's "Built" column (`cpu2026_benchmark_built()`) is a substring match against
`$SPECDIR/benchspec/CPU2026/<bench>/exe/*<tag>*` (SPEC's exe naming carries machine/OS suffixes like
`_base.<tag>-m64`, so it's not an exact reconstructed filename), recomputed live per inventory render
rather than cached at registration time, since a Build action changes that state afterward and
staleness here would be actively wrong rather than merely a re-check-later warning — same
cheap-check-now/re-verify-at-run-time posture `list_installed_phoronix_test_versions()` documents, but
live instead of cached given that difference.

Two actions per row: **Build** (`runcpu --config <cfg> --action=build --tune <tune> <bench>`, run as a
backgrounded subprocess via `execute_cpu2026_build()` and relayed live through a new `CPU2026_BUILDS`
SSE registry — a build isn't a wspy run at all, no report directory or manifest, so it gets its own
registry rather than reusing `RUNS`; `_stream_events()` gained a `make_report_url` parameter, always
`False` here, so this reuses the same SSE relay `execute_analyze()` uses without fabricating a bogus
report link) and **Use in Run tab** (`build_cpu2026_run_workload()` prefills the Run tab's `workload`
field, mirroring `_phoronix_use_in_run`). The one real wrinkle vs. Phoronix: `runcpu` needs SPEC's
`shrc` sourced for its environment, but neither `execute_cpu2026_build()`'s `Popen()` nor the Run tab's
`workload` field (`shlex.split()`, no shell) invoke a shell on their own, so
`build_cpu2026_shell_argv()` wraps the real command in `bash -c "source $SPECDIR/shrc && cd $SPECDIR &&
ulimit -s unlimited && runcpu ..."` — same `--action=validate --nobuild`/`ulimit -s unlimited` choices
`workload/cpu2017/run_test.sh` already makes for its own build-then-validate split. A new
`cpu2026_test_point` hidden Run-tab field (alongside the existing `phoronix_test_point` one) carries the
registry dir through `_link_cpu2026_test_point()` to a post-run symlink-back, same as
`_link_phoronix_test_point()` — deliberately *not* wired into `_enqueue_job()` yet (no
`_cpu2026_test_point_identity()` analog exists), left for a follow-up. No `wspy-ledger` integration
needed — 52 benchmarks don't need a "what's left" backlog ledger the way Phoronix's huge test catalog
does; the inventory table itself is the coverage view.

Verified via 30 new `test_joblib.py` cases across 10 new test classes (212/212 joblib tests passing:
install-sanity check, discovery, build-check substring matching, registration idempotency, path-
resolution rejection, inventory listing/grouping, run-symlinking, and argv-building for both the Build
and Use-in-Run-tab commands) plus a live smoke test against a fake SPEC install exercising discovery,
registration, inventory rendering, Use-in-Run-tab prefill, and the Build SSE stream's error handling.

**Tree viewer: cumulative time + hot-process table (`web/static/proctree_viewer.js`, PR #163) — makes
it easier to narrow to the processes where a run's user/system time actually went.** The single-tree
viewer's per-comm summary rollup (`proctree.c`'s `build_comm_table()`, already exposed as `data.summary`
in the JSON but previously only ever rendered by the diff view) is now shown as a clickable "hot
processes" table above the tree. Each node also displays cumulative (self + descendant) utime/stime and
its share of total run time, computed client-side; auto-expand now opens subtrees at/above a 5%
cumulative-time-share threshold instead of a flat depth<3 cutoff, and a new "hide branches under N%"
filter complements the existing text search. Child rendering order intentionally stays chronological
(fork order), not resorted by time. Entirely client-side — no `proctree.c`/wspy changes needed, since the
JSON already carried the required per-node/per-comm fields. Verified live via headless-Chromium
DevTools-Protocol automation against a real 403-process run.

**PID-targeted counter attachment (`--target=comm=<name>[,cmdline=<substr>]`, PRs #164–167) — closes
4.4's "PID-targeted counter attachment for known-hot processes" item.** Once a `--tree` run's hot-process
table shows which comm dominates a run's time, `--target` attaches a second, dedicated counter group
(the same counters the run's other flags requested) to just the matching process(es), rather than only
ever reading the whole-subtree aggregate. `setup_counters()` (`topdown.c`) gained a `pid_t attach_pid`
parameter (`-1` preserves every existing call site's behavior unchanged); a `>=0` caller skips any
`requires_system_wide` counter (RAPL/AMD L3/IBS — not meaningful process-scoped) and opens the rest with
`inherit=0` against that exact pid. `target_parse_spec()`/`target_match()` implement the comm=/cmdline=
grammar (comma-separated key=value, AND semantics when both given), mirroring `affinity.c`'s spec-parsing
style. Attachment happens live inside `ptrace_loop()`'s `PTRACE_EVENT_EXEC` handling (comm/cmdline are
already readable there, same timing `--tree-cmdline` relies on) — matched processes each get their own
counter group built from whatever `counter_mask` this specific wspy invocation has active; readings are
read back and emitted as `targetcounter <group> <label> <value>` tree-file lines at that pid's own exit,
then the fds are closed immediately rather than held until end-of-run (a long matching-heavy run could
otherwise exhaust PMU slots) (PR #164). `proctree.c` parses these into an open-ended `target_counters`
array — present on both the per-process node and the per-comm rollup (`PROCTREE_JSON_SCHEMA_VERSION`
bumped 1.0.0 → 1.1.0) — since the counter set depends on whichever flags the producing run had active,
not a fixed field list the way futex/io/schedstat's extras are.

Three follow-on web changes, each surfaced by actually using the feature end-to-end: (1) the Run tab's
Process-tree card gained a matching free-text `--target` field (PR #165), which in turn surfaced a real,
unrelated latent bug in the process-tree pass's own argv builder (`web/joblib.py`): the "software
counters too" checkbox had *never* actually turned software counters on — wspy's own default
`counter_mask` is `COUNTER_IPC` only (`wspy.c`), not IPC-plus-software as the old code assumed, so the
checked case emitted no flag at all while only the unchecked case emitted a no-op `--no-software`; a
plain `--tree` run through the web UI had been collecting zero performance counters regardless of that
checkbox. (2) That single checkbox was then replaced with the same full counter-group selector the
"Performance counters" card already uses (`render_group_checkboxes()`/`counter_group_flags()`), so
`--target`'s attachment isn't limited to software-or-nothing — which also fixed `ALL_GROUPS`' `"software"`
entry (wrongly marked `default_on=True`), a bug that had silently affected the *main* "Performance
counters" card's own software checkbox too, predating `--target` entirely (PR #166). (3) The web tree
viewer's per-comm hot-process table (previous entry) renders one column per distinct counter when
`target_counters` is present; per-process detail (already in the JSON per node) now also renders in the
per-node tree view itself, toggled the same way futex/io_wait/etc. already are, and a shared
`computeDisplayCounters()` helper collapses any group's `instructions`+`cpu-cycles` pair into a single
derived `IPC` ratio (dropping the raw counters, including AMD's `ex_ret_ops`, for that group) since
those numbers are only meaningful as the ratio — no other counter-pair gets this treatment (PR #167).

This item was originally planned as two backlog entries — PID-targeting (item 10) and a later
uprobe-based argument-capture item (item 11) meant to reuse its comm/PID-match plumbing — but the
argument-capture half hadn't landed yet at the time this doc entry was written; see 4.4 priorities below,
which now picks up numbering at 10 again for that remaining piece. PR #164 verified via `run_tests.sh`'s
full matrix (incl. two new `capability_matrix.sh` bundles: graceful attach, and the
`--target`-without-`--tree` fatal rejection); PRs #165/#166 verified via `web/test_joblib.py` (228/228,
6 new cases incl. a regression test for the `ALL_GROUPS` fix) plus a live web-launcher smoke test
(`/api/run-custom` → real `--target` run → `/api/tree-json` → confirmed both `ipc` and `software`
counters attach); PR #167 verified by mirroring the JS collapse logic in Python against a real run's
JSON (no JS runtime available in this environment to execute it directly), confirming both the
comm-summed and per-process cases collapse correctly while leaving other groups' raw counters intact.

**Symbol-level profiling (`--symbol-sample`/`--symbol-sample-event`, `wspy-symbolize`, the tree
viewer's "Profile" drill-down, 2026-07-29/30) — closes 4.4's "Symbol-level profiling" item.** Maps
sampled events to routine/symbol names, scoped to `--target`-matched processes rather than a whole-
system `perf record`-style capture (already covered by `perf` itself; see "Symbol-level profiling
deep-dive" for the full use-case discussion). Capture: a generic `PERF_SAMPLE_IP` sampling counter,
generalized from `ibs_sample.c`'s mmap ring-buffer plumbing (now factored out into shared
`perf_ring.c`/`perf_ring.h`, with `ibs_sample.c` refactored onto it unchanged in behavior), attached
at the same `PTRACE_EVENT_EXEC` point `--target`'s counter attachment already uses; drains into
`targetsample`/`targetsamplelost`/`targetmap` tree-file lines at that pid's own `PTRACE_EVENT_EXIT`,
which `proctree.c` parses into `target_samples`/`target_samples_lost`/`target_maps` on each `tree`
JSON node (`PROCTREE_JSON_SCHEMA_VERSION` 1.1.0 → 1.2.0) — deliberately *not* rolled into the
per-`comm` `summary` totals the way `target_counters` is, since raw addresses carry per-process
ASLR/PIE load bias and summing them across pids would be actively wrong, not just imprecise.
Resolution: a new stdlib-only Python tool, `wspy-symbolize` (`wspy-analyze`/`wspy-bundle`'s own
category, not linked into wspy itself), resolves `target_samples` against `target_maps` (PIE bias:
`addr - map.start + map.file_offset`) via batched `addr2line` calls (one per binary/library, not one
per address), with five distinct non-crashing degradation reasons rather than one generic
"unresolved" bucket — see `doc/ARTIFACT_CONTRACT.md`'s "Symbol table" section for the full schema.
Web UI: `/api/symbolize/<suite>/<benchmark>/<run_id>?pid=<n>|comm=<name>` (mirrors `/api/tree-json`'s
on-demand, nothing-written-to-disk shape) plus a "▶ profile" toggle in the tree viewer on any node
carrying `target_maps`, caching its fetched result/expanded state on the node object itself so it
survives a rerender from an unrelated column toggle. Verified live end-to-end against real hardware
in this environment throughout (capture: real `--target --symbol-sample` runs against `sleep`/`yes`
workloads, including hand-checking the PIE offset arithmetic against a real `libc.so.6` map;
resolution: `wspy-symbolize` correctly dominated by `libc`'s `write`/`syscall_cancel` internals for a
`yes` busy-loop, correctly flagging this host's own stripped Rust-`coreutils` `yes` binary as
unresolved; web UI: the real, unmodified `proctree_viewer.js` executed under `gjs`
(GNOME JavaScript/SpiderMonkey) with a hand-written DOM+`fetch` shim fed real captured JSON, since no
browser was available this session — see "Symbol-level profiling deep-dive" for the full verification
narrative of each piece). This item was originally planned as two backlog entries — the capture/
resolution work itself (item 9) and a later uprobe-based argument-capture item (item 10) meant to
reuse its resolution output — but the argument-capture half hasn't landed yet; see 4.4 priorities
below, which now picks up numbering at 9 again for that remaining piece.

**Tree viewer oversized-JSON handling (`web/server.py`/`proctree_viewer.js`, 2026-07-30, PR #170) —
bugfix, found via real use rather than backlog.** A `build-linux-kernel-defconfig` run forking ~99,570
processes (a `-j`-parallel kernel build) made `_api_tree_json`'s on-demand `proctree --json` shell-out
produce a 541MB JSON blob; the browser's `fetch`/`JSON.parse` couldn't reliably complete on a response
that large, surfacing as a confusing client-side `Unexpected end of JSON input` instead of anything
actionable. Fix landed in three increments on the same branch: (1) a `TREE_VIEWER_MAX_BYTES` guard,
initially 20MB, that rejects an oversized response with a clear `413` instead of attempting to ship it;
(2) raised to 500MB once a real, smaller kernel-build run (219MB, `--tree-cmdline` only, no other
per-tick flags — same ~99.5k process count, just fewer bytes/process) needed the headroom; (3) instead
of just rejecting, `_api_tree_json` now keeps `process_count`/`summary` (comm-grouped, small regardless
of process count) and only drops the `"tree"` key, flagging `tree_omitted`/`tree_omitted_bytes` so
`proctree_viewer.js` still renders the process-count line and summary table but skips the interactive
tree, replacing it with a message pointing at `process.tree.summary.txt` (linked via the existing
`/files/` route when present) — same degrade-gracefully idiom other parts of this codebase already use
rather than hard-failing. Increment (3) also fixed a bug in the guard itself: it compared `proctree`'s
raw stdout length against the threshold, but the actual response is that data re-serialized through
`json.dumps()` afterward (which re-escapes `cmdline` strings and can inflate size several percent) —
enough that the 541MB run's 502.8MB-raw output slipped under the newly-raised 500MB limit undetected
until the check was moved to measure the real serialized size. Note this only addresses the fetch/parse
failure, not a separate, still-open render-cost concern: `proctree_viewer.js`'s `renderNode()` builds one
DOM element per process up front regardless of payload size, so a tree just under the byte limit but
still tens of thousands of processes could be slow to render even once it loads (see that file's own
header comment on deferring child-DOM construction until expand, not done here). Verified live: confirmed
the failure mode by curling `/api/tree-json` directly against the real 541MB run (valid-but-oversized
JSON, matching the reported truncation symptom); re-verified after each increment against that run plus
a smaller in-limit kernel-build run and a normal small run (`coremark-default`) to confirm non-oversized
trees stay unaffected; user confirmed the tree viewer renders correctly end-to-end in a real browser
post-fix.

**`wspy-analyze`: fixed a "thinking" model silently producing an empty `aianalysis.<model>.txt`
(`ollama_generate()`/`analyze_one_model()`).** Root-caused live (2026-08-01) against the real first
published `/workload` report (WordPress page id=35, `phoronix/coremark-default`): `gpt-oss:20b` drafts
its answer inside Ollama's separate `"thinking"` stream field before ever emitting `"response"` text,
and Ollama was running it with only a 4096-token context window (confirmed against the loaded model's
own `llama-server` process args) — for this codebase's multi-KB prompt, the model exhausted that budget
still mid-thought (`done_reason: "length"`), so `response` stayed permanently empty with no exception
anywhere. Replayed the exact real prompt directly against `/api/generate` to confirm both the failure
and the fix: request now sends `options.num_ctx` (new `--num-ctx`, default 16384) — confirmed this alone
turns `done_reason` from `"length"` into a normal `"stop"` with real output. Defense in depth: if
generation is still cut short before any visible response text exists, `analyze_one_model()` now skips
writing the file and warns instead of silently succeeding empty. `test_ai_analyze.sh` still passes;
re-ran the real failing case through the actual CLI post-fix (real 1.3KB narrative, not 0 bytes).

**New pre-computed report artifacts + a "default curation" button** — a bundle of three small, related
additions from iterating on that same first published report, filling gaps found while trying to reuse
its curation as a starting template for future reports:
- `wspy-run` now writes `command.txt` alongside `manifest.json` — the same workload argv
  `manifest.json`'s own `"command"` array already records, as one copy-pasteable shell-quoted line
  (`printf '%q '`) rather than a JSON array a reader has to mentally reassemble; shows up in the
  curation studio's "+ add" list as "command line".
- `web/joblib.py`'s `run_proctree_besteffort()` gains two more best-effort derived views alongside the
  existing `process.tree.summary.txt`/`process.tree.simple.txt`: `process.tree.top.txt` (individual
  processes — not proctree's own per-comm rollup — ranked by actual CPU time, `utime_seconds+
  stime_seconds`) and `process.tree.top1pct.txt` (the full tree pruned to just those top processes plus
  their ancestor chain, every other subtree collapsed to a "N more process(es) omitted" line at the
  level it was hidden). "Top 1%" is `top_fraction=0.01` clamped to `[5, 50]` processes
  (`PROCESS_TREE_TOP_FRACTION`/`_MIN`/`_MAX`) so it stays readable on both a tiny run and the
  ~155K-process stress run `doc/ARTIFACT_CONTRACT.md` mentions — a starting default the author expects
  to tune, not a fixed policy. The pruning walk is a single bottom-up "does this subtree contain a kept
  pid" pass before printing, not repeated per-node subtree scans, to stay O(process_count) rather than
  O(process_count²) on that same large-tree case. Verified against a real 640-process tree (from a real
  `ollama-subset` run): the 727-line raw tree collapses to a 15-line pruned view.
- The curation studio (`render_studio()`) gained an "Apply default curation" button
  (`build_default_curation_blocks()`, `apply_studio_post()`'s new `default-curation` op) — one click
  replaces the current curation with a fixed starting set: every pass's own raw text/CSV output,
  `command.txt`, every AI narrative analysis (not prompt/critique files), every plot, all at
  `depth="full"`. A policy over `collect_run_files()`'s *kinds* (this run's own recorded passes, the
  `ai_generated` flag, `plots/` membership) rather than hardcoded filenames, since different `wspy-run`
  profiles name their passes differently — deliberately *not* a literal reproduction of page 35's exact
  hand-picked 3-plot/2-text-block selection (which would need per-profile filename knowledge this
  function doesn't have), a known, accepted gap given the author's own framing that this default will
  need tuning over time. Always replaces, never merges — confirmed working end to end through the real
  running server (`curl`-submitted the real form POST against the real `coremark-default` run:
  correctly produced all 5 pass outputs + `aianalysis.gpt-oss_20b.txt` + all 14 plots).

New tests: `web/test_studio_curation.py` (`build_default_curation_blocks()`), `web/test_joblib.py`
(`render_top_processes_text()`/`render_top1pct_tree_text()`, `classify_bundle_kind()`/
`collect_run_files()` for the two new filenames). Not done in this pass: re-publishing WordPress page
35 itself with the fixed analysis/new curation (held off deliberately — its curation will change again
once these land, no point publishing twice), and the page's sidebar/full-width layout question (a
WordPress theme/Customizer setting outside what this codebase's publishing code controls, left for the
author to check in wp-admin directly).

**Default curation moved into an editable config file** — the "default we'll want to change over
time" gap flagged above got hit for real on a `zen4plus-deep` run: reviewing the button's output
against a real 20-artifact run surfaced that only ~10 of them were actually wanted, in a different
order than the kind-based policy produced. `build_default_curation_blocks()` now reads
`web/default_curation.conf` (`load_default_curation_config()`) — an ordered, plain-text list of
filenames/glob patterns, one per line, `#`-prefixed for a documented exclusion (kept in the file
rather than deleted, so re-enabling something is a one-character edit) and `##`-prefixed for section
headers/prose. Config-listed artifacts are added in file order; everything the config doesn't mention
(a pass output from a profile it hasn't been tuned for, a plot template it doesn't list, additional AI
narrative variants) still falls through to the pre-config kind-based logic, appended after — nothing
disappears just because the config is incomplete for a given run. The shipped config also folds in two
artifact kinds the studio could already produce but the original policy never offered:
`process.tree.top1pct.txt`/`process.tree.top.txt` (the tree-heavy pass's derived process-list views)
and `aianalysis.*.txt` (any model's narrative, via glob — silently skipped if not generated yet, not
an error). New tests: `web/test_studio_curation.py`'s `LoadDefaultCurationConfigTest` (parser) plus
three new `BuildDefaultCurationBlocksTest` cases (config ordering/exclusion, exclusion not
reintroduced by the fallback loop, unlisted artifacts still auto-appended).

**Composite-preset process-tree auto-generation bug + manual retrigger button** — found while chasing
why a real `zen4plus-deep` run's process-tree views (above) never showed up:
`execute_profile_run()` (`web/joblib.py`) only auto-ran `run_proctree_besteffort()` when the *raw*
profile string was literally `"tree-heavy"`/`"gpu-compute"`, but the web launcher only ever submits a
composite preset's own name (`"zen4plus-deep"`), never its expanded `"deep-cpu,ibs-sample,tree-heavy"`
pass list — `server.py` already had `COMPOSITE_PRESET_PROFILES`/`_expand_preset_names()` for exactly
this (its own IBS/power probe tables needed it), `joblib.py`'s post-processing step just wasn't using
it. Both moved into `joblib.py` (renamed `expand_preset_names()`, still imported back into `server.py`)
and `execute_profile_run()` now expands before checking. Also added a "Generate process-tree views"
button on the report page (`render_proctree_card()`, wherever `process.tree.txt` exists) — synchronous
`POST /api/proctree-views/<suite>/<benchmark>/<run_id>` (`_proctree_views()`), not SSE, since this is
local proctree/Python rendering, not an LLM call — so a run from before this fix (or before the button
existed at all) can get its views generated retroactively without re-running the benchmark. Requests
every proctree annotation flag unconditionally; proctree treats an unrequested-at-collection-time flag
as "absent", not an error, so the maximal request is safe without knowing which `--tree-*` flags the
original run used. Verified end-to-end against a real `process.tree.txt` (root-less `wspy --tree`)
through the live HTTP server, both the button and the 400/404 guard paths. New test:
`web/test_joblib.py`'s `ExpandPresetNamesTest`.

**wspy-analyze output rendered as real Markdown, not dumped verbatim** — the report page, HTML export,
and WordPress export all just `html.escape()`'d wspy-analyze's `.txt` output into a `<pre>` block, so a
model's `**bold**`/table syntax showed up as literal asterisks and pipes (surfaced publishing a real
report: gpt-oss:20b's narrative is markdown-flavored by default, primed by the prompt templates' own
markdown headers). Fix, in three parts:
- wspy-analyze now writes `.md` (`aianalysis.<model>.md`, `aiprompt.md`, and their `.critique`/
  `.compare.` variants) instead of `.txt` — content was always markdown, the extension now says so.
  `joblib.py`'s `AIANALYSIS_RE`/`AIPROMPT_CRITIQUE_RE`/`ai_artifact_label()` match either extension
  (no regression for an existing run's `.txt` narrative — it keeps being recognized/labeled/curated
  exactly as before, just rendered as plain text rather than parsed markdown, same as any other `.txt`
  block); `guess_kind()` maps `.md` to a new `"markdown"` kind. `default_curation.conf` lists both
  extensions for the AI-narrative slot.
- New `web/markdown_lite.py`: a small stdlib-only Markdown → HTML/WordPress-block converter (this repo
  has no non-stdlib Python dependency to reach for). Deliberately not full CommonMark — ATX headings,
  paragraphs, bold/italic/inline-code spans, single-level lists, GFM pipe tables, fenced code, hr; no
  nested lists/blockquotes/links, since none of those show up in this tool's actual prompt/response
  shape. `to_html()` for the report page/HTML export; `to_wp_blocks()` decomposes into native Gutenberg
  blocks (`wp:heading`/`wp:paragraph`/`wp:list`/`wp:table`/`wp:code`) rather than one opaque raw-HTML
  blob, matching how every other content type in `render_export_wordpress()` already works — stays
  editable in the WP block editor afterward. 26 new tests (`web/test_markdown_lite.py`).
- `prompts/perf_analysis.tmpl`/`perf_analysis2.tmpl` (versions bumped to 1.2/2.1) now explicitly ask
  for a Markdown response, including real `|`-delimited tables over ASCII-art ones. `perf_analysis2.tmpl`
  (the structured, numbered-section template) is now wspy-analyze's default (`DEFAULT_TEMPLATE`) instead
  of the short-prose `perf_analysis.tmpl` — the web UI's template `<select>` reordered to match. Verified
  against a real live Ollama call (`test_ai_analyze.sh`, `qwen2.5-coder:3b`): real `.md` output, template
  v2.1 by default. Not done: a one-time rename/backfill tool for pre-existing `.txt` narrative files —
  they keep working, just without the new rendering, per the author's own call when scoping this.

**wspy-analyze: AMD IBS counting-mode CSV data now reaches the AI narrative prompt** (PR #182) — a new
streaming per-column summarizer (`summarize_csv()`) feeds `ibs.csv` min/max/mean/stddev into the prompt,
closing a gap where IBS counting-mode data was invisible to the narrative. See
`doc/INVESTIGATION_ARCHIVE.md` for the full write-up.

**`wspy-testpoint`: full run-selection/aggregation/rendering pipeline for a test point** (PRs #183-186)
— `select-runs` assigns each linked run a role (stats-pool/supplementary/excluded/primary) so a redo or
a differently-scoped run never pollutes statistics; `aggregate`/`render` compute and curate a
`README.md` from the resolved run set, including a cross-run archetype-stability signal. Closed out
(now-former) Tier 3 item 5. See `doc/INVESTIGATION_ARCHIVE.md` for the full design and PR-by-PR
write-up.

**Web UI "Publish test-point report" button** (PR #187) — wires the `wspy-testpoint` pipeline above
into the report page: a card runs `select-runs`/`render` in a background thread with live SSE output,
same shape as the existing AI-narrative-analysis card. Closes (now-former) Tier 3 item 6's
write-path/trigger scope (the WordPress page-hierarchy bug is a separate, still-open item).

**WordPress REST publishing primitives** (`web/wp_client.py`, `wspy-publish`) — authenticated page
create/update/draft/publish, media upload, Gutenberg-block content reuse from the existing exporter, and
a per-report "Publish to WordPress" button in the web UI, all verified against the real
`mvermeulen.org/workload` site. See `doc/INVESTIGATION_ARCHIVE.md` for the full 8-step build history.
Does not include the site-wide automated pipeline (walking the whole store to publish/update suite- and
cross-suite-level pages) Tier 3 item 2 originally asked for — see that item for what's still open.

**cpu2026 level-3 benchmark pages + `wp_client.publish_page_at_path()`** (PR #188) — materializes
`doc/REPORT_HIERARCHY.md`'s level-3 pages for all 52 real SPEC CPU2026 benchmarks, on both the
file-system report-root and the live site (`scripts/publish_cpu2026_benchmarks.py`), verified end to end
and published live. New `publish_page_at_path()` is the hierarchy-aware create-with-content primitive
that was missing (walks/auto-creates parent stubs, then sets content on the leaf) — closed the
now-former Tier 3 item 6's underlying gap in spirit; the web UI "Publish to WordPress" button has since
switched onto it too (see below), fully closing that item.

**Phoronix level-3/4 hierarchy pages** (PR #189) — `scripts/publish_phoronix_pages.py`, second use of
the cpu2026 recipe, confirming `publish_page_at_path()` generalizes: level-3 (97 materialized tests,
content reused from `write_phoronix_test_readme()`'s existing output — closing `doc/REPORT_HIERARCHY.md`'s
own flagged migration debt) and level-4 (test-points that actually have a linked run, 2 of 443
materialized — the rest are never-benchmarked option-combination noise), all published live, no changes
needed to `wp_client.py`/`report_root.py`/`wspy-publish` themselves.

**Web UI "Publish to WordPress" button now nests correctly** (PR #190) — fixes the now-former Tier 3
item 6: the button's form previously always published at WordPress root (`parent_id` defaulted to `0`,
had to be typed in by hand). Replaced with a required `machine` field; the full
`suite/test/test-point/machine/run-id` path resolves automatically via `joblib.resolve_test_identity()`
(moved from `wspy-testpoint`, now shared) and publishes via `publish_page_at_path()`, nesting each run's
page as a child of its auto-created machine stub. Verified against the real site — confirmed the created
page's parent chain resolves correctly leaf-to-root via the WP REST API. `publish-page --slug`'s flat
CLI path stays a deliberately simple primitive, unchanged.

**Machine catalog pages** (PR #191) — `scripts/publish_machine_page.py` resolves
`doc/REPORT_HIERARCHY.md`'s two long-open machine-level questions: a `/machine/` index plus one
`/machine/<short-name>/` detail page per physical machine (run locally, since hardware detection can't
describe a machine it isn't running on), named `<vendor>-<short-model>-<ram-gib>gb` to disambiguate
machines sharing a chip but differing in memory. Auto-created machine stubs inside the suite hierarchy
now link back to their catalog entry (`publish_page_at_path()`'s new `stub_content` parameter). A new
`machine_short_name` field in `~/.config/wspy/publish.json` (`save_config()` now shared, moved out of
`wspy-publish`) lets both the CLI and the web UI's publish form remember a machine once registered,
instead of retyping it every time. Verified live against the real site.

**Phoronix test-point stub auto-populated at publish time** (PR #192) — publishing a run already gave
its auto-created machine-level page a real catalog link (PR #191); now the test-point-level page above
it (e.g. `/phoronix/openssl/sha256/`) also gets real content (`test_id`/`arguments`) automatically,
instead of staying empty until a separate manual `scripts/publish_phoronix_pages.py` re-run. New
`joblib.find_materialized_phoronix_test_point()`/`test_point_wp_content()` (the latter moved out of the
script, now shared). Phoronix only for now — verified live against the real site.

**cpu2026 identity resolution fixed** (PR #193) — `resolve_test_identity()` never special-cased cpu2026;
publishing a cpu2026 run would have created a wrongly-named sibling page instead of nesting under the
real `cpu2026/<bench>/` page. New `joblib.find_materialized_cpu2026_point()` (mirrors the Phoronix one)
splits identity into `(bench, "tag-tune")` correctly, and `cpu2026_test_point_wp_content()` gives the
auto-created test-point stub real config content too, same parity as PR #192. Caught by the user before
trying a new SPEC benchmark; not yet verified against the real site (no such run exists yet), covered by
unit tests only for now.

**`wspy-testpoint aggregate`/`render` resolve a run's actual per-pass store run_ids** (PR #194) —
`select-runs`/`runs.json` name a run by its `wspy-run` directory, but `wspy-store`'s `runs` table keys
each row by the underlying `wspy` invocation's own run-index `run_id`, generated independently per
collection pass and never equal to the directory name; `aggregate`/`render` were passing the directory
name straight through as `--run-id`, so every multi-pass run (the norm for anything collected via
`wspy-run --suite/--benchmark`, cpu2026 or Phoronix alike) reported "not found in store" even after
correct ingestion. New `resolve_store_pass_rows()` correlates a run directory to its real store rows by
matching `output_path`/`manifest_path` under `<suite>/<benchmark>/<run_id>/`, falling back to an exact
`(hostname, run_id)` match for a bare wspy run dropped into the layout by hand (what the existing smoke-
test fixtures already modeled); `aggregate`/`render` now expand each stats-pool run into all of its
resolved pass ids, and the archetype cross-run section picks one representative pass (the `"counters"`
configuration when present). Caught by the user trying to publish a real 707.ntest_r cpu2026 test-point
report through the pipeline PRs #183-186 built; new regression coverage in `tests/testpoint_smoke.sh`
reproduces the exact pre-fix failure. Verified live retrying that same publish attempt — which then
surfaced a second, separate identity-resolution bug this fix didn't touch (PR #196 below).

**"Publish test-point report" pre-fills Machine slug from config** (PR #195) — the card's Machine field
was always blank, even though `wp_client.load_config()`'s `machine_short_name` (set once via
`wspy-publish configure`) already backs the sibling single-run "Publish to WordPress" panel's own
Machine field. Same read, same still-editable/required text input — just removes retyping a slug this
web layer already had on hand. Verified live: a real report page now renders the field pre-filled from
the real config.

**`wspy-testpoint` threads `--cpu2026-dest-root` through to identity resolution** (PR #196) —
`resolve_test_point_paths()` called `joblib.resolve_test_identity()` with only `--phoronix-dest-root`,
never a cpu2026 one; PR #193 fixed this exact call site in `web/server.py`'s single-run publish path,
but `wspy-testpoint`'s own CLI (what the "Publish test-point report" button shells out to) never got the
same follow-up. Every cpu2026 benchmark identity therefore hit `resolve_test_identity()`'s generic
fallback (`test=<full identity>`, `test_point="default"`), landing as a wrongly-named sibling directory
instead of nesting under its real `<bench>/` page — caught live publishing a real 707.ntest_r test-point
report. New `--cpu2026-dest-root` flag (default matches `web/server.py`'s own `CPU2026_DEST_ROOT`),
threaded through the same way `--phoronix-dest-root` already was; new regression coverage in
`tests/testpoint_smoke.sh`. Verified live: hierarchy confirmed correct on retry; the wrongly-nested tree
the pre-fix code had already committed into the real report-root was manually cleaned up (git-committed
removal, local-only, same "never push automatically" convention as every other `wspy-testpoint` commit).

**WordPress idempotent content-merge protection** (PR #197) — closes Tier 3 item 2's "idempotent content
merge" gap (see the corrected item 2 bullet list above). `publish_page_content()` always did an
unconditional `update_page()` on an existing page, with no way to tell "nothing's changed since our last
write" from "a human hand-edited this in wp-admin since" — any repeat publish would silently clobber a
hand-edit. New fingerprint tracking (`~/.config/wspy/publish_state.json`, page id → sha256 of the
content wspy itself last confirmed live there): before overwriting, refetches the page's current raw
content and compares against the recorded fingerprint, raising `WPContentDriftError` on a mismatch
instead of overwriting (a page with no fingerprint on record yet — predating this feature — is trusted,
not retroactively blocked). `force=True` (`wspy-publish publish-path --force`, or the web UI's new
"Overwrite" checkbox) bypasses the check. Every successful write re-fetches the page's own post-write
live content (not the caller's raw input) to record the new fingerprint, so WordPress's own block-markup
normalization-on-save can't cause a false-positive drift on the very next publish. 5 new unit tests
(`web/test_wp_client.py`); not yet verified against the real site (no hand-edited page to test drift
against yet).

**Characterization badges in the curation studio** (PR #198) — closes the badges half of Tier 3 item 3
(similarity panels remain open, see item 3's own corrected text above). A "Generate characterization
badge" panel on the studio page shells `wspy-archetype --run <hostname>:<run_id>` and writes a compact
resource_dominance/confidence/etc. markdown snippet (`archetype_badge.md`) into the run directory —
deliberately not a new curation-studio block "kind" with its own rendering path in every export format;
once written it's an ordinary artifact, already offered by `collect_run_files()` and already rendered
through the existing markdown pipeline via `guess_kind()`'s `.md` rule, the same "external tool output
becomes a curatable file" precedent `wspy-analyze`'s `aianalysis.<model>.md` established. Resolving which
store row to classify hit the same directory-name-vs-store-run_id mismatch `wspy-testpoint`'s own
`aggregate`/`render` had to solve — caught live against the real store before shipping (an early draft
used the run directory's own name and silently resolved the wrong pass every time). `resolve_store_pass_rows()`/
`escape_like()`/`pick_counters_pass_id()` moved out of `wspy-testpoint` into `web/joblib.py` so both
callers share one implementation instead of two independently-drifting copies. Verified live end to end
through the actual running server (generate → file appears in "Add a block" → panel switches to
"Regenerate"); 16 new unit tests across `web/test_archetype_badge.py`/`web/test_joblib.py`.

## Known gaps (still open)
Real-hardware/real-scale validation this project's hand-testing hasn't covered yet. Not release
blockers — just don't assume these are confirmed:
- **AMD IBS `fetchlat` threshold minimum unverified:** unlike `ibs_op`'s `ldlat` field (see above,
  now fixed), no host available during that validation exposed a working `fetchlat` sysfs format
  field on `ibs_fetch` to test whether it has an analogous hardware-enforced minimum below
  `IBS_DEFAULT_FETCHLAT_THRESHOLD`'s current value of 120. Worth the same bit-sweep treatment once
  hardware with a live `fetchlat` field is available.
- **Sub-~10ms target processes can read back all-zero/`-nan` counters — a real perf-subsystem timing
  limitation, not a wspy bug, no fix planned.** Root-caused 2026-07-22 on carlsbad (real Intel hybrid
  hardware) via `strace` on a failing `--ipc -- true` run: the raw `read()` bytes showed
  `value=0, time_enabled≈10.8ms, time_running=0` — the counter was armed the whole window but the
  kernel never scheduled it onto a real PMU register during the child's brief life, so both the
  parent's own count and (per `perf_event_exit_task`'s inherited-count rollup) the child's are
  genuinely zero, not misread. Confirmed generic across counter families — plain `--ipc` (ordinary
  `PERF_TYPE_HARDWARE`, no Perf Metrics involved) reproduces it at a similar rate to `--topdown2`,
  contradicting this item's original working theory that it was Perf-Metrics/fixed-counter-specific.
  Essentially absent on realistic workloads: 0 failures across ~130 combined runs of a 0.3-0.4s
  CPU-bound workload (both `--ipc` and `--topdown2`), vs. ~15-30% of runs against `true` (~3ms). A
  standalone probe (open on self + `inherit=1` before fork, matching `setup_counters()`/`launch_child()`
  exactly, `/tmp/claude-*/inherit_probe.c`, not part of the tree) reproduced the same failure mode at
  lower but nonzero rates and showed it scales with how many counters share a perf group (single event:
  ~0.7-1.7%; a 2-member group: ~5.3%) — consistent with a larger/more-complex group simply taking the
  kernel measurably longer to finish installing onto real hardware after the task starts, which a
  short-enough-lived child can outrun entirely. Also ruled out one likely-sounding "fix": switching from
  wspy's self+`inherit=1`-before-fork model to directly targeting the child's pid (the `perf stat`-style
  approach, opening after a `PTRACE_TRACEME` exec-trap stop) made failures dramatically *worse* (81% vs.
  <2% in the same probe) — the extra ptrace stop/`PTRACE_CONT` round-trip itself eats into the
  already-tiny time budget. No wspy-side action item: don't chase this further without new evidence.

## Track deep-dives
Reasoning that doesn't fit a single backlog line, for tracks with genuinely open work. Deep-dives for
work that's since fully shipped (blocking I/O, schedstat, vmsize, connect/wait/poll/nanosleep, power,
the LLM narrative-analysis design, the full critical-path-instrumentation candidate rationale,
repeatability policy/confidence metadata, comparison matrix mode, and symbol-level profiling) have moved
to `doc/INVESTIGATION_ARCHIVE.md`. The Zen5/IBS and Topdown deep-dives below have also each been trimmed
to just their one remaining open thread — the confirmed-platform-behavior background that fed their
now-shipped items moved to the archive too.

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
  capability-probing rows. Both are candidate inputs for a future "platform formula registry" (see the
  Topdown deep-dive's own remaining item, and "Open questions for prioritization") once Zen5-specific
  formulas are actually versioned there — no standalone backlog item yet.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Now unblocks 4.3's "IBS-derived memory-path bottleneck decomposition" (shipped, see "Shipped since
4.2"). `--interval`-integrated periodic IBS sampling rates (needs a real poll-loop architectural change)
was deliberately deferred out of that item's scope — see 4.4 priorities.

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
event_source/devices/` enumerated live, not from documentation alone):
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
  natural Intel counterpart to AMD IBS sampling-mode support (shipped, see "Shipped since 4.2"),
  comparable in spirit to IBS's `IbsOpData`/`DcMiss`/`NbIbsReqSrc` tag bits. Not investigated in depth
  this pass; worth scoping now that IBS sampling-mode's mmap-ring-buffer/per-sample decode
  infrastructure exists to model this against.

### Topdown deep-dive
Everything this originally tracked has shipped — multiplex-aware confidence/decomposition sanity checks
("What shipped in 4.0"), the hierarchical L1→L2→L3 schema ("What shipped in 4.2"'s "Hierarchical topdown
schema"), and phase-aware topdown/cross-signal attribution/hybrid core-class summaries ("Shipped since
4.2") — except one item:

- Platform formula registry — versioned event/formula mapping per CPU family/model, for auditability.
  See "Open questions for prioritization" and the Zen5/IBS deep-dive above for concrete candidate
  inputs once this exists.

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

**Tier 1 — needs 4.1's normalized store/history:**

Fully shipped for 4.3: nearest-neighbor search and K-means clustering + cluster profile cards, both
`wspy-archetype` modes (`--nearest`, `--kmeans`) — see "Shipped since 4.2" for both write-ups. Tier
number kept stable rather than renumbering Tier 2-7 below, since several other entries in this document
cross-reference them by tier number.

**Tier 2 — topdown/attribution, needs 4.2's hierarchical schema + phase detection (both shipped) +
AMD IBS sampling-mode support (shipped, see "Shipped since 4.2" and the Zen5/IBS deep-dive):**

Fully shipped for 4.3: phase-aware topdown, composite attribution (including its blocking-syscall-split
modifier), core-class-aware topdown, and IBS-derived memory-path bottleneck decomposition — see "Shipped
since 4.2" for all four write-ups. Tier number kept stable rather than renumbering Tier 3-7 below, same
reasoning as Tier 1 above.

**Tier 3 — publishing/reporting expansion, needs 4.1's report studio:**

2. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates), targeting
   a new WordPress site at `mvermeulen.org/workload` (parallel to, not replacing, the author's existing
   hand-curated `mvermeulen.org/perf/workloads/`). **REST auth/page/media primitives, a CLI
   (`wspy-publish`), a per-report "Publish to WordPress" web UI button, and idempotent content-merge
   protection are all shipped** — see "Shipped since 4.2" and `doc/INVESTIGATION_ARCHIVE.md` for the
   full build history (the latter also carries a correction on `--slug`'s flat lookup, previously
   miscounted here as a second open gap it never actually was). One thing this item originally asked
   for is still open:
   - **The actual site-wide pipeline.** Nothing yet walks the whole `wspy-store` and generates/updates
     suite-level (a ~30-column reference-matrix table, matching `/perf/workloads/<suite>/`'s existing
     hand-maintained shape) or cross-suite rollup pages — today's tooling only publishes individual
     benchmark/test-point pages, one human click at a time. Depends on item 5's reference-matrix
     database as the suite-level data source (deciding whether to generate that table from the store
     instead of hand-maintaining it is exactly item 5's own open question).
3. Similarity panels in reports (`wspy-archetype --nearest`-based nearest-neighbor comparison) — the
   deferred half of "characterization badges + similarity panels"; the badges half shipped as an
   ordinary curatable artifact rather than a new block type (see "Shipped since 4.2").
4. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1's
   static inclusion-depth mechanism (none/summary/excerpt/full) for the tree/interval blocks
   specifically; that mechanism stays the right default for a published, non-interactive report even
   once this exists.
5. Benchmark reference-matrix database keyed by (test name, test version, test point) × (machine,
   bucketed to a coarse architecture class: AMD/Intel/ARM/SoC) — a wide, curated comparison table in
   the spirit of the author's existing external reference page
   (https://mvermeulen.org/perf/workloads/, 60+ columns per test), but generated from wspy's own
   normalized store (4.1's `store.c`) and extended with metrics that page doesn't carry (notably the AMD
   IBS-derived fields shipped in 4.2/4.3). Distinct from `store.c`'s per-run long/tall `metric_values`
   table and from the now-shipped `wspy-testpoint` per-test-point narrative README pipeline (see
   "Shipped since 4.2"): this is a queryable, pivoted-wide table meant for side-by-side comparison
   across tests and machines, not one run's or one test point's own story. `doc/REPORT_HIERARCHY.md`
   (established this cycle) already earmarks
   `<report-root>/` (the hierarchy's own top level) as this database's natural home, alongside the
   rendered reports it would feed/be fed by. Real design work needed before scoping further:
   - **Column vocabulary.** Audit what the reference page's 60+ columns actually measure against what
     wspy already captures/`extract_run_features()` already derives, to find the real gap (expected to
     be mostly-covered plus new IBS columns, but unverified until audited).
   - **Machine identity granularity.** The requested key is deliberately coarse (AMD/Intel/ARM/SoC)
     rather than full provenance (4.0's per-run environment capture) — needs a defined bucketing rule
     from the already-captured vendor/family/model fields, not a new capture mechanism.
   - **Web integration.** Once the schema exists, wire it into the web interface (4.1) as both a
     collection target (new runs populate rows as they land) and a browsing surface (multiple views: by
     test, by machine, cross-machine comparison for one test) — likely a new tab alongside Run/Validate/
     Store & Summary/Discovery, not a retrofit of an existing one.
   - **Analysis feed.** Once populated, this is a natural input table for `wspy-archetype --kmeans`
     (shipped, see "Shipped since 4.2") and for `wspy-analyze`-style AI narrative generation that
     references how a workload compares to others in its cluster — but building the matrix itself never
     depended on clustering existing first; the two were always sequenced, not coupled.

**Tier 4 — report-layer additions on data already collected in 4.0:**

7. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
   process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
8. System (`--system`) → per-interface network attribution and local-vs-system-pressure
   attribution, plus steal-time capture (user/system/iowait are already captured and printed —
   `system.c`'s existing `/proc/stat` parsing — this item is the missing steal column and the
   analysis layer on top of what's already there, not the raw mix itself).
9. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
   `comm`-pattern role tagging).

**Tier 5 — GPU deeper profiling:**

10. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
    heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
11. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) — builds
    on 4.2's (shipped) GPU fusion layer (`gpu_fusion.c`, `--gpu-metrics`) for consistent per-metric data.
12. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`,
    extended once GPU runs feed the same index.

**Tier 6 — infra:**

13. Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for
    `--tree`/`--tree-open` — `ptrace` context-switches on every syscall entry/exit, which skews the
    very counters being measured for I/O-heavy or fork-heavy workloads. Also the eventual fix for the
    observer-effect caveat noted under "Critical-path / synchronization-latency: what's left" above.
14. Collector-plugin implementation (perf stat / trace-cmd / GPU tools as collectors behind the
    `collector` field, normalization path) — the schema seam shipped in 4.0; this is the actual
    implementation of wrapping a non-wspy collector.
15. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
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
16. Collapse `wspy-run`'s builtin profiles onto native `--passes` bin-packing. Low value relative to
    everything else on the 4.3 board, no dependents, safe to leave alone indefinitely. Most profiles
    are already collapsed as far as they can go: `deep-cpu`/`deep-gpu` folded their pure-counter middle
    pass onto `--passes=...` back in 4.1; their remaining separate passes all use `--interval 1`, which
    is hard-fatal'd against `--passes` (no defined multi-pass merge semantics for periodic ticks) — a
    real architectural constraint, not a missed collapse. `tree-heavy`/`gpu-compute` (`--tree`) and
    `ibs-basic`/`ibs-memory-deep`/`ibs-sample` (IBS) are excluded from `--passes` the same way; `quick`
    is already one pass; `zen-portable`/`zen4plus-deep` just compose other profiles. The only real
    remaining candidate is `deep-cpu-intel`, which still hand-authors 4 separate `wspy` invocations
    that don't touch any `--passes`-incompatible flag — collapsing it to one pass is the entire
    remaining scope. Note: this changes on-disk output shape from 4 files to 1, so anything downstream
    assuming those 4 filenames (external scripts, `tests/capability_matrix.sh`) would need checking.
17. Detect and resume interrupted `wspy-run` profiles (raised after a real host crash mid-batch, twice,
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
18. `wspy-run`-profile-driven batchable equivalent of the single-test-point Phoronix suite flow
    (`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab; see "Shipped since 4.2" for
    what's already landed) — a saved profile or `-c` file, run non-interactively/scriptable/batchable
    across many materialized test points at once. Only the direct wspy/checklist Run tab path (one test
    point, launched by a human clicking Run) exists today.
**Tier 7 — testing:**

19. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
20. Contributor guide for adding a collector/metric/schema bump safely.

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
8. AMD IBS sampling-mode: `--interval`-integrated periodic rates — split out of 4.3's now-shipped "AMD
   IBS sampling-mode support" item (see "Shipped since 4.2" and the Zen5/IBS deep-dive). Today
   `ibs_sample.c` only drains the perf ring buffer once, at end-of-run (walking/decoding records isn't
   async-signal-safe, and wspy has no poll/epoll loop anywhere to hang a real-time drain off of), so
   `--ibs-sample` combined with `--interval` zeroes every periodic row and populates only the final tail
   row. A real per-tick rate needs an actual poll-loop architectural change — genuinely a different
   scale of work than the fixed-offset decode work that shipped, not a small follow-on.
9. Uprobe-based function-argument capture ("ltrace-style" hooks) for hot functions identified by
    symbol-level profiling (now shipped — `--symbol-sample`/`wspy-symbolize`, see "Shipped since 4.2"
    and the "Symbol-level profiling deep-dive") — e.g. recovering GEMM dimensions (M/N/K) from a BLAS
    library's hottest routine once profiling shows it's the actual bottleneck, or any other case where a
    Pareto list of hot symbols isn't enough and the actual call arguments matter. Mechanically: manage
    `/sys/kernel/tracing/uprobe_events` to attach at a resolved `symbol+offset` in a target binary/shared
    library (the offset comes from `wspy-symbolize`'s own address-to-symbol resolution), declare an
    arg-capture spec against the calling-convention register at function entry (e.g. `%di:s32 %si:s32
    %dx:s32` for the first three integer args under the x86_64 System V ABI), and drain events via the
    same `perf_event_open()` + mmap ring-buffer pattern `ibs_sample.c`/`symbol_sample.c` already
    established (`perf_ring.c`) — mechanically similar to code already in the tree rather than a new
    paradigm. Reuses the shipped `--target=comm=<name>[,cmdline=<substr>]` comm/PID-match plumbing
    ("Shipped since 4.2" above) to decide which process(es) get the uprobe attached, same as it reuses
    `wspy-symbolize`'s resolution to find where to attach it. Deliberately not `PTRACE_POKETEXT`/manual
    INT3 injection — fragile, forces singlestepping, and has no precedent in this codebase; the kernel's
    uprobe infrastructure is the existing, supported mechanism for this. Explicit, real limitations to
    document up front rather than discover per-target: works cleanly only for register-passed scalar
    args under a stable calling convention; degrades or fails outright on inlined callees (no call site
    to hook), stack/struct-passed args, stripped symbols with no resolvable offset, and JIT'd code. Needs
    root (writing `uprobe_events` is root-only, same privilege class wspy already assumes for its
    ptrace/perf paths).

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
- **Does wspy's counter-group naming/organization need a separate Intel-focused and AMD-focused split
  (CLI flags, `--counters=` group names, web UI panels), since today's single vocabulary can feel
  AMD-centric on Intel hardware?** Raised 2026-07-22 off a real Intel Coremark run where several
  requested groups (`opcache`, `memory`) silently produced zero columns. Recommendation: no — don't
  fork the vocabulary. The Preset/Configuration/Option deep-dive above already commits to one vocabulary
  shared by the CLI/`wspy-run`/web UI specifically so none of them invents its own mental model, and
  forking by vendor would double that surface (two flag sets, two web-UI panels, two things to keep in
  sync in `wspy-run`/`wspy-queue`/tests) while also making `wspy-summary`/`wspy-plot`'s cross-run
  comparisons harder, since those already lean on shared column *identity* across vendors (`CLAUDE.md`:
  "column identity decides template membership"). The actual problem is **coverage, not naming**:
  `intel_raw_events[]` has zero entries for `COUNTER_OPCACHE`/`COUNTER_MEMORY` today, so those group
  names are silent no-ops on Intel with no warning — already tracked as the Intel hybrid deep-dive's
  "Additional Intel counters worth adding" list (op-cache/DSB raw events, `uncore_imc` DRAM bandwidth,
  `uncore_cbox` LLC, per-core-domain/iGPU RAPL energy, `i915` GPU PMU, C-state residency). Closing those
  gaps makes the *same* group names carry real data on Intel; a follow-up worth scoping alongside that
  work is making coverage/capability reporting explicit about "not implemented on this vendor" (a
  `raw_counter_group()` call that matched zero table entries) vs. "requested but failed to open" (a real
  per-run EINVAL/EACCES) — today both look identical (silently zero columns) from the CLI/web UI.
- **Does the test-point-level curated performance-summary README (Tier 4) reduce the scope of, or
  replace, this same tier's static-site publishing pipeline / characterization badges / interactive
  drill-down items?** Raised 2026-07-23 alongside that item. Recommendation: not decidable yet — scope
  the summary item first and see what it actually needs. A static-site pipeline that indexes these
  per-test-point READMEs might turn out to *be* most of the static-site item's remaining value (a
  browsable, multi-benchmark site) rather than a separate deliverable, and the characterization badges
  are a plausible direct input to the summary rather than a competing surface. Revisit once the summary
  item has a real design, not before.
- **Should the Topdown deep-dive's "platform formula registry" (versioned event/formula mapping per CPU
  family/model, for auditability) be scoped and built now?** Not previously linked from here, unlike the
  Intel-counters list above — noticed while auditing the deep-dives for staleness (2026-08-03).
  Recommendation: not yet. It currently has exactly two candidate inputs, both from the Zen5/IBS
  deep-dive (split ALU/AGU scheduler-stall counters, `IBS_LD_L1_DTLB_REFILL_LAT`) and neither is itself
  a numbered backlog item yet; design a registry against a real third input (e.g. once the Intel-counters
  list above starts landing) rather than against two AMD-only data points.

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
  "Export Benchmark Data: Result File to Test Suite (XML)" link, the seed mechanism for 4.3 Tier 7's
  new "openbenchmarking.org-seeded single-test-point Phoronix suites" item: https://openbenchmarking.org/

Note (2026-07-22): an earlier research pass hit an HTTP 403 fetching OpenBenchmarking.org directly from
this environment and left it unreviewed (see prior revisions of this note); a user-provided result URL
in this same conversation confirmed the export-XML link exists and is usable as a seed, so that blocker
was environment/fetch-specific, not a real access restriction — reviewed manually, not by this
environment's own fetch tooling.
