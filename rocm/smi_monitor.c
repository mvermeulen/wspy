#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include "amd_smi/amdsmi.h"

// Helper function for robust error handling
// All amdsmi_status_t return codes should be checked.
void check_status(amdsmi_status_t status, const std::string& msg) {
    if (status!= AMDSMI_STATUS_SUCCESS) {
        throw std::runtime_error(msg + " | AMDSMI Error: " + std::to_string(status));
    }
}

int main() {
    amdsmi_status_t ret;

    try {
        // 1. API Lifecycle (Init): Initialize the AMD SMI library.
        // We pass 0 (default flags) to initialize for all supported devices.
        // [4, 13]
        ret = amdsmi_init(0);
        check_status(ret, "Failed to initialize AMDSMI");

        // 2. Discovery (Sockets): Get the count and handles for all sockets.
        // [4, 13]
        uint32_t socket_count = 0;
        ret = amdsmi_get_socket_handles(&socket_count, nullptr);
        check_status(ret, "Failed to get socket count");

        if (socket_count == 0) {
            std::cout << "No AMD sockets found on this system." << std::endl;
            amdsmi_shut_down(); // Ensure shutdown on early exit
            return 0;
        }

        std::vector<amdsmi_socket_handle> sockets(socket_count);
        ret = amdsmi_get_socket_handles(&socket_count, sockets.data());
        check_status(ret, "Failed to get socket handles");

        std::cout << "Found " << socket_count << " sockets." << std::endl;
        std::cout << "-----------------------------------" << std::endl;

        // Outer loop: Iterate through each discovered socket
        for (uint32_t i = 0; i < socket_count; ++i) {
            std::cout << "Querying Socket " << i << "..." << std::endl;

            // 3. Discovery (Processors): Get count and handles for all
            //    processors *within this socket*. [4, 5, 13]
            uint32_t proc_count = 0;
            ret = amdsmi_get_processor_handles(sockets[i], &proc_count, nullptr);
            check_status(ret, "Failed to get processor count for socket");

            if (proc_count == 0) {
                std::cout << "\tNo processors found on this socket." << std::endl;
                continue;
            }

            std::vector<amdsmi_processor_handle> processors(proc_count);
            ret = amdsmi_get_processor_handles(sockets[i], &proc_count, processors.data());
            check_status(ret, "Failed to get processor handles for socket");

            // Inner loop: Iterate through each processor on this socket
            for (uint32_t j = 0; j < proc_count; ++j) {
                
                // 4. Identification (Type): Get the processor type. 
                processor_type_t proc_type;
                ret = amdsmi_get_processor_type(processors[j], &proc_type);
                check_status(ret, "Failed to get processor type");

                // We are only interested in GPU activity.
                // AMDSMI_PROCESSOR_TYPE_AMD_GPU is typically 1. 
                if (proc_type!= AMDSMI_PROCESSOR_TYPE_AMD_GPU) {
                    std::cout << "\tFound Processor " << j << " (Type: CPU). Skipping." << std::endl;
                    continue;
                }
                
                // 5. Identification (Name): Get board info for a human-readable name.
                // This is the best way to see if it's an APU iGPU or a dGPU.
                // [5, 9, 19, 20]
                amdsmi_board_info_t board_info;
                ret = amdsmi_get_gpu_board_info(processors[j], &board_info);
                if (ret!= AMDSMI_STATUS_SUCCESS) {
                    std::cout << "\tFound Processor " << j << " (Type: GPU). Failed to get board info." << std::endl;
                    continue;
                }
                
                // Example product_name: "Radeon RX 7900 XTX" (dGPU)
                // Example product_name: "Ryzen 9 7950X with Radeon Graphics" (APU)
                std::cout << "\tFound Processor " << j << " (Type: GPU)" << std::endl;
                std::cout << "\tDevice Name: " << board_info.product_name << std::endl;


                // 6. Polling "Busyness" (Activity): Get the engine usage vector.
                // [8, 9, 11]
                amdsmi_engine_usage_t usage;
                ret = amdsmi_get_gpu_activity(processors[j], &usage);
                check_status(ret, "Failed to get GPU activity");

                // Print the core "busyness" metrics.
                std::cout << "\t " << std::endl;
                std::cout << "\t  - GFX (Compute) : " << usage.gfx_activity << "%" << std::endl;
                std::cout << "\t  - Media (VCN)   : " << usage.mm_activity << "%" << std::endl;
                std::cout << "\t  - Memory (UMC)  : " << usage.umc_activity << "%" << std::endl;
                std::cout << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    // 7. API Lifecycle (Shutdown): Release all library resources.
    // This must be called, even if an error occurred. [4, 13]
    ret = amdsmi_shut_down();
    if (ret!= AMDSMI_STATUS_SUCCESS) {
        std::cerr << "Failed to shut down AMDSMI cleanly." << std::endl;
    }

    return 0;
}
