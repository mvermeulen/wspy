# wspy profile cookbook & interpretation playbook

This is a reading guide for the analytical signals wspy's reporting layer (`wspy-summary`,
`wspy-archetype`, `phase.c`) attaches to a run or a group of runs: confidence, phase, comparability,
and (see the note at the end) clustering. It answers "what does this field actually mean, and what
should I do when it fires" — not the artifact *format* itself, which is `doc/ARTIFACT_CONTRACT.md`'s
job. Every example below is real captured output from a small synthetic dataset (3-4 runs of one
workload ingested through `wspy-store`), not invented — see each section for how it was produced, so
you can reproduce the shape yourself against your own store.

## Reading confidence

Two independent confidence signals exist today, at two different levels: `wspy-summary`'s
per-`(group,metric)` verdict (is this number trustworthy across repeated runs), and
`wspy-archetype`'s per-run confidence (is this run's *classification* trustworthy).

### `wspy-summary`'s verdict

Every reported bucket carries `cv_percent`, `ci95_low`/`ci95_high` (95% confidence interval of the
mean, Student's t), and a `verdict` column — `PASS` or `WARN:` followed by a comma-joined, fixed-order
list of reasons. Real captured example (4 runs of one workload, one run on a different CPU vendor):

```
group                   metric  n    min    max      mean   median  stddev    cv   ci95_low ci95_high verdict              out
/bin/cookbook_workload   retire  4   41.5   54.5   47.9062  47.8125  6.98613 14.58%   36.7913   59.0212 WARN:noisy,mixed-pmu   0
```

Reasons, always in this order when more than one applies:

- **`thin`** — fewer than `VERDICT_MIN_RUNS_FOR_CONFIDENCE` (3) runs contributed to this bucket.
  Statistics computed from 1-2 runs aren't wrong, just not enough to say anything about
  run-to-run variance yet — treat `stddev`/`cv_percent` as not-yet-meaningful, not as "zero variance."
- **`noisy`** — `cv_percent` (`stddev / |mean| * 100`) exceeds `--max-cv` (default **5.0**, one global
  threshold, not per-metric). In the example above, `cv_percent=14.58%` — real run-to-run variance,
  not a fluke, because one contributing run had a `degraded` phase tick (see "Reading phase output"
  below) that dragged its own per-run average down. **What to do**: don't treat the reported `mean` as
  a single trustworthy number — look at `min`/`max` and, with `--show-runs`, which specific runs are
  driving the spread (`wspy-summary --trace <hostname>:<run_id>` resolves one of them back to its raw
  artifacts).
- **`mixed-pmu`** — the bucket's contributing runs don't all share the identical
  `(cpu_vendor, counters_requested, counters_measured)` triple. This is **exact-match, not a numeric
  closeness threshold** — there's no principled "how different is different enough" for a coverage
  triple the way there is for a percentage, so *any* deviation from the bucket's first-seen signature
  trips it. In the example above, one of the four runs recorded `cpu_vendor=Intel` against the other
  three's `AMD` — the same-named `retire` column is computed from genuinely different topdown
  formulas per vendor (see `CLAUDE.md`'s Topdown deep-dive), so averaging across them is comparing
  two different things that happen to share a column name. **What to do**: don't average across a
  `mixed-pmu` bucket at all — re-run with `--group-by cpu_vendor` (or filter with `--hostname`) to
  split it into vendor-homogeneous buckets first.

`--strict` exits 1 on any non-`PASS` verdict — use it in an automated pipeline to catch a bucket that
needs a closer look before publishing, rather than silently averaging across noise or mixed hardware.

### `wspy-archetype`'s confidence

`wspy-archetype` classifies one run along six axes (see "Reading the archetype scorecard" below) and
reports its own `confidence` — `high`/`medium`/`low`/`insufficient-data` — plus `confidence_reasons`.
Real captured example (`--run cookbook-host:run2`, one of the same four runs above) — captured before
the `vectorization_density` axis (issue #227) and `memory_attribution`/`memory_attribution_locus` (4.3
"Composite attribution") existed, so this transcript predates those fields rather than omitting them:

```
resource_dominance=compute-bound
resource_dominance_pct=42.25
alternative=memory-bound
alternative_pct=37.50
confidence=low
confidence_reasons=narrow-margin,missing-parallelism_shape-data,missing-control_flow_style-data
```

- **`insufficient-data`** — no topdown L1 data at all (`--topdown`/`--topdown2` wasn't collected this
  run). Terminal case: nothing else about the run can be classified either, since `resource_dominance`
  is the headline axis every other confidence tier depends on.
- Otherwise, confidence is driven by **margin** (the gap in percentage points between the winning
  topdown category and its runner-up) and **known** (how many of the 4 supporting axes —
  `parallelism_shape`/`control_flow_style`/`runtime_stability`/`vectorization_density` — had data):
  - `high`: margin ≥ 20 points **and** ≥ 2 supporting axes known.
  - `medium`: margin ≥ 10 points **and** ≥ 1 supporting axis known.
  - `low`: anything less — in the example above, `compute-bound` (42.25%) barely edges out
    `memory-bound` (37.50%), a margin of under 5 points, so `low` fires even though the run otherwise
    looks unremarkable.
- **`narrow-margin`** — the margin itself was under the `high` threshold (20 points); appears whenever
  it applies, even for a `medium`/`low` verdict, so you can tell a *close call* apart from *missing
  data* as the reason confidence isn't `high`.
- **`missing-<axis>-data`** (one per unavailable supporting axis) — that axis needs a flag this run
  didn't use: `parallelism_shape` needs `--per-core`, `control_flow_style` needs `--branch`,
  `runtime_stability` needs `--interval`, `vectorization_density` needs `--float` (AMD only).
  **What to do**: if you want a `high`-confidence classification routinely, collect `--per-core
  --branch --interval` alongside topdown, not just topdown alone.

## Reading phase output

`phase.c` classifies each `--interval` tick as `warmup`/`steady`/`degraded` from that tick's IPC —
disabled under `--per-core`, `--no-ipc`, or without `--interval` at all (`--no-phase-detect` opts out
explicitly). A run's final CSV row and human summary both carry the last tick's own classification
(confirmed live, a short `sleep 3` run under `--interval 1` with no perf access here still degrades
gracefully and reports its phase):

```
phase                warmup
```

The exact thresholds (`phase.h`'s own documented constants): warmup ends once a 3-sample rolling
window's coefficient of variation drops below 15%, at which point that window's mean becomes the
steady-state baseline; a sample landing more than 20% below baseline is a *candidate* degraded
transition that only commits after 2 consecutive candidate ticks (one noisy sample can't flip the
label on its own); recovery back to `steady` needs a 10-point hysteresis margin, so a sample sitting
right at the boundary doesn't flap the phase every tick; the baseline itself drifts by a small
exponential-moving-average weight (10%) while steady, so slow legitimate drift isn't mistaken for
degradation.

In human (non-CSV) output only, a trailing summary lists every transition with its elapsed-time
timestamp — illustrative shape (`phase.c`'s own format string, `"  %6.1fs  %-8s -> %s\n"`):

```
phase boundaries:
     3.2s  warmup   -> steady
    18.7s  steady   -> degraded
    24.1s  degraded -> steady
```

CSV output deliberately has no second, redundant representation of these boundaries — a downstream
reader can already reconstruct them by diffing adjacent rows' `phase` column.

**What to do**: a `degraded` tick isn't automatically a problem — it's a signal to go look at *why*
(a real hardware stall vs. the process merely not being scheduled — cross-reference `--tree-schedstat`'s
`run_delay_seconds` if you collected it, which distinguishes "runnable but not scheduled" from a
genuine on-CPU stall). When summarizing a multi-tick run into one number (as `wspy-store`'s
`ingest_csv_metrics()` does for `wspy-summary`), a `warmup`/`degraded`-heavy run will pull its own
per-run average away from the `steady`-phase number you probably actually care about — this is
exactly the mechanism behind the `noisy` verdict example above, where one run's single `degraded` tick
(IPC dropped to 0.6 against a steady ~1.8) dragged that run's own `retire` average down relative to
its peers.

### Reading `--phase-topdown` output

`wspy-summary --phase-topdown <hostname>:<run_id>` answers the question the paragraph above raises
directly for one run's own topdown columns, instead of leaving you to eyeball a CSV diff: does this
column's value genuinely *shift* between phases, and by how much. Real captured example (a real
compute-then-pointer-chase run, `--topdown --interval 1`, two phases actually observed — `warmup` and
`steady`, no `degraded` transition this particular run):

```
metric    warmup       steady       degraded   drift_pct
retire    12.87(n=3)   8.94(n=5)    -          3.93
backend   83.33(n=3)   90.82(n=5)   -          7.49
wspy-summary: largest phase drift: backend (7.49 pts, between warmup and steady)
```

Each cell is that column's own mean *within* the named phase, with its own tick count `n` — a phase
this run never reached (here, `degraded`) prints `-`, never a fabricated 0 or an extrapolated value.
`drift_pct` is the largest phase-to-phase swing for that column specifically; the trailing note names
the single largest drifter across every topdown column in the run — here, `backend` swinging 7.49
points between `warmup` and `steady` is the biggest single shift, even though `retire` also moved.

**What to do**: a large `drift_pct` on a column that matters to your investigation (e.g. `backend` for
a suspected memory-bound workload) means the run's own topdown read isn't uniform across its
lifetime — the single aggregate number `wspy-summary`'s normal bucket view reports can hide a
`warmup`-phase read that looks nothing like the `steady`-phase one it's actually averaged together
with. A run collected without phase data (aggregate, `--per-core`, or no `--interval`) degrades to an
explicit notice rather than an empty table — `--phase-topdown` needs `--interval` plus IPC counters
plus phase detection all active on the original run, same gating `phase.c` itself uses.

## Reading comparability signals

Three real, shipped comparability mechanisms, from coarsest to most detailed:

- **`mixed-pmu`** (see above) — exact-match on `(cpu_vendor, counters_requested, counters_measured)`,
  catching the case where a same-named column was computed from genuinely different hardware or a
  degraded counter setup on one contributing run.
- **`--group-by cpu_governor` / `--group-by virt_role`** — real, shipped grouping columns pulled from
  `provenance.c`'s environment capture (CPU frequency-scaling governor, host-vs-guest virtualization
  role). Use these when you suspect an apparent difference between two buckets is actually explained
  by an environment difference rather than the workload itself — e.g. `wspy-summary --group-by
  cpu_governor --metric ipc` splits a mixed `performance`/`powersave`-governor dataset into its own
  buckets instead of averaging across a difference that has nothing to do with the code being measured.
- **`env_score`/`mixed-env`** — a single composite comparability *score*, every bucket, every time
  (no `--group-by` needed to notice it): the fraction of 8 tracked `provenance.c` fields (`virt_role`,
  `hypervisor_vendor`, `microcode_version`, `bios_vendor`/`bios_version`/`bios_date`, `cpu_governor`,
  `memory_total_kb`) that agreed across a bucket's contributing runs. Real captured example (4 runs of
  one workload, same command, deliberately varied environment provenance — one re-run with a flipped
  governor, one collected inside a VM on different hardware):

  ```
  group    metric   n      mean  ...  env_score  ...  verdict
  phasey   backend  4   88.0125  ...          0  ...  WARN:mixed-env
  ```

  `env_score=0` here because every tracked field disagreed somewhere across the 4 runs (the VM run
  alone differs on `virt_role`/`hypervisor_vendor`/`bios_*`/`memory_total_kb`; the governor-flip run
  differs on `cpu_governor`). `--min-env-score` (default 0.8) is the threshold below which `mixed-env`
  fires — a single disagreeing field among otherwise-identical runs (e.g. just the governor flip, no
  VM run) stays *above* 0.8 and the bucket still passes; it takes several fields disagreeing, or one
  run from genuinely different hardware, to actually trip it. A bucket with zero mutually-comparable
  fields (no `run_environment` data collected at all) gets an explicit no-data sentinel (`-`), never a
  fabricated 0 or 100 — absence of provenance data is not evidence of a mismatch either way.

  **What to do**: `mixed-env` firing doesn't by itself say *which* field is the outlier — cross-reference
  `--group-by cpu_governor`/`--group-by virt_role` (or query `run_environment` directly) to find it, the
  same way `mixed-pmu`'s "what to do" above points at `--group-by cpu_vendor`. `--check-regression`
  reports the same `env_score` on its baseline row too, for the identical reason: a flagged
  `above`/`below` deviation is far less trustworthy if the baseline it's compared against turns out to
  be `mixed-env` itself.

## Reading the archetype scorecard

`wspy-archetype` (`wspy-store`'s `run_features` → six classified axes) is the closest thing to a
"workload profile" this toolset produces today. Real captured output (bulk mode, one row per run) —
captured before the `vectorization_density` axis (issue #227) existed, so this row's columns predate
it rather than showing it as `unknown`:

```
hostname       run_id  command                 resource_dominance alternative  parallelism  control_flow  stability  conf.   reasons
cookbook-host  run1    /bin/cookbook_workload   compute-bound      memory-bound unknown     unknown       phased     medium  missing-parallelism_shape-data,missing-control_flow_style-data
```

- **`resource_dominance`** (the headline axis, always paired with a top-2 `alternative`) —
  `compute-bound`/`frontend-bound`/`memory-bound`/`speculation-bound`, ranked from topdown L1
  percentages. Read the *margin* between primary and alternative (also what drives `confidence`
  above) as much as the label itself — a workload at 42%/37% is a much less clean-cut case than one
  at 70%/15%, even though both would print `compute-bound`.
- **`parallelism_shape`** (`balanced-parallel`/`imbalanced`, needs `--per-core`) — cross-core IPC
  coefficient of variation; `unknown` (as in the example, since this dataset didn't collect
  `--per-core`) rather than a guess when the data wasn't collected.
- **`control_flow_style`** (`straight-line`/`branch-heavy`, needs `--branch`) — from branch
  mispredict rate.
- **`runtime_stability`** (`steady`/`phased`/`erratic`, needs `--interval`) — directly derived from
  the fraction of `phase.c`-classified `steady` ticks (see "Reading phase output" above) — `phased`
  here reflects that this run's own interval data included a real `warmup`→`steady`→`degraded`→
  `steady` sequence, not a clean, uniformly `steady` run throughout.
- **`vectorization_density`** (`low`/`moderate`/`high`, AMD only, needs `--float`) — threshold on
  `float_pct` (AMD FP-op density, `float_all/instructions*100`); `high` corroborates a `compute-bound`/
  `memory-bound` read that's genuinely FP-vector-heavy rather than integer-dominated, the distinction
  issue #227's GCC-optimization-guide motivation needed to gate vector-width-tuning flag suggestions
  (`-mprefer-vector-width=256`/`512`) on more than `resource_dominance` alone.
- **`memory_attribution`** (needs no extra flags beyond topdown — strengthened by `--dcache`/
  `--cache2`/`--cache3`/`--tlb`/`--ibs-sample`/`--tree-io-wait`/`--tree-schedstat`) — unlike the axes
  above, not a threshold on a single value: cross-references `resource_dominance`'s own `backend_pct` against
  every independently-measured cache/TLB/IBS signal the run collected, rather than trusting a
  "memory-bound" topdown read on its own. Real captured example (a real pointer-chase workload,
  `--topdown --dcache --cache2`):

  ```
  resource_dominance=memory-bound
  resource_dominance_pct=80.90
  memory_attribution=uncorroborated
  memory_attribution_reasons=checked:dcache_miss_pct,checked:l2_miss_pct,checked:smt_contention_pct
  ```

  `backend_pct=80.9%` clears the significance floor easily, but every signal that was actually
  *collected* to corroborate it (`dcache_miss_pct=8.76%`, `l2_miss_pct=0.56%`) sat below the elevated
  threshold — genuinely interesting information a raw topdown read can't surface on its own: either
  the real bottleneck here is something none of the collected signals capture (this workload's
  dependent-load pointer chase is latency-, not miss-rate-, dominated at these array sizes), or the
  signal set collected this run just isn't the discriminating one. **What to do**: `uncorroborated`
  is not "wrong" or "ignore this run" — it's a prompt to either collect a more targeted signal
  (`--ibs-sample` for a real per-sample memory-data-source breakdown, if this is AMD hardware) or to
  treat the "memory-bound" read as provisional rather than settled. Two more outcomes
  (`blocked`/`oversubscribed`) take priority over this cache/TLB/IBS corroboration check entirely,
  for a run collected with `--tree-io-wait`/`--tree-schedstat`/`--tree-futex`: a "memory-bound"
  topdown read on a phase that was actually blocked on the kernel or just not scheduled isn't a
  genuine hardware stall to begin with, so asking whether cache counters "corroborate" it would be
  the wrong question.

  On AMD hosts, a `corroborated` result additionally gets **`memory_attribution_locus`** —
  *which* cache level the stall concentrates in (`l1`/`l2-l3`/`dram`/`remote-numa`, plus a
  `tlb-cofire` co-firing tag), from `--ibs-sample`'s per-sample hit-outcome tags where available,
  falling back to plain miss-rate signals otherwise (`memory_attribution_locus_reasons` names which
  tier/signal decided it, always prefixed `tier=ibs-sample`/`tier=cache-counter` so the two
  precision levels are never mistaken for each other) — the AMD/IBS-side answer to a question
  Intel/ARM's `--topdown-backend` L1/L2/L3/DRAM stall-cycle chain already answers directly.

**What to do**: treat the 4 supporting axes (and `memory_attribution`, when it applies) as
independent tags around the one headline classification (`resource_dominance`), not a single
combined label — a `compute-bound` / `imbalanced` / `branch-heavy` / `erratic` run and a
`compute-bound` / `balanced-parallel` / `straight-line` / `steady` run are both "compute-bound," but
they're very different workloads, and the axes are what tell you that apart.

## Nearest-neighbor search and clustering (`--nearest`, `--kmeans`)

Unlike `--run`'s rule-based classification above (thresholds on individual metrics), these two modes
compare runs to each other directly, over the same `run_features` vocabulary. Both compute a
**coverage-aware, z-score-standardized RMS distance**, counted only over the dimensions both runs
actually have `coverage='measured'` — a pair sharing 6 features isn't penalized just for sharing less
than a pair sharing 18. Population mean/stddev for standardization are computed once across every
candidate, so every pairwise distance shares the same scale.

**`wspy-archetype --db <path> --nearest <host>:<run_id> [--k N] [--csv]`** ranks every other run in the
store by similarity to the named one, most similar first, printing `--k` of them (default 5):

```
hostname:run_id                            distance  compared_features
roswell:20260801T151107.433-1157536         0.06405                  5
roswell:20260803T112631.909-710276           0.7518                  5
```

- **distance** — lower means more similar. Not bounded to a fixed range, so only meaningful *relative
  to the other rows in the same ranking*, never as an absolute score, and never comparable across two
  different `--nearest` invocations (the population stats it's standardized against can differ).
- **compared_features** — how many `run_features` dimensions both runs shared. A `distance` computed
  over 2 shared features and one computed over 15 aren't equally trustworthy even if the numbers look
  similar — always read them together, not `distance` alone.
- `--command`/`--hostname` filter the candidate pool the same way they filter `wspy-summary`'s bucket.
- Exit 1 if the target run isn't in the store at all; exit 0 with an empty result if it has no measured
  features or nothing else in the store shares any — not an error, just nothing to rank.

**`wspy-archetype --db <path> --kmeans <n> [--seed <n>] [--iterations <n>] [--csv]`** partitions every
candidate run into `n` clusters over the same distance, printing one row per member grouped by cluster,
closest-to-centroid first. Each member's row carries a **profile card**: the `KMEANS_TOP_FEATURES`
dimensions where that cluster's centroid sits furthest from the population mean — the "what makes this
cluster distinctive" summary, not every feature. A centroid, per dimension, is the *available-case
mean* (averaged only over members that actually measured that dimension) — the same "only shared
dimensions count" idiom `--nearest`'s distance already uses, just applied to a group instead of a pair.
Same seed + same data always yields the same clustering (`--seed`, default 1, seeds k-means++
initialization from real data points).

Read the web report's "Similarity panel" (a `--nearest` snapshot for that run, linked to each
neighbor's own report) as the everyday entry point to this — reach for the raw CLI modes above when you
need `--kmeans`, a custom `--k`, or `--command`/`--hostname` filtering the panel doesn't expose.

(Separately, and unrelated to this section despite the shared word: `wspy --capabilities` reports
real ARM PMU **hardware** clusters — `cpu_info.c`'s `discover_arm_pmu_topology()`, e.g. `"ARM PMU
topology: 2 cluster(s)"` — and `--list-affinity` reports L3-domain/core-type topology groups. Both
are topology *discovery*, not an interpretive/statistical signal, so they're out of this cookbook's
scope.)

## Putting it together: reading one bucket end to end

Given the real `WARN:noisy,mixed-pmu` bucket captured above, here's the reasoning chain this cookbook
is meant to shortcut:

1. **`mixed-pmu` fires first** — one contributing run recorded a different `cpu_vendor`. Stop:
   averaging `retire` across an AMD run and an Intel run isn't meaningful regardless of anything else,
   since the two vendors compute that topdown category from different raw events. Re-run
   `wspy-summary --group-by cpu_vendor` to split the bucket before drawing any conclusion from it.
2. Within the AMD-only split, **`noisy` still likely fires** (the three-run `cv_percent` from the
   first captured example was already 13.53%, above the 5% threshold) — trace it with `--show-runs`
   to find which run is the outlier, then `wspy-archetype --run <that run>` to check its
   `runtime_stability`: in this dataset it comes back `phased`, and the underlying CSV shows exactly
   one `degraded` tick amid an otherwise-`steady` run.
3. That one `degraded` tick is *why* the bucket is noisy — not a data quality problem, a real
   phase transition in one run that the others didn't have. Whether that's worth investigating
   further (a genuine hardware stall vs. scheduling contention) is exactly what `--tree-schedstat`'s
   `run_delay_seconds` (if collected) or a closer look at that run's own topdown breakdown would tell
   you next.
