// Renderer2D.cpp - 2D 渲染核心实现（图元合批 + 纹理描述符管理）
#include "Rendering/Renderer2D.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/RuntimeCapabilities.h"
#include "Core/VulkanManager.h"
#include "Rendering/TexturePool.h"
#include "Rendering/RendererBase.h"
#include <algorithm>
#include <cstring>
#include <iostream>

extern TexturePool* g_TexturePool;

namespace {
    constexpr uint32_t VERTEX_STRIDE = sizeof(Vertex2D);
    constexpr uint32_t QUAD_VERTICES = 6;  // 2 个三角形展开（非索引）
}

Renderer2D& Renderer2D::GetInstance() {
    static Renderer2D instance;
    return instance;
}

bool Renderer2D::Init(VkRenderPass offscreenPass, VkRenderPass overlayPass, VkRenderPass displayUIPass, VkRenderPass swapchainUIPass) {
    if (offscreenPass == VK_NULL_HANDLE || overlayPass == VK_NULL_HANDLE) return false;
    if (m_Pipeline.GetPipeline() != VK_NULL_HANDLE && m_OverlayPipeline.GetPipeline() != VK_NULL_HANDLE) {
        if (m_OffscreenPass == offscreenPass && m_OverlayPass == overlayPass) return true;  // 幂等
        Cleanup();  // render pass 重建（swapchain/离屏目标），管线需重建
    }
    m_OffscreenPass = offscreenPass;
    m_OverlayPass = overlayPass;
    m_DisplayUIPass = displayUIPass;
    m_SwapchainUIPass = swapchainUIPass;

    // 1. 纹理描述符布局（1 个 COMBINED_IMAGE_SAMPLER）
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_TextureLayout) != VK_SUCCESS) {
        std::cerr << "[Renderer2D] Failed to create descriptor set layout" << std::endl;
        return false;
    }

    // 2. 描述符池（2D 纹理上限）
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_2D_TEXTURES;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_2D_TEXTURES;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_TexturePoolHandle) != VK_SUCCESS) {
        std::cerr << "[Renderer2D] Failed to create descriptor pool" << std::endl;
        return false;
    }

    // 3. 白色 1x1 纹理（无纹理时的默认采样）
    if (!CreateWhiteTexture()) {
        std::cerr << "[Renderer2D] Failed to create white texture" << std::endl;
        return false;
    }

    // 4. 顶点缓冲（固定容量：MAX_QUADS * 6 顶点 * 32B = 3MB）
    const VkDeviceSize bufferSize = (VkDeviceSize)MAX_QUADS * QUAD_VERTICES * VERTEX_STRIDE;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (!m_VertexBuffers[frame].Create(bufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            std::cerr << "[Renderer2D] Failed to create vertex buffer" << std::endl;
            return false;
        }
        m_VertexBuffers[frame].Map();  // 显式映射：Flush 直接写 GetMappedPtr()
        // SceneView 专用缓冲：避免同一帧不同 render pass 的 UI 顶点相互覆盖。
        if (!m_VertexBuffersSecondary[frame].Create(bufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            std::cerr << "[Renderer2D] Failed to create secondary vertex buffer" << std::endl;
            return false;
        }
        m_VertexBuffersSecondary[frame].Map();
    }
    m_MaxVertices = MAX_QUADS * QUAD_VERTICES;

    // 5. 管线（透明混合、无深度、无剔除、push constant = mat4 viewProj）
    PipelineConfig config;
    config.vertShader = "ui2d.vert.spv";
    config.fragShader = "ui2d.frag.spv";
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = false;
    config.depthWrite = false;
    config.blending = true;
    config.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    config.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    config.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    config.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    config.colorAttachmentCount = 1;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4);

    // 顶点输入：pos(2) + uv(2) + color(4) + screenPxRange(1)，stride 36B
    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = VERTEX_STRIDE;
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[4] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(Vertex2D, pos);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(Vertex2D, uv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[2].offset = offsetof(Vertex2D, color);
    attributes[3].location = 3;
    attributes[3].binding = 0;
    attributes[3].format = VK_FORMAT_R32_SFLOAT;
    attributes[3].offset = offsetof(Vertex2D, screenPxRange);

    config.vertexBindings = { vertexBinding };
    config.vertexAttributes.assign(attributes, attributes + 4);

    // 离屏管线：写深度（UI/世界层深度 0.x，合成时区分天空深度 1.0，避免 UI 被天空覆盖）
    PipelineConfig offscreenConfig = config;
    offscreenConfig.depthWrite = true;
    offscreenConfig.subpass = 1;      // MRT 几何 subpass（0=z-prepass）
    if (!m_Pipeline.Create(offscreenPass, m_TextureLayout, offscreenConfig)) {
        std::cerr << "[Renderer2D] Failed to create offscreen pipeline" << std::endl;
        return false;
    }
    // 主窗口管线：无深度附件，保持 depthWrite=false
    if (!m_OverlayPipeline.Create(overlayPass, m_TextureLayout, config)) {
        std::cerr << "[Renderer2D] Failed to create overlay pipeline" << std::endl;
        return false;
    }

    // SDF 管线（同一布局/顶点输入，frag 换 ui2d_sdf 距离场 shader）
    PipelineConfig offscreenSdfConfig = offscreenConfig;
    offscreenSdfConfig.fragShader = "ui2d_sdf.frag.spv";
    if (!m_SdfPipeline.Create(offscreenPass, m_TextureLayout, offscreenSdfConfig)) {
        std::cerr << "[Renderer2D] Failed to create SDF offscreen pipeline" << std::endl;
        return false;
    }
    PipelineConfig sdfConfig = config;
    sdfConfig.fragShader = "ui2d_sdf.frag.spv";
    if (!m_SdfOverlayPipeline.Create(overlayPass, m_TextureLayout, sdfConfig)) {
        std::cerr << "[Renderer2D] Failed to create SDF overlay pipeline" << std::endl;
        return false;
    }

    // UI 叠加管线（链末 tonemap 后，显示附件 loadOp=LOAD pass）：alpha 混合，无深度写入
    // 与离屏管线同 config（blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA），depth 关闭（不写深度，叠加在链结果之上）
    PipelineConfig displayUIConfig = config;
    displayUIConfig.depthWrite = false;
    displayUIConfig.depthTest = false;
    displayUIConfig.subpass = 0;   // 显示附件 pass 单 subpass
    if (displayUIPass != VK_NULL_HANDLE) {
        if (!m_DisplayUIPipeline.Create(displayUIPass, m_TextureLayout, displayUIConfig)) {
            std::cerr << "[Renderer2D] Failed to create display UI pipeline" << std::endl;
            return false;
        }
        PipelineConfig displayUISdfConfig = displayUIConfig;
        displayUISdfConfig.fragShader = "ui2d_sdf.frag.spv";
        if (!m_DisplayUISdfPipeline.Create(displayUIPass, m_TextureLayout, displayUISdfConfig)) {
            std::cerr << "[Renderer2D] Failed to create display UI SDF pipeline" << std::endl;
            return false;
        }
    }
    // swapchain UI 叠加管线（游戏模式，swapchain loadOp=LOAD pass）
    if (swapchainUIPass != VK_NULL_HANDLE) {
        if (!m_SwapchainUIPipeline.Create(swapchainUIPass, m_TextureLayout, displayUIConfig)) {
            std::cerr << "[Renderer2D] Failed to create swapchain UI pipeline" << std::endl;
            return false;
        }
        PipelineConfig swapchainUISdfConfig = displayUIConfig;
        swapchainUISdfConfig.fragShader = "ui2d_sdf.frag.spv";
        if (!m_SwapchainUISdfPipeline.Create(swapchainUIPass, m_TextureLayout, swapchainUISdfConfig)) {
            std::cerr << "[Renderer2D] Failed to create swapchain UI SDF pipeline" << std::endl;
            return false;
        }
    }

    return true;
}

void Renderer2D::Cleanup() {
    m_Quads.clear();
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (m_VertexBuffers[frame].GetBuffer() != VK_NULL_HANDLE) {
            m_VertexBuffers[frame].Cleanup();
        }
        if (m_VertexBuffersSecondary[frame].GetBuffer() != VK_NULL_HANDLE) {
            m_VertexBuffersSecondary[frame].Cleanup();
        }
    }
    m_Pipeline.Cleanup();
    m_OverlayPipeline.Cleanup();
    m_SdfPipeline.Cleanup();
    m_SdfOverlayPipeline.Cleanup();
    m_DisplayUIPipeline.Cleanup();
    m_DisplayUISdfPipeline.Cleanup();
    m_SwapchainUIPipeline.Cleanup();
    m_SwapchainUISdfPipeline.Cleanup();
    if (m_WhiteSampler) { vkDestroySampler(g_Device, m_WhiteSampler, g_Allocator); m_WhiteSampler = VK_NULL_HANDLE; }
    if (m_WhiteView) { vkDestroyImageView(g_Device, m_WhiteView, g_Allocator); m_WhiteView = VK_NULL_HANDLE; }
    if (m_WhiteImage) { vkDestroyImage(g_Device, m_WhiteImage, g_Allocator); m_WhiteImage = VK_NULL_HANDLE; }
    if (m_WhiteMemory) { vkFreeMemory(g_Device, m_WhiteMemory, g_Allocator); m_WhiteMemory = VK_NULL_HANDLE; }
    if (m_TexturePoolHandle) { vkDestroyDescriptorPool(g_Device, m_TexturePoolHandle, g_Allocator); m_TexturePoolHandle = VK_NULL_HANDLE; }
    if (m_TextureLayout) { vkDestroyDescriptorSetLayout(g_Device, m_TextureLayout, g_Allocator); m_TextureLayout = VK_NULL_HANDLE; }
    m_WhiteDescriptor = VK_NULL_HANDLE;
    m_Textures.clear();
}

void Renderer2D::BeginFrame(VkCommandBuffer cmd, const glm::mat4& viewProj, uint32_t width, uint32_t height, bool overlay) {
    m_Cmd = cmd;
    m_ViewProj = viewProj;
    m_Quads.clear();
    // FrameRender 已等待当前 swapchain image 对应的 fence；轮换到该帧槽后，
    // 本次 CPU 写入不会覆盖 GPU 仍在消费的其他帧顶点数据。
    m_CurrentFrameIndex = GetCurrentFrameIndex() % MAX_FRAMES_IN_FLIGHT;
    m_IsOverlay = overlay;
    m_CurrentPipeline = overlay ? &m_OverlayPipeline : &m_Pipeline;
    (void)width; (void)height;
}

void Renderer2D::ResetFrame() {
    m_VertexWriteOffset = 0;
}

void Renderer2D::DrawQuad(const Quad2D& quad) {
    if (m_Quads.size() >= MAX_QUADS) return;  // 超上限丢弃（安全）
    m_Quads.push_back(quad);
}

void Renderer2D::DrawRect(glm::vec2 pos, glm::vec2 size, const glm::vec4& color, int layer) {
    DrawSprite(pos, size, m_WhiteDescriptor, glm::vec2(0.0f), glm::vec2(1.0f), color, layer);
}

void Renderer2D::DrawSprite(glm::vec2 pos, glm::vec2 size, VkDescriptorSet texture,
                            const glm::vec2& uv0, const glm::vec2& uv1,
                            const glm::vec4& color, int layer) {
    Quad2D q;
    q.p0 = pos;
    q.p1 = glm::vec2(pos.x + size.x, pos.y);
    q.p2 = glm::vec2(pos.x + size.x, pos.y + size.y);
    q.p3 = glm::vec2(pos.x, pos.y + size.y);
    q.uv0 = uv0;
    q.uv1 = uv1;
    q.color = color;
    q.texture = (texture != VK_NULL_HANDLE) ? texture : m_WhiteDescriptor;
    q.layer = layer;
    DrawQuad(q);
}

void Renderer2D::DrawSlice9(glm::vec2 pos, glm::vec2 size, VkDescriptorSet texture,
                            const glm::vec4& border, const glm::vec2& srcSize,
                            const glm::vec2& uv0, const glm::vec2& uv1,
                            const glm::vec4& color, int layer) {
    // 参数校验
    if (texture == VK_NULL_HANDLE) texture = m_WhiteDescriptor;
    if (srcSize.x <= 0.0f || srcSize.y <= 0.0f) {
        // 源尺寸未知时退化为普通精灵
        DrawSprite(pos, size, texture, uv0, uv1, color, layer);
        return;
    }

    const float bL = border.x, bR = border.y, bT = border.z, bB = border.w;
    // 边框不能超过目标尺寸的一半
    const float maxBorderX = size.x * 0.45f;
    const float maxBorderY = size.y * 0.45f;
    const float bl = std::min(bL, maxBorderX);
    const float br = std::min(bR, maxBorderX);
    const float bt = std::min(bT, maxBorderY);
    const float bb = std::min(bB, maxBorderY);

    // 源 UV 分段：按 border/size 比例
    const float uL  = uv0.x + (uv1.x - uv0.x) * (bl / srcSize.x);
    const float uR  = uv1.x - (uv1.x - uv0.x) * (br / srcSize.x);
    const float vB  = uv0.y + (uv1.y - uv0.y) * (bb / srcSize.y);
    const float vT  = uv1.y - (uv1.y - uv0.y) * (bt / srcSize.y);

    // 目标坐标分段
    const float x0 = pos.x,  x1 = pos.x + bl,  x2 = pos.x + size.x - br,  x3 = pos.x + size.x;
    const float y0 = pos.y,  y1 = pos.y + bb,  y2 = pos.y + size.y - bt,  y3 = pos.y + size.y;

    // UV 坐标对: [bottom, top]×[left, right] — 9 格
    const float uCol[4] = { uv0.x, uL, uR, uv1.x };   // left, mid-left, mid-right, right
    const float vRow[4] = { uv0.y, vB, vT, uv1.y };   // bottom, mid-bottom, mid-top, top

    // 目标坐标对
    const float xCol[4] = { x0, x1, x2, x3 };
    const float yRow[4] = { y0, y1, y2, y3 };

    // 遍历 3×3 格（跳过面积为 0 的格子）
    for (int row = 0; row < 3; ++row) {
        const float vy0 = yRow[row], vy1 = yRow[row + 1];
        if (vy1 - vy0 <= 0.0f) continue;
        for (int col = 0; col < 3; ++col) {
            const float vx0 = xCol[col], vx1 = xCol[col + 1];
            if (vx1 - vx0 <= 0.0f) continue;
            Quad2D q;
            q.p0 = glm::vec2(vx0, vy0);
            q.p1 = glm::vec2(vx1, vy0);
            q.p2 = glm::vec2(vx1, vy1);
            q.p3 = glm::vec2(vx0, vy1);
            q.uv0 = glm::vec2(uCol[col], vRow[row]);
            q.uv1 = glm::vec2(uCol[col + 1], vRow[row + 1]);
            q.color = color;
            q.texture = texture;
            q.layer = layer;
            DrawQuad(q);
        }
    }
}

void Renderer2D::Flush() {
    if (m_Quads.empty() || m_Cmd == VK_NULL_HANDLE) return;

    // 诊断：每 60 帧打印一次图元数量（确认 2D 渲染链；stderr 无缓冲）——已注释（2026-08-06，subpass 改造排查期）
    // {
    //     static int s_fc = 0;
    //     if (++s_fc % 60 == 0) {
    //         fprintf(stderr, "[Renderer2D] frame %d: quads=%zu\n", s_fc, m_Quads.size());
    //     }
    // }

    // 排序: layer 优先(小 layer 先画=下层, 大 layer 后画=上层), 同 layer 按(shaderMode,纹理)分组(减少切换)
    std::stable_sort(m_Quads.begin(), m_Quads.end(), [](const Quad2D& a, const Quad2D& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.shaderMode != b.shaderMode) return a.shaderMode < b.shaderMode;
        return a.texture < b.texture;
    });

    // 写入顶点（6 顶点/四边形，非索引）
    // UV y 翻转：SDL 加载的纹理在 GPU 上上下颠倒，Vulkan 采样 (0,0) 对应原图底部，
    // 因此顶点 UV 的 v 与四边形上/下边对调，保证显示不反
    const uint32_t quadCount = (uint32_t)m_Quads.size();
    // 容量检查：帧内累计 + 本次周期的顶点数不能超过固定缓冲上限
    // （超限丢弃本次周期，避免越界写破坏先前周期的顶点数据）
    if (m_VertexWriteOffset + quadCount * QUAD_VERTICES > m_MaxVertices) {
        fprintf(stderr, "[Renderer2D] Vertex buffer overflow (offset=%u quads=%u), dropping this pass\n",
                m_VertexWriteOffset, quadCount);
        return;
    }
    VulkanBuffer& vertexStorage = m_UseSecondaryBuffer
        ? m_VertexBuffersSecondary[m_CurrentFrameIndex]
        : m_VertexBuffers[m_CurrentFrameIndex];
    Vertex2D* verts = static_cast<Vertex2D*>(vertexStorage.GetMappedPtr());
    for (uint32_t i = 0; i < quadCount; i++) {
        const Quad2D& q = m_Quads[i];
        Vertex2D* v = verts + (m_VertexWriteOffset + i * QUAD_VERTICES);
        const float range = q.screenPxRange;
        v[0] = { q.p0, { q.uv0.x, q.uv1.y }, q.color, range };   // 左下 ← 纹理底部
        v[1] = { q.p1, { q.uv1.x, q.uv1.y }, q.color, range };   // 右下
        v[2] = { q.p2, { q.uv1.x, q.uv0.y }, q.color, range };   // 右上 ← 纹理顶部
        v[3] = { q.p0, { q.uv0.x, q.uv1.y }, q.color, range };
        v[4] = { q.p2, { q.uv1.x, q.uv0.y }, q.color, range };
        v[5] = { q.p3, { q.uv0.x, q.uv0.y }, q.color, range };   // 左上
    }

    // 顶点缓冲绑定一次（各组的管线/描述符不同，但顶点缓冲相同）
    VkBuffer vertexBuffer = vertexStorage.GetBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(m_Cmd, 0, 1, &vertexBuffer, &offset);

    // 按 (shaderMode, 纹理) 分组 draw：组内一次 vkCmdDraw
    // 管线按 (overlay, shaderMode) 选择；firstVertex 是【顶点索引】单位，
    // 需加上本周期在缓冲中的起始顶点偏移（m_VertexWriteOffset 已是顶点数）
    const uint32_t passBaseVertex = m_VertexWriteOffset;
    uint32_t groupStart = 0;
    VkDescriptorSet currentTex = m_Quads[0].texture;
    int currentMode = m_Quads[0].shaderMode;
    for (uint32_t i = 1; i <= quadCount; i++) {
        bool boundary = (i == quadCount)
                     || (m_Quads[i].shaderMode != currentMode)
                     || (m_Quads[i].texture != currentTex);
        if (boundary) {
            uint32_t groupQuads = i - groupStart;
            VulkanPipeline* p = nullptr;
            if (m_IsSwapchainUI) {
                p = (currentMode == SHADER_MODE_SDF) ? &m_SwapchainUISdfPipeline : &m_SwapchainUIPipeline;
            } else if (m_IsDisplayUI) {
                p = (currentMode == SHADER_MODE_SDF) ? &m_DisplayUISdfPipeline : &m_DisplayUIPipeline;
            } else if (m_IsOverlay) {
                p = (currentMode == SHADER_MODE_SDF) ? &m_SdfOverlayPipeline : &m_OverlayPipeline;
            } else {
                p = (currentMode == SHADER_MODE_SDF) ? &m_SdfPipeline : &m_Pipeline;
            }
            // 防御：管线未创建（如 SDF shader 缺失）时跳过该组，避免 bind NULL 崩溃
            if (p->GetPipeline() == VK_NULL_HANDLE) {
                fprintf(stderr, "[Renderer2D] Skipping %d quad(s): pipeline not created (mode=%d)\n",
                        groupQuads, currentMode);
            } else {
                vkCmdBindPipeline(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->GetPipeline());
                vkCmdPushConstants(m_Cmd, p->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &m_ViewProj);
                vkCmdBindDescriptorSets(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    p->GetLayout(), 0, 1, &currentTex, 0, nullptr);
                vkCmdDraw(m_Cmd, groupQuads * QUAD_VERTICES, 1,
                          passBaseVertex + groupStart * QUAD_VERTICES, 0);
            }
            if (i < quadCount) {
                groupStart = i;
                currentTex = m_Quads[i].texture;
                currentMode = m_Quads[i].shaderMode;
            }
        }
    }

    // 本周期结束：累计顶点写入位置，供下一周期（下一次 BeginFrame/Flush）接续
    m_VertexWriteOffset += quadCount * QUAD_VERTICES;
}

bool Renderer2D::LoadTexture(const std::string& name, const std::string& filePath) {
    if (m_Textures.count(name)) return true;
    // Gameplay-only tests deserialize the same scenes/plugins without creating Vulkan resources.
    // Record a placeholder so tilemap/plugin initialization can continue; no render call occurs there.
    if (!Core::GetRuntimeCapabilities().rendering) {
        m_Textures[name] = VK_NULL_HANDLE;
        return true;
    }
    if (!g_TexturePool) return false;

    g_TexturePool->LoadTexture2D(name, filePath, SamplerType::Linear);
    VkImageView view = g_TexturePool->GetImageView(name);
    VkSampler sampler = g_TexturePool->GetSampler(name);
    if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) return false;

    // 用 2D 专用布局创建描述符
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_TexturePoolHandle;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_TextureLayout;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, &ds) != VK_SUCCESS) return false;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = view;
    imageInfo.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ds;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);

    m_Textures[name] = ds;
    return true;
}

VkDescriptorSet Renderer2D::GetTexture(const std::string& name) const {
    auto it = m_Textures.find(name);
    return (it != m_Textures.end()) ? it->second : m_WhiteDescriptor;
}

bool Renderer2D::CreateWhiteTexture() {
    // 1x1 RGBA8 白色图像
    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.extent.width = 1;
    imageCreateInfo.extent.height = 1;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_Device, &imageCreateInfo, g_Allocator, &m_WhiteImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_Device, m_WhiteImage, &memReq);
    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &memAllocInfo, g_Allocator, &m_WhiteMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(g_Device, m_WhiteImage, m_WhiteMemory, 0);

    // 写入白色像素（staging buffer → transfer）
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements bufReq;
    vkGetBufferMemoryRequirements(g_Device, staging, &bufReq);
    VkMemoryAllocateInfo bufAlloc{};
    bufAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAlloc.allocationSize = bufReq.size;
    bufAlloc.memoryTypeIndex = RendererUtils::FindMemoryType(bufReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &bufAlloc, g_Allocator, &stagingMem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_Device, staging, stagingMem, 0);

    uint8_t white[4] = { 255, 255, 255, 255 };
    void* mapped;
    vkMapMemory(g_Device, stagingMem, 0, 4, 0, &mapped);
    memcpy(mapped, white, 4);
    vkUnmapMemory(g_Device, stagingMem);

    // 单次命令缓冲做 layout 转换 + 拷贝
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = g_CommandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g_Device, &cmdAlloc, &cmd) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_WhiteImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { 1, 1, 1 };
    vkCmdCopyBufferToImage(cmd, staging, m_WhiteImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);

    // image view + sampler
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_WhiteImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_WhiteView) != VK_SUCCESS) return false;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_WhiteSampler) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = m_TexturePoolHandle;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &m_TextureLayout;
    if (vkAllocateDescriptorSets(g_Device, &setAllocInfo, &m_WhiteDescriptor) != VK_SUCCESS) return false;

    VkDescriptorImageInfo descImageInfo{};
    descImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descImageInfo.imageView = m_WhiteView;
    descImageInfo.sampler = m_WhiteSampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_WhiteDescriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descImageInfo;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);

    return true;
}

void Renderer2D::EnsureVertexCapacity(size_t quads) {
    // 固定容量缓冲：超上限时截断（Flush 已按 MAX_QUADS 丢弃，这里防御）
    (void)quads;
}
