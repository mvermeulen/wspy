# wspy 4.2

wspy 4.2 is the largest release yet (70 merged PRs). It turns 4.1's reporting/workflow layer into a
deep hardware-and-workload characterization toolkit: full critical-path/synchronization-latency
process-tree instrumentation with an interactive tree viewer, core/thread affinity control with
per-core diagnostics, CPU energy/power counters (including per-core energy), a hierarchical
L1→L2→L3 topdown schema, AMD IBS hardening and Zen-family preset packs, a fused AMD+NVIDIA GPU
telemetry layer, new system-wide disk/memory/cgroup metrics, a repeatability/comparability
statistics layer (confidence intervals, PMU-aware warnings, a comparison matrix mode, and an
archetype scorecard), local-LLM narrative analysis of run artifacts, and a round of
release-engineering tooling. This note groups what shipped by theme and summarizes why it matters;
see `README.md`, `CLAUDE.md`, and `INVESTIGATION.md`'s "What shipped in 4.2" for full command/flag
reference and design detail.

## Process-tree instrumentation & the interactive tree viewer
- **Full critical-path/synchronization-latency coverage** — `--tree-futex`, `--tree-io`/
  `--tree-io-wait`, `--tree-connect`, `--tree-nanosleep`, `--tree-wait`, `--tree-poll`,
  `--tree-schedstat` (run-delay/timeslice), and `--tree-vmsize` (peak RSS + anon/file/shmem
  composition + swap) complete `proctree`'s originally-scoped syscall-latency candidates. Together
  they let a degraded interval phase be explained as blocked-in-kernel, runnable-but-not-scheduled,
  or a genuine on-CPU stall, instead of leaving that ambiguous.
- **`proctree --json` + interactive web viewer + run-to-run diff** — a versioned JSON export
  replaces the text tree as the interchange format for a new collapsible/searchable web viewer and a
  structural `--diff` mode; the web launcher gained on-demand tree-viewer and tree-diff pages, linked
  from every report that has a process tree.
- ARM64 build fix for the legacy poll syscalls the `--tree-poll` work introduced.
- Process-tree checklist card in the web launcher's Run tab grouped and gridded for readability.

## Core/thread affinity and per-core diagnostics
- **`--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|cpuset=<c0,...>`** — SMT/L3-domain/
  core-type topology discovery, `--list-affinity`, and manifest/run-index/`wspy-run`/web-launcher/
  `wspy-queue` wiring, shipped ahead of its originally planned phase.
- ARM PMU cache-event fixes.
- **`wspy-core-report`** — a new standalone binary reporting cross-core min/max/mean/stddev/CV for
  every `--per-core` metric, naming the hot/cold core and, on heterogeneous hosts, breaking stats out
  by core class; a new Validate-tab web UI hook runs it against a discovered or pasted CSV.
- **AMD Zen5/Zen5c core detection** — clusters family-0x1a cores by per-core cpufreq max to tell
  full Zen5 cores apart from the physically compact Zen5c cores on hybrid parts, feeding
  `wspy-core-report`'s class breakdown and fixing the topdown slots-per-cycle formula and per-core
  counter eligibility for the new class.
- **`--per-core-freq`** — live per-core cpufreq reading on `--per-core` rows, a matching web
  launcher checkbox, and a fix to `--per-core --interval`'s CSV shape (one row per core per tick,
  previously malformed).

## CPU energy and power
- **`--power`/`--no-power`** — package `pkg_joules`/`pkg_watts` via the `power` perf PMU, plus
  per-core `core_joules`/`core_watts` via `power_core` under `--power --per-core`.
- Web launcher folds power into the Performance-counters/System-metrics checklist rather than a
  separate card, since it's not a standalone collection pass.
- `setup_perf.sh` now also checks/grants `CAP_PERFMON` (needed for `energy-pkg` access beyond plain
  `perf_event_paranoid`), and the web launcher's Check button gained a real, `EACCES`-aware probe
  with remediation guidance.
- Brought `deep-gpu`'s systemtime pass in line with `deep-cpu`'s — both now carry `--power`.

## Topdown hierarchy and AMD counter fixes
- **Hierarchical L1→L2→L3 topdown schema** — the already-computed per-vendor L2 breakdown
  (ucode/fastpath, frontend latency/bandwidth, backend cpu/memory, speculation branch/pipeline) now
  reaches CSV as 9 new trailing columns, plus an L3 tie-in expressing `--topdown-backend`'s
  l1/l2/l3/dram/store-bound detail on the same denominator; both get matching `wspy-plot` templates
  (`topdown-detail`, `memory-bound-detail`) and a `topdown_formula_version` field in the
  manifest/run-index.
- AMD counter-group audit: wired in `cache3`/`memory`, extended the branch group, fixed an AMD L2
  hit/miss label swap and an unsigned-subtraction underflow, and skip AMD-only
  topdown-frontend/optlb passes on non-AMD hosts.

## AMD IBS and Zen-family presets
- Bit-swept `ibs_op`'s `ldlat` field on real Zen5 hardware and found the kernel enforces a minimum
  of 128 — `IBS_DEFAULT_LDLAT_THRESHOLD` (previously 120, silently failing to open the filtered
  counter) is fixed to match.
- **`zen-portable`/`zen4plus-deep`** — new `wspy-run` preset packs (`quick`+`ibs-basic`,
  `deep-cpu`+`ibs-memory-deep`) composed from existing profiles rather than hand-written flag
  strings, tuned to run cleanly across the whole Zen family or to assume Zen4+ hardware respectively.

## GPU support
- **NVIDIA GPU monitoring** via NVML (`--gpu-nvidia`), `dlopen()`'d at runtime with no build-time
  CUDA dependency, plus a matching checkbox in the web launcher's custom GPU card.
- **`gpu-compute`** builtin profile — tree tracing + system + power + both GPU backends + topdown on
  one shared `--interval` timeline, for workloads a separate-re-execution-per-category profile can't
  correlate tick-for-tick; also added CPU temperature (`SYSTEM_TEMP`), GPU-aware plot templates, and
  dual process-tree output.
- **ROCm SMI + sysfs GPU fusion layer** — `--gpu-metrics` now merges both backends into one column
  set with per-metric source tracking (`gpu_temp_source`/`gpu_activity_source`), and GPU telemetry
  provenance (requested flags, resolved device index, per-backend success) was added to the
  manifest/run-index.
- Fixed a `--gpu-smi --interval` CSV column-count gap and an `--capabilities` AMD sysfs
  device-selection marker bug, both found while validating on a real multi-GPU (AMD iGPU + NVIDIA
  dGPU) laptop.

## System-wide metrics
- **`SYSTEM_DISK`** — per-block-device read/write bytes and time-in-I/O, filtered to real disks
  (excluding loop/ram/zram devices, which also protects `wspy-plot`'s column budget on hosts with
  many virtual block devices).
- **`SYSTEM_MEM`** — host-wide free/cached/dirty/writeback/swap-free/committed-AS from
  `/proc/meminfo`.
- **cgroup v2 identity, resource limits, and `cpu.stat` throttling deltas** in the manifest/
  run-index, for fair comparison of runs in containerized or quota-limited environments.

## Reproducibility, comparability & statistics
- **Repeatability policy + confidence metadata** — `wspy-summary` now reports a 95% confidence
  interval of the mean and a `PASS`/`WARN:thin,noisy` repeatability verdict by default.
- **Comparison matrix mode** — `wspy-summary --group-by`/`--group-by-option` can group by affinity
  mode/preset/config/governor/virt-role or an arbitrary swept axis; the new `wspy-sweep` tool
  cross-products `--affinity` values against workloads and tags each cell for later comparison.
- **PMU-capability-aware comparability warnings** — a new `mixed-pmu` repeatability-verdict reason
  flags a bucket whose contributing runs differ in CPU vendor or requested/measured counter coverage.
- **Feature normalization + archetype scorecard** — `wspy-store` derives a coverage-aware feature
  vocabulary (`run_features`) from stored metrics, and the new `wspy-archetype` tool classifies a run
  along four axes (resource dominance, parallelism shape, control-flow style, runtime stability) with
  a confidence level, grounded in prior workload-clustering research.
- Compare-view curation (Phase 1) — an optional overview/per-row annotation layer on the web
  launcher's multi-run compare page.

## Local LLM (Ollama) narrative analysis
- **`wspy-analyze`** — turns already-computed, already-validated run numbers into prose via a local
  Ollama model (the model narrates; it never re-derives the underlying classification), with
  multi-model sweep, versioned prompt templates with a critique mode, a comparative mode
  (`--compare-rundir`), and full web-launcher integration with a model picker and an always-visible
  "AI-generated" marker on any content copied from it.

## Phoronix integration
- **`result_notifier` hook capture** — `pre_test_run`/`post_test_run` hook output is now captured
  into a per-pass artifact regardless of which front end (`wspy-run`, the web launcher's custom path,
  `wspy-queue`) launched the test, with a setup-script directory-name fix and a web launcher warning
  for a known upstream `result_notifier.php` crash bug.
- **`wspy-ledger` correctness fixes** — auto-flags workloads via known-unavailable Phoronix
  `ExternalDependencies` tags instead of requiring hand annotation, and no longer permanently
  misreports a workload's status after its output directory is deleted.
- The Run tab's Check button gained `gnuplot`-availability and Phoronix batch-mode-configuration
  checks.
- **`wspy-run`'s `--tree` pass timeout** is now sized from an actual Phoronix run-time estimate
  instead of a fixed constant, with a 6-hour ceiling as a hang backstop rather than a
  normal-operation limit.

## CLI cleanup & correctness fixes
- **`--counters=<list>`** is the new recommended way to select counter groups; the ~20 individual
  per-group boolean flags are now soft-deprecated (still work, just warn once). Also fixed a
  `getopt_long` `val`-collision bug that let some malformed flags silently run the workload anyway
  instead of reporting a usage error, and cleaned up the `--help` text.
- Fixed an `--interval` tail-print race where a queued `SIGALRM` could splice a periodic row into the
  middle of the final tail row's output.

## Release engineering & documentation tooling
- **`scripts/release_prep.sh`** — a repeatable pre-flight/PR-label-audit/version-bump/
  stale-reference-grep/test-matrix/release-notes-draft script, ending in print-only tag/push/publish
  commands it never runs itself. Found and fixed a real missing release label the first time it ran.
- **`tests/doc_version_check.sh`** — a grep-based doc/version drift check, wired into
  `run_tests.sh`. Found and fixed real stale schema-version references in
  `doc/ARTIFACT_CONTRACT.md` and a missing README section the first time it ran.
- **`doc/PROFILE_COOKBOOK.md`** — a reading guide for `wspy-summary`'s verdict column,
  `wspy-archetype`'s confidence, and `phase.c`'s phase output, grounded in real captured example
  output.
- **`wspy-bundle`** — bundles a run directory's manifests, raw output, and derived artifacts into a
  single checksummed, portable `.tar.gz`, with a matching "Download reproducibility bundle"
  report-page link.

## Hand-testing coverage
This release leaned heavily on real-hardware validation rather than synthetic fixtures alone: real
AMD Zen5 hardware surfaced and fixed the IBS `ldlat` minimum, and confirmed per-core energy and
Zen5/Zen5c core detection; a real multi-GPU (AMD iGPU + NVIDIA dGPU) laptop confirmed correct
per-backend device selection and surfaced an `--capabilities` marker bug; ARM64 got real
topology/topdown/ptrace validation, not just register-access prep; `wspy-validate`/`wspy-ledger`
were exercised against a 100+-run accumulated index, including interrupted-process and
mixed-schema-version scenarios; and cgroup v2 throttling/limits were confirmed against a real
no-cpu-controller leaf cgroup. See `INVESTIGATION.md`'s "What shipped in 4.2" for the full
validation detail behind each item.

## What's next
4.3 turns to AMD IBS sampling-mode support (decoding real per-sample tag data instead of
counting-mode sample counts), the regression-detection/comparability-scoring work the 4.1 normalized
store enables, and statistical clustering/nearest-neighbor cluster-profile cards building on this
release's archetype scorecard — see `INVESTIGATION.md`'s "4.3 priorities" for the full list.

## Pull requests in this release
#58 #59 #60 #61 #62 #63 #64 #65 #66 #67 #68 #69 #70 #71 #72 #73 #74 #75 #77 #78 #79 #80 #81 #82 #83
#84 #85 #86 #87 #88 #89 #90 #91 #92 #93 #94 #95 #96 #97 #98 #99 #100 #101 #102 #103 #104 #105 #106
#107 #108 #109 #110 #111 #112 #113 #114 #115 #116 #117 #118 #119 #120 #121 #122 #123 #124 #125 #126
#127 #128

**Full Changelog**: https://github.com/mvermeulen/wspy/compare/v4.1.1...v4.2
