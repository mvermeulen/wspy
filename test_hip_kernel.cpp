#include <hip/hip_runtime.h>
#include <iostream>

__global__ void test_kernel(int* data) {
    int idx = threadIdx.x + blockIdx.x * blockDim.x;
    data[idx] = idx * 2;
}

int main() {
    std::cout << "Starting HIP test..." << std::endl;
    int* d_data;
    hipMalloc(&d_data, 256 * sizeof(int));
    test_kernel<<<1, 256>>>(d_data);
    hipDeviceSynchronize();
    hipFree(d_data);
    std::cout << "HIP test completed." << std::endl;
    return 0;
}
