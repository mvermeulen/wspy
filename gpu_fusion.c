#include "gpu_fusion.h"
#include <string.h>

static int temp_valid = 0;
static int temp_c = 0;
static enum gpu_metric_source temp_source = GPU_SOURCE_NONE;

static int activity_valid = 0;
static uint16_t activity_pct = 0;
static enum gpu_metric_source activity_source = GPU_SOURCE_NONE;

static int power_valid = 0;
static float power_w = 0.0f;

static int freq_valid = 0;
static uint16_t freq_mhz = 0;

static int vram_valid = 0;
static uint32_t vram_used_mb = 0;
static uint32_t vram_total_mb = 0;

void gpu_fusion_combine(const struct gpu_fusion_source_data *src){
  temp_valid = 0; temp_source = GPU_SOURCE_NONE;
  activity_valid = 0; activity_source = GPU_SOURCE_NONE;
  power_valid = 0;
  freq_valid = 0;
  vram_valid = 0;

  if (src->sysfs_metrics_valid){
    temp_c = src->sysfs_temp_c;
    temp_valid = 1;
    temp_source = GPU_SOURCE_SYSFS;

    activity_pct = src->sysfs_activity_pct;
    activity_valid = 1;
    activity_source = GPU_SOURCE_SYSFS;

    power_w = src->sysfs_power_w;
    power_valid = 1;

    freq_mhz = src->sysfs_freq_mhz;
    freq_valid = 1;
  }

  if (src->smi_metrics_valid){
    if (!temp_valid){
      temp_c = src->smi_temp_c;
      temp_valid = 1;
      temp_source = GPU_SOURCE_SMI;
    }
    if (!activity_valid){
      activity_pct = src->smi_activity_pct;
      activity_valid = 1;
      activity_source = GPU_SOURCE_SMI;
    }
  }

  if (src->smi_memory_valid){
    vram_used_mb = src->smi_vram_used_mb;
    vram_total_mb = src->smi_vram_total_mb;
    vram_valid = 1;
  }
}

int gpu_fusion_temp_valid(void){ return temp_valid; }
int gpu_fusion_get_temp(void){ return temp_c; }
enum gpu_metric_source gpu_fusion_temp_source(void){ return temp_source; }

int gpu_fusion_activity_valid(void){ return activity_valid; }
uint16_t gpu_fusion_get_activity(void){ return activity_pct; }
enum gpu_metric_source gpu_fusion_activity_source(void){ return activity_source; }

int gpu_fusion_power_valid(void){ return power_valid; }
float gpu_fusion_get_power(void){ return power_w; }

int gpu_fusion_freq_valid(void){ return freq_valid; }
uint16_t gpu_fusion_get_freq(void){ return freq_mhz; }

int gpu_fusion_vram_valid(void){ return vram_valid; }
uint32_t gpu_fusion_get_vram_used(void){ return vram_used_mb; }
uint32_t gpu_fusion_get_vram_total(void){ return vram_total_mb; }

const char *gpu_metric_source_name(enum gpu_metric_source src){
  switch (src){
    case GPU_SOURCE_SYSFS: return "sysfs";
    case GPU_SOURCE_SMI: return "smi";
    default: return "none";
  }
}

#if AMDGPU
#include "amd_smi.h"
#include "amd_sysfs.h"

void gpu_fusion_update(void){
  struct gpu_fusion_source_data src;
  memset(&src,0,sizeof(src));

  amd_sysfs_gpu_metrics();
  src.sysfs_metrics_valid = amd_sysfs_gpu_metrics_valid();
  if (src.sysfs_metrics_valid){
    src.sysfs_temp_c = amd_sysfs_get_gpu_temp();
    src.sysfs_activity_pct = amd_sysfs_get_gpu_activity();
    src.sysfs_power_w = amd_sysfs_get_gpu_power();
    src.sysfs_freq_mhz = amd_sysfs_get_gpu_freq();
  }

  amd_smi_metrics();
  src.smi_metrics_valid = amd_smi_metrics_valid();
  if (src.smi_metrics_valid){
    src.smi_temp_c = amd_smi_get_temp();
    src.smi_activity_pct = amd_smi_get_activity();
  }

  amd_smi_memory();
  src.smi_memory_valid = amd_smi_memory_valid();
  if (src.smi_memory_valid){
    src.smi_vram_used_mb = amd_smi_get_vram_used();
    src.smi_vram_total_mb = amd_smi_get_vram_total();
  }

  gpu_fusion_combine(&src);
}
#endif
