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
  `summary.c` (4.1 Tier 1 item 2) queries `wspy-store`'s normalized store directly and regenerates a
  min/max/mean/median/stddev/outlier-flag table on demand, no copy/paste involved.
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.
  **Met** — `wspy-summary --show-runs`/`--trace` (4.1 Tier 2 item 14, "Traceability links") resolves a
  summary row's contributing `hostname:run_id` identities straight through to the manifest (command
  line + environment), raw CSV, tree artifact, and plots, via `store.c`'s already-recorded
  `manifest_path`/`output_path`/`tree_output_path` columns; `web/server.py`'s Store & Summary tab
  surfaces the same chain as real links wherever the artifacts live under its own `--output-root`.

Rationale (for deferring, at the time): building a minimal report/summary generator immediately, then
rebuilding it properly once the 4.1 normalized-store work (schema + indexed queries) landed, would have
been duplicated effort for no real benefit — nothing downstream depended on a 4.0-era stub existing
first. Better to do the reporting layer once, thoroughly, as 4.1 Tier 1-2 scoped it (canonical schema,
summary table generator, report studio, traceability links) — which is what happened: 4.0 shipped the
foundation those depend on (manifest/run-index/validation/coverage/provenance), and 4.1 turned it into
the actual page/row a person reads, closing both deferred criteria above.

## How to use this document
- "What shipped in 4.0" is a pointer list, not a feature log — `CLAUDE.md` documents each module's
  actual behavior in detail; `git log` has history. Don't restate mechanism here, link to it.
- 4.0 is done; open work now lives in "4.1 / 4.2 / 4.3 / 4.4 priorities" below, not in this document's own
  status section. "Known gaps carried into 4.1" lists the specific hand-testing coverage 4.0 shipped
  without — not a blocker list, just a pointer so it isn't silently forgotten.
- "4.1 / 4.2 / 4.3 / 4.4 priorities" are ordered backlogs, one per phase, grouped into dependency tiers
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

## Known gaps carried into 4.1
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
  check. Follow-up not yet done: `--interval` support isn't exposed on the `ibs-basic`/`ibs-memory-deep`
  profiles or the web checklist's IBS row, and `plot.c` has no `ibs_fetch`/`ibs_op` template yet — real
  filtering behavior (l3missonly/ldlat skew) also still untested against actual filtered vs. unfiltered
  data on this hardware.
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
  a useful data point but not a substitute for a genuinely large fork-heavy workload.)

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

→ Informs the 4.2 priority list's "Zen-family preset packs" and "PMU-capability-aware comparability
warnings," and the 4.3 list's "IBS-derived memory-path bottleneck decomposition."

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
fallback (4.1 item #9) is this rule, discovered bottom-up from the real constraints before being named
top-down here. Worth treating as the general rule going forward rather than a fact specific to
counters/tree/interval, since it will recur every time a new configuration or option is added.

Implications:
- **Web launcher (#9, with #7 as its first cut):** read as *presets first* — named shortcuts that
  populate a configuration+option checklist — with a live indicator of whether the current selection
  still matches a named preset or has been customized (and therefore will run as separate `wspy`
  lines). The checklist itself, not the preset layer, is the actual list of configurations+options;
  presets are just quick starting points into it, matching this hierarchy exactly. #7's `wspy-run`
  profile launcher is the presets-only slice of this — no checklist yet, no customization, just
  `wspy-run`'s existing named profiles — so it inherits this framing directly rather than inventing a
  separate one.
- **Reports (#16/#17, new):** a report today only records a flat command line and flag set. Recording
  the preset/configuration/option choice as structured data — not something re-derived by re-parsing
  argv — lets a report say "this was `deep-cpu`, with the TLB group swapped for L3" in the same
  vocabulary the launcher uses, and lets a "customize & run again" action restore exactly that state
  into the launcher rather than starting from scratch. This is a real `manifest.h`/`run_index.h`
  schema question, not only a UI one.

Cross-cutting goal, not yet committed to: the same preset/configuration/option vocabulary should
eventually describe `wspy`'s own CLI options (today an unstructured flat flag list) and `wspy-run`'s
profile format (today hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in `load_builtin_profile()`), not
just the web UI. Nothing here commits to that refactor — see the matching entry in "Open questions for
prioritization" — but this is the vocabulary to design against as 4.1's web work and any later CLI/
`wspy-run` restructuring both proceed, so they don't independently invent two different models for the
same thing. There is real leeway to adjust existing options/commands toward this if it produces a
cleaner architecture — this isn't a constraint to preserve backward compatibility around at all costs.

→ Informs 4.1's #6 (thin end-to-end slice), #7 (wspy-run profile launcher — presets only, no
checklist yet), #9 (the fully general web launcher — presets expanding to configurations+options),
#16 (structured configuration provenance), and #17 (report ↔ configuration linkage +
customize-and-rerun). Also
background for any future `wspy-run` profile-format refactor.

## 4.1 priorities
Goal: Tier 1 — normalized store, summary table generator, native multi-pass counter execution, and
the multiplexed-counter scaling correctness fix — is complete and shipped (see the items below). The
remaining focus of 4.1 is narrowed to a single track: the web interface layer, both input (forms that
construct and launch the same commands a user could type by hand) and output (a browsable, publishable
report built on Tier 1's normalized store). Everything else originally scoped for 4.1 — the
stats/confidence layer, topdown/IBS refinement, `/proc`/tree enrichment, GPU fusion, characterization
prerequisites, portability, and testing/docs cleanups — has moved to 4.2 (see below), so this phase can
stay focused on shipping a well-designed web layer instead of splitting attention across many tracks at
once. Ordered in dependency tiers; items within a tier are independently startable.

**Tier 1 — foundational, unlocks most of the rest of 4.1: shipped, kept here for the record.**

1. ~~Canonical metrics schema + normalized store (SQLite and/or Parquet)~~ — **shipped.**
   `wspy-store`/`store.c` (SQLite; see `CLAUDE.md` and `doc/ARTIFACT_CONTRACT.md`'s "Normalized
   store" section) ingests run metadata (`--run-index` records plus best-effort manifest enrichment)
   into a queryable, idempotent run catalog, **and** parses each run's CSV output into a queryable
   long/tall `metric_values` table (IPC, topdown, cache numbers, with dimension columns for
   `--interval`/`--per-core`/phase) — the actual piece #2 above and 4.2's stats/confidence items
   (#1-3) and characterization-prerequisite items (#16-17) need to draw stats and comparisons from.
   Keeps raw CSV/JSONL files untouched; this is purely a derived, rebuildable
   second layer, as originally scoped. Parquet was addressed as a downstream export
   (`duckdb store.db -c "COPY ... TO 'x.parquet'"`), not a second format `wspy-store` itself writes —
   see the doc section's "Why SQLite, not Parquet" for the reasoning; nothing about that is still
   open. Left listed here rather than moved into a "What shipped in 4.1" section, since this doc's
   own convention (see "How to use this document") is to roll items up in bulk at phase completion,
   not one at a time.
2. ~~Summary table generator (min/max/median/mean/stddev/outlier flags) from indexed data~~ —
   **shipped.** `wspy-summary`/`summary.c` (see `CLAUDE.md` and `doc/ARTIFACT_CONTRACT.md`'s "Summary
   tables" section) queries `wspy-store`'s normalized store directly, averaging each contributing
   run's rows down to one number per `(run,metric)` (collapsing `--interval` ticks/`--per-core` cores
   transparently) and reporting min/max/mean/median/stddev plus a z-score outlier flag across those
   per-run values, grouped by workload command (or `--group-by hostname|cpu_vendor`) — closing the
   "summary page regenerated from data only" criterion deferred from 4.0 (see "Success criteria for a
   4.0 kickoff"). Built against #1 (already shipped) rather than the run index directly, since the
   normalized store already solved the CSV-shape-independent parsing this needed.
3. ~~Native multi-pass counter execution (`--passes=ipc,topdown,cache,software`, internal N-run
   loop, merged manifest/CSV)~~ — **shipped.** `wspy --passes=<list>`/`multipass.c` (see
   `CLAUDE.md` and `doc/ARTIFACT_CONTRACT.md`'s manifest/run-index "passes" field notes) takes a
   comma-separated union of counter-group names and automatically bin-packs them into N
   automatically-sized passes using `preflight.c`'s existing PMU-slot-budget arithmetic (no
   hand-curated bundle table), re-launching the workload once per pass and merging the result into
   one CSV row/manifest/run-index record — confirmed real pain: `workload/phoronix/run_test.sh`
   used to launch the same command up to 8 times by hand to dodge multiplexing, and `wspy-run`'s
   builtin profiles (`deep-cpu` et al.) still do the equivalent today via N separate `wspy`
   processes. V1 scope is aggregate-only (`--interval`/`--per-core`/`--tree`/IBS/GPU all fatal
   against `--passes`); `wspy-run`'s own profiles are unchanged by this and still shell out
   N times — collapsing them onto `--passes` is a documented follow-up, not part of this item.
4. ~~**Correctness:** scale multiplexed counter values by `time_running`/`time_enabled` in
   `read_counters()`~~ — **shipped.** `read_counters()` (`topdown.c`) now tracks each counter's
   previous cumulative `time_running`/`time_enabled` (`last_time_running`/`last_time_enabled`,
   `cpu_info.h`) and scales that read's raw delta by *this window's* multiplex ratio
   (`enabled_delta/running_delta`) before storing it in `.value` — previously only the confidence
   envelope (`multiplex-aware confidence`, shipped in 4.0) accounted for multiplexing; the raw
   counter value itself was never extrapolated, so an oversubscribed run (more groups requested than
   `preflight.c`'s counter-fit budget, or the NMI watchdog eating a slot) silently undercounted
   absolute values, not just their confidence. Fixed at the source in `read_counters()`, so every
   consumer (topdown/cache/branch/tlb formulas, not just IPC) benefits automatically; the two sites
   that used to do this scaling themselves ad hoc (`print_ipc()` in `topdown.c`, `phase_current_ipc()`
   in `phase.c`) were simplified to just read the now-pre-scaled `.value` directly, since redoing the
   scaling there would double-count it. `time_running`/`time_enabled` on `struct counter_info` are
   correspondingly now *this read's delta* rather than cumulative-since-start, which also makes
   `confidence_ratio()`/phase detection's per-tick "was this counter scheduled at all" checks reflect
   that tick specifically instead of the ratio accumulated since the run started. Covered by
   `test_read_counters_multiplex_scaling` (`test_wspy.c`), which drives `read_counters()` against a
   real fd (a temp file standing in for a perf_event fd) across several simulated ticks.
   Also shipped alongside: `--multiplex` (`wspy.c`/`multipass.c`), an opt-in modifier to `--passes`
   that collapses every requested counter group into a single pass instead of bin-packing N passes,
   relying on the kernel's own PMU multiplexing plus the correctness fix above to keep the resulting
   values right — trading precision (heavier multiplexing means lower per-counter
   `confidence_ratio()`) for one workload execution instead of several. Not the default: bin-packing
   into fully-scheduled passes remains more precise when re-executing the workload N times is
   affordable. `multipass_plan_build_multiplexed()` is the one-pass counterpart to
   `multipass_plan_build()`; `--multiplex` given without `--passes` is fatal, since it has no meaning
   on its own.

**Tier 2 — web interface: forms for input, reports for output (now the sole remaining focus of 4.1):**

Design principle for everything in this tier: the web layer is a thin client over existing commands.
Every form submission should map to a command line a user could equally well type by hand (and should
show that command line, not hide it); every report should be reconstructible from `wspy-store`'s
normalized data plus the raw artifacts already on disk — no server-side state that isn't also
derivable from files already being produced.

5. ~~Design/mockup pass for the web interface~~ — **done.** Done deliberately *before* building #6-14
   below — the "step back" this reorg calls for. Covered both halves: wireframes/mockups for the input
   side (how to organize forms across more than just `wspy-run` — see #9) and the output side (a
   single-run report page, a compare-two-or-more-runs view, and a historical index/search view — see
   #8/#11). First pass of mockups produced 2026-07-11 (see chat/session record); feedback round 1 (same
   day) converged on the decisions below. This entry is that round's short writeup, written once #6-14
   had actually shipped and could confirm which mockup decisions held up in practice rather than staying
   speculative.
   - **Static-file-only vs. thin dynamic backend:** decided in favor of a thin dynamic backend
     (`web/server.py`, stdlib-only Python), against static-file-only rendering (`file://` or any static
     host, no backend process). A static build couldn't support two things this tier needed from the
     start: live SSE-streamed output while a launched run is in progress (#6/#7/#9), and later, #13's
     job queue (a pending job has to be written somewhere a background worker can pick it up — no
     static-file equivalent). This choice held for the rest of 4.1 without revisiting.
   - **Per-tool tabs at the top level**, not one monolithic form — shipped exactly as mocked in #9
     (`Run`/`Validate`/`Store & Summary`/`Discovery`).
   - **Multi-select capability checklist inside the Run tab**, rather than mutually-exclusive presets —
     counters/tree/interval/GPU/IBS each independently toggleable with their own sub-customization.
     Shipped in #9, composing into native `--passes` bin-packing (multi-group, no-interval selections)
     or separate per-configuration `wspy` invocations (an interval given), rather than the originally
     imagined `wspy-run --profile a,b,c` composition — the CLI's own multi-pass execution (Tier 1 item
     3, not yet shipped when this mockup round happened) turned out to be the better fit once it landed.
   - **Reserved, disabled row for future `/proc` extras** (4.2 Tier 3) — shipped in #9 as a disabled
     sixth checklist row, so that expansion has a slot without pre-building it.
   - **Defaults-on toggle chips** for manifest/run-index/store-ingest, instead of opt-in checkboxes —
     shipped in #9.
   - **Local-vs-shared deployment toggle answering #13** (local executes with live output, shared stays
     copy-only) — this was the mockup round's proposed answer to #13, but it is **not** what #13 shipped
     as. #13's own design work (done later, independently) concluded that the real need wasn't a
     local/shared execution-mode toggle on the same launcher, but a portable, spec-only **job** file plus
     a queue processor (`wspy-queue`/`web/joblib.py`) that decouples *creating* a job (from any machine's
     web UI) from *running* it (headless, on whichever machine actually has the hardware/counters
     available, including a second machine entirely) — a materially different shape than "toggle output
     mode on this same launcher." The toggle idea is superseded, not extended, by #13's shipped design;
     see #9's "Not yet covered" note, which has been updated to match.
   - **Gap surfaced, since resolved:** `wspy-run --profile` only accepts its own named, pre-baked
     profiles, so a custom counter selection or `--interval` sampling couldn't be composed with
     tree/GPU/IBS into one invocation the way two named profiles can. The mockup's fallback was separate
     command lines per capability; #9's shipped custom (checklist) path resolved this directly via native
     `--passes` bin-packing instead, so the fallback was never needed in the shipped UI.
   - Net effect: every layout/interaction decision from the mockup round shipped as designed in #6-#12;
     the one open item (#13's answer) evolved past the mockup's proposal once #13's own design pass ran,
     which is expected — this item's own scoping said to expect #6-14 to be revised once feedback landed,
     and #13 is the one place that happened at the *answer* level rather than the *layout* level.
6. ~~**Thin end-to-end slice through the launcher and report browser, one fixed configuration**~~ —
   **shipped.** `web/server.py` (stdlib-only Python, see `CLAUDE.md`'s `web/` entry) proves the whole
   launch → run → artifact → browse → edit pipeline end to end against the one already-well-worn
   configuration this item scoped: the `amdtopdown` pass `wspy-run`'s `deep-cpu`/`deep-gpu` profiles
   already use (`wspy --csv --interval 1 --topdown --no-rusage --no-software --no-ipc`), paired with
   `workload/phoronix/gnuplot.sh`'s existing `amdtopdown.csv` → `amdtopdown.png` plot block, reused
   as-is (cwd'd into the run directory, not parameterized). The launcher (`GET /`) has no preset
   picker or configuration/option checklist yet — one fixed selection, wired into the position #9's
   real picker will occupy later — runs locally with live output (SSE-streamed to the page and mirrored
   to a `launch.log`), and shows both the literal `wspy` and `gnuplot.sh` command lines before running
   them, copy/paste-able, never a paraphrase. Each run writes into
   `<output-root>/<suite>/<benchmark>/<run-id>/`, the same unified-output-layout shape
   `wspy-run --suite/--benchmark` uses (reused even though `wspy-run` itself isn't invoked, since
   there's exactly one pass and no profile to select), ending up with whatever subset of
   `amdtopdown.csv`/`amdtopdown.manifest.json`/`amdtopdown.png`/`launch.log` a (possibly partial or
   failed) run actually produced. The report browser (`GET /report/<suite>/<benchmark>/<run_id>`)
   reads that directory straight off disk (CSV linked, image rendered inline, manifest linked) —
   no `wspy-store` ingestion, no server-owned state — and offers a "customize & run again" link back
   to the launcher, prefilled from the manifest's own recorded workload command; a homepage listing of
   discovered run directories (newest first) means a report never has to be found by knowing its path
   by heart. Confirmed end to end on real hardware (AMD, this repo's dev machine): a `sleep`-workload
   run produced a valid 1280×960 PNG via a real `gnuplot` install, and a deliberately-broken gnuplot
   path exercised the partial-artifact degrade path (CSV/manifest present, report shows "not
   generated" for the plot) cleanly. Not yet covered, by design: a real preset/configuration/option
   checklist (#9), curation/reordering/commentary (#8), publish export (#10), and `wspy-store`
   ingestion — #7-#10 generalize this slice's pieces rather than starting from zero, per this item's
   original scoping.
7. ~~Web-based `wspy-run` profile launcher~~ — **shipped.** A deliberately cut-down first slice of #9,
   inserted ahead of #8's curation studio because curation needs a portfolio of real, varied reports
   to curate *against*, and #6's single fixed configuration doesn't provide that. Scope is exactly
   `wspy-run`'s own existing surface — pick a builtin profile (or comma-composed list: `quick`,
   `deep-cpu`, `deep-cpu-intel`, `deep-gpu`, `tree-heavy`, `ibs-basic`, `ibs-memory-deep`) plus
   suite/benchmark/workload command, run it, browse the result — and nothing past that: no ad-hoc
   counter/option checklist (that's #9's job once the preset/configuration/option vocabulary above is
   actually built out), no `wspy-validate`/`wspy-store`/`wspy-summary`/discovery-command coverage
   (also #9). Mirrors `workload/phoronix/run_test.sh`'s own real, already-hand-rolled pattern rather
   than inventing a new one: invoke `wspy-run --suite <suite> --benchmark <benchmark> <profile(s)> --
   <workload>`, then, only if the resulting run directory contains `amdtopdown.csv` (i.e. the chosen
   profile included that pass — true for `deep-cpu`/`deep-gpu`, false for `deep-cpu-intel`), `cd` into
   it and best-effort run `gnuplot.sh` same as #6, silently skipping the plot step otherwise rather
   than erroring against a profile that never produces that CSV. Command-line display/live-output
   streaming/directory-per-run plumbing all reuse #6's, unchanged. The report browser generalizes
   correspondingly: instead of #6's three hardcoded `amdtopdown.*` filenames, it reads `wspy-run`'s
   own run-level `manifest.json` (already lists each pass's name/output/manifest/status — see
   `CLAUDE.md`'s `wspy-run` entry) and renders whatever that profile actually produced — `summary.txt`,
   per-pass outputs/manifests, `process.tree.txt` for a `tree-heavy` pass, `amdtopdown.png` when the
   post-hoc plot step ran — falling back to #6's fixed-shape rendering for a report directory that has
   no run-level manifest (i.e. one #6's own fixed-config path produced). Still no
   selection/reordering/commentary/compare view — that stays #8's job once it exists.
   Confirmed end to end on real hardware (AMD, this repo's dev machine) across four cases: the
   `quick` profile (no `amdtopdown.csv`, plot step correctly skipped); `deep-cpu` with gnuplot
   missing (CSV/manifests present, plot step failed and was reported without aborting the run —
   report degrades to no PNG entry, same as #6's own broken-gnuplot path); `deep-cpu` re-run after
   installing gnuplot (real 1280×960 `amdtopdown.png`/`systemtime.png` produced and rendered inline,
   the latter picked up as an unclaimed file rather than a named pass); and a comma-composed
   `deep-cpu-intel,tree-heavy` profile, chosen only to exercise comma-composition and the `--tree`
   pass's rendering with a profile other than `deep-cpu` — `deep-cpu-intel`'s flags are vendor-neutral
   and resolved against this machine's real AMD event tables (`wspy-run` doesn't gate profiles by
   detected CPU vendor), so this ran cleanly but didn't exercise anything Intel-specific (5 passes
   including the `--tree` pass, all rendered correctly, gnuplot correctly skipped since neither
   sub-profile produces `amdtopdown.csv`). All four runs also appeared correctly on the homepage
   listing.
8. ~~Report review + curation studio~~ — **shipped** (supersedes the original "HTML report bundle"
   framing — same item number, sharpened after 4.1 feedback against a real precedent: an existing
   hand-built WordPress page per workload, e.g. a chart image + pasted raw-output block + hand-written
   commentary, repeated per configuration measured). Serves two purposes in one page: (a) **review** —
   examine every configuration a run collected and its artifacts, so nothing needs a separate viewer;
   (b) **curate** — select a subset, reorder it, and write commentary *per configuration* ("what does
   this tell us"), not just one whole-report note, then hand it off to #10's export (not yet built —
   #10 is the next consumer of this item's block-sequence output, not part of this item's own scope).
   `web/server.py`'s new `/studio/<suite>/<benchmark>/<run_id>` page (`render_studio()`) is the studio:
   a single server-rendered HTML form (no JS, matching the tier's stdlib-only/no-build-step design
   principle) listing every artifact `collect_run_files()` finds in the run directory — wspy-run's own
   passes when a run-level `manifest.json` exists, item 6's fixed `amdtopdown.*` shape otherwise, plus
   anything else sitting in the directory unclaimed by either — as "+ add" buttons, alongside a
   "+ freeform section" button for a commentary-only block with no artifact behind it at all. Adding an
   artifact more than once is allowed (each addition is its own block instance with its own id), which
   is what lets the same underlying file back two differently-titled sections the way the real
   precedent page's separate "AMD results"/"Intel results" sections do. Each block instance carries a
   title, an **inclusion depth** — `none`/`summary`/`excerpt`/`full`, the generalized single control the
   spec called for rather than a separate all-or-nothing toggle; `none` excludes it from the curated
   view without deleting it from the studio — and a commentary field, plus move-up/move-down/remove
   buttons. `render_block_content()` implements depth uniformly across every text-shaped artifact (CSV,
   `.txt`/`.log`, JSON), not just the process-tree case the spec called out concretely: `summary` is a
   short peek (line/byte count, first few lines, JSON top-level keys); `excerpt` is the first N lines
   (N user-configurable per block, default 40) with a "showing X of Y lines" note and a link to the full
   file; `full` embeds the whole thing (capped at 5MB inline — past that it degrades to a link rather
   than handing the browser a pathologically large `<pre>`). Images only offer `none`/`full` (no
   meaningful partial rendering of a PNG); a file that doesn't decode as UTF-8 degrades to a link-only
   render regardless of requested depth rather than failing the page. State persists to
   `<rundir>/curation.json` (`load_curation()`/`save_curation()`) — one more file the run directory
   holds, not server-owned memory, matching every other artifact in this tier. The whole editor is one
   `<form>`: every submit button (move/remove/add/save) carries an `op` value naming its action, but all
   of the form's other fields — every existing block's title/depth/commentary — ride along on the same
   POST, so `apply_studio_post()` reconstructs the full block list from the submission before applying
   that one op and re-saving; clicking "move up" on one block never discards an unsaved edit typed into
   another block's commentary field. POST redirects (303) back to the same GET so a page refresh never
   resubmits. The report page (`render_report()`, both shapes) grows a "curated view" section
   (`render_curated_section()`) above the existing raw artifact listing whenever `curation.json` has at
   least one included (depth != `none`) block, rendering each block's title/commentary/depth-limited
   content in curated order; the raw listing collapses into a `<details>` underneath once there's a
   curated view to lead with, and stays open (as before) when there isn't. A "Curate this report" /
   "Edit curation" link is always present either way. **Compare view** (`GET /compare?r=<suite>/
   <benchmark>/<run_id>&r=...`, `render_compare()`) is the other half of this item: pick 2+ runs from the
   homepage's report table (now wrapped in a checkbox form posting to `/compare`, no JS needed — plain
   HTML `GET` with repeated `r` params) and see them side by side, one column per run, one row per
   filename (the union of `collect_run_files()` across all selected runs) — images render as inline
   thumbnails, everything else as a link with its byte size, and a missing file in one run's column
   shows `—` rather than breaking the grid. Deliberately raw/filename-aligned rather than curation-aware
   — comparing actual artifacts across runs is useful whether or not either run has been curated yet,
   and block-level alignment (matching curated titles across runs) isn't attempted here. Exercised
   against three real wspy-run report directories already on disk from item 7's own hardware testing
   (`web/runs/phoronix/{coremark,stockfish,c-ray}/...`, produced by real `deep-cpu` runs on this repo's
   AMD dev machine) via scripted form submissions that replay real browser semantics (fetch the current
   studio page, round-trip every existing hidden/text/select/textarea field plus one new `op`, exactly
   what a browser's own submit does) — added/reordered/depth-changed/annotated blocks across both a
   wspy-run-shaped and a synthetic item-6-fixed-shaped run directory, confirmed the curated view and
   collapsed raw-artifact `<details>` render correctly on the report page, and confirmed the compare
   grid against two real runs. Not yet covered, by design: #10's actual export formats (this item
   produces the curated block sequence #10 will read, not the WordPress/HTML/Markdown rendering
   itself), and compare view has no curation/annotation layer of its own.
9. ~~Web-based run launcher + report browser generalization~~ — **shipped.** Landed as four tabs on
   `web/server.py`'s `GET /` (`.tab-btn`/`.tab-panel`, client-side switching, no server round-trip) —
   Run, Validate, Store & Summary, Discovery — settling #5's mockup-pass layout question (per-tool
   tabs) directly. The Run tab is organized around the preset/configuration/option hierarchy the
   deep-dive above calls for: a preset dropdown (`BUILTIN_PROFILES`, routing through #7's existing
   `wspy-run` path unchanged and atomic — selecting one disables the checklist) alongside a real
   five-configuration checklist (process tree, performance counters, system metrics, GPU metrics, AMD
   IBS, plus a sixth disabled "/proc extras" row reserved for 4.2 Tier 3) that composes directly into
   `wspy` command lines — `build_configuration_passes()` is the one place checklist state becomes
   flags, shared by the preview endpoint and the real executor so the shown command line is never a
   paraphrase. This also resolved the concrete gap #5's feedback round flagged (`wspy-run --profile`
   couldn't compose an ad-hoc counter/interval selection with tree/GPU/IBS in one invocation): a
   multi-group, no-interval counter selection now bin-packs via native `--passes` instead of going
   through `wspy-run` at all. Custom runs write a `wspy-run`-shaped `manifest.json`/`summary.txt` so
   #7/#8's existing report/curation/compare pages render them with no new code path. Toggle chips for
   manifest/run-index/store-ingest are default-on, per #5's feedback. Validate/Store & Summary/
   Discovery tabs wrap `wspy-validate`, `wspy-store`+`wspy-summary`, and `wspy --capabilities`/
   `--preflight` respectively, all synchronous (no run directory, no SSE, nothing to browse to). Not
   yet covered, by design: **#16** structured configuration provenance (so "customize & run again"
   still can't restore exact preset/checklist state, only workload/suite/benchmark — the same
   limitation #6/#7 already had) — since shipped, see its own entry and #17's — and **#18**
   estimated-runtime display — also since shipped, as a standalone Run tab "Check" button, see its
   own entry. **#13** was originally
   listed here as an open gap (no design note existed yet, and #5's mockup round had proposed a
   local-vs-shared execution-mode toggle on this same Run tab as the answer) — since resolved, but not
   the way either of those assumed: #13's own design pass concluded the real need was a portable,
   spec-only job file plus a headless queue processor (`wspy-queue`/`web/joblib.py`), not an
   execution-mode toggle on this tab. What actually landed in the Run tab is a "Queue instead of run
   now" checkbox that reuses the tab's exact preset-or-checklist/workload/toggle state to build a job
   (`POST /api/enqueue-job`) instead of launching immediately — an addition to this item's shipped
   surface, not a revision of it, and the toggle idea from #5 is superseded rather than implemented.
10. ~~Publish-ready data export format~~ — **shipped.** `GET /export/<suite>/<benchmark>/<run_id>`
    (`render_export_page()`, linked from the report page and the curation studio once `curation.json`
    has at least one included block) renders #8's curated block sequence into three target formats,
    switchable via `?format=` tabs with no server round-trip cost (each is computed on request, nothing
    cached):
    - **WordPress block markup (Gutenberg comment format)** — the default. `render_export_wordpress()`
      wraps each block's title/commentary/content in real `<!-- wp:heading -->`/`<!-- wp:paragraph -->`/
      `<!-- wp:image -->`/`<!-- wp:preformatted -->` comment pairs (`_wp_block()`), so pasting it into the
      WordPress block editor produces separately-editable native blocks instead of one opaque blob.
    - **Self-contained inline-styled HTML** (`render_export_html()`) — a standalone document with every
      style attribute inline, for a "Custom HTML" block or any CMS that just wants raw markup.
    - **Markdown** (`render_export_markdown()`) — heading/image/fenced-code per block, portable to
      anywhere that takes it directly or as a conversion source.
    All three are thin wrappers over one shared `export_block_content()`, which mirrors #8's own
    `render_block_content()` depth handling (summary/excerpt/full) but returns structured data (image
    URL / preformatted text / a plain-text note) instead of an HTML string, so no rendering logic is
    tripled across formats. `GET .../download?format=...` sends the same content with a
    `Content-Disposition: attachment` header and format-appropriate extension (`.html`/`.html`/`.md`);
    the on-page view is a plain `<textarea readonly>` (select-all-and-copy, no clipboard JS needed).
    Real images still aren't uploaded anywhere by this item — an image block exports as a URL pointing
    back at this server's own `/files/...` endpoint, which only resolves while the server keeps running
    at that address. That gap (flagged in the original scoping as "a real gap between mockup and
    implementation, not a detail to gloss over") is surfaced explicitly: the export page prints a visible
    note whenever an image block is included, telling the user to re-upload it to the target platform's
    media library and swap in the resulting URL before publishing. Actually generating those images from
    a normalized schema (rather than `gnuplot.sh`'s per-suite script) is #12, not this item.
11. ~~Historical run index browser/search~~ — **shipped.** `GET /history` (`web/server.py`'s
    `render_history()`) generalizes the homepage's `discover_reports()` list (newest-50, no
    filtering) into a dedicated search/browse page over every run directory: filters on workload
    command substring, suite/benchmark/hostname substring, exact CPU vendor/status, and a
    start-date/end-date range, plus pagination — all plain query-string GET params, bookmarkable
    and JS-free like `/compare`. `discover_run_history()` scans every run directory (no cap) and
    `load_run_history_entry()` enriches each from whichever manifest shape is present — workload
    and status (mirroring `wspy-validate`'s clean-exit definition) always available, hostname/
    `cpu_vendor`/`start_time`/`elapsed_seconds` read from a representative per-process `--manifest`
    when one exists — rather than querying `wspy-store`'s normalized `runs` table, since store
    ingestion is only an opt-in toggle chip and this page should cover every run regardless. Result
    rows reuse the homepage's "select rows, compare selected" pattern into `/compare`. See
    `CLAUDE.md`'s `web/` entry for the full breakdown.
12. ~~Shared plotting templates~~ — **shipped.** `wspy-plot` (`plot.c`) replaces
    `workload/phoronix/gnuplot.sh`'s per-suite script (retired) with a normalized-schema pipeline built
    on #1's own column-identity convention (a column named exactly `time`/`core`/`phase` is a
    dimension, everything else is a metric) rather than on `store.c`'s actual database, so it works
    directly against a run directory's CSVs without requiring store ingestion first. A small built-in
    template table (`--list-templates`) matches a CSV's *header* against each counter group's known
    metric columns (topdown, memory-boundedness, cache-miss, system-cpu, ipc, branch-miss, float),
    firing once enough of a template's candidate columns are present regardless of position or which
    flags produced them; any metric columns no template claims still get one generic fallback plot, so
    an unfamiliar counter-group combination produces something rather than nothing. Only a CSV with a
    `time` column (produced with `--interval`) is a time series to chart at all. `wspy-plot --rundir
    <dir>` scans every `*.csv` in a run directory and writes matched plots into `<dir>/plots/`
    (`wspy-run`'s previously-empty reserved directory) via a generated gnuplot script — this is what
    closes #10's real-image gap: the studio (#8) and export (#10) can now reference a real chart for
    any run, not just the fixed `amdtopdown`/`systemtime` shape. `web/server.py`'s item 6/7/9 run
    executors and `workload/phoronix/run_test.sh`/`workload/cpu2017/run_test.sh` all call `wspy-plot`
    unconditionally (best-effort) after a run instead of gating on whether `amdtopdown.csv` exists,
    since template matching makes that gate unnecessary. `--plot NAME=col1,col2,...` (repeatable) lets
    a user hand-pick exactly which counters land on one plot together — useful when specific counters
    matter for a given interval report but don't share a scale with any built-in template's grouping
    (mixing them into the generic fallback would otherwise flatten the smaller-magnitude ones to an
    indistinguishable line, confirmed empirically doing exactly this against a real `--system` run);
    `--only-custom` renders exactly the given `--plot` spec(s) and nothing else, for full control. The
    Run tab's "Custom plots" section (`web/server.py`/`app.js`) exposes both flags directly: pick and
    choose multiple named plots for one workload launch, some default (the built-in templates, still
    additive unless "only render these" is checked) and some custom (specific counters of interest),
    without needing to re-run `wspy-plot` by hand afterward. In custom (checklist) mode, requesting a
    column whose counter group isn't already checked auto-enables it (plus a 1s `--interval`, without
    which there's no time series at all) rather than silently producing an empty plot — reflected back
    into the actual checkboxes, not just a text note. Preset mode can't be auto-fixed the same way (a
    preset is atomic), so it warns instead when the chosen preset's own passes won't produce a
    requested column at all — true of most presets, which are aggregate-only and have nothing
    time-series to chart regardless of counter selection; only `deep-cpu`/`deep-gpu` do. See
    `CLAUDE.md`'s `plot.c` and item 9
    entries for the full breakdown.
13. ~~Deployment/hosting design note~~ — **shipped**, as a real feature rather than staying
    design-only. Original scope: answer, for both a person browsing their own local run output
    and a team publishing to a shared site: does #8/#11 need to run anywhere besides `localhost`, is a
    static site (generated files, no server process) sufficient, and if not, what's the smallest
    backend that covers both cases? Feeds #5's mockup pass directly and should land before #9's
    launcher decides how it invokes `wspy-run` (local subprocess vs. something that only makes sense
    on a single machine). Answered, driven by three concrete use cases beyond "launch one run and
    watch it stream": (a) using the web UI to create a series of **jobs**, each describing one
    intended benchmark run, without launching it immediately; (b) processing a queue of such jobs from
    the command line, independent of the web UI being open or even running, so a batch can be queued
    once and worked off later/unattended; (c) copying a job to a second machine that also has wspy
    checked out, and processing it there instead — i.e. a job must be **portable**, not tied to the
    machine that created it.
    - A **job** is a spec-only JSON file — the same preset/checklist configuration + workload command +
      suite/benchmark identity #9's Run tab already builds an argv from (`build_configuration_passes()`
      / the preset-vs-custom split), captured *before* any run directory or output exists, not a
      pre-staged run directory with a placeholder manifest. This keeps a queued-but-not-yet-run job out
      of #11's history browser and #8's report listings entirely, and keeps the job file itself trivial
      to copy. It's the natural companion to #16's structured configuration provenance: #16 records
      *what already ran*; a job records *what should run*, ideally in close to the same shape so the
      two don't drift into separate vocabularies.
    - Portability requires a job file to carry no reference to the machine that created it — no
      absolute `--output-root`, no path into that machine's `--run-index`/`store.db`. #9's Run tab
      becomes a job *creator* as well as a job *runner*: submitting the form can enqueue a job instead
      of launching immediately. A destination machine drops the copied file into its own local jobs
      directory and works it against its own independent output tree — there is no shared/synced state
      between machines, by design.
    - A new standalone CLI tool, `wspy-queue` (alongside `wspy-run`, not a mode of `web/server.py`),
      scans a jobs directory and processes pending jobs **strictly serially** — matches every existing
      assumption in this codebase that a `wspy` run has exclusive use of the machine's hardware PMU
      counters (`preflight.c`/`coverage.c`'s multiplexing math already treats counter contention as a
      single-process concern); no `--parallel` option. Each job moves through a minimal
      `pending → running → done`/`failed` lifecycle with no silent auto-retry on failure (matching the
      degrade-don't-fail-but-don't-hide-it convention `coverage.c`/`validate.c` already use elsewhere) —
      a failed job stays failed until a human re-queues it. `wspy-queue` must work fully headless: no
      dependency on `web/server.py` being up, so it can run via cron or an interactive SSH session on a
      machine with no browser/web server involved at all. This settles the "smallest backend" question
      for *execution*: no persistent server process is required at all — the web UI is purely optional,
      for creating/browsing jobs and reports, never required for a job to actually run. #8/#11 (browsing
      curated reports / historical search) can still be a thin server (or later, a static-site export)
      layered on top, since neither depends on how a run was launched.
    - This is also the concrete driving use case behind #19's rough "read a Phoronix article, inventory
      its benchmarks, and create jobs for whichever haven't been run yet" idea: "already run" there is
      answered by matching a candidate benchmark name against `wspy-store`'s normalized `runs` table (or
      the run-index directly) using the same substring-matching approach `ledger.c` already implements
      for suite-level workload coverage, not a new completion-tracker bolted onto the job file itself.
      See #19's own entry for the current (still speculative/future) scope of that connection.
    - **What shipped:** every piece of the design above landed as described, not just the note. The
      checklist/preset → `wspy` argv builders and the actual run executors that used to live directly in
      `web/server.py` were pulled out (no behavior change) into `web/joblib.py`, which now also holds the
      job format itself (`build_job()`/`validate_job()`/`resolve_toggles()`/`JOB_SCHEMA_VERSION`) — this
      is what lets `wspy-queue` (repo root, alongside `wspy-run`, stdlib-only Python, no build step) share
      the identical preset/checklist → command-line logic instead of reimplementing it, so a job behaves
      identically whether it's processed by `wspy-queue` headless or (indirectly, via the same functions)
      by the web UI. `wspy-queue`'s `pending`/`running`/`done`/`failed` lifecycle is literally four
      `<jobs-dir>/<state>/*.json` directories (Maildir-style); claiming a job is one `os.rename()` from
      `pending/` to `running/`, which is atomic on a POSIX filesystem and doubles as the only concurrency
      control needed. Subcommands: `add` (preset mode via `--profile`, custom mode via
      `--checklist-json <file>`), `run` (drains pending jobs — `--job`/`--limit`/`--strict`), `list`,
      `show`, `requeue`. `web/server.py`'s Run tab grew a "Queue this instead of running it now" checkbox
      that posts the same preset-or-checklist/workload/toggle state to a new `POST /api/enqueue-job`
      (`Handler._enqueue_job()`) instead of `/api/run-profile`/`/api/run-custom` — the server never
      executes a job itself, only ever writes one into `<jobs-dir>/pending/` (default `web/jobs`, same
      default `wspy-queue` uses), confirming the "smallest backend for execution is none" answer above.
      Tests: `tests/wspy_queue_smoke.sh` (wired into `run_tests.sh`; fake `wspy`/`wspy-run`/`wspy-plot`/
      `wspy-store` binaries exercise the full lifecycle, failure/requeue, and portability by copying a
      job file into a second, independent jobs/output tree) and `web/test_joblib.py` (job schema/
      validation and checklist→argv unit tests, standalone like the rest of `web/`'s test story). Not
      done as part of this item, left for whoever picks up #16/#17/#19: a job-browsing view in the web
      UI (today a queued job is only visible via `wspy-queue list`/`show`), and #16's structured
      configuration provenance still isn't shared with the job format even though they're designed to be
      close in shape.
14. ~~Traceability links (summary row → manifest → raw CSV → plots → tree artifacts)~~ — **shipped.**
    Closes the "every published row traces back to command/environment/artifacts" criterion deferred
    from 4.0 (see "Success criteria for a 4.0 kickoff", now updated to reflect both deferred criteria
    being met). Built directly on `store.c`'s existing `runs.{manifest_path,output_path,
    tree_output_path}` columns (already populated at ingest time, per `wspy-summary`'s own
    "Reads store.c's ... tables directly" design) rather than a new schema/index — the data traceability
    needs was already there, it just had no query path back out.
    - `wspy-summary --show-runs` appends every contributing run's `hostname:run_id` to a
      `(group,metric)` bucket (a new trailing column in both `--csv` and human output — all
      contributing runs, not just `outlier_run_ids`' flagged subset), so any row in a summary table,
      surprising or not, has a concrete set of run identities to chase.
    - `wspy-summary --trace <hostname>:<run_id>` is a new standalone mode (`trace_run()`) that
      resolves one of those identities to the actual chain: command line + environment (via the
      manifest path), raw CSV, tree artifact, and plots (derived as `<output_path's directory>/plots`
      — `wspy-plot`'s output location, not a column the store tracks on its own) — checking with
      `stat()`/`opendir()` which of them still exist on disk rather than trusting what was recorded at
      ingest time. Every field degrades independently (`exists=0`, not a failure) when a path no
      longer resolves — expected for a run-index ingested from a different host, per
      `doc/ARTIFACT_CONTRACT.md`'s existing note that `manifest_path`/`output_path` are frequently
      host-relative in that setup. Output is stable `key=value` lines (not `--csv`'s table shape, not
      a bespoke JSON encoding) so a script or `web/server.py` can parse one resolved run without a
      JSON library on either side. See `CLAUDE.md`'s `summary.c` entry and
      `doc/ARTIFACT_CONTRACT.md`'s "Summary tables" section for the full breakdown, and
      `test_summary.c` for coverage (contributing-run-list presence/absence, artifact resolution
      against a real temp directory including stale/never-existed paths, an unmatched
      `(hostname,run_id)`).
    - `web/server.py`'s Store & Summary tab gets a "show contributing runs" checkbox (passes
      `--show-runs` through to `/api/discovery/summary`) and a new "Trace a run" sub-form
      (`POST /api/discovery/trace`) that runs `--trace`, parses its `key=value` output, and —
      best-effort, via `_rundir_triple_for_path()`/`_resolve_trace_links()` — resolves real
      `/report`/`/files` links whenever the returned paths happen to fall under this server's own
      `--output-root` (the common case for a run this same server launched and stored), rendering
      real clickable links instead of bare filesystem path text wherever that's possible. Covered
      standalone by `web/test_trace_links.py` (same "not wired into `make test`" convention as
      `web/test_joblib.py`), matching this tier's design principle that a report/query result should
      be reconstructible from the normalized store plus raw artifacts already on disk, with no new
      server-owned state.
    - Not part of this item's scope (left for #16/#17): restoring exact preset/checklist launcher
      state from a traced run — `--trace` surfaces the *artifacts* a run produced, not the
      *configuration* that produced them; that's #16's structured configuration provenance, still
      open.
15. ~~Report commentary/annotation~~ — **shipped.** Per-configuration commentary ("what does *this*
    configuration tell us," attached to a block in #8's curation studio) already shipped as part of #8
    itself; this item adds the other half the doc called for — "not a single global field" — one
    **overview note** for the report as a whole, distinct from every block's own commentary. Persisted
    as a new `overview_note` string alongside `blocks` in `<rundir>/curation.json`
    (`CURATION_SCHEMA_VERSION` bumped 1.0 → 1.1, a backward-compatible addition — `load_curation()`
    defaults a pre-existing file's missing key to `""`), not the normalized store as originally
    speculated: #8 already established curation state as a run-directory file rather than server-owned
    state, and the note needs exactly the same "survives a report being regenerated, editable from the
    studio, degrades to absent on an old curation.json" behavior the per-block commentary field already
    has — reusing that mechanism is simpler than introducing a second, store-backed persistence path for
    one string. `render_studio()` gets a "Report overview" textarea above the block list (inside the
    same `<form>`, so it saves along with any block edit exactly like every other field);
    `apply_studio_post()` reads it back via `form.get("overview_note", [""])[0]`.
    `render_curated_section()` renders it first, above the per-block sequence, whenever it's non-empty
    — and now shows the curated view even when there are zero included blocks but a written overview
    note, rather than requiring at least one block. All three of #10's export formats
    (`render_export_markdown`/`_html`/`_wordpress`) render it as an intro paragraph right after the
    title heading, ahead of the per-block content; `_export_data()` replaces the old `_export_blocks()`
    helper so the note and the block sequence are read from `curation.json` together and can't drift out
    of sync in the export/download code paths. Not in scope, per the doc's own framing as a lower
    priority: an optional local-LLM-drafted starting point generated from a configuration's own metrics
    — still just a visibly-disabled mockup affordance, no real integration.
16. ~~Structured configuration provenance~~ — **shipped.** Records a run's preset/configuration/option
    choices (see the deep-dive above) as structured data in `manifest.h`/`run_index.h`, not just the
    flat command line and flag set already captured — this is what lets #8/#11's reports eventually
    render "how this was run" in the same vocabulary the launcher uses, and lets #17's "customize & run
    again" restore exact launcher state from a report instead of re-parsing argv (both still open,
    consuming this item's output). A real schema change, as scoped: `MANIFEST_SCHEMA_VERSION`/
    `RUN_INDEX_SCHEMA_VERSION` bumped `1.4.0` → `1.5.0` (MINOR, additive) for the new
    `configuration_provenance` object (`struct manifest_config_provenance`, `manifest.h`) — `{ "preset":
    ..., "configuration": ..., "options": [...] }`, all-`null`/`[]` (not a gap) for a plain direct
    `wspy` invocation. `wspy` itself has no notion of presets or configurations — that vocabulary
    belongs to a front end — so the fields are populated purely from three new metadata-only CLI flags
    (`wspy.c`): `--preset-name <name>`, `--config-name <name>`, and repeatable `--config-option
    <k>=<v>` (a malformed one is warned about and dropped, not fatal). Neither flag affects what the
    run actually does; they exist only so a caller that *does* have that vocabulary can stamp it onto
    the manifest/run-index instead of a report having to re-derive "this was deep-cpu's
    performance-counters pass" from `counter_mask`/argv later. Two front ends populate it today:
    `wspy-run`'s `run_pass()` passes `--config-name <pass-name>` on every pass and `--preset-name
    "$PROFILE"` whenever the pass came from a named builtin profile (not a `-c`/`--config` pass-list
    file, which has no preset to record); `web/joblib.py`'s `build_configuration_passes()` tags each
    checklist row with a stable `category` (`process-tree`/`performance-counters`/`system-metrics`/
    `gpu-metrics`/`ibs` — distinct from the pass's output-filename `name`, which can be a legacy alias
    like `amdtopdown`) and a generic `options` projection of that row's own sub-dict
    (`_config_options()`), and `build_pass_argv()` threads both through as `--config-name`/
    `--config-option` — with no `--preset-name`, since a checklist-driven run has no named preset by
    definition. `store.c`/`summary.c` ingestion of this new field, and #8/#11/#17's actual rendering of
    it, are explicitly out of scope for this item (see #17 below) — this item only had to get the data
    captured and structurally real, not yet consumed.
17. ~~Browse-reports~~ — **shipped.** Relates each report's artifacts back to the preset/configuration/
    option choices that produced them (via #16's `configuration_provenance`) and lets "customize & run
    again" return to the launcher pre-filled from the report rather than from scratch, closing the gap
    #16 deliberately left open. `read_manifest_config_provenance()` (`web/server.py`) pulls a pass's own
    `configuration_provenance` back out of its per-pass manifest.json; `render_wspy_run_report()`
    (item 7/9 reports) and `render_fixed_report()` (item 6's superseded fixed-config reports, checked
    for completeness even though `execute_run()` never populates it) print it inline next to each pass
    (`format_config_provenance()`) so a report visibly states "this pass was `deep-cpu`" or "this pass
    was `performance-counters` with `groups=topdown,cache2`", not just a bare command line. The read side
    of #16's structured provenance lives in `web/joblib.py`: `checklist_section_from_options()` is the
    exact reverse of `_config_options()`, and `checklist_from_pass_provenance()` aggregates a run's
    per-pass records into either a restorable preset name (any pass carrying `--preset-name`, per
    `wspy-run`'s `run_pass()`) or a full checklist dict (item 9's checklist-driven runs, which record
    `--config-name`/`--config-option` per category but never a preset) — mutually exclusive by
    construction, matching how `build_pass_argv()` emits them. `build_rerun_url()` folds whichever one
    resolved into a single JSON `config` query parameter on the "Customize & run again" link (a
    checklist is a nested structure the older flat `workload`/`suite`/`benchmark` query params can't
    express); `do_GET("/")` parses and validates it defensively (preset checked against
    `BUILTIN_PROFILES`, checklist restricted to the known `tree`/`counters`/`system`/`gpu`/`ibs` category
    keys) before it reaches `render_run_tab()`, which renders the preset `<select>`'s matching `<option
    selected>` and every checklist checkbox/field pre-checked/pre-filled from the restored state — a
    plain visit to `/` or a report with no recorded provenance (predating #16, or a direct `wspy`
    invocation) falls back to today's workload/suite/benchmark-only prefill unchanged. Covered by
    `web/test_joblib.py`'s `ChecklistFromProvenanceTest` (round-trips a checklist through
    `build_configuration_passes()` → `_config_options()` → `checklist_section_from_options()` and back,
    plus the preset-wins-over-checklist and no-provenance-at-all cases) and hand-verified end-to-end
    against a real checklist-driven run and a synthetic preset-driven manifest.
18. ~~Estimated runtime display~~ — **shipped**, as a standalone "Check" button on the Run tab
    (`web/server.py`'s `render_run_tab()`/`Handler._check_run()`, `POST /api/check-run`) rather than
    inline once a benchmark is selected — deliberately optional/read-only per the item's own
    "quickly check ... before launching" framing, so it never blocks or slows down an actual Run
    click. Two independent checks: (a) `perf_event_paranoid`/`nmi_watchdog` read directly from
    `/proc/sys/kernel/*` and reported ok/warn against wspy's documented minimum
    (`scripts/setup_perf.sh`'s own thresholds); (b) for a `phoronix-test-suite batch-run`/`run`/
    `benchmark` workload specifically, `phoronix-test-suite info <test>` per test — parsed for
    install status, times run, and `Estimated Run-Time` vs. this host's own measured
    `Average Run-Time` — picking whichever is more informative (not installed, or installed-but-
    never-run → the generic estimate; already run here → the measured average), matching the
    original design note's install/not-run/run three-way split. Still not scoped for other suites
    (`cpu2017`, `pbbsbench`) or arbitrary workload commands, which have no equivalent estimator
    today — those just report "no estimate available" rather than guessing. See `CLAUDE.md`'s
    `web/` entry, "Estimated runtime check" bullet, for the full design.
19. Deeper Phoronix Test Suite awareness in the web UI — not needed for 4.1's release, flagged as a
    future backlog item since most real wspy testing in practice runs through Phoronix specifically
    (`workload/phoronix/`), so some suite-specific knowledge on top of the general-purpose launcher is
    likely worth it eventually. Rough shape, to be refined when actually scoped:
    - A "Phoronix" tab to browse/filter the suite's installed vs. not-yet-installed tests and surface
      their attributes (description, last-updated date, typical run time, ...) so a test can be chosen
      — and, if not installed, installed — without leaving the web UI or knowing `phoronix-test-suite`
      CLI incantations by heart.
    - Reduce redundant typing in the Run tab: today the "workload command," "suite," and "benchmark"
      fields are filled in by hand even when the workload is a Phoronix test, where the suite/benchmark
      are already implied by the test name. Run directories are already unique, but embedding the
      benchmark name more directly (they're already namespaced by suite/benchmark, see `wspy-run`'s
      unified output layout) would make Phoronix-sourced runs easier to recognize at a glance in the
      homepage listing/#11's historical browser.
    - Locate where a Phoronix test actually ran/logged on disk (its own results/log directory, separate
      from wspy's own run directory) and surface/link to it from the report page — particularly useful
      for troubleshooting when a benchmark run fails and the wspy-side artifacts alone don't explain why.
    - Reading a Phoronix benchmark *article* (not just the locally-installed suite) and inventorying the
      benchmarks it lists, so that whichever ones haven't been run yet get jobs created for them, and
      once every benchmark from that article has a completed run, an uplevel report can be generated
      referencing the article alongside pointers to each benchmark's run(s). Now builds on #13's
      job/queue mechanism rather than needing its own separate execution/dedup design: this becomes
      "parse the article, diff its benchmark list against the store/run-index the same way `ledger.c`
      already diffs a workload list for suite-level coverage, emit a job file per gap, let `wspy-queue`
      work them off," plus a small article-level report generator once #13's job states show every job
      for that article as `done`.
    - Likely other Phoronix-specific conveniences once this is actually scoped; the general idea is a
      dedicated future item for deeper Phoronix-particular knowledge in the web UI, layered on top of
      (not replacing) the suite-agnostic launcher #9 already ships.

## 4.2 priorities
Goal: everything originally scoped for 4.1 beyond Tier 1 (shipped) and Tier 2 (now 4.1's
web-interface focus — see above): the stats/confidence layer, topdown/IBS refinement, `/proc`/tree
enrichment, GPU fusion, characterization prerequisites, portability, and testing/docs cleanups.
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
14. `proctree`/tree-file robustness against out-of-order fork/exit ptrace events —
    `run_tests.sh`'s 2000-process tree stress test intermittently reports `exit for unknown pid`
    (observed once during the `wspy-store` branch's testing, not reproducible across 3 isolated
    reruns of the same stress test — load-dependent flake, not a deterministic failure). Root cause
    is a real kernel-level race, not a `wait4()` bug: `ptrace_loop()` (`topdown.c`) writes tree-file
    lines in whatever order the kernel delivers ptrace stops across the traced process *and* every
    child it auto-attaches to via `PTRACE_O_TRACEFORK`/`TRACECLONE`/`TRACEVFORK`; a short-lived child
    (e.g. `/bin/true`) can run and hit its own exit stop before the *parent's* `PTRACE_EVENT_FORK`
    stop — which is what records the parent→child edge — is dequeued and written, so under high
    fork-rate/scheduling pressure an `exit` line can legitimately land in the tree file before its
    `fork` line. The fix belongs in the reader, not the writer: make `proctree.c`'s reconstruction
    tolerate an `exit` for a not-yet-seen pid (e.g. a two-pass parse, or buffering unresolved exits
    keyed by pid and reconciling them once/if a later `root`/`fork` line establishes that pid)
    instead of assuming strict single-pass ordering. `run_tests.sh`'s own stress-test `awk` integrity
    checker encodes the same strict-ordering assumption and should be relaxed (or made two-pass)
    alongside `proctree.c`, so a legitimate kernel race doesn't get flagged as a correctness bug.

**Tier 4 — GPU track:**

15. ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) —
    merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`).
16. Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory
    activity) — reuses 4.0 foundation work rather than a parallel GPU-only pipeline.

**Tier 5 — characterization prerequisites:**

17. Feature normalization prerequisites (fixed feature set from counters/topdown/faults/context-
    switch/I-O) — needs 4.1's #1 normalized schema to draw features from.
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
7. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache).

**Tier 3 — publishing/reporting expansion, needs 4.1's report studio:**

8. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1 #8's per-run studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats #10 (4.1) produces rather
   than inventing a fourth.
9. Characterization badges + similarity panels in reports — a new block type in 4.1 #8's studio once
   4.2 #18's archetype scorecard exists to draw a badge from, not a separate report surface.
10. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1 #8's
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
20. Core/thread affinity control (`--affinity=all|thread=<id>|domain=<id>|cpuset=<c0,c1,...>`) — pin
    the launched workload to a selected set of logical CPUs via `sched_setaffinity()` on the forked
    child before `execve` in `topdown.c`'s `launch_child()`: `all` (default, every visible thread —
    today's implicit behavior), `thread=<id>` (that single thread, letting a caller deliberately avoid
    its SMT sibling), `domain=<id>` (every thread on one core-complex/CCD), and `cpuset=<c0,c1,...>`
    (explicit enumerated core list — the general form the others are shorthand for). `thread=`/
    `domain=` need core topology data (SMT sibling pairs, CCD/core-complex grouping) that
    `cpu_info.c`'s `struct cpu_core_info` doesn't capture yet — parsing
    `/sys/devices/system/cpu/cpu*/topology/{core_id,core_siblings,package_id}` (plus, for AMD CCD
    grouping, cache-topology or `cpuid` leaf data) is a real prerequisite here, not a detail to defer
    silently. The resolved core list should also be recorded in `--manifest`/`--run-index` so a run's
    placement is part of its provenance rather than only implicit in how it was launched.

**Tier 7 — testing:**

21. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
22. Contributor guide for adding a collector/metric/schema bump safely.

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
  Recommendation: not yet — let the web UI's preset/configuration/option model (4.1 #9) stabilize
  against real feedback first, then decide whether `wspy-run` itself should be rebuilt on the same
  vocabulary. Premature to commit to a CLI/`wspy-run` restructure before the vocabulary has been used
  for anything real; there's real leeway to make this change later if it produces a cleaner
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
- **Should `wspy` natively handle multi-pass execution? — resolved.** Yes, shipped in 4.1 Tier 1
  item 3 above (`wspy --passes=<list>`/`multipass.c`).
- **Is ARM64/AArch64 support a priority for 4.x? — resolved.** Yes, fallback CPU topology
  detection and register-access abstractions have been fully implemented and validated on real
  AArch64 hardware (including the `ptrace_arch.h` `__aarch64__` implementation). Both gaps found by
  code review (raw counter chunking/bin-packing and topdown sanity-tolerance warning checks) have
  been fully resolved, and all tests have been validated and verified on real ARM64 hardware.
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
  trace analysis pipeline (4.4): https://perfetto.dev/docs/

Note: OpenBenchmarking.org returned HTTP 403 in the environment used for this research; not
reviewed.
