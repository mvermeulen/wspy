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
  unsigned long int config;
  struct counter_def *cdef;
  int fd;
  unsigned long int value;
  unsigned long int time_running;
  unsigned long int time_enabled;
};

// definition of counter parameters
// TODO: Remove once no longer referenced
struct counter_def {
  char *name;
  unsigned int long event;
  unsigned int long umask;
  unsigned int long cmask;
  unsigned int long any;
  unsigned int long scale;
  unsigned int use;
};

// TODO: Remove once no longer referenced
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

