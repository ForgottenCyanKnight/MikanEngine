// Renderer2D.cpp - 2D 渲染核心实现（图元合批 + 纹理描述符管理）
#include "Rendering/Renderer2D.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/RuntimeCapabilities.h"
#include "Core/ProjectManager.h"
#include "Core/VulkanManager.h"
#include "Rendering/TexturePool.h"
#include "Rendering/RendererBase.h"
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include "Rendering/RenderStats.h"
#include "Core/Log.h"
#include "Core/LogStream.h"

extern TexturePool* g_TexturePool;

namespace {
    constexpr uint32_t VERTEX_STRIDE = sizeof(Vertex2D);
    constexpr uint32_t QUAD_VERTICES = 6;  // 2 个三角形展开（非索引）

    // ===== Sprite 轮廓网格生成（tight mesh, overdraw 优化）=====
    // 管线: alpha 二值掩码 → 边界线段链环(定向遍历, 右转优先防对角粘连) →
    //       Douglas-Peucker 简化 → 耳切法三角化。
    // 坐标均为像素空间, 晶格角点 (x,y) ∈ [0,w]×[0,h]，行 0 = 图像顶部。

    struct MeshLoop { std::vector<glm::vec2> pts; };

    // 从二值掩码提取全部边界环。每条边界边只属于一个内部像素，按
    // "内部在行进方向右侧"(y 向下语义)定向链环，外轮廓与孔洞环方向相反，
    // 耳切前统一归一化即可。对角配置(棋盘格)顶点用右转优先消歧。
    void ExtractBoundaryLoops(const std::vector<uint8_t>& inside, int w, int h,
                              std::vector<MeshLoop>& loops) {
        using Key = long long;
        const Key stride = w + 1;
        auto key = [stride](int x, int y) -> Key { return (Key)y * stride + x; };
        auto insideAt = [&](int px, int py) -> bool {
            return px >= 0 && py >= 0 && px < w && py < h && inside[(size_t)py * w + px] != 0;
        };

        // 邻接表: 起点 → 出边终点列表（每条边恰好被消费一次）
        std::unordered_map<Key, std::vector<Key>> adj;
        auto addEdge = [&](int x0, int y0, int x1, int y1) {
            adj[key(x0, y0)].push_back(key(x1, y1));
        };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!insideAt(x, y)) continue;
                if (!insideAt(x, y - 1)) addEdge(x, y, x + 1, y);          // 上边 → +x
                if (!insideAt(x + 1, y)) addEdge(x + 1, y, x + 1, y + 1);  // 右边 → +y(下)
                if (!insideAt(x, y + 1)) addEdge(x + 1, y + 1, x, y + 1);  // 下边 → -x
                if (!insideAt(x - 1, y)) addEdge(x, y + 1, x, y);          // 左边 → -y
            }
        }

        auto decode = [stride](Key k, int& x, int& y) {
            x = (int)(k % stride); y = (int)(k / stride);
        };
        auto dirOf = [stride](Key a, Key b, int& dx, int& dy) {
            dx = (int)((b % stride) - (a % stride));
            dy = (int)(b / stride - a / stride);
        };

        // 消费边直到耗尽；每步按"右转优先(最大 cross)再直行(最大 dot)"选出边，
        // 使对角粘连处正确分裂为两个独立环。
        size_t remaining = 0;
        for (const auto& kv : adj) remaining += kv.second.size();
        size_t guard = remaining * 4 + 16;
        while (guard-- > 0) {
            // 任取一条未消费边作为环起点
            auto itStart = adj.begin();
            if (itStart == adj.end() || itStart->second.empty()) {
                bool found = false;
                for (auto it = adj.begin(); it != adj.end(); ++it) {
                    if (!it->second.empty()) { itStart = it; found = true; break; }
                }
                if (!found) break;
            }
            const Key startKey = itStart->first;
            Key curKey = itStart->second.back();
            itStart->second.pop_back();
            if (itStart->second.empty()) adj.erase(itStart);

            MeshLoop loop;
            int sx, sy; decode(startKey, sx, sy);
            loop.pts.push_back(glm::vec2((float)sx, (float)sy));
            int inDx = 0, inDy = 0;
            dirOf(startKey, curKey, inDx, inDy);

            bool closed = false;
            while (guard-- > 0) {
                int cx, cy; decode(curKey, cx, cy);
                if (curKey == startKey) { closed = true; break; }
                loop.pts.push_back(glm::vec2((float)cx, (float)cy));

                auto it = adj.find(curKey);
                if (it == adj.end() || it->second.empty()) break;  // 断链(理论上不发生)
                // 右转优先: cross(d,c) 大者(顺时针), 同向 cross 取 dot 大者(直行)
                Key bestKey = 0; float bestCross = -2.0f, bestDot = -2.0f;
                size_t bestSlot = 0;
                for (size_t s = 0; s < it->second.size(); ++s) {
                    int ddx = 0, ddy = 0;
                    dirOf(curKey, it->second[s], ddx, ddy);
                    const float cross = (float)(inDx * ddy - inDy * ddx);
                    const float dot = (float)(inDx * ddx + inDy * ddy);
                    if (cross > bestCross || (cross == bestCross && dot > bestDot)) {
                        bestCross = cross; bestDot = dot; bestKey = it->second[s]; bestSlot = s;
                    }
                }
                Key nextKey = bestKey;
                it->second.erase(it->second.begin() + (long)bestSlot);
                if (it->second.empty()) adj.erase(it);
                inDx = (int)((nextKey % stride) - (curKey % stride));
                inDy = (int)(nextKey / stride - curKey / stride);
                curKey = nextKey;
            }
            if (closed && loop.pts.size() >= 3) loops.push_back(std::move(loop));
        }
    }

    // 开放折线 RDP（递归深度由点数上界控制, 精灵轮廓点数有限）
    void RDP(const std::vector<glm::vec2>& pts, size_t first, size_t last, float eps,
             std::vector<uint8_t>& keep) {
        if (last <= first + 1) return;
        const glm::vec2 a = pts[first], b = pts[last];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        float maxDist = -1.0f; size_t maxIdx = first;
        for (size_t i = first + 1; i < last; ++i) {
            const glm::vec2 ap = pts[i] - a;
            float d;
            if (len2 < 1e-12f) d = glm::length(ap);
            else d = std::fabs(ab.x * ap.y - ab.y * ap.x) / std::sqrt(len2);
            if (d > maxDist) { maxDist = d; maxIdx = i; }
        }
        if (maxDist > eps) {
            keep[maxIdx] = 1;
            RDP(pts, first, maxIdx, eps, keep);
            RDP(pts, maxIdx, last, eps, keep);
        }
    }

    // 闭环简化：以距 p0 最远点劈开成两条开链分别 RDP
    void SimplifyClosedLoop(const std::vector<glm::vec2>& in, float eps,
                            std::vector<glm::vec2>& out) {
        if (in.size() < 4) { out = in; return; }
        // 去连续重复点
        std::vector<glm::vec2> pts;
        pts.reserve(in.size());
        for (const glm::vec2& p : in) {
            if (!pts.empty() && glm::distance(p, pts.back()) < 1e-4f) continue;
            pts.push_back(p);
        }
        if (pts.size() >= 2 && glm::distance(pts.front(), pts.back()) < 1e-4f) pts.pop_back();
        if (pts.size() < 3) { out = pts; return; }

        size_t farIdx = 0; float farDist = -1.0f;
        for (size_t i = 1; i < pts.size(); ++i) {
            const float d = glm::distance(pts[i], pts[0]);
            if (d > farDist) { farDist = d; farIdx = i; }
        }
        if (farDist < eps) { out.push_back(pts[0]); return; }

        // 链1: [0..farIdx]  链2: [farIdx..N-1, 0]
        std::vector<uint8_t> keep(pts.size(), 0);
        keep[0] = keep[farIdx] = 1;
        RDP(pts, 0, farIdx, eps, keep);
        // 链2 借助首点拼接: 构造临时序列 farIdx..N-1 再回 0
        std::vector<glm::vec2> back;
        back.reserve(pts.size() - farIdx + 1);
        for (size_t i = farIdx; i < pts.size(); ++i) back.push_back(pts[i]);
        back.push_back(pts[0]);
        std::vector<uint8_t> keep2(back.size(), 0);
        keep2[0] = keep2[back.size() - 1] = 1;
        RDP(back, 0, back.size() - 1, eps, keep2);
        // keep2[1..N-2] 对应 pts[farIdx+1..N-1]
        for (size_t i = 1; i + 1 < back.size(); ++i) {
            if (keep2[i]) keep[farIdx + i] = 1;
        }

        for (size_t i = 0; i < pts.size(); ++i) if (keep[i]) out.push_back(pts[i]);

        // 去共线点：闭合环劈半的锚点可能落在直边中点（如全矩形精灵），
        // 耳切遇到共线顶点会因 cross≈0 永不成为"耳"，产生退化三角形。
        // 用"到相邻点连线的垂距 < eps"判定并剔除（两轮覆盖相邻级联）。
        for (int pass = 0; pass < 2 && out.size() >= 3; ++pass) {
            std::vector<glm::vec2> tight;
            tight.reserve(out.size());
            for (size_t i = 0; i < out.size(); ++i) {
                const glm::vec2& prev = out[(i + out.size() - 1) % out.size()];
                const glm::vec2& cur = out[i];
                const glm::vec2& next = out[(i + 1) % out.size()];
                const glm::vec2 seg = next - prev;
                const float len2 = glm::dot(seg, seg);
                float dist;
                if (len2 < 1e-12f) dist = glm::length(cur - prev);
                else dist = std::fabs(seg.x * (cur.y - prev.y) - seg.y * (cur.x - prev.x))
                          / std::sqrt(len2);
                if (dist > eps) tight.push_back(cur);
            }
            out = std::move(tight);
        }
    }

    // 耳切法三角化（输入简单多边形；方向归一化为正叉积面积）
    bool TriangulateLoop(const std::vector<glm::vec2>& poly, std::vector<uint32_t>& tris) {
        const size_t n = poly.size();
        if (n < 3) return false;
        float area2 = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const glm::vec2& a = poly[i];
            const glm::vec2& b = poly[(i + 1) % n];
            area2 += a.x * b.y - b.x * a.y;
        }
        if (std::fabs(area2) < 1e-6f) return false;

        std::vector<uint32_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = (uint32_t)i;
        if (area2 < 0.0f) std::reverse(idx.begin(), idx.end());  // 统一 CCW(正面积)

        auto cross2 = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        };
        auto pointInTri = [&](const glm::vec2& p, const glm::vec2& a,
                              const glm::vec2& b, const glm::vec2& c) {
            const float d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
            return d1 >= -1e-9f && d2 >= -1e-9f && d3 >= -1e-9f;
        };

        size_t guard = n * n + 16;
        while (idx.size() > 3 && guard-- > 0) {
            bool clipped = false;
            float bestCross = -1e9f; size_t bestK = 0;
            for (size_t k = 0; k < idx.size(); ++k) {
                const size_t prev = (k + idx.size() - 1) % idx.size();
                const size_t next = (k + 1) % idx.size();
                const glm::vec2& a = poly[idx[prev]];
                const glm::vec2& b = poly[idx[k]];
                const glm::vec2& c = poly[idx[next]];
                const float cr = cross2(a, b, c);
                if (cr <= 1e-9f) continue;  // 凹点
                bool ear = true;
                for (uint32_t other : idx) {
                    if (other == idx[prev] || other == idx[k] || other == idx[next]) continue;
                    if (pointInTri(poly[other], a, b, c)) { ear = false; break; }
                }
                if (ear) {
                    tris.push_back(idx[prev]); tris.push_back(idx[k]); tris.push_back(idx[next]);
                    idx.erase(idx.begin() + (long)k);
                    clipped = true;
                    break;
                }
                if (cr > bestCross) { bestCross = cr; bestK = k; }  // 兜底记录最凸点
            }
            if (!clipped) {
                // 数值兜底: 无合法耳时裁掉最凸顶点, 保证终止
                const size_t k = bestK;
                const size_t prev = (k + idx.size() - 1) % idx.size();
                const size_t next = (k + 1) % idx.size();
                tris.push_back(idx[prev]); tris.push_back(idx[k]); tris.push_back(idx[next]);
                idx.erase(idx.begin() + (long)k);
            }
        }
        if (idx.size() == 3) {
            tris.push_back(idx[0]); tris.push_back(idx[1]); tris.push_back(idx[2]);
            return true;
        }
        return false;
    }
}

Renderer2D& Renderer2D::GetInstance() {
    static Renderer2D instance;
    return instance;
}

bool Renderer2D::Init(VkRenderPass offscreenPass, VkRenderPass overlayPass, VkRenderPass displayUIPass, VkRenderPass swapchainUIPass) {
    if (overlayPass == VK_NULL_HANDLE) return false;
    if (m_OffscreenPass == offscreenPass && m_OverlayPass == overlayPass &&
        m_DisplayUIPass == displayUIPass && m_SwapchainUIPass == swapchainUIPass) {
        return true;
    }

    // Swapchain rebuilds stay on the same device. Only graphics pipelines
    // depend on its render passes, so keep the sprites' texture descriptors.
    if (m_TextureLayout == VK_NULL_HANDLE && !InitDeviceResources()) {
        Cleanup();
        return false;
    }
    CleanupPipelines();
    if (!InitPipelines(offscreenPass, overlayPass, displayUIPass, swapchainUIPass)) {
        CleanupPipelines();
        return false;
    }
    m_OffscreenPass = offscreenPass;
    m_OverlayPass = overlayPass;
    m_DisplayUIPass = displayUIPass;
    m_SwapchainUIPass = swapchainUIPass;
    return true;
}

bool Renderer2D::InitDeviceResources() {
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
        LOGSTREAM(Error) << "[Renderer2D] Failed to create descriptor set layout" << std::endl;
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
        LOGSTREAM(Error) << "[Renderer2D] Failed to create descriptor pool" << std::endl;
        return false;
    }

    // 3. 白色 1x1 纹理（无纹理时的默认采样）
    if (!CreateWhiteTexture()) {
        LOGSTREAM(Error) << "[Renderer2D] Failed to create white texture" << std::endl;
        return false;
    }

    // 4. 顶点缓冲（固定容量：MAX_QUADS * 6 顶点 * 32B = 3MB）
    const VkDeviceSize bufferSize = (VkDeviceSize)MAX_QUADS * QUAD_VERTICES * VERTEX_STRIDE;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (!m_VertexBuffers[frame].Create(bufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create vertex buffer" << std::endl;
            return false;
        }
        m_VertexBuffers[frame].Map();  // 显式映射：Flush 直接写 GetMappedPtr()
        if (g_Standalone2DRuntime) continue;
        // SceneView 专用缓冲：避免同一帧不同 render pass 的 UI 顶点相互覆盖。
        if (!m_VertexBuffersSecondary[frame].Create(bufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create secondary vertex buffer" << std::endl;
            return false;
        }
        m_VertexBuffersSecondary[frame].Map();
    }
    m_MaxVertices = MAX_QUADS * QUAD_VERTICES;
    return true;
}

bool Renderer2D::InitPipelines(VkRenderPass offscreenPass, VkRenderPass overlayPass,
                               VkRenderPass displayUIPass, VkRenderPass swapchainUIPass) {
    // 5. 管线（透明混合、无深度、无剔除、push constant = mat4 viewProj）
    PipelineConfig config;
    config.vertShader = "ui/ui2d.vert.spv";
    config.fragShader = "ui/ui2d.frag.spv";
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
    offscreenConfig.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    offscreenConfig.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0,
        0,
        0,
        0,
        0
    };
    offscreenConfig.subpass = 1;      // MRT 几何 subpass（0=z-prepass）
    if (offscreenPass != VK_NULL_HANDLE) {
        if (!m_Pipeline.Create(offscreenPass, m_TextureLayout, offscreenConfig)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create offscreen pipeline" << std::endl;
            return false;
        }
        PipelineConfig offscreenSdfConfig = offscreenConfig;
        offscreenSdfConfig.fragShader = "ui/ui2d_sdf.frag.spv";
        if (!m_SdfPipeline.Create(offscreenPass, m_TextureLayout, offscreenSdfConfig)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create SDF offscreen pipeline" << std::endl;
            return false;
        }
    }
    // 主窗口管线：无深度附件，保持 depthWrite=false
    if (!m_OverlayPipeline.Create(overlayPass, m_TextureLayout, config)) {
        LOGSTREAM(Error) << "[Renderer2D] Failed to create overlay pipeline" << std::endl;
        return false;
    }

    // SDF 管线（同一布局/顶点输入，frag 换 ui2d_sdf 距离场 shader）
    PipelineConfig sdfConfig = config;
    sdfConfig.fragShader = "ui/ui2d_sdf.frag.spv";
    if (!m_SdfOverlayPipeline.Create(overlayPass, m_TextureLayout, sdfConfig)) {
        LOGSTREAM(Error) << "[Renderer2D] Failed to create SDF overlay pipeline" << std::endl;
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
            LOGSTREAM(Error) << "[Renderer2D] Failed to create display UI pipeline" << std::endl;
            return false;
        }
        PipelineConfig displayUISdfConfig = displayUIConfig;
        displayUISdfConfig.fragShader = "ui/ui2d_sdf.frag.spv";
        if (!m_DisplayUISdfPipeline.Create(displayUIPass, m_TextureLayout, displayUISdfConfig)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create display UI SDF pipeline" << std::endl;
            return false;
        }
    }
    // swapchain UI 叠加管线（游戏模式，swapchain loadOp=LOAD pass）
    if (swapchainUIPass != VK_NULL_HANDLE && swapchainUIPass != overlayPass) {
        if (!m_SwapchainUIPipeline.Create(swapchainUIPass, m_TextureLayout, displayUIConfig)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create swapchain UI pipeline" << std::endl;
            return false;
        }
        PipelineConfig swapchainUISdfConfig = displayUIConfig;
        swapchainUISdfConfig.fragShader = "ui/ui2d_sdf.frag.spv";
        if (!m_SwapchainUISdfPipeline.Create(swapchainUIPass, m_TextureLayout, swapchainUISdfConfig)) {
            LOGSTREAM(Error) << "[Renderer2D] Failed to create swapchain UI SDF pipeline" << std::endl;
            return false;
        }
    }

    return true;
}

void Renderer2D::CleanupPipelines() {
    m_Pipeline.Cleanup();
    m_OverlayPipeline.Cleanup();
    m_SdfPipeline.Cleanup();
    m_SdfOverlayPipeline.Cleanup();
    m_DisplayUIPipeline.Cleanup();
    m_DisplayUISdfPipeline.Cleanup();
    m_SwapchainUIPipeline.Cleanup();
    m_SwapchainUISdfPipeline.Cleanup();
    m_CurrentPipeline = nullptr;
    m_OffscreenPass = m_OverlayPass = m_DisplayUIPass = m_SwapchainUIPass = VK_NULL_HANDLE;
}

void Renderer2D::Cleanup() {
    CleanupPipelines();
    m_Quads.clear();
    m_Tris.clear();
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (m_VertexBuffers[frame].GetBuffer() != VK_NULL_HANDLE) {
            m_VertexBuffers[frame].Cleanup();
        }
        if (m_VertexBuffersSecondary[frame].GetBuffer() != VK_NULL_HANDLE) {
            m_VertexBuffersSecondary[frame].Cleanup();
        }
    }
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
    m_Tris.clear();
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

void Renderer2D::DrawLine(glm::vec2 a, glm::vec2 b, float width, const glm::vec4& color, int layer) {
    const glm::vec2 d = b - a;
    const float len = glm::length(d);
    if (len < 1e-5f) return;
    // 单位法线 × 半线宽 → 旋转矩形四角
    const glm::vec2 n = glm::vec2(-d.y, d.x) / len * (width * 0.5f);
    Quad2D q;
    q.p0 = a + n;
    q.p1 = b + n;
    q.p2 = b - n;
    q.p3 = a - n;
    q.uv0 = glm::vec2(0.0f);
    q.uv1 = glm::vec2(1.0f);
    q.color = color;
    q.texture = m_WhiteDescriptor;
    q.layer = layer;
    DrawQuad(q);
}

void Renderer2D::DrawCircle(glm::vec2 center, float radius, const glm::vec4& color,
                            int layer, int segments) {
    if (radius <= 0.0f) return;
    segments = std::clamp(segments, 8, 128);
    const float step = 6.28318530718f / static_cast<float>(segments);
    // 扇形逐段展开为退化 quad：三角形 (center, prev, cur) + (center, cur, cur) 退化。
    // 每段 1 quad，合批/分层/管线全部复用，无需新管线。
    glm::vec2 prev(center.x + radius, center.y);
    for (int i = 1; i <= segments; ++i) {
        const float ang = step * static_cast<float>(i);
        const glm::vec2 cur(center.x + radius * std::cos(ang),
                            center.y + radius * std::sin(ang));
        Quad2D q;
        q.p0 = center;
        q.p1 = prev;
        q.p2 = cur;
        q.p3 = cur;
        q.uv0 = glm::vec2(0.0f);
        q.uv1 = glm::vec2(1.0f);
        q.color = color;
        q.texture = m_WhiteDescriptor;
        q.layer = layer;
        DrawQuad(q);
        prev = cur;
    }
}

void Renderer2D::DrawRing(glm::vec2 center, float radius, float thickness,
                          const glm::vec4& color, int layer, int segments) {
    if (radius <= 0.0f || thickness <= 0.0f) return;
    segments = std::clamp(segments, 8, 256);
    const float step = 6.28318530718f / static_cast<float>(segments);
    const float r0 = std::max(0.0f, radius - thickness * 0.5f);
    const float r1 = radius + thickness * 0.5f;
    glm::vec2 prevO(center.x + r1, center.y);
    glm::vec2 prevI(center.x + r0, center.y);
    for (int i = 1; i <= segments; ++i) {
        const float ang = step * static_cast<float>(i);
        const glm::vec2 curO(center.x + r1 * std::cos(ang),
                             center.y + r1 * std::sin(ang));
        const glm::vec2 curI(center.x + r0 * std::cos(ang),
                             center.y + r0 * std::sin(ang));
        Quad2D q;
        q.p0 = prevI;
        q.p1 = prevO;
        q.p2 = curO;
        q.p3 = curI;
        q.uv0 = glm::vec2(0.0f);
        q.uv1 = glm::vec2(1.0f);
        q.color = color;
        q.texture = m_WhiteDescriptor;
        q.layer = layer;
        DrawQuad(q);
        prevO = curO;
        prevI = curI;
    }
}

void Renderer2D::DrawTriangle(const Tri2D& tri) {
    if (m_Tris.size() >= MAX_QUADS) return;  // 超上限丢弃（安全）
    Tri2D t = tri;
    if (t.texture == VK_NULL_HANDLE) t.texture = m_WhiteDescriptor;
    m_Tris.push_back(t);
}

void Renderer2D::DrawSpriteMesh(glm::vec2 pos, glm::vec2 size, const std::string& meshName,
                                const glm::vec4& color, int layer, int shaderMode,
                                float screenPxRange) {
    const SpriteMesh2D* mesh = GetSpriteMesh(meshName);
    if (!mesh || mesh->triangleCount == 0 || mesh->indices.size() < 3) {
        // 网格未生成 → 回退整块精灵 quad
        DrawSprite(pos, size, GetTexture(meshName), glm::vec2(0.0f), glm::vec2(1.0f), color, layer);
        return;
    }
    const VkDescriptorSet texture = GetTexture(meshName);
    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
        const uint32_t i0 = mesh->indices[i], i1 = mesh->indices[i + 1], i2 = mesh->indices[i + 2];
        if (i0 >= mesh->positions.size() || i1 >= mesh->positions.size() ||
            i2 >= mesh->positions.size()) return;  // 索引异常防御
        Tri2D tri;
        tri.p0 = pos + mesh->positions[i0] * size;
        tri.p1 = pos + mesh->positions[i1] * size;
        tri.p2 = pos + mesh->positions[i2] * size;
        tri.uv0 = mesh->uvs[i0];
        tri.uv1 = mesh->uvs[i1];
        tri.uv2 = mesh->uvs[i2];
        tri.color = color;
        tri.texture = texture;
        tri.layer = layer;
        tri.shaderMode = shaderMode;
        tri.screenPxRange = screenPxRange;
        DrawTriangle(tri);
    }
}

bool Renderer2D::GenerateSpriteMeshFromPixels(const std::string& name, const uint8_t* rgba,
                                              int w, int h, float alphaThreshold) {
    if (m_SpriteMeshes.count(name)) return true;
    if (!rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const float threshold = std::clamp(alphaThreshold, 1e-3f, 1.0f);
    const uint8_t cut = (uint8_t)std::floor(threshold * 255.0f + 0.5f);

    // 二值掩码
    std::vector<uint8_t> inside((size_t)w * h, 0);
    size_t opaqueCount = 0;
    for (size_t i = 0; i < inside.size(); ++i) {
        if (rgba[i * 4 + 3] >= cut) { inside[i] = 1; ++opaqueCount; }
    }
    if (opaqueCount == 0) return false;  // 全透明精灵: 无轮廓可提取

    // 提取边界环 → 简化 → 三角化
    std::vector<MeshLoop> loops;
    ExtractBoundaryLoops(inside, w, h, loops);
    const float simplifyEps = std::max(1.25f, 0.008f * (float)std::max(w, h));

    SpriteMesh2D mesh;
    std::vector<std::vector<uint32_t>> loopTris;
    loopTris.reserve(loops.size());
    double meshArea = 0.0;
    for (MeshLoop& loop : loops) {
        std::vector<glm::vec2> simplified;
        SimplifyClosedLoop(loop.pts, simplifyEps, simplified);
        if (simplified.size() < 3) continue;
        std::vector<uint32_t> tris;
        if (!TriangulateLoop(simplified, tris) || tris.size() < 3) continue;
        // 基点偏移进全局顶点表; 像素空间 → 归一化 (0,0)=左上, y 向下
        const uint32_t base = (uint32_t)mesh.positions.size();
        for (const glm::vec2& p : simplified) {
            mesh.positions.push_back(glm::vec2(p.x / (float)w, p.y / (float)h));
            // 最终采样 UV: v=1 对应原图顶部（与 Flush 的 v 翻转约定一致）
            mesh.uvs.push_back(glm::vec2(p.x / (float)w, 1.0f - p.y / (float)h));
        }
        for (uint32_t t : tris) mesh.indices.push_back(base + t);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const glm::vec2& a = simplified[tris[i]];
            const glm::vec2& b = simplified[tris[i + 1]];
            const glm::vec2& c = simplified[tris[i + 2]];
            meshArea += 0.5 * std::fabs((double)a.x * ((double)b.y - (double)c.y) +
                                        (double)b.x * ((double)c.y - (double)a.y) +
                                        (double)c.x * ((double)a.y - (double)b.y));
        }
        loopTris.push_back(std::move(tris));
    }
    mesh.triangleCount = (uint32_t)(mesh.indices.size() / 3);
    if (mesh.triangleCount == 0) return false;
    mesh.coverage = (float)std::min(1.0, meshArea / ((double)w * (double)h));

    m_SpriteMeshes[name] = std::move(mesh);
    return true;
}

bool Renderer2D::GenerateSpriteMesh(const std::string& name, float alphaThreshold) {
    if (m_SpriteMeshes.count(name)) return true;
    auto itPath = m_TexturePaths.find(name);
    if (itPath == m_TexturePaths.end()) {
        LOGW("[Renderer2D] GenerateSpriteMesh: no source path recorded for '%s'", name.c_str());
        return false;
    }
    if (!Core::GetRuntimeCapabilities().rendering && !ProjectManager::GetInstance().HasActiveProject()) {
        LOGW("[Renderer2D] GenerateSpriteMesh: no active project to resolve '%s'", itPath->second.c_str());
        return false;
    }

    // 解析源路径（与 TexturePool::LoadTexture2D 一致：非 Android 走 ResolveAssetPath）
    std::string fullPath = itPath->second;
#ifndef __ANDROID__
    if (ProjectManager::GetInstance().HasActiveProject()) {
        const std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(fullPath);
        if (!resolved.empty()) fullPath = resolved;
    }
#endif

    SDL_Surface* surface = IMG_Load(fullPath.c_str());
    if (surface == nullptr) {
        LOGE("[Renderer2D] GenerateSpriteMesh: failed to open '%s': %s",
             fullPath.c_str(), SDL_GetError());
        return false;
    }
    // SDL_PIXELFORMAT_ABGR8888 在小端机器上内存字节序即 R,G,B,A（与 TexturePool 同款约定）
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    if (converted == nullptr) {
        LOGE("[Renderer2D] GenerateSpriteMesh: failed to convert '%s': %s",
             fullPath.c_str(), SDL_GetError());
        return false;
    }

    const int w = converted->w, h = converted->h;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(converted->pixels);
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + (size_t)y * w * 4,
                    src + (size_t)y * (size_t)converted->pitch, (size_t)w * 4);
    }
    SDL_DestroySurface(converted);
    return GenerateSpriteMeshFromPixels(name, rgba.data(), w, h, alphaThreshold);
}

const SpriteMesh2D* Renderer2D::GetSpriteMesh(const std::string& name) const {
    auto it = m_SpriteMeshes.find(name);
    return (it != m_SpriteMeshes.end()) ? &it->second : nullptr;
}

void Renderer2D::Flush() {
    const uint32_t quadCount = (uint32_t)m_Quads.size();
    const uint32_t triCount = (uint32_t)m_Tris.size();
    if ((quadCount == 0 && triCount == 0) || m_Cmd == VK_NULL_HANDLE) return;

    // 合流排序: layer 优先(小 layer 先画=下层), 同 layer 按(shaderMode,纹理)分组。
    // quad 与 triangle 处于同一层序流——DrawSpriteMesh 的三角形必须与普通 quad
    // 保持正确的层叠关系，因此不能分开排序/绘制。
    struct PrimRef { uint32_t index; bool isTri; };
    std::vector<PrimRef> order;
    order.reserve((size_t)quadCount + triCount);
    for (uint32_t i = 0; i < quadCount; ++i) order.push_back({ i, false });
    for (uint32_t i = 0; i < triCount; ++i) order.push_back({ i, true });
    auto keyOf = [&](const PrimRef& r, int& layer, int& mode, VkDescriptorSet& tex) {
        if (r.isTri) {
            const Tri2D& t = m_Tris[r.index];
            layer = t.layer; mode = t.shaderMode; tex = t.texture;
        } else {
            const Quad2D& q = m_Quads[r.index];
            layer = q.layer; mode = q.shaderMode; tex = q.texture;
        }
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](const PrimRef& a, const PrimRef& b) {
        int la = 0, lb = 0, ma = 0, mb = 0;
        VkDescriptorSet ta = VK_NULL_HANDLE, tb = VK_NULL_HANDLE;
        keyOf(a, la, ma, ta);
        keyOf(b, lb, mb, tb);
        if (la != lb) return la < lb;
        if (ma != mb) return ma < mb;
        return ta < tb;
    });

    // 写入顶点：quad=6 顶点(2 三角形展开)，tri=3 顶点
    // UV 约定：quad 侧保持原有"顶点 UV 的 v 与四边形上/下边对调"翻转（SDL 纹理
    // 在 GPU 上上下颠倒）；Tri2D 的 uv 已是最终采样值，直接透传。
    const uint32_t totalVertices = quadCount * QUAD_VERTICES + triCount * 3u;
    // 容量检查：帧内累计 + 本次周期的顶点数不能超过固定缓冲上限
    // （超限丢弃本次周期，避免越界写破坏先前周期的顶点数据）
    if (m_VertexWriteOffset + totalVertices > m_MaxVertices) {
        LOGE("[Renderer2D] Vertex buffer overflow (offset=%u verts=%u), dropping this pass",
                m_VertexWriteOffset, totalVertices);
        return;
    }
    VulkanBuffer& vertexStorage = m_UseSecondaryBuffer
        ? m_VertexBuffersSecondary[m_CurrentFrameIndex]
        : m_VertexBuffers[m_CurrentFrameIndex];
    Vertex2D* verts = static_cast<Vertex2D*>(vertexStorage.GetMappedPtr());

    std::vector<uint32_t> primStart(order.size() + 1);
    uint32_t cursor = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        primStart[k] = cursor;
        if (order[k].isTri) {
            const Tri2D& t = m_Tris[order[k].index];
            Vertex2D* v = verts + (m_VertexWriteOffset + cursor);
            v[0] = { t.p0, t.uv0, t.color, t.screenPxRange };
            v[1] = { t.p1, t.uv1, t.color, t.screenPxRange };
            v[2] = { t.p2, t.uv2, t.color, t.screenPxRange };
            cursor += 3;
        } else {
            const Quad2D& q = m_Quads[order[k].index];
            Vertex2D* v = verts + (m_VertexWriteOffset + cursor);
            const float range = q.screenPxRange;
            v[0] = { q.p0, { q.uv0.x, q.uv1.y }, q.color, range };   // 左下 ← 纹理底部
            v[1] = { q.p1, { q.uv1.x, q.uv1.y }, q.color, range };   // 右下
            v[2] = { q.p2, { q.uv1.x, q.uv0.y }, q.color, range };   // 右上 ← 纹理顶部
            v[3] = { q.p0, { q.uv0.x, q.uv1.y }, q.color, range };
            v[4] = { q.p2, { q.uv1.x, q.uv0.y }, q.color, range };
            v[5] = { q.p3, { q.uv0.x, q.uv0.y }, q.color, range };   // 左上
            cursor += QUAD_VERTICES;
        }
    }
    primStart[order.size()] = cursor;

    // 顶点缓冲绑定一次（各组的管线/描述符不同，但顶点缓冲相同）
    VkBuffer vertexBuffer = vertexStorage.GetBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(m_Cmd, 0, 1, &vertexBuffer, &offset);

    // 按 (shaderMode, 纹理) 分组 draw：组内一次 vkCmdDraw
    // 管线按 (overlay, shaderMode) 选择；firstVertex 是【顶点索引】单位，
    // 需加上本周期在缓冲中的起始顶点偏移（m_VertexWriteOffset 已是顶点数）
    const uint32_t passBaseVertex = m_VertexWriteOffset;
    size_t groupStart = 0;
    int currentMode = 0;
    VkDescriptorSet currentTex = VK_NULL_HANDLE;
    {
        int layer = 0;
        keyOf(order[0], layer, currentMode, currentTex);
    }
    for (size_t i = 1; i <= order.size(); ++i) {
        bool boundary = (i == order.size());
        if (!boundary) {
            int layer = 0, mode = 0;
            VkDescriptorSet tex = VK_NULL_HANDLE;
            keyOf(order[i], layer, mode, tex);
            boundary = (mode != currentMode) || (tex != currentTex);
        }
        if (boundary) {
            const uint32_t groupVertices = primStart[i] - primStart[groupStart];
            VulkanPipeline* p = nullptr;
            if (m_IsSwapchainUI && m_SwapchainUIPass == m_OverlayPass) {
                p = (currentMode == SHADER_MODE_SDF) ? &m_SdfOverlayPipeline : &m_OverlayPipeline;
            } else if (m_IsSwapchainUI) {
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
                LOGW("[Renderer2D] Skipping group of %u vert(s): pipeline not created (mode=%d)",
                        groupVertices, currentMode);
            } else {
                vkCmdBindPipeline(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->GetPipeline());
                vkCmdPushConstants(m_Cmd, p->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &m_ViewProj);
                vkCmdBindDescriptorSets(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    p->GetLayout(), 0, 1, &currentTex, 0, nullptr);
                vkCmdDraw(m_Cmd, groupVertices, 1,
                          passBaseVertex + primStart[groupStart], 0);
            }
            if (i < order.size()) {
                groupStart = i;
                int layer = 0;
                keyOf(order[i], layer, currentMode, currentTex);
            }
        }
    }

    // 本周期结束：累计顶点写入位置，供下一周期（下一次 BeginFrame/Flush）接续
    m_VertexWriteOffset += totalVertices;
}

bool Renderer2D::LoadTexture(const std::string& name, const std::string& filePath) {
    return LoadTexture(name, filePath, SamplerType::Linear);
}

bool Renderer2D::LoadTexture(const std::string& name, const std::string& filePath, SamplerType samplerType) {
    m_TexturePaths[name] = filePath;  // 记录源路径（GenerateSpriteMesh 重解码用）
    if (m_Textures.count(name)) return true;
    // Gameplay-only tests deserialize the same scenes/plugins without creating Vulkan resources.
    // Record a placeholder so tilemap/plugin initialization can continue; no render call occurs there.
    if (!Core::GetRuntimeCapabilities().rendering) {
        m_Textures[name] = VK_NULL_HANDLE;
        return true;
    }
    if (!g_TexturePool || !g_TexturePool->LoadTexture2D(name, filePath, samplerType)) {
        return false;
    }
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
