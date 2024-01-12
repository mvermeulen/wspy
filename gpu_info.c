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
  notice("%u amdsmi socket handles\n",socket_count);

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

    processor_handles = calloc(device_count,sizeof(amdsmi_processor_handle));

    // get the devices of the socket
    status = amdsmi_get_processor_handles(sockets[i],&device_count,processor_handles);
    if (status != AMDSMI_STATUS_SUCCESS)
      fatal("unable to get amdsmi device count\n");

    // for each device, get the name and temperature
    for (j=0;j<device_count;j++){

      // get device type, expect this to be AMD_GPU
      processor_type_t processor_type;
      status = amdsmi_get_processor_type(processor_handles[j],&processor_type);
      if (status != AMDSMI_STATUS_SUCCESS)
	fatal("unable to get processor type\n");
      if (processor_type != AMD_GPU)
	fatal("expect processor type AMD_GPU\n");

      // get device name
      amdsmi_board_info_t board_info;
      status = amdsmi_get_gpu_board_info(processor_handles[j],&board_info);
      if (status != AMDSMI_STATUS_SUCCESS)
	fatal("unable to get gpu board info\n");      
      notice("device %d name %s\n",j,board_info.product_name);

      // get temperature
      status = amdsmi_get_temp_metric(processor_handles[j],TEMPERATURE_TYPE_EDGE,
				       AMDSMI_TEMP_CURRENT,&value);
      if (status != AMDSMI_STATUS_SUCCESS)
	fatal("unable to get gpu temp metric\n");      
      notice("device %d temperature %luC\n",j,value);
    }
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
  gpu_info_finalize();
}
#endif
