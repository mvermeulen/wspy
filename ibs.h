/*
 * ibs.h - capability-driven discovery of AMD's Instruction-Based Sampling
 * (IBS) PMU support.
 *
 * IBS is exposed to userspace as two dynamic PMUs the kernel registers at
 * boot -- ibs_fetch and ibs_op -- present only when the running CPU/kernel
 * combination actually supports IBS. Unlike PERF_TYPE_HARDWARE, there is no
 * fixed perf_event "type" constant for them: each PMU's numeric type
 * (needed for perf_event_open's attr.type), its config-register bitfield
 * layout ("format", e.g. "config:59"), and which optional hardware
 * features it implements ("caps", e.g. l3missonly/ldlat/zen4_ibs_extensions)
 * are all read from /sys/bus/event_source/devices/{ibs_fetch,ibs_op}/* at
 * runtime instead of hardcoded, since both format fields and caps flags
 * vary across kernel versions and CPU generations (see
 * INVESTIGATION_4.0.md's "Zen5/IBS deep-dive": Zen5 caps differ from Zen4,
 * and future generations will add more).
 *
 * This is deliberately scoped to discovery only -- it doesn't build
 * perf_event_attr configs or open any IBS counters itself; that's the
 * follow-on "ibs-basic"/"ibs-memory-deep" collection-profile inventory
 * item, which this one is a named prerequisite for.
 */
#ifndef _WSPY_IBS_H
#define _WSPY_IBS_H 1

/* Uses FILE *outfile from wspy.h, like coverage.h -- see that header's own
 * comment for why wspy.h isn't included here directly. */

#define IBS_MAX_FORMAT_FIELDS 16
#define IBS_MAX_CAPS 16

struct ibs_format_field {
  char name[32];
  char location[32]; /* e.g. "config:59" or "config1:0-11", verbatim from sysfs */
};

struct ibs_cap {
  char name[32];
  int enabled;
};

struct ibs_pmu {
  int present; /* 1 if this PMU's sysfs directory exists at all */
  int type;    /* perf_event_open() attr.type value; -1 if unknown */
  struct ibs_format_field format[IBS_MAX_FORMAT_FIELDS];
  int format_count;
  struct ibs_cap caps[IBS_MAX_CAPS];
  int caps_count;
};

struct ibs_capabilities {
  int supported; /* 1 if both ibs_fetch and ibs_op PMUs are present */
  struct ibs_pmu fetch;
  struct ibs_pmu op;
};

/* Probes /sys/bus/event_source/devices/{ibs_fetch,ibs_op} and returns the
 * discovered capabilities. Never fails -- an absent PMU (non-AMD hardware,
 * IBS unsupported/disabled, or an old kernel) just yields present == 0 /
 * supported == 0 rather than an error. */
struct ibs_capabilities ibs_probe(void);

/* Looks up a named format field or cap within one PMU; returns NULL if not
 * present. Convenience for a future caller (a collection profile) that
 * needs to check e.g. "does this kernel/CPU support l3missonly" without
 * walking the arrays by hand. */
const struct ibs_format_field *ibs_pmu_format(const struct ibs_pmu *pmu,const char *name);
const struct ibs_cap *ibs_pmu_cap(const struct ibs_pmu *pmu,const char *name);

void print_ibs_capability_report(const struct ibs_capabilities *ibs);

#endif
