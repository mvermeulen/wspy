#ifndef NVIDIA_NVML_H
#define NVIDIA_NVML_H

#if NVIDIA
#include <stdint.h>
#include <stdio.h>

/* One NVIDIA device discovered via NVML, e.g. index 0 -> "NVIDIA GeForce ...". */
struct nvidia_nvml_device {
	int index;
	char name[96];
};
#define NVIDIA_NVML_MAX_DEVICES 16

void nvidia_nvml_initialize(int device_index); /* device_index < 0 => auto-select device 0 */
void nvidia_nvml_metrics(void);
void nvidia_nvml_finalize(void);

int nvidia_nvml_metrics_valid(void);
uint32_t nvidia_nvml_get_busy(void);
uint64_t nvidia_nvml_get_vram_used_mb(void);
uint64_t nvidia_nvml_get_vram_total_mb(void);

uint32_t nvidia_nvml_device_count(void);
int nvidia_nvml_selected_device(void); /* -1 if none selected */
void nvidia_nvml_print_capability_report(FILE *out);

#endif /* NVIDIA */

#endif /* NVIDIA_NVML_H */
