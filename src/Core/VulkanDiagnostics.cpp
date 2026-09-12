#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanManager.h"

#include <cstdio>

// check_vk_result 函数实现
void check_vk_result(VkResult err)
{
    if (err == 0) {
        return;
    }
    
    const char* error_text = "";
    const char* error_description = "";
    switch (err) {
        case VK_NOT_READY: 
            error_text = "VK_NOT_READY";
            error_description = "Operation is not ready to complete";
            break;
        case VK_TIMEOUT: 
            error_text = "VK_TIMEOUT";
            error_description = "Operation timed out";
            break;
        case VK_EVENT_SET: 
            error_text = "VK_EVENT_SET";
            error_description = "Event is set";
            break;
        case VK_EVENT_RESET: 
            error_text = "VK_EVENT_RESET";
            error_description = "Event is reset";
            break;
        case VK_INCOMPLETE: 
            error_text = "VK_INCOMPLETE";
            error_description = "Operation completed but not all data was returned";
            break;
        case VK_ERROR_OUT_OF_HOST_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_HOST_MEMORY";
            error_description = "Out of host memory";
            break;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            error_description = "Out of device memory";
            break;
        case VK_ERROR_INITIALIZATION_FAILED: 
            error_text = "VK_ERROR_INITIALIZATION_FAILED";
            error_description = "Initialization failed";
            break;
        case VK_ERROR_DEVICE_LOST: 
            error_text = "VK_ERROR_DEVICE_LOST";
            error_description = "Device lost - likely due to invalid operation, memory access, or driver crash";
            break;
        case VK_ERROR_MEMORY_MAP_FAILED: 
            error_text = "VK_ERROR_MEMORY_MAP_FAILED";
            error_description = "Memory map failed";
            break;
        case VK_ERROR_LAYER_NOT_PRESENT: 
            error_text = "VK_ERROR_LAYER_NOT_PRESENT";
            error_description = "Requested layer not present";
            break;
        case VK_ERROR_EXTENSION_NOT_PRESENT: 
            error_text = "VK_ERROR_EXTENSION_NOT_PRESENT";
            error_description = "Requested extension not present";
            break;
        case VK_ERROR_FEATURE_NOT_PRESENT: 
            error_text = "VK_ERROR_FEATURE_NOT_PRESENT";
            error_description = "Requested feature not present";
            break;
        case VK_ERROR_INCOMPATIBLE_DRIVER: 
            error_text = "VK_ERROR_INCOMPATIBLE_DRIVER";
            error_description = "Incompatible driver";
            break;
        case VK_ERROR_TOO_MANY_OBJECTS: 
            error_text = "VK_ERROR_TOO_MANY_OBJECTS";
            error_description = "Too many objects";
            break;
        case VK_ERROR_FORMAT_NOT_SUPPORTED: 
            error_text = "VK_ERROR_FORMAT_NOT_SUPPORTED";
            error_description = "Format not supported";
            break;
        case VK_ERROR_FRAGMENTED_POOL: 
            error_text = "VK_ERROR_FRAGMENTED_POOL";
            error_description = "Pool is fragmented";
            break;
        case VK_ERROR_UNKNOWN: 
            error_text = "VK_ERROR_UNKNOWN";
            error_description = "Unknown error";
            break;
        case VK_ERROR_OUT_OF_POOL_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_POOL_MEMORY";
            error_description = "Out of pool memory";
            break;
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: 
            error_text = "VK_ERROR_INVALID_EXTERNAL_HANDLE";
            error_description = "Invalid external handle";
            break;
        case VK_ERROR_FRAGMENTATION: 
            error_text = "VK_ERROR_FRAGMENTATION";
            error_description = "Fragmentation";
            break;
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: 
            error_text = "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
            error_description = "Invalid opaque capture address";
            break;
        case VK_ERROR_SURFACE_LOST_KHR: 
            error_text = "VK_ERROR_SURFACE_LOST_KHR";
            error_description = "Surface lost";
            break;
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: 
            error_text = "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            error_description = "Native window in use";
            break;
        case VK_SUBOPTIMAL_KHR: 
            error_text = "VK_SUBOPTIMAL_KHR";
            error_description = "Suboptimal";
            break;
        case VK_ERROR_OUT_OF_DATE_KHR: 
            error_text = "VK_ERROR_OUT_OF_DATE_KHR";
            error_description = "Out of date";
            break;
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: 
            error_text = "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
            error_description = "Incompatible display";
            break;
        case VK_ERROR_VALIDATION_FAILED_EXT: 
            error_text = "VK_ERROR_VALIDATION_FAILED_EXT";
            error_description = "Validation failed";
            break;
        case VK_ERROR_INVALID_SHADER_NV: 
            error_text = "VK_ERROR_INVALID_SHADER_NV";
            error_description = "Invalid shader";
            break;
        case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR";
            error_description = "Image usage not supported";
            break;
        case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
            error_description = "Video picture layout not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
            error_description = "Video profile operation not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
            error_description = "Video profile format not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
            error_description = "Video profile codec not supported";
            break;
        case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
            error_description = "Video std version not supported";
            break;
        case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT: 
            error_text = "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
            error_description = "Invalid DRM format modifier plane layout";
            break;
        case VK_ERROR_NOT_PERMITTED_KHR: 
            error_text = "VK_ERROR_NOT_PERMITTED_KHR";
            error_description = "Not permitted";
            break;
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: 
            error_text = "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
            error_description = "Full screen exclusive mode lost";
            break;
        default: 
            error_text = "UNKNOWN_ERROR";
            error_description = "Unknown error";
            break;
    }
    
    fprintf(stderr, "[vulkan] Error: VkResult = %d (%s) - %s\n", static_cast<int>(err), error_text, error_description);
    if (err == VK_ERROR_VALIDATION_FAILED_EXT || err == VK_ERROR_UNKNOWN) {
        // 断言失败，可以在此处设置断点
        #ifdef _DEBUG
        __debugbreak();
        #endif
    }
}


