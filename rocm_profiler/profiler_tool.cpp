#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <atomic>
#include <mutex>
#include <map>

// Define HIP platform before including HIP headers
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

#include <rocprofiler-sdk/rocprofiler.h>
#include <rocprofiler-sdk/registration.h>

// Global variables
static rocprofiler_context_id_t context_id;
static std::mutex print_mutex;

// Helper to check status
#define CHECK_ROCPROFILER(status) \
    if (status != ROCPROFILER_STATUS_SUCCESS) { \
        std::cerr << "ROCProfiler error: " << status << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    }

// Callback for kernel dispatch tracing
void kernel_dispatch_callback(rocprofiler_callback_tracing_record_t record,
                              rocprofiler_user_data_t* user_data,
                              void* callback_data) {
    if (record.kind == ROCPROFILER_CALLBACK_TRACING_KERNEL_DISPATCH) {
        rocprofiler_callback_tracing_kernel_dispatch_data_t* data = 
            static_cast<rocprofiler_callback_tracing_kernel_dispatch_data_t*>(record.payload);
        
        if (record.phase == ROCPROFILER_CALLBACK_PHASE_EXIT) {
             std::lock_guard<std::mutex> lock(print_mutex);
             std::cout << "[ROCm Trace] Kernel Dispatch: " 
                       << "Dispatch ID: " << data->dispatch_info.dispatch_id 
                       << ", Queue ID: " << data->dispatch_info.queue_id.handle
                       << ", Kernel ID: " << data->dispatch_info.kernel_id
                       << std::endl;
        }
    }
}

// Tool initialization callback
int tool_init(rocprofiler_client_finalize_t fini_func, void* tool_data) {
    std::cout << "[ROCm Trace] Tool Initialized" << std::endl;
    // Create a context
    CHECK_ROCPROFILER(rocprofiler_create_context(&context_id));

    // Configure tracing for kernel dispatches
    CHECK_ROCPROFILER(rocprofiler_configure_callback_tracing_service(
        context_id,
        ROCPROFILER_CALLBACK_TRACING_KERNEL_DISPATCH,
        nullptr, 0, 
        kernel_dispatch_callback,
        nullptr
    ));

    // Start the context
    CHECK_ROCPROFILER(rocprofiler_start_context(context_id));
    
    return 0;
}

// Tool finalization callback
void tool_fini(void* tool_data) {
    // Cleanup if necessary
}

// Tool configuration entry point
extern "C" rocprofiler_tool_configure_result_t* rocprofiler_configure(
    uint32_t version,
    const char* runtime_version,
    uint32_t priority,
    rocprofiler_client_id_t* client_id) {

    // Set the client name
    client_id->name = "wspy_profiler_tool";

    // Return configuration struct
    static rocprofiler_tool_configure_result_t result = {
        sizeof(rocprofiler_tool_configure_result_t),
        tool_init,
        tool_fini,
        nullptr
    };
    return &result;
}
