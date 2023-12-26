#include <linux/perf_event.h>

// cpu_info holds the root of information kept about a processor
extern struct cpu_info *cpu_info;
extern int inventory_cpu(void);

// CPU information including performance counters
enum cpu_vendor { VENDOR_UNKNOWN=0, VENDOR_AMD=1, VENDOR_INTEL=2 };

// Overall CPU
struct cpu_info {
  enum cpu_vendor vendor;
  unsigned int family;
  unsigned int model;
  unsigned int num_cores;
  unsigned int num_cores_available;
  unsigned int is_hybrid:1;                  // Intel hybrid CPU with mixed cores
  struct cpu_core_info *coreinfo;
  struct counter_group *systemwide_counters; // In memory information for cores
};

// groups of counters launched together
struct counter_group {
  char *label;
  enum perf_type_id type_id;
  int ncounters;
  unsigned int mask;
  struct counter_info *cinfo;
  struct counter_group *next;
};

// in memory information kept with each counter
// cpu core information
enum cpu_core_type {
  CORE_UNKNOWN,
  CORE_AMD_UNKNOWN, CORE_AMD_ZEN,
  CORE_INTEL_UNKNOWN, CORE_INTEL_ATOM, CORE_INTEL_CORE
};

struct counter_info {
  char *label;
  int corenum; // for hw counters
  unsigned int is_group_leader : 1;
  unsigned long int config;
  struct counter_def *cdef;
  int fd;
  unsigned long int value;
  unsigned long int time_running;
  unsigned long int time_enabled;
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

struct raw_event {
  char *name;
  char *description;
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
