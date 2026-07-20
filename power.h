/*
 * power.h - CPU energy/power via the Linux power/power_core dynamic PMUs
 * (energy-pkg/energy-core, RAPL-equivalent).
 *
 * power/power_core are exposed to userspace as dynamic PMUs the kernel
 * registers at boot, present only when the running CPU/kernel combination
 * actually supports them (Intel RAPL support has existed for years; AMD
 * Family 19h+ support is newer). Like AMD IBS (see ibs.h), there's no fixed
 * perf_event "type" constant -- each PMU's numeric type, its one format
 * field's bit location, and its one real event's raw config value plus
 * Joules-per-LSB scale are all read from
 * /sys/bus/event_source/devices/{power,power_core}/* at runtime instead of
 * hardcoded, since none of this is guaranteed stable across kernel versions.
 * See doc/INVESTIGATION_ARCHIVE.md's "Concrete design: CPU energy/power via the
 * power/power_core perf PMUs" for the full writeup.
 *
 * Much simpler than ibs.h: power/power_core each expose exactly one format
 * field ("event", a plain 0-255 raw index -- no bitfield encoding to parse
 * the way IBS's l3missonly/ldlat filters need) and one real event apiece
 * (energy-pkg/energy-core), so there's no profile/filter machinery here.
 *
 * Per-core energy (power_core) support (INVESTIGATION.md's 4.2 Tier 1,
 * "Per-core energy support" item): power_core is a real per-physical-core
 * RAPL-equivalent counter, but its own sysfs cpumask (parsed by
 * power_probe_at() into struct power_capabilities.core_cpus) lists only one
 * representative logical CPU per physical core -- confirmed live (AMD
 * Zen5, SMT2): "0,2,4,...,30" out of 32 logical CPUs. power_core_counter_
 * group() (wspy.c's --per-core setup) opens a real event only on those
 * representative CPUs; every other per-core-enabled CPU still gets a
 * structurally identical group (same "core_joules" column, so --per-core's
 * CSV header/row column counts stay in lockstep across every row) but with
 * its one counter marked POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE so
 * setup_counters() (topdown.c) skips perf_event_open() and coverage_note()
 * for it entirely -- not "requested but failed" (which would skew
 * counters_requested/measured and preflight.c's budget estimate), genuinely
 * never attempted, same as an SMT sibling simply not being a valid read
 * point for this specific PMU. Reads 0.0 on those rows -- the same
 * "0 = not available/not applicable, documented" convention used
 * throughout this codebase (e.g. proctree.c's -X/-B fields), applied here
 * to "not applicable to this specific CPU" rather than "not applicable to
 * this vendor". print_power_core() (topdown.c, dispatched via the new,
 * internal-only COUNTER_POWER_CORE mask bit -- never a user-facing flag,
 * never in COUNTER_ALL, set automatically only when --power and --per-core
 * are both already in effect) emits core_joules/core_watts as new trailing
 * per-core-row columns, alongside (not replacing) the existing systemwide
 * pkg_joules/pkg_watts columns power_counter_group() already produces.
 */
#ifndef _WSPY_POWER_H
#define _WSPY_POWER_H 1

/* Uses FILE *outfile from wspy.h, like coverage.h/ibs.h -- see coverage.h's
 * own comment for why wspy.h isn't included here directly. */

#define POWER_MAX_FORMAT_FIELDS 4

/* Bound on the number of power_core/cpumask representative CPUs tracked --
 * one entry per physical core, not per logical CPU, so this is generous
 * even for large multi-socket hosts (256 physical cores). */
#define POWER_MAX_CORE_CPUS 256

/* setup_counters() (topdown.c) special-cases this device_type value to skip
 * perf_event_open()/coverage_note() entirely for a power_core counter on a
 * CPU that isn't one of power_core's own cpumask-listed representative
 * CPUs -- see this file's top comment. Distinct from the pre-existing
 * (unnamed, left as-is) 9999 hwmon-fallback marker setup_counters() already
 * checks at the same point. */
#define POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE 9998

struct power_format_field {
  char name[32];
  char location[32]; /* e.g. "config:0-7", verbatim from sysfs */
};

struct power_pmu {
  int present;              /* 1 if this PMU's sysfs directory exists at all */
  int type;                 /* perf_event_open() attr.type value; -1 if unknown */
  struct power_format_field format[POWER_MAX_FORMAT_FIELDS];
  int format_count;
  int event_present;        /* events/<event_name> file exists and parsed */
  unsigned long event;      /* parsed "event=0x.." value from events/<event_name> */
  double scale;             /* Joules-per-LSB, from events/<event_name>.scale; 1.0 if absent */
  char unit[32];            /* from events/<event_name>.unit, e.g. "Joules"; empty if absent */
};

struct power_capabilities {
  int supported;      /* pkg.present && pkg.event_present -- power_core's
                        * own per-core support is tracked separately below */
  struct power_pmu pkg;   /* power/energy-pkg */
  struct power_pmu core;  /* power_core/energy-core */
  /* power_core's own cpumask (comma/range list, e.g. "0,2,4-6"), parsed at
   * probe time -- the representative logical CPUs power_core can actually
   * be read from, one per physical core. Empty (ncore_cpus == 0) when
   * power_core itself isn't present. */
  int core_cpus[POWER_MAX_CORE_CPUS];
  int ncore_cpus;
  int is_fallback;
  char fallback_path[256];
};

/* 1 if cpu is one of power_core's own cpumask-listed representative CPUs
 * (a real power_core event can be opened there), 0 otherwise (an SMT
 * sibling or any other non-representative CPU). */
int power_core_cpu_is_representative(const struct power_capabilities *caps,int cpu);

extern char fallback_power_path[256];

/* Probes /sys/bus/event_source/devices/{power,power_core} and returns the
 * discovered capabilities. Never fails -- an absent PMU (no RAPL-equivalent
 * support, old kernel) just yields present == 0 / supported == 0 rather
 * than an error. */
struct power_capabilities power_probe(void);

/* access is 1/0/-1 (opened fine / failed / nothing to test) and
 * access_errno only meaningful when access==0 -- see power.c's own comment
 * on why this is computed by the caller (a real setup_counters() attempt
 * via a throwaway power_counter_group(), wspy.c's run_capabilities_probe())
 * rather than in here. */
void print_power_capability_report(const struct power_capabilities *power,int access,int access_errno);

/* One PMU event ready for perf_event_open(): resolved dynamic type plus the
 * assembled config word. valid==0 means the PMU/event isn't present
 * (nothing to open). Mirrors ibs.h's struct ibs_event/ibs_build_*_event()
 * shape -- built from an already-probed struct power_pmu so it's
 * independently testable against a fake sysfs fixture (see test_power.c),
 * without needing power_counter_group()'s own real-host power_probe(). */
struct power_event {
  int valid;
  int type;
  unsigned long config;
};
struct power_event power_build_pkg_event(const struct power_pmu *pkg);
/* Same shape as power_build_pkg_event(), built from a power_core struct
 * power_pmu (caps.core) instead of the package one. */
struct power_event power_build_core_event(const struct power_pmu *core);

/* struct counter_group is defined in cpu_info.h, which this header
 * deliberately doesn't include (see the top-of-file comment) -- forward
 * declared here since this function only needs the pointer type. */
struct counter_group;
/* Builds the counter_group for --power (COUNTER_POWER): one counting event,
 * power/energy-pkg, with struct counter_info.scale set from the probed
 * .scale so read_counters() converts the raw LSB delta to Joules directly
 * (see topdown.c's entry in CLAUDE.md). Probes support itself; returns NULL
 * with a warning if power/energy-pkg isn't present on this host/kernel,
 * matching how ibs_counter_group() returns NULL for unsupported IBS. */
struct counter_group *power_counter_group(char *name);

/* Builds one power_core (COUNTER_POWER_CORE) counter_group for --power
 * --per-core: one counting event, "core_joules", bound to target_cpu
 * (cgroup->target_cpu, picked up by setup_counters()'s existing
 * aflag+target_cpu>=0 per-CPU-system-wide-open path with no changes
 * there). caps must be an already-probed struct power_capabilities (the
 * caller already has one from deciding whether --power is even usable at
 * all -- this doesn't re-probe). When target_cpu is one of caps's own
 * power_core cpumask CPUs, the counter is real (device_type/config/scale
 * from power_build_core_event()); otherwise it's marked
 * POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE so setup_counters() skips it
 * entirely -- see this file's top comment. Returns NULL (with a warning)
 * only if power_core isn't present/usable on this host/kernel at all --
 * the caller is expected to only call this when caps.core.present &&
 * caps.core.event_present, so this is a defensive fallback, not the
 * common path. */
struct counter_group *power_core_counter_group(char *name,const struct power_capabilities *caps,
                                                int target_cpu);

#endif
