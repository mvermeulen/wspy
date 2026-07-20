#ifndef GPU_FUSION_H
#define GPU_FUSION_H

#include <stdint.h>

/* Which backend actually supplied a fused metric this tick -- the
 * per-metric "validity" signal for the two metrics both amd_smi.c and
 * amd_sysfs.c can independently report (temp, activity). Power/freq/VRAM
 * each have exactly one possible source today (sysfs for power/freq, SMI
 * for VRAM), so their validity is the usual zero-means-unmeasured
 * convention this codebase already uses elsewhere (see e.g. power.c's
 * print_power()) -- no separate source flag needed for those. */
enum gpu_metric_source {
  GPU_SOURCE_NONE = 0,
  GPU_SOURCE_SYSFS,
  GPU_SOURCE_SMI,
};

/* Raw per-backend readings for one tick, already fetched by the caller.
 * Kept as a plain struct of primitive fields rather than including
 * amd_smi.h/amd_sysfs.h here so gpu_fusion_combine() itself has no
 * ROCm-header dependency and can be exercised by test_gpu_fusion.c without
 * an AMDGPU=1 build -- mirroring power.c's/ibs.c's own split between pure,
 * unit-testable logic and hardware-dependent glue (see multipass.c's
 * "pure, unit-testable half" precedent). */
struct gpu_fusion_source_data {
  int sysfs_metrics_valid;
  int sysfs_temp_c;
  uint16_t sysfs_activity_pct;
  float sysfs_power_w;
  uint16_t sysfs_freq_mhz;

  int smi_metrics_valid;
  uint16_t smi_temp_c;
  uint16_t smi_activity_pct;

  int smi_memory_valid;
  uint32_t smi_vram_used_mb;
  uint32_t smi_vram_total_mb;
};

/* Applies source precedence and refreshes the fused metrics below: sysfs
 * supplies temp/activity/power/freq whenever its gpu_metrics blob was
 * readable this tick (it's the only source for power/freq at all, and the
 * actively-used path -- amd_smi.c is documented "legacy" in wspy.c); SMI
 * fills in temp/activity only when sysfs's own reading wasn't valid, and
 * is the only source for VRAM either way. */
void gpu_fusion_combine(const struct gpu_fusion_source_data *src);

int gpu_fusion_temp_valid(void);
int gpu_fusion_get_temp(void);
enum gpu_metric_source gpu_fusion_temp_source(void);

int gpu_fusion_activity_valid(void);
uint16_t gpu_fusion_get_activity(void);
enum gpu_metric_source gpu_fusion_activity_source(void);

int gpu_fusion_power_valid(void);
float gpu_fusion_get_power(void);

int gpu_fusion_freq_valid(void);
uint16_t gpu_fusion_get_freq(void);

int gpu_fusion_vram_valid(void);
uint32_t gpu_fusion_get_vram_used(void);
uint32_t gpu_fusion_get_vram_total(void);

const char *gpu_metric_source_name(enum gpu_metric_source src);

#if AMDGPU
/* Reads amd_sysfs.c/amd_smi.c (both must already be initialized by the
 * caller) and refreshes the fused metrics above for this tick. */
void gpu_fusion_update(void);
#endif

#endif /* GPU_FUSION_H */
