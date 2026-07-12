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

Two criteria that were part of 4.0's original bar are **deliberately deferred to 4.1, not dropped**:
- A summary page can be regenerated from data only (no manual copy/paste).
- Every published benchmark row can be traced back to command line, environment, and raw artifacts.

Rationale: building a minimal report/summary generator now, then rebuilding it properly once the 4.1
normalized-store work (schema + indexed queries) lands, is duplicated effort for no real benefit —
nothing downstream depends on a 4.0-era stub existing first. Better to do the reporting layer once,
thoroughly, as 4.1 Tier 1-2 already scopes it (canonical schema, summary table generator, report
studio, traceability links). 4.0 ships the foundation those depend on (manifest/run-index/validation/coverage/
provenance); 4.1 turns it into the actual page/row a person reads.

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
- Exercise `--ibs-basic`/`--ibs-memory-deep` against real `ibs_fetch`/`ibs_op` PMUs on Zen4/Zen5
  hardware — `test_ibs.c` only drives `ibs_probe_at()` against a synthetic fake sysfs tree, never
  real IBS counters or real filtering behavior.
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

5. Design/mockup pass for the web interface, done deliberately *before* building #6-14 below — the
   "step back" this reorg calls for. Covers both halves: wireframes/mockups for the input side (how
   to organize forms across more than just `wspy-run` — see #9) and the output side (a single-run
   report page, a compare-two-or-more-runs view, and a historical index/search view — see #8/#11).
   Explicitly evaluate static-file-only rendering (works from `file://` or any static host, no
   backend process) against a thin dynamic backend, since that choice shapes #8's report studio, #9's
   launcher, and #10's export format all at once. Deliverable is throwaway mockups plus a short
   writeup of the chosen direction and open tradeoffs, not production code — expect #6-14 to be
   revised once feedback lands. First pass of mockups produced 2026-07-11 (see chat/session record);
   treat those as a starting point for discussion, not a final layout. Feedback round 1 (same day)
   converged on: per-tool tabs at the top level (#9) with a *multi-select* capability checklist inside
   the Run tab rather than mutually-exclusive presets — counters/tree/interval/GPU/IBS are each
   independently toggleable with their own sub-customization, composing into one `wspy-run
   --profile a,b,c` invocation where the CLI already supports that and falling back to separate
   command lines (with an inline explanation) where it doesn't yet; a reserved, disabled row for
   future `/proc` extras (4.2 Tier 3) so that expansion has a natural slot without pre-building it;
   defaults-on toggle chips for manifest/run-index/store-ingest instead of opt-in checkboxes; and a
   local-vs-shared deployment toggle answering #13 directly (local executes with live output, shared
   stays copy-only). That round also surfaced a real gap worth tracking: `wspy-run --profile` only
   accepts its own named, pre-baked profiles, so a custom counter selection or `--interval` sampling
   can't be composed with tree/GPU/IBS into a single invocation the way two named profiles can — the
   mockup's fallback is separate command lines per capability; the real fix would be teaching
   `wspy-run` to accept an ad-hoc flags-based pass alongside named profiles, which isn't scoped as its
   own backlog item yet.
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
9. Web-based run launcher + report browser — a thin web form over `wspy-run`'s option surface
   (profile/suite/benchmark/workload command) that constructs and executes the command, then links
   to the resulting output once it lands. Most valuable once #8's report studio exists to link
   to, but the launcher half itself has no hard dependency on Tier 1's normalized store and could
   ship early. Motivated directly by 4.0's release-testing experience — hand-typing long `wspy-run`
   invocations while validating flag combinations was real, repeated friction, and a form would make
   the real-hardware hand-testing gaps carried into 4.1 (IBS, GPU, `wspy-validate`/`wspy-ledger` at
   scale — see "Known gaps carried into 4.1") faster to iterate on too. Scope is broader than
   `wspy-run` alone: the same form-based UI should also cover `wspy-validate` (manifest quality
   checks), `wspy-store`/`wspy-summary` (ingest + query the normalized store), and discovery-only
   commands (`wspy --capabilities`, `wspy --preflight`) that have no run to launch, just a report to
   render. #5's mockup pass should settle the layout question (one unified form with a
   command-type selector vs. per-tool tabs vs. a task-first wizard) before this is built — whichever
   shape wins, it stays a thin wrapper that assembles/runs/shows the equivalent CLI invocation rather
   than reimplementing logic that already lives in the C tools. Organize the Run tab specifically
   around the preset/configuration/option hierarchy (see the deep-dive above): named preset shortcuts
   that populate a multi-select configuration+option checklist, with a live indicator of whether the
   current selection still matches a named preset or has been customized into separate-command-line
   territory. #7's `wspy-run` profile launcher is this item's starting point, not a separate effort —
   generalize its command-display/local-execute plumbing (itself inherited from #6) into the real
   checklist rather than rebuilding it.
10. Publish-ready data export format — takes #8's curated block sequence (heading/image/preformatted/
    commentary, per selected configuration) and renders it in more than one target format, not just a
    bulk data dump:
    - **WordPress block markup (Gutenberg comment format)** — the recommended default once a real
      implementation exists. Pasting this into the WordPress block editor produces separately-editable
      native blocks (a real heading block, a real image block, a real preformatted block, ...) instead
      of one opaque blob, which is exactly what makes a report easy to keep tweaking *inside* WordPress
      afterward rather than only by regenerating and re-pasting from here.
    - **Self-contained inline-styled HTML** — for a WordPress "Custom HTML" block or any CMS that just
      wants raw HTML; easiest to paste, hardest to edit again afterward. Kept as an option, not the
      default, once the block format exists.
    - **Markdown** — portable, for anywhere that takes it directly or as a conversion source.
    Real images (gnuplot/chart output) need actual uploaded files for the WordPress/HTML formats to
    reference — the mockup stands these in with a live chart preview and a placeholder path, which is a
    real gap between mockup and implementation, not a detail to gloss over when this is built.
11. Historical run index browser/search.
12. Shared plotting templates — replace `workload/phoronix/gnuplot.sh`'s per-suite script with one
    normalized-schema pipeline once #1 exists. This is what closes #10's real-image gap: the studio
    (#8) and export (#10) currently stand in a placeholder for every chart because nothing yet
    generates the actual `plots/*.png` a real report would reference — this item is that generator,
    not a separate concern from the reporting work above. #6's slice and #7's `wspy-run` launcher
    deliberately keep calling `gnuplot.sh` as-is rather than pre-empting this generalization.
13. Deployment/hosting design note — answer, for both a person browsing their own local run output
    and a team publishing to a shared site: does #8/#11 need to run anywhere besides `localhost`, is a
    static site (generated files, no server process) sufficient, and if not, what's the smallest
    backend that covers both cases? Feeds #5's mockup pass directly and should land before #9's
    launcher decides how it invokes `wspy-run` (local subprocess vs. something that only makes sense
    on a single machine).
14. Traceability links (summary row → manifest → raw CSV → plots → tree artifacts) — closes the
    "every published row traces back to command/environment/artifacts" criterion deferred from 4.0
    (see "Success criteria for a 4.0 kickoff").
15. Report commentary/annotation — free-text notes saved alongside a report (session-local in the
    mockup; a real implementation needs a place to persist it — most naturally the normalized store
    from #1, keyed to the run — so it survives a report being regenerated). Scoped *per configuration*
    ("what does this tell us," attached to that configuration's block in #8) plus one overview note for
    the report as a whole — not a single global field — matching the real precedent's pattern of
    commentary interspersed between each chart/output block rather than one summary at the end.
    Speculative/lower priority within this tier; flagged explicitly as forward-looking by 4.1 feedback
    rather than a firm requirement. Future extension, not in scope now: an optional local-LLM-drafted
    starting point generated from a configuration's own metrics, which the user edits rather than
    writes from scratch — shown in the mockup as a visibly disabled affordance, not a real integration.
16. Structured configuration provenance — record a run's preset/configuration/option choices (see the
    deep-dive above) as structured data in `manifest.h`/`run_index.h`, not just the flat command line
    and flag set already captured. This is what lets #8/#11's reports render "how this was run" in the
    same vocabulary the launcher uses, and lets #17's "customize & run again" restore exact launcher
    state from a report instead of re-parsing argv. A real schema change (bump
    `MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION`), not only a UI concern.
17. Browse-reports: relate each report's artifacts back to the preset/configuration/option choices
    that produced it (via #16), and add a "customize & run again" action that returns to the launcher
    pre-filled from the report rather than from scratch. The mockup demonstrates this in-session
    (capturing the launcher's live state into each simulated run and restoring it on demand); the real
    version depends on #16 actually persisting that state somewhere reports can read it back from.
18. Estimated runtime display in the `wspy-run` profile launcher (#7) — once benchmark is selected,
    show an estimated runtime for the run before it's launched. `phoronix-test-suite` reportedly has
    a run-time-estimate command that could source this for Phoronix benchmarks specifically (the same
    command #23 in the 4.2 "testing/docs" tier proposes using to size `--tree`'s pass timeout); not
    yet scoped for other suites (`cpu2017`, `pbbsbench`) or for arbitrary workload commands, which
    have no equivalent estimator today.

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
