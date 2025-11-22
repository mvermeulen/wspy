#include <iostream>
#include <vector>
#include "amd_smi/amdsmi.h"

int main() {
  amdsmi_status_t ret;

  // Init amdsmi for sockets and devices. Here we are only interested in AMD_GPUS.
  ret = amdsmi_init(AMDSMI_INIT_AMD_GPUS);

  // Get all sockets
  uint32_t socket_count = 0;

  // Get the socket count available in the system.
  ret = amdsmi_get_socket_handles(&socket_count, nullptr);

  // Allocate the memory for the sockets
  std::vector<amdsmi_socket_handle> sockets(socket_count);
  // Get the socket handles in the system
  ret = amdsmi_get_socket_handles(&socket_count, &sockets[0]);

  std::cout << "Total Socket: " << socket_count << std::endl;

  // For each socket, get identifier and devices
  for (uint32_t i=0; i < socket_count; i++) {
    // Get Socket info
    char socket_info[128];
    ret = amdsmi_get_socket_info(sockets[i], 128, socket_info);
    std::cout << "Socket " << socket_info<< std::endl;

    // Get the device count for the socket.
    uint32_t device_count = 0;
    ret = amdsmi_get_processor_handles(sockets[i], &device_count, nullptr);

    // Allocate the memory for the device handlers on the socket
    std::vector<amdsmi_processor_handle> processor_handles(device_count);
    // Get all devices of the socket
    ret = amdsmi_get_processor_handles(sockets[i],
				       &device_count, &processor_handles[0]);

    // For each device of the socket, get name and temperature.
    for (uint32_t j=0; j < device_count; j++) {
      // Get device type. Since the amdsmi is initialized with
      // AMD_SMI_INIT_AMD_GPUS, the processor_type must be AMD_GPU.
      processor_type_t processor_type;
      ret = amdsmi_get_processor_type(processor_handles[j], &processor_type);
      if (processor_type != AMDSMI_PROCESSOR_TYPE_AMD_GPU) {
	std::cout << "Expect AMD_GPU device type!\n";
	return 1;
      }

      // Get device name
      amdsmi_board_info_t board_info;
      ret = amdsmi_get_gpu_board_info(processor_handles[j], &board_info);
      std::cout << "\tdevice "
		<< j <<"\n\t\tName:" << board_info.product_name << std::endl;

      // Get temperature
      int64_t val_i64 = 0;
      ret =  amdsmi_get_temp_metric(processor_handles[j], AMDSMI_TEMPERATURE_TYPE_EDGE,
				    AMDSMI_TEMP_CURRENT, &val_i64);
      std::cout << "\t\tTemperature: " << val_i64 << "C" << std::endl;
    }
  }

  // Clean up resources allocated at amdsmi_init. It will invalidate sockets
  // and devices pointers
  ret = amdsmi_shut_down();

  return 0;
}
