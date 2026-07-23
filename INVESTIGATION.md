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

### Intel hybrid / counter-grouping deep-dive
Real Intel hybrid hardware became available for the first time this cycle (a Raptor Lake HX host,
codenamed "carlsbad", 2026-07-22) and immediately surfaced a cluster of confirmed, hardware-verified
counter-grouping bugs in `topdown.c` — these predate this investigation entirely (the shared-group
design dates to commit `273e9af`, Dec 2023) and were never caught before because no Intel hardware
existed in this environment to exercise them. What's confirmed:

1. ~~`--per-core` on Intel silently measured only the first core~~ — **shipped**, see "Shipped since
   4.2". `intel_group_id`, a module-static "current Intel perf-group leader fd," was scoped to outlive
   a single `setup_counters()` call; `--per-core`'s setup loop calls `setup_counters()` once per
   eligible core back-to-back with no `close_counters()` in between, so every core after the first
   tried to open its counters as members of a group led by a *different* CPU's fd — the kernel requires
   group members to share their leader's cpu/task target, so those opens failed `EINVAL` silently. Fix:
   reset `intel_group_id = -1` at the top of every `setup_counters()` call.
2. ~~Topdown/topdown2 silently reported all-zero whenever any other Intel counter opened first~~ —
   **shipped**, alongside #1. Intel's Perf Metrics fixed-counter feature (`slots` + its
   `core.topdown-*` sub-events) is a genuine kernel-enforced special case: those sub-events are only
   valid as members of a group whose *literal* leader is `slots` itself. Because every Intel group
   funneled into one shared `intel_group_id` regardless of which group opened first, and `ipc`
   (default-on, list-ordered ahead of `topdown`) opens its own `instructions` event first, `slots`'s
   sub-metrics tried to join a group led by `instructions` and failed — silently zeroing `--topdown`'s
   output in its single most common invocation. Fix: a second, dedicated leader variable
   (`intel_topdown_group_id`) scoped to exactly the groups whose mask includes
   `COUNTER_TOPDOWN`/`COUNTER_TOPDOWN2`.
3. ~~The single-shared-Intel-group design cascades into wholesale counter loss once the combined group
   exceeds real hardware PMU capacity, and cannot mix counters across different underlying PMUs~~ —
   **shipped**, see "Shipped since 4.2"'s "Intel counter-group budget chunking". A perf
   event *group* requires every member to be simultaneously schedulable — no within-group multiplexing
   by design — so once that's impossible the kernel refuses further members with `EINVAL` rather than
   degrading gracefully. Confirmed live: a realistic multi-group combo
   (`--counters=dcache,icache,tlb,branch,cache2`) measured only 10/19 counters, with one *whole* group
   (`cache`, 9 counters) failing 0/9 outright rather than partial degradation; `wspy --capabilities`
   (`COUNTER_ALL`) makes this maximally visible (20/48 available). Separately, `--power` (aggregate)
   tries to put RAPL's `energy-pkg` event — a *different* dynamic PMU (`type=35` on this host, not the
   general-purpose `cpu`/`cpu_core` PMU) — into the same shared group as whatever opened first; a perf
   group can't span two PMUs, so `energy-pkg` fails `EINVAL` whenever anything else is set up in the
   same call (this specific RAPL-scope symptom's actual root cause is finding #4 below, still open — the
   grouping fix here stops RAPL from fighting for a slot in Intel's general group, but doesn't by itself
   fix RAPL's own `pid=0` scope bug). Fix: move Intel away from "one shared group across every requested counter" toward
   AMD's model (ungrouped, independently-multiplexed general-purpose events, with only the topdown
   Perf-Metrics family kept as its own small dedicated group per #2 above), or route grouping through
   `preflight.c`'s existing budget/bin-packing logic (`multipass.c`) instead of one ungated shared
   leader.
4. ~~RAPL/`energy-pkg` opened with the wrong scope (`pid=0` instead of `pid=-1`) on Intel~~ — **shipped.**
   Confirmed even fully isolated (`--power --no-ipc`, ruling out #3). `setup_counters()`'s dispatch only
   special-cased system-wide/uncore PMU semantics (`pid=-1`) for `pe.type == PERF_TYPE_L3` specifically;
   every other counter, including RAPL's `power` PMU (whose driver sets `task_ctx_nr =
   perf_invalid_context` and rejects task-scoped opens outright), fell through to the generic
   per-process branch. **Not actually Intel-specific** — on the author's AMD dev host, the `power` PMU's
   dynamic type apparently *coincidentally* equalled `PERF_TYPE_L3`'s sentinel (14), routing it through
   the correct branch by accident (the same coincidence independently documented in `manifest.c`'s own
   history); on this Intel host `power`'s real type is 35, doesn't collide, and took the wrong branch.
   Any host — AMD or Intel — where the `power` PMU's type doesn't happen to equal 14 hit this
   identically; it was only ever masked by chance. Fix: a new explicit `requires_system_wide` marker
   (`struct counter_info`, `cpu_info.h`) set by `power_counter_group()`/`ibs_counter_group()`/
   `raw_counter_group()`'s AMD L3 entries, replacing the incidental `PERF_TYPE_L3` type-value match
   entirely — verified live on carlsbad: the real perf_event_open() failure changed from `EINVAL` (wrong
   args, rejected regardless of privilege) to `EACCES` (right args, just needs `CAP_PERFMON`/root),
   exactly the signature this finding predicted.
5. ~~Intel topdown-family counters intermittently read back as zero/`-nan` despite full counter
   coverage~~ — **root-caused 2026-07-22, turned out to be two independent things, not one.**
   - **The corrupt-percentage half is a real code bug, shipped.** The originally-observed
     `spec_pipeline_pct=72407003082176.4` wasn't a divide-by-near-zero/denominator issue as guessed —
     it was `print_topdown()`'s Intel L2 splits (`backend_cpu`/`speculation_pipeline`/
     `frontend_bandwidth`/`retire_fastpath`) using plain unsigned subtraction instead of the `safe_sub()`
     clamp AMD's equivalent code already uses, wrapping to near-`ULONG_MAX` whenever an independently-read
     child counter ticks fractionally above its parent. Reproduced live with a branch-heavy workload
     (`spec_pipeline_pct=173616230410.5`) and fixed — see "Shipped since 4.2".
   - **The all-zero/`-nan` half is real but isn't Perf-Metrics-specific**, contrary to this finding's
     original "plain hardware/raw groups haven't shown this" read — plain `--ipc` (ordinary
     `PERF_TYPE_HARDWARE`, no fixed-counter family involved) reproduces the identical symptom at a
     similar rate. `strace`'d down to the raw kernel bytes on a failing run: `value=0,
     time_enabled≈10.8ms, time_running=0` — the counter was armed the whole window but never actually
     scheduled onto a PMU register during the (sub-10ms) child's life, so the zero is genuine, not a
     read/parse bug. Scales with target-process duration (0 failures across ~130 runs of a 0.3-0.4s
     workload vs. ~15-30% against `true`) and, per a standalone reproduction probe, with how many
     counters share a perf group — consistent with larger groups simply taking the kernel measurably
     longer to finish installing onto real hardware than a short-enough-lived child leaves available. No
     wspy-side fix exists (a `perf stat`-style direct-child-pid-targeting rewrite was tried in the probe
     and made it *worse*, not better). Full writeup in "Known gaps" above; not carried forward as an open
     backlog item.

Additional Intel counters worth adding, grounded in the same real-hardware pass (`/sys/bus/
event_source/devices/` enumerated live, not from documentation alone):
- ~~**Per-core-type-aware raw event tables.**~~ — **shipped.** `cpu_core`'s dynamic PMU type is `4` on
  this host (which happens to equal `PERF_TYPE_RAW`'s own numeric value — the likely reason
  `intel_raw_events[]`'s hardcoded `PERF_TYPE_RAW` had silently "worked" for P-cores despite never doing
  a real per-core PMU-type lookup the way `cpu_info.c` already did for ARM); `cpu_atom`'s type is `10` —
  confirmed different, and every event in `intel_raw_events[]` was P-core-only-correct, so Gracemont
  E-cores needed their own encodings entirely. Fix: `cpu_info.c` now resolves each core's real dynamic
  PMU type the same way it already did for ARM (reusing `mark_cpus_for_pmu()`); a new
  `intel_atom_raw_events[]` (`topdown.c`) carries Gracemont-correct encodings for
  instructions/cpu-cycles/topdown(4 fields, no L2 breakdown)/branch/L2 — every value read directly off
  this host's live `cpu_atom` PMU (`/sys/devices/cpu_atom/events/`, `perf stat -vv`), not guessed;
  `raw_counter_group()`/`setup_counter_groups()` gained a `core_class` parameter to select it. Gracemont
  has no `slots`/fixed-counter register at all — `print_topdown()` now synthesizes one from
  `cpu-cycles * 5`, a width measured empirically (4.9997 across 4 independent real runs). Verified live
  via `strace`: E-core opens now show `type=0xa` (10) with Gracemont's own configs, P-cores unchanged.
  `core_is_per_core_eligible()` no longer excludes `CORE_INTEL_ATOM`. See "Shipped since 4.2".
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
  natural Intel counterpart to Tier 1's AMD IBS sampling-mode item, comparable in spirit to IBS's
  `IbsOpData`/`DcMiss`/`NbIbsReqSrc` tag bits. Not investigated in depth this pass; worth scoping once
  IBS sampling-mode ships and its mmap-ring-buffer/per-sample decode infrastructure exists to model this
  against.

→ Findings 1-4 and half of 5 (the underflow fix) shipped (see "Shipped since 4.2"); finding 5's other
half is a documented, non-actionable perf-subsystem limitation (see "Known gaps"), not open backlog.
The E-core raw-event gap above (not one of the original 5 findings) has also shipped — nothing remains
open from this pass. This also removes the last blocker from Tier 3's "Core-class-aware topdown" item,
which is now just the weighted-aggregate work itself.

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
"Composite attribution," and "Core-class-aware topdown" (moved to 4.3, no longer blocked at all — real
Intel/AMD hybrid hardware became available this cycle, and the Gracemont E-core raw-event gap that
briefly blocked it has since shipped too, see "Core-class-aware topdown" in Tier 3 above).

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
9. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — **no longer blocked at
   all.** AMD Zen5/Zen5c hardware was already available (per-core classification shipped in 4.2); Intel
   hybrid hardware became available this cycle ("carlsbad") and its first real-hardware pass turned up
   correctness bugs more fundamental than the E-core-exclusion gap this item was originally scoped
   around — all now resolved, including Gracemont's own raw event tables (`intel_atom_raw_events[]`,
   `topdown.c` — see "Shipped since 4.2"), which is what this item was waiting on: E-core topdown data
   now exists (`--per-core --topdown` measures Gracemont cores directly) for the first time.
   `--affinity=coretype=<id>` (`affinity.c`) resolves x86 P-core/E-core and Zen5/Zen5c groups too
   (shipped, see "Shipped since 4.2"). This item is now just the weighted-aggregate/summary-presentation
   work itself.

**Tier 4 — publishing/reporting expansion, needs 4.1's report studio:**

10. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1's per-run curation studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats (WordPress/HTML/Markdown,
   4.1) rather than inventing a fourth.
11. Characterization badges + similarity panels in reports — a new block type in 4.1's curation studio
    drawing a badge from 4.2's (shipped) archetype scorecard (`wspy-archetype`), not a separate report
    surface.
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
17. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) — builds
    on 4.2's (shipped) GPU fusion layer (`gpu_fusion.c`, `--gpu-metrics`) for consistent per-metric data.
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
25. `wspy-run`-profile-driven batchable equivalent of the single-test-point Phoronix suite flow
    (`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab; see "Shipped since 4.2" for
    what's already landed) — a saved profile or `-c` file, run non-interactively/scriptable/batchable
    across many materialized test points at once. Only the direct wspy/checklist Run tab path (one test
    point, launched by a human clicking Run) exists today.

**Tier 8 — testing:**

26. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
27. Contributor guide for adding a collector/metric/schema bump safely.

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
