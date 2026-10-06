#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>

// Engine-facing contract. No vendor SDK types or encodings cross this boundary.
namespace mikan::denoising {
enum class Backend { None, Nrd };
// Both signals share guide/record/output plumbing, with separate backend histories.
enum class Signal { Diffuse, Specular };
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
struct Device {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice logical = VK_NULL_HANDLE;
    const VkAllocationCallbacks* allocator = nullptr;
    uint32_t queueFamily = 0;
    uint32_t framesInFlight = 1;
};
struct Frame {
    glm::mat4 view{1.0f}, projection{1.0f}; // Non-jittered, Vulkan clip depth [0,1].
    glm::vec2 jitterNdc{0.0f};
    VkExtent2D sourceExtent{};
    uint32_t frameSlot = 0; // Caller has waited for this slot's fence.
    bool resetHistory = false;
    // 0 = normalized guide resampling; >0 = guides at pixel*stride + 0.5.
    uint32_t guidePixelStride = 0;
};
struct DiffuseInputs {
    // RGB = demodulated diffuse or specular illumination in linear HDR; A = distance along
    // sampled rays in world units. Miss uses 65504, not zero (unsampled).
    Texture radianceHitDistance;
    Texture depth;          // NDC depth, sky=1; sampled at sourceExtent.
    Texture worldNormal;    // Engine octahedral normals in RG (signed [-1,1]).
    Texture motion;         // Non-jittered UV delta: currentUV - previousUV.
    bool linearViewDepth = false; // RT may supply positive viewZ directly (sky >= 1e6).
    bool unpackedWorldNormal = false; // RGB world normal, A linear material roughness (diffuse backend uses 1).
};
class IDiffuseDenoiser {
public:
    virtual ~IDiffuseDenoiser() = default;
    virtual bool Initialize(const Device&, VkExtent2D signalExtent) = 0;
    // Must run outside a render pass, on the same queue as the producer.
    virtual bool Record(VkCommandBuffer, const Frame&, const DiffuseInputs&) = 0;
    virtual Texture Output() const = 0; // Linear RGB, ready for fragment sampling.
    virtual void ResetHistory() = 0;
};
std::unique_ptr<IDiffuseDenoiser> CreateDiffuseDenoiser(Backend backend, Signal signal = Signal::Diffuse);
// Called before vkCreateDevice; enables supported core features used by backends.
void EnableDenoiserDeviceFeatures(const VkPhysicalDeviceFeatures& available,
                                 VkPhysicalDeviceFeatures& requested);
} // namespace mikan::denoising
