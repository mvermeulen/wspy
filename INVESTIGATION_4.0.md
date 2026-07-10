# wspy Investigation 4.0

Date: 2026-07-09 (consolidated pass)
Status: Phase 4.0 in progress — minimal foundation slice shipped (see "Minimal foundation slice"),
next tranche of 4.0-tagged work queued below ("Next up after the minimal slice")

## Purpose
This document captures ideas for a future round of improvements focused on making benchmark
collection, organization, and publication easier and more repeatable.

## How to use this document
- Treat this file as a prioritization backlog, not a committed implementation plan.
- "Idea inventory" is the single source of truth for what an idea is and which phase it targets —
  it replaces three earlier overlapping lists (blog-derived opportunities / easy edits / major
  ideas) that said the same things at different altitudes. Don't re-add an idea in prose elsewhere;
  extend its inventory row or its track deep-dive instead.
- "Track deep-dives" hold reasoning that doesn't fit a table row (Zen5/IBS, topdown, AMD GPU,
  portability, process telemetry, characterization). Each ends by pointing back at its inventory
  rows so the two stay in sync.
- "Phased plan" is the scheduling view. It's generated from the inventory's `Phase` column, not an
  independently maintained list — if you move an idea's phase, update it in the inventory only.
- "Open questions" now carry a recommendation each; re-open a question by editing its row, not by
  appending new "what about X" prose at the bottom of the file.

Primary context reviewed (2026-07-08 pass, unchanged from prior pass):
- https://mvermeulen.org/perf/, /workloads/cpu2017/, /workloads/phoronix/,
  /2024/02/10/adding-summary-statistics-for-all-benchmarks/, /tools/wspy/
- Local: `workload/cpu2017/run_test.sh`, `run_all.sh`; `workload/phoronix/run_test.sh`,
  `run_all.sh`, `gnuplot.sh`; `workload/pbbsbench/run.sh`

## Verified against current source (this pass)
Prior passes speculated about the codebase from blog content and a source skim without always
citing line numbers. This pass re-checked the load-bearing claims directly:

- **Superseded (2026-07-08, later in this cycle): the `amd_sysfs.c` `card1` hardcode is fixed.** This
  bullet previously said `amd_sysfs.c:25,31,48,67` all read
  `/sys/class/drm/card1/device/{gpu_busy_percent,gpu_metrics}` literally, no scan, no env override —
  that was accurate when first written but went stale after `feature/gpu-path-scan` merged (PR #7).
  `amd_sysfs_initialize()` now calls `find_amd_drm_card()`, which scans
  `/sys/class/drm/card*/device/vendor` for the lowest-numbered AMD (`0x1002`) card and resolves the
  busy-percent/metrics paths against it. What's still actually missing: per-device selection
  (`--gpu-device=<idx>`) and full multi-GPU enumeration — a multi-AMD-GPU machine still only ever
  uses the lowest-numbered match. See "Minimal foundation slice" item 6, now shipped.
- **Superseded (2026-07-09, later in this cycle): `ptrace_loop()`'s register-access abstraction
  shipped.** This bullet previously said `topdown.c:449,454,456` read `regs.orig_rax`/`regs.rsi`
  directly with no macro layer — that's fixed: `ptrace_arch.h` now provides `wspy_regs_t`,
  `ptrace_getregs()`, and `PTRACE_SYSCALL_NUM`/`PTRACE_SYSCALL_ARG2` macros behind
  `#ifdef __x86_64__`/`__aarch64__`, and `ptrace_loop()` uses them instead of the raw struct fields.
  Only the x86_64 branch has been built/exercised; the aarch64 branch is unverified prep modeled on
  documented arm64 ptrace/syscall-ABI conventions, not a working ARM64 backend. `cpu_info.c:33,61`
  calling `__cpuid()` from `<cpuid.h>` remains a genuine, not-yet-addressed portability blocker for
  ARM64 — that part of this bullet still stands.
- **Superseded (2026-07-08, later in this cycle): the manifest/run-index/profile-launcher slice
  shipped.** This section previously said "there is no manifest, no JSON output, and no schema
  anywhere in the codebase today" — that was accurate when first written but was left stale after
  the three branches below merged into `master`, understating how much of the "minimal foundation
  slice" (below) is now done:
  - `manifest.c`/`manifest.h` — JSON run manifest (`--manifest <file>`), `MANIFEST_SCHEMA_VERSION`
    (SemVer).
  - `run_index.c`/`run_index.h` — JSONL run-index append (`--run-index <file>`),
    `RUN_INDEX_SCHEMA_VERSION` (SemVer, versioned independently of the manifest).
  - `wspy-run` — the common workload wrapper / profile-driven launcher (builtin `quick`/`deep-cpu`/
    `deep-gpu`/`tree-heavy` profiles, `-c <file>` config-file execution), reusing the two above for
    `--manifest-dir`/`--run-index`.
  What's still actually missing, i.e. what the "invent the manifest" framing below still applies to:
  no unified output layout, no coverage ledger, and `workload/*/run_test.sh` haven't been migrated to
  call `wspy-run` yet. (Superseded 2026-07-09: the "no validation/quality-check pass" clause is no
  longer true — `wspy-validate`/`validate.c` shipped, see "Run artifact foundation" above. It's also
  the first manifest *reader* in the tree and does warn on a `MANIFEST_SCHEMA_VERSION` major-version
  mismatch, which partially covers the still-open "Portability and robustness" row below — that row's
  remaining gap is run-index (`RUN_INDEX_SCHEMA_VERSION`) ingest, which `wspy-validate` doesn't touch.)
  (Superseded 2026-07-09, later in this cycle: the "no coverage ledger" clause is also no longer
  true — `wspy-ledger`/`ledger.c` shipped, see "Run artifact foundation" above. It's a second
  run-index reader alongside `wspy-validate`, though it still doesn't check `RUN_INDEX_SCHEMA_VERSION`
  itself, so the "Portability and robustness" gap above stands.)
  (Superseded 2026-07-10: the "Portability and robustness" gap just referenced is now closed —
  `wspy-ledger` checks each run-index record's `schema_version` against `RUN_INDEX_SCHEMA_VERSION`
  and warns on a major-version mismatch or missing field, same as `wspy-validate` does for manifests.
  See "Portability and robustness" above.)
  Building `wspy-run` also surfaced a real, previously-unknown bug: `wspy`'s own process exit code
  never reflects the launched command's success — see the new "Portability and robustness" row
  ("Propagate child exit status...").
  This matters for scoping: 4.0 isn't "extend the manifest," it's "invent the manifest," which was a
  bigger first slice than the word "foundation" suggests (see "Minimal foundation slice" below) —
  and it's now substantially built, not just scoped.
- **`--tree-open` already exists and works as described** — `wspy.c:106`, `topdown.c:455` gate
  `SYS_openat` capture behind the `tree_open` flag. The "convert it into higher-level insight" idea
  is additive, not speculative.
- **Superseded (2026-07-09, later in this cycle): the `getrusage` CSV/normal mismatch is fixed.** This
  bullet previously said `topdown.c:909` `print_usage()` printed `nvcsw`, `nivcsw`, `inblock`,
  `oublock` in `PRINT_NORMAL` mode but only emitted `elapsed,utime,stime` in `PRINT_CSV`/
  `PRINT_CSV_HEADER` — that was accurate when first written but went stale once the fix landed.
  `print_usage()` now emits all four fields in CSV too (`elapsed,utime,stime,nvcsw,nivcsw,inblock,
  oublock`), and `run_tests.sh`'s CSV column-order checks were updated to match. See "Process /
  `getrusage` / `/proc` telemetry" track, now shipped.
- **The `workload/phoronix/run_test.sh` 7-8 invocation pattern is real and current** — confirmed by
  reading the script directly; it launches `phoronix-test-suite batch-run $TESTNAME` up to 8 times
  per AMD run with different counter flag combinations to avoid multiplexing. This is the strongest
  concrete case for native multi-pass execution (inventory: "Native multi-pass counter execution").
- **CUDA/Vulkan: zero footprint in the codebase.** No NVIDIA, CUDA, CUPTI, Vulkan, or Nsight
  references anywhere in the source tree. GPU support is exclusively AMD, via two independent paths
  (`amd_smi.c` using ROCm's `libamd_smi`, `amd_sysfs.c` reading sysfs directly), both gated by
  `AMDGPU=1` at build time. Per project scope decision (2026-07-08): CUDA/Vulkan instrumentation is
  cut from this roadmap — see "AMD GPU track" below.

## Idea inventory
Single source of truth. `Phase` is a target, not a commitment — see "Phased plan" for how phases are
meant to be read. Ideas already implemented are not listed (this file is 4.0 forward, not a feature
log — `git log`/`README.md` cover what exists).

### Run artifact foundation
Shipped since the last consolidated pass (removed from the inventory per this file's own "ideas
already implemented are not listed" rule — see `git log`/`CLAUDE.md` for what exists): run manifest
(JSON) + SemVer schema version (`manifest.c`), run index generation (`run_index.c`), the common
workload wrapper / profile-driven launcher (`wspy-run`, builtin `quick`/`deep-cpu`/`deep-gpu`/
`tree-heavy` profiles plus `-c <file>` config-file execution), and basic pre-publish validation/quality
checks (`wspy-validate`, backed by a new `json_reader.c` parser — the tree's first manifest *reader*,
not just writer). Given one or more `manifest.json` paths, it checks schema version, required
`output_files` existence, output CSV well-formedness/non-emptiness, workload exit status,
counter-coverage completeness (partial coverage warns, doesn't fail — that's `coverage.c`'s
by-design degradation), positive elapsed time, and per-column sanity ranges on the CSV (an
extensible `sanity_bounds[]` table plus a generic finite/non-negative/not-implausibly-large rule for
every other numeric column). `-q`/`--quiet` and `--strict` control report verbosity and whether
warnings affect exit status. See `CLAUDE.md`'s `validate.c`/`json_reader.c` entries for the full
behavior; this was item 1 of the "next up after the minimal slice" list below.

Also shipped since that pass: the coverage ledger (`wspy-ledger`/`ledger.c`, 2026-07-09) — workload
status (done/skipped/unsupported/needs-tool-support) generated from one or more `--run-index` files
instead of the hand-maintained "what's still missing" tracking `workload/phoronix/phoronix.tests.txt`
and the "Intel not supported" early-exit in `workload/cpu2017/run_test.sh` did today. Given a
workload-list file (name plus an optional `unsupported`/`needs-tool-support` annotation and note),
it matches each name as a substring against every run-index record's `command` array: no match is
`skipped`, a matching record with a clean exit is `done`, matching record(s) with none succeeding is
`needs-tool-support` (an explicit annotation always overrides inference). `--csv` and `--strict`
mirror `wspy-validate`'s conventions. This was item 1 of the "next up after the minimal slice" list
at the time it shipped.
| Idea | Phase | Why |
| --- | --- | --- |
| Unified output layout (`suite/benchmark/run_id/{metrics.csv,summary.txt,process.tree.txt,plots/*.png,manifest.json}`) | 4.0 | cpu2017/phoronix/pbbsbench each invent their own file layout; publishing tools currently need suite-specific logic. |
| Reproducibility bundle export (tarball: manifest + raw + derived per batch) | 4.1 | Depends on manifest/index existing; archival/re-analysis convenience, not foundational. |
| Traceability links (summary row → manifest → raw CSV → plots → tree artifacts) | 4.1 | Same dependency; fold into the report generator once there's a normalized index to link from. |

### Reproducibility, comparability, statistics
Shipped since the last consolidated pass (removed from the inventory per this file's own "ideas
already implemented are not listed" rule — see `git log`/`CLAUDE.md` for what exists): counter
capability discovery + "measured vs unavailable" coverage reporting (`coverage.c`, `wspy
--capabilities`). `setup_counters()` (`topdown.c`) no longer treats any single `perf_event_open`
failure as fatal for the whole run -- it records the outcome (`coverage_note()`) and continues, so a
run with partial counter access (restrictive `perf_event_paranoid`, NMI-watchdog contention, an
unsupported raw event) still produces output instead of aborting. Coverage counts/gaps are surfaced
in human/CSV output, `--manifest`'s `counter_coverage` object (bumped `MANIFEST_SCHEMA_VERSION` to
1.1.0), and `--run-index`'s leaner counts-only `counter_coverage` field (bumped
`RUN_INDEX_SCHEMA_VERSION` to 1.1.0). Also shipped since that pass: environment/provenance capture
(`provenance.c`) -- virtualization role (host/guest, from `cpuid`), microcode version, BIOS
vendor/version/date, CPU frequency governor + uniformity across cores, total memory, and the
compiler/libc that built the binary. Each of the 9 tracked fields degrades to unavailable-with-reason
independently (mirroring the counter coverage "measured vs unavailable" pattern) rather than blocking
the run, per this section's own "optional-but-recommended field with a coverage flag" recommendation
below. Surfaced as `environment`/`environment_coverage` in `--manifest` (bumped
`MANIFEST_SCHEMA_VERSION` to 1.2.0) and the same fields in leaner compact form plus counts-only
coverage in `--run-index` (bumped `RUN_INDEX_SCHEMA_VERSION` to 1.2.0). Kernel release was already
captured pre-4.0 (`--manifest`'s `host.kernel_release`); this fills in the rest of the field list this
row named.
| Idea | Phase | Why |
| --- | --- | --- |
| Repeatability policy + confidence metadata (mean, stddev, CV, CI) as default output | 4.1 | Depends on the run index (need multiple runs grouped) more than on the manifest alone. |
| Outlier/threshold engine (per-metric, global + suite-local) | 4.1 | Needs a populated index/history to threshold against; not meaningful on a single run. |
| Comparison matrix mode (sweep compiler/kernel/governor/SMT/VM-native) | 4.1 | Builds on the profile-driven launcher; a declarative sweep runner, not new collection logic. |
| Canonical metrics schema + normalized store (SQLite and/or Parquet) | 4.1–4.2 | **Gap in the prior draft** — "major idea B" (data lake) was never assigned a phase. Needed once the run index outgrows CSV/JSONL scanning; keep raw files, add normalized querying as a second layer. |
| Machine/environment comparability scoring | 4.2 | Depends on provenance capture (4.0) existing across enough runs to score against. |
| Baselines and regression/anomaly detection | 4.2 | Depends on the normalized store existing (needs to query history, not just the latest run). |
| Distribution-first reporting (quantiles, clustering) | 4.2 | Same dependency; aggregate means already exist, this is a report-layer addition. |

### Topdown quality
See "Topdown deep-dive" below for reasoning; MVP acceptance criteria unchanged from prior draft.
Shipped since the last consolidated pass (removed from the inventory per this file's own "ideas
already implemented are not listed" rule — see `git log`/`CLAUDE.md` for what exists): confidence
envelope + decomposition sanity checks for the primary `print_topdown()` breakdown (`topdown.c`).
Each of retire/frontend/backend/speculate is annotated with `confidence_ratio()`
(`time_running/time_enabled`) for the specific raw counter backing it, and the whole row carries a
row-level confidence (the min across those four) plus a `sanity_pct` decomposition check
(retire+frontend+backend+speculate vs. slots_no_contention) — both surfaced in human output
(`low-confidence(NN%)` per-row annotations, a `sanity check` summary line) and CSV
(`confidence,sanity` columns appended after `speculate`). Deliberately scoped to the level-1
breakdown only — the level-2 hierarchy (backend cpu/memory, frontend latency/bandwidth, etc.) is
still 4.1 work per the "Hierarchical" row below, and `print_topdown_be/fe/op` (the separate
level-2-only groups) are untouched.
| Idea | Phase | Why |
| --- | --- | --- |
| Hierarchical (parent→child) schema + explicit raw-vs-contention-adjusted denominators + formula/version metadata | 4.1 | Requires the manifest/normalized-schema work from 4.0 to have somewhere to live. |
| Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) | 4.1 | Depends on per-core collection (`--per-core`) plus the hierarchical schema. |
| Phase-aware topdown (warmup/steady/degraded segmentation, drift signal) | 4.2 | Depends on interval phase-segmentation (see "Existing capability extensions") landing first. |
| Composite attribution (topdown + cache/TLB/IBS signals) | 4.2 | Depends on both the topdown hierarchy and IBS memory-source classes existing. |

### Zen5 / IBS
See "Zen5/IBS deep-dive" below.

Shipped since the last consolidated pass: capability-driven IBS probing (`ibs.c`/`ibs.h`,
2026-07-10) — `ibs_probe()` discovers `ibs_fetch`/`ibs_op` PMU support from
`/sys/bus/event_source/devices/{ibs_fetch,ibs_op}/{type,format/*,caps/*}` at runtime (readdir-driven,
not a hardcoded field list), so it doesn't hardcode which format fields/caps exist -- both vary by
kernel version and CPU generation (Zen4 vs Zen5 caps already differ on this project's own hardware).
Wired into `wspy --capabilities`, which now prints an IBS capability report (PMU type, format fields,
caps) alongside the existing counter capability report. This was item 1 of the "next up after the
minimal slice" list below.

Also shipped (2026-07-10, same day): `ibs-basic`/`ibs-memory-deep` collection profiles plus sampling
skew/quality annotations (`ibs.c`'s `ibs_build_fetch_event()`/`ibs_build_op_event()`/
`ibs_counter_group()`, `topdown.c`'s `print_ibs()`, `wspy.c`'s `--ibs-basic`/`--ibs-memory-deep`/
`--ibs-maxcnt`/`--ibs-ldlat`/`--ibs-fetchlat`, `wspy-run`'s `ibs-basic`/`ibs-memory-deep` builtin
profiles) — both items shipped together since the skew annotations only mean something once the
profiles that trigger them exist, per this list's own sequencing note below. `ibs-basic` opens
`ibs_fetch`/`ibs_op` as ordinary counting events (perf's "stat" mode, not sampling/mmap capture, so it
reuses the same `perf_event_open()`+`read()` path every other counter in this tree uses);
`ibs-memory-deep` additionally requests `l3missonly`+`ldlat` filtering on `ibs_op` (and
`l3missonly`+`fetchlat` on `ibs_fetch` where exposed) and opens a second, always-unfiltered `ibs_op`
counter as a baseline. Config bits are assembled by parsing each format field's sysfs `location` string
at runtime (e.g. `"config1:0-11"`), not hardcoded offsets, so a kernel/CPU that places a field
differently still works. Because l3missonly/ldlat filtering is documented to skew the effective IBS
sampling period, that skew is now visible in output rather than only in a code comment: CSV gets
`ibs_op_unfiltered`/`ibs_op_accepted_ratio` (the filtered/unfiltered count ratio) plus
`ibs_l3missonly`/`ibs_ldlat_threshold`/`ibs_fetchlat_threshold`, and a filter requested by the profile
but unsupported by the running kernel/CPU prints an explicit warning instead of silently collecting
unfiltered data under a "deep" label. See `CLAUDE.md`'s `ibs.c` entry for the full behavior. Per this
file's own "ideas already implemented are not listed" rule both rows are dropped.
| Idea | Phase | Why |
| --- | --- | --- |
| Zen-family preset packs (`zen-portable`, `zen4plus-deep`) | 4.1 | Convenience layer now that capability probing exists (`ibs.c`); not needed for correctness. |
| PMU-capability-aware comparability warnings | 4.1 | Depends on the general comparability-scoring work (4.2) or at minimum the provenance capture (4.0). |
| IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) | 4.2 | Depends on both IBS memory-source classes and the topdown hierarchy existing. |

### Process / `getrusage` / `/proc` telemetry
Shipped since the last consolidated pass: `rusage` CSV/normal output mismatch fix — `print_usage()`
(`topdown.c`) now emits `nvcsw,nivcsw,inblock,oublock` in `PRINT_CSV`/`PRINT_CSV_HEADER`, matching the
fields `PRINT_NORMAL` already printed. `run_tests.sh`'s CSV column-order checks were updated for the
four new columns landing between `stime` and the GPU/`ipc` columns. Also shipped: expanded `getrusage`
coverage — `print_usage()` now reports `ru_maxrss`/`ru_minflt`/`ru_majflt`/`ru_nswap` too, in both
`PRINT_CSV` (raw values appended after `oublock`: `maxrss,minflt,majflt,nswap`) and `PRINT_NORMAL`
(raw value plus a normalized rate — MB for `maxrss`, /sec for the three fault/swap counters — as an
inline `#` comment, matching how `inblock`/`oublock` were already annotated). `run_tests.sh`'s CSV
column-order checks were updated again for these four columns. Per this file's own "ideas already
implemented are not listed" rule both rows are dropped.
| Idea | Phase | Why |
| --- | --- | --- |
| `/proc/<pid>/io` byte counters (read/write/cancelled-write bytes) | 4.1 | Distinguishes small-op-count vs high-throughput I/O; needs `--tree`-style per-pid tracking already present. |
| `/proc/<pid>/schedstat` run-delay/timeslice capture | 4.1 | Separates CPU saturation from waiting-to-run; same per-pid dependency. |
| Memory footprint detail (`VmRSS`/`VmHWM`/anon-file-shmem split via `/proc/<pid>/status` or `smaps_rollup`) | 4.1 | Same per-pid dependency. |
| cgroup identity + limits in manifest, `cpu.stat` throttling stats | 4.1 | Needed for fair comparison in containerized environments; depends on manifest existing (4.0). |
| Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional `comm`-pattern role tagging) | 4.2 | Builds on existing `--tree` output; report-layer addition, not new collection. |

### Characterization and clustering
| Idea | Phase | Why |
| --- | --- | --- |
| Feature normalization prerequisites (fixed feature set from counters/topdown/faults/context-switch/I-O) | 4.1 | Needs the normalized metrics schema (above) to draw features from. |
| Archetype scorecard (parallelism shape, resource dominance, control-flow style, runtime stability) + confidence + top-2 alternatives | 4.1 | Depends on feature normalization; explicitly emit confidence/ambiguity rather than a hard label. |
| Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace only when data coverage differs) | 4.2 | Needs a populated normalized store with enough runs to cluster against. |
| Temporal drift detection (cluster movement across versions/configs/machines) | 4.3 | Needs clustering (4.2) plus enough history to detect movement; treat as an investigation trigger, not a standalone feature. |

### AMD GPU track
CUDA and Vulkan instrumentation are **cut from this roadmap** (see "Verified against current
source"): this codebase has no NVIDIA or Vulkan code, builds against ROCm only, and is described in
`CLAUDE.md` as the author's own research testbed — CUPTI/Nsight/RenderDoc integration would be a
separate project on hardware/APIs not currently in scope. Revisit only if the project's mission
changes to cross-vendor GPU profiling.

Shipped since the last consolidated pass (`feature/gpu-path-scan`, PR #7): the dynamic path-scan half
of the row below — `amd_sysfs.c`'s `find_amd_drm_card()` now scans `/sys/class/drm/card*/device/vendor`
for the lowest-numbered AMD (`0x1002`) card instead of hardcoding `card1`. Per this file's own "ideas
already implemented are not listed" rule the shipped half is dropped; the row is narrowed to what's
still open.
| Idea | Phase | Why |
| --- | --- | --- |
| `--gpu-device=<idx>` override + multi-GPU enumeration (report/select among multiple AMD cards) | 4.0 | The path scan (shipped) always picks the lowest-numbered AMD card; a machine with more than one AMD GPU has no way to target a specific one or see the others, and `amd_smi.c`'s device enumeration isn't threaded through `amd_sysfs.c` either. |
| ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) | 4.1 | Merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`) once each is trustworthy standalone. |
| Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory activity) | 4.1 | Reuses 4.0 foundation work rather than building a parallel GPU-only pipeline. |
| `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) | 4.2 | Heavier, optional trace-rich profile — same "default vs debug profile" pattern as IBS. |
| Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) | 4.2 | Depends on the fusion layer providing consistent per-metric data first. |
| GPU coverage ledger (backend/device-class support, caveats) | 4.2 | Same pattern as `wspy-ledger` (the CPU workload coverage ledger, shipped), extended once GPU runs feed the same index. |
| Fold into general environment-comparability scoring (power cap, memory clock, thermal state, driver version) | 4.2 | No separate "GPU comparability score" needed — this is the general provenance/comparability item (Reproducibility track) applied to GPU fields; keep one scoring mechanism, not two. |

### Existing-capability extensions
| Idea | Phase | Why |
| --- | --- | --- |
| Counter-fit preflight ("will this profile multiplex heavily?") + suggested downgrades | 4.0 | Availability/NMI-watchdog handling already exists at runtime; this surfaces fit quality *before* a run instead of discovering it after. |
| Interval (`--interval`) → automatic phase-boundary detection (warmup/steady/degraded) | 4.0 | Prerequisite for phase-aware topdown (Topdown track, 4.2) and phase-aware IBS; basic marker detection can land now. |
| Per-core (`--per-core`) → imbalance/hot-core/migration diagnostics, core-class summaries | 4.1 | Report-layer addition on data `--per-core` already collects. |
| `proctree` → JSON/Graphviz export + run-to-run tree diff | 4.1 | Tree capture already exists; today's consumption is text-only. |
| `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms, process→file maps) | 4.2 | `tree_open`/`SYS_openat` capture already exists (`topdown.c:455`); this is a report-layer summarization of data already collected. |
| System (`--system`) → per-interface network attribution, user/system/iowait/steal mix, local-vs-system-pressure attribution | 4.2 | Report-layer addition to existing `system.c` collection. |

### Portability and robustness
Shipped since the last consolidated pass (`feature/exit-with-child`): opt-in child exit status
propagation — `wspy --exit-with-child` now exits with the launched command's own exit code (128+signal
if it was killed by a signal), instead of `main()`'s unconditional `return 0`. This also fixed a related
manifest/run-index gap as a side effect: `--tree` mode previously always reported `exit_status.known:
false` because the root child's status wasn't captured outside `ptrace_loop()`'s own wait loop;
`ptrace_loop()` now records it (`child_exit_known`/`child_exited`/`child_exit_code`/`child_signaled`/
`child_term_signal` in `topdown.c`, shared with the non-tree `wait4()` path in `wspy.c`), so both modes
report real exit status now. Per this file's own "ideas already implemented are not listed" rule the row
is dropped.

Also shipped since that pass: arch-neutral `ptrace` register-access macros (`ptrace_arch.h`,
2026-07-09) — `wspy_regs_t`, `ptrace_getregs()`, and `PTRACE_SYSCALL_NUM`/`PTRACE_SYSCALL_ARG2`
macros behind `#ifdef __x86_64__`/`__aarch64__`, replacing `topdown.c`'s direct `struct
user_regs_struct`/`PTRACE_GETREGS`/`.orig_rax`/`.rsi` use in `ptrace_loop()`. A mechanical refactor
of the existing (working, x86_64) logic — the `__aarch64__` branch is unverified prep modeled on
documented arm64 ptrace/syscall-ABI conventions, not a tested ARM64 backend. See `CLAUDE.md`'s
`ptrace_arch.h` entry for the full behavior; this was item 1 of the "next up after the minimal
slice" list below.

Also shipped since that pass: run-index schema validation on ingest (2026-07-10, `ledger.c`) —
`wspy-ledger` now checks each run-index record's `schema_version` field against
`RUN_INDEX_SCHEMA_VERSION` and warns (once per distinct mismatched version, per file, to stderr) on
a major-version difference or a missing field entirely, mirroring `validate.c`'s
`check_schema_version()` for manifests (WARN, not FAIL — a run-index reader is meant to tolerate
minor/patch schema drift, same as `wspy-validate` tolerates it for manifests). This closes the gap
this file previously called out: the write side (`RUN_INDEX_SCHEMA_VERSION` in `run_index.h`) had
shipped with no reader anywhere in the tree ever checking it.
| Idea | Phase | Why |
| --- | --- | --- |
| Fallback CPU topology detection for non-x86_64 (`/proc/cpuinfo`, `/sys/devices/system/cpu`) | 4.1 | Actual ARM64 `cpu_info` support; the `ptrace` macro extraction it depended on has since shipped (`ptrace_arch.h`), but `cpu_info.c`'s `__cpuid()`/`<cpuid.h>` use is a separate, still-open x86_64-only blocker this row covers. |
| Native multi-pass counter execution (`--passes=ipc,topdown,cache,software`, internal N-run loop, merged manifest/CSV) | 4.1 | Confirmed real pain: `workload/phoronix/run_test.sh` already launches the same command up to 8 times by hand to dodge multiplexing. Depends on the profile launcher (4.0) to define what a "pass" is. |
| Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for `--tree`/`--tree-open` | 4.2 | `ptrace` context-switches on every syscall entry/exit, which skews the very counters being measured for I/O-heavy or fork-heavy workloads — but this is a substantial new backend, correctly sequenced after the cheaper wins above. |
| Collector-plugin architecture (wspy core / perf stat / trace-cmd / GPU tools behind one manifest+normalization path) | design decision in 4.0, implementation 4.2+ | **Gap in the prior draft** — "major idea H" had no phase. The *decision* (does the manifest schema assume one collector or many?) needs to be made while designing the 4.0 manifest, even though building out non-wspy collectors is a 4.2+ effort — retrofitting pluggability after the schema is locked is expensive. |

### Publishing, reporting, UI
| Idea | Phase | Why |
| --- | --- | --- |
| Shared plotting templates (replace per-suite gnuplot scripts with one normalized-schema pipeline) | 4.1 | **Re-phased (2026-07-09)** — was tagged 4.0, but its own rationale ("generate from the normalized schema once it exists") depends on the canonical metrics schema/store, which the Reproducibility track tags 4.1–4.2. Moved to 4.1 so the phase tag matches its actual dependency; `workload/phoronix/gnuplot.sh` stays suite-specific until then. |
| Summary table generator (min/max/median/mean/stddev/outlier flags) from indexed data | 4.1 | Depends on the run index (4.0) and ideally the stats work (4.1) landing first. |
| Publish-ready data export format | 4.1 | Same dependency; export layer on top of the normalized store. |
| HTML report bundle (summary, bottlenecks, tree, top counters, links to raw artifacts) + compare view | 4.1 | Depends on the report data existing in normalized form. |
| Historical run index browser/search | 4.1 | Thin UI on top of the run index. |
| Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates) | 4.2 | **Gap in the prior draft** — "major idea C" (remove manual synthesis for the website) was never assigned a phase despite being one of the two headline goals in "Success criteria." Needs the summary generator + HTML bundle (4.1) as inputs. |
| Characterization badges + similarity panels in reports | 4.2 | Depends on clustering (4.2) existing. |
| Interactive tree/timeline drill-down, GPU phase overlays | 4.2 | Builds on `proctree` exports (4.1) and phase segmentation (4.0/4.2). |
| Optional dashboard backend (e.g. Grafana) for exploratory slicing | 4.3 | Explicitly optional/coexists with static-first publishing; don't block 4.0–4.2 on this decision. |
| Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions, resumable/selective re-execution) | 4.3 | Full version of the lightweight config-file execution already in 4.0; don't build both at once. |
| Optional deep trace analysis (Perfetto-compatible export of tree+topdown+interval timelines) | 4.3 | Advanced companion path for difficult workloads once the low-overhead tracing backend (4.2) exists to feed it. |
| Optional live TUI (run progress, interval metrics, throttling/skew warnings) | 4.3 | Nice-to-have; CLI-first model stays primary. |

### Testing and documentation
Shipped (2026-07-10): golden output-contract tests + capability-matrix smoke tests
(`tests/golden_output.sh`, `tests/capability_matrix.sh`, wired into `run_tests.sh`) — see `CLAUDE.md`'s
"Build & Test" section for what each covers. Building the golden CSV-header/column-count checks
surfaced five real, independent output-contract bugs that shipped alongside the tests themselves (all
pre-existing, none introduced by this pass): `--cache3` fataled the whole run instead of gracefully
skipping when `/sys/devices/amd_l3/type` was absent (contradicting this file's/`CLAUDE.md`'s own
documented behavior); `--dcache`/`--icache`/`--tlb` combined with the default `--ipc` produced a
duplicate `ipc` CSV column instead of their own (a stray `counter_mask` bit leaking into
`print_metrics()`'s dispatch chain); `--software --csv` was corrupted (the value row never checked
`csvflag`, so the human-readable multi-line dump landed mid-row); `--memory` measured its counters but
never printed them anywhere (an implemented `print_memory()` was simply never wired into the dispatch
chain); and `--topdown-frontend`/`--topdown-optlb --csv` produced malformed rows (missing trailing
commas fusing fields together) that also silently dropped every column under permission-denied
conditions, the same class of bug fixed for `--topdown-backend`'s premature/duplicate header branch.
The capability-matrix smoke tests separately caught `--tree-vmsize` crashing on any use (a long option
registered and documented in `--help` with no corresponding `case` in `parse_options()` at all).
Documented but intentionally *not* fixed in this pass: `--per-core` combined with any counter group
produces a CSV header with only the base/coverage columns while each per-core row still appends that
group's values — a real gap, but one needing `wspy.c`'s `aflag`/per-core setup-and-print flow
re-architected rather than a single `print_*()` fix (see `tests/capability_matrix.sh`'s
`per-core-topdown` bundle comment).

Shipped (2026-07-10): artifact contract doc + troubleshooting runbook (`doc/ARTIFACT_CONTRACT.md`,
linked from `README.md`'s "Other contents"). Documents the manifest/run-index/CSV/tree-file shapes,
the SemVer contract behind `MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION`, the
"degrade-don't-fail" pattern shared by `coverage.c`/`provenance.c`/GPU-flag handling, how
`wspy-validate`'s per-run gate, `counter_coverage`, and `wspy-ledger`'s suite-level status relate
without being interchangeable, and a symptom-first troubleshooting runbook (partial counter
coverage, `nmi_watchdog`, `--tree`'s `exit_status.known: false`, GPU-not-built warnings,
`--per-core`'s documented CSV column-count mismatch, `wspy-validate`/`wspy-ledger` false negatives).

| Idea | Phase | Why |
| --- | --- | --- |
| Schema compatibility/migration tests + reproducibility/idempotency tests | 4.1 | Depends on the schema versioning (4.0) having something to test compatibility against. |
| Profile cookbook + interpretation playbook (how to read confidence/phase/comparability/cluster output) | 4.1 | Write once the features it documents (confidence envelope, phases) exist in 4.0. |
| Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead guardrails | 4.2 | Needs deterministic micro-workloads and the stats/index infrastructure (4.1) to compare against. |
| Contributor guide for adding a collector/metric/schema bump safely | 4.2 | Write once the collector-plugin decision (Portability track) and schema versioning have real precedent to document. |

## Track deep-dives

### Zen5/IBS deep-dive
What appears confirmed from current Linux perf/PMU behavior for AMD Family 1Ah (Zen5):
1. Zen5-specific IBS load-latency filtering enables L3-miss-only filtering via a Zen5 feature check.
2. Generic `PERF_COUNT_HW_*` mapping on Family 1Ah still follows the Zen4 event-map path in current
   kernel PMU logic — there isn't yet a distinct "Zen5-only" generic hardware-event map.
3. IBS capability extensions (L3-miss-only, load-latency/fetch-latency filters, richer memory-source
   decoding) are the strongest near-term source of additional signal.
4. L3-miss-only filtering is documented to skew sampling-period behavior — runs using it need
   explicit annotation.
5. Zen5's topdown dispatch baseline shifted from Zen4's 6 slots/cycle to 8 — already implemented
   (`topdown.c`'s `CORE_AMD_ZEN`/`CORE_AMD_ZEN5` slot-multiplier branch), not a gap. But the finer
   per-scheduler breakdown events AMD introduced alongside that width change aren't in
   `amd_raw_events[]` yet: split ALU/AGU scheduler-stall counters, and op-cache/execution-queue
   events that would separate `Frontend Latency` from `Frontend Bandwidth` (today's
   `de_no_dispatch_per_slot.*` events only give the coarser no-ops-from-frontend/backend-stalls/
   smt-contention split). `IBS_LD_L1_DTLB_REFILL_LAT` specifically also isn't named anywhere in the
   IBS capability-probing rows above. Neither needs its own inventory row yet — both are candidate
   inputs for the Topdown deep-dive's "platform formula registry" item (below) once Zen5-specific
   formulas are actually versioned there.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Inventory rows: "Zen5 / IBS" track, the three remaining rows above (capability-driven IBS probing,
`ibs-basic`/`ibs-memory-deep` collection profiles, and sampling skew/quality annotations all shipped,
`ibs.c`).

### Topdown deep-dive
Advancements worth adopting, in priority order for `wspy` specifically:
1. Multiplex-aware confidence (`time_running/time_enabled`) — cheapest, most trust-building; do
   first.
2. Decomposition consistency/sanity checks — pairs with #1, still read-only.
3. Hierarchical (L1→L2→L3) parent/child schema with explicit raw-vs-contention-adjusted
   denominators — needed before drill-down reporting means anything.
4. SMT/contention-aware normalization — publish both denominators, document which one drives
   classification.
5. Phase-aware topdown (warmup/steady/degraded) — depends on interval phase segmentation.
6. Hybrid/heterogeneous core-class summaries — don't mix Atom+Core topdown into one headline number.
7. Cross-signal attribution (topdown + cache/TLB/IBS) — composite bottleneck rules over single-
   counter heuristics.
8. Platform formula registry — versioned event/formula mapping per CPU family/model, for
   auditability.

**MVP acceptance criteria** (unchanged from prior draft, still the right bar):
- ≥95% of topdown fields in standard profiles include confidence metadata.
- Reports clearly mark low-confidence topdown rows and avoid strong claims from them.
- One benchmark run demonstrates phase-specific topdown shifts in generated summary output.

**MVP execution order**: quality envelope + sanity checks → hierarchical schema + denominator/
formula metadata → phase segmentation + phase-level bottleneck summaries. This maps directly to the
4.0 → 4.1 → 4.2 phase tags in the inventory.

→ Inventory rows: "Topdown quality" track.

## Phased plan
Read literally from the inventory's `Phase` column — this section is the narrative summary, not a
separate list to keep in sync by hand.

### Phase 4.0 — foundation (greenfield, not incremental)
Goal: invent the manifest/index/schema that everything else depends on, fix the two confirmed real
bugs (`card1` hardcode, `ptrace` x86_64-only access), and land the cheapest trust-building wins
(topdown confidence envelope, IBS capability probing). Every "Run artifact foundation" row, most
"Portability" rows, the AMD GPU path-scan fix, and the topdown/IBS confidence work are tagged 4.0.

**This phase was originally scoped large** — roughly 25-30 inventory rows when first drafted. The
minimal foundation slice (6 rows) has since shipped in full, and roughly 8 4.0-tagged rows remain
open (see "Next up after the minimal slice" for the current priority order across all of them).

### Phase 4.1 — automation
Goal: turn the 4.0 foundation into less manual work — stats/confidence reporting, matrix sweeps, the
normalized metrics store, native multi-pass execution, `/proc` enrichment, HTML reports, ARM64
`cpu_info` fallback.

### Phase 4.2 — analysis
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and the eBPF/ftrace tracing backend.

### Phase 4.3 — platform
Goal: optional/heavier pieces that shouldn't block the rest — full config-first experiment
definitions, optional dashboard backend, deep trace analysis, live TUI, characterization drift
alerting.

### Minimal foundation slice (if 4.0 needs to ship narrower)
Six items unlock nearly everything else and are independently shippable in roughly this order:
1. ~~Run manifest (JSON) + SemVer schema version~~ — shipped (`manifest.c`)
2. ~~Run index generation~~ — shipped (`run_index.c`)
3. ~~Common workload wrapper / profile-driven launcher~~ — shipped (`wspy-run`)
4. ~~Counter capability discovery + coverage reporting~~ — shipped (`coverage.c`, `wspy --capabilities`)
5. ~~Topdown confidence envelope + sanity checks~~ — shipped (`topdown.c`'s `print_topdown()`, see "Topdown quality")
6. ~~`amd_sysfs.c` dynamic GPU path scan~~ — shipped (`amd_sysfs.c`'s `find_amd_drm_card()`; still
   the isolated one-file fix — `--gpu-device=<idx>` and full multi-GPU enumeration from the inventory
   row above remain open follow-ons, not required for this slice)

Everything else currently tagged 4.0 (validation checks, coverage ledger, IBS profiles, `ptrace`
macro extraction, plotting templates, golden tests) can slip to a 4.0.x follow-on without blocking
downstream phases, since nothing in 4.1+ depends on them specifically. (Validation checks, the
coverage ledger, the `ptrace` macro extraction, the IBS profiles, and the golden output-contract/
capability-matrix tests have all since shipped — see "Run artifact foundation"/"Portability and
robustness"/"Zen5 / IBS"/"Testing and documentation" above and the "Next up" list below.)

### Next up after the minimal slice
All six items from the minimal foundation slice are shipped (2026-07-08), plus environment/provenance
capture (2026-07-09, `provenance.c` — see "Reproducibility, comparability, statistics" above), opt-in
child exit status propagation (2026-07-09, `--exit-with-child` — see "Portability and robustness"
above), the `rusage` CSV/normal output mismatch fix (2026-07-09, `print_usage()` in `topdown.c` — see
"Process / `getrusage` / `/proc` telemetry" above), basic pre-publish validation/quality checks
(2026-07-09, `wspy-validate`/`validate.c`/`json_reader.c` — see "Run artifact foundation" above), and
the coverage ledger (2026-07-09, `wspy-ledger`/`ledger.c` — see "Run artifact foundation" above),
arch-neutral `ptrace` register-access macros (2026-07-09, `ptrace_arch.h` — see "Portability and
robustness" above), capability-driven IBS probing (2026-07-10, `ibs.c` — see "Zen5 / IBS" above), and
`ibs-basic`/`ibs-memory-deep` collection profiles plus sampling skew/quality annotations (2026-07-10,
`ibs.c`/`topdown.c`/`wspy.c`/`wspy-run` — see "Zen5 / IBS" above), run-index schema validation on
ingest (2026-07-10, `ledger.c` — see "Portability and robustness" above), and golden output-contract
tests + capability-matrix smoke tests (2026-07-10, `tests/golden_output.sh`/`tests/capability_matrix.sh`
— see "Testing and documentation" above, including the five output-contract bugs and one crash that
building them surfaced and fixed), and the artifact contract doc + troubleshooting runbook
(2026-07-10, `doc/ARTIFACT_CONTRACT.md` — see "Testing and documentation" above); all eleven were item
1 of this list at the time they shipped and are dropped from the ordering below per this file's own
"ideas already implemented are not listed" rule.
That leaves roughly 5 rows still tagged 4.0 across the inventory (one, "Shared plotting templates," was
just re-phased to 4.1 above since its own rationale depended on the 4.1–4.2 normalized schema — see
that row). The list below covers all of them except that one, grouped and ordered by priority (design
decisions first since they get more expensive to retrofit the longer they wait, heavier collection/
report-layer work last). All are already rows in the inventory above — this is a suggested ordering,
not a separate list to maintain by hand.
1. Collector-plugin architecture design decision (wspy core / perf stat / trace-cmd / GPU tools behind
   one manifest+normalization path) ("Portability and robustness" track) — only the *decision* (does
   the schema assume one collector or many?) is 4.0 work; implementation is 4.2+. Cheap to decide now,
   expensive to retrofit once more schema/normalization work (items above, plus 4.1's canonical
   metrics schema) is built on top of an unexamined assumption.
2. Counter-fit preflight ("will this profile multiplex heavily?" + suggested downgrades)
   ("Existing-capability extensions" track) — builds directly on availability/NMI-watchdog handling
   and `coverage.c` that already exist at runtime; this just surfaces the same fit information before
   a run instead of after.
3. Interval (`--interval`) → automatic phase-boundary detection (warmup/steady/degraded)
   ("Existing-capability extensions" track) — basic marker detection can land now and is a named
   prerequisite for phase-aware topdown (4.2) and phase-aware IBS.
4. `--gpu-device=<idx>` override + multi-GPU enumeration ("AMD GPU track") — isolated, self-contained
   follow-on to the `card1` path-scan fix already shipped; doesn't block or get blocked by anything
   else in this list.
5. Unified output layout (`suite/benchmark/run_id/{metrics.csv,summary.txt,process.tree.txt,
   plots/*.png,manifest.json}`) ("Run artifact foundation" track) — the largest remaining piece,
   sequenced last here: nothing else in this list depends on it, but 4.1's report/publishing work
   will, so it shouldn't slip past 4.0 entirely.

Also a real (pre-existing, not newly introduced) gap surfaced but deliberately not fixed while building
the tests above: `--per-core` combined with any counter group produces a CSV header with only the
base/coverage columns while each per-core row still appends that group's values. Needs `wspy.c`'s
`aflag`/per-core setup-and-print flow re-architected, not a single `print_*()` fix — not added as its
own numbered item above since it hasn't been triaged into a phase yet, but flagged here so it isn't
lost; see `tests/capability_matrix.sh`'s `per-core-topdown` bundle comment for the concrete symptom.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision — flag if
context has changed. Three of the six below have since been settled by shipped work (2026-07-09 pass)
and are marked **Resolved**; they're kept rather than deleted so the reasoning that led to what shipped
stays attached to it.

- **Publication automation first, or reproducibility/provenance first?**
  **Resolved (2026-07-09):** the recommended *capture* half shipped — `provenance.c` captures
  virtualization role, microcode, BIOS, governor, memory, and toolchain into the manifest/run-index.
  Publication automation itself hasn't started. The remaining half of this question (whether to build
  comparability *scoring* on top of that captured data before publication automation) is really a
  restatement of the next question below, not a separate open decision.
- **Is cross-machine comparability a hard requirement for the first round?**
  Still open. Recommendation unchanged: no. Provenance fields are captured (4.0, shipped), defer
  comparability *scoring* to 4.2. Scoring needs enough historical runs across machines to be
  meaningful, which doesn't exist yet even with capture shipped.
- **Should the website stay static-only, or add an interactive backend?**
  Still open — no publishing/report-layer work (HTML bundle, static-site pipeline) has landed yet, so
  nothing has changed since this was written. Recommendation unchanged: static-first through 4.2, keep
  an optional Grafana-style backend as a 4.3 nice-to-have. Non-goal: do not let the interactive-backend
  question block the 4.1/4.2 HTML report and static-site work.
- **Minimum metadata set for a run to be "publishable"?**
  **Resolved (2026-07-09):** every field the recommendation named is now actually captured —
  timestamps and full command line (`manifest.c`'s always-present fields), host CPU/GPU/kernel
  (`host` block: hostname, cpu_vendor/family/model, kernel_release), compiler/toolchain plus
  BIOS/power/governor/memory (`provenance.c`), `wspy_version`/`schema_version`, output file list, and
  a pass/fail validation result (`wspy-validate`). The one named field wspy itself doesn't populate is
  "benchmark name/suite" — that's out of wspy's scope by design; it belongs to the wrapper layer
  (`wspy-run` pass names, `workload/*/run_test.sh`), not the manifest. The optional-but-recommended /
  coverage-flag treatment for anything beyond this set is exactly what `counters_unavailable` and
  `provenance`'s per-field "unavailable-with-reason" already do, so that half of the recommendation is
  also in place.
- **Should `wspy` natively handle multi-pass execution?**
  Still open as a build decision, but its stated precondition is now met: the profile launcher
  (`wspy-run`) shipped, so "what a pass is" is now defined. The feature itself (`--passes=...`, an
  internal N-run loop, merged manifest/CSV) hasn't been built. Recommendation unchanged: yes, in 4.1 —
  see "Portability and robustness" in the inventory.
- **Is ARM64/AArch64 support a priority for 4.x?**
  **Resolved (2026-07-09):** the recommended mechanical prep shipped — `ptrace_arch.h` macro-abstracts
  the `ptrace` register access `ptrace_loop()` needs (see "Portability and robustness" above), with an
  `__aarch64__` branch that's unverified prep (modeled on documented ABI conventions, never built or
  run on real hardware) rather than a tested backend. Whether ARM64 support itself is a 4.x priority
  is still open; the recommendation is unchanged either way — defer the actual `cpu_info` fallback
  (`__cpuid()`/`<cpuid.h>` is still x86_64-only) and full ARM64 validation to 4.1+ unless a concrete
  ARM64 machine makes it urgent sooner.

## Success criteria for a 4.0 kickoff
- A newcomer can run one benchmark suite and produce publish-ready structured artifacts without
  editing scripts.
- A summary page can be regenerated from data only (no manual copy/paste).
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.

## External brainstorming references
- ReBench — reproducible experiment configuration, resumable execution, explicit benchmark
  parameter tracking: https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) — static-site publication for benchmark trends with an interactive
  frontend model: https://asv.readthedocs.io/en/stable/
- Grafana OSS — optional dashboard-based slicing/templating if the interactive-backend path is
  taken: https://grafana.com/oss/grafana/
- Perfetto — timeline/trace analysis and SQL-based trace queries, relevant to the optional deep
  trace analysis pipeline (4.3): https://perfetto.dev/docs/

Note: OpenBenchmarking.org returned HTTP 403 in the environment used for this research; not
reviewed.
