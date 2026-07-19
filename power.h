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
 * V1 scope is package-level only: power_core (per-core energy) is probed
 * for --capabilities discovery but never opened as a real counter -- see
 * power_counter_group()'s comment and INVESTIGATION.md's "4.2 -- remaining
 * work", "Per-core energy (power_core) support" for the deferred per-core
 * work.
 */
#ifndef _WSPY_POWER_H
#define _WSPY_POWER_H 1

/* Uses FILE *outfile from wspy.h, like coverage.h/ibs.h -- see coverage.h's
 * own comment for why wspy.h isn't included here directly. */

#define POWER_MAX_FORMAT_FIELDS 4

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
  int supported;      /* pkg.present && pkg.event_present -- power_core is
                        * discovery-only in v1, doesn't gate this */
  struct power_pmu pkg;   /* power/energy-pkg */
  struct power_pmu core;  /* power_core/energy-core -- discovery only, see above */
};

/* Probes /sys/bus/event_source/devices/{power,power_core} and returns the
 * discovered capabilities. Never fails -- an absent PMU (no RAPL-equivalent
 * support, old kernel) just yields present == 0 / supported == 0 rather
 * than an error. */
struct power_capabilities power_probe(void);

void print_power_capability_report(const struct power_capabilities *power);

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

#endif
