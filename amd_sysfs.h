 #if AMDGPU
#include <stdint.h>

// Helper to match the kernel struct layout
// See: drivers/gpu/drm/amd/include/kgd_pp_interface.h in Linux source
struct amd_sysfs_metrics_header {
    uint16_t structure_size;
    uint8_t format_revision;
    uint8_t content_revision;
};

// Simplified v1.0 struct
struct amd_sysfs_gpu_metrics_v1_0 {
    struct amd_sysfs_metrics_header common_header;
    uint64_t system_clock_counter;
    uint16_t temperature_edge;
    uint16_t temperature_hotspot;
    uint16_t temperature_mem;
    uint16_t temperature_vrgfx;
    uint16_t temperature_vrsoc;
    uint16_t temperature_vrmem;
    uint16_t average_gfx_activity;
    uint16_t average_umc_activity;
    uint16_t average_mm_activity;
    uint16_t average_socket_power;
    uint64_t energy_accumulator;
};

struct amd_sysfs_gpu_metrics_v2_0 {
	struct amd_sysfs_metrics_header	common_header;

	uint16_t			temperature_gfx;
	uint16_t			temperature_soc;
	uint16_t			temperature_core[8];
	uint16_t			temperature_l3[2];

	uint16_t			average_gfx_activity;
	uint16_t			average_mm_activity;

	uint64_t			system_clock_counter;

	uint16_t			average_socket_power;
	uint16_t			average_cpu_power;
	uint16_t			average_soc_power;
	uint16_t			average_gfx_power;
	uint16_t			average_core_power[8];

	uint16_t			average_gfxclk_frequency;
	uint16_t			average_socclk_frequency;
	uint16_t			average_uclk_frequency;
	uint16_t			average_fclk_frequency;
	uint16_t			average_vclk_frequency;
	uint16_t			average_dclk_frequency;

	uint16_t			current_gfxclk;
	uint16_t			current_socclk;
	uint16_t			current_uclk;
	uint16_t			current_fclk;
	uint16_t			current_vclk;
	uint16_t			current_dclk;
	uint16_t			current_coreclk[8];
	uint16_t			current_l3clk[2];

	uint32_t			throttle_status;

	uint16_t			fan_pwm;

	uint16_t			padding[3];
};

struct amd_sysfs_gpu_metrics_v3_0 {
	struct amd_sysfs_metrics_header	common_header;
	uint16_t			temperature_gfx;
	uint16_t			temperature_soc;
	uint16_t			temperature_core[16];
	uint16_t			temperature_skin;

	uint16_t			average_gfx_activity;
	uint16_t			average_vcn_activity;
	uint16_t			average_ipu_activity[8];
	uint16_t			average_core_c0_activity[16];
	uint16_t			average_dram_reads;
	uint16_t			average_dram_writes;
	uint16_t			average_ipu_reads;
	uint16_t			average_ipu_writes;

	uint64_t			system_clock_counter;

	uint32_t			average_socket_power;
	uint16_t			average_ipu_power;
	uint32_t			average_apu_power;
	uint32_t			average_gfx_power;
	uint32_t			average_dgpu_power;
	uint32_t			average_all_core_power;
	uint16_t			average_core_power[16];
	uint16_t			average_sys_power;
	uint16_t			stapm_power_limit;
	uint16_t			current_stapm_power_limit;

	uint16_t			average_gfxclk_frequency;
	uint16_t			average_socclk_frequency;
	uint16_t			average_vpeclk_frequency;
	uint16_t			average_ipuclk_frequency;
	uint16_t			average_fclk_frequency;
	uint16_t			average_vclk_frequency;
	uint16_t			average_uclk_frequency;
	uint16_t			average_mpipu_frequency;

	uint16_t			current_coreclk[16];
	uint16_t			current_core_maxfreq;
	uint16_t			current_gfx_maxfreq;

	uint32_t			throttle_residency_prochot;
	uint32_t			throttle_residency_spl;
	uint32_t			throttle_residency_fppt;
	uint32_t			throttle_residency_sppt;
	uint32_t			throttle_residency_thm_core;
	uint32_t			throttle_residency_thm_gfx;
	uint32_t			throttle_residency_thm_soc;

	uint32_t			time_filter_alphavalue;
};


 void amd_sysfs_initialize(void);
 void amd_sysfs_finalize(void);
 int amd_sysfs_gpu_busy_percent(void);
 void amd_sysfs_gpu_metrics(void);
 #endif
 int amd_sysfs_get_gpu_temp(void);
 uint16_t amd_sysfs_get_gpu_activity(void);
 float amd_sysfs_get_gpu_power(void);
 uint16_t amd_sysfs_get_gpu_freq(void);
 int amd_sysfs_gpu_metrics_valid(void);
