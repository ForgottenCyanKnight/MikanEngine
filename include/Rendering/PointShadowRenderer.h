#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>

//  - 单张 cubemap 数组（MAX_SHADOW_LIGHTS 个光源 × 6 面，D16_UNORM 256²）——VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
//  - 每光源 6 面 90° view-proj（CPU 每帧算，far = range）
//  - 渲染：SceneRenderer 收集几何 → 对每光源×面 BeginFace/EndFace 提交 depth-only（线性深度 dist/range）
//  - 采样：合成 pass samplerCubeArray + PCF（fullscreen.frag binding 13）
class MIKAN_API PointShadowRenderer {
public:
    static constexpr int MAX_SHADOW_LIGHTS = 8;    static constexpr int SHADOW_MAP_SIZE = 256;
    static constexpr float NEAR_PLANE = 0.1f;

    ~PointShadowRenderer() { Cleanup(); }

    // 创建 cube array + face views + depth-only render pass + framebuffers（幂等）
    bool Init();
    void Cleanup();

    // 每帧：给定光源（世界位置 + range）计算 6 面 view-proj（far = range）
    void UpdateMatrices(const glm::vec3* lightPositions, const float* lightRanges, int lightCount);

    // 每光源×面渲染：begin depth-only render pass（face framebuffer）+ viewport/scissor
    void BeginFace(VkCommandBuffer cmd, int lightIndex, int face, int width, int height);
    void EndFace(VkCommandBuffer cmd);

    // 全部面渲染完后：cube array → SHADER_READ_ONLY + 深度采样 barrier
    void Finalize(VkCommandBuffer cmd);

    VkRenderPass GetRenderPass() const { return m_RenderPass; }
    VkImageView GetCubeArrayView() const { return m_CubeArrayView; }
    const glm::mat4& GetFaceProjView(int lightIndex, int face) const { return m_FaceProjView[lightIndex * 6 + face]; }
    bool IsInitialized() const { return m_Initialized; }

private:
    void CreateFaceView(int face);

    VkImage m_Image = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    VkImageView m_CubeArrayView = VK_NULL_HANDLE;                       // CUBE_ARRAY（采样用）
    VkImageView m_FaceViews[MAX_SHADOW_LIGHTS * 6] = {};                // 2D face views（framebuffer 附件）
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_FrameBuffers[MAX_SHADOW_LIGHTS * 6] = {};
    std::array<glm::mat4, MAX_SHADOW_LIGHTS * 6> m_FaceProjView;        // 每光源 6 面
    int m_LightCount = 0;
    bool m_Initialized = false;
};
