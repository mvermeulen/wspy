/*
 * gpu_info.c - get information on AMD ROCm GPU using libamdsmi
 */
#include <stdio.h>
#include <stdlib.h>
#include "amd_smi/amdsmi.h"
#include "gpu_info.h"
#include "error.h"

amdsmi_socket_handle *sockets;
amdsmi_processor_handle *processor_handles;

int num_gpu = 0;
void **gpu_handles;

void gpu_info_query(void){
  amdsmi_gpu_metrics_t metric_info;
  int status;
  
  if (num_gpu > 0)
    status = amdsmi_get_gpu_metrics_info(gpu_handles[0],&metric_info);
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to get gpu metrics\n");
  
  notice("temperature         %dC\n",metric_info.temperature_edge);
  notice("gfx activity        %d\n",metric_info.average_gfx_activity);
  notice("umc activity        %d\n",metric_info.average_umc_activity);
  notice("mm activity         %d\n",metric_info.average_mm_activity);
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
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to initialize amdsmi\n");

  // get the socket count and allocate memory
  status = amdsmi_get_socket_handles(&socket_count,NULL);
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to get amdsmi socket handles\n");

  sockets = calloc(socket_count,sizeof(amdsmi_socket_handle));
  
  // fill in the socket handles
  status = amdsmi_get_socket_handles(&socket_count,sockets);
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to get amdsmi socket handles\n");
  
  // for each socket, get identifier and devices
  for (i=0;i<socket_count;i++){
    char socket_info[128];
    status = amdsmi_get_socket_info(sockets[i],sizeof(socket_info),socket_info);
    if (status != AMDSMI_STATUS_SUCCESS)
      fatal("unable to get amdsmi socket info\n");

    // get the device count for the socket and allocate memory
    status = amdsmi_get_processor_handles(sockets[i],&device_count,NULL);
    if (status != AMDSMI_STATUS_SUCCESS)
      fatal("unable to get amdsmi device count\n");

    if (num_gpu == 0){
      gpu_handles = malloc(device_count * sizeof(gpu_handles[0]));
    } else {
      gpu_handles = realloc(gpu_handles,(device_count+num_gpu)*sizeof(gpu_handles[0]));
    }

    //    processor_handles = calloc(device_count,sizeof(amdsmi_processor_handle));

    // get the devices of the socket
    status = amdsmi_get_processor_handles(sockets[i],&device_count,/*processor_handles*/&gpu_handles[num_gpu]);
    if (status != AMDSMI_STATUS_SUCCESS)
      fatal("unable to get amdsmi device count\n");
    num_gpu += device_count;
  }
}

void gpu_info_finalize(void){
  int status;
  status = amdsmi_shut_down();
  if (status != AMDSMI_STATUS_SUCCESS)
    fatal("unable to shut down amdsmi\n");
}

#if TEST_GPU_INFO
int main(void){
  gpu_info_initialize();
  gpu_info_query();
  gpu_info_finalize();
}
#endif
