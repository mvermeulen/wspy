#include <stdio.h>
#include <linux/perf_event.h>

// cpu_info holds the root of information kept about a processor
extern struct cpu_info *cpu_info;
extern int inventory_cpu(void);

// CPU information including performance counters
enum cpu_vendor { VENDOR_UNKNOWN=0, VENDOR_AMD=1, VENDOR_INTEL=2, VENDOR_ARM=3 };
const char *cpu_vendor_name(enum cpu_vendor vendor);

// Overall CPU
struct cpu_info {
  enum cpu_vendor vendor;
  unsigned int family;
  unsigned int model;
  unsigned int num_cores;
  unsigned int num_cores_available;
  unsigned int num_pmu_clusters;
  unsigned int mixed_pmu_types:1;          // available cores span >1 PMU type (big.LITTLE)
  unsigned int is_hybrid:1;                  // Intel hybrid CPU with mixed cores
  struct cpu_core_info *coreinfo;
  struct counter_group *systemwide_counters; // In memory information for cores
};

// groups of counters launched together
struct counter_group {
  char *label;
  enum perf_type_id type_id;
  int ncounters;
  int target_cpu;         // -1 for process-wide counting, >=0 for per-cpu counting
  unsigned int mask;
  struct counter_info *cinfo;
  struct counter_group *next;
};

// in memory information kept with each counter
// cpu core information
enum cpu_core_type {
  CORE_UNKNOWN,
  CORE_ARM_GENERIC,
  CORE_ARM_CORTEX_A53,
  CORE_ARM_CORTEX_A57,
  CORE_ARM_CORTEX_A72,
  CORE_ARM_NEOVERSE_N1,
  CORE_ARM_NEOVERSE_V1,
  CORE_ARM_NEOVERSE_N2,
  CORE_ARM_NEOVERSE_V2,
  CORE_ARM_CORTEX_A78,
  CORE_ARM_CORTEX_X1,
  CORE_ARM_CORTEX_A710,
  CORE_ARM_CORTEX_X2,
  CORE_ARM_CORTEX_A510,
  CORE_ARM_CORTEX_A520,
  CORE_ARM_CORTEX_A720,
  CORE_ARM_CORTEX_X4,
  CORE_AMD_UNKNOWN, CORE_AMD_ZEN, CORE_AMD_ZEN5, CORE_AMD_ZEN5C,
  CORE_INTEL_UNKNOWN, CORE_INTEL_ATOM, CORE_INTEL_CORE
};

struct counter_info {
  char *label;
  int corenum; // for hw counters
  unsigned int is_group_leader : 1;
  unsigned int device_type;
  unsigned long int config;
  unsigned long int config1; // extended config word (e.g. AMD IBS ldlat threshold); 0 for counters that don't use it
  unsigned long int config2;
  unsigned long int sample_period; // perf_event_attr.sample_period (e.g. AMD IBS MaxCnt); 0 for counters that don't use it
  struct counter_def *cdef;
  int fd;
  unsigned long int value;
  unsigned long int last_read;
  unsigned long int prev_read;
  unsigned long int time_running;   // this read's time_running *delta* (see read_counters())
  unsigned long int time_enabled;   // this read's time_enabled *delta* (see read_counters())
  unsigned long int last_time_running; // cumulative time_running as of the previous read
  unsigned long int last_time_enabled; // cumulative time_enabled as of the previous read
  double scale;         // raw-LSB-to-real-unit multiplier (e.g. Joules-per-LSB for power/
                         // energy-pkg, from that event's sysfs .scale file); 0.0 (the default
                         // for every counter that isn't power) means "no scaling"
  double scaled_value;  // this read's .value * scale, in the counter's own real unit (e.g.
                         // Joules) -- only meaningful when scale != 0.0; see read_counters()
};

// CPU counter tables
// raw_event_info is the descriptive form
// Format of perf counter events as described in reflects /sys/devices/cpu_core/format
union intel_raw_cpu_format {
  struct {
    unsigned long event:8; // 0-7
    unsigned long umask:8; // 8-15
    unsigned long :2;      // 16-17 pad
    unsigned long edge:1;  // 18
    unsigned long pc:1;    // 19
    unsigned long :3;      // 20-22 pad
    unsigned long inv:1;   // 23
    unsigned long cmask:8; // 24-31
    
  };
  struct {
    unsigned int frontend:24; // 0-23
  };
  struct {
    unsigned int ldlat:16; // 0-15
  };
  struct {
    unsigned long offcore_rsp:64; // 0-63
  };
  unsigned long config;
};

union amd_raw_cpu_format {
  struct {
    unsigned long event:8;  // 0-7 -- also event2 below for high order bits
    unsigned long umask:8;  // 8-15
    unsigned long :2;       // 16-17 pad
    unsigned long edge:1;   // 18
    unsigned long :4;       // 19-22 pad
    unsigned long inv:1;    // 23
    unsigned long cmask:8;  // 24-31
    unsigned long event2:4; // 32-35
  };
  unsigned long config;
};

#define PERF_TYPE_L3 0xe // extra device for amd_l3
struct raw_event {
  char *name;
  char *description;
  unsigned int device_type;
  unsigned int use;
  union {
    union intel_raw_cpu_format raw_intel;
    union amd_raw_cpu_format raw_amd;
    unsigned long config;
  } raw;
};

struct cache_event {
  enum perf_type_id type_id; // normally PERF_TYPE_HW_CACHE - but can also put in some PERF_TYPE_RAW events
  char *name;
  unsigned long config;
  unsigned int group_id;
  unsigned int use;
};

struct cpu_core_info {
  enum cpu_core_type vendor;
  unsigned int is_available: 1;
  unsigned int is_counter_started: 1;
  unsigned int pmu_type;      // perf PMU type id for this core (PERF_TYPE_RAW if unknown)
  int pmu_cluster;            // ARM PMU cluster index, -1 when not applicable
  unsigned long int td_instructions;
  unsigned long int td_cycles;
  unsigned long int td_slots;
  unsigned long int td_retire;
  unsigned long int td_frontend;
  unsigned long int td_backend;
  unsigned long int td_speculation;
  unsigned long int td_smt;
  struct counter_group *core_specific_counters;
  int ncounters;
  struct counter_info *counters;
};

static inline int is_arm_core_type(enum cpu_core_type vendor) {
  return (vendor == CORE_ARM_GENERIC ||
          vendor == CORE_ARM_CORTEX_A53 ||
          vendor == CORE_ARM_CORTEX_A57 ||
          vendor == CORE_ARM_CORTEX_A72 ||
          vendor == CORE_ARM_NEOVERSE_N1 ||
          vendor == CORE_ARM_NEOVERSE_V1 ||
          vendor == CORE_ARM_NEOVERSE_N2 ||
          vendor == CORE_ARM_NEOVERSE_V2 ||
          vendor == CORE_ARM_CORTEX_A78 ||
          vendor == CORE_ARM_CORTEX_X1 ||
          vendor == CORE_ARM_CORTEX_A710 ||
          vendor == CORE_ARM_CORTEX_X2 ||
          vendor == CORE_ARM_CORTEX_A510 ||
          vendor == CORE_ARM_CORTEX_A520 ||
          vendor == CORE_ARM_CORTEX_A720 ||
          vendor == CORE_ARM_CORTEX_X4);
}

void print_cpu_pmu_report(FILE *fp);
