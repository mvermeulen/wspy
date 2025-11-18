#if AMDGPU
/*
 * amd_smi.c - thin wrapper or stub for ROCm/AMDSMI functions if needed
 * This file intentionally left mostly empty; including it ensures a
 * single place to add AMD SMI platform-specific helpers later.
 */

#include "amd_smi/amdsmi.h"

/* Add any platform compatibility / wrapper helpers for amdsmi here. */

#if TEST_AMD_SMI
int main(void){
	/* empty test harness for AMD SMI helpers */
	return 0;
}
#endif
#endif
