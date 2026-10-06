#pragma once

#include "Rendering/Denoising/DiffuseDenoiser.h"
#include <span>
#include <string>
#include <vector>

namespace mikan::denoising {
// Called after physical-device selection, before vkCreateDevice. Non-NVIDIA
// adapters return immediately, without calling NGX or loading feature DLLs.
std::vector<std::string> ProbeDlssRayReconstruction(
    VkInstance, VkPhysicalDevice, std::span<const char* const> enabledInstanceExtensions,
    std::span<const VkExtensionProperties> availableDeviceExtensions);
bool IsDlssRayReconstructionSupported(VkPhysicalDevice);

struct RayReconstructionFrame {
    glm::mat4 view{1}, projection{1};
    glm::vec2 jitterUV{0}; // Ray sample offset; excludes jitter from motion vectors.
    VkDescriptorSet rayTracingSet = VK_NULL_HANDLE;
    bool resetHistory = false;
    uint64_t captureSerial = UINT64_MAX;
    uint32_t captureViewSlot = 0;
};
struct RayReconstructionInputs {
    Texture baseColor, diffuse, diffuseMaterial, specular, specularMaterial, primaryMotionDepth;
    Texture virtualMotion, virtualDepth, virtualNormal; // SR mirror guides; optional for RR.
};
class IRayReconstruction {
public:
    virtual ~IRayReconstruction() = default;
    virtual bool Initialize(VkInstance, const Device&, VkExtent2D, VkDescriptorSetLayout rayTracingLayout, VkExtent2D outputExtent = {}) = 0;
    // Same serialized offscreen queue as the producer; no render pass may be active.
    // Replaces NRD and TAA, producing native-resolution, linear HDR. False means
    // the caller must use its existing NRD/TAA path for this frame.
    virtual bool Record(VkCommandBuffer, const RayReconstructionFrame&, const RayReconstructionInputs&) = 0;
    virtual Texture Output() const = 0;
    virtual void ResetHistory() = 0;
};
std::unique_ptr<IRayReconstruction> CreateDlssRayReconstruction();
std::unique_ptr<IRayReconstruction> CreateDlssSuperResolution();
VkExtent2D ConfigureDlssSuperResolution(uint32_t viewSlot, VkExtent2D nativeExtent);
VkExtent2D GetDlssOutputResolution(uint32_t viewSlot, VkExtent2D inputExtent);
void ShutdownDlssDevice(VkDevice device);
} // namespace mikan::denoising
