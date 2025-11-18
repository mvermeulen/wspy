#if AMDGPU

#include "amd_smi/amdsmi.h"
#include "error.h"

void amd_smi_initialize(void)
{
	amdsmi_status_t ret;
	
	ret = amdsmi_init(AMDSMI_INIT_AMD_GPUS);
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_init failed with status %d", ret);
	}
}

void amd_smi_finalize(void)
{
	amdsmi_status_t ret;
	
	ret = amdsmi_shut_down();
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_shut_down failed with status %d", ret);
	}
}

#if TEST_AMD_SMI
int main(void){

	amd_smi_initialize();
	amd_smi_finalize();
	return 0;
}
#endif
#endif
