/*
 * affinity.h - core/thread affinity control (INVESTIGATION.md's
 * "Core/thread affinity control" item): pin the launched workload to a
 * selected set of logical CPUs via sched_setaffinity() on the forked child,
 * before execve, in topdown.c's launch_child().
 *
 * Vocabulary (--affinity=<spec>, wspy.c's parse_options()):
 *   all            every CPU currently available to this wspy process (the
 *                  default -- today's implicit behavior, made an explicit,
 *                  no-op choice rather than "absence of --affinity")
 *   thread=<id>    pin to exactly logical CPU <id> -- lets a caller
 *                  deliberately avoid that CPU's SMT sibling by simply never
 *                  scheduling on it
 *   nosmt          one thread per core, across every core -- the "turn off
 *                  hyperthreading" preset: every core's lowest-numbered (
 *                  "primary") SMT sibling, none of the rest
 *   domain=<id>    every thread on one L3-sharing core-complex/CCD (id is an
 *                  index into the L3 domains discovered on this host, see
 *                  affinity_print_report()/--list-affinity below)
 *   coretype=<id>  every thread of one microarchitecture/core type (id is an
 *                  index into the core types discovered on this host) -- the
 *                  heterogeneous-core counterpart to domain=<id>: picks e.g.
 *                  only a big.LITTLE ARM part's "big" Cortex-A7xx cores or
 *                  only its "little" Cortex-A5xx ones, even when (unlike a
 *                  CCD split) they all share one combined L3, so domain=<id>
 *                  can't tell them apart
 *   cpuset=<list>  explicit core list, the general form the others are
 *                  shorthand for -- comma-separated CPU numbers and/or
 *                  ranges, e.g. "0,2-3,7"
 *
 * Topology discovery (SMT sibling grouping + L3-sharing domains) comes from
 * /sys/devices/system/cpu/cpu<N>/topology/{thread_siblings_list,core_id,
 * physical_package_id} and .../cache/index<K>/{level,shared_cpu_list,size}
 * -- the same facts scripts/map_cpu_hierarchy.py maps out for a human, but
 * read directly here since a real run can't shell out to a helper script
 * (and to keep this a pure C module with no Python dependency). Core-type
 * grouping (coretype=<id>) comes from ARM's per-CPU MIDR_EL1 register
 * (.../cpu<N>/regs/identification/midr_el1, implementer+part fields only --
 * variant/revision are just silicon steppings within the same
 * microarchitecture, not a different core type), the same signal
 * map_cpu_hierarchy.py's decode_midr()/get_core_type_score() use to tell
 * e.g. Cortex-A720 "big" cores from Cortex-A520 "little" ones on a mixed
 * part; a host with no midr_el1 (x86, or an ARM host reporting one uniform
 * MIDR across all cores) simply has every CPU's core_type left unassigned
 * (see below) and ncoretypes stays 0, so coretype=<id> degrades to a clear
 * "no such core type" error rather than silently doing nothing. Detecting
 * x86 hybrid parts (Intel Atom+Core, already tracked as CORE_INTEL_ATOM/
 * CORE_INTEL_CORE in cpu_info.c's own per-core vendor field) as a coretype=
 * grouping too is a natural follow-up, not implemented here yet.
 *
 * A resolved spec never fatal's here on a request that overlaps this
 * process's own available-CPU mask only partially (that CPU subset is used,
 * with a warning) -- but affinity_resolve() does return -1 when nothing in
 * the request is available at all, or the id was flatly out of range, since
 * a run that resolves to zero eligible CPUs cannot be launched (the caller,
 * wspy.c, treats that as fatal for a real run).
 *
 * Doesn't include wspy.h/cpu_info.h itself (like coverage.h/preflight.h/
 * phase.h) so wspy.h can include this header for the struct affinity_spec
 * type without a cycle; affinity.c includes wspy.h itself for cpu_info's
 * per-CPU availability mask.
 */
#ifndef _WSPY_AFFINITY_H
#define _WSPY_AFFINITY_H 1

/* cpu_set_t/CPU_SET()/etc. are glibc extensions gated on _GNU_SOURCE (or
 * _DEFAULT_SOURCE) -- that macro must be defined before the *first* system
 * header a translation unit includes (glibc's feature-test decisions are
 * locked in then), so it's each .c file's job to define it up front (as
 * affinity.c/wspy.c/topdown.c/test_affinity.c already do), not this
 * header's -- defining it here would be too late for a .c file that already
 * included <stdio.h> or similar before reaching this include. */
#include <sched.h>
#include <stdio.h>

enum affinity_mode { AFFINITY_ALL = 0, AFFINITY_THREAD, AFFINITY_NOSMT, AFFINITY_DOMAIN, AFFINITY_CPUSET, AFFINITY_CORETYPE };

const char *affinity_mode_name(enum affinity_mode mode);

/* Topology facts about one logical CPU -- kept separate from cpu_info.h's
 * struct cpu_core_info (which is about counter/PMU capability, not
 * scheduling placement) even though both index by logical CPU number. */
struct affinity_cpu_info {
  int core_id;           /* topology/core_id, -1 if unreadable            */
  int package_id;        /* topology/physical_package_id, -1 if unreadable */
  int is_primary_thread;  /* lowest-numbered CPU among its SMT siblings     */
  int l3_domain;          /* index into affinity_topology.l3domains[], -1 if none found */
  int core_type;          /* index into affinity_topology.coretypes[], -1 if undetected (no midr_el1, e.g. x86) */
};

struct affinity_l3_domain {
  cpu_set_t cpus;
  unsigned long size_bytes; /* 0 if unknown */
};

/* One MIDR-distinct microarchitecture/core type (ARM only today -- see the
 * header comment above). implementer/part are MIDR_EL1's raw Implementer
 * (bits 31:24) and PartNum (bits 15:4) fields, deliberately not decoded into
 * a vendor/model name here (that full lookup table lives in
 * scripts/map_cpu_hierarchy.py, not duplicated in this smaller C module) --
 * affinity_print_report() prints them as plain hex, pointing at that script
 * for a human-readable name. */
struct affinity_core_type {
  unsigned int implementer;
  unsigned int part;
  cpu_set_t cpus;
};

struct affinity_topology {
  int ncpus;
  struct affinity_cpu_info *cpu;        /* array, length ncpus */
  int nl3domains;
  struct affinity_l3_domain *l3domains; /* array, length nl3domains */
  int ncoretypes;
  struct affinity_core_type *coretypes; /* array, length ncoretypes */
};

extern struct affinity_topology affinity_topology;

/* Discovers topology for logical CPUs 0..cpu_info->num_cores-1. Safe to call
 * more than once (rebuilds from scratch, freeing any previous discovery);
 * never fails -- a CPU/domain whose sysfs files can't be read just degrades
 * (core_id/package_id/l3_domain -1), matching this codebase's "measured vs
 * unavailable" idiom elsewhere. Must be called after inventory_cpu(). */
void affinity_topology_discover(void);

/* Frees affinity_topology's arrays and resets it to empty. */
void affinity_topology_free(void);

/* One resolved --affinity request. set/id are meaningful per mode: cpuset=
 * fills in set directly at parse time; thread=/domain= use id; nosmt/all
 * need neither (resolved purely from topology/cpu_info). */
struct affinity_spec {
  enum affinity_mode mode;
  int id;
  cpu_set_t set; /* explicit set (cpuset=), or the resolved answer after affinity_resolve() */
};

/* Parses one --affinity=<spec> argument (the text after "--affinity="/
 * "--affinity ") into *spec. Returns 0 on success; -1 on a malformed spec
 * (unknown mode keyword, non-numeric id, unparsable/empty cpuset list) --
 * *spec is left unmodified on failure. Doesn't touch cpu_info/affinity_topology
 * (they may not exist yet at CLI-parse time) -- see affinity_resolve(). */
int affinity_parse_spec(const char *arg,struct affinity_spec *spec);

/* Resolves spec->mode (+ id/set) into a final cpu_set_t in spec->set, using
 * affinity_topology and cpu_info's own available-CPU mask (cpu_info->
 * coreinfo[i].is_available, i.e. this process's current sched affinity --
 * "all" means every CPU currently visible to wspy itself, matching
 * inventory_cpu()'s own convention). Requires affinity_topology_discover()
 * and inventory_cpu() to have already run. A request that partially
 * overlaps the available mask is intersected down to that overlap (warned,
 * not fatal); one that doesn't overlap at all, or names an out-of-range
 * thread/domain id, returns -1 (logged via error()) rather than resolving
 * to an empty/meaningless set. */
int affinity_resolve(struct affinity_spec *spec);

/* wspy-level state (populated by wspy.c's parse_options()/main(), read by
 * topdown.c's launch_child() -- like counter_mask/aflag, this lives in
 * wspy.c despite being declared here rather than wspy.h, since affinity.h
 * has no dependency on wspy.h/cpu_info.h and both consumers already include
 * this header directly). requested_affinity holds the parsed (then, after
 * main() calls affinity_resolve(), resolved) spec; affinity_active is 0 for
 * the default AFFINITY_ALL request (launch_child() skips the
 * sched_setaffinity() call entirely in that case -- a documented no-op, see
 * the "all" vocabulary entry above) and 1 once main() has resolved a real
 * restriction. */
extern struct affinity_spec requested_affinity;
extern int affinity_active;

/* Renders a cpu_set_t as a compact ascending, run-length-collapsed list
 * ("0,2-3,7") into buf -- used for manifest/run-index provenance and
 * human-readable printing. Truncates (with a trailing "...") rather than
 * overflowing buf if the set is large. */
void affinity_format_cpu_set(const cpu_set_t *set,int ncpus,char *buf,size_t bufsize);

/* Prints every discovered CPU's core_id/package_id/l3_domain/primary-thread
 * flag plus an L3-domain summary (id, cpu list, size) to fp -- used by the
 * standalone `wspy --list-affinity` probe (no privileges/workload needed,
 * mirrors --capabilities/--preflight's own pattern) and folded into
 * --capabilities' own combined report. One CSV-shaped table (not dual CSV/
 * human like print_system()/print_metrics() -- this report has exactly one
 * consumer shape, matching print_capability_report()'s own always-text
 * style, but comma-separated so it doubles as something a script or the web
 * launcher can parse without a JSON library). */
void affinity_print_report(FILE *fp);

#endif
