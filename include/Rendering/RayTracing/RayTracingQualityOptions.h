#pragma once
#include "Core/VulkanContext.h"
#include <cstdlib>
#include "Platform/Export.h"

namespace mikan::rt {
// Shared Game.dll state; do not put a mutable function-static in this header,
// because Editor.dll must select the same tier as the rendering runtime.
MIKAN_API uint32_t GetNeeDiffuseSamples();
MIKAN_API uint32_t GetNeeDirectSamples();
MIKAN_API void SetNeeDiffuseSamples(uint32_t samples);
MIKAN_API bool GetRestirPathTracing();
MIKAN_API void SetRestirPathTracing(bool enabled);
MIKAN_API bool GetRestirTemporalReuse();
MIKAN_API void SetRestirTemporalReuse(bool enabled);
inline bool UseFreshDiffuseExperiment() {
    static const bool enabled=[] {
        const char* value=std::getenv("MIKAN_HWRT_FRESH_DIFFUSE_2SPP");
        return !value||value[0]!='0';
    }();
    return enabled;
}
// Shared policy for allocation, shader flags, and dispatch. Explicit SDK/RR
// choices take precedence over automatic quality mode; the master switch
// remains an exact way to restore the smooth ordinary NEE/original GI path.
inline bool RestirSpatialSupported() {
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_HWRT_UNBIASED_RESTIR");
        if (value) return value[0] == '1';
        const char* sdk = std::getenv("MIKAN_HWRT_RTXDI");
        const char* rr = std::getenv("MIKAN_HWRT_DLSS_RR");
        const char* reference = std::getenv("MIKAN_HWRT_REFERENCE_SPP");
        if ((sdk && sdk[0] == '1') || (rr && rr[0] == '0') ||
            (reference && std::atoi(reference) > 0)) return false;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);
        return properties.vendorID == 0x10DE;
    }();
    return enabled;
}
inline bool UseUnbiasedSpatialRestir() {
    if(UseFreshDiffuseExperiment())return false;
    return RestirSpatialSupported();
}
MIKAN_API bool GetRestirEstimatorRestir();
MIKAN_API void SetRestirEstimatorRestir(bool restir);
}
