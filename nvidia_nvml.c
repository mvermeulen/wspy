#if NVIDIA

#include "nvidia_nvml.h"
#include "error.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* Minimal, hand-vendored subset of NVIDIA's NVML C API -- just enough to read
 * GPU busy percent and VRAM used/total. NVML's ABI is stable/versioned (the
 * "_v2" suffixes below are the actual exported symbol names in
 * libnvidia-ml.so.1, not just header macros), so this avoids needing the
 * CUDA toolkit / an nvidia-*-dev package's nvml.h at build time -- the
 * library is dlopen()'d at runtime instead, same approach nvtop itself uses.
 */
typedef enum {
	NVML_SUCCESS = 0
} nvmlReturn_t;

typedef struct nvmlDevice_st *nvmlDevice_t;

typedef struct {
	unsigned int gpu;
	unsigned int memory;
} nvmlUtilization_t;

typedef struct {
	unsigned long long total;
	unsigned long long free;
	unsigned long long used;
} nvmlMemory_t;

typedef nvmlReturn_t (*nvmlInit_v2_fn)(void);
typedef nvmlReturn_t (*nvmlShutdown_fn)(void);
typedef nvmlReturn_t (*nvmlDeviceGetCount_v2_fn)(unsigned int *deviceCount);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_v2_fn)(unsigned int index, nvmlDevice_t *device);
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_fn)(nvmlDevice_t device, nvmlUtilization_t *utilization);
typedef nvmlReturn_t (*nvmlDeviceGetMemoryInfo_fn)(nvmlDevice_t device, nvmlMemory_t *memory);
typedef nvmlReturn_t (*nvmlDeviceGetName_fn)(nvmlDevice_t device, char *name, unsigned int length);
typedef const char *(*nvmlErrorString_fn)(nvmlReturn_t result);

static void *nvml_lib = NULL;
static nvmlInit_v2_fn p_nvmlInit_v2;
static nvmlShutdown_fn p_nvmlShutdown;
static nvmlDeviceGetCount_v2_fn p_nvmlDeviceGetCount_v2;
static nvmlDeviceGetHandleByIndex_v2_fn p_nvmlDeviceGetHandleByIndex_v2;
static nvmlDeviceGetUtilizationRates_fn p_nvmlDeviceGetUtilizationRates;
static nvmlDeviceGetMemoryInfo_fn p_nvmlDeviceGetMemoryInfo;
static nvmlDeviceGetName_fn p_nvmlDeviceGetName;
static nvmlErrorString_fn p_nvmlErrorString;

static int nvml_symbols_loaded = 0; /* dlopen+dlsym succeeded */
static int nvml_initialized = 0;    /* nvmlInit_v2() succeeded */

static uint32_t device_count = 0;
static struct nvidia_nvml_device devices[NVIDIA_NVML_MAX_DEVICES];

/* Index into devices[]/NVML device handles that nvidia_nvml_metrics() reads
 * from; -1 means no device selected (either none enumerated, or an
 * explicitly requested --gpu-nvidia-device index was invalid) -- degrades to
 * "no NVML data" rather than silently reading a different device than
 * requested, same convention as amd_smi.c's selected_device_index. */
static int selected_device_index = -1;

static int metrics_valid = 0;
static uint32_t last_busy = 0;
static uint64_t last_vram_used_mb = 0;
static uint64_t last_vram_total_mb = 0;

static const char *nvml_strerror(nvmlReturn_t ret)
{
	if (nvml_symbols_loaded && p_nvmlErrorString) return p_nvmlErrorString(ret);
	return "unknown";
}

/* dlopen()s libnvidia-ml.so.1 (the runtime SONAME the proprietary driver
 * package installs) and resolves every symbol this module needs. A missing
 * library or symbol is logged and leaves nvml_symbols_loaded false rather
 * than aborting -- "NVIDIA driver/library not present" is a normal, expected
 * outcome on a host with no NVIDIA GPU, same idiom as amd_sysfs.c finding no
 * AMD card. */
static int load_nvml_library(void)
{
	if (nvml_symbols_loaded) return 1;

	nvml_lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
	if (!nvml_lib) nvml_lib = dlopen("libnvidia-ml.so", RTLD_NOW);
	if (!nvml_lib) {
		debug("NVIDIA NVML: libnvidia-ml.so.1 not found (%s)\n", dlerror());
		return 0;
	}

	p_nvmlInit_v2 = (nvmlInit_v2_fn)dlsym(nvml_lib, "nvmlInit_v2");
	p_nvmlShutdown = (nvmlShutdown_fn)dlsym(nvml_lib, "nvmlShutdown");
	p_nvmlDeviceGetCount_v2 = (nvmlDeviceGetCount_v2_fn)dlsym(nvml_lib, "nvmlDeviceGetCount_v2");
	p_nvmlDeviceGetHandleByIndex_v2 = (nvmlDeviceGetHandleByIndex_v2_fn)dlsym(nvml_lib, "nvmlDeviceGetHandleByIndex_v2");
	p_nvmlDeviceGetUtilizationRates = (nvmlDeviceGetUtilizationRates_fn)dlsym(nvml_lib, "nvmlDeviceGetUtilizationRates");
	p_nvmlDeviceGetMemoryInfo = (nvmlDeviceGetMemoryInfo_fn)dlsym(nvml_lib, "nvmlDeviceGetMemoryInfo");
	p_nvmlDeviceGetName = (nvmlDeviceGetName_fn)dlsym(nvml_lib, "nvmlDeviceGetName");
	p_nvmlErrorString = (nvmlErrorString_fn)dlsym(nvml_lib, "nvmlErrorString");

	if (!p_nvmlInit_v2 || !p_nvmlShutdown || !p_nvmlDeviceGetCount_v2 ||
	    !p_nvmlDeviceGetHandleByIndex_v2 || !p_nvmlDeviceGetUtilizationRates ||
	    !p_nvmlDeviceGetMemoryInfo || !p_nvmlDeviceGetName) {
		error("NVIDIA NVML: libnvidia-ml.so.1 found but missing expected symbols\n");
		dlclose(nvml_lib);
		nvml_lib = NULL;
		return 0;
	}

	nvml_symbols_loaded = 1;
	return 1;
}

/* device_index < 0 selects device 0 (prior default behavior, matching
 * amd_smi_initialize()); device_index >= 0 requires an exact match against
 * the enumerated device_count. */
void nvidia_nvml_initialize(int device_index)
{
	nvmlReturn_t ret;
	unsigned int i, count = 0;

	if (!load_nvml_library()) return;

	ret = p_nvmlInit_v2();
	if (ret != NVML_SUCCESS) {
		error("nvmlInit_v2 failed: %s\n", nvml_strerror(ret));
		return;
	}
	nvml_initialized = 1;

	ret = p_nvmlDeviceGetCount_v2(&count);
	if (ret != NVML_SUCCESS) {
		error("nvmlDeviceGetCount_v2 failed: %s\n", nvml_strerror(ret));
		return;
	}
	if (count > NVIDIA_NVML_MAX_DEVICES) count = NVIDIA_NVML_MAX_DEVICES;
	device_count = count;

	for (i = 0; i < device_count; i++) {
		nvmlDevice_t handle;

		devices[i].index = (int)i;
		devices[i].name[0] = '\0';
		ret = p_nvmlDeviceGetHandleByIndex_v2(i, &handle);
		if (ret == NVML_SUCCESS) {
			p_nvmlDeviceGetName(handle, devices[i].name, sizeof(devices[i].name));
		} else {
			error("nvmlDeviceGetHandleByIndex_v2 failed for device %u: %s\n", i, nvml_strerror(ret));
		}
	}

	if (device_index < 0) {
		selected_device_index = (device_count > 0) ? 0 : -1;
	} else if ((uint32_t)device_index < device_count) {
		selected_device_index = device_index;
	} else {
		error("NVIDIA NVML: --gpu-nvidia-device=%d not found (%u device(s) enumerated)\n",
		      device_index, device_count);
		selected_device_index = -1;
	}
}

uint32_t nvidia_nvml_device_count(void)
{
	return device_count;
}

int nvidia_nvml_selected_device(void)
{
	return selected_device_index;
}

void nvidia_nvml_print_capability_report(FILE *out)
{
	uint32_t i;

	if (!nvml_symbols_loaded) {
		fprintf(out, "NVIDIA NVML: libnvidia-ml.so.1 not found (no NVIDIA driver installed?)\n");
		return;
	}
	if (!nvml_initialized) {
		fprintf(out, "NVIDIA NVML: library found but nvmlInit_v2() failed\n");
		return;
	}

	fprintf(out, "NVIDIA NVML devices found: %u\n", device_count);
	for (i = 0; i < device_count; i++) {
		fprintf(out, "  device %u: %s%s\n", i,
			devices[i].name[0] ? devices[i].name : "?",
			(int)i == selected_device_index ? " (selected)" : "");
	}
}

void nvidia_nvml_metrics(void)
{
	nvmlReturn_t ret;
	nvmlDevice_t handle;
	nvmlUtilization_t util;
	nvmlMemory_t mem;

	metrics_valid = 0;

	if (!nvml_initialized || selected_device_index < 0) {
		debug("NVIDIA NVML: no device selected for metrics\n");
		return;
	}

	ret = p_nvmlDeviceGetHandleByIndex_v2((unsigned int)selected_device_index, &handle);
	if (ret != NVML_SUCCESS) {
		error("nvmlDeviceGetHandleByIndex_v2 failed: %s\n", nvml_strerror(ret));
		return;
	}

	ret = p_nvmlDeviceGetUtilizationRates(handle, &util);
	if (ret != NVML_SUCCESS) {
		error("nvmlDeviceGetUtilizationRates failed: %s\n", nvml_strerror(ret));
		return;
	}
	last_busy = util.gpu;

	ret = p_nvmlDeviceGetMemoryInfo(handle, &mem);
	if (ret != NVML_SUCCESS) {
		error("nvmlDeviceGetMemoryInfo failed: %s\n", nvml_strerror(ret));
		return;
	}
	last_vram_used_mb = mem.used / (1024 * 1024);
	last_vram_total_mb = mem.total / (1024 * 1024);

	metrics_valid = 1;
	debug("NVIDIA NVML device %d: busy=%u%% vram_used=%lluMB vram_total=%lluMB\n",
	      selected_device_index, last_busy,
	      (unsigned long long)last_vram_used_mb, (unsigned long long)last_vram_total_mb);
}

int nvidia_nvml_metrics_valid(void)
{
	return metrics_valid;
}

uint32_t nvidia_nvml_get_busy(void)
{
	return last_busy;
}

uint64_t nvidia_nvml_get_vram_used_mb(void)
{
	return last_vram_used_mb;
}

uint64_t nvidia_nvml_get_vram_total_mb(void)
{
	return last_vram_total_mb;
}

void nvidia_nvml_finalize(void)
{
	if (nvml_initialized) {
		nvmlReturn_t ret = p_nvmlShutdown();
		if (ret != NVML_SUCCESS) {
			error("nvmlShutdown failed: %s\n", nvml_strerror(ret));
		}
		nvml_initialized = 0;
	}

	if (nvml_lib) {
		dlclose(nvml_lib);
		nvml_lib = NULL;
	}

	nvml_symbols_loaded = 0;
	device_count = 0;
	selected_device_index = -1;
	metrics_valid = 0;
}

#if TEST_NVIDIA_NVML
int main(void)
{
	set_error_level(ERROR_LEVEL_DEBUG);

	nvidia_nvml_initialize(-1);
	nvidia_nvml_print_capability_report(stdout);
	nvidia_nvml_metrics();
	if (nvidia_nvml_metrics_valid()) {
		printf("busy=%u%% vram_used=%lluMB vram_total=%lluMB\n",
		       nvidia_nvml_get_busy(),
		       (unsigned long long)nvidia_nvml_get_vram_used_mb(),
		       (unsigned long long)nvidia_nvml_get_vram_total_mb());
	} else {
		printf("no valid metrics\n");
	}
	nvidia_nvml_finalize();
	return 0;
}
#endif

#endif /* NVIDIA */
