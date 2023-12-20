// definition of counter parameters
struct counterdef {
  char *name;
  unsigned int event;
  unsigned int umask;
  unsigned int cmask;
  unsigned int any;
  unsigned int scale;
  unsigned int use;
};

// in memory information kept with each counter
struct counter_info {
  int corenum;
  struct counterdef *cdef;
  int fd;
  unsigned long int value;
};
  
enum cpu_core_type {
  CORE_UNKNOWN,
  CORE_AMD_UNKNOWN, CORE_AMD_ZEN,
  CORE_INTEL_UNKNOWN, CORE_INTEL_ATOM, CORE_INTEL_CORE
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
  int ncounters;
  struct counter_info *counters;
};

enum cpu_vendor { VENDOR_UNKNOWN=0, VENDOR_AMD=1, VENDOR_INTEL=2 };

struct cpu_info {
  enum cpu_vendor vendor;
  unsigned int family;
  unsigned int model;
  unsigned int num_cores;
  unsigned int num_cores_available;
  struct cpu_core_info *coreinfo;
};

extern struct cpu_info *cpu_info;
extern int inventory_cpu(void);
