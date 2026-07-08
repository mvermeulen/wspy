# wspy Investigation 4.0

Date: 2026-07-08
Status: Brainstorming only (no implementation yet)

## Purpose
This document captures major ideas for a future round of improvements focused on making benchmark collection, organization, and publication easier and more repeatable.

## How to use this document
- Treat this file as a brainstorming backlog, not a committed implementation plan.
- Use each thematic section for idea generation and context.
- Use "Candidate phased plan (discussion draft)" as the scheduling source of truth.
- Use "Topdown-first MVP (condensed)" when selecting a narrow first implementation slice.
- Use "Open questions for prioritization" to decide trade-offs before execution.

Primary context reviewed:
- https://mvermeulen.org/perf/
- https://mvermeulen.org/perf/workloads/cpu2017/
- https://mvermeulen.org/perf/workloads/phoronix/
- https://mvermeulen.org/perf/2024/02/10/adding-summary-statistics-for-all-benchmarks/
- https://mvermeulen.org/perf/tools/wspy/

Local workflow context reviewed:
- [workload/cpu2017/run_test.sh](workload/cpu2017/run_test.sh)
- [workload/cpu2017/run_all.sh](workload/cpu2017/run_all.sh)
- [workload/phoronix/run_test.sh](workload/phoronix/run_test.sh)
- [workload/phoronix/run_all.sh](workload/phoronix/run_all.sh)
- [workload/phoronix/gnuplot.sh](workload/phoronix/gnuplot.sh)
- [workload/pbbsbench/run.sh](workload/pbbsbench/run.sh)

## Current workflow observations
- The perf site already has rich benchmark coverage and useful summary tables, including cross-workload statistical summaries.
- Data generation appears script-driven but still requires substantial manual synthesis for web publication.
- Existing workload scripts repeat many similar wspy invocations and output patterns.
- Paths and machine assumptions are mostly hard-coded, which makes reruns on new machines or by new contributors harder.
- Result organization is per-workload directories with mixed artifacts (text, CSV, PNG, tree files), but no single run manifest that ties everything together.

## Blog-derived opportunities
Themes below come from reviewing blog entries across 2023-2024, including posts on automated tables, summary statistics, histograms, clustering, invalid-run cleanup, thresholding, compiler comparisons, virtualization comparisons, and kernel/driver refreshes.

1. Built-in run validity gates
- Motivation: The "oops" write-up shows how invalid benchmark configuration can look plausible and still poison large result sets.
- Improvement: add preflight and post-run validity checks with hard fail and a quarantine tag for suspect runs.
- Examples: expected output files present, non-empty CSV rows, command exit status, known placeholder/error signatures, and metric sanity ranges.

2. Repeatability and uncertainty as first-class outputs
- Motivation: Many comparisons rely on small deltas where run-to-run variance matters.
- Improvement: support repeat count policies and emit confidence metadata by default (mean, stddev, coefficient of variation, confidence interval).
- Result: easier "is this real" decisions for benchmark deltas.

3. Outlier and threshold framework
- Motivation: Existing threshold and histogram work indicates recurring need to identify unusual workloads quickly.
- Improvement: formal threshold policies per metric (global and suite-local) and automatic outlier flags in summaries.
- Result: automatic shortlist of workloads to investigate deeply.

4. Comparison matrix mode (factor sweeps)
- Motivation: Blog comparisons repeatedly vary one factor at a time (compiler, kernel, hypervisor, machine).
- Improvement: declarative "matrix" runs that vary dimensions such as compiler, flags, kernel, governor, SMT, VM/native.
- Result: fewer ad hoc scripts and consistent side-by-side reports.

5. Strong environment provenance and compatibility checks
- Motivation: kernel and driver changes, and virtualization contexts, strongly influence counters and throughput.
- Improvement: capture host/guest role, kernel, microcode, BIOS/power settings, cpufreq governor, memory profile, and tool versions; include comparability warnings when key fields differ.
- Result: safer cross-run and cross-machine interpretation.

6. Counter capability discovery and graceful degradation
- Motivation: posts around new perf counters and platform differences suggest brittle assumptions across kernels/CPUs.
- Improvement: probe available events at runtime, map to requested metric groups, and emit "measured vs unavailable" coverage in manifest and reports.
- Result: predictable behavior across Intel/AMD generations and kernel revisions.

7. GPU characterization track
- Motivation: multiple posts skipped GPU/graphics due tooling gaps.
- Improvement: define a staged GPU metrics profile (busy, clocks, power, temp, memory activity) with the same manifest/index pipeline as CPU runs.
- Result: closes a major coverage gap and enables mixed CPU/GPU workload studies.

8. Workload coverage ledger and backlog automation
- Motivation: many posts track growth from ~50 to 200+ workloads and "what is still missing."
- Improvement: maintain a generated coverage ledger per article/source with statuses: done, skipped, unsupported, needs tool support.
- Result: easier planning and transparent scope decisions.

9. Distribution-first reporting
- Motivation: histogram and clustering entries show that aggregate means are not enough.
- Improvement: generate per-metric distributions, quantiles, and cluster labels from the normalized index during report generation.
- Result: faster workload-family discovery and better drill-down targeting.

10. Traceability from headline tables to raw evidence
- Motivation: manual synthesis is currently required to connect summary statements back to raw artifacts.
- Improvement: each summary row should link to manifest, raw CSV, plots, and tree/trace artifacts.
- Result: publish-ready pages remain auditable and reproducible.

## Easy edits for near-term leverage
These are intentionally "small surface area" ideas that could reduce manual work quickly.

1. Standard run manifest per benchmark run
- Add one machine-readable metadata file per run (for example: JSON).
- Include: benchmark name, suite, timestamps, host CPU/GPU/kernel, compiler/config, wspy version, command lines, and output file list.
- Benefit: easier indexing, filtering, and future automation.

2. Unified output naming convention
- Standardize file names and folder shape across cpu2017, phoronix, pbbsbench.
- Example structure: suite/benchmark/run_id/{metrics.csv, summary.txt, process.tree.txt, plots/*.png, manifest.json}
- Benefit: scripts and publishing tools no longer need suite-specific assumptions.

3. Common workload wrapper
- Replace repeated long wspy command lines in suite scripts with one common helper that takes a profile name.
- Profiles can map to counter groups (for example: system, branch+software, topdown, cache+float, tree).
- Benefit: less duplication, lower drift risk, simpler updates when wspy options change.

4. Automatic run index generation
- After each run, append a normalized row into a top-level index file (CSV or JSONL).
- Include pointers to artifacts and key headline metrics (elapsed, IPC, topdown groups, etc.).
- Benefit: building summary pages from one index instead of scraping folders.

5. Data quality checks before publish
- Add lightweight validation rules (required files present, CSV columns expected, no empty result files, key metric ranges).
- Benefit: catches bad runs early, supports "bad data" tagging intentionally instead of accidentally.

6. Automatic summary table generator
- Add a script to regenerate "all benchmark statistics" tables directly from indexed data.
- Support min/max/median/mean/stddev and outlier flags.
- Benefit: aligns with existing site content and removes manual table assembly.

7. Reproducibility bundle export
- Produce a tarball containing manifest + raw outputs + derived summaries for each run batch.
- Benefit: easier archival and reproducible re-analysis.

## Major 4.x ideas (larger rounds)
This section collects broader architecture-level directions that can be staged incrementally.

### A. Experiment definition system (config-first)
Use a declarative config format (YAML/JSON) for suites, benchmarks, commands, repetitions, and collection profiles.
- Inspiration: ReBench goals around reproducibility and benchmark parameter documentation.
- Allows resumable runs and selective re-execution.
- Could eventually replace per-suite ad hoc shell logic.

### B. Canonical metrics schema and data lake
Define a single schema for metric fields and store normalized outputs in SQLite and/or Parquet.
- Keep raw files, but make normalized querying first-class.
- Enables fast cross-benchmark analysis and future dashboards.

### C. Automated publishing pipeline
Generate static site pages from normalized data and templates.
- Per-benchmark pages + suite summary pages + cross-suite analytics pages.
- Remove manual "synthesis" step currently needed to update website pages.

### D. Regression and anomaly analysis
Introduce baseline comparisons and trend detection across runs/machines.
- Detect statistically meaningful shifts (not just raw deltas).
- Label likely regressions and outliers automatically.

### E. Multi-machine and environment provenance
Treat environment capture as core data.
- Kernel, microcode, BIOS/power settings, governor, memory profile, compiler/toolchain.
- Store provenance with every run for trustworthy cross-machine comparisons.

### F. Visualization stack upgrade
Two directions are possible and can coexist:
- Static-first: richer generated plots and compact benchmark cards for the website.
- Interactive: optional dashboard backend (for example Grafana) for exploratory slicing and drill-down.

### G. Deeper trace analysis mode
Optional advanced analysis path for heavy traces.
- Integrate export and post-processing hooks for timeline tools such as Perfetto-compatible views.
- Focus on difficult workloads where process and scheduling behavior explains metric anomalies.

### H. Pluggable collectors
Move toward a collector plugin model (wspy core, perf stat, trace-cmd, GPU tools).
- Keep a common run manifest and normalization path regardless of collector mix.
- Enables incremental feature growth without script explosion.

## GPU-focused 4.x candidates (CUDA, ROCm, Vulkan)
The ideas below focus on raising GPU instrumentation to the same maturity level as current CPU analysis.

### 1. Cross-stack GPU run model (applies to all backends)
- Introduce a GPU run schema with common fields: device id, architecture, driver/runtime versions, clocks, power, memory usage/bandwidth, utilization, kernel/queue timing, and thermal state.
- Keep backend-specific raw fields, but always emit a normalized subset so CUDA/ROCm/Vulkan runs can be compared.
- Add explicit host-device correlation fields (pid/tid, workload phase id, timestamp domain and sync quality).

### 2. Time-synchronized CPU+GPU timeline
- Build a unified timeline with CPU counters, process tree events, and GPU queue/kernel events.
- Provide clock-domain alignment metadata and drift estimates.
- Support "phase marks" (startup, warmup, steady state, teardown) for clearer per-phase attribution.

### 3. Repeatability and noise controls for GPU runs
- Record warmup policy, persistence mode, power cap, fan profile, and clock lock state where available.
- Add checks for thermal throttling and dynamic clock instability; annotate runs as constrained/unstable when detected.
- Add configurable repetition with confidence output for GPU throughput and latency metrics.

### CUDA instrumentation candidates

### 4. CUPTI activity ingestion
- Capture CUDA runtime/driver API calls, kernel launches, memcpy/memset, stream synchronization, and occupancy-related metadata.
- Aggregate into run-level metrics: kernel time share, memcpy share, launch overhead, stream idle fraction.
- Export both event stream and summarized table for publication.

### 5. Nsight-compatible export path
- Offer optional export to formats that can be viewed or cross-checked with Nsight Systems/Compute workflows.
- Keep this as an analysis companion, not a required runtime dependency.

### 6. CUDA memory behavior characterization
- Track H2D/D2H transfer volume and rate, pinned vs pageable use, unified memory migrations/faults (where available), and overlap ratio (copy vs compute).
- Flag likely bottlenecks: transfer-bound, launch-bound, occupancy-limited, or sync-heavy.

### 7. Multi-GPU and MIG awareness
- Capture topology (NVLink/PCIe), visible device mapping, peer access status, and MIG partition info.
- Report per-device and aggregate metrics with placement-aware summaries.

### ROCm instrumentation candidates

### 8. ROCm SMI + sysfs fusion layer
- Merge existing amd_smi/sysfs views into one coherent metrics stream with source precedence and validity flags.
- Detect unavailable sensors/counters explicitly and annotate confidence per metric.

### 9. rocprof/roctracer integration profile
- Add optional detailed tracing profile for HIP kernels, memory copies, and runtime API activity.
- Aggregate wave-level/occupancy-style indicators where feasible into simple report cards.

### 10. ROCm counter-set presets by architecture
- Provide curated metric presets for common AMD GPU generations to avoid brittle hand selection.
- Emit "requested vs collected" coverage so missing counters are visible rather than silently dropped.

### 11. Queue and SDMA diagnostics
- Characterize compute queue utilization, SDMA transfer activity, and overlap between copy engines and compute.
- Add simple imbalance flags (for example, one queue saturated while others idle).

### Vulkan instrumentation candidates

### 12. Vulkan timeline and pipeline-stage profiling
- Capture queue submit/complete timing, command buffer durations, stage-level timing (where timestamp queries exist), and pipeline barrier/synchronization cost.
- Summarize frame and non-frame workloads separately.

### 13. Renderdoc/validation-layer assisted metadata mode
- Add optional capture hooks for shader/pipeline metadata snapshots in debug runs.
- Use this for classification and regression triage, not for every production benchmark run.

### 14. Vulkan memory residency and allocation analysis
- Track allocation classes, budget pressure, residency/migration pressure, and fragmentation indicators (where extensions expose data).
- Flag memory-pressure signatures that correlate with frame-time spikes or throughput drops.

### 15. Present/frame pacing analytics
- For graphics-style workloads, compute frame-time distributions (p50/p95/p99), jitter, and pacing anomalies.
- Keep separate from pure compute Vulkan runs to avoid misleading aggregates.

### Cross-platform analysis and reporting candidates

### 16. GPU bottleneck classifier
- Produce a first-pass classifier with confidence: compute-bound, memory-bandwidth-bound, latency-bound, transfer-bound, sync-bound, thermal/power constrained.
- Base classification on normalized metrics plus backend-specific hints.

### 17. Counterfactual comparison reports
- For matrix runs (driver/runtime/version/flags), generate "what changed" reports linking metric deltas to likely causes.
- Include guardrails for non-comparable runs (different clocks, thermals, power limits, or workload parameters).

### 18. Device comparability score
- Add a score indicating how fair two runs are to compare, using environment/provenance checks.
- Penalize mismatched power caps, memory clocks, thermal state, runtime version, and queueing model differences.

### 19. GPU coverage ledger
- Extend workload coverage tracking with GPU status: supported backend(s), tested device classes, missing instrumentation, and known caveats.
- This directly addresses gaps where GPU benchmarks are skipped due to tooling limitations.

### 20. Escalation path for deep dives
- Keep a lightweight default profile and a heavy debug profile (trace-rich) for suspicious workloads.
- Ensure both profiles share the same manifest and index pipeline so deep-dive findings can flow back into normal summaries.

## Zen5 counters and IBS enhancements
This section captures practical counter/tool updates based on currently visible Linux perf PMU behavior for AMD Family 1Ah (Zen5 generation).

### What appears confirmed now
1. Zen5-specific IBS behavior exists
- In AMD IBS handling, when load latency threshold filtering is enabled for IBS Op, Zen5 enables L3-miss-only filtering via a Zen5 feature check.

2. Generic perf hardware events are still effectively Zen4-style on Family 1Ah
- In current kernel PMU mapping logic, Family 1Ah generic PERF_COUNT_HW mapping follows the Zen4 event map path.
- Implication: there is not yet a widely exposed, separate generic "Zen5-only" PERF_COUNT_HW map in the same way users might expect.

3. IBS capability extensions remain the strongest source of additional signal
- L3-miss-only filters, load-latency and fetch-latency filters, and richer memory-source decoding are the most actionable near-term enhancements for deeper Zen5 analysis.

4. Sampling skew risk must be surfaced
- Tooling warns that L3-miss-only filtering can skew sampling period behavior, so runs using these filters should be explicitly annotated in outputs.

### Candidate wspy enhancements for Zen5/IBS
1. Capability-driven IBS configuration
- Discover supported `ibs_fetch`/`ibs_op` formats and caps at runtime.
- Enable features only when present, and emit requested-vs-collected coverage in manifest/output.

2. New IBS collection profiles
- `ibs-basic`: low overhead, stable defaults for broad workload sweeps.
- `ibs-memory-deep`: includes ldlat/fetchlat and memory-source oriented fields for bottleneck triage.

3. Sampling quality and skew annotations
- Record active filters (`l3missonly`, `ldlat`, `fetchlat`), accepted vs filtered samples, and confidence/skew notes.
- Mark runs as cautionary when filter-induced skew is likely.

4. Zen-family preset packs
- Add preset counter bundles for `zen-portable` and `zen4plus-deep` (usable for Zen4/Zen5 class systems).
- Keep raw events optional, with safe defaults to reduce user misconfiguration.

5. Memory-path bottleneck decomposition
- Use IBS-derived memory-source classes (local cache, local DRAM, remote cache/DRAM, IO-like) when available.
- Combine with existing topdown/cache metrics to classify memory stalls more reliably.

6. PMU version and branch facility awareness
- Capture perfmon version/counter count and branch facility availability in run metadata.
- Add comparability warnings when two runs differ in PMU capabilities.

### Delivery notes
- Prioritize capability-driven IBS probing, profile presets, and sampling quality/skew annotations first.
- Add PMU-capability-aware comparability scoring as a follow-on automation step.
- Rollout sequencing is consolidated in "Consolidated rollout map by track" below.

### Caveat tracking
- If/when upstream kernel/perf exposes additional Zen5-specific generic event mappings or new PMU caps, update presets and coverage logic without changing report schema.

## Topdown analysis advancements and 4.0 implications
Yes, there have been meaningful improvements in topdown methodology and tooling over the last few years. The practical impact for `wspy` is mostly around normalization quality, confidence reporting, and deeper stall attribution rather than just adding more raw counters.

### Key advancements worth adopting
1. Deeper hierarchical topdown breakdowns
- Modern workflows increasingly treat topdown as a hierarchy (L1 -> L2 -> L3 style decomposition) rather than a single four-bucket snapshot.
- Practical implication: preserve parent/child relationships in output so users can drill down from "backend bound" to memory/core/internal causes.

2. Multiplex-aware metric confidence
- Topdown often competes for limited counter slots; multiplexing can reduce reliability.
- Practical implication: emit per-metric confidence from `time_running/time_enabled` and mark low-confidence topdown values in summaries/plots.

3. SMT and contention-aware normalization
- Interpretation quality improves when normalization clearly distinguishes raw slot ratios from contention-adjusted ratios.
- Practical implication: always publish both denominators where available, and explain which one drives classification.

4. Interval and phase-aware topdown
- Teams now use phase segmentation (warmup, steady, degraded) to avoid misleading whole-run averages.
- Practical implication: compute topdown per phase and include phase drift indicators for long runs.

5. Hybrid/heterogeneous core awareness
- On mixed-core systems, aggregate topdown without core-type stratification can hide true bottlenecks.
- Practical implication: add core-class summaries and avoid mixing fundamentally different core types in one headline metric.

6. Cross-signal attribution (topdown + memory/source signals)
- Better tools combine topdown with cache/TLB/latency sampling (for example IBS-like sources) to explain "why" backend or frontend is high.
- Practical implication: promote composite bottleneck rules (for example backend-memory + LLC/TLB evidence) over single-counter heuristics.

7. Guardrails and conservation checks
- Mature pipelines verify that topdown components are internally consistent (for example sum checks near expected totals).
- Practical implication: add validation checks and emit warnings when decomposition consistency is poor.

8. Availability/fallback mapping by platform
- Event names and exact formulas vary across vendors/generations.
- Practical implication: maintain platform capability maps with explicit fallback behavior and "requested vs computed" coverage reporting.

### Candidate `wspy` enhancements from these advancements
1. Topdown confidence envelope
- Add confidence columns and quality flags for each topdown family output.

2. Hierarchical topdown schema
- Encode parent-child topdown fields in normalized output so report tools can render drill-down trees consistently.

3. Phase-aware topdown summaries
- Compute topdown for each detected phase; report phase transitions and dominant phase-specific bottlenecks.

4. Core-class topdown views
- Split topdown summaries by core class where applicable, then provide an explicit weighted aggregate.

5. Consistency validator
- Add checks for decomposition plausibility and flag suspect runs or formulas.

6. Platform formula registry
- Keep a versioned mapping of formulas/events used per CPU family/model so report interpretation is auditable.

### Rollout note
- Topdown sequencing is consolidated in "Consolidated rollout map by track" below.

### Topdown-first MVP (condensed)
If topdown is the first 4.0 priority, keep scope to three deliverables that improve trust and actionability quickly.

1. Topdown quality envelope (must-have)
- Emit per-metric confidence from counter multiplex coverage (`time_running/time_enabled`).
- Add low-confidence labels in CSV/report output.
- Add decomposition sanity checks (for example component-sum plausibility).

2. Hierarchical export schema (must-have)
- Publish parent-child topdown fields in normalized output.
- Include explicit denominator semantics (raw slots vs contention-adjusted, where applicable).
- Keep formulas/version in metadata for auditability.

3. Phase-aware attribution (should-have)
- Use interval data to split run into warmup/steady/degraded phases.
- Report dominant bottleneck per phase instead of run-only aggregate.
- Add phase drift signal for long runs.

### MVP acceptance criteria
- At least 95% of topdown fields in standard profiles include confidence metadata.
- Reports clearly mark low-confidence topdown rows and avoid strong claims from them.
- One benchmark run can show phase-specific topdown shifts in generated summary output.

### MVP execution order
1. Implement quality envelope + sanity checks.
2. Implement hierarchical schema + denominator/formula metadata.
3. Implement phase segmentation + phase-level bottleneck summaries.

## Process and `getrusage` telemetry candidates
Current `wspy` output already includes useful child-process accounting (`elapsed`, `utime`, `stime`, context switches, and block I/O ops). The following additions would improve diagnosis of memory pressure, scheduler delays, and I/O bottlenecks.

### A. Expand `getrusage` coverage (low friction)
1. Add remaining high-value `rusage` fields
- `ru_maxrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`.
- Keep raw counts and normalized rates (per second, per CPU-second).

2. Add optional full-compat mode
- Include less common fields (`ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`) for environments where they are meaningful.
- Default off in concise output, on in a verbose profile.

3. Clarify aggregation semantics in output
- Explicitly tag metrics as `RUSAGE_CHILDREN` aggregate values.
- Add notes for fields with caveats (for example `maxrss` interpretation across children).

### B. `/proc`-based process diagnostics
1. Process I/O bytes (not only block operation counts)
- From `/proc/<pid>/io`: `read_bytes`, `write_bytes`, `cancelled_write_bytes`, plus char I/O counters.
- Helps distinguish many small block ops from high-byte-throughput behavior.

2. Scheduler delay and runqueue wait
- From `/proc/<pid>/schedstat` (where available): run delay and timeslices.
- Useful to separate CPU saturation from waiting-to-run effects.

3. Memory footprint detail
- From `/proc/<pid>/status` or `smaps_rollup`: `VmRSS`, `VmHWM`, anon/file/shmem split.
- Adds context missing from fault counts alone.

4. Thread and fd/socket pressure indicators
- Thread count over time, open fd count, optional socket count snapshot.
- Useful for diagnosing scalability and descriptor-leak style regressions.

### C. cgroup and container-aware process context
1. cgroup identity and limits in manifest
- Capture cgroup path and key limits (`cpu.max`, `memory.max`, cpuset constraints).

2. cgroup throttling/pressure stats
- Include `cpu.stat` (`nr_throttled`, `throttled_usec`) and memory events where available.
- Important for fair cross-run comparisons in containerized environments.

### D. Tree/process-lifecycle enrichments
1. Per-process exit diagnostics
- Exit code/signal/core-dump flag summary for root and notable children.

2. Spawn/exit burst indicators
- Peak concurrent processes/threads and spawn rate windows.
- Helpful for interpreting short benchmark phases with high process churn.

3. Process role tagging (optional)
- Lightweight labeling of dominant children (for example compiler, linker, test worker) by `comm` pattern.
- Makes tree output more actionable for large workloads.

### Rollout note
- Process-telemetry sequencing is consolidated in "Consolidated rollout map by track" below.

## High-level application characterization and clustering
This section defines application characterization as a first-class output layer.

### Goal
Produce a concise, explainable "app character" summary from `wspy` outputs, then place each run into a neighborhood of similar workloads.

### Candidate characterization labels
1. Parallelism shape
- Single-thread dominant
- Modestly parallel
- Highly parallel
- Burst-parallel (short high-fanout phases)

2. Resource dominance
- Compute-bound
- Memory-bandwidth-bound
- Memory-latency-bound
- I/O-bound
- Scheduler/concurrency-bound
- GPU-bound / transfer-bound (when GPU telemetry exists)

3. Control-flow and locality style
- Branch-sensitive / speculation-sensitive
- Cache-friendly vs cache-thrashing
- Streaming vs pointer-chasing

4. Runtime stability
- Stable run profile
- Phase-shifting profile
- Environment-constrained profile (thermal/cgroup throttling)

### Explainable scoring model (recommended)
1. Archetype scorecard
- For each label family, compute normalized scores from a fixed feature set (CPU counters, topdown, faults, context switches, process churn, I/O bytes, optional GPU metrics).

2. Confidence and ambiguity reporting
- Emit confidence and top-2 alternatives rather than a single hard label.
- Flag low-confidence runs when signals conflict or data coverage is incomplete.

3. Feature attribution
- Include top contributing signals (for example: high backend-memory + high LLC miss + low IPC -> memory-bound tendency).
- Keep rationale short and deterministic for reproducibility.

### Clustering and similarity outputs
1. Cluster assignment and nearest-neighbor list
- Assign each run to a cluster and provide nearest known workloads with distance score.

2. Cluster profile cards
- Summarize cluster centroid characteristics (for example: "high branch miss + moderate IPC + low I/O").

3. Temporal drift detection
- Track when a workload moves clusters across versions/configs/machines.
- Treat large cluster movement as an investigation trigger.

4. Coverage-aware clustering
- Use capability masks so missing counters do not create misleading distances.
- Compare only on common feature subspace when data coverage differs.

### Candidate output format
1. Per-run character block
- `app_character`: top labels, confidence, dominant bottlenecks, stability notes.

2. Similarity block
- `cluster_id`, `cluster_confidence`, `nearest_runs[]`, `distance_metric`, `feature_coverage`.

3. Publish summary hooks
- Render compact "character badges" and "similar workloads" links on benchmark pages.

### Rollout note
- Characterization sequencing is consolidated in "Consolidated rollout map by track" below.

## User interface and workflow integration
Current usage is effective for power users but heavily script-driven (many direct command invocations and ad hoc plotting). These additions would improve usability without removing the CLI-first model.

### A. Unified run entry points
1. Profile-driven launcher
- Add one front-door command (or wrapper) that selects predefined profiles (`quick`, `deep-cpu`, `deep-gpu`, `tree-heavy`) instead of long option strings.

2. Config-file execution
- Support YAML/JSON run specs for benchmark command, profiles, intervals, outputs, and tags.
- This reduces script duplication and improves reproducibility.

3. Batch and matrix command UX
- Add explicit subcommands for parameter sweeps and benchmark batches with consistent output directories.

### B. Reporting and plotting UX
1. Standard plotting pipeline
- Replace per-suite hand-authored plot scripts with a shared plotting layer and templates.
- Keep gnuplot support, but generate plots from one normalized schema.

2. HTML report bundle
- Emit a self-contained report per run/batch with sections for summary, bottlenecks, process tree, top counters, and links to raw artifacts.

3. Characterization badges and similarity panels
- Show high-level labels (for example: memory-bound, highly parallel, scheduler-constrained) and nearest-neighbor workloads directly in reports.

4. Compare view
- Add side-by-side and delta views for two runs (or two matrices) with significance and comparability warnings.

### C. Interactive UX (optional but high impact)
1. Lightweight TUI dashboard
- Terminal dashboard for live run progress, interval metrics, and warnings (throttling, missing counters, skew).

2. Run index browser
- Search/filter/sort historical runs by workload, host, profile, bottleneck label, and cluster.

3. Tree and timeline navigation
- Interactive process-tree view with collapsible branches and drill-down to per-process stats.
- Optional timeline overlays for CPU/system/GPU phases.

### D. Operator ergonomics
1. Better defaults and guardrails
- Detect common misconfigurations and suggest fixes (permissions, unsupported counters, unavailable GPU paths).

2. Explainability-first warnings
- Warn with plain-language impact statements (for example: "L3-miss-only filtering can skew period; confidence reduced").

3. Stable artifact contract
- Keep file layout and naming stable, versioned, and machine-readable so external scripts and dashboards are resilient.

### Rollout note
- UI/workflow sequencing is consolidated in "Consolidated rollout map by track" below.

## Unexplored extensions of current wspy capabilities
The current roadmap is broad, but a few natural extensions of already-existing `wspy` features were not called out explicitly.

### 1. `--tree-open` syscall tracing -> file-I/O behavior analysis
- Today this flag can capture open-related activity, but roadmap items do not yet convert it into higher-level insights.
- Candidate 4.x extension: summarize hot paths, open-failure rates, startup open storms, and process-to-file access maps.
- UI/report hook: per-run "file-I/O topology" panel with top contributors.

### 2. `--per-core` mode -> imbalance and topology diagnostics
- Existing per-core collection can naturally power skew analysis.
- Candidate 4.x extension: identify core imbalance, hot-core concentration, and migrations between core classes.
- For hybrid systems, add class-level summaries (for example efficiency/performance core families).

### 3. Interval output (`--interval`) -> automatic phase segmentation
- Interval snapshots are available, but the roadmap does not explicitly add phase-boundary detection.
- Candidate 4.x extension: change-point detection that marks warmup, steady state, and degradation periods.
- Use phase marks to compute phase-specific bottlenecks instead of run-wide averages only.

### 4. `proctree` artifacts -> machine-readable exports and diffs
- Tree capture exists, but downstream usage is mostly text-centric.
- Candidate 4.x extension: export JSON/Graphviz plus run-to-run tree diff summaries.
- This can make process-lifecycle regressions easier to detect and publish.

### 5. Counter-fit preflight (existing counter groups + PMU limits)
- `wspy` already handles availability and NMI-watchdog constraints at runtime, but fit quality is not surfaced as a first-class planning step.
- Candidate 4.x extension: preflight "can this profile fit without heavy multiplexing" checks with suggested downgrades.
- Add a quality indicator for each run when multiplex pressure is high.

### 6. System metrics (`--system`) -> interface and saturation diagnostics
- Current system collection can be extended into interpretable saturation signals.
- Candidate 4.x extension: per-interface network attribution and CPU-time mix indicators (user/system/iowait/steal context where available).
- Report whether observed bottlenecks are likely application-local vs system-pressure-driven.

### Rollout note
- Gap-closure sequencing is consolidated in "Consolidated rollout map by track" below.

## Testing and documentation additions to consider
Current ideas include validation checks and schema notes, but the roadmap does not yet define a full quality strategy for tests/docs as first-class outputs.

### A. Testing improvements
1. Golden artifact tests for output contracts
- Keep small canonical fixtures for CSV headers/order, summary text fragments, and tree output format.
- Fail CI when output contracts change unintentionally.

2. Schema compatibility tests
- Validate manifest/index/schema versions with explicit backward-compatibility checks.
- Add migration tests whenever schema changes are introduced.

3. Capability-matrix smoke tests
- Run a compact matrix for CPU vendor/family modes, with and without GPU build, and key option bundles.
- Ensure graceful degradation behavior is tested (unsupported counters, missing GPU support, permissions).

4. Statistical/regression test harness
- Add deterministic micro-workloads to detect major drift in IPC/topdown/system metrics and confidence fields.
- Use tolerance bands rather than exact-value assertions for noisy counters.

5. Performance-overhead checks
- Track instrumentation overhead by profile (`quick`, `deep-cpu`, `deep-gpu`, tree-heavy) and set alert thresholds.
- Prevent automation features from silently doubling runtime cost.

6. Reproducibility tests
- Verify that the same run definition reproduces artifact layout, manifest completeness, and index rows.
- Add checks for rerun idempotency and resume behavior.

### B. Documentation improvements
1. Docs-as-contract approach
- Maintain one normative artifact contract doc (manifest fields, CSV column order, naming/layout, versioning policy).
- Link every report field back to source metric/formula definitions.

2. Profile cookbook
- Add practical profile recipes by goal (quick triage, topdown-first, memory deep-dive, process-tree forensic, GPU focused).
- Include expected runtime overhead and confidence caveats for each profile.

3. Interpretation playbook
- Document "how to read" topdown confidence, phase shifts, comparability warnings, and cluster labels.
- Include anti-pattern examples to avoid overinterpreting low-confidence data.

4. Troubleshooting runbook
- Common failures: permissions, perf_event settings, missing PMU events, GPU sensor availability, counter multiplex pressure.
- Include diagnosis commands and recommended fallback profiles.

5. Contributor guide for collectors and schemas
- Define how to add a metric/collector safely, required tests, schema bump rules, and deprecation policy.
- Include checklist for report/backward-compat updates.

### Rollout note
- Testing/docs sequencing is consolidated in "Consolidated rollout map by track" below.

## Consolidated rollout map by track
Use this compact cross-track map for sequencing decisions. The full phase inventory remains in "Candidate phased plan (discussion draft)".

| Track | Phase 4.0 focus | Phase 4.1 focus | Phase 4.2 focus | Phase 4.3 focus |
| --- | --- | --- | --- | --- |
| Zen5/IBS | Capability probe, IBS profiles, skew metadata | Presets and PMU-aware comparability | Memory-path attribution refinement | Maintain with new PMU capabilities |
| Topdown | Confidence envelope and consistency checks | Hierarchical schema and core-class views | Phase-aware drift and composite attribution | Fold into broader platform analytics |
| Process telemetry | Expanded `getrusage` and aggregation semantics | `/proc` IO/sched/memory plus cgroup identity | Pressure-aware comparability and lifecycle role tags | Fold into dashboard/report automation |
| Characterization/clustering | Feature normalization prerequisites | Archetype scorecard and confidence | Clustering and coverage-aware distance | Drift alerts and publishing integration |
| UI/workflow | Profile launcher and standardized outputs | HTML report and run index browser | Interactive tree/timeline and similarity panels | Optional live TUI/dashboard integration |
| Existing capability extensions | Counter-fit preflight and phase markers | Per-core topology and `proctree` exports/diffs | `--tree-open` file-I/O topology and system-pressure attribution | Fold into platform defaults |
| Testing/docs | Golden contract tests and troubleshooting docs | Schema compatibility tests and profile/interpretation guides | Statistical harness and contributor quality gates | Ongoing maintenance and release policy hardening |

## External brainstorming references
- ReBench documentation highlights reproducible experiment configuration, resumable execution, and explicit benchmark parameter tracking:
  - https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) demonstrates static-site publication for benchmark trends across versions with an interactive frontend model:
  - https://asv.readthedocs.io/en/stable/
- Grafana OSS suggests a path for optional dashboard-based slicing, templating, and observability-as-code practices:
  - https://grafana.com/oss/grafana/
- Perfetto docs suggest a route for deeper timeline/trace analysis and SQL-based trace analysis for advanced investigations:
  - https://perfetto.dev/docs/

Note: OpenBenchmarking content was attempted but returned HTTP 403 from this environment:
- https://openbenchmarking.org/

## Candidate phased plan (discussion draft)
This is the consolidated schedule view for 4.x. Track-local rollout notes above are context only; when priorities conflict, prefer this phased plan.

### Phase 4.0 (foundation)
- Run manifest
- Unified output layout
- Common workload wrapper
- Run index generation
- Basic validation checks
- Coverage report (done/skipped/unsupported)
- Counter capability probing and availability reporting
- Capability-driven IBS probing and requested-vs-collected coverage (Zen4/Zen5 aware)
- `ibs-basic` and `ibs-memory-deep` collection profiles
- Sampling quality/skew annotations for filtered IBS modes (for example `l3missonly`)
- Expanded `getrusage` coverage (`maxrss`, `minflt`, `majflt`, `nswap`) with normalized rates
- Profile-driven launcher/config-file execution to reduce script complexity
- Shared plotting templates generated from normalized run data
- Counter-fit preflight with multiplex-pressure quality annotations
- Basic interval phase-segmentation markers (warmup/steady/degrade)
- Multiplex-aware topdown confidence scoring and low-confidence labels
- Topdown decomposition consistency checks and fallback coverage reporting
- Topdown-first MVP step 1: quality envelope + sanity checks
- Golden output-contract tests and capability-matrix smoke tests
- Artifact contract documentation and troubleshooting runbook

### Phase 4.1 (automation)
- Summary table generator
- Cross-run aggregation scripts
- Publish-ready data export format
- Matrix run definitions for compiler/kernel/VM sweeps
- Outlier and threshold engine
- Zen-family preset bundles (`zen-portable`, `zen4plus-deep`) with profile selection hints
- PMU capability comparability checks in cross-run reports
- Process `/proc` enrichment (`io` bytes, `schedstat`, memory footprint) and cgroup identity capture
- Application archetype scorecard output (labels + confidence + feature attribution)
- HTML report bundle and compare view for run-to-run UX
- Historical run index browser/search view
- Per-core imbalance and core-class topology summaries
- `proctree` machine-readable export (`json`/`graphviz`) and basic tree-diff report
- Hierarchical topdown schema export with parent-child drill-down fields
- Core-class-aware topdown weighted aggregate views
- Topdown-first MVP step 2: hierarchical schema with denominator/formula metadata
- Schema compatibility/migration tests and reproducibility-idempotency tests
- Profile cookbook and interpretation playbook documentation

### Phase 4.2 (analysis)
- Baselines and regression detection
- Outlier diagnostics
- Machine provenance checks and comparability scoring
- Distribution and clustering report generation
- Repeatability/confidence reporting defaults
- Memory-path bottleneck classification from IBS + topdown/cache signals
- Process/cgroup pressure-aware comparability and throttling diagnostics
- Workload similarity clustering, nearest-neighbor reporting, and cluster profile cards
- Interactive process-tree/timeline drill-down views in analysis reports
- `--tree-open`-derived file-I/O topology analysis and hotspot ranking
- System-pressure attribution (application-local vs host/cgroup constrained)
- Phase-aware topdown drift analysis and phase-specific bottleneck reporting
- Composite topdown attribution rules (topdown + cache/TLB/IBS signals)
- Topdown-first MVP step 3: phase-aware bottleneck summaries and drift signal
- Statistical regression harness with tolerance bands and overhead guardrails
- Contributor quality guide for collector and schema changes

### Phase 4.3 (platform)
- Config-first experiment definitions
- Optional dashboard layer
- Optional deep trace analysis pipeline
- GPU characterization profile integration
- Characterization drift tracking and alerting across benchmark history
- Optional live TUI for active runs and warning surfacing

## Open questions for prioritization
- Should 4.0 prioritize publication automation first, or reproducibility/provenance first?
- Is cross-machine comparability a hard requirement for the first round?
- Should the website remain static-only, or include an optional interactive data backend?
- What is the minimum metadata set that must be present for every run to be considered publishable?

## Success criteria for a 4.0 kickoff
- A newcomer can run one benchmark suite and produce publish-ready structured artifacts without editing scripts.
- A summary page can be regenerated from data only (no manual copy/paste).
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.
