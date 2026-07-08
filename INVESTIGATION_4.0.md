# wspy Investigation 4.0

Date: 2026-07-08 (consolidated pass)
Status: Brainstorming only (no implementation yet)

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

- **`amd_sysfs.c` really does hardcode `card1`** — `amd_sysfs.c:25,31,48,67` all read
  `/sys/class/drm/card1/device/{gpu_busy_percent,gpu_metrics}` literally, no scan, no env override.
  Confirmed real; `--gpu-device=<idx>` (or an equivalent scan) is a legitimate 4.0 fix, not a
  hypothetical.
- **`ptrace_loop()` really is x86_64-only** — `topdown.c:449,454,456` reads `regs.orig_rax` and
  `regs.rsi` directly (raw `struct user_regs_struct` fields, no macro layer), and `cpu_info.c:33,61`
  call `__cpuid()` from `<cpuid.h>`. Both are genuine portability blockers for ARM64, not just style
  issues — there is currently zero abstraction to build on.
- **There is no manifest, no JSON output, and no schema anywhere in the codebase today**
  (`grep -rn manifest\|json` across all `.c`/`.h` returns nothing). Every "Phase 4.0" item involving
  a manifest, run index, or coverage ledger is greenfield infrastructure on a tool that currently
  only emits flat CSV/text to a file — there's no prior schema to be backward-compatible with yet.
  This matters for scoping: 4.0 isn't "extend the manifest," it's "invent the manifest," which is a
  bigger first slice than the word "foundation" suggests (see "Minimal foundation slice" below).
- **`--tree-open` already exists and works as described** — `wspy.c:106`, `topdown.c:455` gate
  `SYS_openat` capture behind the `tree_open` flag. The "convert it into higher-level insight" idea
  is additive, not speculative.
- **`getrusage` output is narrower than the doc previously implied, and CSV/normal modes disagree**
  — `topdown.c:909` `print_usage()` prints `nvcsw`, `nivcsw`, `inblock`, `oublock` in `PRINT_NORMAL`
  mode, but the `PRINT_CSV`/`PRINT_CSV_HEADER` cases only emit `elapsed,utime,stime` — none of the
  context-switch or block-I/O fields reach CSV output at all today. That's a real, narrow bug/gap
  worth fixing before adding new `rusage` fields (`ru_maxrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`)
  on top of an already-inconsistent base. See inventory row "Expand getrusage coverage."
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
| Idea | Phase | Why |
| --- | --- | --- |
| Run manifest (JSON) + SemVer schema version | 4.0 | Nothing machine-readable describes a run today; every downstream idea in this doc depends on this existing first. |
| Unified output layout (`suite/benchmark/run_id/{metrics.csv,summary.txt,process.tree.txt,plots/*.png,manifest.json}`) | 4.0 | cpu2017/phoronix/pbbsbench each invent their own file layout; publishing tools currently need suite-specific logic. |
| Common workload wrapper / profile-driven launcher (`quick`, `deep-cpu`, `deep-gpu`, `tree-heavy`, config-file execution) | 4.0 | Collapses the repeated long `wspy` command lines in every `workload/*/run_test.sh` into one reusable entry point; config-file form is the lightweight precursor to the full "Phase 4.3" experiment-definition system, not a separate thing. |
| Run index generation (CSV/JSONL append per run) | 4.0 | Lets summary pages and tooling query "all runs" without scraping directories. |
| Basic validation / quality checks (required files present, non-empty CSV, exit status, sanity ranges) pre-publish | 4.0 | The blog's "oops" post shows a bad config can look plausible and poison a whole result set; catch it at the artifact boundary. |
| Coverage ledger (workload status: done/skipped/unsupported/needs-tool-support) | 4.0 | Blog history shows recurring "what's still missing" tracking done manually; generate it from the run index instead. |
| Reproducibility bundle export (tarball: manifest + raw + derived per batch) | 4.1 | Depends on manifest/index existing; archival/re-analysis convenience, not foundational. |
| Traceability links (summary row → manifest → raw CSV → plots → tree artifacts) | 4.1 | Same dependency; fold into the report generator once there's a normalized index to link from. |

### Reproducibility, comparability, statistics
| Idea | Phase | Why |
| --- | --- | --- |
| Environment/provenance capture (host/guest role, kernel, microcode, BIOS/power, governor, memory profile, tool versions) | 4.0 | Cheap to capture into the manifest now; expensive to reconstruct retroactively later. |
| Counter capability discovery + "measured vs unavailable" coverage reporting | 4.0 | Needed before any cross-kernel/cross-vendor claim in a report can be trusted. |
| Repeatability policy + confidence metadata (mean, stddev, CV, CI) as default output | 4.1 | Depends on the run index (need multiple runs grouped) more than on the manifest alone. |
| Outlier/threshold engine (per-metric, global + suite-local) | 4.1 | Needs a populated index/history to threshold against; not meaningful on a single run. |
| Comparison matrix mode (sweep compiler/kernel/governor/SMT/VM-native) | 4.1 | Builds on the profile-driven launcher; a declarative sweep runner, not new collection logic. |
| Canonical metrics schema + normalized store (SQLite and/or Parquet) | 4.1–4.2 | **Gap in the prior draft** — "major idea B" (data lake) was never assigned a phase. Needed once the run index outgrows CSV/JSONL scanning; keep raw files, add normalized querying as a second layer. |
| Machine/environment comparability scoring | 4.2 | Depends on provenance capture (4.0) existing across enough runs to score against. |
| Baselines and regression/anomaly detection | 4.2 | Depends on the normalized store existing (needs to query history, not just the latest run). |
| Distribution-first reporting (quantiles, clustering) | 4.2 | Same dependency; aggregate means already exist, this is a report-layer addition. |

### Topdown quality
See "Topdown deep-dive" below for reasoning; MVP acceptance criteria unchanged from prior draft.
| Idea | Phase | Why |
| --- | --- | --- |
| Confidence envelope (`time_running/time_enabled` per metric) + low-confidence labels | 4.0 | Cheapest, highest-trust win — multiplexing already silently degrades topdown accuracy today with no signal to the reader. |
| Decomposition consistency/sanity checks | 4.0 | Pairs naturally with the confidence envelope; both are read-only checks on data already collected. |
| Hierarchical (parent→child) schema + explicit raw-vs-contention-adjusted denominators + formula/version metadata | 4.1 | Requires the manifest/normalized-schema work from 4.0 to have somewhere to live. |
| Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) | 4.1 | Depends on per-core collection (`--per-core`) plus the hierarchical schema. |
| Phase-aware topdown (warmup/steady/degraded segmentation, drift signal) | 4.2 | Depends on interval phase-segmentation (see "Existing capability extensions") landing first. |
| Composite attribution (topdown + cache/TLB/IBS signals) | 4.2 | Depends on both the topdown hierarchy and IBS memory-source classes existing. |

### Zen5 / IBS
See "Zen5/IBS deep-dive" below.
| Idea | Phase | Why |
| --- | --- | --- |
| Capability-driven IBS probing (discover `ibs_fetch`/`ibs_op` formats/caps at runtime) | 4.0 | Prerequisite for everything else in this track; avoids hardcoding assumptions that break on non-Zen5 kernels. |
| `ibs-basic` / `ibs-memory-deep` collection profiles | 4.0 | Thin layer on top of the capability probe and the profile-launcher work already in 4.0. |
| Sampling skew/quality annotations (`l3missonly`, `ldlat`, `fetchlat`, accepted-vs-filtered) | 4.0 | L3-miss-only filtering is known to skew sampling period; must be visible in output, not just in code comments. |
| Zen-family preset packs (`zen-portable`, `zen4plus-deep`) | 4.1 | Convenience layer once capability probing exists; not needed for correctness. |
| PMU-capability-aware comparability warnings | 4.1 | Depends on the general comparability-scoring work (4.2) or at minimum the provenance capture (4.0). |
| IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) | 4.2 | Depends on both IBS memory-source classes and the topdown hierarchy existing. |

### Process / `getrusage` / `/proc` telemetry
| Idea | Phase | Why |
| --- | --- | --- |
| Fix `rusage` CSV/normal output mismatch (`nvcsw`,`nivcsw`,`inblock`,`oublock` missing from CSV) | 4.0 | Real, narrow bug found this pass (see "Verified against current source"); fix before adding new fields on the same inconsistent base. |
| Expand `getrusage` coverage (`ru_maxrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`) with raw + normalized rates | 4.0 | Low-friction addition once the CSV/normal mismatch above is fixed, so new fields don't inherit the same bug. |
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
| Idea | Phase | Why |
| --- | --- | --- |
| Dynamic AMD GPU path scan (`/sys/class/drm/card*/device/vendor` == `0x1002`) + `--gpu-device=<idx>` + multi-GPU enumeration | 4.0 | Fixes a confirmed real bug (`amd_sysfs.c` hardcodes `card1`); single-GPU machines where the AMD card enumerates as `card0` silently get no sysfs metrics today. |
| ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) | 4.1 | Merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`) once each is trustworthy standalone. |
| Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory activity) | 4.1 | Reuses 4.0 foundation work rather than building a parallel GPU-only pipeline. |
| `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) | 4.2 | Heavier, optional trace-rich profile — same "default vs debug profile" pattern as IBS. |
| Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) | 4.2 | Depends on the fusion layer providing consistent per-metric data first. |
| GPU coverage ledger (backend/device-class support, caveats) | 4.2 | Same pattern as the CPU workload coverage ledger, extended once GPU runs feed the same index. |
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
| Idea | Phase | Why |
| --- | --- | --- |
| Manifest schema versioning (SemVer) + validation on ingest, warn on mismatched schemas | 4.0 | Needs to exist from the manifest's first line, not retrofitted later — this is why 4.0 is "invent," not "extend" (see verification notes). |
| Arch-neutral `ptrace` register-access macros (`PTRACE_SYSCALL_NUM(regs)` etc. behind `#ifdef __x86_64__`/`__aarch64__`) | 4.0 | Confirmed real blocker: `topdown.c` reads `regs.orig_rax`/`regs.rsi` with no abstraction today. Do the macro extraction even before an ARM64 backend exists — it's a mechanical refactor now, a risky one once more logic depends on the current shape. |
| Fallback CPU topology detection for non-x86_64 (`/proc/cpuinfo`, `/sys/devices/system/cpu`) | 4.1 | Actual ARM64 `cpu_info` support; depends on the macro extraction above landing cleanly first. |
| Native multi-pass counter execution (`--passes=ipc,topdown,cache,software`, internal N-run loop, merged manifest/CSV) | 4.1 | Confirmed real pain: `workload/phoronix/run_test.sh` already launches the same command up to 8 times by hand to dodge multiplexing. Depends on the profile launcher (4.0) to define what a "pass" is. |
| Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for `--tree`/`--tree-open` | 4.2 | `ptrace` context-switches on every syscall entry/exit, which skews the very counters being measured for I/O-heavy or fork-heavy workloads — but this is a substantial new backend, correctly sequenced after the cheaper wins above. |
| Collector-plugin architecture (wspy core / perf stat / trace-cmd / GPU tools behind one manifest+normalization path) | design decision in 4.0, implementation 4.2+ | **Gap in the prior draft** — "major idea H" had no phase. The *decision* (does the manifest schema assume one collector or many?) needs to be made while designing the 4.0 manifest, even though building out non-wspy collectors is a 4.2+ effort — retrofitting pluggability after the schema is locked is expensive. |

### Publishing, reporting, UI
| Idea | Phase | Why |
| --- | --- | --- |
| Shared plotting templates (replace per-suite gnuplot scripts with one normalized-schema pipeline) | 4.0 | `workload/phoronix/gnuplot.sh` is suite-specific today; keep gnuplot support, generate from the normalized schema once it exists. |
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
| Idea | Phase | Why |
| --- | --- | --- |
| Golden output-contract tests (CSV header/order, summary fragments, tree format) | 4.0 | Cheapest regression guard; `run_tests.sh` already checks CSV column order for existing fields — extend the same pattern as new fields land. |
| Capability-matrix smoke tests (CPU vendor/family × GPU build × key option bundles, graceful-degradation paths) | 4.0 | `run_tests.sh` already does a version of this (AMDGPU-build vs not); formalize it as new tracks add more axes. |
| Artifact contract doc + troubleshooting runbook | 4.0 | Needs to exist before external scripts start depending on the new manifest/layout shape. |
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

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Inventory rows: "Zen5 / IBS" track, all six rows above.

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

**This phase as currently scoped is still large** — roughly 25-30 inventory rows. If a narrower
first cut is needed, see "Minimal foundation slice" below.

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
1. Run manifest (JSON) + SemVer schema version
2. Run index generation
3. Common workload wrapper / profile-driven launcher
4. Counter capability discovery + coverage reporting
5. Topdown confidence envelope + sanity checks
6. `amd_sysfs.c` dynamic GPU path scan (isolated, one-file fix, no dependency on 1-5)

Everything else currently tagged 4.0 (validation checks, coverage ledger, IBS profiles, `ptrace`
macro extraction, plotting templates, golden tests) can slip to a 4.0.x follow-on without blocking
downstream phases, since nothing in 4.1+ depends on them specifically.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision — flag if
context has changed.

- **Publication automation first, or reproducibility/provenance first?**
  Recommendation: provenance first, but only the *capture* half (environment fields into the
  manifest), not the scoring/comparability half. Capturing provenance is nearly free once the
  manifest exists and is much cheaper to do at the source than to reconstruct later; scoring and
  publication automation both consume it, so it should land in 4.0 ahead of both.
- **Is cross-machine comparability a hard requirement for the first round?**
  Recommendation: no. Capture provenance fields in 4.0 (cheap), defer comparability *scoring* to
  4.2. Scoring needs enough historical runs across machines to be meaningful, which won't exist
  right after 4.0 ships.
- **Should the website stay static-only, or add an interactive backend?**
  Recommendation: static-first through 4.2, keep an optional Grafana-style backend as a 4.3
  nice-to-have. The static path directly serves the stated success criteria (regenerate a summary
  page from data only); an interactive backend is additive, not required to hit that bar.
  Non-goal: do not let the interactive-backend question block the 4.1/4.2 HTML report and
  static-site work.
- **Minimum metadata set for a run to be "publishable"?**
  Recommendation: benchmark name/suite, timestamps, host CPU/GPU/kernel, compiler/toolchain, wspy
  version + manifest schema version, full command line, output file list, and a pass/fail validation
  result. Anything environment-related beyond that (BIOS/power settings, microcode) is valuable but
  should not block publishability — treat it as an optional-but-recommended field with a coverage
  flag, mirroring the "measured vs unavailable" pattern already planned for counters.
- **Should `wspy` natively handle multi-pass execution?**
  Recommendation: yes, in 4.1, after the profile launcher (4.0) exists to define what a "pass" is.
  This is not speculative — `workload/phoronix/run_test.sh` already hand-rolls up to 8 sequential
  invocations of the same command today to work around multiplexing; native support directly
  replaces real, currently-duplicated logic.
- **Is ARM64/AArch64 support a priority for 4.x?**
  Recommendation: do the mechanical prep (macro-abstract the `ptrace` register access) in 4.0
  regardless of priority, since it's cheap now and expensive to retrofit later. Defer the actual
  `cpu_info` fallback and full ARM64 validation to 4.1+ unless there's a concrete ARM64 machine this
  tool needs to run on soon — without that, it's prep work, not a deliverable.

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
