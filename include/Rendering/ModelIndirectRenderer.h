#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <glm/glm.hpp>
class SceneRenderer;
class ModelRenderer;
struct RenderFrameContext;
class MIKAN_API ModelIndirectRenderer {
public:
    static VkDescriptorSetLayout GetInstanceLayout();
    static void Prepare(SceneRenderer&, VkCommandBuffer, int width, int height,
                        const glm::mat4& view, const glm::mat4& proj, int viewSlot);
    static void Render(SceneRenderer&, RenderFrameContext&);
    static bool Handles(ModelRenderer*, bool doubleSided, int viewSlot, uint64_t epoch);
    static void Cleanup();
};
