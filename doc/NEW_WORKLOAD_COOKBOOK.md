# Onboarding a new workload suite: a wspy cookbook & tutorial

A step-by-step walkthrough for characterizing a *new* benchmark suite with wspy end to end — from "does
it even run under wspy" to "which benchmarks are memory-bound, which are frontend-bound, are any of them
secretly I/O-blocked instead of hardware-stalled, and how do I keep tracking that as the suite grows."
Written with SPEC CPU2026 in mind (released 2026-05-05, https://spec.org/cpu2026/ — paid license, four
suites: SPECspeed 2026 Integer/Floating Point and SPECrate 2026 Integer/Floating Point, 52 benchmarks
total, still launched via `runcpu` per the official docs, same as CPU2017), but every step generalizes to
any new suite — pbbsbench, a new Phoronix test, an internal benchmark, whatever shows up next. This
document deliberately doesn't enumerate CPU2026's actual 52 benchmark names or `runcpu` config-file
syntax (not published on the landing page at the time of writing, and orthogonal to the wspy side of
this walkthrough anyway) — pull those from the official CPU2026 documentation once you have a license,
and drop them into `workload/cpu2026/` (Step 2) the same way `workload/cpu2017/` is already structured.
One real CPU2026-specific detail worth flagging up front: it adds an official optional energy-consumption
metric (per the same landing page) — worth cross-checking against wspy's own independent `--power` (RAPL
`energy-pkg`) measurement once you're running real CPU2026 binaries, the same way Step 12 cross-checks a
topdown read against IBS's independent per-sample data. Not its own numbered step here (no live capture
to demonstrate it against yet), but a natural one to add once real energy-metric-reporting CPU2026 runs
exist to validate against.

**Two things this document is for, deliberately combined**: a real onboarding guide for a new suite, and
a guided tour that exercises a wide swath of wspy's own functionality — the CLI tools, the normalized
store, the classification/clustering layer, per-core and phase-aware diagnostics, IBS sampling, and the
suite-orchestration layer. Working through it is itself a manual test pass. Places where a step would be
worth scripting once you've done it by hand a few times are called out explicitly as **Automation
opportunity** — that's the other thing to watch for as you go.

**Don't have a CPU2026 license in hand yet?** Every wspy command below is demonstrated against a tiny,
fully reproducible 3-program toy suite (source included, see "The toy suite" below) that you can build
and run right now, with zero dependency on any real benchmark package. Every captured example in this
document is real output from that toy suite on this repo's own dev host — not invented. Each step also
has a "**When this is SPEC CPU2026**" note translating it to the real thing. Work through the toy suite
first to get comfortable with the commands and the shape of the output; the SPEC pass later is then just
"same commands, real binaries, more of them."

## Prerequisites

- `make` (or `make AMDGPU=1`/`make NVIDIA=1` if this host has a GPU you also want data from).
- Perf counter access: `scripts/setup_perf.sh` grants `wspy` the `CAP_PERFMON` file capability it needs
  for `--power`, AMD IBS (`--ibs-basic`/`--ibs-memory-deep`/`--ibs-sample`), and `--per-core` without
  running every single invocation as root. **This capability is a file attribute tied to the current
  binary's inode — it's dropped on every rebuild** and needs re-running after `make`. `--tree` (ptrace)
  needs root or `CAP_SYS_PTRACE` + `perf_event_paranoid <= 1` separately.
- `gcc` (only for building the toy suite below — not needed once you're on real SPEC binaries).
- `gnuplot` if you want `wspy-plot` to render PNGs (checked at runtime, not a build dependency).

## The toy suite

Three tiny C programs standing in for three different resource-dominance shapes, so clustering/bucketing
later in this walkthrough has something real to separate. Build them once:

```
mkdir -p /tmp/toybench && cd /tmp/toybench

cat > compute.c <<'EOF'
#include <stdio.h>
int main(void){
  unsigned long a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8;
  long i;
  for (i = 0; i < 2000000000L; i++){
    a = a*2654435761UL + 1; b = b*2654435761UL + 1;
    c = c*2654435761UL + 1; d = d*2654435761UL + 1;
    e = e*2654435761UL + 1; f = f*2654435761UL + 1;
    g = g*2654435761UL + 1; h = h*2654435761UL + 1;
  }
  printf("%lu\n", a+b+c+d+e+f+g+h);
  return 0;
}
EOF

cat > membound.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (64*1024*1024)
int main(void){
  int *a = malloc(N*sizeof(int));
  long i,j,sum=0;
  srand(42);
  for (i=0;i<N;i++) a[i]=rand()%N;
  for (j=0;j<6;j++){
    long idx=0;
    for (i=0;i<N;i++){ idx = a[idx]; sum += idx; }
  }
  printf("%ld\n", sum);
  return 0;
}
EOF

cat > branchy.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(void){
  long i,sum=0;
  srand(7);
  for (i=0;i<300000000L;i++){
    if (rand() % 2) sum += i; else sum -= (i%3);
  }
  printf("%ld\n", sum);
  return 0;
}
EOF

gcc -O3 -o compute compute.c
gcc -O2 -o membound membound.c
gcc -O2 -o branchy branchy.c
```

Each runs in 2-4 seconds — short enough to iterate on quickly, long enough for wspy's 2-second
pre-launch counter-arming window not to dominate. Real captured topdown shape for each, so you know what
to expect once you get to Step 6:

| toy binary | dominant topdown category | why |
|---|---|---|
| `compute` | backend (~54%), retire (~46%) — **narrow margin, low confidence** | tight integer multiply-add dependency chains; a good example of a workload that *looks* compute-bound by name but isn't a clean topdown call |
| `membound` | backend (~89%), high confidence | pointer-chasing traversal over a 256MB array — genuinely cache/TLB-thrashing |
| `branchy` | frontend (~58%) + speculation (~14%) | `rand()`-driven unpredictable branches |

**When this is SPEC CPU2026**: obviously skip building the toy suite — you'll have real `runcpu`-built
binaries instead. Everything from Step 1 onward reads the same either way; only the workload command
line changes.

## Step 1 — First contact: does it even run, and what can this host measure?

Before spending time on a real capture, check what this host can actually collect and whether your
intended counter set fits the hardware budget.

```
./wspy --capabilities
```

Real output shape (trimmed):

```
counter capability report: 75/75 available
  available           software     cpu-clock
  available           ipc          instructions
  available           topdown      cpu-cycles
  ...
```

A count below the total (e.g. `70/75`) tells you up front which counters this host/kernel can't provide
— cheaper to find out now than after a long batch run. Then check whether your planned counter set
actually fits the PMU budget before committing to it:

```
./wspy --preflight --counters=topdown,cache2,branch,tlb
```

Real output:

```
counter-fit preflight: 29/6 general-purpose hardware PMU counter slot(s) requested
  group               branch       9 counter(s), disable: --no-branch
  group               topdown      6 counter(s), disable: --no-topdown
  group               l2 cache     6 counter(s), disable: --no-cache2
  group               cache        5 counter(s), disable: --no-tlb
  group               ipc          3 counter(s), disable: --no-ipc
  fit                 WILL MULTIPLEX -- 23 slot(s) over budget
```

`WILL MULTIPLEX` isn't fatal — wspy scales multiplexed counters back up automatically and reports a
per-counter confidence based on how much of the window each one was actually scheduled — but it's worth
knowing before you interpret a number as precise. `wspy --list-affinity` is worth a look too, especially
on a hybrid or NUMA host (P-core/E-core, Zen5/Zen5c, multiple L3 domains) — it tells you what
`--affinity=coretype=<id>`/`domain=<id>` groups exist, which matters once you get to per-core diagnostics
in Step 10.

Now run one benchmark by hand, human-readable, no CSV, just to see it work:

```
./wspy --counters=topdown -- /tmp/toybench/membound
```

Real captured output:

```
elapsed              3.564
cpu-cycles           12557738161    # 0.11 GHz
instructions         6272692350     # 0.50 IPC low
slots                100561334248   #
retiring             5827533334     #  5.8% ( 5.8%) low low-confidence(50%)
frontend             4316762485     #  4.3% ( 4.3%) low low-confidence(50%)
backend              89676763864    # 89.2% (89.7%) high low-confidence(50%)
speculation          169376907      #  0.2% ( 0.2%) low low-confidence(50%)
smt-contention       537590846      #  0.5% ( 0.0%)
sanity check         100.0% of slots
counter coverage     9/9 measured
```

**What to look for**: `counter coverage N/N measured` (did every requested counter actually get
scheduled, not just requested), `sanity check` close to 100% (the four topdown L1 categories should sum
to ~100% by construction — a real drift means a measurement problem, not a workload characteristic), and
the `low-confidence(50%)` annotations here specifically — this run multiplexed (9 counters over a
6-slot budget from the preflight check above), so treat the exact percentages as estimates, not precise
counts. `retiring`/`backend`/etc. print two percentages: the first is share of *all* slots (contention
included), the parenthetical is share of `slots_no_contention` — the second one is what CSV/downstream
tooling actually uses.

**When this is SPEC CPU2026**: run one `runcpu --action=run` invocation under
`wspy --counters=topdown -- runcpu ...` for one new benchmark before committing to a full
batch — the same "does it run, what's the coverage" sanity check.

## Step 2 — Decide what to collect, and stop hand-rolling the flags

A single-shot `wspy --counters=...` invocation is fine for a spot check, but a real characterization pass
wants several different counter groups (topdown, cache, branch, an `--interval` time series) collected
consistently across every benchmark. `wspy-run` turns that into named, reusable profiles instead of a
hand-copied flag list per benchmark:

```
./wspy-run --list
```

Real (trimmed) output — the profiles you'll reach for most on a CPU-only characterization pass:

```
Builtin profiles:
  quick       one fast pass: ipc + system, human-readable output
  deep-cpu    systemtime/amdtopdown (--interval) plus one --passes sweep
              covering software/branch/ipc/topdown2/cache2/float/
              topdown-frontend/topdown-optlb, used for topdown
              characterization
  deep-cpu-intel  the Intel-only equivalent ...
  tree-heavy  single --tree pass with full command-line capture, wrapped in a
              3600s timeout ...
  ibs-sample  single pass: AMD IBS *sampling* mode ...
  zen4plus-deep    deep-cpu + ibs-sample + tree-heavy composed (AMD
              Family 19h+, i.e. Zen4/Zen5) ...
```

This exact `--suite`/`--benchmark` combination, run with a `deep-cpu,tree-heavy` profile list, is the
production pattern this repo already uses for SPEC CPU2017 — `workload/cpu2017/run_test.sh` is a real,
running example, not a hypothetical:

```sh
"$WSPY_RUN" --wspy "$WSPY" --suite cpu2017 --benchmark "$TESTNAME" \
    --run-id "$RUN_ID" -o "$OUTROOT" --run-index "${OUTROOT}/cpu2017/run-index.jsonl" \
    deep-cpu,tree-heavy -- \
    runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
```

**Automation opportunity**: fork `workload/cpu2017/` into `workload/cpu2026/` once CPU2026 lands — same
`run_test.sh`/`run_all.sh` shape, swap the `runcpu` invocation and benchmark list. This is genuinely the
fastest path to a working batch driver; don't design a new one from scratch.

For the toy suite (no `--suite`/`--benchmark` needed for a one-off), the direct equivalent:

```
./wspy --csv --counters=topdown,cache2,branch,tlb --dcache \
  --manifest run1.manifest.json --run-index toysuite-index.jsonl \
  -o run1.csv -- /tmp/toybench/membound
```

**What to look for**: `--csv` output has a machine-readable header/row instead of the annotated human
text above — same numbers, meant for ingestion, not reading directly.

## Step 3 — Run it enough times to trust the numbers

One run tells you nothing about run-to-run noise. Collect at least 3 repetitions per benchmark before you
draw any conclusion — `wspy-summary`'s own verdict logic (Step 6) treats fewer than 3 as `WARN:thin` and
won't compute a meaningful confidence interval. For the toy suite:

```
for bench in compute membound branchy; do
  for rep in 1 2 3; do
    ./wspy --csv --counters=topdown,cache2,branch,tlb --dcache \
      --manifest "$bench.$rep.manifest.json" --run-index toysuite-index.jsonl \
      -o "$bench.$rep.csv" -- "/tmp/toybench/$bench"
  done
done
```

Now get everything into the normalized store — this is the step that unlocks every analysis tool from
here on:

```
./wspy-store --db toysuite.db --run-index toysuite-index.jsonl
```

Real output:

```
toysuite-index.jsonl: 9 record(s): 9 new, 0 updated, 0 malformed, 0 collision(s); 9 manifest(s)
enriched, 0 skipped, 0 mismatched; 9 metric-set(s) ingested, 0 skipped, 0 row(s) mismatched; 9 run(s)
feature-extracted
toysuite.db: 9 total run(s) in store
```

Re-running `wspy-store` against a growing run-index is always safe — it's keyed on
`(hostname,run_id)` and upserts, so point it at the same suite-level run-index file every time you add
more benchmarks/repetitions rather than tracking separate per-batch databases.

**Automation opportunity**: this is the natural point to stop invoking `wspy-store` by hand — either
tack `wspy-store --db toysuite.db --run-index toysuite-index.jsonl` onto the end of your batch driver
script, or use `wspy-sweep` (Step 16) which does this automatically after every sweep.

## Step 4 — Sanity-check before you trust anything

`wspy-validate` runs a cheap set of pre-publish quality checks against each run's manifest — required
files present, CSV well-formed, workload actually exited cleanly, counter coverage, sanity ranges on the
numeric columns:

```
./wspy-validate compute.1.manifest.json membound.1.manifest.json
```

Real output:

```
compute.1.manifest.json: PASS
  [PASS] schema version 1.9.0 recognized
  [PASS] output CSV well-formed: 38 column(s), 1 data row(s)
  [PASS] sanity checks passed on 37 numeric value(s) across 1 row(s)
  [PASS] required files present (2/2)
  [PASS] workload exited normally (status 0)
  [PASS] full counter coverage: 31/31 counters measured
  [PASS] elapsed time 1.959s
2 manifest(s) checked: 2 passed, 0 warned, 0 failed
```

**Automation opportunity**: `wspy-validate --strict *.manifest.json` (nonzero exit on any WARN/FAIL) is
exactly the shape of a CI gate — run it right after the batch driver, before anything downstream trusts
the data. This is also the natural place to decide "did this benchmark actually validate/complete" for
SPEC's own reference-vs-run-output comparison, independent of what wspy measured.

## Step 5 — First read: the bulk scorecard

With data in the store, `wspy-archetype` in bulk mode gives you one row per run — the fastest way to see
the whole suite's shape at a glance:

```
./wspy-archetype --db toysuite.db --csv
```

Real (trimmed) output, one row per run:

```
hostname,run_id,command,resource_dominance,resource_dominance_pct,alternative,alternative_pct,...,memory_attribution,memory_attribution_reasons
sacramento,...,.../compute,memory-bound,53.9,compute-bound,45.8,...,corroborated,"l2_miss_pct=21,itlb_generic_miss_pct=37.63"
sacramento,...,.../membound,memory-bound,89.3,compute-bound,5.9,...,corroborated,"dcache_miss_pct=16.27,dtlb_generic_miss_pct=10.63"
sacramento,...,.../branchy,frontend-bound,57.5,compute-bound,27.4,...,not-memory-bound,
```

Notice `compute` — named for what it does, but wspy calls it `memory-bound` (a narrow 53.9%/45.8% margin,
which `resource_dominance_pct`/`alternative_pct` make visible directly, and which the fuller scorecard's
`confidence` column would report as `low`). That's the point of measuring instead of assuming: a tight
dependency chain and real cache/DRAM pressure both show up as "backend," and only the margin/confidence
plus the corroboration columns (Step 9) tell you which kind you're actually looking at.

For a single run's full detail (every axis, not just the CSV row), use `--run`:

```
./wspy-archetype --db toysuite.db --run sacramento:<run_id>
```

**What to look for**: `resource_dominance` is the headline; treat it alongside its margin and
`confidence`, not as a bare label — see `doc/PROFILE_COOKBOOK.md`'s "Reading the archetype scorecard"
for the full interpretation guide (confidence tiers, what `narrow-margin` means, what each supporting
axis needs collected to stop reading `unknown`).

**When this is SPEC CPU2026**: this is your first "what kind of suite did I actually get" pass — expect
a real mix (SPEC is deliberately built from varied workload shapes), and this table is the fastest way
to see that mix before doing anything else.

## Step 6 — Establish repeatability baselines

`wspy-summary` computes per-`(group,metric)` statistics across repeated runs — min/max/mean/stddev, a
95% CI, and a PASS/WARN verdict:

```
./wspy-summary --db toysuite.db --metric retire --metric backend --metric frontend --metric speculate
```

Real (trimmed) output for `membound`'s 3 reps:

```
group          metric      n    min    max     mean   stddev      cv verdict
.../membound   backend     3   89.1   90.2  89.5333 0.585947   0.65%   PASS
.../membound   frontend    3    4.2    4.7  4.46667 0.251661   5.63% WARN:noisy
.../membound   retire      3    5.5    6.0     5.80 0.264575   4.56%   PASS
```

`frontend`'s `WARN:noisy` here is real, small-sample noise (a 5.63% CV against the default 5.0%
threshold) on a metric that's a minor contributor to this workload's story anyway — not every WARN needs
chasing, but every WARN is worth *reading*, since sometimes it's the interesting signal (see
`doc/PROFILE_COOKBOOK.md`'s "Reading confidence" for the full `thin`/`noisy`/`mixed-pmu` reference and a
worked example of tracing a noisy bucket back to its cause).

**Automation opportunity**: `wspy-summary --strict` exits non-zero on any WARN — a natural gate for "this
benchmark's baseline isn't trustworthy enough yet, collect more repetitions before using it for
anything downstream" once you're driving this from a script rather than reading it by hand.

**When this is SPEC CPU2026**: this is where you decide, per benchmark, how many repetitions its own
variance actually needs — a workload with real run-to-run jitter (thermal throttling, background system
noise, a workload with inherently non-deterministic timing) may need 5+ reps to clear `noisy`, while a
tight, deterministic one clears it at 3.

## Step 7 — Bucket the suite

This is the "put them in different buckets" step. `wspy-archetype --kmeans <n>` partitions every run
matching your filters into `n` clusters over a coverage-aware, z-standardized distance across every
`run_features` column each run actually collected (not just the four topdown categories) — cache/TLB
rates, branch mispredict rate, IPC, contention, whatever's present:

```
./wspy-archetype --db toysuite.db --kmeans 3
```

Real output — with only 3 fundamentally different toy programs (3 reps each), k-means correctly
recovers exactly the 3 real clusters, near-zero distance within each:

```
cluster   size hostname:run_id                            distance  top_features (name:z-score)
     0      3 sacramento:...222217                        0.2695  speculate_pct:+1.33;branch_mispredict_pct:+1.33;frontend_pct:+1.33;...
     0      3 sacramento:...222223                        0.3152  speculate_pct:+1.33;branch_mispredict_pct:+1.33;frontend_pct:+1.33;...
     0      3 sacramento:...222220                        0.5282  speculate_pct:+1.33;branch_mispredict_pct:+1.33;frontend_pct:+1.33;...
     1      3 sacramento:...222208                        0.2872  fault_rate:+1.33;l2_miss_pct:-1.31;dcache_miss_pct:+1.31;...
     1      3 sacramento:...222214                        0.4228  fault_rate:+1.33;l2_miss_pct:-1.31;dcache_miss_pct:+1.31;...
     1      3 sacramento:...222211                        0.5125  fault_rate:+1.33;l2_miss_pct:-1.31;dcache_miss_pct:+1.31;...
     2      3 sacramento:...222201                        0.2567  ipc_mean:+1.14;retire_pct:+1.13;l2_miss_pct:+0.79;...
     2      3 sacramento:...222205                        0.3356  ipc_mean:+1.14;retire_pct:+1.13;l2_miss_pct:+0.79;...
     2      3 sacramento:...222197                        0.4262  ipc_mean:+1.14;retire_pct:+1.13;l2_miss_pct:+0.79;...
```

Cluster 0 (`branchy`) is characterized by elevated `speculate_pct`/`branch_mispredict_pct`/`frontend_pct`;
cluster 1 (`membound`) by elevated `dcache_miss_pct` and reduced `l2_miss_pct`/`retire_pct`; cluster 2
(`compute`) by elevated `ipc_mean`/`retire_pct`. Members are printed closest-to-centroid first, so the
first row of each cluster is the most "representative" member — the one to look at first if you only
have time to deep-dive one per bucket.

**What to look for**: `top_features` names *why* a cluster is distinct, in z-score units relative to the
whole population — a real signal (this cluster is +1.3 standard deviations above the suite's own mean
on this feature), not an arbitrary label. Picking `n`: for a suite the size of SPEC CPU, start around
4-8 (roughly the number of resource_dominance categories, maybe split a couple further) and look at
whether clusters merge/split sensibly as you adjust — there's no single "correct" `n` from the tool
itself.

**When this is SPEC CPU2026**: this is the actual deliverable this step exists for — a data-driven
grouping of the whole suite (rather than SPEC's own hand-assigned integer/fp/rate/speed categories,
which describe *what* the benchmark computes, not *how the hardware spends its time* running it) to
prioritize optimization/investigation effort: which benchmarks share a bottleneck shape, so a fix
investigated on one representative benchmark is likely to transfer to the rest of its cluster.

## Step 8 — Find close relatives

Given one new/interesting run, `--nearest` ranks every other run by similarity, over whichever
`run_features` both runs actually share — useful for "have I basically already characterized something
like this" or spotting near-duplicate benchmark coverage:

```
./wspy-archetype --db toysuite.db --nearest sacramento:<run_id> --k 3
```

Real output (target = one `compute` run):

```
hostname:run_id                            distance  compared_features
sacramento:...222201                        0.6184                 13
sacramento:...222205                        0.7229                 13
sacramento:...222220                         1.253                 13
```

The two closest are `compute`'s own other reps (as expected); the third-closest is a `branchy` run at
roughly double the distance — a real, checkable signal that `branchy` is the *next*-most-similar
workload shape in this tiny suite, not an arbitrary third pick.

**When this is SPEC CPU2026**: run this against a new benchmark once you've characterized it, filtered
against your *already-understood* benchmarks (`--command`/`--hostname` narrow the candidate pool) — a
short distance to something you've already deep-dived is a shortcut to "I probably already know what's
going on here," a long distance to everything is a flag that this one needs its own investigation.

## Step 9 — Per-bucket deep dives: a decision tree

Once you know which bucket a benchmark landed in, `memory_attribution` (part of the same
`wspy-archetype` scorecard) tells you whether to trust a `memory-bound` read and what to look at next.
Real examples from the toy suite, `--run` mode:

```
$ ./wspy-archetype --db toysuite.db --run sacramento:<membound-run>
resource_dominance=memory-bound
resource_dominance_pct=89.3
memory_attribution=corroborated
memory_attribution_reasons=dcache_miss_pct=16.27,dtlb_generic_miss_pct=10.63

$ ./wspy-archetype --db toysuite.db --run sacramento:<branchy-run>
resource_dominance=frontend-bound
memory_attribution=not-memory-bound
memory_attribution_reasons=
```

Use this as a routing table for where to spend more counter-collection effort:

- **`resource_dominance=memory-bound`, `memory_attribution=corroborated`** — trust it; the reasons
  string already tells you which signal fired. `dcache_miss_pct`/`l2_miss_pct` elevated → go deeper with
  `--cache3`/an L3-focused pass to see whether it's resolved at L3 or genuinely reaching DRAM.
  `itlb_generic_miss_pct`/`dtlb_generic_miss_pct` elevated → TLB pressure, worth checking huge-page
  usage. On AMD, an `--ibs-sample` pass (Step 12) gives a direct DRAM-vs-cache breakdown instead of
  inferring it from miss rates.
- **`memory-bound`, `uncorroborated`** — genuinely interesting: topdown says backend-bound but nothing
  else measured backs it up. Worth a `--tree --tree-io-wait --tree-schedstat` pass (see below) to rule
  out "not actually stalled at all," and/or an IBS sampling pass — this is exactly the disagreement a
  single-counter read can't surface on its own.
- **`memory-bound`, `blocked` or `oversubscribed`** — the CPU wasn't stalled at all; see below.
- **`resource_dominance=frontend-bound`** — `memory_attribution` doesn't apply (it's specifically about
  backend). Look at `icache_miss_pct`/`opcache` (AMD, `--topdown-optlb`) for code-footprint pressure, or
  `branch_mispredict_pct`/`control_flow_style` if frontend stalls trace back to redirects from
  mispredicts rather than pure fetch/decode bandwidth.
- **`resource_dominance=speculation-bound`** — `spec_branch_pct` vs `spec_pipeline_pct` (both in the
  base topdown L2 split, no extra flag needed) tells apart mispredicted-branch-driven waste from other
  pipeline-clear-driven waste.

**The blocked/oversubscribed check**, real side-by-side comparison — an uncontended single-process
capture vs. a genuinely oversubscribed one (32 busy processes on this same 32-core host, from earlier
real testing):

```
uncontended: blocking_wait_pct≈0.0%, sched_rundelay_pct≈0.05%   (nothing to see — trust the topdown read)
oversubscribed: blocking_wait_pct≈0.28%, sched_rundelay_pct≈32%  (the CPU was mostly waiting for a
                                                                    turn to run at all, not stalled)
```

Collecting these needs `--tree` with the relevant sub-flags:

```
./wspy --tree tree.txt --tree-io-wait --tree-schedstat \
  --manifest run.manifest.json --run-index toysuite-index.jsonl \
  -- /tmp/toybench/membound
```

Once ingested, `memory_attribution` checks `blocking_wait_pct`/`sched_rundelay_pct` *before*
cache/TLB/IBS corroboration — a `memory-bound` read on a `blocked`/`oversubscribed` run means "don't
even ask whether cache counters corroborate it," since the CPU wasn't attempting real work during that
time.

**When this is SPEC CPU2026**: SPEC's rate-mode runs (N copies of the same benchmark concurrently) are
exactly the shape that can trigger `oversubscribed` if N exceeds available cores/threads — genuinely
useful to know before concluding a rate-mode slowdown is a hardware bottleneck rather than a scheduling
one.

## Step 10 — Multi-core-aware diagnostics

For anything multi-threaded, or for a rate-mode SPEC run, `--per-core` plus `wspy-core-report` finds
imbalance a system-wide average hides:

```
./wspy --per-core --csv --counters=topdown -o percore.csv -- /tmp/toybench/membound
./wspy-core-report percore.csv --metric ipc --metric backend
```

Real output — a single-threaded program running on one of 32 cores, unweighted stats dragged toward
zero by 31 idle cores:

```
Cross-core stats:
  ipc                      n=32  mean=0.5069       stddev=0.3974     cv= 78.41%  hot=core9(1.8800)  cold=core13(0.1300)
  backend                  n=32  mean=16.8500      stddev=16.1562    cv= 95.88%  hot=core0(89.2000)  cold=core13(2.6000)
```

`--weight-by <metric>` gives the activity-weighted version — each core's contribution weighted by how
much real work it actually did, instead of counting an idle core the same as the busy one:

```
./wspy-core-report --weight-by ipc percore.csv --metric ipc --metric backend
```

```
Cross-core stats (mean/stddev/cv weighted by 'ipc'):
  ipc                      n=32  mean=0.8088       stddev=0.4978     cv= 61.55%  hot=core9(1.8800)  cold=core13(0.1300)
  backend                  n=32  mean=20.3161      stddev=14.8988    cv= 73.33%  hot=core0(89.2000)  cold=core13(2.6000)
```

`hot`/`cold` are unaffected by weighting (still the raw per-core extremes either way) — weighting only
changes how the per-core values combine into one headline number.

**On a hybrid host** (Intel P-core/E-core, AMD Zen5/Zen5c): `wspy --list-affinity` shows the core-type
groups, and `--affinity=coretype=<id>` pins a run to just one type for a class-isolated number.
`wspy-core-report` also auto-detects and breaks out per-core-class stats separately whenever a
`--per-core --csv` file spans more than one type on the *collecting* host — see its own `--help`. If
this host turns out to be Intel hybrid, also check that `--topdown` alone (no `--per-core`) warns about
P-core-only measurement — a real correctness gap fixed this cycle (see `INVESTIGATION.md`'s
"Core-class-aware topdown").

**When this is SPEC CPU2026**: rate-mode runs are the main reason to reach for this — is the aggregate
throughput number actually representative of every copy's behavior, or is one copy (thermal throttling,
NUMA placement, an unlucky core) dragging the whole rate score down while the others look fine?

## Step 11 — Phase-aware analysis for longer-running benchmarks

Most real SPEC benchmarks run for tens of seconds to minutes, long enough for `--interval` sampling and
`phase.c`'s warmup/steady/degraded classification to be meaningful. The toy suite's `membound` needs a
longer iteration count to get there — change its `for (j=0;j<6;j++)` to `for (j=0;j<20;j++)` (~10s
instead of ~3s), rebuild as a separate `membound_long` binary, then:

```
gcc -O2 -o /tmp/toybench/membound_long /tmp/toybench/membound.c   # after the j<20 edit above
./wspy --csv --interval 1 --counters=topdown -o interval.csv \
  --manifest interval.manifest.json --run-index toysuite-index.jsonl -- /tmp/toybench/membound_long
./wspy-store --db toysuite.db --run-index toysuite-index.jsonl
./wspy-summary --db toysuite.db --phase-topdown sacramento:<run_id>
```

Real output — a genuine warmup→steady transition, with the per-column drift it produced:

```
metric                   warmup           steady           degraded          drift_pct
retire                   6.57(n=3)        2.54(n=9)        -                      4.02
frontend                 5.03(n=3)        7.07(n=9)        -                      2.03
backend                  88.13(n=3)       89.90(n=9)       -                      1.77
wspy-summary: largest phase drift: retire (4.02 pts, between warmup and steady)
```

**What to look for**: this run never hit `degraded` (the `-` column) — a clean run. If a benchmark's
per-run average (what `wspy-store` collapses multi-tick data down to for `wspy-summary`) looks noisier
than its peers, `--phase-topdown` on one of its own runs is the first place to check whether a real
`degraded` episode, not measurement noise, explains it (see `doc/PROFILE_COOKBOOK.md`'s "Reading phase
output" for the full mechanism and thresholds).

**When this is SPEC CPU2026**: any benchmark whose steady-state topdown differs materially from its
whole-run average is worth a closer look — a long warmup (JIT-like startup, cold-cache ramp) can
otherwise get baked silently into a "characterization" that doesn't reflect steady-state behavior at
all.

## Step 12 — AMD IBS memory-path deep dive (memory-bound bucket, AMD only)

For a benchmark that landed `corroborated`/memory-bound and you want to know specifically *where* in the
memory hierarchy it's spending time (not just that cache/TLB counters agree something's elevated), IBS
sampling mode decodes each sample's actual data source:

```
./wspy --ibs-sample -- /tmp/toybench/membound
```

Real output (trimmed):

```
ibs_sample_dc_miss_rate         0.9%
ibs_sample_dram_rate           23.7%
ibs_sample_remote_node_rate     0.0%
ibs_sample_data_src_breakdown (scheme: zen4_ibs_extensions):
  Local L3 or other L1/L2 in CCX                  18.9%
  DRAM                                            23.7%
  MMIO/Config/PCI/APIC                             4.0%
```

Nearly a quarter of sampled loads genuinely reached DRAM — direct, per-sample confirmation of the
`corroborated` memory-bound read from Step 9, not an inference from a generic miss-rate counter.
`ibs_dc_miss_pct`/`ibs_dram_pct` are also promoted into `run_features`, so once ingested they show up in
`memory_attribution_reasons` automatically alongside the cache/TLB signals.

**When this is SPEC CPU2026**: reach for this on the handful of benchmarks that landed in your
"memory-bound, corroborated" cluster and matter most (highest weight in the overall metric, or the ones
you're actually trying to optimize) — it's a heavier, single-purpose pass (`wspy-run`'s `ibs-sample`
profile, or composed into `zen4plus-deep`), not something to run suite-wide by default.

## Step 13 — Track suite-wide coverage

As the suite grows (more benchmarks land, or you're re-running after a compiler/config change),
`wspy-ledger` answers "which of these have I actually characterized yet":

```
cat > workload-list.txt <<'EOF'
compute
membound
branchy
sorting
EOF
./wspy-ledger --run-index toysuite-index.jsonl --list workload-list.txt
```

Real output:

```
compute                                  done                 3/3 run(s) succeeded, most recent ...
membound                                 done                 3/3 run(s) succeeded, most recent ...
branchy                                  done                 3/3 run(s) succeeded, most recent ...
sorting                                  skipped              no matching run found in run index
4 workload(s): 3 done, 1 skipped, 0 unsupported, 0 needs-tool-support
```

`--strict` exits non-zero if anything's still `skipped`/`needs-tool-support` — a real CI gate for "has
every benchmark in the suite actually been run at least once."

**When this is SPEC CPU2026**: write `workload-list.txt` once as the full CPU2026 benchmark list (one
name per line — this is a plain text file, not Phoronix-specific despite `wspy-ledger`'s
Phoronix-flavored default path/flags), then re-run this after every batch to see what's left.

## Step 14 — Set up regression tracking going forward

Once a benchmark has baseline history in the store, `--check-regression` compares one new run against
every strictly-earlier run in the same bucket:

```
./wspy-summary --db toysuite.db --check-regression sacramento:<run_id> --metric retire --metric backend
```

Real output:

```
metric      target    n    base_mean     ci95_low    ci95_high verdict          status
backend       53.6    2         53.8      52.5294      55.0706 WARN:thin        within
retire         46.0    2        45.95      44.0441      47.8559 WARN:thin        within
```

`status` is direction-neutral (`within`/`above`/`below` the baseline's 95% CI) — this tool doesn't know
whether a given metric moving up or down is good or bad for a given workload, so it reports the
deviation and leaves the judgment call to you (or a future per-metric semantics table, if this codebase
ever builds one).

**Automation opportunity**: this is the piece that turns a one-time characterization pass into ongoing
tracking. Once you have real baseline history (a few weeks of runs, a few different compiler/config
combinations), wire a nightly/per-commit `wspy-run` + `wspy-store` + `wspy-summary --check-regression
--strict` sequence into `wspy-queue` (headless, cron-friendly, no web server required) so a regression
gets flagged automatically instead of only being noticed the next time someone happens to look.

## Step 15 — Visualize and publish

`wspy-plot` renders any `--interval` CSV against shared templates automatically:

```
./wspy-plot --rundir <run-directory>
```

Real output:

```
wspy-plot: interval.csv -> plots/interval.topdown.png (Topdown Breakdown)
wspy-plot: interval.csv -> plots/interval.topdown-detail.png (Topdown L1->L2 Detail)
wspy-plot: interval.csv -> plots/interval.ipc.png (Instructions per Cycle)
wspy-plot: interval.csv -> plots/interval.metrics.png (Other Metrics)
4 plot(s) generated from 1 CSV file(s)
```

For sharing one specific interesting run with someone else, `wspy-bundle` packages every artifact
(manifest, raw CSV, tree output if present) into one checksummed, portable tarball:

```
./wspy-bundle --rundir <run-directory> --out interesting-run.reproducibility.tar.gz
```

For a browsable view across everything (curate a report page, cross-run compare, tree viewer), start
the web UI — stdlib-only, no build step:

```
python3 web/server.py    # then open http://127.0.0.1:8765/
```

If you have a local Ollama instance running, `wspy-analyze` can generate a narrative summary of one run,
or (`--compare-rundir`) explain what changed between two — useful for a quick "what's the story here"
before writing your own analysis, not a replacement for reading the numbers yourself.

## Step 16 — Automate the whole pipeline

Once you've done the above by hand enough times to trust the sequence, `wspy-sweep` turns it into one
declarative invocation — runs a profile across every combination of a swept axis (today: `--affinity`
values), tags each cell for later `--group-by-option` analysis, and ingests into the store automatically
when it's done:

```
./wspy-sweep --profile deep-cpu --affinity all,nosmt --suite cpu2026 -o results --tag config=baseline
```

`--dry-run` first is worth it before committing to a real sweep — it prints every cell's command line
without running any of them.

For the batch-driver layer itself (looping over every benchmark, building/validating/running/plotting),
`workload/cpu2017/run_test.sh`/`run_all.sh` (already covered in Step 2) is the concrete pattern to fork
into `workload/cpu2026/` once real binaries exist — don't design a new driver shape from scratch.

## Quick command reference

| Step | Command | What it answers |
|---|---|---|
| 1 | `wspy --capabilities` / `--preflight` | can this host measure what I want, does it fit the PMU budget |
| 2 | `wspy-run --list` / `wspy-run <profile> --suite ... --benchmark ...` | consistent multi-pass capture per benchmark |
| 3 | `wspy-store --db <db> --run-index <file>` | get everything into the normalized store |
| 4 | `wspy-validate [--strict] <manifest...>` | is this run's data actually trustworthy |
| 5 | `wspy-archetype --db <db> --csv` / `--run <host:id>` | bulk / single-run classification |
| 6 | `wspy-summary --db <db> [--strict]` | per-benchmark repeatability baseline |
| 7 | `wspy-archetype --db <db> --kmeans <n>` | bucket the whole suite |
| 8 | `wspy-archetype --db <db> --nearest <host:id>` | find the closest already-known benchmark |
| 9 | (scorecard's `memory_attribution` field) | is a memory-bound read actually trustworthy, and why |
| 10 | `wspy --per-core` + `wspy-core-report [--weight-by <metric>]` | per-core imbalance, hybrid-core-aware |
| 11 | `wspy --interval` + `wspy-summary --phase-topdown` | warmup/steady/degraded-aware topdown |
| 12 | `wspy --ibs-sample` (AMD) | direct per-sample memory-hierarchy source breakdown |
| 13 | `wspy-ledger --run-index <file> --list <workloads>` | suite-wide "have I run everything" coverage |
| 14 | `wspy-summary --check-regression <host:id>` | did this run drift from its own baseline history |
| 15 | `wspy-plot` / `wspy-bundle` / `web/server.py` / `wspy-analyze` | visualize, share, browse, narrate |
| 16 | `wspy-sweep` / `workload/<suite>/run_*.sh` | automate the whole sequence above |

## A note on `doc/PROFILE_COOKBOOK.md`

That document is the deeper interpretation reference for `verdict`/`confidence`/`phase` output cited
throughout this walkthrough — read it alongside this one for the "why does this field say what it says"
detail this document deliberately keeps brief. Its own "Statistical clustering — not yet shipped"
section, however, **predates `--kmeans`/`--nearest` shipping** (this document's Step 7/8) and is now
stale — worth a refresh, not a contradiction to trust over what's demonstrated here.
