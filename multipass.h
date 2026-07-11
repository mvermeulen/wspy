/*
 * multipass.h - native multi-pass counter execution: pure, unit-testable
 * pieces of --passes=<name1>,<name2>,... (INVESTIGATION_4.0.md's 4.1 Tier 1
 * "Native multi-pass counter execution" item).
 *
 * --passes takes a comma-separated list of counter-group names (the same
 * names as the long-option flags, e.g. "ipc,topdown,cache2,software") and
 * automatically partitions them into N passes that each fit the available
 * general-purpose hardware PMU counter budget, reusing preflight.c's
 * existing per-group slot-usage arithmetic rather than a hand-curated
 * bundle table (contrast with wspy-run's PASS_FLAGS arrays, which are
 * exactly that -- a fixed, hand-sized table per builtin profile). The
 * trade-off: the number of actual passes for the same --passes flags can
 * differ between hosts with a different available slot count (e.g.
 * nmi_watchdog on vs off, 5 vs 6 slots).
 *
 * This header/its .c file holds only pure computation -- no perf_event_open,
 * no child launching, no privileges needed -- so it can be exercised
 * directly by test_wspy.c the same way preflight.c/phase.c already are. The
 * actual per-pass execution loop (run_multipass()) lives in wspy.c, since it
 * is deeply coupled to wspy.c's own execution-state globals.
 */
#ifndef _WSPY_MULTIPASS_H
#define _WSPY_MULTIPASS_H 1

/* One --passes=<name> token and the COUNTER_* bit it maps to. */
struct multipass_group_name {
  const char *name;
  unsigned int bit;
};

/* Declaration order (ascending COUNTER_* bit value, wspy.h's own existing
 * order -- no new ordering invented) is also the deterministic bin-packing
 * iteration order multipass_plan_build() uses, so --passes=cache2,ipc and
 * --passes=ipc,cache2 always produce the identical plan. COUNTER_IBS is
 * deliberately absent -- --passes is fatal'd against --ibs-basic/
 * --ibs-memory-deep in wspy.c, since IBS has its own separate system-wide
 * PMU budget already excluded from preflight.c's general-purpose count. */
extern const struct multipass_group_name multipass_group_names[];
extern const int multipass_n_group_names;

/* Looks up a --passes=<name> token against multipass_group_names[].
 * Returns 1 and sets *bit_out on a match, 0 (bit_out untouched) otherwise. */
int multipass_lookup_group_name(const char *name,unsigned int *bit_out);

/* pass_mask[0] is the "primary" pass: run_multipass() (wspy.c) sources the
 * merged CSV/manifest's base rusage/elapsed/exit-status fields from it,
 * since those don't have one canonical value across N separately-timed
 * re-executions of the same command. */
struct multipass_plan {
  int npasses;
  unsigned int *pass_mask; /* array[npasses] */
};

/* Greedily bin-packs requested_mask's set bits (iterated in
 * multipass_group_names[] order) into passes that each fit the available
 * general-purpose hardware PMU slot budget, reusing preflight_evaluate()'s
 * real setup_counter_groups()-backed arithmetic for each tentative
 * combination -- this correctly handles cases where two groups share an
 * underlying counter (e.g. --dcache/--icache/--tlb all reuse one
 * "instructions" event in cache_events[], topdown.c) rather than
 * double-counting by summing independently-evaluated per-bit costs. A
 * single group that alone exceeds the budget still gets its own
 * (multiplexing) pass, with the same warning preflight_warn_if_tight()
 * already prints for a tight single-pass run, rather than blocking the
 * whole feature. Pure computation: no privileges, no perf_event_open()
 * calls, safe to run before any workload launches. */
struct multipass_plan multipass_plan_build(unsigned int requested_mask);

/* --multiplex variant: always a single pass covering every bit in
 * requested_mask, relying on the kernel to multiplex counters that don't
 * fit the general-purpose hardware PMU budget rather than bin-packing them
 * into N separate re-executions of the workload. Only correct to offer as
 * an alternative to multipass_plan_build() now that read_counters()
 * (topdown.c) scales a multiplexed counter's value by its
 * time_running/time_enabled ratio instead of silently undercounting it
 * (INVESTIGATION_4.0.md 4.1 Tier 1 #4) -- the trade-off is precision (more
 * multiplexing means a lower per-counter confidence_ratio(), not wrong
 * values) for a single workload execution instead of N. Still routed
 * through preflight_evaluate()/preflight_warn_if_tight() so a tight fit
 * warns the same way a single overflowing bin-packed pass already does.
 * Pure computation, same as multipass_plan_build(). */
struct multipass_plan multipass_plan_build_multiplexed(unsigned int requested_mask);

/* Frees plan->pass_mask. Does not need to be called before process exit
 * (this codebase never frees counter_group lists either -- see
 * preflight.h) but is provided for tests that build/discard many plans. */
void multipass_plan_free(struct multipass_plan *plan);

#endif
