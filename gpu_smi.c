/*
 * gpu_smi.c - get information on AMD ROCm GPU using libamdsmi
 */
#if AMDGPU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "amd_smi/amdsmi.h"
#include "gpu_smi.h"
#include "error.h"

amdsmi_socket_handle *sockets;
amdsmi_processor_handle *processor_handles;

int num_gpu = 0;
void **gpu_handles;

int gpu_smi_available(void){
  int status;
  if (num_gpu > 0)
    status = amdsmi_get_gpu_metrics_info(gpu_handles[0],NULL);
  return (status == AMDSMI_STATUS_SUCCESS) ? 1 : 0;
}

void gpu_smi_query(struct gpu_smi_data *qd){
  amdsmi_gpu_metrics_t metric_info;
  
  if (!gpu_smi_available())
    fatal("unable to get gpu metrics\n");
  if (!qd) return;
  
  amdsmi_get_gpu_metrics_info(gpu_handles[0],&metric_info);

  qd->temperature = metric_info.temperature_edge;
  qd->gfx_activity = metric_info.average_gfx_activity;
  qd->umc_activity = metric_info.average_umc_activity;
  qd->mm_activity = metric_info.average_mm_activity;
  // Optional extended metrics (best-effort; default to 0 if unavailable)
  qd->gfx_clock_mhz = metric_info.average_gfxclk_frequency;
#ifdef AMDSMI_METRIC_HAS_UMC_CLK
  qd->mem_clock_mhz = metric_info.average_umcclk_frequency;
#else
  qd->mem_clock_mhz = 0;
#endif
  qd->power_watts = metric_info.current_socket_power;
  qd->vram_used_mb = 0;
  qd->vram_total_mb = 0;
  //  notice("gfxclk              %d MHz\n",metric_info.average_gfxclk_frequency);
  //  notice("gfx activity acc    %d\n",metric_info.gfx_activity_acc);
  //  notice("mem activity acc    %d\n",metric_info.mem_activity_acc);
  //  notice("socket power        %d watts\n",metric_info.current_socket_power);
}

void gpu_smi_initialize(void){
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

void gpu_smi_finalize(void){
  int status;
  status = amdsmi_shut_down();
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to shut down amdsmi\n");
}

#if TEST_GPU_SMI
int main(void){
  struct gpu_smi_data qd;
  gpu_smi_initialize();

  gpu_smi_query(&qd);
  notice("temperature = %dC\n",qd.temperature);
  notice("gfx         = %d%%\n",qd.gfx_activity);
  notice("umc         = %d%%\n",qd.umc_activity);
  notice("mm          = %d%%\n",qd.mm_activity);

  gpu_smi_finalize();
}
#endif
#endif
