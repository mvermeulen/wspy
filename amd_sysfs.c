#if AMDGPU
#include "amd_sysfs.h"
#include "error.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>

#define AMD_PCI_VENDOR_ID "0x1002"

/* Flag indicating presence of gpu_busy_percent sysfs entry */
static int amd_sysfs_has_gpu_busy = 0;
static int amd_sysfs_has_gpu_metrics = 0;

/* Resolved paths for the AMD GPU card selected by amd_sysfs_initialize() */
static char amd_sysfs_gpu_busy_path[300];
static char amd_sysfs_gpu_metrics_path[300];

/* Cached GPU metrics state */
static struct {
	float temp_gfx;
	uint16_t gfx_activity;
	float gfx_power;
	uint16_t gfxclk_freq;
	int valid;
} gpu_metrics_state = {0};

/* /sys/class/drm entries we care about are "card<N>" (no trailing connector
 * suffix like "card0-DP-1"), so filter on a plain numeric suffix. */
static int is_drm_card_name(const char *name)
{
	const char *p;

	if (strncmp(name, "card", 4) != 0 || name[4] == '\0') {
		return 0;
	}
	for (p = name + 4; *p; p++) {
		if (!isdigit((unsigned char)*p)) {
			return 0;
		}
	}
	return 1;
}

/* Scan /sys/class/drm/card<N>/device/vendor for the lowest-numbered AMD
 * (vendor 0x1002) card and copy its name (e.g. "card0") into card_name.
 * Returns 0 on success, -1 if no AMD card was found. */
static int find_amd_drm_card(char *card_name, size_t card_name_len)
{
	DIR *dir;
	struct dirent *entry;
	int best_index = -1;
	int match_count = 0;
	char best_name[256] = {0};

	dir = opendir("/sys/class/drm");
	if (!dir) {
		error("AMD SYSFS: unable to open /sys/class/drm");
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char vendor_path[300];
		char vendor[16] = {0};
		FILE *fp;
		int index;

		if (!is_drm_card_name(entry->d_name)) {
			continue;
		}

		snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/%s/device/vendor", entry->d_name);
		fp = fopen(vendor_path, "r");
		if (!fp) {
			continue;
		}
		if (fscanf(fp, "%15s", vendor) != 1) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		if (strcasecmp(vendor, AMD_PCI_VENDOR_ID) != 0) {
			continue;
		}

		match_count++;
		index = atoi(entry->d_name + 4);
		if (best_index == -1 || index < best_index) {
			best_index = index;
			snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
		}
	}
	closedir(dir);

	if (best_index == -1) {
		return -1;
	}
	if (match_count > 1) {
		debug("AMD SYSFS: found %d AMD GPUs under /sys/class/drm, using %s\n", match_count, best_name);
	}
	snprintf(card_name, card_name_len, "%s", best_name);
	return 0;
}

 void amd_sysfs_initialize(void)
 {
	char card_name[256];

	if (find_amd_drm_card(card_name, sizeof(card_name)) != 0) {
		error("AMD SYSFS: no AMD GPU (vendor %s) found under /sys/class/drm", AMD_PCI_VENDOR_ID);
		return;
	}
	debug("AMD SYSFS: using GPU device %s\n", card_name);

	snprintf(amd_sysfs_gpu_busy_path, sizeof(amd_sysfs_gpu_busy_path),
		 "/sys/class/drm/%s/device/gpu_busy_percent", card_name);
	snprintf(amd_sysfs_gpu_metrics_path, sizeof(amd_sysfs_gpu_metrics_path),
		 "/sys/class/drm/%s/device/gpu_metrics", card_name);

	if (access(amd_sysfs_gpu_busy_path, R_OK) == 0) {
		amd_sysfs_has_gpu_busy = 1;
	} else {
		error("AMD SYSFS gpu_busy_percent not found");
	}
	if (access(amd_sysfs_gpu_metrics_path, R_OK) == 0) {
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

	if (!amd_sysfs_has_gpu_busy) {
		return 0;
	}

	fp = fopen(amd_sysfs_gpu_busy_path, "r");
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

	if (!amd_sysfs_has_gpu_metrics) {
		return;
	}

	fp = fopen(amd_sysfs_gpu_metrics_path, "rb");
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
		gpu_metrics_state.temp_gfx = metrics_v1.temperature_edge;
		gpu_metrics_state.gfx_activity = metrics_v1.average_gfx_activity;
		gpu_metrics_state.gfx_power = metrics_v1.average_socket_power;
		gpu_metrics_state.gfxclk_freq = 0; /* v1.0 doesn't have frequency */
		gpu_metrics_state.valid = 1;
		debug("  Temperature edge: %u C\n", metrics_v1.temperature_edge);
		debug("  Temperature hotspot: %u C\n", metrics_v1.temperature_hotspot);
		debug("  Average GFX activity: %u%%\n", metrics_v1.average_gfx_activity);
		debug("  Average socket power: %u W\n", metrics_v1.average_socket_power);
	} else if (header.format_revision == 2) {
		struct amd_sysfs_gpu_metrics_v2_0 metrics_v2;
		memset(&metrics_v2, 0, sizeof(metrics_v2));
		bytes_read = fread(&metrics_v2, 1, sizeof(metrics_v2), fp);
		debug("GPU metrics v2.0: read %zu bytes\n", bytes_read);
		gpu_metrics_state.temp_gfx = metrics_v2.temperature_gfx / 100.0;
		gpu_metrics_state.gfx_activity = metrics_v2.average_gfx_activity;
		gpu_metrics_state.gfx_power = metrics_v2.average_gfx_power / 1000.0;
		gpu_metrics_state.gfxclk_freq = metrics_v2.average_gfxclk_frequency;
		gpu_metrics_state.valid = 1;
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
		gpu_metrics_state.temp_gfx = metrics_v3.temperature_gfx / 100.0;
		gpu_metrics_state.gfx_activity = metrics_v3.average_gfx_activity;
		gpu_metrics_state.gfx_power = metrics_v3.average_gfx_power / 1000.0;
		gpu_metrics_state.gfxclk_freq = metrics_v3.average_gfxclk_frequency;
		gpu_metrics_state.valid = 1;
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

int amd_sysfs_get_gpu_temp(void)
{
	return (int)gpu_metrics_state.temp_gfx;
}

uint16_t amd_sysfs_get_gpu_activity(void)
{
	return gpu_metrics_state.gfx_activity;
}

float amd_sysfs_get_gpu_power(void)
{
	return gpu_metrics_state.gfx_power;
}

uint16_t amd_sysfs_get_gpu_freq(void)
{
	return gpu_metrics_state.gfxclk_freq;
}

int amd_sysfs_gpu_metrics_valid(void)
{
	return gpu_metrics_state.valid;
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
