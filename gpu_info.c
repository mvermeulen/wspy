/*
 * gpu_info.c - get information on AMD ROCm GPU using libamdsmi
 */
#if AMDGPU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "amd_smi/amdsmi.h"
#include "gpu_info.h"
#include "error.h"

amdsmi_socket_handle *sockets;
amdsmi_processor_handle *processor_handles;

int num_gpu = 0;
void **gpu_handles;

void gpu_info_query(struct gpu_query_data *qd){
  amdsmi_gpu_metrics_t metric_info;
  amdsmi_engine_usage_t engine_usage;
  amdsmi_power_info_t power_info;
  amdsmi_vram_usage_t vram_usage;
  amdsmi_vram_info_t vram_info;
  amdsmi_clk_info_t clk_info;
  int64_t temp;
  int status;
  
  if (num_gpu == 0) {
    debug("no GPUs detected, cannot query GPU metrics\n");
    if (qd) {
      memset(qd, 0, sizeof(*qd));
    }
    return;
  }
  
  // Try the GPU metrics API first (preferred, but not supported on all GPUs)
  status = amdsmi_get_gpu_metrics_info(gpu_handles[0],&metric_info);
  if (status == AMDSMI_STATUS_SUCCESS){
    debug("using amdsmi_get_gpu_metrics_info\n");
    if (!qd) return;
    
    qd->temperature = metric_info.temperature_edge;
    qd->gfx_activity = metric_info.average_gfx_activity;
    qd->umc_activity = metric_info.average_umc_activity;
    qd->mm_activity = metric_info.average_mm_activity;
    qd->gfx_clock_mhz = metric_info.current_gfxclk;
    qd->mem_clock_mhz = metric_info.current_uclk;
    qd->soc_clock_mhz = metric_info.current_socclk;
    qd->power_watts = metric_info.current_socket_power;
    // Note: metrics API doesn't include VRAM usage, will query separately below
    debug("metrics API: temp=%d°C gfx=%d%% power=%dW\n", 
          qd->temperature, qd->gfx_activity, qd->power_watts);
    // Continue to get VRAM info
  } else {
    // Fallback to individual API calls if metrics API not supported
    debug("amdsmi_get_gpu_metrics_info failed (status=%d), using fallback APIs\n", status);
  }
  
  if (!qd) return;
  
  // Initialize fields if not already set by metrics API
  if (status != AMDSMI_STATUS_SUCCESS) {
    memset(qd, 0, sizeof(*qd));
  }
  
  // Try to get temperature (edge temperature)
  status = amdsmi_get_temp_metric(gpu_handles[0], 
                                   AMDSMI_TEMPERATURE_TYPE_EDGE,
                                   AMDSMI_TEMP_CURRENT, 
                                   &temp);
  if (status == AMDSMI_STATUS_SUCCESS) {
    // Try both interpretations - sometimes API returns degrees, sometimes millidegrees
    if (temp < 200) {
      // Likely already in degrees Celsius
      qd->temperature = (uint16_t)temp;
      debug("temperature: %ld°C (interpreted as degrees)\n", temp);
    } else {
      // Temperature is in millidegrees Celsius
      qd->temperature = (uint16_t)(temp / 1000);
      debug("temperature: %ldm°C -> %d°C\n", temp, qd->temperature);
    }
  } else {
    debug("failed to get edge temperature (status=%d)\n", status);
    // Try hotspot as fallback
    status = amdsmi_get_temp_metric(gpu_handles[0], 
                                     AMDSMI_TEMPERATURE_TYPE_HOTSPOT,
                                     AMDSMI_TEMP_CURRENT, 
                                     &temp);
    if (status == AMDSMI_STATUS_SUCCESS) {
      if (temp < 200) {
        qd->temperature = (uint16_t)temp;
        debug("hotspot temperature: %ld°C\n", temp);
      } else {
        qd->temperature = (uint16_t)(temp / 1000);
        debug("hotspot temperature: %ldm°C -> %d°C\n", temp, qd->temperature);
      }
    } else {
      debug("failed to get hotspot temperature (status=%d)\n", status);
    }
  }
  
  // Try to get GPU activity (GFX, UMC, MM engines)
  status = amdsmi_get_gpu_activity(gpu_handles[0], &engine_usage);
  if (status == AMDSMI_STATUS_SUCCESS) {
    qd->gfx_activity = engine_usage.gfx_activity;
    qd->umc_activity = engine_usage.umc_activity;
    qd->mm_activity = engine_usage.mm_activity;
    debug("activity: gfx=%d%% umc=%d%% mm=%d%%\n", 
          qd->gfx_activity, qd->umc_activity, qd->mm_activity);
  } else {
    debug("failed to get GPU activity (status=%d), trying busy_percent\n", status);
    
    // Try alternative busy percent API
    uint32_t busy_percent;
    status = amdsmi_get_gpu_busy_percent(gpu_handles[0], &busy_percent);
    if (status == AMDSMI_STATUS_SUCCESS) {
      qd->gfx_activity = busy_percent;
      debug("GPU busy: %u%%\n", busy_percent);
    } else {
      debug("failed to get GPU busy percent (status=%d)\n", status);
    }
  }
  
  // Optionally get power info (for future use)
  status = amdsmi_get_power_info(gpu_handles[0], &power_info);
  if (status == AMDSMI_STATUS_SUCCESS) {
    if (qd->power_watts == 0) { // Only set if not already from metrics API
      qd->power_watts = power_info.current_socket_power ? 
                        power_info.current_socket_power : 
                        power_info.average_socket_power;
    }
    qd->power_limit_watts = power_info.power_limit;
    debug("power: %uW (limit=%uW)\n", qd->power_watts, qd->power_limit_watts);
  } else {
    debug("failed to get power info (status=%d)\n", status);
  }
  
  // Get VRAM usage
  status = amdsmi_get_gpu_vram_usage(gpu_handles[0], &vram_usage);
  if (status == AMDSMI_STATUS_SUCCESS) {
    qd->vram_total_mb = vram_usage.vram_total;
    qd->vram_used_mb = vram_usage.vram_used;
    qd->vram_free_mb = vram_usage.vram_total - vram_usage.vram_used;
    debug("VRAM: %u MB used / %u MB total (%u MB free)\n",
          qd->vram_used_mb, qd->vram_total_mb, qd->vram_free_mb);
  } else {
    debug("failed to get VRAM usage (status=%d)\n", status);
  }
  
  // Get VRAM info (static information)
  status = amdsmi_get_gpu_vram_info(gpu_handles[0], &vram_info);
  if (status == AMDSMI_STATUS_SUCCESS) {
    if (qd->vram_total_mb == 0) { // Use as fallback if vram_usage failed
      qd->vram_total_mb = vram_info.vram_size;
    }
    // Check for valid bandwidth (sometimes returns UINT64_MAX for invalid/unknown)
    if (vram_info.vram_max_bandwidth != 0 && 
        vram_info.vram_max_bandwidth != UINT64_MAX) {
      debug("VRAM info: type=%d size=%lu MB bandwidth=%lu GB/s width=%u bits\n",
            vram_info.vram_type, vram_info.vram_size, 
            vram_info.vram_max_bandwidth, vram_info.vram_bit_width);
    } else {
      debug("VRAM info: type=%d size=%lu MB width=%u bits (bandwidth unavailable)\n",
            vram_info.vram_type, vram_info.vram_size, vram_info.vram_bit_width);
    }
  } else {
    debug("failed to get VRAM info (status=%d)\n", status);
  }
  
  // Get clock frequencies
  // Graphics clock
  if (qd->gfx_clock_mhz == 0) { // Only if not set by metrics API
    status = amdsmi_get_clock_info(gpu_handles[0], AMDSMI_CLK_TYPE_GFX, &clk_info);
    if (status == AMDSMI_STATUS_SUCCESS) {
      qd->gfx_clock_mhz = clk_info.clk;
      debug("GFX clock: %u MHz (min=%u max=%u)\n", 
            clk_info.clk, clk_info.min_clk, clk_info.max_clk);
    } else {
      debug("failed to get GFX clock (status=%d)\n", status);
    }
  }
  
  // Memory clock
  if (qd->mem_clock_mhz == 0) {
    status = amdsmi_get_clock_info(gpu_handles[0], AMDSMI_CLK_TYPE_MEM, &clk_info);
    if (status == AMDSMI_STATUS_SUCCESS) {
      qd->mem_clock_mhz = clk_info.clk;
      debug("MEM clock: %u MHz (min=%u max=%u)\n", 
            clk_info.clk, clk_info.min_clk, clk_info.max_clk);
    } else {
      debug("failed to get MEM clock (status=%d)\n", status);
    }
  }
  
  // SOC clock
  if (qd->soc_clock_mhz == 0) {
    status = amdsmi_get_clock_info(gpu_handles[0], AMDSMI_CLK_TYPE_SOC, &clk_info);
    if (status == AMDSMI_STATUS_SUCCESS) {
      qd->soc_clock_mhz = clk_info.clk;
      debug("SOC clock: %u MHz (min=%u max=%u)\n", 
            clk_info.clk, clk_info.min_clk, clk_info.max_clk);
    } else {
      debug("failed to get SOC clock (status=%d)\n", status);
    }
  }
  
  return;

  qd->temperature = metric_info.temperature_edge;
  qd->gfx_activity = metric_info.average_gfx_activity;
  qd->umc_activity = metric_info.average_umc_activity;
  qd->mm_activity = metric_info.average_mm_activity;
  //  notice("gfxclk              %d MHz\n",metric_info.average_gfxclk_frequency);
  //  notice("gfx activity acc    %d\n",metric_info.gfx_activity_acc);
  //  notice("mem activity acc    %d\n",metric_info.mem_activity_acc);
  //  notice("socket power        %d watts\n",metric_info.current_socket_power);
}

void gpu_info_initialize(void){
  int status;
  int i,j;
  unsigned int socket_count;
  unsigned int device_count;
  unsigned long value;
  
  status = amdsmi_init(AMDSMI_INIT_AMD_GPUS);
  if (status != AMDSMI_STATUS_SUCCESS){
    warning("unable to initialize amdsmi (status=%d) - no AMD GPUs available?\n", status);
    return;
  }

  // get the socket count and allocate memory
  status = amdsmi_get_socket_handles(&socket_count,NULL);
  if (status != AMDSMI_STATUS_SUCCESS){
    warning("unable to get amdsmi socket handles (status=%d)\n", status);
    return;
  }

  debug("found %d socket(s)\n", socket_count);

  sockets = calloc(socket_count,sizeof(amdsmi_socket_handle));
  
  // fill in the socket handles
  status = amdsmi_get_socket_handles(&socket_count,sockets);
  if (status != AMDSMI_STATUS_SUCCESS){
    warning("unable to get amdsmi socket handles (status=%d)\n", status);
    return;
  }
  
  // for each socket, get identifier and devices
  for (i=0;i<socket_count;i++){
    char socket_info[128];
    status = amdsmi_get_socket_info(sockets[i],sizeof(socket_info),socket_info);
    if (status != AMDSMI_STATUS_SUCCESS){
      warning("unable to get amdsmi socket info (status=%d)\n", status);
      continue;
    }

    debug("socket %d: %s\n", i, socket_info);

    // get the device count for the socket and allocate memory
    status = amdsmi_get_processor_handles(sockets[i],&device_count,NULL);
    if (status != AMDSMI_STATUS_SUCCESS){
      warning("unable to get amdsmi device count (status=%d)\n", status);
      continue;
    }

    debug("socket %d has %d device(s)\n", i, device_count);

    if (num_gpu == 0){
      gpu_handles = malloc(device_count * sizeof(gpu_handles[0]));
    } else {
      gpu_handles = realloc(gpu_handles,(device_count+num_gpu)*sizeof(gpu_handles[0]));
    }

    //    processor_handles = calloc(device_count,sizeof(amdsmi_processor_handle));

    // get the devices of the socket
    status = amdsmi_get_processor_handles(sockets[i],&device_count,/*processor_handles*/&gpu_handles[num_gpu]);
    if (status != AMDSMI_STATUS_SUCCESS){
      warning("unable to get amdsmi device handles (status=%d)\n", status);
      continue;
    }
    num_gpu += device_count;
  }
  
  debug("total GPUs found: %d\n", num_gpu);
}

void gpu_info_finalize(void){
  int status;
  status = amdsmi_shut_down();
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to shut down amdsmi\n");
}

#if TEST_GPU_INFO
int main(void){
  struct gpu_query_data qd;
  
  initialize_error_subsystem("gpu_info", "-");
  set_error_level(ERROR_LEVEL_DEBUG);
  
  gpu_info_initialize();

  if (num_gpu == 0) {
    notice("No AMD GPUs detected\n");
    return 1;
  }

  notice("Found %d GPU(s)\n", num_gpu);
  
  // Try to get device info
  amdsmi_asic_info_t asic_info;
  int status = amdsmi_get_gpu_asic_info(gpu_handles[0], &asic_info);
  if (status == AMDSMI_STATUS_SUCCESS) {
    notice("ASIC: %s\n", asic_info.market_name);
  }

  gpu_info_query(&qd);
  
  // Display collected metrics
  notice("\n=== GPU Metrics ===\n");
  
  if (qd.temperature > 0) {
    notice("Temperature:   %d°C\n", qd.temperature);
  }
  
  if (qd.gfx_activity > 0 || qd.umc_activity > 0 || qd.mm_activity > 0) {
    notice("Activity:\n");
    notice("  GFX:         %d%%\n", qd.gfx_activity);
    notice("  UMC:         %d%%\n", qd.umc_activity);
    notice("  MM:          %d%%\n", qd.mm_activity);
  }
  
  if (qd.vram_total_mb > 0) {
    notice("VRAM:\n");
    notice("  Total:       %u MB\n", qd.vram_total_mb);
    notice("  Used:        %u MB\n", qd.vram_used_mb);
    notice("  Free:        %u MB\n", qd.vram_free_mb);
    if (qd.vram_total_mb > 0) {
      notice("  Usage:       %d%%\n", 
             (qd.vram_used_mb * 100) / qd.vram_total_mb);
    }
  }
  
  if (qd.gfx_clock_mhz > 0 || qd.mem_clock_mhz > 0 || qd.soc_clock_mhz > 0) {
    notice("Clocks:\n");
    if (qd.gfx_clock_mhz > 0)
      notice("  GFX:         %u MHz\n", qd.gfx_clock_mhz);
    if (qd.mem_clock_mhz > 0)
      notice("  MEM:         %u MHz\n", qd.mem_clock_mhz);
    if (qd.soc_clock_mhz > 0)
      notice("  SOC:         %u MHz\n", qd.soc_clock_mhz);
  }
  
  if (qd.power_watts > 0) {
    notice("Power:\n");
    notice("  Current:     %u W\n", qd.power_watts);
    if (qd.power_limit_watts > 0) {
      notice("  Limit:       %u W\n", qd.power_limit_watts);
      notice("  Usage:       %d%%\n", 
             (qd.power_watts * 100) / qd.power_limit_watts);
    }
  }

  gpu_info_finalize();
  return 0;
}
#endif
#endif
