/*
 * gpu_info.h - query for AMD GPUs
 */

#if AMDGPU
struct gpu_query_data {
  int temperature;
  int gfx_activity;
  int umc_activity;
  int mm_activity;
};

void gpu_info_initialize();
void gpu_info_query(struct gpu_query_data *qd);
void gpu_info_finalize();
#endif

