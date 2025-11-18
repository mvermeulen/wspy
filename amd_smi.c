#if AMDGPU

#include "amd_smi/amdsmi.h"

void amd_smi_initialize(void)
{
	/* Placeholder for AMD SMI initialization helpers */
}

void amd_smi_finalize(void)
{
	/* Placeholder for AMD SMI finalization helpers */
}

#if TEST_AMD_SMI
int main(void){
    
	amd_smi_initialize();
	amd_smi_finalize();
	return 0;
}
#endif
#endif
