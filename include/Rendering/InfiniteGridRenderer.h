#pragma once

#include "Platform/Export.h"
#include "Rendering/RendererBase.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

// Procedural SceneView grid rendered by a full-screen GPU pass.
//
// The CPU only updates the camera/settings UBO and submits one triangle. The
// fragment shader reconstructs a camera ray, intersects it with the XZ plane,
// and evaluates the grid analytically. This keeps the grid effectively
// infinite and gives it the same smooth distance/angle transition as the
// Hazel/Fermion-style implementation.
class MIKAN_API InfiniteGridRenderer {
public:
    struct Settings {
        // Skips the whole grid pass. The origin X/Z/Y axis lines are drawn by
        // the same fragment shader, so they share the grid's visibility.
        bool enabled = true;
        float gridScale = 1.0f;
        float fadeDistance = 500.0f;
        float axisLength = 3.0f;
        glm::vec4 gridColorThin = glm::vec4(0.5f, 0.5f, 0.5f, 0.40f);
        glm::vec4 gridColorThick = glm::vec4(0.5f, 0.5f, 0.5f, 0.60f);
        glm::vec4 axisColorX = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        glm::vec4 axisColorZ = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        glm::vec4 axisColorY = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    };

    InfiniteGridRenderer() = default;
    ~InfiniteGridRenderer() { Cleanup(); }

    // renderPass must be the one-subpass SceneRT composite pass, whose
    // framebuffer contains the HDR color attachment and read-only scene depth.
    void Init(VkRenderPass renderPass);
    void Cleanup();

    void Render(VkCommandBuffer commandBuffer,
                uint32_t width,
                uint32_t height,
                const glm::mat4& view,
                const glm::mat4& projection,
                const Settings& settings = Settings{});

    bool IsInitialized() const { return m_Initialized; }
    VkRenderPass GetRenderPass() const { return m_RenderPass; }

private:
    // Keep this layout vec4-only after the matrices so it matches std140 on
    // all desktop and mobile compilers without relying on vec3 padding.
    struct UniformData {
        glm::mat4 viewProjection = glm::mat4(1.0f);
        glm::mat4 inverseViewProjection = glm::mat4(1.0f);
        glm::vec4 cameraPosition = glm::vec4(0.0f);
        glm::vec4 gridParams = glm::vec4(1.0f, 500.0f, 3.0f, 0.0f);
        glm::vec4 viewport = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        glm::vec4 gridColorThin = glm::vec4(0.5f, 0.5f, 0.5f, 0.40f);
        glm::vec4 gridColorThick = glm::vec4(0.5f, 0.5f, 0.5f, 0.60f);
        glm::vec4 axisColorX = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        glm::vec4 axisColorZ = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        glm::vec4 axisColorY = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    };

    static_assert(sizeof(UniformData) % 16 == 0,
                  "Infinite grid UBO must remain std140 aligned");

    VulkanBuffer m_UniformBuffer;
    VulkanDescriptor m_Descriptor;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VulkanPipeline m_Pipeline;
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    bool m_Initialized = false;
};
