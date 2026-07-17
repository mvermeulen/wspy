# wspy Investigation 4.0

Date: 2026-07-11 (4.0 released as `v4.0` — see the GitHub release notes for the published version of
"What shipped in 4.0" below)
Status: **4.0 is complete and released.** The data/metadata foundation (manifest, run index,
validation, coverage, provenance, profile-driven launcher, unified output layout, output-contract
test suite) shipped and has been hand-tested: full counter matrix confirmed on real AMD/Intel
hardware, `workload/*/run_test.sh` run end to end against real suite installs, and the NMI-watchdog/
`preflight.c` downgrade-suggestion path exercised for real. A handful of hand-testing items
(real-hardware IBS, real-hardware GPU, `wspy-validate`/`wspy-ledger` at accumulated scale) weren't
covered this round; see "Known gaps carried into 4.1" — none block the 4.0 release.

## Purpose
This document captures ideas for a round of improvements focused on making benchmark collection,
organization, and publication easier and more repeatable.

## Success criteria for a 4.0 kickoff
4.0's bar, now that reporting is explicitly out of scope for it (see below):
- A newcomer can run one benchmark suite and produce publish-ready structured artifacts without
  editing scripts. **Met and validated** — real-install hand testing confirmed the three
  `workload/*/run_test.sh` scripts run end to end via `wspy-run --suite/--benchmark` against real
  suite installs (see "What shipped in 4.0").

Two criteria that were part of 4.0's original bar were **deliberately deferred to 4.1, not dropped —
both are now met:**
- A summary page can be regenerated from data only (no manual copy/paste). **Met** — `wspy-summary`/
  `summary.c` queries `wspy-store`'s normalized store directly and regenerates a
  min/max/mean/median/stddev/outlier-flag table on demand, no copy/paste involved.
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.
  **Met** — `wspy-summary --show-runs`/`--trace` ("Traceability links," 4.1) resolves a
  summary row's contributing `hostname:run_id` identities straight through to the manifest (command
  line + environment), raw CSV, tree artifact, and plots, via `store.c`'s already-recorded
  `manifest_path`/`output_path`/`tree_output_path` columns; `web/server.py`'s Store & Summary tab
  surfaces the same chain as real links wherever the artifacts live under its own `--output-root`.

Rationale (for deferring, at the time): building a minimal report/summary generator immediately, then
rebuilding it properly once the 4.1 normalized-store work (schema + indexed queries) landed, would have
been duplicated effort for no real benefit — nothing downstream depended on a 4.0-era stub existing
first. Better to do the reporting layer once, thoroughly, as 4.1 scoped it (canonical schema,
summary table generator, report studio, traceability links) — which is what happened: 4.0 shipped the
foundation those depend on (manifest/run-index/validation/coverage/provenance), and 4.1 turned it into
the actual page/row a person reads, closing both deferred criteria above.

## How to use this document
- "What shipped in 4.0" / "What shipped in 4.1" are pointer lists, not feature logs — `CLAUDE.md`
  documents each module's actual behavior in detail; `git log` has history. Don't restate mechanism
  here, link to it. A phase's own priority-list section (e.g. the old "4.1 priorities") is where that
  detail belongs *while the phase is in progress* — once every item in it ships, roll it up into a new
  "What shipped in 4.N" section in this same style and delete the numbered list, fixing any
  `4.N #<item>` cross-references elsewhere in this file to describe the shipped thing by name instead
  of by number (its number stops being a stable anchor once the list it indexed is gone).
- 4.0 and 4.1 are both done; open work now lives in "4.2 / 4.3 / 4.4 priorities" below, not in this
  document's own status section. "Known gaps carried into 4.2" lists the specific hand-testing
  coverage 4.0 shipped without — not a blocker list, just a pointer so it isn't silently forgotten.
- "4.2 / 4.3 / 4.4 priorities" are ordered backlogs, one per phase, grouped into dependency tiers
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
arch-neutral `ptrace` register access (`ptrace_arch.h` — both x86_64 and `__aarch64__` branches
are fully verified and validated on real hardware); run-index schema validation on ingest
(`wspy-ledger`); collector-plugin schema seam (`manifest.h`/`run_index.h`'s `collector` field,
default `"wspy"` — no non-wspy collector implementation exists yet, that's real 4.3+ scope).

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

## What shipped in 4.1
Grouped the same way as "What shipped in 4.0" above — a pointer list, not a feature log; the
individual numbered items this rolls up (canonical schema, summary generator, multi-pass execution,
the web launcher and every one of its report/export/traceability/provenance/job-queue pieces) lived
in `## 4.1 priorities` while in progress and are summarized here now that the phase is complete. See
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
`pending`/`running`/`done`/`failed` lifecycle by `wspy-queue` (new standalone CLI, headless, no
dependency on the web server) or created from the Run tab's "Queue instead of running it now"
checkbox; a job file can be copied to a second machine with wspy checked out and processed there.

Known follow-ups intentionally not part of any of the above, carried forward as their own backlog
items (see 4.2's Tier 10): collapsing `wspy-run`'s own builtin profiles onto native `--passes`
bin-packing (they still shell out to `wspy` N times); a job-browsing view in the web UI (a queued job
today is only visible via `wspy-queue list`/`show`) and sharing structured configuration provenance
with the job format; and giving the compare view its own curation/annotation layer.

## Known gaps carried into 4.2
4.0 shipped and its hand-testing pass confirmed the full counter matrix on real AMD/Intel hardware,
`workload/*/run_test.sh` end to end against real suite installs, and the NMI-watchdog/`preflight.c`
downgrade path. The following items from that same hand-testing pass weren't covered this round —
carried forward as follow-up validation, not release blockers:
- ~~Exercise `--ibs-basic`/`--ibs-memory-deep` against real `ibs_fetch`/`ibs_op` PMUs on Zen4/Zen5
  hardware~~ — addressed 2026-07-15 on real Zen5 (family 25 model 116) hardware. Surfaced a real bug:
  `ibs.c` derived IBS's MaxCnt from a sysfs `format` field named `"maxcnt"` that doesn't exist on real
  kernels (MaxCnt actually comes from `perf_event_attr.sample_period`, per `perf_ibs_init()` in
  `arch/x86/events/amd/ibs.c`), so every IBS counter had silently failed `perf_event_open()` with
  `-EINVAL` since the feature shipped — `test_ibs.c`'s synthetic-sysfs-only coverage never called
  `perf_event_open()` and so never caught it. Fixed (`sample_period` threaded through
  `ibs.h`/`ibs.c`/`cpu_info.h`/`topdown.c`); confirmed live: `ibs-basic` now measures 2/2 counters,
  `ibs-memory-deep` 3/3, with real nonzero `ibs_fetch`/`ibs_op` values, and `--interval` combined with
  `--ibs-basic` produces a genuine per-tick time series (not just an aggregate row) — mechanically it
  was never aggregate-only, `wspy-run`'s builtin `ibs-basic`/`ibs-memory-deep` profiles just never pass
  `--interval`. Also added a real-hardware IBS probe to the web launcher's "Check" button
  (`ibs_probes_for_request()`/`probe_ibs()` in `web/server.py`) so a run that would use IBS gets this
  same live perf_event_open() verification before launching, not just `--capabilities`' sysfs-presence
  check. `ibs-basic`/`ibs-memory-deep` (`wspy-run`) now always pass `--interval 1`, the web checklist's
  IBS row has its own optional `interval_secs` field, and `plot.c` gained `ibs`/`ibs-accepted-ratio`
  templates — confirmed live: both profiles render real gnuplot PNGs from genuine per-tick
  `ibs_fetch`/`ibs_op`/`ibs_op_accepted_ratio` time series on this hardware. Still not done: real
  filtering behavior (l3missonly/ldlat skew) untested against actual filtered vs. unfiltered data on
  this hardware — this session's runs used `ibs-memory-deep`'s defaults but didn't specifically compare
  filtered vs. unfiltered sample distributions.
- Exercise `--gpu-busy`/`--gpu-metrics`/`--gpu-smi`/`--gpu-device=<idx>` on an `AMDGPU=1` build
  against real AMD GPU hardware, ideally a multi-GPU host, to confirm device enumeration/selection
  and metric correctness beyond what `./run_tests.sh`'s ROCm-header-gated build check covers.
- Run `wspy-validate`/`wspy-ledger` against a real run-index file accumulated over many genuine runs
  (not `test_ledger.c`/`test_validate.c`'s small synthetic fixtures) to sanity-check behavior at
  realistic scale and with real-world messiness (interrupted runs, mixed schema versions over time).
- Run `--tree`/`--tree-cmdline`/`--tree-vmsize` against a genuinely fork-heavy real workload (e.g.
  `make -j`, a SPEC benchmark) beyond `run_tests.sh`'s synthetic ~2000-process stress test, to sanity
  check ptrace overhead and `proctree` reconstruction under realistic timing. (A real `deep-cpu,
  tree-heavy` phoronix/coremark run during 4.0's release testing recorded 494 fork events cleanly,
  a useful data point but not a substitute for a genuinely large fork-heavy workload. **Done**,
  2026-07-16: a `phoronix-test-suite batch-run build-gcc` run under `deep-cpu,tree-heavy` recorded
  155,780 fork events / 155,781 processes over 342s, reconstructed cleanly by `proctree` in well
  under a second — see item 14's Tier 3 entry below for the full writeup, including a small residual
  gap it turned up.)

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

→ Informs the 4.2 priority list's "Zen-family preset packs," "PMU-capability-aware comparability
warnings," and #27's IBS sampling-mode support (icache/TLB/dcache/L2/L3/branch rate estimates decoded
from real per-sample tag data, not just counting-mode sample counts); and the 4.3 list's "IBS-derived
memory-path bottleneck decomposition," which depends on #27 existing first.

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
  (item 5/6 above, and the 4.3 "Phase-aware topdown" priority-list entry).

→ Items 3-8 map to the 4.2 list's "Hierarchical topdown schema" and "Core-class-aware topdown," and
the 4.3 list's "Phase-aware topdown" and "Composite attribution."

### Preset / Configuration / Option hierarchy deep-dive
A three-level vocabulary for describing what wspy can be asked to do, surfaced while iterating the
4.1 web-interface mockup (2026-07-11) and worth fixing in writing now, before it only exists as
implicit UI logic — the goal is for the CLI, `wspy-run`, and the web UI to describe the same thing
the same way, rather than each inventing its own mental model.

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

Implications, both realized as shipped 4.1 features (see "What shipped in 4.1" below):
- **Web launcher:** read as *presets first* — named shortcuts that populate a configuration+option
  checklist — with a live indicator of whether the current selection still matches a named preset or
  has been customized (and therefore will run as separate `wspy` lines). The checklist itself, not the
  preset layer, is the actual list of configurations+options; presets are just quick starting points
  into it, matching this hierarchy exactly. `wspy-run`'s own profile launcher was the presets-only
  first cut of this — no checklist, no customization, just `wspy-run`'s existing named profiles — so
  the full web launcher inherits this framing directly rather than inventing a separate one.
- **Reports:** a report used to only record a flat command line and flag set. Recording the
  preset/configuration/option choice as structured data — not something re-derived by re-parsing
  argv — lets a report say "this was `deep-cpu`, with the TLB group swapped for L3" in the same
  vocabulary the launcher uses, and lets a "customize & run again" action restore exactly that state
  into the launcher rather than starting from scratch. This was a real `manifest.h`/`run_index.h`
  schema change (structured configuration provenance), not only a UI one.

Cross-cutting goal, not yet committed to: the same preset/configuration/option vocabulary should
eventually describe `wspy`'s own CLI options (today an unstructured flat flag list) and `wspy-run`'s
profile format (today hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in `load_builtin_profile()`), not
just the web UI. Nothing here commits to that refactor — see the matching entry in "Open questions for
prioritization" — but this is the vocabulary to design against as any later CLI/`wspy-run`
restructuring proceeds, so it doesn't independently invent a different model for the same thing. There
is real leeway to adjust existing options/commands toward this if it produces a cleaner architecture —
this isn't a constraint to preserve backward compatibility around at all costs.

→ Realized in 4.1: the thin end-to-end launcher/report slice, the `wspy-run` profile launcher, the
general web launcher, structured configuration provenance, and browse-reports/customize-and-rerun
(see "What shipped in 4.1" below). Also background for any future `wspy-run` profile-format refactor.

### Critical-path / synchronization-latency deep-dive
Motivation: hardware counters (topdown/IPC/cache/branch/TLB) characterize how efficiently the CPU
executed while it was running, but say nothing about time spent not running at all — blocked on I/O, a
lock, a child process, or deliberately sleeping. For a workload whose wall time is dominated by waiting
rather than by inefficient execution, no amount of counter analysis explains the bottleneck. The
tree-wide ptrace mechanism `--tree`/`--tree-open` already established (see `CLAUDE.md`'s "Child launch
protocol" and `topdown.c`'s `ptrace_loop()`) is positioned to fill this gap: single-stepping every
syscall entry/exit across the whole process tree already pays the cost of observing every syscall
boundary; the open design question is which of those boundaries are worth decoding and how to report on
them, not whether the mechanism can reach them.

Core mechanism insight: `ptrace_loop()`'s `WSTOPSIG(status) == (SIGTRAP | 0x80)` branch already fires
once per syscall entry and once per exit, and `elapsed` (the run-relative timestamp) is computed at
every stop already. The entry→exit delta for a matched syscall *is* that call's latency/blocking
duration, purely from correlating timestamps already being captured — no new instrumentation primitive
is needed, only (a) a small per-pid "syscall started at T" side table (same shape as the existing
`ptrace_pid_table` used for deferred exit blocks) and (b) per-syscall argument decoding where useful.

Candidate syscalls/signals, roughly in priority order:
1. `futex` — highest value. Uncontended pthread mutex/condvar fast paths never reach the kernel, so any
   observed futex call is itself a contention signal; decoding the low bits of the op argument
   (`FUTEX_WAIT*` vs `FUTEX_WAKE*`) separates blocking waits from wakeups, and entry→exit duration on a
   `FUTEX_WAIT*` is time lost to lock/condvar contention. Directly answers "what's the synchronization
   bottleneck" for multithreaded workloads. **Recommended first concrete step** — see the 4.3 Tier 8
   item below.
2. Blocking I/O (`read`/`pread64`/`recvfrom`/etc.) — entry→exit delta on a blocking fd separates "CPU
   busy" from "blocked on a pipe/socket/slow storage," invisible to a bandwidth counter (same byte
   count, 2µs vs. 200ms).
3. `connect` — entry→exit delta is literal connection-setup latency (DNS, RTT, remote backlog); doubles
   the file/network-provenance use case already noted for `--tree-open` (recording remote addresses
   touched) with an actual latency number, not just an occurrence.
4. `nanosleep`/`clock_nanosleep` — deliberate idle time; a large share of wall time here rules out a
   hardware explanation for a low-IPC interval outright.
5. `wait4`/`waitpid` on tree nodes that are themselves orchestrators (shell scripts, `make -j`, test
   harnesses) — time here is pure "blocked on a child," separating orchestration/serialization overhead
   from compute on the critical path.
6. `poll`/`ppoll`/`select`/`epoll_wait` — decoding the timeout argument distinguishes "waiting
   productively for an event" from a short-timeout spin/poll loop (a workload-tuning smell, not a
   hardware one).

Report-layer payoff — the reason this is worth doing at all: cross-referencing blocking-syscall time
against `phase.c`'s existing IPC-based warmup/steady/degraded segmentation. A degraded phase that
overlaps heavy futex-wait/read-wait time means the CPU had nothing to do (waiting, not stalling); a
degraded phase with no blocking-syscall activity at all is a genuine hardware stall worth chasing with
topdown/cache counters. These two situations currently look identical from counters alone — this is the
mechanism that would tell them apart, across the whole tree, not just one process (the actual advantage
over pointing `strace -T` at one pid by hand — nothing today coordinates N separate strace outputs into
one timeline).

Design forks to resolve per syscall, not once for the whole area:
- **Log-per-call vs. aggregate-per-pid.** Rare events (`connect`, `nanosleep`) are naturally one line
  per occurrence, same as `--tree-open`'s existing `open` lines. High-frequency events (`futex`,
  blocking `read`) need per-pid accumulation (count + total duration) flushed once at exit — same
  pattern as the existing `/proc/<pid>/stat` dump on the `exit` line — or the tree file grows to
  strace-scale and defeats the "lighter than strace" premise.
- **Argument decoding varies per syscall.** `ptrace_read_null_terminated_string()` (pathname) already
  exists; futex's op argument is a plain integer needing bitmask decoding; `poll`/`epoll_wait`'s timeout
  argument and `connect`'s `sockaddr` need a fixed-size remote-memory read (generalizing the existing
  `PEEKDATA`-loop into a bounded-length `ptrace_read_bytes()`).
- **Generalizing `tree_open`.** Worth eventually becoming a small table (same shape as
  `multipass_group_names[]`/`web/server.py`'s `COLUMN_TO_GROUP`) mapping syscall name → number → decode
  function → log-vs-aggregate mode, rather than each new syscall adding another hand-written `if` branch
  in `ptrace_loop()`.

Caveat: `ptrace` itself imposes a real stop-the-world cost on every syscall of the traced process, so
absolute latency numbers collected this way are inflated relative to an untraced run — the same
observer-effect concern already flagged in 4.3 Tier 6's "low-overhead tracing alternative to ptrace"
item (#18). The *relative* split (fraction of wall time in futex-wait vs. read-wait vs. on-CPU) should
stay informative even when absolute numbers are skewed, but this should be documented as an inherent
limitation of the ptrace-based mechanism, not treated as clean latency data.

→ Feeds the 4.3 priority list's new Tier 8 below; the "no blocking-syscall activity" vs. "heavy
blocking-syscall activity" split is also a direct input to Tier 2's "Composite attribution" (topdown +
cache/TLB/IBS signals) — this deep-dive proposes blocking-syscall time as a fourth signal alongside
those three, not a separate report.

## 4.2 priorities
Goal: everything originally scoped for 4.1 beyond Tier 1 and Tier 2 (both shipped as the normalized
store/reporting layer and the web interface — see "What shipped in 4.1"): the stats/confidence layer,
topdown/IBS refinement, `/proc`/tree enrichment, GPU fusion, characterization prerequisites,
portability, and testing/docs cleanups.
Ordered in dependency tiers; items within a tier are independently startable.

**Tier 1 — stats/confidence layer:**

1. Repeatability policy + confidence metadata (mean, stddev, CV, CI) as default output.
2. Outlier/threshold engine (per-metric, global + suite-local).
3. Comparison matrix mode (sweep compiler/kernel/governor/SMT/VM-native) — builds on the
   profile-driven launcher; a declarative sweep runner, not new collection logic.

**Tier 2 — topdown/IBS refinement:**

4. Hierarchical (parent→child) topdown schema + explicit raw-vs-contention-adjusted denominators +
   formula/version metadata.
5. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — depends on per-core
   collection (`--per-core`, shipped) plus #4.
6. Zen-family preset packs (`zen-portable`, `zen4plus-deep`) — convenience layer now that IBS
   capability probing exists.
7. PMU-capability-aware comparability warnings.

**Tier 3 — `/proc` and tree enrichment (independent, moderate value, low risk):**

8. `/proc/<pid>/io` byte counters (read/write/cancelled-write bytes).
9. `/proc/<pid>/schedstat` run-delay/timeslice capture.
10. Memory footprint detail (`VmRSS`/`VmHWM`/anon-file-shmem split via `/proc/<pid>/status` or
    `smaps_rollup`).
11. cgroup identity + limits in manifest, `cpu.stat` throttling stats — needed for fair comparison in
    containerized environments.
12. Per-core (`--per-core`) → imbalance/hot-core/migration diagnostics, core-class summaries.
13. `proctree` → JSON/Graphviz export + run-to-run tree diff.
14. `proctree`/tree-file robustness against out-of-order fork/exit ptrace events — **mostly fixed**,
    see commit `8271e55` ("topdown: fix ptrace_loop() double-continue race dropping fork events",
    2026-07-14). That commit landed two things: (a) a real bug fix — two `WIFSTOPPED` branches in
    `ptrace_loop()` each issued their own `ptrace(CONT/SYSCALL)` call and then fell through to the
    loop's unconditional second `CONT` at the bottom with no intervening `wait4()`, so under a burst
    of concurrent forks/exits the stray second call could race ahead and consume the tracee's next
    real stop (e.g. the very next `PTRACE_EVENT_FORK`) before the main loop ever logged it — a
    genuinely lost `fork` line, not just a misordered one; and (b) writer-side reordering tolerance —
    `ptrace_pid_table[]`, a small hash table keyed by pid that defers a not-yet-known pid's buffered
    "comm"/"cmdline"/"exit" block (built via `open_memstream()`) until its own "fork" line has been
    written, so the file's line order always has `fork` before `exit` for the same pid regardless of
    which ptrace stop the kernel delivers first — the fix landed in the writer (`topdown.c`), not the
    reader (`proctree.c`) as originally proposed below.
    Confirmed against a genuinely large real workload (`workload/phoronix/run_test.sh`'s
    `deep-cpu,tree-heavy` profile against `phoronix-test-suite batch-run build-gcc`, run
    2026-07-16, `sacramento` host): 155,780 fork events across a 342-second, 155,781-process run,
    reconstructed cleanly by `proctree` (0.46s, no `exit for unknown pid`/"unable to remove process"
    warnings) — a large step up from the 494-fork-event data point recorded above and a real answer
    to this item's own "beyond `run_tests.sh`'s synthetic ~2000-process stress test" ask. One small,
    *distinct* residual gap remains, discovered from this same run: 7 of the 155,781 processes
    (~0.0045%) got a plain `WIFEXITED` reap with **no** preceding `PTRACE_EVENT_EXIT` ptrace-stop at
    all (confirmed by diffing `process.tree.txt`'s `fork`-target pids against its `exit`-block pids —
    each of the 7 has a `fork` line and an `exited` line but no `comm`/`cmdline`/`exit`-stat block in
    between). This is a different mechanism than the reordering bug above — not a stop arriving in the
    wrong order, but a stop the kernel apparently never delivered before the process was reaped — so
    `8271e55`'s fix doesn't cover it, and it isn't yet understood whether it's a further ptrace corner
    case (e.g. a process that exits between its `SIGSTOP`-after-fork and the tracer re-enabling
    `PTRACE_O_TRACEEXIT` on it) or something else. Not urgent: `proctree.c` already degrades
    gracefully for it today (a pid whose "exit" block never arrives just keeps its zeroed
    `finish`/`cpu`/`vmsize`/`utime` fields and `??` in place of `comm` — no crash, no warning, verified
    against this same run's output), so this is tracked here as a known, now-quantified small
    imprecision rather than an open correctness bug requiring a code change.
    `run_tests.sh`'s 2000-process tree stress test previously intermittently reported `exit for
    unknown pid` (observed once during the `wspy-store` branch's testing, not reproducible across 3
    isolated reruns — load-dependent flake, not a deterministic failure); worth re-running a few times
    to confirm `8271e55` also cleared that flake, since it wasn't specifically re-verified there.

**Tier 4 — GPU track:**

15. ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) —
    merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`).
16. Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory
    activity) — reuses 4.0 foundation work rather than a parallel GPU-only pipeline.

**Tier 5 — characterization prerequisites:**

17. Feature normalization prerequisites (fixed feature set from counters/topdown/faults/context-
    switch/I-O) — needs 4.1's normalized store schema (`wspy-store`) to draw features from.
18. Archetype scorecard (parallelism shape, resource dominance, control-flow style, runtime
    stability) + confidence + top-2 alternatives.

**Tier 6 — portability: shipped, with two follow-up gaps below.**

19. ~~Fallback CPU topology detection for non-x86_64 (`/proc/cpuinfo`, `/sys/devices/system/cpu`) —
    actual ARM64 `cpu_info` support~~ — **shipped.** `cpu_info.c`'s `__cpuid()`/`<cpuid.h>` use is now
    guarded behind `#ifdef __x86_64__`, with a `/proc/cpuinfo`/`/sys/devices/system/cpu` fallback
    inventory path for everything else (vendor/family/model, core count, `armv8_pmuv3_*` PMU-cluster
    discovery for mixed big.LITTLE systems) and a topdown-equivalent decomposition wired through raw
    ARM PMU events in `topdown.c`'s `print_topdown()`/`print_branch()`/`print_l2cache()`/
    `print_memory()` (see `CLAUDE.md`'s `cpu_info.c`/`topdown.c` entries). `setup_counters()` also
    honors per-core `target_cpu` binding in `--per-core` mode so mixed-PMU clusters route raw events
    to the right core's PMU type. This is real ARM64 `cpu_info` support, not just the `ptrace`-level
    prep (`ptrace_arch.h`'s `__aarch64__` branch) that was shipped earlier.

    Both gaps found by code review (PMU counter chunking/bin-packing and topdown sanity-tolerance warning checks)
    have been fully addressed, and both topology and ptrace support have been validated on real ARM64 hardware.

**Tier 7 — testing/docs and small cleanups (track alongside the schema work above):**

20. Schema compatibility/migration tests + reproducibility/idempotency tests.
21. Profile cookbook + interpretation playbook (how to read confidence/phase/comparability/cluster
    output).
22. Reproducibility bundle export (tarball: manifest + raw + derived per batch).
23. Size `wspy-run`'s `--tree` pass timeout from an actual run-time estimate instead of a fixed 3600s
    constant (e.g. `phoronix-test-suite` reportedly has a run-time-estimate command) — today's
    constant is a blunt stand-in; the real constraint is capping process-record data volume for
    publishing, not workload runtime, so a per-workload estimate would size it more accurately than
    one constant across every suite.
24. Doc/version consistency check — an automated check (script, or an addition to `run_tests.sh`)
    that catches the class of drift found during the v4.0 release audit: `doc/ARTIFACT_CONTRACT.md`'s
    schema-version examples had silently fallen behind `MANIFEST_SCHEMA_VERSION`/
    `RUN_INDEX_SCHEMA_VERSION` (quoting 1.2.0 against an actual 1.3.0, plus an entirely undocumented
    `collector` field), and `README.md` was missing a whole tool's section (`wspy-validate` had been
    built and usable since 4.0 landed but had no README coverage until the v4.0 release audit added
    it). Concretely: grep-based checks that doc-quoted schema versions and the documented tool/flag
    list match the actual header constants and `Makefile` binary list, so this doesn't require a
    manual audit at every release again.
25. Release-prep checklist/script — capture the v4.0 release process (bump `WSPY_VERSION_MAJOR`/
    `MINOR`, grep for stale version-string references across docs, run the full test matrix including
    the `AMDGPU=1` variant, tag, label every merged PR since the last tag, draft release notes from
    the merged-PR list) as a repeatable script or documented checklist instead of redoing it by hand,
    since 4.1/4.2/4.3/4.4 will each need this same sequence again.

**Tier 8 (dropped):** was "Deeper Phoronix Test Suite awareness in the web UI" (item #26). Dropped,
not deferred: its "read a Phoronix benchmark article and inventory its benchmarks" sub-item turned out
to conflict with Phoronix's site use policy (scraping/parsing their articles), so that half is off the
table outright; the rest of the item's motivation — tracking which Phoronix tests have been run —
is now covered by `wspy-ledger`'s existing workload-coverage tracking (see `CLAUDE.md`'s `ledger.c`
entry), which lowers the value of building Phoronix-specific web UI on top of the general launcher
enough that the item isn't worth carrying forward.

**Tier 9 — AMD IBS sampling mode:**

27. **(Addendum — added after Tier 2/Tier 8 were already numbered, so appended out of sequence rather
    than renumbering #4-26; conceptually sits alongside #4-7's topdown/IBS refinement work.)** AMD IBS
    *sampling*-mode support: a genuinely new capability, not an extension of the counting-mode
    `ibs-basic`/`ibs-memory-deep` profiles shipped in 4.0/4.1 (see `ibs.c`'s entry in `CLAUDE.md` and
    the interval/plotting work landed alongside this item) — those open `ibs_fetch`/`ibs_op` as plain
    counting events and only ever `read()` an aggregate/per-tick sample *count*. Real IBS sampling means
    mmap'ing the perf ring buffer and requesting `PERF_SAMPLE_RAW` so each individual sample's tagged
    register data is available, not just a count of how many fired — nothing in wspy today reads a perf
    mmap ring buffer at all, every existing counter group (including IBS as currently implemented) is
    `read()`-based counting. Each `IbsOpData`/`IbsOpData2`/`IbsOpData3`/`IbsDcLinAd` (op samples) and
    `IbsFetchCtl`/`IbsFetchLinAd` (fetch samples) record carries tag bits this item would decode into
    per-tick rate estimates comparable to (but independently sourced from) the equivalent
    hardware-counter groups already reported: `DcMiss`/`L2Miss`/`NbIbsReqSrc` → dcache/L2/L3 miss and
    memory-source-of-fill rates (vs. today's `--dcache`/`--cache2`/`--cache3` groups);
    `DcL1TlbMiss`/`DcL2TlbMiss` → data-TLB miss rates (vs. `--tlb`); fetch-side `IcMiss`/`L1TlbMiss`/
    `L2TlbMiss` → icache/iTLB miss rates (vs. `--icache`/topdown-frontend's iTLB columns); `BrnMisp`
    (on branch ops) → branch misprediction rate (vs. `--branch`). Valuable specifically because it's a
    second, independently-sampled measurement of the same phenomena the counting-mode groups already
    report — a real disagreement between the two is itself a signal (PMU multiplexing skew, a
    counting-mode blind spot, or an IBS sampling-rate artifact) — not just a fifth way to get the same
    number. Format/sysfs-field discovery already exists (`ibs.c`'s `ibs_probe()`) and generalizes
    directly; the new work is the mmap/ring-buffer read path itself, per-sample record parsing, and the
    rate-aggregation/report layer built on top. Feeds 4.3 Tier 2's "IBS-derived memory-path bottleneck
    decomposition" (#7 below), which assumed this sampling capability already existed.

**Tier 10 — small follow-ups carried forward from 4.1 (see "What shipped in 4.1"):**

28. Collapse `wspy-run`'s builtin profiles (`deep-cpu` et al.) onto native `--passes` bin-packing.
    They still shell out to `wspy` once per pass today; 4.1's multi-pass execution item scoped this
    collapse as a documented follow-up, not part of that item.
29. Job-browsing view in the web UI. A queued job (`wspy-queue add`, or the Run tab's "Queue instead
    of running it now" checkbox) is visible today only via `wspy-queue list`/`show`, not from the web
    UI itself. Bundle in sharing structured configuration provenance with the job format
    (`web/joblib.py`'s job schema and `manifest.h`'s `configuration_provenance` are designed to be
    close in shape but aren't wired together yet).
30. Give the report compare view (`GET /compare`) its own curation/annotation layer. It's deliberately
    raw/filename-aligned today (comparing actual artifacts across runs, curated or not); annotating a
    comparison itself, or aligning curated block titles across the compared runs, is still open.

## 4.3 priorities
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and a lower-overhead tracing backend.

**Tier 1 — needs 4.1's normalized store/history:**

1. Baselines and regression/anomaly detection.
2. Machine/environment comparability scoring — depends on provenance capture (shipped, `provenance.c`)
   existing across enough runs to score against.
3. Distribution-first reporting (quantiles, clustering prep).
4. Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace
   only when data coverage differs).

**Tier 2 — topdown/attribution, needs 4.2's hierarchical schema + phase detection (shipped) + IBS:**

5. Phase-aware topdown (warmup/steady/degraded segmentation, drift signal).
6. Composite attribution (topdown + cache/TLB/IBS signals).
7. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) — needs 4.2 #27's
   IBS sampling-mode support first; today's counting-mode IBS has no per-sample tag data to decompose.

**Tier 3 — publishing/reporting expansion, needs 4.1's report studio:**

8. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1's per-run curation studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats (WordPress/HTML/Markdown,
   4.1) rather than inventing a fourth.
9. Characterization badges + similarity panels in reports — a new block type in 4.1's curation studio
   once 4.2 #18's archetype scorecard exists to draw a badge from, not a separate report surface.
10. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1's
    static inclusion-depth mechanism (none/summary/excerpt/full) for the tree/interval blocks
    specifically; that mechanism stays the right default for a published, non-interactive report even
    once this exists.

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
    depends on the fusion layer (4.2) providing consistent per-metric data first.
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
20. ~~Core/thread affinity control~~ — **shipped ahead of schedule** (ahead of the 4.3 cycle this
    tier belongs to; see `CLAUDE.md`'s `affinity.c`/`affinity.h` entry for the actual design). Landed
    as `--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|cpuset=<c0,c1,...>` (`wspy.c`),
    applied via `sched_setaffinity()` on the forked child before `execve` in `topdown.c`'s
    `launch_child()`: `all` (default, every CPU currently visible to this process), `thread=<id>`
    (that single logical CPU, letting a caller deliberately avoid its SMT sibling), `nosmt` (one
    primary/lowest-numbered SMT thread per core, across every core — the "turn off hyperthreading"
    preset), `domain=<id>` (every thread on one L3-sharing core-complex/CCD — e.g. picking Zen5's
    16 MiB-L3 complex vs. Zen5c's 8 MiB-L3 complex on a mixed part), `coretype=<id>` (every thread of
    one MIDR-distinct microarchitecture — e.g. a big.LITTLE ARM part's "big" Cortex-A7xx cores vs. its
    "little" Cortex-A5xx ones, added once a real such host — 8x Cortex-A720 + 4x Cortex-A520 sharing
    one combined 12 MiB L3, so `domain=<id>` alone can't separate them — came up), and
    `cpuset=<c0,c1,...>` (explicit enumerated core list/ranges — the general form the others are
    shorthand for). `affinity.c`'s own topology discovery (SMT sibling grouping via
    `topology/thread_siblings_list`, L3-domain grouping via `cache/index*/{level,shared_cpu_list,size}`,
    core-type grouping via each cpu's own `regs/identification/midr_el1` implementer+part fields)
    covers what this item flagged as the real prerequisite, kept in its own module rather than added
    to `cpu_info.c`'s `struct cpu_core_info` (a placement concern, not a counter/PMU one).
    `wspy --list-affinity` (no privileges needed, same standing as `--preflight`) discovers domain/
    thread/core-type ids up front — mirroring what `scripts/map_cpu_hierarchy.py` maps out for a
    human (including that script's own MIDR-based `decode_midr()`/capability-group logic for
    core-type detection), read directly from sysfs here since a real run can't shell out to a helper
    script — and is also folded into `--capabilities`' combined report. The resolved core list is
    recorded in `--manifest`/`--run-index`'s new `options.affinity`/`affinity` object
    (`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` `1.5.0` → `1.6.0`) so a run's placement is
    part of its provenance rather than only implicit in how it was launched. `wspy-run --affinity
    <spec>` and the web launcher's Run tab "CPU affinity" card (preset radios, including a core-type
    picker, plus a discovery-backed explicit-CPU checkbox list, `POST /api/discovery/affinity-topology`)
    both thread the same spec through to every pass alike; `wspy-queue add --affinity`/job files carry
    it too. Detecting x86 hybrid parts (Intel Atom+Core, already tracked as `CORE_INTEL_ATOM`/
    `CORE_INTEL_CORE` in `cpu_info.c`'s own per-core vendor field) as a `coretype=` grouping too is a
    natural follow-up, not implemented yet.

**Tier 7 — testing:**

21. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
22. Contributor guide for adding a collector/metric/schema bump safely.

**Tier 8 — critical-path / synchronization-latency instrumentation (new — see the "Critical-path /
synchronization-latency deep-dive" above for the full use-case breakdown):**

23. General syscall-level critical-path instrumentation: extend the `--tree`/`ptrace_loop()` mechanism
    already used for `--tree-open` (currently just logs occurrence of `SYS_openat`) to capture
    entry→exit *latency*, not just occurrence, for blocking/synchronization syscalls (`futex`, blocking
    `read`/`recvfrom`, `connect`, `nanosleep`, `wait4`, `poll`/`epoll_wait`). Goal is a per-tree-node
    "time spent blocked, and on what" breakdown correlatable with `phase.c`'s degraded-phase
    segmentation, to distinguish genuine hardware stalls from synchronization/I-O waits — currently
    indistinguishable from counters alone. Design forks (log-per-call vs. per-pid aggregate, per-syscall
    argument decoding, generalizing `tree_open` into a table-driven mechanism) and the ptrace
    observer-effect caveat are covered in the deep-dive above, not repeated here.
24. **Recommended first step:** futex-wait tracking specifically (deep-dive item 1) — highest
    standalone value (any observed futex call is itself a contention signal in glibc's
    fast-path-first mutex/condvar implementation) and the smallest slice to design end-to-end (one
    syscall, one op-argument decode, one per-pid aggregate) before generalizing the mechanism to other
    syscalls.

    **Concrete design (2026-07-16):**
    - **Prerequisite fix, uncovered by this design, not optional:** `ptrace_loop()`'s syscall
      entry/exit classification (`last_syscall`/`syscall_entry` in `topdown.c`) is currently
      loop-global — it compares each stop's syscall number against the *previous stop across the
      whole tree*, not the previous stop for that pid. Harmless for `--tree-open` today (a
      misclassification just prints at the wrong moment), but load-bearing for futex timing: two
      threads entering `futex` around the same time can flip a genuine second entry into a false
      "exit," corrupting the measured duration — exactly the concurrent-thread scenario
      futex-tracking exists to measure. Fix: move entry/exit state into the existing per-pid
      `ptrace_pid_table`/`struct ptrace_pid_entry` (new fields `in_syscall`, `current_syscall`,
      `syscall_entry_elapsed`), replacing the two loop-globals outright. Each tracee's own stop
      stream strictly alternates entry/exit (guaranteed by `PTRACE_O_TRACESYSGOOD` re-arming), so
      per-pid toggling is simply correct with no cross-pid interference — simpler than the
      number-comparison heuristic it replaces, not just more correct. Benefits every future
      syscall-latency flag under item 23, not just this one.
    - **Op decoding is free:** `futex(uaddr, futex_op, val, timeout, uaddr2, val3)` — `futex_op` is
      argument 2, the exact register `ptrace_arch.h`'s `PTRACE_SYSCALL_ARG2` already exposes for
      `openat`'s pathname (arg2 is arg2 regardless of type). No new arch macro needed — read it as an
      int, mask with `FUTEX_CMD_MASK` (`<linux/futex.h>`), and classify blocking ops:
      `FUTEX_WAIT`/`FUTEX_WAIT_BITSET`/`FUTEX_LOCK_PI`/`FUTEX_LOCK_PI2`/`FUTEX_WAIT_REQUEUE_PI`.
      `FUTEX_WAKE*`/`*REQUEUE*` (non-wait)/`FUTEX_UNLOCK_PI`/`FUTEX_TRYLOCK_PI` are non-blocking
      control/wake calls and don't count. A `FUTEX_WAIT` that returns instantly (value already
      changed, `EAGAIN`) still counts toward `count` and contributes ~0 duration — correct as-is, no
      special case needed.
    - **Aggregation:** two new fields on the same per-pid entry, `futex_wait_count`
      (`unsigned long long`) and `futex_wait_seconds` (`double`), incremented at the matching exit
      stop (`seconds += elapsed - syscall_entry_elapsed`) whenever the entry was classified as a wait.
    - **Reporting:** append `<time> <pid> futex <count> <total_wait_seconds>` to the deferred
      exit-block `open_memstream` already built at `PTRACE_EVENT_EXIT` (right after the existing
      `comm`/`cmdline`/`exit`-stat writes), only when `count > 0` — inherits the existing
      fork-before-exit ordering guarantees (`8271e55`) for free, and follows the file's established
      `<time> <pid> <event> [args...]` grammar. Since threads (`clone(CLONE_THREAD)`) already get
      their own `fork`/`exit` lines under the existing `PTRACE_O_TRACECLONE` setup, this naturally
      produces **per-thread**, not just per-process, wait time — the granularity lock-contention
      analysis actually needs.
    - **CLI flag:** `--tree-futex`, mirrors `--tree-open` exactly (`tree_futex = 1; trace_syscall =
      1;` in `parse_options()`). Inert without `--tree`, same as `--tree-open` today.
    - **Deliberate scope cut:** `proctree.c` doesn't recognize `futex` lines yet — like `open` today,
      it logs "unknown command" and skips them (documented forward-compat behavior, not a crash).
      Landing collection first and leaving `proctree`/report-side summarization as a fast-follow
      matches how `--tree-open` itself shipped ahead of its own 4.2 #11 file-I/O summary item.
    - **Validation:** no golden-output test is meaningful (durations are inherently
      non-deterministic) — validate with a small two-thread pthread-mutex-contention test program run
      under `wspy --tree --tree-futex`, cross-checked against `strace -f -T -e futex` as ground truth,
      plus a `capability_matrix.sh`-style smoke bundle (`--tree --tree-futex`, assert no
      fatal/crash) for regression coverage.

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

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision.

- **Should `wspy-run`'s builtin profiles be refactored to be declaratively defined (as
  configurations+options) instead of today's hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in
  `load_builtin_profile()`?** New, opened by the preset/configuration/option deep-dive above.
  Recommendation: not yet — let the web UI's preset/configuration/option model (shipped in 4.1)
  stabilize against real feedback first, then decide whether `wspy-run` itself should be rebuilt on the
  same vocabulary. Premature to commit to a CLI/`wspy-run` restructure before the vocabulary has been
  used for anything real; there's real leeway to make this change later if it produces a cleaner
  architecture, but no reason to rush it ahead of the UI work that motivated it.
- **Is cross-machine comparability a hard requirement for the first round?** Still open.
  Recommendation: no. Provenance fields are captured (4.0); defer comparability *scoring* to 4.3 —
  scoring needs enough historical runs across machines to be meaningful, which doesn't exist yet.
- **Should the website stay static-only, or add an interactive backend?** Still open — no
  publishing/report-layer work has landed yet. Recommendation: static-first through 4.3, keep an
  optional Grafana-style backend as a 4.4 nice-to-have. Non-goal: don't let the interactive-backend
  question block 4.1's web-interface work or 4.3's static-site work. See also 4.1's new
  "Deployment/hosting design note" item, which should produce a concrete answer for the
  input-form/report-browser layer specifically, ahead of this broader static-site question.
- **Should `wspy` natively handle multi-pass execution? — resolved.** Yes, shipped in 4.1
  (`wspy --passes=<list>`/`multipass.c`, see "What shipped in 4.1" above).
- **Is ARM64/AArch64 support a priority for 4.x? — resolved.** Yes, fallback CPU topology
  detection and register-access abstractions have been fully implemented and validated on real
  AArch64 hardware (including the `ptrace_arch.h` `__aarch64__` implementation). Both gaps found by
  code review (raw counter chunking/bin-packing and topdown sanity-tolerance warning checks) have
  been fully resolved, and all tests have been validated and verified on real ARM64 hardware.
- **Publication automation and reproducibility/provenance capture — resolved.** Provenance capture
  shipped (4.0); publication automation is exactly 4.1's work (see "What shipped in 4.1" above).
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
  trace analysis pipeline (4.4): https://perfetto.dev/docs/

Note: OpenBenchmarking.org returned HTTP 403 in the environment used for this research; not
reviewed.
