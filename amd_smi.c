#if AMDGPU

#include "amd_smi/amdsmi.h"
#include "error.h"
#include <stdlib.h>
#include <inttypes.h>

static amdsmi_socket_handle *sockets = NULL;
static uint32_t socket_count = 0;
static amdsmi_processor_handle *devices = NULL;
static uint32_t device_count = 0;

static int metrics_valid = 0;
static uint16_t last_temp = 0;
static uint16_t last_activity = 0;

static int memory_valid = 0;
static uint32_t last_vram_used = 0;
static uint32_t last_vram_total = 0;

void amd_smi_initialize(void)
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
		if (sockets == NULL) {
			error("failed to allocate memory for socket handles\n");
			return;
		}
		
		ret = amdsmi_get_socket_handles(&socket_count, sockets);
		if (ret != AMDSMI_STATUS_SUCCESS) {
			error("amdsmi_get_socket_handles (with buffer) failed with status %d\n", ret);
			free(sockets);
			sockets = NULL;
			socket_count = 0;
			return;
		}
		
		// Get and display socket information
		for (uint32_t i = 0; i < socket_count; i++) {
			char socket_name[256];
			ret = amdsmi_get_socket_info(sockets[i], sizeof(socket_name), socket_name);
			if (ret == AMDSMI_STATUS_SUCCESS) {
				debug("Socket %u: name=%s\n", i, socket_name);
			} else {
				error("amdsmi_get_socket_info failed for socket %u with status %d\n", i, ret);
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
			if (devices == NULL) {
				error("failed to allocate memory for device handles\n");
				free(sockets);
				sockets = NULL;
				socket_count = 0;
				return;
			}
			
			// Get all device handles
			uint32_t device_index = 0;
			for (uint32_t i = 0; i < socket_count; i++) {
				uint32_t socket_device_count = 0;
				ret = amdsmi_get_processor_handles(sockets[i], &socket_device_count, NULL);
				if (ret == AMDSMI_STATUS_SUCCESS && socket_device_count > 0) {
					ret = amdsmi_get_processor_handles(sockets[i], &socket_device_count, &devices[device_index]);
					if (ret == AMDSMI_STATUS_SUCCESS) {
						debug("Retrieved %u device handle(s) from socket %u\n", socket_device_count, i);
						device_index += socket_device_count;
					} else {
						error("amdsmi_get_processor_handles (with buffer) failed for socket %u with status %d\n", i, ret);
					}
				}
			}
		}
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
			if (i == 0) {
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
			if (i == 0) {
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
	
	if (sockets) {
		free(sockets);
		sockets = NULL;
		socket_count = 0;
	}
	
	ret = amdsmi_shut_down();
	if (ret != AMDSMI_STATUS_SUCCESS) {
		error("amdsmi_shut_down failed with status %d\n", ret);
	}
}

#if TEST_AMD_SMI
int main(void){
	set_error_level(ERROR_LEVEL_DEBUG);
	
	amd_smi_initialize();
	amd_smi_metrics();
	amd_smi_memory();
	amd_smi_finalize();
	return 0;
}
#endif
#endif
