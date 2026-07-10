#ifndef AMD_SMI_H
#define AMD_SMI_H

#if AMDGPU
#include "amd_smi/amdsmi.h"
#include <stdio.h>

void amd_smi_initialize(int device_index); /* device_index < 0 => auto-select device 0 */
void amd_smi_metrics(void);
void amd_smi_memory(void);
void amd_smi_finalize(void);

int amd_smi_metrics_valid(void);
uint16_t amd_smi_get_temp(void);
uint16_t amd_smi_get_activity(void);
int amd_smi_memory_valid(void);
uint32_t amd_smi_get_vram_used(void);
uint32_t amd_smi_get_vram_total(void);

uint32_t amd_smi_device_count(void);
int amd_smi_selected_device(void); /* -1 if none selected */
void amd_smi_print_capability_report(FILE *out);

#endif /* AMDGPU */

#endif /* AMD_SMI_H
*/