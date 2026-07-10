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

/* ---- ibs-basic / ibs-memory-deep collection profiles ----
 * Thin layer on top of the capability probe above and the profile-launcher
 * work (wspy-run): assembles real perf_event_attr config words for
 * ibs_fetch/ibs_op from the format-field locations ibs_probe() already
 * discovered (e.g. "config1:0-11"), rather than hardcoded bit offsets, and
 * opens them as ordinary counting events -- the same perf_event_open()+
 * read() pattern every other counter group in this tree uses (IBS supports
 * "perf stat"-style counting, not just sampling/mmap capture).
 *
 * "ibs-basic" opens ibs_fetch+ibs_op unfiltered. "ibs-memory-deep" adds
 * l3missonly+ldlat filtering to ibs_op (and l3missonly+fetchlat to
 * ibs_fetch where the kernel/CPU exposes those fields), which is documented
 * to skew the effective sampling period/rate -- see INVESTIGATION_4.0.md's
 * "Zen5/IBS deep-dive". That skew must be visible in output rather than
 * just in code comments, so ibs-memory-deep also opens a second, unfiltered
 * ibs_op counter purely as a baseline: the filtered/unfiltered ratio is the
 * "accepted-vs-filtered" quality annotation, and struct ibs_event below
 * records which filters were actually applied (vs. merely requested, for
 * kernels/CPUs that don't expose a given format field) so callers can warn
 * when a requested filter silently had no effect. */
enum ibs_profile {
  IBS_PROFILE_NONE = 0,
  IBS_PROFILE_BASIC,
  IBS_PROFILE_MEMORY_DEEP,
};

/* User-overridable knobs (--ibs-maxcnt/--ibs-ldlat/--ibs-fetchlat); 0 means
 * "use the profile's built-in default" below. */
struct ibs_profile_params {
  unsigned int maxcnt;
  unsigned int ldlat_threshold;
  unsigned int fetchlat_threshold;
};

/* wspy's own starting-point defaults when a param above is left at 0 -- not
 * hardware-mandated values, just this tool's default, override via the CLI. */
#define IBS_DEFAULT_MAXCNT             0x1000
#define IBS_DEFAULT_LDLAT_THRESHOLD    120
#define IBS_DEFAULT_FETCHLAT_THRESHOLD 120

extern enum ibs_profile ibs_collection_profile;
extern struct ibs_profile_params ibs_params;

/* One PMU event ready for perf_event_open(): resolved dynamic type plus the
 * assembled config/config1/config2 words. valid==0 means the PMU isn't
 * present (nothing to open). The requested/applied field pairs below are
 * the sampling-skew/quality annotation data: requested is set whenever the
 * profile calls for that filter, applied only if the running kernel/CPU
 * actually exposed the format field so it could be encoded into config. */
struct ibs_event {
  int valid;
  int type;
  unsigned long config,config1,config2;
  int l3missonly_requested,l3missonly_applied;
  int ldlat_requested,ldlat_applied;
  unsigned int ldlat_threshold;
  int fetchlat_requested,fetchlat_applied;
  unsigned int fetchlat_threshold;
};

struct ibs_event ibs_build_fetch_event(const struct ibs_pmu *fetch,enum ibs_profile profile,const struct ibs_profile_params *params);
struct ibs_event ibs_build_op_event(const struct ibs_pmu *op,enum ibs_profile profile,const struct ibs_profile_params *params);
/* Unfiltered ibs_op baseline opened alongside the (possibly filtered) event
 * from ibs_build_op_event() when profile == IBS_PROFILE_MEMORY_DEEP, so the
 * filtered/unfiltered count ratio can be reported as the accepted-vs-filtered
 * annotation. */
struct ibs_event ibs_build_op_unfiltered_event(const struct ibs_pmu *op,const struct ibs_profile_params *params);

/* struct counter_group is defined in cpu_info.h, which this header
 * deliberately doesn't include (see the top-of-file comment) -- forward
 * declared here since this function only needs the pointer type. */
struct counter_group;
/* Builds the counter_group for the given profile (IBS_PROFILE_NONE returns
 * NULL). Probes IBS support itself; returns NULL with a warning if IBS
 * isn't supported on this host/kernel, matching how raw_counter_group() etc.
 * return NULL for a mask with nothing available. */
struct counter_group *ibs_counter_group(char *name,enum ibs_profile profile,const struct ibs_profile_params *params);

#endif
