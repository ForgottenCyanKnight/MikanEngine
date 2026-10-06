#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// Shared, linear-light sky inputs. The panorama is LogLuv32; SH stores irradiance.
struct RayTracingEnvironment {
    VkImageView panorama = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer irradiance = VK_NULL_HANDLE;
    VkImageView transmittance = VK_NULL_HANDLE;
    glm::vec4 tintIntensity = glm::vec4(1.0f);
    float altitudeMeters = 200.0f;
    bool IsValid() const { return panorama && sampler && irradiance; }
};
