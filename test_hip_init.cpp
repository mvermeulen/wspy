#include <iostream>
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

int main() {
    std::cout << "Starting HIP Init test..." << std::endl;
    int count = 0;
    hipError_t err = hipGetDeviceCount(&count);
    if (err != hipSuccess) {
        std::cerr << "hipGetDeviceCount failed: " << err << std::endl;
        return 1;
    }
    std::cout << "HIP device count: " << count << std::endl;
    return 0;
}
