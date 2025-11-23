#if AMDGPU
#include "amd_sysfs.h"
#include "error.h"
#include <unistd.h>

/* Flag indicating presence of gpu_busy_percent sysfs entry */
static int amd_sysfs_has_gpu_busy = 0;
static int amd_sysfs_has_gpu_metrics = 0;

 void amd_sysfs_initialize(void)
 {
	const char *path = "/sys/class/drm/card1/device/gpu_busy_percent";
	if (access(path, R_OK) == 0) {
		amd_sysfs_has_gpu_busy = 1;
	} else {
		error("AMD SYSFS gpu_busy_percent not found");
	}
	const char *path2 = "/sys/class/drm/card1/device/gpu_metrics";
	if (access(path2, R_OK) == 0) {
		amd_sysfs_has_gpu_metrics = 1;
	} else {
		error("AMD SYSFS gpu_metrics not found");
	}
}

void amd_sysfs_finalize(void)
{
}

#ifdef TEST_AMD_SYSFS
int main(void)
{
	amd_sysfs_initialize();
	amd_sysfs_finalize();
	return 0;
}
#endif
#endif
