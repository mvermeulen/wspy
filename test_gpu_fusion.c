#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "gpu_fusion.c"

/* Neither backend has anything -- every metric stays invalid, source none. */
static void test_gpu_fusion_neither_backend(void){
  struct gpu_fusion_source_data src;
  memset(&src,0,sizeof(src));

  gpu_fusion_combine(&src);

  assert(!gpu_fusion_temp_valid());
  assert(gpu_fusion_temp_source() == GPU_SOURCE_NONE);
  assert(!gpu_fusion_activity_valid());
  assert(gpu_fusion_activity_source() == GPU_SOURCE_NONE);
  assert(!gpu_fusion_power_valid());
  assert(!gpu_fusion_freq_valid());
  assert(!gpu_fusion_vram_valid());

  printf("PASS: gpu_fusion_neither_backend\n");
}

/* Only sysfs's gpu_metrics blob was readable: temp/activity/power/freq all
 * come from sysfs; VRAM stays invalid since only SMI reads it. */
static void test_gpu_fusion_sysfs_only(void){
  struct gpu_fusion_source_data src;
  memset(&src,0,sizeof(src));
  src.sysfs_metrics_valid = 1;
  src.sysfs_temp_c = 45;
  src.sysfs_activity_pct = 62;
  src.sysfs_power_w = 87.5f;
  src.sysfs_freq_mhz = 1800;

  gpu_fusion_combine(&src);

  assert(gpu_fusion_temp_valid());
  assert(gpu_fusion_get_temp() == 45);
  assert(gpu_fusion_temp_source() == GPU_SOURCE_SYSFS);
  assert(gpu_fusion_activity_valid());
  assert(gpu_fusion_get_activity() == 62);
  assert(gpu_fusion_activity_source() == GPU_SOURCE_SYSFS);
  assert(gpu_fusion_power_valid());
  assert(gpu_fusion_get_power() == 87.5f);
  assert(gpu_fusion_freq_valid());
  assert(gpu_fusion_get_freq() == 1800);
  assert(!gpu_fusion_vram_valid());

  printf("PASS: gpu_fusion_sysfs_only\n");
}

/* Only SMI has data (sysfs's gpu_metrics blob unreadable/unsupported
 * format_revision): temp/activity fall back to SMI, VRAM comes from SMI,
 * power/freq stay invalid since SMI never reads either. */
static void test_gpu_fusion_smi_only(void){
  struct gpu_fusion_source_data src;
  memset(&src,0,sizeof(src));
  src.smi_metrics_valid = 1;
  src.smi_temp_c = 50;
  src.smi_activity_pct = 70;
  src.smi_memory_valid = 1;
  src.smi_vram_used_mb = 1024;
  src.smi_vram_total_mb = 8192;

  gpu_fusion_combine(&src);

  assert(gpu_fusion_temp_valid());
  assert(gpu_fusion_get_temp() == 50);
  assert(gpu_fusion_temp_source() == GPU_SOURCE_SMI);
  assert(gpu_fusion_activity_valid());
  assert(gpu_fusion_get_activity() == 70);
  assert(gpu_fusion_activity_source() == GPU_SOURCE_SMI);
  assert(!gpu_fusion_power_valid());
  assert(!gpu_fusion_freq_valid());
  assert(gpu_fusion_vram_valid());
  assert(gpu_fusion_get_vram_used() == 1024);
  assert(gpu_fusion_get_vram_total() == 8192);

  printf("PASS: gpu_fusion_smi_only\n");
}

/* Both backends have data: sysfs wins temp/activity/power/freq (it's the
 * documented-preferred, actively-used path), SMI still supplies VRAM since
 * sysfs never reads it at all. */
static void test_gpu_fusion_both_backends_sysfs_precedence(void){
  struct gpu_fusion_source_data src;
  memset(&src,0,sizeof(src));
  src.sysfs_metrics_valid = 1;
  src.sysfs_temp_c = 45;
  src.sysfs_activity_pct = 62;
  src.sysfs_power_w = 87.5f;
  src.sysfs_freq_mhz = 1800;
  src.smi_metrics_valid = 1;
  src.smi_temp_c = 999; /* would be wrong if this leaked through */
  src.smi_activity_pct = 999;
  src.smi_memory_valid = 1;
  src.smi_vram_used_mb = 2048;
  src.smi_vram_total_mb = 16384;

  gpu_fusion_combine(&src);

  assert(gpu_fusion_get_temp() == 45);
  assert(gpu_fusion_temp_source() == GPU_SOURCE_SYSFS);
  assert(gpu_fusion_get_activity() == 62);
  assert(gpu_fusion_activity_source() == GPU_SOURCE_SYSFS);
  assert(gpu_fusion_vram_valid());
  assert(gpu_fusion_get_vram_used() == 2048);
  assert(gpu_fusion_get_vram_total() == 16384);

  printf("PASS: gpu_fusion_both_backends_sysfs_precedence\n");
}

/* A later, all-invalid tick must clear out a previous tick's stale values
 * rather than leaving them stuck -- gpu_fusion_combine() resets every field
 * up front on every call. */
static void test_gpu_fusion_resets_between_ticks(void){
  struct gpu_fusion_source_data src;

  memset(&src,0,sizeof(src));
  src.sysfs_metrics_valid = 1;
  src.sysfs_temp_c = 45;
  src.sysfs_activity_pct = 62;
  src.sysfs_power_w = 87.5f;
  src.sysfs_freq_mhz = 1800;
  gpu_fusion_combine(&src);
  assert(gpu_fusion_temp_valid());

  memset(&src,0,sizeof(src));
  gpu_fusion_combine(&src);
  assert(!gpu_fusion_temp_valid());
  assert(gpu_fusion_temp_source() == GPU_SOURCE_NONE);
  assert(!gpu_fusion_activity_valid());
  assert(!gpu_fusion_power_valid());
  assert(!gpu_fusion_freq_valid());
  assert(!gpu_fusion_vram_valid());

  printf("PASS: gpu_fusion_resets_between_ticks\n");
}

static void test_gpu_metric_source_name(void){
  assert(strcmp(gpu_metric_source_name(GPU_SOURCE_NONE),"none") == 0);
  assert(strcmp(gpu_metric_source_name(GPU_SOURCE_SYSFS),"sysfs") == 0);
  assert(strcmp(gpu_metric_source_name(GPU_SOURCE_SMI),"smi") == 0);

  printf("PASS: gpu_metric_source_name\n");
}

int main(void){
  test_gpu_fusion_neither_backend();
  test_gpu_fusion_sysfs_only();
  test_gpu_fusion_smi_only();
  test_gpu_fusion_both_backends_sysfs_precedence();
  test_gpu_fusion_resets_between_ticks();
  test_gpu_metric_source_name();

  printf("\nAll test_gpu_fusion tests passed.\n");
  return 0;
}
