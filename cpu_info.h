enum cpu_core_type {
  CORE_UNKNOWN,
  CORE_AMD_UNKNOWN, CORE_AMD_ZEN,
  CORE_INTEL_UNKNOWN, CORE_INTEL_ATOM, CORE_INTEL_CORE
};

struct cpu_core_info {
  enum cpu_core_type vendor;
  unsigned int is_available: 1;
  unsigned int is_counter_started: 1;
};

enum cpu_vendor { VENDOR_UNKNOWN=0, VENDOR_AMD=1, VENDOR_INTEL=2 };

struct cpu_info {
  enum cpu_vendor vendor;
  unsigned int family;
  unsigned int model;
  unsigned int num_cores;
  struct cpu_core_info *coreinfo;
};

extern struct cpu_info *cpu_info;
