#if AMDGPU

#include "amd_smi/amdsmi.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static amdsmi_socket_handle *sockets = NULL;
static uint32_t socket_count = 0;
static amdsmi_processor_handle *devices = NULL;
static uint32_t device_count = 0;
/* Parallel to devices[]: which socket index (into sockets[]) each device
 * came from, kept only so amd_smi_print_capability_report() can attribute
 * a device back to its socket without re-querying amdsmi. */
static uint32_t *device_socket_index = NULL;
static char (*socket_names)[256] = NULL;

/* Index into devices[] that amd_smi_metrics()/amd_smi_memory() read from;
 * -1 means an explicitly requested --gpu-device index was invalid, so no
 * device is selected (degrades to "no SMI data" rather than a silent
 * fallback to a different device than the one requested). */
static int selected_device_index = 0;

static int metrics_valid = 0;
static uint16_t last_temp = 0;
static uint16_t last_activity = 0;

static int memory_valid = 0;
static uint32_t last_vram_used = 0;
static uint32_t last_vram_total = 0;

/* device_index < 0 selects device 0 (prior default behavior); device_index
 * >= 0 requires an exact match against the enumerated device_count. */
void amd_smi_initialize(int device_index)
{
	amdsmi_status_t ret;

	ret = amdsmi_init(AMDSMI_INIT_AMD_GPUS);
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_init failed with status %d\n", ret);
		return;
	}

	ret = amdsmi_get_socket_handles(&socket_count, NULL);
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_get_socket_handles failed with status %d\n", ret);
		return;
	}

	debug("AMD SMI found %u socket(s)\n", socket_count);

	if (socket_count > 0) {
		sockets = (amdsmi_socket_handle *)malloc(socket_count * sizeof(amdsmi_socket_handle));
		socket_names = malloc(socket_count * sizeof(*socket_names));
		if (sockets == NULL || socket_names == NULL) {
			error("failed to allocate memory for socket handles\n");
			free(sockets); sockets = NULL;
			free(socket_names); socket_names = NULL;
			socket_count = 0;
			return;
		}

		ret = amdsmi_get_socket_handles(&socket_count, sockets);
		if (ret != AMDSMI_STATUS_SUCCESS) {
			error("amdsmi_get_socket_handles (with buffer) failed with status %d\n", ret);
			free(sockets); sockets = NULL;
			free(socket_names); socket_names = NULL;
			socket_count = 0;
			return;
		}

		// Get and display socket information
		for (uint32_t i = 0; i < socket_count; i++) {
			ret = amdsmi_get_socket_info(sockets[i], sizeof(socket_names[i]), socket_names[i]);
			if (ret == AMDSMI_STATUS_SUCCESS) {
				debug("Socket %u: name=%s\n", i, socket_names[i]);
			} else {
				error("amdsmi_get_socket_info failed for socket %u with status %d\n", i, ret);
				socket_names[i][0] = '\0';
			}

			// Get device count for this socket
			uint32_t socket_device_count = 0;
			ret = amdsmi_get_processor_handles(sockets[i], &socket_device_count, NULL);
			if (ret == AMDSMI_STATUS_SUCCESS) {
				debug("Socket %u has %u device(s)\n", i, socket_device_count);
				device_count += socket_device_count;
			} else {
				error("amdsmi_get_processor_handles failed for socket %u with status %d\n", i, ret);
			}
		}

		// Allocate memory for all device handles
		if (device_count > 0) {
			devices = (amdsmi_processor_handle *)malloc(device_count * sizeof(amdsmi_processor_handle));
			device_socket_index = (uint32_t *)malloc(device_count * sizeof(uint32_t));
			if (devices == NULL || device_socket_index == NULL) {
				error("failed to allocate memory for device handles\n");
				free(sockets); sockets = NULL;
				free(socket_names); socket_names = NULL;
				free(devices); devices = NULL;
				free(device_socket_index); device_socket_index = NULL;
				socket_count = 0;
				device_count = 0;
				return;
			}

			// Get all device handles
			uint32_t device_index_pos = 0;
			for (uint32_t i = 0; i < socket_count; i++) {
				uint32_t socket_device_count = 0;
				ret = amdsmi_get_processor_handles(sockets[i], &socket_device_count, NULL);
				if (ret == AMDSMI_STATUS_SUCCESS && socket_device_count > 0) {
					ret = amdsmi_get_processor_handles(sockets[i], &socket_device_count, &devices[device_index_pos]);
					if (ret == AMDSMI_STATUS_SUCCESS) {
						debug("Retrieved %u device handle(s) from socket %u\n", socket_device_count, i);
						for (uint32_t j = 0; j < socket_device_count; j++) {
							device_socket_index[device_index_pos + j] = i;
						}
						device_index_pos += socket_device_count;
					} else {
						error("amdsmi_get_processor_handles (with buffer) failed for socket %u with status %d\n", i, ret);
					}
				}
			}
		}
	}

	if (device_index < 0) {
		selected_device_index = (device_count > 0) ? 0 : -1;
	} else if ((uint32_t)device_index < device_count) {
		selected_device_index = device_index;
	} else {
		error("AMD SMI: --gpu-device=%d not found (%u device(s) enumerated)\n", device_index, device_count);
		selected_device_index = -1;
	}
}

uint32_t amd_smi_device_count(void)
{
	return device_count;
}

int amd_smi_selected_device(void)
{
	return selected_device_index;
}

void amd_smi_print_capability_report(FILE *out)
{
	uint32_t i;

	fprintf(out, "AMD SMI devices found: %u (%u socket(s))\n", device_count, socket_count);
	for (i = 0; i < device_count; i++) {
		uint32_t sock = device_socket_index ? device_socket_index[i] : 0;
		fprintf(out, "  device %u: socket=%s%s\n", i,
			(socket_names && sock < socket_count) ? socket_names[sock] : "?",
			(int)i == selected_device_index ? " (selected)" : "");
	}
}

void amd_smi_metrics(void)
{
	amdsmi_status_t ret;

	metrics_valid = 0;

	if (devices == NULL || device_count == 0) {
		debug("No devices available for metrics\n");
		return;
	}

	for (uint32_t i = 0; i < device_count; i++) {
		amdsmi_gpu_metrics_t metrics;
		ret = amdsmi_get_gpu_metrics_info(devices[i], &metrics);
		if (ret == AMDSMI_STATUS_SUCCESS) {
			debug("Device %u metrics: temperature=%u, gfx_activity=%u\n",
			      i, metrics.temperature_hotspot, metrics.average_gfx_activity);
			if ((int)i == selected_device_index) {
				last_temp = metrics.temperature_hotspot;
				last_activity = metrics.average_gfx_activity;
				metrics_valid = 1;
			}
		} else {
			error("amdsmi_get_gpu_metrics_info failed for device %u with status %d\n", i, ret);
		}
	}
}

int amd_smi_metrics_valid(void)
{
	return metrics_valid;
}

uint16_t amd_smi_get_temp(void)
{
	return last_temp;
}

uint16_t amd_smi_get_activity(void)
{
	return last_activity;
}


void amd_smi_memory(void)
{
	amdsmi_status_t ret;

	memory_valid = 0;

	if (devices == NULL || device_count == 0) {
		debug("No devices available for VRAM usage\n");
		return;
	}
	for (uint32_t i = 0; i < device_count; i++) {
		amdsmi_vram_usage_t vram;
		ret = amdsmi_get_gpu_vram_usage(devices[i], &vram);
		if (ret == AMDSMI_STATUS_SUCCESS) {
			debug("Device %u VRAM usage: total=%u MB, used=%u MB\n",
			      i, vram.vram_total, vram.vram_used);
			if ((int)i == selected_device_index) {
				last_vram_used = vram.vram_used;
				last_vram_total = vram.vram_total;
				memory_valid = 1;
			}
		} else {
			error("amdsmi_get_gpu_vram_usage failed for device %u with status %d\n", i, ret);
		}
	}
}

int amd_smi_memory_valid(void)
{
	return memory_valid;
}

uint32_t amd_smi_get_vram_used(void)
{
	return last_vram_used;
}

uint32_t amd_smi_get_vram_total(void)
{
	return last_vram_total;
}

void amd_smi_finalize(void)
{
	amdsmi_status_t ret;
	
	if (devices) {
		free(devices);
		devices = NULL;
		device_count = 0;
	}

	if (device_socket_index) {
		free(device_socket_index);
		device_socket_index = NULL;
	}

	if (sockets) {
		free(sockets);
		sockets = NULL;
		socket_count = 0;
	}

	if (socket_names) {
		free(socket_names);
		socket_names = NULL;
	}

	selected_device_index = 0;

	ret = amdsmi_shut_down();
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_shut_down failed with status %d\n", ret);
	}
}

#if TEST_AMD_SMI
int main(void){
	set_error_level(ERROR_LEVEL_DEBUG);

	amd_smi_initialize(-1);
	amd_smi_print_capability_report(stdout);
	amd_smi_metrics();
	amd_smi_memory();
	amd_smi_finalize();
	return 0;
}
#endif
#endif
