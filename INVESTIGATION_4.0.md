# wspy Investigation 4.0

Date: 2026-07-10 (scope pass — defer publishing/reporting to 4.1, done thoroughly there rather than
as a 4.0 stub; see "Success criteria for a 4.0 kickoff" and "What remains before 4.0 is complete")
Status: **4.0's data/metadata foundation is built (manifest, run index, validation, coverage,
provenance, profile-driven launcher, unified output layout, output-contract test suite). What's left
is hand-testing that foundation against real hardware/installs — see "What remains before 4.0 is
complete" and "Recommended hand testing before closing out 4.0."**

## Purpose
This document captures ideas for a round of improvements focused on making benchmark collection,
organization, and publication easier and more repeatable.

## Success criteria for a 4.0 kickoff
4.0's bar, now that reporting is explicitly out of scope for it (see below):
- A newcomer can run one benchmark suite and produce publish-ready structured artifacts without
  editing scripts. **Met** (see "What shipped in 4.0"), pending the real-install validation tracked
  in "What remains before 4.0 is complete."

Two criteria that were part of 4.0's original bar are **deliberately deferred to 4.1, not dropped**:
- A summary page can be regenerated from data only (no manual copy/paste).
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.

Rationale: building a minimal report/summary generator now, then rebuilding it properly once the 4.1
normalized-store work (schema + indexed queries) lands, is duplicated effort for no real benefit —
nothing downstream depends on a 4.0-era stub existing first. Better to do the reporting layer once,
thoroughly, as 4.1 Tier 1-2 already scopes it (canonical schema, summary table generator, HTML report,
traceability links). 4.0 ships the foundation those depend on (manifest/run-index/validation/coverage/
provenance); 4.1 turns it into the actual page/row a person reads.

## How to use this document
- "What shipped in 4.0" is a pointer list, not a feature log — `CLAUDE.md` documents each module's
  actual behavior in detail; `git log` has history. Don't restate mechanism here, link to it.
- "What remains before 4.0 is complete" is the current single source of truth for open 4.0 work.
  When something on that list ships, delete the line — don't leave superseded narrative behind it.
- "4.1 / 4.2 / 4.3 priorities" are ordered backlogs, one per phase, grouped into dependency tiers
  (earlier tiers unlock later ones within the same phase). This replaces the old per-track inventory
  tables — don't re-introduce a parallel table; add or reorder an item in these lists instead.
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, topdown). Each
  points back at the priority-list items it informs.
- "Open questions" carry a recommendation each; re-open one by editing its entry, not by appending
  new prose elsewhere in the file.

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
arch-neutral `ptrace` register access (`ptrace_arch.h` — x86_64 verified and in use, `__aarch64__`
branch is unverified prep, not a tested backend); run-index schema validation on ingest
(`wspy-ledger`); collector-plugin schema seam (`manifest.h`/`run_index.h`'s `collector` field,
default `"wspy"` — no non-wspy collector implementation exists yet, that's real 4.2+ scope).

**AMD GPU track:** dynamic GPU path scan (`amd_sysfs_scan_devices()`, replacing the old `card1`
hardcode); `--gpu-device=<idx>` override + full multi-GPU enumeration across both the sysfs and SMI
backends. CUDA/Vulkan instrumentation is **cut from this roadmap** (project scope decision,
2026-07-08) — this codebase has no NVIDIA/Vulkan code and builds against ROCm only; revisit only if
the project's mission changes to cross-vendor GPU profiling.

**Testing and documentation:** golden output-contract tests + capability-matrix smoke tests
(`tests/golden_output.sh`, `tests/capability_matrix.sh`) — building these surfaced and fixed five
independent, pre-existing output-contract bugs and one crash (see `CLAUDE.md`'s "Build & Test" for
specifics, not repeated here); `doc/ARTIFACT_CONTRACT.md` artifact-contract doc + troubleshooting
runbook. `--per-core` CSV column-count mismatch fixed: `wspy.c`'s `per_core_csv` re-architects the
aflag/csv print flow into one row per active core (a `core` column identifies which), so header and
row column counts now match like any other flag combination; `--per-core` combined with `--interval`
still keeps the old, separately-caused mismatch (`timer_callback()` never reads per-core counters —
see `wspy.c`'s `per_core_csv` comment and `doc/ARTIFACT_CONTRACT.md`'s CSV section).

## What remains before 4.0 is complete
1. **Validate criterion 1 against real installs.** "Newcomer can run one suite without editing
   scripts" is believed met (the three `workload/*` scripts now call `wspy-run --suite/--benchmark`),
   but has only been exercised with `sleep 1` stand-ins in this environment — `runcpu`,
   `phoronix-test-suite`, and pbbsbench's `runall` aren't installed here. See "Recommended hand
   testing" below — this is now the main remaining work before 4.0 ships.
2. **A 4.0 release manifest**, once 1 lands: a short, structured summary of what actually shipped —
   final schema versions, module list, known carried-forward gaps (including the reporting deferral
   to 4.1 above) — the way a run's own `--manifest` records what a `wspy` invocation did. Where it
   lives (a `CHANGELOG.md` entry, a dedicated `RELEASE_4.0.md`, or a section here) is undecided; "What
   shipped in 4.0" above is functioning as a running draft of it until then.

## Recommended hand testing before closing out 4.0
Everything below requires real hardware, root/perf access, or third-party installs this environment
doesn't have, so it hasn't been exercised beyond synthetic/unit-test coverage:
- Run the full counter matrix as root (or `CAP_SYS_PTRACE` + `perf_event_paranoid <= 1`) on real AMD
  Zen4 and Zen5 hardware and on Intel (including a hybrid Atom+Core part) to confirm full counter
  coverage rather than the permission-denied degradation this environment always sees.
- Run `workload/cpu2017/run_test.sh`, `workload/phoronix/run_test.sh`, and
  `workload/pbbsbench/run.sh` end to end against real `runcpu`/`phoronix-test-suite`/`runall`
  installs — confirm the unified output layout, `--run-index` accumulation, the `deep-cpu-intel`
  Intel branch, and the `--tree` pass's 3600s timeout all behave as intended at realistic (not
  `sleep 1`) runtimes and process-fork volumes.
- Exercise `--ibs-basic`/`--ibs-memory-deep` against real `ibs_fetch`/`ibs_op` PMUs on Zen4/Zen5
  hardware — `test_ibs.c` only drives `ibs_probe_at()` against a synthetic fake sysfs tree, never
  real IBS counters or real filtering behavior.
- Exercise `--gpu-busy`/`--gpu-metrics`/`--gpu-smi`/`--gpu-device=<idx>` on an `AMDGPU=1` build
  against real AMD GPU hardware, ideally a multi-GPU host, to confirm device enumeration/selection
  and metric correctness beyond what `./run_tests.sh`'s ROCm-header-gated build check covers.
- Toggle the NMI watchdog (`scripts/setup_perf.sh`) and request a counter combination that
  genuinely doesn't fit, to confirm `preflight.c`'s suggested downgrades are actually accurate and
  helpful in practice, not just synthetically triggered.
- Run `wspy-validate`/`wspy-ledger` against a real run-index file accumulated over many genuine runs
  (not `test_ledger.c`/`test_validate.c`'s small synthetic fixtures) to sanity-check behavior at
  realistic scale and with real-world messiness (interrupted runs, mixed schema versions over time).
- Run `--tree`/`--tree-cmdline`/`--tree-vmsize` against a genuinely fork-heavy real workload (e.g.
  `make -j`, a SPEC benchmark) beyond `run_tests.sh`'s synthetic ~2000-process stress test, to sanity
  check ptrace overhead and `proctree` reconstruction under realistic timing.

## Track deep-dives

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
   formula registry" (see Topdown deep-dive item 8) once Zen5-specific formulas are actually
   versioned there — no standalone backlog item yet.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Informs the 4.1 priority list's "Zen-family preset packs" and "PMU-capability-aware comparability
warnings," and the 4.2 list's "IBS-derived memory-path bottleneck decomposition."

### Topdown deep-dive
Advancements worth adopting, in priority order for `wspy` specifically:
1. ~~Multiplex-aware confidence~~ — shipped (see "What shipped in 4.0").
2. ~~Decomposition consistency/sanity checks~~ — shipped alongside #1.
3. Hierarchical (L1→L2→L3) parent/child schema with explicit raw-vs-contention-adjusted
   denominators — needed before drill-down reporting means anything.
4. SMT/contention-aware normalization — publish both denominators, document which one drives
   classification.
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
  (item 5/6 above, and the 4.2 "Phase-aware topdown" priority-list entry).

→ Items 3-8 map to the 4.1 list's "Hierarchical topdown schema" and "Core-class-aware topdown," and
the 4.2 list's "Phase-aware topdown" and "Composite attribution."

## 4.1 priorities
Goal: turn the 4.0 foundation into less manual work — normalized data, stats/confidence reporting,
a real report generator, native multi-pass execution, `/proc` enrichment, and a multiplexed-counter
scaling correctness fix. Ordered in dependency tiers; items within a tier are independently startable.

**Tier 1 — foundational, unlocks most of the rest of 4.1:**
1. Canonical metrics schema + normalized store (SQLite and/or Parquet) — almost everything below
   (stats, comparison matrix, HTML report, summary generator, feature normalization) wants queryable
   data instead of re-scanning CSV/JSONL by hand. Keep raw files; add this as a second, derived layer.
2. Summary table generator (min/max/median/mean/stddev/outlier flags) from indexed data — closes the
   "summary page regenerated from data only" criterion deferred from 4.0 (see "Success criteria for a
   4.0 kickoff"); can start against the run index directly and layer onto #1 once it exists.
3. Native multi-pass counter execution (`--passes=ipc,topdown,cache,software`, internal N-run loop,
   merged manifest/CSV) — confirmed real pain: `workload/phoronix/run_test.sh` used to launch the
   same command up to 8 times by hand to dodge multiplexing; `wspy-run`'s profile launcher defines
   what a "pass" is, this makes it native instead of N separate processes.
4. **Correctness:** scale multiplexed counter values by `time_running`/`time_enabled` in
   `read_counters()` (`topdown.c`) — today only the confidence envelope accounts for multiplexing
   (`multiplex-aware confidence`, shipped in 4.0); the raw counter *value* itself is never
   extrapolated to the full interval, so any run where PMU slots are oversubscribed (more groups
   requested than `preflight.c`'s counter-fit budget, or the NMI watchdog eating a slot) silently
   undercounts absolute values, not just their confidence. A correctness fix, not new capability —
   worth doing before Tier 3's stats/comparison work builds on these numbers.

**Tier 2 — reporting/UI on top of Tier 1's data shapes:**
5. HTML report bundle (summary, bottlenecks, tree, top counters, links to raw artifacts) + compare
   view.
6. Publish-ready data export format.
7. Historical run index browser/search.
8. Shared plotting templates — replace `workload/phoronix/gnuplot.sh`'s per-suite script with one
   normalized-schema pipeline once #1 exists.
9. Traceability links (summary row → manifest → raw CSV → plots → tree artifacts) — closes the
   "every published row traces back to command/environment/artifacts" criterion deferred from 4.0
   (see "Success criteria for a 4.0 kickoff").

**Tier 3 — stats/confidence layer:**
10. Repeatability policy + confidence metadata (mean, stddev, CV, CI) as default output.
11. Outlier/threshold engine (per-metric, global + suite-local).
12. Comparison matrix mode (sweep compiler/kernel/governor/SMT/VM-native) — builds on the
    profile-driven launcher; a declarative sweep runner, not new collection logic.

**Tier 4 — topdown/IBS refinement:**
13. Hierarchical (parent→child) topdown schema + explicit raw-vs-contention-adjusted denominators +
    formula/version metadata.
14. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — depends on per-core
    collection (`--per-core`, shipped) plus #13.
15. Zen-family preset packs (`zen-portable`, `zen4plus-deep`) — convenience layer now that IBS
    capability probing exists.
16. PMU-capability-aware comparability warnings.

**Tier 5 — `/proc` and tree enrichment (independent, moderate value, low risk):**
17. `/proc/<pid>/io` byte counters (read/write/cancelled-write bytes).
18. `/proc/<pid>/schedstat` run-delay/timeslice capture.
19. Memory footprint detail (`VmRSS`/`VmHWM`/anon-file-shmem split via `/proc/<pid>/status` or
    `smaps_rollup`).
20. cgroup identity + limits in manifest, `cpu.stat` throttling stats — needed for fair comparison in
    containerized environments.
21. Per-core (`--per-core`) → imbalance/hot-core/migration diagnostics, core-class summaries.
22. `proctree` → JSON/Graphviz export + run-to-run tree diff.

**Tier 6 — GPU track:**
23. ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) —
    merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`).
24. Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory
    activity) — reuses 4.0 foundation work rather than a parallel GPU-only pipeline.

**Tier 7 — characterization prerequisites:**
25. Feature normalization prerequisites (fixed feature set from counters/topdown/faults/context-
    switch/I-O) — needs #1's normalized schema to draw features from.
26. Archetype scorecard (parallelism shape, resource dominance, control-flow style, runtime
    stability) + confidence + top-2 alternatives.

**Tier 8 — portability:**
27. Fallback CPU topology detection for non-x86_64 (`/proc/cpuinfo`, `/sys/devices/system/cpu`) —
    actual ARM64 `cpu_info` support; `cpu_info.c`'s `__cpuid()`/`<cpuid.h>` use is the remaining
    x86_64-only blocker (the `ptrace` side of ARM64 prep already shipped, see `ptrace_arch.h`).

**Tier 9 — testing/docs and small cleanups (track alongside the schema work above):**
28. Schema compatibility/migration tests + reproducibility/idempotency tests.
29. Profile cookbook + interpretation playbook (how to read confidence/phase/comparability/cluster
    output).
30. Reproducibility bundle export (tarball: manifest + raw + derived per batch).
31. Size `wspy-run`'s `--tree` pass timeout from an actual run-time estimate instead of a fixed 3600s
    constant (e.g. `phoronix-test-suite` reportedly has a run-time-estimate command) — today's
    constant is a blunt stand-in; the real constraint is capping process-record data volume for
    publishing, not workload runtime, so a per-workload estimate would size it more accurately than
    one constant across every suite.

## 4.2 priorities
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and a lower-overhead tracing backend.

**Tier 1 — needs 4.1's normalized store/history:**
1. Baselines and regression/anomaly detection.
2. Machine/environment comparability scoring — depends on provenance capture (shipped, `provenance.c`)
   existing across enough runs to score against.
3. Distribution-first reporting (quantiles, clustering prep).
4. Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace
   only when data coverage differs).

**Tier 2 — topdown/attribution, needs 4.1's hierarchical schema + phase detection (shipped) + IBS:**
5. Phase-aware topdown (warmup/steady/degraded segmentation, drift signal).
6. Composite attribution (topdown + cache/TLB/IBS signals).
7. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache).

**Tier 3 — publishing/reporting expansion, needs 4.1's HTML report:**
8. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates).
9. Characterization badges + similarity panels in reports.
10. Interactive tree/timeline drill-down, GPU phase overlays.

**Tier 4 — report-layer additions on data already collected in 4.0:**
11. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
    process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c:455`).
12. System (`--system`) → per-interface network attribution, user/system/iowait/steal mix,
    local-vs-system-pressure attribution.
13. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
    `comm`-pattern role tagging).

**Tier 5 — GPU deeper profiling:**
14. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
    heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
15. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) —
    depends on the fusion layer (4.1) providing consistent per-metric data first.
16. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`,
    extended once GPU runs feed the same index.
17. Fold into general environment-comparability scoring (power cap, memory clock, thermal state,
    driver version) — no separate "GPU comparability score" needed; one scoring mechanism, not two.

**Tier 6 — infra:**
18. Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for
    `--tree`/`--tree-open` — `ptrace` context-switches on every syscall entry/exit, which skews the
    very counters being measured for I/O-heavy or fork-heavy workloads.
19. Collector-plugin implementation (perf stat / trace-cmd / GPU tools as collectors behind the
    `collector` field, normalization path) — the schema seam shipped in 4.0; this is the actual
    implementation of wrapping a non-wspy collector.

**Tier 7 — testing:**
20. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's stats/index infrastructure.
21. Contributor guide for adding a collector/metric/schema bump safely.

## 4.3 priorities
Goal: optional/heavier pieces that shouldn't block the rest, in priority order:
1. Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions,
   resumable/selective re-execution) — full version of the lightweight config-file execution
   already in `wspy-run` (4.0); don't build both at once.
2. Optional deep trace analysis (Perfetto-compatible export of tree+topdown+interval timelines) —
   advanced companion path for difficult workloads, needs 4.2's lower-overhead tracing backend to
   feed it.
3. Temporal drift detection (cluster movement across versions/configs/machines) — needs 4.2's
   clustering plus enough history to detect movement; treat as an investigation trigger, not a
   standalone feature.
4. Optional dashboard backend (e.g. Grafana) for exploratory slicing — explicitly optional/coexists
   with static-first publishing; doesn't block 4.0-4.2.
5. Optional live TUI (run progress, interval metrics, throttling/skew warnings) — nice-to-have;
   CLI-first model stays primary.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision.

- **Is cross-machine comparability a hard requirement for the first round?** Still open.
  Recommendation: no. Provenance fields are captured (4.0); defer comparability *scoring* to 4.2 —
  scoring needs enough historical runs across machines to be meaningful, which doesn't exist yet.
- **Should the website stay static-only, or add an interactive backend?** Still open — no
  publishing/report-layer work has landed yet. Recommendation: static-first through 4.2, keep an
  optional Grafana-style backend as a 4.3 nice-to-have. Non-goal: don't let the interactive-backend
  question block the 4.1/4.2 HTML report and static-site work.
- **Should `wspy` natively handle multi-pass execution?** Its precondition is met (the profile
  launcher, `wspy-run`, defines what a "pass" is); the feature itself (`--passes=...`, internal N-run
  loop, merged manifest/CSV) hasn't been built. Recommendation: yes, in 4.1 (see Tier 1 above).
- **Is ARM64/AArch64 support a priority for 4.x?** Still open. Mechanical prep shipped
  (`ptrace_arch.h`'s `__aarch64__` branch, unverified/untested), but `cpu_info.c`'s `__cpuid()` use
  is still x86_64-only and full ARM64 validation hasn't happened. Recommendation unchanged: defer
  unless a concrete ARM64 machine makes it urgent sooner (4.1 Tier 8).
- **Publication automation and reproducibility/provenance capture — resolved.** Provenance capture
  shipped (4.0); publication automation is exactly the 4.1 Tier 1-2 work above.
- **Minimum metadata set for a run to be "publishable" — resolved.** Every field the original
  recommendation named is captured (timestamps, command line, host/CPU/GPU/kernel, provenance,
  schema version, output file list, `wspy-validate` pass/fail). "Benchmark name/suite" is
  intentionally out of `wspy`'s own scope — it's `wspy-run`/`workload/*`'s job, not the manifest's.

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
