/*
 * cgroup.h - best-effort cgroup v2 identity + resource limits + CPU-
 * throttling delta over the measurement window (INVESTIGATION.md's 4.2
 * Tier 1 "cgroup identity + limits in manifest, cpu.stat throttling stats"
 * item) -- needed for fair comparison of runs in containerized
 * environments, where a cpu.max quota or an ongoing cpu.stat throttling
 * episode can explain a degraded result that has nothing to do with the
 * workload itself.
 *
 * cgroup v2 (unified hierarchy) only -- a pure cgroup v1 host (no "0::"
 * line in /proc/self/cgroup) degrades the whole thing to unavailable, the
 * same "measured vs unavailable" idiom provenance.c/coverage.c use
 * elsewhere. Every limit field degrades independently too (e.g. a
 * container that only exposes some cgroup files) -- a missing file is not
 * fatal.
 *
 * Identity/limits are read once (they don't meaningfully change mid-run);
 * cpu.stat's nr_periods/nr_throttled/throttled_usec are cumulative
 * counters since the cgroup's own creation, so they're read *twice* --
 * once near workload launch (the baseline) and once at manifest-write
 * time -- and the caller (wspy.c) reports the delta, matching
 * read_counters()'s own before/after idiom for perf counters rather than
 * provenance.c's one-shot facts.
 */
#ifndef _WSPY_CGROUP_H
#define _WSPY_CGROUP_H 1

struct cgroup_throttle {
  int available;
  unsigned long long nr_periods;
  unsigned long long nr_throttled;
  unsigned long long throttled_usec;
};

struct cgroup_info {
  int available;              /* a cgroup v2 path was found at all */
  char path[512];             /* e.g. "/user.slice/user-1000.slice/session-2.scope" */

  int cpu_max_available;
  long long cpu_quota_us;     /* -1 = unlimited ("max") */
  long long cpu_period_us;

  int cpu_weight_available;
  int cpu_weight;              /* cgroup v2 default 100 */

  int memory_max_available;
  long long memory_max_bytes;  /* -1 = unlimited ("max") */

  int memory_high_available;
  long long memory_high_bytes; /* -1 = not set ("max") */
};

/* Identity + limits, read once. Always succeeds (never fails the run) --
 * info->available is 0 if no cgroup v2 path was found at all, in which
 * case every other field is left at its zeroed default and not meaningful. */
void cgroup_collect_identity_and_limits(struct cgroup_info *info);

/* One cpu.stat snapshot for info->path (requires
 * cgroup_collect_identity_and_limits() to have already run) -- call once
 * near workload launch and again at manifest-write time; the delta
 * between the two is the throttling that happened during this run
 * specifically, not since the cgroup was created. out->available is 0 if
 * info->available is 0 or cpu.stat couldn't be read. */
void cgroup_read_throttle(const struct cgroup_info *info,struct cgroup_throttle *out);

/* end - start, field by field. delta->available only when both inputs
 * were. Pure/testable -- no filesystem access. */
void cgroup_throttle_delta(const struct cgroup_throttle *start,const struct cgroup_throttle *end,
                           struct cgroup_throttle *delta);

/* Run-lifetime state, owned here (defined in cgroup.c) rather than as a
 * wspy.c-local, mirroring affinity.h's requested_affinity/affinity_active
 * precedent. wspy.c calls cgroup_collect_identity_and_limits(&cgroup_state)
 * plus cgroup_read_throttle(&cgroup_state,&cgroup_throttle_baseline) once,
 * near workload launch (right before the --passes/single-pass branch, so
 * it applies uniformly to both); populate_manifest_common() takes a fresh
 * throttle reading at manifest-write time and diffs it against
 * cgroup_throttle_baseline. */
extern struct cgroup_info cgroup_state;
extern struct cgroup_throttle cgroup_throttle_baseline;

#endif
