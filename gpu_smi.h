/*
 * gpu_info.h - query for AMD GPUs
 */

#if AMDGPU
struct gpu_smi_data {
  int temperature;
  int gfx_activity;
  int umc_activity;
  int mm_activity;
};

void gpu_smi_initialize();
int gpu_smi_available();
void gpu_smi_query(struct gpu_smi_data *qd);
void gpu_smi_finalize();
#endif

