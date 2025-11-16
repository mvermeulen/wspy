/*
 * gpu_info.h - query for AMD GPUs
 */

#if AMDGPU
struct gpu_query_data {
  // Temperature
  int temperature;              // GPU edge temperature in °C
  
  // Activity/Utilization
  int gfx_activity;             // Graphics engine activity (%)
  int umc_activity;             // Memory controller activity (%)
  int mm_activity;              // Multimedia engine activity (%)
  
  // Memory
  unsigned int vram_total_mb;   // Total VRAM in MB
  unsigned int vram_used_mb;    // Used VRAM in MB
  unsigned int vram_free_mb;    // Free VRAM in MB
  
  // Clock frequencies
  unsigned int gfx_clock_mhz;   // Graphics clock in MHz
  unsigned int mem_clock_mhz;   // Memory clock in MHz
  unsigned int soc_clock_mhz;   // SOC clock in MHz
  
  // Power
  unsigned int power_watts;     // Current power consumption in W
  unsigned int power_limit_watts; // Power limit in W
};

void gpu_info_initialize();
void gpu_info_query(struct gpu_query_data *qd);
void gpu_info_finalize();
#endif

