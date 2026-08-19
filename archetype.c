/*
 * archetype.c - wspy-archetype: classifies a wspy run along a small set of
 * workload "archetype" axes -- INVESTIGATION.md's 4.2 Tier 1 "Archetype
 * scorecard" item, the second (and last) of the characterization-track
 * prerequisites started by store.c's run_features ("Feature normalization
 * prerequisites", shipped in PR #121).
 *
 * Reads store.c's runs/run_features tables directly via SQL, read-only --
 * this tool never writes, same convention as summary.c/core_report.c. No
 * taxonomy/threshold/confidence-formula spec existed anywhere in this repo
 * before this file (confirmed by search) -- every classification rule
 * below is a from-scratch v1 design, deliberately simple and documented as
 * a starting point, the same style phase.c/preflight.c already use for
 * their own heuristics.
 *
 * Four axes, confirmed with the user as the chosen design (the backlog's
 * "confidence + top-2 alternatives" language doesn't by itself say whether
 * it means one composite label or per-axis classifications):
 *   - resource_dominance: the headline axis, a ranked 4-way score over
 *     topdown L1 (retire/frontend/backend/speculate) -- the one axis with
 *     a natural percentage to rank alternatives by, so "confidence" and
 *     "top-2 alternatives" are scoped to it specifically.
 *   - parallelism_shape, control_flow_style, runtime_stability: simpler
 *     threshold-based supporting tags (each independently "unknown" when
 *     their source run_features value wasn't collected), whose combined
 *     availability feeds overall confidence but which don't themselves
 *     have "alternatives" to rank.
 *
 * Real prior art grounded this design further: a 2024 clustering analysis
 * (github.com/mvermeulen, ~240 Phoronix tests + 23 SPEC CPU2017
 * benchmarks, k-means/Lloyd's algorithm into 30 clusters) used exactly
 * retire/frontend/backend/speculation as its core clustering metrics --
 * directly validating the resource_dominance approach (its own cluster #1,
 * "backend bound... running on all cores", is exactly the kind of label
 * this axis produces) -- and separately used "on_cpu" (cores actively
 * used) as a first-class clustering dimension distinct from load balance,
 * which parallelism_proxy (run_features) didn't capture; active_core_count
 * (run_features, added alongside this file) is this codebase's own proxy
 * for that idea, not a reproduction of that work's undisclosed exact
 * formula. That same post's own restraint -- explicit cluster boundaries
 * were never reduced to hard thresholds, only relative distances -- is
 * exactly the caution summary.c's own header comment states about not
 * inventing a differentiated-threshold table without real data to justify
 * it; the thresholds below are offered in that same spirit, a starting
 * point rather than a validated boundary.
 *
 * Extensibility (a new axis -- vectorization_density below is a real
 * example now): a new *simple* (single-value, threshold-based) axis is one
 * more SIMPLE_AXES entry plus a
 * classify_simple_axis() call site reading the new run_features value.
 * compute_overall_confidence() needs no changes -- it already loops over
 * however many simple axes exist -- but score_runs()'s CSV/human headers
 * and print_scorecard_row()/print_scorecard_fields() each enumerate every
 * axis by name (not a generic loop, so each new axis's column/field needs
 * adding by hand in all of them; confirmed the hard way adding both axes
 * above). A new *ranked* axis (like resource_dominance) needs its own small
 * candidate table/ranking function, following the same shape this file
 * already establishes.
 *
 * A fifth axis, memory_attribution, was added later (INVESTIGATION.md's 4.3
 * "Composite attribution" item, see classify_memory_attribution() below):
 * unlike the four original axes, it doesn't rank/threshold a single
 * run_features value, it cross-references topdown's own backend_pct
 * against independently-measured cache/TLB/IBS signals to say whether
 * a "memory-bound" verdict is corroborated by other counters or not --
 * genuinely new information a single-counter threshold can't produce,
 * not a replacement for resource_dominance's own topdown-only read.
 *
 * A sixth and seventh axis, allocation_pressure and vectorization_density,
 * were added still later (issues #231 and #227 respectively, landed as two
 * independent PRs off the same base) -- both plain SIMPLE_AXES entries (see
 * ALLOCATION_PRESSURE_RULES/VECTORIZATION_DENSITY_RULES below) over
 * already-promoted run_features values (fault_rate, float_pct), exactly the
 * one-entry extension shape this file's own "Extensibility" note above
 * describes, the first two times that note's promise was exercised for
 * real.
 *
 * An eighth axis, core_utilization, was added still later (issue #230's
 * first half; see CORE_UTILIZATION_RULES below) over on_cpu -- itself
 * reworked at the same time from an AVG()-of-a-raw-CSV-column feature into a
 * proper derived one (store.c), so this axis (and on_cpu itself) backfills
 * on every run ever collected via wspy-run just by re-running wspy-store
 * over already-existing artifacts, no re-collection needed. The issue's
 * other half -- a ctxswitch_rate-based "oversubscribed" label -- stays
 * deliberately unaddressed; see CORE_UTILIZATION_RULES's own comment for why
 * it's not simply the same recipe one more time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <getopt.h>
#include <sqlite3.h>
#include "json_reader.h"

/* run_features (store.c) didn't exist before store.c's schema version 4
 * (MIGRATION_V3_TO_V4) -- a database older than that has nothing for this
 * tool to score. */
#define ARCHETYPE_MIN_SCHEMA_VERSION 4

/* Resource-dominance confidence margin thresholds (percentage points
 * between the primary and runner-up topdown L1 category) and the
 * "known simple axes" floor each confidence level also requires --
 * deliberately simple v1 starting points, not derived from a formal study
 * (see file header). */
#define DOMINANCE_HIGH_MARGIN   20.0
#define DOMINANCE_MEDIUM_MARGIN 10.0

/* memory_attribution thresholds (classify_memory_attribution(), below) --
 * same "deliberately simple v1 starting point, not derived from a formal
 * study" caveat as DOMINANCE_*_MARGIN above. backend_pct itself must clear
 * MEMORY_ATTRIBUTION_FLOOR before an attribution is even attempted (below
 * that, memory isn't the dominant story for this run, nothing to
 * corroborate); the rest are "notably elevated" cutoffs for each
 * independently-measured corroborating signal. */
#define MEMORY_ATTRIBUTION_FLOOR_PCT 20.0
#define CACHE_MISS_ELEVATED_PCT       10.0  /* dcache_miss_pct/l2_miss_pct/l3_miss_pct */
#define TLB_MISS_ELEVATED_PER1K        5.0  /* itlb_miss_per1k/dtlb_miss_per1k */
#define SMT_CONTENTION_ELEVATED_PCT    5.0
#define IBS_DC_MISS_ELEVATED_PCT      10.0
#define IBS_DRAM_ELEVATED_PCT         10.0
/* memory_attribution_locus thresholds (classify_memory_locus(), below) --
 * same "deliberately simple v1 starting point" caveat. CACHE_MISS_ELEVATED_PCT
 * above doubles as this function's cache-counter-fallback-tier cutoff for
 * dcache_miss_pct/l2_miss_pct/l3_miss_pct/dtlb_generic_miss_pct (mem_signals[]'s
 * own threshold for those same features, not a new cutoff), and
 * IBS_DC_MISS_ELEVATED_PCT/IBS_DRAM_ELEVATED_PCT above double as its IBS-tier
 * dc_miss/dram cutoffs -- only the two signals with no existing threshold
 * anywhere else in this file get a new #define here. */
#define IBS_REMOTE_NODE_ELEVATED_PCT  10.0
#define IBS_TLB_MISS_ELEVATED_PCT     10.0
/* blocking-syscall-split modifier thresholds (fraction of run elapsed time,
 * store.c's blocking_wait_pct/sched_rundelay_pct) -- checked ahead of the
 * cache/TLB/IBS corroboration signals above: if the CPU wasn't actually
 * stalled (it was blocked in the kernel, or runnable but not scheduled),
 * asking whether cache/TLB/IBS "corroborate" a hardware stall is moot
 * regardless of what they show. */
#define BLOCKING_WAIT_ELEVATED_PCT    20.0
#define SCHED_RUNDELAY_ELEVATED_PCT   20.0

struct threshold_rule { double max_value; const char *label; }; /* ascending; last entry is the catch-all */

static const struct threshold_rule PARALLELISM_RULES[] = {
  { 0.15, "balanced-parallel" },
  { HUGE_VAL, "imbalanced" },
};
static const struct threshold_rule CONTROL_FLOW_RULES[] = {
  { 2.0, "straight-line" },
  { HUGE_VAL, "branch-heavy" },
};
static const struct threshold_rule STABILITY_RULES[] = {
  { 0.4, "erratic" },
  { 0.8, "phased" },
  { HUGE_VAL, "steady" },
};
/* allocation_pressure (issue #231) thresholds on fault_rate (run_features,
 * (minflt+majflt)/elapsed_seconds) -- deliberately simple v1 starting
 * points, same caveat as every other threshold table in this file, but no
 * longer speculative: tertile-informed cut points from the CPU2026
 * reference-matrix corpus (147 SPEC CPU2026 runs, 3 machines) once it had
 * enough spread to threshold against -- fault_rate ranged ~630-936,000/sec
 * across that corpus, roughly log-distributed (p33~14000, p80~86000). The
 * boundaries below round those to memorable log-scale numbers rather than
 * fitting them exactly, the same "starting point, not a validated
 * boundary" spirit DOMINANCE_*_MARGIN above already documents. */
static const struct threshold_rule ALLOCATION_PRESSURE_RULES[] = {
  { 15000.0,  "low" },
  { 100000.0, "moderate" },
  { HUGE_VAL, "high" },
};
/* vectorization_density (issue #227) thresholds on float_pct (run_features,
 * AMD-only FP-op density, topdown.c:print_float()'s "float" CSV column --
 * NOTE this is float_all/instructions*100, a PERCENTAGE, despite the human
 * output's separate "per 1000 inst" annotation using a 10x-larger scale;
 * don't confuse the two when reading reference-matrix data against these
 * cut points). Deliberately simple v1 starting points, same caveat as every
 * other threshold table in this file, but grounded in real cross-workload
 * data: the CPU2026 reference-matrix corpus (147 SPEC CPU2026 runs, 3
 * machines) shows intrate/fprate (PR #228's original comparison) cleanly
 * separated with zero overlap (int max 5.66%, fp min 14.52%) -- but adding
 * the intspeed/fpspeed variants shows real overlap at the boundary
 * (intspeed max 6.77% vs fpspeed min 3.58%, traced to one benchmark
 * (800-pot3d_s) whose vectorization differs by CPU generation -- Zen4 vs
 * Zen5 AVX-512 support -- not measurement noise). The 3-tier split below
 * absorbs that ambiguity into "moderate" rather than forcing a single
 * boundary through it: nothing int-flavored in the corpus ever reaches 10%
 * (max 6.77%), and nothing fp-flavored ever drops under 2% (observed min
 * 3.58%), so "low"/"high" stay unambiguous while "moderate" is the
 * deliberately-mixed middle. Still gcc-only/n=1-per-test/no-repeat-runs
 * data -- a starting point, not a validated boundary. */
static const struct threshold_rule VECTORIZATION_DENSITY_RULES[] = {
  { 2.0,      "low" },
  { 10.0,     "moderate" },
  { HUGE_VAL, "high" },
};
/* core_utilization (issue #230, first half) thresholds on on_cpu (run_features,
 * fraction of available cores kept busy end-to-end) -- deliberately simple v1
 * starting points, same caveat as every other threshold table in this file, but
 * grounded in real cross-workload data: the CPU2026 reference-matrix corpus
 * (147 SPEC CPU2026 runs, 3 machines) shows a clean, strong, entirely organic
 * split -- intrate/fprate (multi-copy parallel) sit at 0.80-0.99 on_cpu, but
 * intspeed (single multi-threaded EDA/simulation tools -- vpr/gem5/ns3/abc)
 * ranges 0.03-0.99 with many runs under 0.1, i.e. essentially single-threaded
 * on a 32-96 core machine. No deliberate thread-count-vs-core-count experiment
 * needed -- this fell straight out of already-collected data, unlike the
 * ctxswitch_rate-based "true oversubscription" half of this issue (see below).
 * Boundaries: nothing rate-flavored in the corpus drops under 0.8, and a solid
 * chunk of intspeed sits under 0.5, so "high" is unambiguous and "low" catches
 * the clearly-underutilized tail; "moderate" is the deliberately-uncertain
 * middle. Still single-compiler/n=1-per-test/no-repeat-runs data -- a starting
 * point, not a validated boundary.
 *
 * The issue's other half -- a ctxswitch_rate-based "oversubscribed" label --
 * is deliberately NOT attempted here even though ctxswitch_rate is already a
 * run_features value: unlike on_cpu, a high ctxswitch_rate/nivcsw doesn't have
 * one honest explanation. It's consistent with genuine oversubscription (more
 * runnable threads than cores), but equally consistent with heavy lock/futex
 * contention, timer-driven I/O, or external host noise -- CPU2026 (single
 * workload at a time, no deliberate thread-count variation) can't distinguish
 * these, and neither can a plain threshold on ctxswitch_rate alone. Revisit
 * once there's enough Phoronix corpus data to check whether high-ctxswitch_rate
 * runs cluster cleanly (real signal) the way float_pct/on_cpu did, or need a
 * corroborating signal (e.g. --tree-schedstat's run_delay_seconds, or the
 * workload's own known thread count) before it's safe to threshold. */
static const struct threshold_rule CORE_UTILIZATION_RULES[] = {
  { 0.5,      "low" },
  { 0.8,      "moderate" },
  { HUGE_VAL, "high" },
};

enum simple_axis_id {
  AXIS_PARALLELISM_SHAPE, AXIS_CONTROL_FLOW_STYLE, AXIS_RUNTIME_STABILITY,
  AXIS_ALLOCATION_PRESSURE, AXIS_VECTORIZATION_DENSITY, AXIS_CORE_UTILIZATION,
  NUM_SIMPLE_AXES
};
static const char *SIMPLE_AXIS_KEYS[NUM_SIMPLE_AXES] = {
  "parallelism_shape", "control_flow_style", "runtime_stability", "allocation_pressure",
  "vectorization_density", "core_utilization"
};

struct classified_axis { char label[24]; int available; };

/* Generic threshold classifier shared by every simple axis: walk `rules`
 * (ascending max_value, last entry is the unconditional catch-all) and
 * return the first one `value` doesn't exceed. `available` false (the
 * source run_features row was coverage='unavailable') always yields
 * "unknown" regardless of value. */
static void classify_simple_axis(double value,int available,
                                  const struct threshold_rule *rules,int nrules,
                                  struct classified_axis *out){
  int i;
  out->available = available;
  if (!available){ snprintf(out->label,sizeof(out->label),"unknown"); return; }
  for (i = 0; i < nrules; i++){
    if (value <= rules[i].max_value || i == nrules - 1){
      snprintf(out->label,sizeof(out->label),"%s",rules[i].label);
      return;
    }
  }
}

struct dominance_result {
  int available;
  char primary_label[24];
  double primary_pct;
  int has_alternative;
  char alternative_label[24];
  double alternative_pct;
};

/* The one ranked/multi-candidate axis: topdown L1's four mutually-
 * exclusive slot-destination percentages, renamed into plain-English
 * resource-dominance categories. Ranks only the candidates that were
 * actually measured (coverage='measured' in run_features) -- all four
 * normally co-occur (print_topdown() emits them together), so partial
 * availability is an edge case, not the common path. */
static void classify_resource_dominance(double retire_pct,int have_retire,
                                         double frontend_pct,int have_frontend,
                                         double backend_pct,int have_backend,
                                         double speculate_pct,int have_speculate,
                                         struct dominance_result *out){
  struct { const char *label; double value; int available; } cands[4] = {
    { "compute-bound",      retire_pct,    have_retire },
    { "frontend-bound",     frontend_pct,  have_frontend },
    { "memory-bound",       backend_pct,   have_backend },
    { "speculation-bound",  speculate_pct, have_speculate },
  };
  int i,best = -1,second = -1;

  memset(out,0,sizeof(*out));
  for (i = 0; i < 4; i++){
    if (!cands[i].available) continue;
    if (best == -1 || cands[i].value > cands[best].value){ second = best; best = i; }
    else if (second == -1 || cands[i].value > cands[second].value) second = i;
  }
  if (best == -1) return; /* no topdown L1 data at all this run */

  out->available = 1;
  snprintf(out->primary_label,sizeof(out->primary_label),"%s",cands[best].label);
  out->primary_pct = cands[best].value;
  if (second != -1){
    out->has_alternative = 1;
    snprintf(out->alternative_label,sizeof(out->alternative_label),"%s",cands[second].label);
    out->alternative_pct = cands[second].value;
  }
}

struct confidence_result {
  char level[24];        /* "high"/"medium"/"low"/"insufficient-data" (19 bytes incl. NUL) */
  /* comma-joined, fixed order, empty for "high" with nothing to flag. 256 (not the smaller size this
   * used to carry) since "narrow-margin" plus one "missing-<axis>-data" per unavailable simple axis
   * can legitimately need more room than it looks -- confirmed live: with 5 simple axes (as of
   * core_utilization, issue #230) the worst case is 174 bytes, already past a 160-byte buffer; adding
   * this file's own axis-count history (four -> five -> six -> seven -> eight, see file header)
   * shows this number only grows, never shrinks. Matches memory_attribution_result.reasons's own
   * 256-byte convention below rather than a tighter size tuned to today's axis count. */
  char reasons[256];
};

/* Mirrors summary.c's compute_verdict() shape: deterministic booleans, a
 * fixed-order reason list, appended only when they apply -- here a
 * confidence *level* rather than a pass/fail verdict, since "confidence"
 * isn't binary the way repeatability is. resource_dominance being
 * unavailable is its own terminal case (nothing to score at all); otherwise
 * confidence is driven by how decisive the dominance margin is and how
 * many of the supporting simple axes had data. */
static void compute_overall_confidence(const struct dominance_result *dom,
                                        const struct classified_axis *simple,int nsimple,
                                        struct confidence_result *out){
  double margin;
  int known = 0,i;
  size_t used;

  if (!dom->available){
    snprintf(out->level,sizeof(out->level),"insufficient-data");
    snprintf(out->reasons,sizeof(out->reasons),"no-topdown-data");
    return;
  }

  margin = dom->has_alternative ? (dom->primary_pct - dom->alternative_pct) : dom->primary_pct;
  for (i = 0; i < nsimple; i++) if (simple[i].available) known++;

  if (margin >= DOMINANCE_HIGH_MARGIN && known >= 2)
    snprintf(out->level,sizeof(out->level),"high");
  else if (margin >= DOMINANCE_MEDIUM_MARGIN && known >= 1)
    snprintf(out->level,sizeof(out->level),"medium");
  else
    snprintf(out->level,sizeof(out->level),"low");

  out->reasons[0] = '\0';
  used = 0;
  if (margin < DOMINANCE_HIGH_MARGIN)
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"narrow-margin");
  for (i = 0; i < nsimple; i++){
    if (simple[i].available) continue;
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"%smissing-%s-data",
                              used ? "," : "",SIMPLE_AXIS_KEYS[i]);
  }
}

struct memory_attribution_result {
  int available;     /* 0 only when backend_pct itself was never measured -- see file header */
  char label[24];     /* "unknown"/"not-memory-bound"/"corroborated"/"uncorroborated" */
  char reasons[256];  /* corroborated: which signal(s) fired, "name=value" comma-joined;
                        * uncorroborated: which signal(s) were checked and found unremarkable;
                        * empty for "unknown"/"not-memory-bound" */
  /* memory_attribution_locus (classify_memory_locus(), below) -- which hop
   * in the memory hierarchy a corroborated read concentrates in. Populated
   * only when label=="corroborated"; "unknown" otherwise, including when
   * corroboration came from a signal (e.g. smt_contention_pct) that carries
   * no hierarchy-position information at all. See INVESTIGATION.md's
   * "Shipped since 4.2" write-up ("IBS-derived memory-path bottleneck
   * decomposition") for the full design rationale. */
  char locus[24];         /* "l1"/"l2-l3"/"dram"/"remote-numa"/"unknown" --
                            * "tlb" is a co-firing tag in locus_reasons, not
                            * a distinct label, since address-translation
                            * misses are an independent hop from the data
                            * miss chain above. */
  char locus_reasons[160]; /* "tier=ibs-sample"/"tier=cache-counter" first,
                             * then which signal(s) drove the label. */
};

/* One corroborating raw signal this axis cross-references against
 * backend_pct -- a small table (mirrors PARALLELISM_RULES/etc.'s own
 * data-driven-loop style) rather than N copy-pasted if-blocks, so adding a
 * future corroborating signal (e.g. a new IBS field) is one more row here,
 * not a new code path. */
struct memory_signal { const char *name; double value; int available; double elevated_threshold; };

/* Cross-references topdown's own backend_pct (already used by
 * resource_dominance above) against every independently-measured
 * memory-hierarchy signal this run collected -- cache miss rates (generic
 * PERF_TYPE_HW_CACHE group, a completely different counter path than
 * topdown's own PMU events), TLB miss rates, SMT contention, and AMD IBS
 * sampling's per-sample-derived dc-miss/DRAM rates when available. A
 * "memory-bound" topdown read corroborated by at least one of these is much
 * higher-confidence than topdown alone; a "memory-bound" read with *zero*
 * corroboration from any signal that *was* actually collected is exactly
 * the kind of disagreement a single-counter heuristic can't surface --
 * flagged here as "uncorroborated" rather than silently trusted, since it
 * suggests the real bottleneck may be something not measured yet
 * (cross-NUMA latency, lock contention masquerading as a backend stall,
 * etc.) rather than a straightforward capacity/miss-rate problem.
 *
 * blocking_wait_pct/sched_rundelay_pct (store.c, scanned from --tree's raw
 * futex/io_wait/schedstat lines) are checked FIRST, ahead of every cache/
 * TLB/IBS signal above -- INVESTIGATION.md's "Composite attribution"
 * remaining-scope item: a "memory-bound" topdown read on a phase/run
 * dominated by kernel-blocking (futex/io-wait) or scheduler run-delay isn't
 * a genuine hardware stall at all, so asking whether cache/TLB/IBS
 * "corroborate" it is moot regardless of what they show -- checking
 * corroboration first would risk a technically-true but misleading
 * "corroborated" verdict on a run that was mostly blocked or
 * oversubscribed, not actually executing memory-bound code on a core.
 * "blocked" (heavy futex/io-wait) is checked ahead of "oversubscribed"
 * (heavy run_delay), matching the critical-path work's own three-way
 * priority ("heavy run_delay *with low futex/io-wait*") -- a process can't
 * be both meaningfully blocked-in-kernel and meaningfully not-yet-scheduled
 * for the same slice of time, so kernel-blocking (the more directly
 * explanatory signal) wins when both happen to be elevated.
 *
 * Deliberately does NOT itself rank *which* cache level (L1/L2/L3) the
 * stall concentrates in -- print_topdown_be()'s own dedicated --topdown-backend
 * group (l1_bound_slots_pct/etc., doc/METRICS.md) is a genuinely different
 * measurement (stall-cycle attribution) than a miss *rate* (request-outcome
 * attribution) can honestly claim to reproduce, and that group is Intel/
 * ARM-only besides (print_topdown_be() returns immediately on VENDOR_AMD).
 * classify_memory_locus(), below, is the request-outcome-based answer for
 * the AMD/IBS side of that same question -- a different *kind* of
 * decomposition than print_topdown_be()'s, not a duplicate of it. */
static void classify_memory_attribution(double backend_pct,int have_backend,
                                         double blocking_wait_pct,int have_blocking_wait,
                                         double sched_rundelay_pct,int have_sched_rundelay,
                                         const struct memory_signal *signals,int nsignals,
                                         struct memory_attribution_result *out){
  int i,any_measured = 0,any_elevated = 0;
  size_t used;

  memset(out,0,sizeof(*out));
  snprintf(out->label,sizeof(out->label),"unknown");
  if (!have_backend) return;

  out->available = 1;
  if (backend_pct < MEMORY_ATTRIBUTION_FLOOR_PCT){
    snprintf(out->label,sizeof(out->label),"not-memory-bound");
    return;
  }

  if (have_blocking_wait && blocking_wait_pct >= BLOCKING_WAIT_ELEVATED_PCT){
    snprintf(out->label,sizeof(out->label),"blocked");
    snprintf(out->reasons,sizeof(out->reasons),"blocking_wait_pct=%.4g",blocking_wait_pct);
    return;
  }
  if (have_sched_rundelay && sched_rundelay_pct >= SCHED_RUNDELAY_ELEVATED_PCT){
    snprintf(out->label,sizeof(out->label),"oversubscribed");
    snprintf(out->reasons,sizeof(out->reasons),"sched_rundelay_pct=%.4g",sched_rundelay_pct);
    return;
  }

  used = 0;
  for (i = 0; i < nsignals; i++){
    if (!signals[i].available) continue;
    any_measured = 1;
    if (signals[i].value < signals[i].elevated_threshold) continue;
    any_elevated = 1;
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"%s%s=%.4g",
                              used ? "," : "",signals[i].name,signals[i].value);
  }

  if (any_elevated){
    snprintf(out->label,sizeof(out->label),"corroborated");
    return;
  }

  if (!any_measured){
    snprintf(out->label,sizeof(out->label),"unknown");
    return;
  }

  snprintf(out->label,sizeof(out->label),"uncorroborated");
  used = 0;
  for (i = 0; i < nsignals; i++){
    if (!signals[i].available) continue;
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"%schecked:%s",
                              used ? "," : "",signals[i].name);
  }
}

/* One run's worth of run_features values, assembled by streaming
 * contiguous (run_id-ordered) rows out of the store -- see score_runs()/
 * trace_run_archetype(). Only the features this tool's axes actually need
 * are named fields; run_features carries several more (ipc_mean, cache/
 * TLB miss rates, ctxswitch_rate) not yet used by any axis. */
struct run_snapshot {
  sqlite3_int64 run_id;
  char hostname[256];
  char run_id_text[128];
  char command[512];
  double retire_pct,frontend_pct,backend_pct,speculate_pct;
  int have_retire,have_frontend,have_backend,have_speculate;
  double parallelism_proxy;      int have_parallelism;
  double active_core_count;      int have_active_core_count;
  double branch_mispredict_pct;  int have_branch;
  double phase_stability;        int have_phase;
  double smt_contention_pct;     int have_smt_contention;
  double fault_rate;             int have_fault_rate;
  double float_pct;              int have_float;
  double on_cpu;                 int have_on_cpu;
  /* memory_attribution's corroborating signals (classify_memory_attribution()) --
   * independently-measured cache/TLB/IBS raw rates, cross-referenced against
   * backend_pct above rather than folded into any existing axis. */
  double dcache_miss_pct;        int have_dcache_miss;
  double l2_miss_pct;            int have_l2_miss;
  double l3_miss_pct;            int have_l3_miss;
  double itlb_miss_per1k;        int have_itlb_miss;
  double dtlb_miss_per1k;        int have_dtlb_miss;
  double ibs_dc_miss_pct;        int have_ibs_dc_miss;
  double ibs_dram_pct;           int have_ibs_dram;
  double itlb_generic_miss_pct;  int have_itlb_generic_miss;
  double dtlb_generic_miss_pct;  int have_dtlb_generic_miss;
  /* memory_attribution_locus's own decomposition inputs, on top of the
   * corroborating signals above -- see classify_memory_locus(). */
  double ibs_dc_l1tlb_miss_pct;  int have_ibs_dc_l1tlb_miss;
  double ibs_dc_l2tlb_miss_pct;  int have_ibs_dc_l2tlb_miss;
  double ibs_remote_node_pct;    int have_ibs_remote_node;
  double backend_memory_pct;     int have_backend_memory;
  /* blocking-syscall-split modifier inputs (store.c, from --tree) --
   * checked ahead of the corroborating signals above, see
   * classify_memory_attribution()'s own comment. */
  double blocking_wait_pct;      int have_blocking_wait;
  double sched_rundelay_pct;     int have_sched_rundelay;
};

static void run_snapshot_reset(struct run_snapshot *snap,sqlite3_int64 run_id,
                                const char *hostname,const char *run_id_text,const char *command){
  memset(snap,0,sizeof(*snap));
  snap->run_id = run_id;
  snprintf(snap->hostname,sizeof(snap->hostname),"%s",hostname ? hostname : "?");
  snprintf(snap->run_id_text,sizeof(snap->run_id_text),"%s",run_id_text ? run_id_text : "?");
  snprintf(snap->command,sizeof(snap->command),"%s",command ? command : "");
}

/* Applies one run_features (feature_name,value,coverage) row to the
 * snapshot being assembled. Feature names not recognized here (every other
 * run_features entry -- ipc_mean, cache/TLB rates, fault_rate,
 * ctxswitch_rate, and any future addition -- see file header's
 * "Extensibility" note) are silently ignored, not an error: this tool only
 * scores what its axes currently use. */
static void run_snapshot_apply_feature(struct run_snapshot *snap,const char *feature_name,
                                        double value,int measured){
  if (!strcmp(feature_name,"retire_pct")){ snap->retire_pct = value; snap->have_retire = measured; }
  else if (!strcmp(feature_name,"frontend_pct")){ snap->frontend_pct = value; snap->have_frontend = measured; }
  else if (!strcmp(feature_name,"backend_pct")){ snap->backend_pct = value; snap->have_backend = measured; }
  else if (!strcmp(feature_name,"speculate_pct")){ snap->speculate_pct = value; snap->have_speculate = measured; }
  else if (!strcmp(feature_name,"parallelism_proxy")){ snap->parallelism_proxy = value; snap->have_parallelism = measured; }
  else if (!strcmp(feature_name,"active_core_count")){ snap->active_core_count = value; snap->have_active_core_count = measured; }
  else if (!strcmp(feature_name,"branch_mispredict_pct")){ snap->branch_mispredict_pct = value; snap->have_branch = measured; }
  else if (!strcmp(feature_name,"phase_stability")){ snap->phase_stability = value; snap->have_phase = measured; }
  else if (!strcmp(feature_name,"smt_contention_pct")){ snap->smt_contention_pct = value; snap->have_smt_contention = measured; }
  else if (!strcmp(feature_name,"fault_rate")){ snap->fault_rate = value; snap->have_fault_rate = measured; }
  else if (!strcmp(feature_name,"float_pct")){ snap->float_pct = value; snap->have_float = measured; }
  else if (!strcmp(feature_name,"on_cpu")){ snap->on_cpu = value; snap->have_on_cpu = measured; }
  else if (!strcmp(feature_name,"dcache_miss_pct")){ snap->dcache_miss_pct = value; snap->have_dcache_miss = measured; }
  else if (!strcmp(feature_name,"l2_miss_pct")){ snap->l2_miss_pct = value; snap->have_l2_miss = measured; }
  else if (!strcmp(feature_name,"l3_miss_pct")){ snap->l3_miss_pct = value; snap->have_l3_miss = measured; }
  else if (!strcmp(feature_name,"itlb_miss_per1k")){ snap->itlb_miss_per1k = value; snap->have_itlb_miss = measured; }
  else if (!strcmp(feature_name,"dtlb_miss_per1k")){ snap->dtlb_miss_per1k = value; snap->have_dtlb_miss = measured; }
  else if (!strcmp(feature_name,"ibs_dc_miss_pct")){ snap->ibs_dc_miss_pct = value; snap->have_ibs_dc_miss = measured; }
  else if (!strcmp(feature_name,"ibs_dram_pct")){ snap->ibs_dram_pct = value; snap->have_ibs_dram = measured; }
  else if (!strcmp(feature_name,"itlb_generic_miss_pct")){ snap->itlb_generic_miss_pct = value; snap->have_itlb_generic_miss = measured; }
  else if (!strcmp(feature_name,"dtlb_generic_miss_pct")){ snap->dtlb_generic_miss_pct = value; snap->have_dtlb_generic_miss = measured; }
  else if (!strcmp(feature_name,"ibs_dc_l1tlb_miss_pct")){ snap->ibs_dc_l1tlb_miss_pct = value; snap->have_ibs_dc_l1tlb_miss = measured; }
  else if (!strcmp(feature_name,"ibs_dc_l2tlb_miss_pct")){ snap->ibs_dc_l2tlb_miss_pct = value; snap->have_ibs_dc_l2tlb_miss = measured; }
  else if (!strcmp(feature_name,"ibs_remote_node_pct")){ snap->ibs_remote_node_pct = value; snap->have_ibs_remote_node = measured; }
  else if (!strcmp(feature_name,"backend_memory_pct")){ snap->backend_memory_pct = value; snap->have_backend_memory = measured; }
  else if (!strcmp(feature_name,"blocking_wait_pct")){ snap->blocking_wait_pct = value; snap->have_blocking_wait = measured; }
  else if (!strcmp(feature_name,"sched_rundelay_pct")){ snap->sched_rundelay_pct = value; snap->have_sched_rundelay = measured; }
}

/* Ranks *where* in the memory hierarchy a corroborated memory-bound read
 * concentrates -- the scope classify_memory_attribution()'s own header
 * comment explicitly deferred (see "Deliberately does NOT itself rank...",
 * above). Only called when attr->label is already "corroborated": ranking
 * hops on an uncorroborated/blocked/oversubscribed/not-memory-bound read
 * would claim precision this signal set doesn't have. Gated on
 * backend_memory_pct rather than backend_pct (the coarser L1 category
 * classify_memory_attribution() itself gates on) -- the L2 sub-split
 * specifically attributing memory *within* backend is the more precise
 * anchor for "is this run's memory-bound-ness actually concentrated in the
 * data path," as opposed to a backend stall that's more core/execution-
 * bound than memory-specific.
 *
 * Two precision tiers, tried in order (takes the whole snapshot rather than
 * classify_memory_attribution()'s explicit-parameter style, since this
 * function's decision tree genuinely needs this many run_snapshot fields at
 * once):
 *
 * 1. IBS tier (AMD --ibs-sample): the only source with real per-sample
 *    hierarchy-position data (see ibs_sample.h) -- ibs_remote_node_pct
 *    checked first (a cross-socket hop dominates regardless of where else
 *    the access resolved), then ibs_dram_pct, then the L1-miss residual
 *    that didn't reach DRAM ("l2-l3"), then a plain dc_miss with nothing
 *    else elevated ("l1"). ibs_dc_l1tlb_miss_pct/ibs_dc_l2tlb_miss_pct are
 *    an independent hop (address translation, not data) so they're a
 *    co-firing tag in locus_reasons rather than competing for the label.
 *    Falls through to tier 2 if IBS data exists but nothing in it is
 *    elevated -- classify_memory_attribution()'s own corroboration may have
 *    come from a signal (e.g. smt_contention_pct) with no hierarchy-
 *    position information at all, so an unelevated IBS tier isn't itself
 *    "l1".
 * 2. Cache-counter tier (generic PERF_TYPE_HW_CACHE, any vendor): coarser
 *    same-shape fallback over dcache_miss_pct/l2_miss_pct/l3_miss_pct,
 *    reusing CACHE_MISS_ELEVATED_PCT (mem_signals[]'s own threshold for
 *    these same features, not a new cutoff). dtlb_generic_miss_pct is this
 *    tier's TLB co-firing tag.
 *
 * locus_reasons always starts with "tier=ibs-sample" or "tier=cache-counter"
 * so a reader never mistakes cache-counter-derived precision for IBS-grade. */
static void classify_memory_locus(const struct run_snapshot *snap,
                                   struct memory_attribution_result *attr){
  size_t used;

  snprintf(attr->locus,sizeof(attr->locus),"unknown");
  attr->locus_reasons[0] = '\0';
  if (strcmp(attr->label,"corroborated") != 0) return;

  if (!snap->have_backend_memory){
    snprintf(attr->locus_reasons,sizeof(attr->locus_reasons),"missing-backend_memory_pct");
    return;
  }
  if (snap->backend_memory_pct < MEMORY_ATTRIBUTION_FLOOR_PCT) return;

  if (snap->have_ibs_dc_miss){
    const char *label = NULL;

    used = (size_t)snprintf(attr->locus_reasons,sizeof(attr->locus_reasons),"tier=ibs-sample");
    if (snap->have_ibs_remote_node && snap->ibs_remote_node_pct >= IBS_REMOTE_NODE_ELEVATED_PCT){
      label = "remote-numa";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",ibs_remote_node_pct=%.4g",snap->ibs_remote_node_pct);
    } else if (snap->have_ibs_dram && snap->ibs_dram_pct >= IBS_DRAM_ELEVATED_PCT){
      label = "dram";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",ibs_dram_pct=%.4g",snap->ibs_dram_pct);
    } else if (snap->have_ibs_dram){
      /* dram_pct is a real measured 0-ish value here (not just missing) --
       * safe to treat the gap between it and dc_miss_pct as "resolved
       * before reaching DRAM". */
      double resolved_before_dram = snap->ibs_dc_miss_pct - snap->ibs_dram_pct;
      if (resolved_before_dram >= IBS_DC_MISS_ELEVATED_PCT){
        label = "l2-l3";
        used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                  ",ibs_dc_miss_pct=%.4g,ibs_dram_pct=%.4g",
                                  snap->ibs_dc_miss_pct,snap->ibs_dram_pct);
      } else if (snap->ibs_dc_miss_pct >= IBS_DC_MISS_ELEVATED_PCT){
        label = "l1";
        used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                  ",ibs_dc_miss_pct=%.4g",snap->ibs_dc_miss_pct);
      }
    } else if (snap->ibs_dc_miss_pct >= IBS_DC_MISS_ELEVATED_PCT){
      /* dram_pct itself unmeasured -- no basis to localize past "missed
       * L1D, unknown how far it traveled", so this is honestly "l1", not a
       * fabricated l2-l3 guess from treating missing data as zero. */
      label = "l1";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",ibs_dc_miss_pct=%.4g",snap->ibs_dc_miss_pct);
    }

    if (label){
      snprintf(attr->locus,sizeof(attr->locus),"%s",label);
      if ((snap->have_ibs_dc_l1tlb_miss && snap->ibs_dc_l1tlb_miss_pct >= IBS_TLB_MISS_ELEVATED_PCT) ||
          (snap->have_ibs_dc_l2tlb_miss && snap->ibs_dc_l2tlb_miss_pct >= IBS_TLB_MISS_ELEVATED_PCT))
        snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,",tlb-cofire");
      return;
    }
  }

  {
    const char *label = NULL;

    used = (size_t)snprintf(attr->locus_reasons,sizeof(attr->locus_reasons),"tier=cache-counter");
    if (snap->have_l3_miss && snap->l3_miss_pct >= CACHE_MISS_ELEVATED_PCT){
      label = "dram";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",l3_miss_pct=%.4g",snap->l3_miss_pct);
    } else if (snap->have_l2_miss && snap->l2_miss_pct >= CACHE_MISS_ELEVATED_PCT){
      label = "l2-l3";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",l2_miss_pct=%.4g",snap->l2_miss_pct);
    } else if (snap->have_dcache_miss && snap->dcache_miss_pct >= CACHE_MISS_ELEVATED_PCT){
      label = "l1";
      used += (size_t)snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,
                                ",dcache_miss_pct=%.4g",snap->dcache_miss_pct);
    }

    if (!label){
      attr->locus_reasons[0] = '\0';
      return; /* stays "unknown" -- corroboration came from a signal with no hierarchy-position information */
    }
    snprintf(attr->locus,sizeof(attr->locus),"%s",label);
    if (snap->have_dtlb_generic_miss && snap->dtlb_generic_miss_pct >= CACHE_MISS_ELEVATED_PCT)
      snprintf(attr->locus_reasons+used,sizeof(attr->locus_reasons)-used,",tlb-cofire");
  }
}

/* The full scorecard for one already-assembled snapshot -- shared by both
 * output modes so bulk and --run can never compute different answers for
 * the same run. */
struct scorecard {
  struct dominance_result dominance;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result confidence;
  struct memory_attribution_result memory_attribution;
};

static void score_snapshot(const struct run_snapshot *snap,struct scorecard *out){
  struct memory_signal mem_signals[10] = {
    { "dcache_miss_pct",       snap->dcache_miss_pct,       snap->have_dcache_miss,       CACHE_MISS_ELEVATED_PCT },
    { "l2_miss_pct",           snap->l2_miss_pct,           snap->have_l2_miss,           CACHE_MISS_ELEVATED_PCT },
    { "l3_miss_pct",           snap->l3_miss_pct,           snap->have_l3_miss,           CACHE_MISS_ELEVATED_PCT },
    { "itlb_miss_per1k",       snap->itlb_miss_per1k,       snap->have_itlb_miss,         TLB_MISS_ELEVATED_PER1K },
    { "dtlb_miss_per1k",       snap->dtlb_miss_per1k,       snap->have_dtlb_miss,         TLB_MISS_ELEVATED_PER1K },
    { "itlb_generic_miss_pct", snap->itlb_generic_miss_pct, snap->have_itlb_generic_miss, CACHE_MISS_ELEVATED_PCT },
    { "dtlb_generic_miss_pct", snap->dtlb_generic_miss_pct, snap->have_dtlb_generic_miss, CACHE_MISS_ELEVATED_PCT },
    { "ibs_dc_miss_pct",       snap->ibs_dc_miss_pct,       snap->have_ibs_dc_miss,       IBS_DC_MISS_ELEVATED_PCT },
    { "ibs_dram_pct",          snap->ibs_dram_pct,          snap->have_ibs_dram,          IBS_DRAM_ELEVATED_PCT },
    { "smt_contention_pct",    snap->smt_contention_pct,    snap->have_smt_contention,    SMT_CONTENTION_ELEVATED_PCT },
  };

  classify_resource_dominance(snap->retire_pct,snap->have_retire,
                               snap->frontend_pct,snap->have_frontend,
                               snap->backend_pct,snap->have_backend,
                               snap->speculate_pct,snap->have_speculate,
                               &out->dominance);
  classify_simple_axis(snap->parallelism_proxy,snap->have_parallelism,
                        PARALLELISM_RULES,2,&out->simple[AXIS_PARALLELISM_SHAPE]);
  classify_simple_axis(snap->branch_mispredict_pct,snap->have_branch,
                        CONTROL_FLOW_RULES,2,&out->simple[AXIS_CONTROL_FLOW_STYLE]);
  classify_simple_axis(snap->phase_stability,snap->have_phase,
                        STABILITY_RULES,3,&out->simple[AXIS_RUNTIME_STABILITY]);
  classify_simple_axis(snap->fault_rate,snap->have_fault_rate,
                        ALLOCATION_PRESSURE_RULES,3,&out->simple[AXIS_ALLOCATION_PRESSURE]);
  classify_simple_axis(snap->float_pct,snap->have_float,
                        VECTORIZATION_DENSITY_RULES,3,&out->simple[AXIS_VECTORIZATION_DENSITY]);
  classify_simple_axis(snap->on_cpu,snap->have_on_cpu,
                        CORE_UTILIZATION_RULES,3,&out->simple[AXIS_CORE_UTILIZATION]);
  classify_memory_attribution(snap->backend_pct,snap->have_backend,
                               snap->blocking_wait_pct,snap->have_blocking_wait,
                               snap->sched_rundelay_pct,snap->have_sched_rundelay,
                               mem_signals,10,&out->memory_attribution);
  classify_memory_locus(snap,&out->memory_attribution);
  compute_overall_confidence(&out->dominance,out->simple,NUM_SIMPLE_AXES,&out->confidence);
}

static sqlite3 *open_archetype_db(const char *path){
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt;
  int user_version = 0;

  if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: unable to open database %s: %s\n",path,sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db,30000);

  if (sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(stmt) == SQLITE_ROW) user_version = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }
  if (user_version < ARCHETYPE_MIN_SCHEMA_VERSION){
    fprintf(stderr,"wspy-archetype: %s: schema version %d predates run_features (needs >= %d) "
                    "-- re-ingest with a current wspy-store build\n",
            path,user_version,ARCHETYPE_MIN_SCHEMA_VERSION);
    sqlite3_close(db);
    return NULL;
  }
  return db;
}

static void print_csv_field(FILE *out,const char *s){
  int needs_quote = (strchr(s,',') != NULL) || (strchr(s,'"') != NULL);
  const char *p;

  if (!needs_quote){ fputs(s,out); return; }
  fputc('"',out);
  for (p = s; *p; p++){
    if (*p == '"') fputc('"',out);
    fputc(*p,out);
  }
  fputc('"',out);
}

static void print_scorecard_row(FILE *out,const struct run_snapshot *snap,
                                 const struct scorecard *sc,int csvflag){
  const char *dom_label = sc->dominance.available ? sc->dominance.primary_label : "unknown";
  const char *alt_label = sc->dominance.has_alternative ? sc->dominance.alternative_label : "";

  if (csvflag){
    print_csv_field(out,snap->hostname); fputc(',',out);
    print_csv_field(out,snap->run_id_text); fputc(',',out);
    print_csv_field(out,snap->command); fputc(',',out);
    print_csv_field(out,dom_label); fputc(',',out);
    if (sc->dominance.available) fprintf(out,"%.6g,",sc->dominance.primary_pct); else fputc(',',out);
    print_csv_field(out,alt_label); fputc(',',out);
    if (sc->dominance.has_alternative) fprintf(out,"%.6g,",sc->dominance.alternative_pct); else fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_PARALLELISM_SHAPE].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_CONTROL_FLOW_STYLE].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_RUNTIME_STABILITY].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_ALLOCATION_PRESSURE].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_VECTORIZATION_DENSITY].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_CORE_UTILIZATION].label); fputc(',',out);
    print_csv_field(out,sc->confidence.level); fputc(',',out);
    print_csv_field(out,sc->confidence.reasons); fputc(',',out);
    print_csv_field(out,sc->memory_attribution.label); fputc(',',out);
    print_csv_field(out,sc->memory_attribution.reasons); fputc(',',out);
    print_csv_field(out,sc->memory_attribution.locus); fputc(',',out);
    print_csv_field(out,sc->memory_attribution.locus_reasons);
    fputc('\n',out);
  } else {
    fprintf(out,"%-24.24s %-20.20s %-28.28s %-18s %-18s %-16s %-14s %-9s %-10s %-12s %-11s %-8s %-30.30s  %-16s  %-16s  %s",
            snap->hostname,snap->run_id_text,snap->command,
            dom_label,alt_label,
            sc->simple[AXIS_PARALLELISM_SHAPE].label,
            sc->simple[AXIS_CONTROL_FLOW_STYLE].label,
            sc->simple[AXIS_RUNTIME_STABILITY].label,
            sc->simple[AXIS_ALLOCATION_PRESSURE].label,
            sc->simple[AXIS_VECTORIZATION_DENSITY].label,
            sc->simple[AXIS_CORE_UTILIZATION].label,
            sc->confidence.level,sc->confidence.reasons,
            sc->memory_attribution.label,sc->memory_attribution.reasons,
            sc->memory_attribution.locus);
    if (sc->memory_attribution.locus_reasons[0]) fprintf(out,"  %s",sc->memory_attribution.locus_reasons);
    fputc('\n',out);
  }
}

/* Streams (run,feature_name,value,coverage) rows ordered by run id, so one
 * run's ~18 run_features rows are always contiguous (same convention
 * summary.c's summarize() uses for its own (group,metric) buckets) --
 * assemble a snapshot per run id and score it once the id changes. INNER
 * JOIN deliberately, not LEFT: a run with zero run_features rows (e.g.
 * --no-feature-extract was used and it's never been re-ingested since)
 * genuinely has nothing to score, which is different information than "all
 * axes came back unknown" (features computed, all unavailable) -- showing
 * it here would conflate the two. */
static int score_runs(sqlite3 *db,const char *command_filter,const char *hostname_filter,
                       int csvflag,FILE *out){
  const char *sql =
    "SELECT r.id, r.hostname, r.run_id, r.command, rf.feature_name, rf.value, rf.coverage "
    "FROM runs r JOIN run_features rf ON rf.run_id = r.id "
    "WHERE (?1 = '' OR r.command LIKE '%' || ?1 || '%') "
    "AND (?2 = '' OR r.hostname = ?2) "
    "ORDER BY r.id, rf.feature_name;";
  sqlite3_stmt *stmt;
  struct run_snapshot snap;
  int have_snap = 0,rows = 0;

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,command_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,hostname_filter,-1,SQLITE_TRANSIENT);

  if (csvflag)
    fprintf(out,"hostname,run_id,command,resource_dominance,resource_dominance_pct,"
                "alternative,alternative_pct,parallelism_shape,control_flow_style,"
                "runtime_stability,allocation_pressure,vectorization_density,core_utilization,"
                "confidence,confidence_reasons,memory_attribution,memory_attribution_reasons,"
                "memory_attribution_locus,memory_attribution_locus_reasons\n");
  else
    fprintf(out,"%-24.24s %-20.20s %-28.28s %-18s %-18s %-16s %-14s %-9s %-10s %-12s %-11s %-8s %-30s  %-16s  %-16s  %s  mem_locus_reasons\n",
            "hostname","run_id","command","resource_dominance","alternative",
            "parallelism","control_flow","stability","alloc_pressure","vec_density","core_util",
            "conf.","reasons","mem_attribution","mem_attr_reasons","mem_locus");

  while (sqlite3_step(stmt) == SQLITE_ROW){
    sqlite3_int64 run_id = sqlite3_column_int64(stmt,0);
    const unsigned char *hostname = sqlite3_column_text(stmt,1);
    const unsigned char *run_id_text = sqlite3_column_text(stmt,2);
    const unsigned char *command = sqlite3_column_text(stmt,3);
    const unsigned char *feature_name = sqlite3_column_text(stmt,4);
    double value = sqlite3_column_double(stmt,5);
    const unsigned char *coverage = sqlite3_column_text(stmt,6);
    int measured = coverage && !strcmp((const char *)coverage,"measured");

    if (!have_snap || snap.run_id != run_id){
      if (have_snap){ struct scorecard sc; score_snapshot(&snap,&sc); print_scorecard_row(out,&snap,&sc,csvflag); rows++; }
      run_snapshot_reset(&snap,run_id,(const char *)hostname,(const char *)run_id_text,(const char *)command);
      have_snap = 1;
    }
    if (feature_name) run_snapshot_apply_feature(&snap,(const char *)feature_name,value,measured);
  }
  if (have_snap){ struct scorecard sc; score_snapshot(&snap,&sc); print_scorecard_row(out,&snap,&sc,csvflag); rows++; }

  sqlite3_finalize(stmt);
  return rows;
}

static void print_trace_field(FILE *out,const char *key,const char *value){
  const char *p;
  fprintf(out,"%s=",key);
  for (p = value; *p; p++) fputc((*p == '\n' || *p == '\r') ? ' ' : *p,out);
  fputc('\n',out);
}

/* Shared key=value rendering for a computed scorecard -- factored out of trace_run_archetype()
 * (INVESTIGATION.md 4.3 item 23) so run_guest_archetype() below can emit the exact same field set
 * and ordering for an externally-supplied (not-in-the-store) feature vector, letting
 * wspy-testpoint's existing key=value parser (collect_archetype_scorecards()) work unmodified for
 * either source. */
static void print_scorecard_fields(FILE *out,const char *hostname,const char *run_id_text,
                                    const char *command,const struct scorecard *sc){
  char pct_buf[32];
  print_trace_field(out,"hostname",hostname);
  print_trace_field(out,"run_id",run_id_text);
  print_trace_field(out,"command",command);
  print_trace_field(out,"resource_dominance",sc->dominance.available ? sc->dominance.primary_label : "unknown");
  if (sc->dominance.available){
    snprintf(pct_buf,sizeof(pct_buf),"%.2f",sc->dominance.primary_pct);
    print_trace_field(out,"resource_dominance_pct",pct_buf);
  }
  if (sc->dominance.has_alternative){
    print_trace_field(out,"alternative",sc->dominance.alternative_label);
    snprintf(pct_buf,sizeof(pct_buf),"%.2f",sc->dominance.alternative_pct);
    print_trace_field(out,"alternative_pct",pct_buf);
  }
  print_trace_field(out,"parallelism_shape",sc->simple[AXIS_PARALLELISM_SHAPE].label);
  print_trace_field(out,"control_flow_style",sc->simple[AXIS_CONTROL_FLOW_STYLE].label);
  print_trace_field(out,"runtime_stability",sc->simple[AXIS_RUNTIME_STABILITY].label);
  print_trace_field(out,"allocation_pressure",sc->simple[AXIS_ALLOCATION_PRESSURE].label);
  print_trace_field(out,"vectorization_density",sc->simple[AXIS_VECTORIZATION_DENSITY].label);
  print_trace_field(out,"core_utilization",sc->simple[AXIS_CORE_UTILIZATION].label);
  print_trace_field(out,"confidence",sc->confidence.level);
  print_trace_field(out,"confidence_reasons",sc->confidence.reasons);
  print_trace_field(out,"memory_attribution",sc->memory_attribution.label);
  print_trace_field(out,"memory_attribution_reasons",sc->memory_attribution.reasons);
  print_trace_field(out,"memory_attribution_locus",sc->memory_attribution.locus);
  print_trace_field(out,"memory_attribution_locus_reasons",sc->memory_attribution.locus_reasons);
}

/* --run <hostname>:<run_id>: detailed single-run scorecard, key=value lines
 * like summary.c's --trace. Existence is checked against `runs` directly
 * first (not inferred from the JOIN below coming back empty), so a run
 * that exists but has zero run_features rows is correctly reported as
 * "found, nothing to score" rather than "not found" -- see score_runs()'s
 * own comment on the same distinction. Returns 0 if found (regardless of
 * how much could be scored), 1 if the (hostname,run_id) isn't in the store
 * at all, matching summary.c's --trace exit-code convention (data-missing,
 * not a usage error). */
static int trace_run_archetype(sqlite3 *db,const char *hostname,const char *run_id_text,FILE *out){
  sqlite3_stmt *stmt;
  sqlite3_int64 run_id;
  char command[512] = "";
  struct run_snapshot snap;
  struct scorecard sc;

  if (sqlite3_prepare_v2(db,"SELECT id,command FROM runs WHERE hostname=? AND run_id=?;",
                         -1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: query failed: %s\n",sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,run_id_text,-1,SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW){
    sqlite3_finalize(stmt);
    fprintf(stderr,"wspy-archetype: no run found for %s:%s\n",hostname,run_id_text);
    return 1;
  }
  run_id = sqlite3_column_int64(stmt,0);
  {
    const unsigned char *c = sqlite3_column_text(stmt,1);
    snprintf(command,sizeof(command),"%s",c ? (const char *)c : "");
  }
  sqlite3_finalize(stmt);

  run_snapshot_reset(&snap,run_id,hostname,run_id_text,command);

  if (sqlite3_prepare_v2(db,"SELECT feature_name,value,coverage FROM run_features WHERE run_id=?;",
                         -1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_int64(stmt,1,run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW){
      const unsigned char *feature_name = sqlite3_column_text(stmt,0);
      double value = sqlite3_column_double(stmt,1);
      const unsigned char *coverage = sqlite3_column_text(stmt,2);
      int measured = coverage && !strcmp((const char *)coverage,"measured");
      if (feature_name) run_snapshot_apply_feature(&snap,(const char *)feature_name,value,measured);
    }
    sqlite3_finalize(stmt);
  }

  score_snapshot(&snap,&sc);
  print_scorecard_fields(out,hostname,run_id_text,command,&sc);
  return 0;
}

/* --run-guest <json-file>: same scorecard as --run, but scored from a flat JSON object of
 * feature_name->value pairs instead of a `runs`/`run_features` database row (INVESTIGATION.md 4.3
 * item 23) -- lets a caller characterize a run this store has never heard of, e.g. one recovered
 * from an already-published WordPress page for a machine with no local wspy-store presence at all
 * (web/server.py's recover_machine_metrics_from_wordpress(), item 21). No database access at all:
 * score_snapshot() is a pure function of struct run_snapshot, so this mode doesn't even require
 * --db. Every value is treated as "measured" (present in the JSON at all is the only signal this
 * mode has -- there's no separate coverage concept for external data the way store.c's `coverage`
 * column has for a real run); a key run_snapshot_apply_feature() doesn't recognize is silently
 * ignored, same forward-compatible behavior a real run_features row with an unrecognized name
 * already gets. hostname/run_id/command are always printed as "(guest)"/""  -- there's no real
 * identity to report, and wspy-testpoint's own caller supplies the actual machine label separately
 * rather than relying on this tool to know it. Returns 0 normally, 1 if the JSON can't be parsed or
 * isn't an object, matching --run's own "bad input" exit code.
 */
static int run_guest_archetype(const char *json_path,FILE *out){
  char errbuf[256];
  struct json_value *root;
  struct run_snapshot snap;
  struct scorecard sc;
  size_t i;

  root = json_parse_file(json_path,errbuf,sizeof(errbuf));
  if (!root){
    fprintf(stderr,"wspy-archetype: --run-guest %s: %s\n",json_path,errbuf);
    return 1;
  }
  if (root->type != JSON_OBJECT){
    fprintf(stderr,"wspy-archetype: --run-guest %s: expected a JSON object of feature_name->value "
                   "pairs\n",json_path);
    json_free(root);
    return 1;
  }

  run_snapshot_reset(&snap,0,"(guest)","(guest)","");
  for (i = 0; i < root->u.object.count; i++){
    const struct json_value *v = root->u.object.values[i];
    if (v->type == JSON_NUMBER){
      run_snapshot_apply_feature(&snap,root->u.object.keys[i],v->u.number,1);
    }
  }
  json_free(root);

  score_snapshot(&snap,&sc);
  print_scorecard_fields(out,"(guest)","(guest)","",&sc);
  return 0;
}

/* ---- --nearest: coverage-aware nearest-neighbor search (INVESTIGATION.md's
 * 4.3 "Clustering + nearest-neighbor + cluster profile cards" -- nearest-
 * neighbor only this cycle; K-means clustering + cluster profile cards are a
 * deliberately separate follow-up, since a common-subspace centroid has no
 * clean textbook definition when cluster members don't all share the same
 * available features -- real design work, not yet done here).
 *
 * struct run_snapshot above is a fixed 9-named-field struct built for this
 * file's own rule-based axis classification -- it doesn't even cover
 * ipc_mean or the ipc_p10/p90/iqr quantile features, let alone the full
 * run_features vocabulary (~21 names and growing). Nearest-neighbor needs
 * every measured feature a run has, generically, so any future run_features
 * addition (a new metric's quantiles, say) shows up here automatically with
 * zero code changes -- a name->value vector, not a named struct. Mirrors
 * summary.c's own struct metric_value_map/metric_map_add() growable-array-
 * by-name idiom (~21 names is small enough that linear scan is entirely
 * appropriate, no hash table needed). */
struct feature_vector {
  sqlite3_int64 run_id;
  char hostname[256],run_id_text[128];
  char **names;   /* measured feature names only -- an unavailable feature is
                    * simply absent, never stored as a sentinel value, so
                    * "shared dimensions" between two vectors is a real set
                    * intersection, not a NULL-check */
  double *values; /* raw (not yet standardized) values, same index as names */
  int n,cap;
};

static void feature_vector_add(struct feature_vector *fv,const char *name,double value){
  if (fv->n == fv->cap){
    fv->cap = fv->cap ? fv->cap * 2 : 8;
    fv->names = realloc(fv->names,sizeof(char *) * (size_t)fv->cap);
    fv->values = realloc(fv->values,sizeof(double) * (size_t)fv->cap);
  }
  fv->names[fv->n] = strdup(name);
  fv->values[fv->n] = value;
  fv->n++;
}

static void feature_vector_free_contents(struct feature_vector *fv){
  int i;
  for (i = 0; i < fv->n; i++) free(fv->names[i]);
  free(fv->names);
  free(fv->values);
}

static int feature_vector_find(const struct feature_vector *fv,const char *name,double *value_out){
  int i;
  for (i = 0; i < fv->n; i++){
    if (!strcmp(fv->names[i],name)){ *value_out = fv->values[i]; return 1; }
  }
  return 0;
}

/* Population statistics for one feature name, across every loaded
 * candidate -- standardization must use the same population for every
 * pairwise distance, not be recomputed per pair, or distances between
 * different pairs wouldn't be comparable to each other. */
struct feature_stats {
  char name[64];
  double mean,stddev;
  int count;
};

static struct feature_stats *feature_stats_find_or_add(struct feature_stats **stats,int *n,int *cap,
                                                         const char *name){
  int i;
  for (i = 0; i < *n; i++) if (!strcmp((*stats)[i].name,name)) return &(*stats)[i];
  if (*n == *cap){
    *cap = *cap ? *cap * 2 : 8;
    *stats = realloc(*stats,sizeof(struct feature_stats) * (size_t)*cap);
  }
  memset(&(*stats)[*n],0,sizeof(struct feature_stats));
  snprintf((*stats)[*n].name,sizeof((*stats)[*n].name),"%s",name);
  return &(*stats)[(*n)++];
}

/* Loads every run matching the optional command/hostname filters (same
 * shape as score_runs()'s own WHERE clause) into a growable array of
 * struct feature_vector, one per run -- coverage='measured' in the SQL
 * itself means an unavailable feature never enters a vector at all, so no
 * NULL-handling is needed downstream. Same realloc-doubling idiom
 * load_baseline_buckets() (summary.c) already uses for a growable array of
 * structs. Caller owns the returned array (feature_vector_free_contents()
 * each entry, then free() the array). */
static int load_feature_vectors(sqlite3 *db,const char *command_filter,const char *hostname_filter,
                                 struct feature_vector **out_vectors,int *out_n){
  const char *sql =
    "SELECT r.id, r.hostname, r.run_id, rf.feature_name, rf.value "
    "FROM runs r JOIN run_features rf ON rf.run_id = r.id "
    "WHERE rf.coverage = 'measured' "
    "AND (?1 = '' OR r.command LIKE '%' || ?1 || '%') "
    "AND (?2 = '' OR r.hostname = ?2) "
    "ORDER BY r.id;";
  sqlite3_stmt *stmt;
  struct feature_vector *vectors = NULL;
  int n = 0,cap = 0;
  int have_vec = 0;
  struct feature_vector cur;

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,command_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,hostname_filter,-1,SQLITE_TRANSIENT);

  memset(&cur,0,sizeof(cur));
  while (sqlite3_step(stmt) == SQLITE_ROW){
    sqlite3_int64 run_id = sqlite3_column_int64(stmt,0);
    const unsigned char *hostname = sqlite3_column_text(stmt,1);
    const unsigned char *run_id_text = sqlite3_column_text(stmt,2);
    const unsigned char *feature_name = sqlite3_column_text(stmt,3);
    double value = sqlite3_column_double(stmt,4);

    if (!have_vec || cur.run_id != run_id){
      if (have_vec){
        if (n == cap){ cap = cap ? cap * 2 : 8; vectors = realloc(vectors,sizeof(struct feature_vector) * (size_t)cap); }
        vectors[n++] = cur;
      }
      memset(&cur,0,sizeof(cur));
      cur.run_id = run_id;
      snprintf(cur.hostname,sizeof(cur.hostname),"%s",hostname ? (const char *)hostname : "?");
      snprintf(cur.run_id_text,sizeof(cur.run_id_text),"%s",run_id_text ? (const char *)run_id_text : "?");
      have_vec = 1;
    }
    if (feature_name) feature_vector_add(&cur,(const char *)feature_name,value);
  }
  if (have_vec){
    if (n == cap){ cap = cap ? cap * 2 : 8; vectors = realloc(vectors,sizeof(struct feature_vector) * (size_t)cap); }
    vectors[n++] = cur;
  }

  sqlite3_finalize(stmt);
  *out_vectors = vectors;
  *out_n = n;
  return 0;
}

/* Two-pass population mean/stddev per feature name, across every loaded
 * vector -- first pass discovers the feature-name universe and accumulates
 * sum/count (-> mean), second pass accumulates sum-of-squared-deviation
 * (-> sample stddev). Z-score standardization ((value-mean)/stddev) is why
 * this exists: raw feature scales are wildly different (ipc_mean ~0-2 vs
 * dcache_miss_pct ~0-100) and would otherwise let one dimension dominate
 * Euclidean distance for no principled reason. A feature seen on fewer than
 * 2 vectors has no meaningful stddev (stays 0, see the zero-variance
 * exclusion in nearest_neighbor_distance() below). */
static void compute_feature_stats(const struct feature_vector *vectors,int nvectors,
                                   struct feature_stats **out_stats,int *out_n){
  struct feature_stats *stats = NULL;
  int n = 0,cap = 0;
  int i,j;

  for (i = 0; i < nvectors; i++){
    for (j = 0; j < vectors[i].n; j++){
      struct feature_stats *s = feature_stats_find_or_add(&stats,&n,&cap,vectors[i].names[j]);
      s->mean += vectors[i].values[j];
      s->count++;
    }
  }
  for (i = 0; i < n; i++) if (stats[i].count > 0) stats[i].mean /= stats[i].count;

  for (i = 0; i < nvectors; i++){
    for (j = 0; j < vectors[i].n; j++){
      struct feature_stats *s = feature_stats_find_or_add(&stats,&n,&cap,vectors[i].names[j]);
      double d = vectors[i].values[j] - s->mean;
      s->stddev += d * d;
    }
  }
  for (i = 0; i < n; i++){
    if (stats[i].count >= 2) stats[i].stddev = sqrt(stats[i].stddev / (stats[i].count - 1));
    else stats[i].stddev = 0.0; /* one contributing run -- "variance" is meaningless, treat as zero */
  }

  *out_stats = stats;
  *out_n = n;
}

static const struct feature_stats *feature_stats_lookup(const struct feature_stats *stats,int n,
                                                          const char *name){
  int i;
  for (i = 0; i < n; i++) if (!strcmp(stats[i].name,name)) return &stats[i];
  return NULL;
}

/* Coverage-aware distance -- the "common-subspace" requirement: only
 * features BOTH a and b actually measured contribute, and a zero-variance
 * feature (every candidate has the identical value -- zero discriminating
 * information, and would divide-by-zero under standardization) is skipped
 * entirely. Root-*mean*-square, not root-sum-square, over the shared
 * dimensions -- dividing by n_shared is what makes distances comparable
 * across differently-covered pairs: two runs sharing 18 dimensions and two
 * runs sharing 6 shouldn't be penalized/rewarded purely for how much they
 * happened to have in common, only the RMS standardized difference across
 * whatever they *do* share matters. Returns 0 (pair excluded from results)
 * when n_shared is 0 -- an undefined distance is never fabricated as 0.0
 * ("identical") or left uninitialized. */
static int nearest_neighbor_distance(const struct feature_vector *a,const struct feature_vector *b,
                                      const struct feature_stats *stats,int nstats,
                                      double *distance_out,int *n_shared_out){
  double sumsq = 0;
  int n_shared = 0;
  int i;

  for (i = 0; i < a->n; i++){
    double bval;
    const struct feature_stats *s;
    double za,zb,diff;

    if (!feature_vector_find(b,a->names[i],&bval)) continue;
    s = feature_stats_lookup(stats,nstats,a->names[i]);
    if (!s || s->stddev <= 0.0) continue; /* zero-variance: carries no discriminating information */

    za = (a->values[i] - s->mean) / s->stddev;
    zb = (bval - s->mean) / s->stddev;
    diff = za - zb;
    sumsq += diff * diff;
    n_shared++;
  }

  if (n_shared == 0) return 0;
  *distance_out = sqrt(sumsq / n_shared);
  *n_shared_out = n_shared;
  return 1;
}

struct neighbor_result {
  const struct feature_vector *candidate;
  double distance;
  int n_shared;
};

static int cmp_neighbor_result(const void *a,const void *b){
  double da = ((const struct neighbor_result *)a)->distance;
  double db = ((const struct neighbor_result *)b)->distance;
  if (da < db) return -1;
  if (da > db) return 1;
  return 0;
}

static void print_neighbor_row(FILE *out,int csvflag,const char *hostname,const char *run_id_text,
                                double distance,int n_shared){
  if (csvflag){
    print_csv_field(out,hostname); fputc(',',out);
    print_csv_field(out,run_id_text); fputc(',',out);
    fprintf(out,"%.4g,%d\n",distance,n_shared);
  } else {
    char host_run[400];
    snprintf(host_run,sizeof(host_run),"%s:%s",hostname,run_id_text);
    fprintf(out,"%-40.40s %10.4g %18d\n",host_run,distance,n_shared);
  }
}

#define NEAREST_DEFAULT_K 5

/* --nearest <hostname>:<run_id>: ranks every other run matching the
 * optional command/hostname filters by coverage-aware distance to the
 * target, prints the closest k. Returns 1 if the target run isn't found
 * (matches --run's own not-found exit-code convention), 2 on a query
 * failure, else 0 -- including the "target has zero measured features" and
 * "no other candidates" cases, both of which print an empty result rather
 * than failing (degrade, don't crash -- same convention as every other
 * coverage-driven feature in this codebase). */
static int nearest_neighbors(sqlite3 *db,const char *hostname,const char *run_id_text,int k,
                              const char *command_filter,const char *hostname_filter,
                              int csvflag,FILE *out){
  struct feature_vector *vectors = NULL;
  int nvectors = 0;
  struct feature_stats *stats = NULL;
  int nstats = 0;
  struct neighbor_result *results = NULL;
  int nresults = 0;
  const struct feature_vector *target = NULL;
  int i,rc = 0;

  if (load_feature_vectors(db,command_filter,hostname_filter,&vectors,&nvectors) != 0) return 2;

  for (i = 0; i < nvectors; i++){
    if (!strcmp(vectors[i].hostname,hostname) && !strcmp(vectors[i].run_id_text,run_id_text)){
      target = &vectors[i];
      break;
    }
  }
  if (!target){
    /* Distinguish "not in the store at all" from "in the store, but excluded
     * by --command/--hostname or has zero measured features" -- mirrors
     * trace_run_archetype()'s own two-query existence check. */
    sqlite3_stmt *stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(db,"SELECT 1 FROM runs WHERE hostname=? AND run_id=?;",-1,&stmt,NULL) == SQLITE_OK){
      sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt,2,run_id_text,-1,SQLITE_TRANSIENT);
      exists = (sqlite3_step(stmt) == SQLITE_ROW);
      sqlite3_finalize(stmt);
    }
    if (!exists){
      fprintf(stderr,"wspy-archetype: no run found for %s:%s\n",hostname,run_id_text);
      rc = 1;
      goto done;
    }
    /* Exists, but no measured features (or filtered out) -- empty result, not an error. */
  }

  if (target){
    compute_feature_stats(vectors,nvectors,&stats,&nstats);
    results = calloc((size_t)nvectors,sizeof(struct neighbor_result));
    for (i = 0; i < nvectors; i++){
      double distance;
      int n_shared;
      if (&vectors[i] == target) continue;
      if (!nearest_neighbor_distance(target,&vectors[i],stats,nstats,&distance,&n_shared)) continue;
      results[nresults].candidate = &vectors[i];
      results[nresults].distance = distance;
      results[nresults].n_shared = n_shared;
      nresults++;
    }
    qsort(results,(size_t)nresults,sizeof(struct neighbor_result),cmp_neighbor_result);
  }

  if (csvflag) fprintf(out,"hostname,run_id,distance,compared_features\n");
  else fprintf(out,"%-40s %10s %18s\n","hostname:run_id","distance","compared_features");

  for (i = 0; i < nresults && i < k; i++){
    print_neighbor_row(out,csvflag,results[i].candidate->hostname,results[i].candidate->run_id_text,
                        results[i].distance,results[i].n_shared);
  }

done:
  free(results);
  free(stats);
  for (i = 0; i < nvectors; i++) feature_vector_free_contents(&vectors[i]);
  free(vectors);
  return rc;
}

/* ---- --kmeans: K-means clustering + cluster profile cards (INVESTIGATION.md's
 * 4.3 Tier 1 item 1, the remaining scope after --nearest shipped the
 * coverage-aware distance function this reuses).
 *
 * The design problem the backlog entry flagged -- "no clean textbook answer
 * for how a centroid should be computed when cluster members don't all share
 * the same available features" -- is resolved here the same way
 * nearest_neighbor_distance() already resolves pairwise comparison: a
 * per-dimension *available-case* mean (average the z-scored value of
 * whichever members actually measured that dimension; a dimension no
 * member measured simply doesn't exist in the centroid). This is the
 * "partial distance strategy" documented in the missing-data-clustering
 * literature (e.g. Dixon 1979; Hathaway & Bezdek's fuzzy c-means-with-
 * missing-data extension) applied to plain K-means -- not a novel
 * invention, just not previously wired up in this codebase. Assignment
 * uses the exact same coverage-aware RMS-over-shared-dimensions distance
 * --nearest uses, just against a centroid's defined dimensions instead of
 * another run's measured ones.
 *
 * Initialization is k-means++ (Arthur & Vassilvitskii 2007) seeded from real data
 * points (never a fabricated average point, which would need to invent
 * values for dimensions no nearby run actually measured) -- a real point is
 * always a well-defined centroid to start from, sidestepping the missing-
 * data question entirely for the one place it would otherwise bite hardest
 * (an initial centroid with literally zero members to average over yet). */

struct centroid_dim { char name[64]; double z; };

struct centroid {
  struct centroid_dim *dims;
  int n,cap;
  int size; /* member count in this iteration's assignment */
};

static void centroid_reset(struct centroid *c){
  free(c->dims);
  c->dims = NULL;
  c->n = c->cap = c->size = 0;
}

static int centroid_dim_find(const struct centroid *c,const char *name,double *z_out){
  int i;
  for (i = 0; i < c->n; i++){
    if (!strcmp(c->dims[i].name,name)){ *z_out = c->dims[i].z; return 1; }
  }
  return 0;
}

/* Coverage-aware distance from a raw data point to a centroid -- mirrors
 * nearest_neighbor_distance() exactly (same RMS-over-shared-dimensions,
 * same zero-variance exclusion), just against centroid dims instead of a
 * second feature_vector. */
static int centroid_distance(const struct feature_vector *v,const struct centroid *c,
                              const struct feature_stats *stats,int nstats,
                              double *distance_out,int *n_shared_out){
  double sumsq = 0;
  int n_shared = 0;
  int i;

  for (i = 0; i < v->n; i++){
    double cz,diff;
    const struct feature_stats *s;

    if (!centroid_dim_find(c,v->names[i],&cz)) continue;
    s = feature_stats_lookup(stats,nstats,v->names[i]);
    if (!s || s->stddev <= 0.0) continue;

    diff = ((v->values[i] - s->mean) / s->stddev) - cz;
    sumsq += diff * diff;
    n_shared++;
  }

  if (n_shared == 0) return 0;
  *distance_out = sqrt(sumsq / n_shared);
  *n_shared_out = n_shared;
  return 1;
}

/* Recomputes one centroid from whichever vectors are currently assigned to
 * cluster_id -- available-case mean per dimension, see file comment above.
 * A cluster with zero members leaves the centroid empty (n=0,size=0); the
 * caller (kmeans_cluster()) never lets an empty cluster survive past
 * reassignment, but this function itself stays a pure "recompute from
 * current assignment" step with no reinit policy baked in. */
static void centroid_update(struct centroid *c,const struct feature_vector *vectors,int nvectors,
                             const int *assignments,int cluster_id,
                             const struct feature_stats *stats,int nstats){
  struct { char name[64]; double sum; int count; } *accum = NULL;
  int n = 0,cap = 0;
  int i,j,size = 0;

  for (i = 0; i < nvectors; i++){
    if (assignments[i] != cluster_id) continue;
    size++;
    for (j = 0; j < vectors[i].n; j++){
      const struct feature_stats *s = feature_stats_lookup(stats,nstats,vectors[i].names[j]);
      double z;
      int k;

      if (!s || s->stddev <= 0.0) continue;
      z = (vectors[i].values[j] - s->mean) / s->stddev;

      for (k = 0; k < n; k++) if (!strcmp(accum[k].name,vectors[i].names[j])) break;
      if (k == n){
        if (n == cap){ cap = cap ? cap * 2 : 8; accum = realloc(accum,sizeof(*accum) * (size_t)cap); }
        snprintf(accum[n].name,sizeof(accum[n].name),"%s",vectors[i].names[j]);
        accum[n].sum = 0; accum[n].count = 0;
        n++;
      }
      accum[k].sum += z;
      accum[k].count++;
    }
  }

  centroid_reset(c);
  c->size = size;
  if (n > 0){
    c->dims = malloc(sizeof(struct centroid_dim) * (size_t)n);
    c->cap = c->n = n;
    for (i = 0; i < n; i++){
      snprintf(c->dims[i].name,sizeof(c->dims[i].name),"%s",accum[i].name);
      c->dims[i].z = accum[i].sum / accum[i].count;
    }
  }
  free(accum);
}

/* k-means++ seeding (Arthur & Vassilvitskii 2007): pick the first centroid
 * uniformly at random, then each subsequent one with probability
 * proportional to its squared distance from the nearest centroid chosen so
 * far -- spreads initial centroids out instead of risking several landing
 * in the same real cluster (plain-random init's classic failure mode).
 * rand_r() with a caller-supplied seed, not rand()/srand(), so results are
 * reproducible independent of any other code's global RNG state -- the
 * whole point of exposing --seed.
 *
 * dist2[i] tracks the squared distance from vector i to the nearest
 * already-chosen centroid; a pair with zero shared measured dimensions
 * (nearest_neighbor_distance() returns 0/undefined) leaves dist2[i]
 * unchanged rather than fabricating a value -- "no information" must never
 * silently read as "identical" (would starve that point of ever being
 * picked) or bias the weighting. Any entries still at the initial
 * DBL_MAX sentinel after all rounds so far (never shared a dimension with
 * any chosen centroid yet) are capped to the largest real distance seen,
 * once, right before the weighted draw -- so they're treated as "maximally
 * dissimilar so far" for selection purposes without ever contributing an
 * infinite/undefined weight to the sum. */
static void kmeans_plusplus_init(const struct feature_vector *vectors,int nvectors,int k,
                                  const struct feature_stats *stats,int nstats,
                                  unsigned int seed,int *chosen){
  double *dist2 = malloc(sizeof(double) * (size_t)nvectors);
  unsigned int state = seed;
  int i,c;

  for (i = 0; i < nvectors; i++) dist2[i] = DBL_MAX;
  chosen[0] = (int)(rand_r(&state) % (unsigned)nvectors);
  dist2[chosen[0]] = 0.0;

  for (c = 1; c < k; c++){
    int last = chosen[c - 1];
    double max_finite = 0.0;
    double total = 0.0;
    double r,running;

    for (i = 0; i < nvectors; i++){
      double d,d2;
      int n_shared;
      if (nearest_neighbor_distance(&vectors[i],&vectors[last],stats,nstats,&d,&n_shared)){
        d2 = d * d;
        if (d2 < dist2[i]) dist2[i] = d2;
      }
      if (dist2[i] < DBL_MAX && dist2[i] > max_finite) max_finite = dist2[i];
    }
    for (i = 0; i < nvectors; i++) if (dist2[i] >= DBL_MAX) dist2[i] = max_finite;
    dist2[chosen[c - 1]] = 0.0;

    for (i = 0; i < nvectors; i++) total += dist2[i];

    if (total <= 0.0){
      /* every remaining point already coincides (in shared-dimension terms)
       * with a chosen centroid -- fall back to the first not-yet-chosen
       * index so we still return k distinct centroids. */
      int already,j;
      for (i = 0; i < nvectors; i++){
        already = 0;
        for (j = 0; j < c; j++) if (chosen[j] == i){ already = 1; break; }
        if (!already) break;
      }
      chosen[c] = (i < nvectors) ? i : chosen[c - 1];
      continue;
    }

    r = ((double)rand_r(&state) / ((double)RAND_MAX + 1.0)) * total;
    running = 0.0;
    chosen[c] = chosen[c - 1]; /* fallback if float rounding leaves running < r for all i */
    for (i = 0; i < nvectors; i++){
      running += dist2[i];
      if (running >= r){ chosen[c] = i; break; }
    }
  }

  free(dist2);
}

#define KMEANS_DEFAULT_ITERATIONS 50
#define KMEANS_DEFAULT_SEED 1
#define KMEANS_TOP_FEATURES 5

struct cluster_member { const struct feature_vector *vector; double distance; };

static int cmp_cluster_member(const void *a,const void *b){
  double da = ((const struct cluster_member *)a)->distance;
  double db = ((const struct cluster_member *)b)->distance;
  if (da < db) return -1;
  if (da > db) return 1;
  return 0;
}

/* Runs Lloyd's algorithm (assign -> recompute centroids -> repeat) to
 * convergence or max_iterations, whichever first, over the coverage-aware
 * z-standardized space compute_feature_stats() defines. Returns the final
 * per-vector cluster assignment (0..k-1) and per-cluster centroids; caller
 * owns both (free(assignments), centroid_reset() each centroid then
 * free(centroids)).
 *
 * Empty-cluster handling: after each assignment pass, any cluster with zero
 * members steals the single worst-fit point (largest distance to its own
 * assigned centroid) from whichever cluster currently has more than one
 * member -- keeps every cluster non-empty without ever leaving a vector
 * unassigned, the standard practical fix for K-means' "centroid drifts to
 * where no point wants it" failure mode. */
static void kmeans_cluster(const struct feature_vector *vectors,int nvectors,int k,
                            const struct feature_stats *stats,int nstats,
                            unsigned int seed,int max_iterations,
                            int **out_assignments,struct centroid **out_centroids,
                            double **out_distances){
  int *chosen = malloc(sizeof(int) * (size_t)k);
  int *assignments = malloc(sizeof(int) * (size_t)nvectors);
  int *prev_assignments = malloc(sizeof(int) * (size_t)nvectors);
  double *distances = malloc(sizeof(double) * (size_t)nvectors);
  struct centroid *centroids = calloc((size_t)k,sizeof(struct centroid));
  int i,c,iter;

  kmeans_plusplus_init(vectors,nvectors,k,stats,nstats,seed,chosen);
  for (i = 0; i < nvectors; i++) assignments[i] = -1;
  for (c = 0; c < k; c++){
    /* seed centroid c directly from the chosen real data point's own
     * z-scored values -- a real point is always fully defined on its own
     * measured dimensions, unlike an averaged centroid with no members yet. */
    const struct feature_vector *seedv = &vectors[chosen[c]];
    int j;
    centroids[c].dims = malloc(sizeof(struct centroid_dim) * (size_t)(seedv->n ? seedv->n : 1));
    centroids[c].n = centroids[c].cap = 0;
    for (j = 0; j < seedv->n; j++){
      const struct feature_stats *s = feature_stats_lookup(stats,nstats,seedv->names[j]);
      if (!s || s->stddev <= 0.0) continue;
      snprintf(centroids[c].dims[centroids[c].n].name,sizeof(centroids[c].dims[0].name),"%s",seedv->names[j]);
      centroids[c].dims[centroids[c].n].z = (seedv->values[j] - s->mean) / s->stddev;
      centroids[c].n++;
    }
    centroids[c].cap = seedv->n ? seedv->n : 1;
    centroids[c].size = 0;
  }

  for (iter = 0; iter < max_iterations; iter++){
    int changed = 0;
    int *sizes = calloc((size_t)k,sizeof(int));

    memcpy(prev_assignments,assignments,sizeof(int) * (size_t)nvectors);

    for (i = 0; i < nvectors; i++){
      double best_d = 0.0;
      int best_c = -1;
      for (c = 0; c < k; c++){
        double d; int n_shared;
        if (!centroid_distance(&vectors[i],&centroids[c],stats,nstats,&d,&n_shared)) continue;
        if (best_c < 0 || d < best_d){ best_d = d; best_c = c; }
      }
      if (best_c < 0){
        /* vector shares zero measured dimensions with every centroid
         * (pathological -- only possible with wildly heterogeneous
         * coverage across the candidate pool): park it on the smallest
         * cluster so far rather than leaving it unassigned. */
        int smallest = 0;
        for (c = 1; c < k; c++) if (sizes[c] < sizes[smallest]) smallest = c;
        best_c = smallest;
        best_d = 0.0;
      }
      assignments[i] = best_c;
      distances[i] = best_d;
      sizes[best_c]++;
      if (assignments[i] != prev_assignments[i]) changed = 1;
    }

    for (c = 0; c < k; c++){
      while (sizes[c] == 0){
        int worst = -1,donor = -1;
        for (i = 0; i < nvectors; i++){
          if (sizes[assignments[i]] <= 1) continue; /* would create a new empty cluster */
          if (worst < 0 || distances[i] > distances[worst]){ worst = i; donor = assignments[i]; }
        }
        if (worst < 0) break; /* nvectors < k already rejected by the caller; defensive only */
        sizes[donor]--;
        assignments[worst] = c;
        distances[worst] = 0.0;
        sizes[c]++;
        changed = 1;
      }
    }
    free(sizes);

    for (c = 0; c < k; c++) centroid_update(&centroids[c],vectors,nvectors,assignments,c,stats,nstats);

    if (!changed && iter > 0) break;
  }

  /* Final distances must reflect the *final* centroids, not the ones from
   * the assignment step that produced the last `changed` -- recompute once
   * more so cluster-card distances and member ordering are accurate. */
  for (i = 0; i < nvectors; i++){
    double d; int n_shared;
    if (centroid_distance(&vectors[i],&centroids[assignments[i]],stats,nstats,&d,&n_shared)) distances[i] = d;
  }

  free(chosen);
  free(prev_assignments);
  *out_assignments = assignments;
  *out_centroids = centroids;
  *out_distances = distances;
}

static int cmp_centroid_dim_by_abs_z(const void *a,const void *b){
  double za = fabs(((const struct centroid_dim *)a)->z);
  double zb = fabs(((const struct centroid_dim *)b)->z);
  if (za > zb) return -1; /* descending by |z| -- most-distinguishing first */
  if (za < zb) return 1;
  return 0;
}

/* Renders a cluster's "profile card" headline -- the KMEANS_TOP_FEATURES
 * dimensions where this cluster's centroid sits furthest (in either
 * direction) from the overall population mean, formatted "name:+z;name:-z".
 * A copy of the centroid's dims is sorted rather than the centroid itself,
 * since the centroid is still live (read again on subsequent calls for
 * other member rows of the same cluster). */
static void format_top_features(const struct centroid *c,char *buf,size_t bufsize){
  struct centroid_dim *sorted;
  int i,shown,limit;
  size_t used = 0;

  buf[0] = '\0';
  if (c->n == 0) return;

  sorted = malloc(sizeof(struct centroid_dim) * (size_t)c->n);
  memcpy(sorted,c->dims,sizeof(struct centroid_dim) * (size_t)c->n);
  qsort(sorted,(size_t)c->n,sizeof(struct centroid_dim),cmp_centroid_dim_by_abs_z);

  limit = c->n < KMEANS_TOP_FEATURES ? c->n : KMEANS_TOP_FEATURES;
  for (i = 0,shown = 0; i < limit; i++){
    int n = snprintf(buf + used,bufsize - used,"%s%s:%+.2f",
                      shown ? ";" : "",sorted[i].name,sorted[i].z);
    if (n < 0 || (size_t)n >= bufsize - used) break;
    used += (size_t)n;
    shown++;
  }
  free(sorted);
}

static void print_cluster_member_row(FILE *out,int csvflag,int cluster_id,int cluster_size,
                                      const char *hostname,const char *run_id_text,
                                      double distance,const char *top_features){
  if (csvflag){
    fprintf(out,"%d,%d,",cluster_id,cluster_size);
    print_csv_field(out,hostname); fputc(',',out);
    print_csv_field(out,run_id_text); fputc(',',out);
    fprintf(out,"%.4g,",distance);
    print_csv_field(out,top_features);
    fputc('\n',out);
  } else {
    char host_run[400];
    snprintf(host_run,sizeof(host_run),"%s:%s",hostname,run_id_text);
    fprintf(out,"%6d %6d %-40.40s %10.4g  %s\n",cluster_id,cluster_size,host_run,distance,top_features);
  }
}

/* --kmeans <k>: partitions every run matching the optional --command/
 * --hostname filters into k clusters (see the design comment above
 * kmeans_cluster()) and prints one row per member, grouped by cluster
 * ascending then by distance-to-centroid ascending (the most representative
 * member of each cluster prints first). Returns 1 when there are fewer
 * candidate runs than k (not enough data to form k clusters -- a data-
 * availability condition, same rc convention as --run/--nearest's
 * "no such run"), 2 on a query failure, else 0. */
static int kmeans_report(sqlite3 *db,int k,unsigned int seed,int max_iterations,
                          const char *command_filter,const char *hostname_filter,
                          int csvflag,FILE *out){
  struct feature_vector *vectors = NULL;
  int nvectors = 0;
  struct feature_stats *stats = NULL;
  int nstats = 0;
  int *assignments = NULL;
  struct centroid *centroids = NULL;
  double *distances = NULL;
  int c,i;

  if (load_feature_vectors(db,command_filter,hostname_filter,&vectors,&nvectors) != 0) return 2;

  if (nvectors < k){
    fprintf(stderr,"wspy-archetype: only %d candidate run(s) available, need at least k=%d\n",nvectors,k);
    for (i = 0; i < nvectors; i++) feature_vector_free_contents(&vectors[i]);
    free(vectors);
    return 1;
  }

  compute_feature_stats(vectors,nvectors,&stats,&nstats);
  kmeans_cluster(vectors,nvectors,k,stats,nstats,seed,max_iterations,&assignments,&centroids,&distances);

  if (csvflag) fprintf(out,"cluster,size,hostname,run_id,distance,top_features\n");
  else fprintf(out,"%6s %6s %-40s %10s  %s\n","cluster","size","hostname:run_id","distance","top_features (name:z-score)");

  for (c = 0; c < k; c++){
    char top_features[512];
    struct cluster_member *members = malloc(sizeof(struct cluster_member) * (size_t)centroids[c].size);
    int nmembers = 0;

    format_top_features(&centroids[c],top_features,sizeof(top_features));

    for (i = 0; i < nvectors; i++){
      if (assignments[i] != c) continue;
      members[nmembers].vector = &vectors[i];
      members[nmembers].distance = distances[i];
      nmembers++;
    }
    qsort(members,(size_t)nmembers,sizeof(struct cluster_member),cmp_cluster_member);

    for (i = 0; i < nmembers; i++){
      print_cluster_member_row(out,csvflag,c,centroids[c].size,
                                members[i].vector->hostname,members[i].vector->run_id_text,
                                members[i].distance,top_features);
    }
    free(members);
  }

  free(assignments);
  free(distances);
  for (c = 0; c < k; c++) centroid_reset(&centroids[c]);
  free(centroids);
  free(stats);
  for (i = 0; i < nvectors; i++) feature_vector_free_contents(&vectors[i]);
  free(vectors);
  return 0;
}

#ifndef TEST_ARCHETYPE
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s --db <path> [options]\n"
    "\n"
    "Classifies wspy runs recorded in a normalized store (wspy-store --db\n"
    "<path>) along several workload \"archetype\" axes, scored from store.c's\n"
    "run_features -- INVESTIGATION.md's 4.2 \"Archetype scorecard\" item.\n"
    "\n"
    "resource_dominance is the headline axis (compute-bound/frontend-bound/\n"
    "memory-bound/speculation-bound, ranked from topdown L1 percentages),\n"
    "with a top-2 alternative and an overall confidence level (high/medium/\n"
    "low/insufficient-data, with reasons). parallelism_shape/control_flow_style/\n"
    "runtime_stability/allocation_pressure/vectorization_density/core_utilization\n"
    "are simpler supporting tags, \"unknown\" when their source run_features value\n"
    "wasn't collected (needs --per-core/--branch/--interval/rusage [always\n"
    "collected]/--float [AMD only]/rusage+manifest [always collected] respectively).\n"
    "\n"
    "Options:\n"
    "  --db <path>          normalized store database (required)\n"
    "  --command <substr>   only include runs whose command matches this substring\n"
    "  --hostname <name>    only include runs from this host\n"
    "  --csv                machine-readable CSV output instead of the human table\n"
    "  -h, --help           show this help\n"
    "\n"
    "  --run <host>:<run_id>  standalone mode: detailed scorecard for one run\n"
    "                       (as named by a run-index record), ignoring every\n"
    "                       other option above except --db. Prints key=value\n"
    "                       lines.\n"
    "\n"
    "  --run-guest <json>   standalone mode: same scorecard as --run, but scored\n"
    "                       from a flat JSON object of feature_name->value pairs\n"
    "                       instead of a database row -- no --db needed at all.\n"
    "                       For characterizing a run this store has never heard\n"
    "                       of (e.g. one recovered from an already-published\n"
    "                       page for a machine with no local store presence).\n"
    "                       An unrecognized JSON key is silently ignored. Prints\n"
    "                       the same key=value lines as --run, with hostname/\n"
    "                       run_id always \"(guest)\".\n"
    "\n"
    "  --nearest <host>:<run_id>  standalone mode: rank every other run in the\n"
    "                       store by similarity to this one, over whichever\n"
    "                       run_features both runs have measured (a\n"
    "                       coverage-aware, z-score-standardized RMS distance\n"
    "                       -- a pair sharing 6 features isn't penalized just\n"
    "                       for sharing less than a pair sharing 18). --command/\n"
    "                       --hostname filter the candidate pool; --db is the\n"
    "                       only other option honored alongside it.\n"
    "  --k <n>              number of nearest neighbors to print with --nearest\n"
    "                       (default 5)\n"
    "\n"
    "  --kmeans <n>         standalone mode: partition every run matching\n"
    "                       --command/--hostname into n clusters (K-means over\n"
    "                       the same coverage-aware z-standardized distance\n"
    "                       --nearest uses) and print one row per member,\n"
    "                       grouped by cluster then by distance-to-centroid\n"
    "                       (most representative member first). A cluster's\n"
    "                       centroid, per dimension, averages only the members\n"
    "                       that actually measured that feature -- there's no\n"
    "                       single textbook centroid definition when members\n"
    "                       don't all share the same coverage, so this is the\n"
    "                       same \"whatever they share\" idiom --nearest's\n"
    "                       distance already uses. --db, --command, --hostname,\n"
    "                       --csv, --seed, --iterations are the only other\n"
    "                       options honored alongside it.\n"
    "  --seed <n>           RNG seed for --kmeans' k-means++ initialization\n"
    "                       (default 1; same seed + same data always yields\n"
    "                       the same clustering)\n"
    "  --iterations <n>     max Lloyd's-algorithm iterations for --kmeans\n"
    "                       before giving up on convergence (default 50)\n"
    "\n"
    "Only runs that have gone through feature extraction (default on in\n"
    "wspy-store, opt out via --no-feature-extract) are scored -- a run with\n"
    "no run_features rows at all has nothing to classify, distinct from one\n"
    "whose axes all came back \"unknown\"/\"unavailable\".\n"
    "\n"
    "Exit status: 0 normally; 1 with --run/--nearest if no such run is\n"
    "recorded, with --run-guest if the JSON can't be parsed or isn't an object,\n"
    "or with --kmeans if fewer candidate runs than n are available;\n"
    "2 on a usage error or if the database could not be opened.\n",
    prog);
}

int main(int argc,char **argv){
  int opt;
  const char *db_path = NULL;
  const char *command_filter = "";
  const char *hostname_filter = "";
  const char *run_key = NULL;
  const char *run_guest_key = NULL;
  const char *nearest_key = NULL;
  const char *kmeans_key = NULL;
  int k = NEAREST_DEFAULT_K;
  unsigned int seed = KMEANS_DEFAULT_SEED;
  int iterations = KMEANS_DEFAULT_ITERATIONS;
  int csvflag = 0;
  sqlite3 *db;
  int rc;

  static struct option long_options[] = {
    { "db",         required_argument, 0, 'd' },
    { "command",    required_argument, 0, 'c' },
    { "hostname",   required_argument, 0, 'H' },
    { "csv",        no_argument,       0, 'C' },
    { "run",        required_argument, 0, 'r' },
    { "run-guest",  required_argument, 0, 'g' },
    { "nearest",    required_argument, 0, 'n' },
    { "k",          required_argument, 0, 'k' },
    { "kmeans",     required_argument, 0, 'K' },
    { "seed",       required_argument, 0, 'S' },
    { "iterations", required_argument, 0, 'I' },
    { "help",       no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"h",long_options,NULL)) != -1){
    switch (opt){
    case 'd': db_path = optarg; break;
    case 'c': command_filter = optarg; break;
    case 'H': hostname_filter = optarg; break;
    case 'C': csvflag = 1; break;
    case 'r': run_key = optarg; break;
    case 'g': run_guest_key = optarg; break;
    case 'n': nearest_key = optarg; break;
    case 'k': k = atoi(optarg); break;
    case 'K': kmeans_key = optarg; break;
    case 'S': seed = (unsigned int)strtoul(optarg,NULL,10); break;
    case 'I': iterations = atoi(optarg); break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if ((run_key && run_guest_key) || (run_key && nearest_key) || (run_key && kmeans_key) ||
      (run_guest_key && nearest_key) || (run_guest_key && kmeans_key) || (nearest_key && kmeans_key)){
    fprintf(stderr,
            "wspy-archetype: --run, --run-guest, --nearest, and --kmeans are mutually exclusive\n\n");
    usage(argv[0]);
    return 2;
  }
  /* --run-guest scores a caller-supplied JSON feature vector via the pure score_snapshot()
   * function -- no database involved at all, so it's dispatched before the --db requirement below
   * applies to every other mode. */
  if (run_guest_key){
    return run_guest_archetype(run_guest_key,stdout);
  }
  if (!db_path){
    fprintf(stderr,"wspy-archetype: --db <path> is required\n\n");
    usage(argv[0]);
    return 2;
  }
  if (k < 1) k = 1;
  if (iterations < 1) iterations = 1;

  if (kmeans_key){
    int kmeans_k = atoi(kmeans_key);

    if (kmeans_k < 1){
      fprintf(stderr,"wspy-archetype: --kmeans expects a cluster count >= 1, got '%s'\n\n",kmeans_key);
      usage(argv[0]);
      return 2;
    }

    db = open_archetype_db(db_path);
    if (!db) return 2;
    rc = kmeans_report(db,kmeans_k,seed,iterations,command_filter,hostname_filter,csvflag,stdout);
    sqlite3_close(db);
    return rc;
  }

  if (nearest_key){
    const char *colon = strchr(nearest_key,':');
    char hostname_buf[256];

    if (!colon || colon == nearest_key || colon[1] == '\0'){
      fprintf(stderr,"wspy-archetype: --nearest expects <hostname>:<run_id>, got '%s'\n\n",nearest_key);
      usage(argv[0]);
      return 2;
    }
    snprintf(hostname_buf,sizeof(hostname_buf),"%.*s",(int)(colon - nearest_key),nearest_key);

    db = open_archetype_db(db_path);
    if (!db) return 2;
    rc = nearest_neighbors(db,hostname_buf,colon + 1,k,command_filter,hostname_filter,csvflag,stdout);
    sqlite3_close(db);
    return rc;
  }

  if (run_key){
    const char *colon = strchr(run_key,':');
    char hostname_buf[256];

    if (!colon || colon == run_key || colon[1] == '\0'){
      fprintf(stderr,"wspy-archetype: --run expects <hostname>:<run_id>, got '%s'\n\n",run_key);
      usage(argv[0]);
      return 2;
    }
    snprintf(hostname_buf,sizeof(hostname_buf),"%.*s",(int)(colon - run_key),run_key);

    db = open_archetype_db(db_path);
    if (!db) return 2;
    rc = trace_run_archetype(db,hostname_buf,colon + 1,stdout);
    sqlite3_close(db);
    return rc;
  }

  db = open_archetype_db(db_path);
  if (!db) return 2;

  rc = score_runs(db,command_filter,hostname_filter,csvflag,stdout);
  sqlite3_close(db);
  return (rc < 0) ? 2 : 0;
}
#endif /* TEST_ARCHETYPE */
