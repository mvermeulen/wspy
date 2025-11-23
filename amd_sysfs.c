#if AMDGPU
#include "amd_sysfs.h"
#include "error.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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
		error("AMD SYSFS gpu_metrics not found\n");
	}
}

void amd_sysfs_finalize(void)
{
}

int amd_sysfs_gpu_busy_percent(void)
{
	FILE *fp;
	int value = 0;
	
	fp = fopen("/sys/class/drm/card1/device/gpu_busy_percent", "r");
	if (fp) {
		if (fscanf(fp, "%d", &value) == 1) {
			debug("GPU busy percent: %d\n", value);
		}
		fclose(fp);
	}
	return value;
}

void amd_sysfs_gpu_metrics(void)
{
	FILE *fp;
	struct amd_sysfs_metrics_header header;
	struct amd_sysfs_gpu_metrics_v1_0 metrics_v1;
	struct amd_sysfs_gpu_metrics_v2_0 metrics_v2;
	struct amd_sysfs_gpu_metrics_v3_0 metrics_v3;
	size_t bytes_read;
	
	fp = fopen("/sys/class/drm/card1/device/gpu_metrics", "rb");
	if (!fp) {
		return;
	}
	
	bytes_read = fread(&header, 1, sizeof(header), fp);
	if (bytes_read < sizeof(header)) {
		fclose(fp);
		return;
	}
	
	debug("GPU metrics header:\n");
	debug("  Structure size: %u\n", header.structure_size);
	debug("  Format revision: %u\n", header.format_revision);
	debug("  Content revision: %u\n", header.content_revision);
	
	rewind(fp);
	
	if (header.format_revision == 1) {
		memset(&metrics_v1, 0, sizeof(metrics_v1));
		bytes_read = fread(&metrics_v1, 1, sizeof(metrics_v1), fp);
		debug("GPU metrics v1.0: read %zu bytes\n", bytes_read);
		debug("  Temperature edge: %u C\n", metrics_v1.temperature_edge);
		debug("  Temperature hotspot: %u C\n", metrics_v1.temperature_hotspot);
		debug("  Average GFX activity: %u%%\n", metrics_v1.average_gfx_activity);
		debug("  Average socket power: %u W\n", metrics_v1.average_socket_power);
	} else if (header.format_revision == 2) {
		struct amd_sysfs_gpu_metrics_v2_0 metrics_v2;
		memset(&metrics_v2, 0, sizeof(metrics_v2));
		bytes_read = fread(&metrics_v2, 1, sizeof(metrics_v2), fp);
		debug("GPU metrics v2.0: read %zu bytes\n", bytes_read);
		debug("  Temperature GFX: %.1f C\n", metrics_v2.temperature_gfx / 100.0);
		debug("  Temperature SOC: %.1f C\n", metrics_v2.temperature_soc / 100.0);
		debug("  Average GFX activity: %u%%\n", metrics_v2.average_gfx_activity);
		debug("  Average MM activity: %u%%\n", metrics_v2.average_mm_activity);
		debug("  Average socket power: %.2f W\n", metrics_v2.average_socket_power / 1000.0);
		debug("  Average CPU power: %.2f W\n", metrics_v2.average_cpu_power / 1000.0);
		debug("  Average SOC power: %.2f W\n", metrics_v2.average_soc_power / 1000.0);
		debug("  Average GFX power: %.2f W\n", metrics_v2.average_gfx_power / 1000.0);
		debug("  Average GFXCLK frequency: %u MHz\n", metrics_v2.average_gfxclk_frequency);
		debug("  Average SOCCLK frequency: %u MHz\n", metrics_v2.average_socclk_frequency);
		debug("  Average FCLK frequency: %u MHz\n", metrics_v2.average_fclk_frequency);
		debug("  Average UCLK frequency: %u MHz\n", metrics_v2.average_uclk_frequency);
		debug("  Current GFXCLK: %u MHz\n", metrics_v2.current_gfxclk);
		debug("  Throttle status: 0x%08x\n", metrics_v2.throttle_status);
		debug("  Fan PWM: %u\n", metrics_v2.fan_pwm);
	} else if (header.format_revision == 3) {
		memset(&metrics_v3, 0, sizeof(metrics_v3));
		bytes_read = fread(&metrics_v3, 1, sizeof(metrics_v3), fp);
		debug("GPU metrics v3.0: read %zu bytes\n", bytes_read);
		debug("  Temperature GFX: %.1f C\n", metrics_v3.temperature_gfx / 100.0);
		debug("  Temperature SOC: %.1f C\n", metrics_v3.temperature_soc / 100.0);
		debug("  Temperature skin: %.1f C\n", metrics_v3.temperature_skin / 100.0);
		debug("  Average GFX activity: %u%%\n", metrics_v3.average_gfx_activity);
		debug("  Average VCN activity: %u%%\n", metrics_v3.average_vcn_activity);
		debug("  Average socket power: %.2f W\n", metrics_v3.average_socket_power / 1000.0);
		debug("  Average APU power: %.2f W\n", metrics_v3.average_apu_power / 1000.0);
		debug("  Average GFX power: %.2f W\n", metrics_v3.average_gfx_power / 1000.0);
		debug("  Average GFXCLK frequency: %u MHz\n", metrics_v3.average_gfxclk_frequency);
		debug("  Average SOCCLK frequency: %u MHz\n", metrics_v3.average_socclk_frequency);
		debug("  Average FCLK frequency: %u MHz\n", metrics_v3.average_fclk_frequency);
		debug("  Average UCLK frequency: %u MHz\n", metrics_v3.average_uclk_frequency);
		debug("  Current GFX max freq: %u MHz\n", metrics_v3.current_gfx_maxfreq);
	} else {
		debug("Unknown GPU metrics format revision: %u\n", header.format_revision);
	}
	
	fclose(fp);
}

#ifdef TEST_AMD_SYSFS
int main(void)
{
	set_error_level(ERROR_LEVEL_DEBUG);
	amd_sysfs_initialize();
	amd_sysfs_gpu_busy_percent();
	amd_sysfs_gpu_metrics();
	amd_sysfs_finalize();
	return 0;
}
#endif
#endif
