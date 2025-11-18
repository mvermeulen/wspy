#ifndef AMD_SMI_H
#define AMD_SMI_H

#if AMDGPU
#include "amd_smi/amdsmi.h"

void amd_smi_initialize(void);
void amd_smi_finalize(void);

#endif /* AMDGPU */

#endif /* AMD_SMI_H
*/