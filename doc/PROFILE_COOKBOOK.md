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

`wspy-archetype` classifies one run along four axes (see "Reading the archetype scorecard" below) and
reports its own `confidence` — `high`/`medium`/`low`/`insufficient-data` — plus `confidence_reasons`.
Real captured example (`--run cookbook-host:run2`, one of the same four runs above):

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
  topdown category and its runner-up) and **known** (how many of the 3 supporting axes —
  `parallelism_shape`/`control_flow_style`/`runtime_stability` — had data):
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
  `runtime_stability` needs `--interval`. **What to do**: if you want a `high`-confidence
  classification routinely, collect `--per-core --branch --interval` alongside topdown, not just
  topdown alone.

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

## Reading comparability signals

Two real, shipped comparability mechanisms exist today — there is no single composite "comparability
score" yet (that's still a 4.3 backlog item, "Machine/environment comparability scoring," broader
than either of these):

- **`mixed-pmu`** (see above) — exact-match on `(cpu_vendor, counters_requested, counters_measured)`,
  catching the case where a same-named column was computed from genuinely different hardware or a
  degraded counter setup on one contributing run.
- **`--group-by cpu_governor` / `--group-by virt_role`** — real, shipped grouping columns pulled from
  `provenance.c`'s environment capture (CPU frequency-scaling governor, host-vs-guest virtualization
  role). Use these when you suspect an apparent difference between two buckets is actually explained
  by an environment difference rather than the workload itself — e.g. `wspy-summary --group-by
  cpu_governor --metric ipc` splits a mixed `performance`/`powersave`-governor dataset into its own
  buckets instead of averaging across a difference that has nothing to do with the code being measured.

**What's not here yet**: a report doesn't currently score "how comparable are these two environments"
as a single number or verdict — you have to know to reach for `--group-by` yourself. If a future
`wspy-summary` version adds that scoring, this section is the place it'll be documented.

## Reading the archetype scorecard

`wspy-archetype` (`wspy-store`'s `run_features` → four classified axes) is the closest thing to a
"workload profile" this toolset produces today. Real captured output (bulk mode, one row per run):

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

**What to do**: treat the 4 axes as independent supporting tags around the one headline
classification (`resource_dominance`), not a single combined label — a `compute-bound` /
`imbalanced` / `branch-heavy` / `erratic` run and a `compute-bound` / `balanced-parallel` /
`straight-line` / `steady` run are both "compute-bound," but they're very different workloads, and
the axes are what tell you that apart.

## Statistical clustering — not yet shipped

The backlog line this cookbook was written to close out also names "cluster" output. There is
**no statistical clustering/nearest-neighbor feature in this codebase today** — it's a distinct,
not-yet-built 4.3 priority ("Clustering + nearest-neighbor + cluster profile cards," needs 4.1's
normalized store and history, which do exist). A 2024 external clustering analysis
(`archetype.c`'s own header comment cites it) informed `resource_dominance`'s axis design, but no
clustering algorithm runs anywhere in this repo — don't confuse `wspy-archetype`'s rule-based
classification above with a learned/statistical cluster assignment. This section will be filled in
once that item ships, rather than describing something that doesn't exist yet.

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
