// ⚠️ 2026-08-15：本 TU 的 glm::ortho 必须 ZERO_TO_ONE（0..1 深度）！
// Vulkan viewport 深度变换是直存（z_fb = z_ndc，NDC z 约定 [0,1]），而 GLM 默认 RH_NO 产出 [-1,1]
// → 深度附件 = z_ndc 直存（近半 clamp 0）。主渲染已按此语义消费（反投影直传）。
// CSM shadowmap 必须用 ZO ortho 才能让附件存完整 [0,1]（否则近半深度全 clamp 0 → 阴影深度比较全错）。
// 本文件其余 glm 调用（lookAt/inverse）与深度约定无关；nearP/farP 提取、视锥角点反投影是手动公式不受影响。
// ⚠️ 必须在任何 glm include 之前定义（头文件 CascadeShadowRenderer.h 含 <glm/glm.hpp>）
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "Rendering/CascadeShadowRenderer.h"
#include "Core/EngineGlobal.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <cstring>

// 方向光 CSM 阴影实现（2026-08-14，参考 LimitlessSquareEngine）：
//  - 桌面 2 槽 / 移动 1 槽 × 4 级联 2048² D16 2D array（每槽独立锚点/矩阵）
//  - 级联分裂等比（base 2 × scale 3）：2 / 8 / 26 / 80（相对近平面）
//  - ⭐ 世界锚点防抖：double 锚点滞回更新 + 相对锚点 texel snap（详见头文件注释）
//  - 深度约定：glm::ortho 默认 -1..1（mikan 无 GLM_FORCE_DEPTH_ZERO_TO_ONE），经 Vulkan
//    viewport（minDepth=0/maxDepth=1）自动映射 [0,1]；采样端 ndc.z*0.5+0.5 直接比较

namespace {
// CSM 不使用地下光源：太阳在地平线以上时保持太阳方向，落到地平线下
// 立即切换到反向月光方向。这里不做插值，避免阴影贴图间隔刷新时矩阵与贴图方向不一致。
glm::vec3 ComputeCsmLightDirection(const glm::vec3& sunDirection)
{
    const float lengthSquared = glm::dot(sunDirection, sunDirection);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-8f) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const glm::vec3 sun = sunDirection / std::sqrt(lengthSquared);
    return sun.y >= 0.0f ? sun : -sun;
}

uint32_t FindCsmMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return VK_MAX_MEMORY_TYPES;
}

// GPU 级联数据（std140，288B）——与 fullscreen.frag binding 15 严格一致
struct GpuCsmData {
    glm::mat4 shadowMatrices[CascadeShadowRenderer::MAX_CASCADES];   // 世界 → 光 NDC
    glm::vec4 splitDepths;    // 每级联 far（view depth，无效 = -1）
    glm::vec4 params;         // x = 有效级联数，y = 启用（>0），z 保留
};
}

void CascadeShadowRenderer::Cleanup()
{
    if (!m_Initialized && m_Slots[0].image == VK_NULL_HANDLE) return;
    for (auto& s : m_Slots) {
        for (auto& fb : s.framebuffers) {
            if (fb != VK_NULL_HANDLE && g_Device) vkDestroyFramebuffer(g_Device, fb, g_Allocator);
            fb = VK_NULL_HANDLE;
        }
        for (auto& v : s.layerViews) {
            if (v != VK_NULL_HANDLE && g_Device) vkDestroyImageView(g_Device, v, g_Allocator);
            v = VK_NULL_HANDLE;
        }
        if (s.arrayView != VK_NULL_HANDLE && g_Device) vkDestroyImageView(g_Device, s.arrayView, g_Allocator);
        s.arrayView = VK_NULL_HANDLE;
        for (int f = 0; f < 3; f++) {   // 2026-08-17：3 帧槽
            if (s.uboMapped[f] && s.uboMemories[f] && g_Device) vkUnmapMemory(g_Device, s.uboMemories[f]);   // ⚠️ g_Device 守卫（静态析构期可能已置 NULL）
            s.uboMapped[f] = nullptr;
            if (s.uboBuffers[f] != VK_NULL_HANDLE && g_Device) vkDestroyBuffer(g_Device, s.uboBuffers[f], g_Allocator);
            s.uboBuffers[f] = VK_NULL_HANDLE;
            if (s.uboMemories[f] != VK_NULL_HANDLE && g_Device) vkFreeMemory(g_Device, s.uboMemories[f], g_Allocator);
            s.uboMemories[f] = VK_NULL_HANDLE;
        }
        if (s.image != VK_NULL_HANDLE && g_Device) vkDestroyImage(g_Device, s.image, g_Allocator);
        s.image = VK_NULL_HANDLE;
        if (s.memory != VK_NULL_HANDLE && g_Device) vkFreeMemory(g_Device, s.memory, g_Allocator);
        s.memory = VK_NULL_HANDLE;
        s.anchorInitialized = false;   // 2026-08-15：单例锚点
    }
    if (m_RenderPass != VK_NULL_HANDLE && g_Device) vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
    m_RenderPass = VK_NULL_HANDLE;
    m_Initialized = false;
}

bool CascadeShadowRenderer::Init()
{
    if (m_Initialized) return true;

    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        Slot& s = m_Slots[slot];

        // ---- 2D array image（4 层 D16，每层 2048²）----
        VkImageCreateInfo ii = {};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_D16_UNORM;
        ii.extent = { CASCADE_SIZE, CASCADE_SIZE, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = MAX_CASCADES;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_Device, &ii, g_Allocator, &s.image) != VK_SUCCESS) {
            LOGE("[CSM] 2D array image create failed (slot %d)", slot);
            Cleanup();
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_Device, s.image, &req);
        const uint32_t mt = FindCsmMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == VK_MAX_MEMORY_TYPES) { Cleanup(); return false; }
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mt;
        if (vkAllocateMemory(g_Device, &ai, g_Allocator, &s.memory) != VK_SUCCESS) { Cleanup(); return false; }
        vkBindImageMemory(g_Device, s.image, s.memory, 0);

        // ---- array view（合成采样）----
        VkImageViewCreateInfo avi = {};
        avi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        avi.image = s.image;
        avi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        avi.format = VK_FORMAT_D16_UNORM;
        avi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        avi.subresourceRange.baseMipLevel = 0;
        avi.subresourceRange.levelCount = 1;
        avi.subresourceRange.baseArrayLayer = 0;
        avi.subresourceRange.layerCount = MAX_CASCADES;
        if (vkCreateImageView(g_Device, &avi, g_Allocator, &s.arrayView) != VK_SUCCESS) { Cleanup(); return false; }
    }

    // ---- depth-only render pass（与 PointShadowRenderer 同结构）----
    VkAttachmentDescription att = {};
    att.format = VK_FORMAT_D16_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = {};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 0;
    sub.pDepthStencilAttachment = &ref;
    VkRenderPassCreateInfo rpi = {};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    if (vkCreateRenderPass(g_Device, &rpi, g_Allocator, &m_RenderPass) != VK_SUCCESS) { Cleanup(); return false; }

    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        Slot& s = m_Slots[slot];
        for (int c = 0; c < MAX_CASCADES; c++) {
            // 单层 2D view + framebuffer
            VkImageViewCreateInfo fvi = {};
            fvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            fvi.image = s.image;
            fvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            fvi.format = VK_FORMAT_D16_UNORM;
            fvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            fvi.subresourceRange.baseMipLevel = 0;
            fvi.subresourceRange.levelCount = 1;
            fvi.subresourceRange.baseArrayLayer = c;
            fvi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(g_Device, &fvi, g_Allocator, &s.layerViews[c]) != VK_SUCCESS) { Cleanup(); return false; }
            VkFramebufferCreateInfo fbi = {};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = m_RenderPass;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &s.layerViews[c];
            fbi.width = CASCADE_SIZE;
            fbi.height = CASCADE_SIZE;
            fbi.layers = 1;
            if (vkCreateFramebuffer(g_Device, &fbi, g_Allocator, &s.framebuffers[c]) != VK_SUCCESS) { Cleanup(); return false; }
        }
        // ---- 级联 UBO（per-frame 双缓冲——2 帧 in-flight：合成读与下一帧写竞争 → 阴影移动偏离）----
        for (int f = 0; f < 3; f++) {
            VkBufferCreateInfo bi = {};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = sizeof(GpuCsmData);
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(g_Device, &bi, g_Allocator, &s.uboBuffers[f]) != VK_SUCCESS) { Cleanup(); return false; }
            VkMemoryRequirements breq;
            vkGetBufferMemoryRequirements(g_Device, s.uboBuffers[f], &breq);
            const uint32_t bmt = FindCsmMemoryType(breq.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (bmt == VK_MAX_MEMORY_TYPES) { Cleanup(); return false; }
            VkMemoryAllocateInfo bai = {};
            bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            bai.allocationSize = breq.size;
            bai.memoryTypeIndex = bmt;
            if (vkAllocateMemory(g_Device, &bai, g_Allocator, &s.uboMemories[f]) != VK_SUCCESS) { Cleanup(); return false; }
            vkBindBufferMemory(g_Device, s.uboBuffers[f], s.uboMemories[f], 0);
            if (vkMapMemory(g_Device, s.uboMemories[f], 0, breq.size, 0, &s.uboMapped[f]) != VK_SUCCESS) { Cleanup(); return false; }
        }
    }

    m_Initialized = true;
    LOGI("[CSM] cascade shadow ready: %d slots × %d cascades × %d² D16", MAX_SLOTS, MAX_CASCADES, CASCADE_SIZE);
    return true;
}

// ===== ⭐ 世界锚点（LimitlessSquare GetDirectionalShadowStableAnchorRelative 移植）=====
// 锚点 = 相机位置对齐到 grid 网格的 double 世界点；滞回：仅当相机偏离锚点超过 grid/2 才跳变。
// 返回锚点相对相机的向量（float——因 |anchor-camera| ≤ grid/2 恒成立，相对值精度有保证）
glm::vec3 CascadeShadowRenderer::GetStableAnchorRelative(int slot, int cascade, const glm::dvec3& cameraWorld, double gridWorldSize)
{
    if (slot < 0 || slot >= MAX_SLOTS) return glm::vec3(0.0f);
    Slot& s = m_Slots[slot];
    if (gridWorldSize <= 0.0000001) return glm::vec3(0.0f);

    // ⚠️ 2026-08-15：单例锚点（原版 `_directionalShadowStableAnchorWorld`）——cascade 参数忽略。
    // 所有级联共享同一锚点 → snap 相位一致 → 层级切换阴影对齐。grid 用每级联各自的 halfExtent*2
    // （原版同式：每级联调用传本级联 grid，最小 grid 级联主导滞回）。
    double grid = gridWorldSize;
    glm::dvec3& anchor = s.stableAnchorWorld;
    bool& initialized = s.anchorInitialized;
    if (!initialized) {
        anchor = glm::dvec3(
            glm::round(cameraWorld.x / grid) * grid,
            glm::round(cameraWorld.y / grid) * grid,
            glm::round(cameraWorld.z / grid) * grid);
        initialized = true;
    } else {
        double dx = cameraWorld.x - anchor.x;
        double dy = cameraWorld.y - anchor.y;
        double dz = cameraWorld.z - anchor.z;
        if (glm::abs(dx) > grid * 0.5) anchor.x = glm::round(cameraWorld.x / grid) * grid;
        if (glm::abs(dy) > grid * 0.5) anchor.y = glm::round(cameraWorld.y / grid) * grid;
        if (glm::abs(dz) > grid * 0.5) anchor.z = glm::round(cameraWorld.z / grid) * grid;
    }

    glm::dvec3 relative = anchor - cameraWorld;
    // 注：LimitlessSquare 原版返回时 z 取反（其 OpenGL 世界约定）；mikan 为 Vulkan 右手系，
    // anchor 与 center 在同一 world 坐标系投影到光基——直接返回，不取反（取反会破坏投影一致性）
    return glm::vec3((float)relative.x, (float)relative.y, (float)relative.z);
}

// ===== texel snap（LimitlessSquare SnapDirectionalShadowCenterToTexelStable 移植）=====
// 视锥中心相对锚点在光 right/up 平面内 round 到 texel；forward（深度）方向不 snap。
// 量化参考系固定在世界锚点（滞回不动）→ 相机移动时阴影矩阵以 texel 为粒度确定性变化，不累积漂移
glm::vec3 CascadeShadowRenderer::SnapCenterToTexelStable(const glm::vec3& centerWorld,
                                                         const glm::vec3& anchorRelative,
                                                         const glm::vec3& right, const glm::vec3& up, const glm::vec3& forward,
                                                         double texelWorldSize)
{
    if (texelWorldSize <= 0.0000001) return centerWorld;
    const double texel = texelWorldSize;

    double centerLocalX = (double)centerWorld.x * right.x + (double)centerWorld.y * right.y + (double)centerWorld.z * right.z;
    double centerLocalY = (double)centerWorld.x * up.x   + (double)centerWorld.y * up.y   + (double)centerWorld.z * up.z;
    double centerLocalZ = (double)centerWorld.x * forward.x + (double)centerWorld.y * forward.y + (double)centerWorld.z * forward.z;

    double anchorLocalX = (double)anchorRelative.x * right.x + (double)anchorRelative.y * right.y + (double)anchorRelative.z * right.z;
    double anchorLocalY = (double)anchorRelative.x * up.x   + (double)anchorRelative.y * up.y   + (double)anchorRelative.z * up.z;

    double deltaX = centerLocalX - anchorLocalX;
    double deltaY = centerLocalY - anchorLocalY;

    double snappedDeltaX = glm::round(deltaX / texel) * texel;
    double snappedDeltaY = glm::round(deltaY / texel) * texel;

    double snappedLocalX = anchorLocalX + snappedDeltaX;
    double snappedLocalY = anchorLocalY + snappedDeltaY;

    return right * (float)snappedLocalX + up * (float)snappedLocalY + forward * (float)centerLocalZ;
}

void CascadeShadowRenderer::UpdateCascades(int slot, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& lightDir)
{
    if (slot < 0 || slot >= MAX_SLOTS || !m_Initialized) return;
    Slot& s = m_Slots[slot];

    const glm::dvec3 cameraWorld = glm::dvec3(glm::inverse(view)[3]);
    // lightDir 是太阳（表面指向光源）的原始方向；CSM 使用经过地平线过渡的
    // 太阳/月光方向，保证太阳落山后 shadow map 仍在有效半球内。
    const glm::vec3 csmLightDir = ComputeCsmLightDirection(lightDir);
    const glm::vec3 lightDirN = glm::normalize(-csmLightDir);

    // 近/远平面提取（mikan 深度约定 -1..1：n = p32/(p22-1)，f = p32/(p22+1)）
    float nearP = proj[3][2] / (proj[2][2] - 1.0f);
    float farP  = proj[3][2] / (proj[2][2] + 1.0f);
    if (!(nearP > 0.0f) || !(farP > nearP)) {
        // 投影无效（正交/退化）→ 全级联禁用（合成端回退无阴影）
        for (int c = 0; c < MAX_CASCADES; c++) {
            s.valid[c] = false;
            s.splitFars[c] = -1.0f;
        }
        GpuCsmData data = {};
        data.params = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        if (s.uboMapped[m_FrameIndex % 3]) memcpy(s.uboMapped[m_FrameIndex % 3], &data, sizeof(data));   // 2026-08-17：% 3（3 帧槽——读写槽必须一致，此前 if (s.uboMapped[m_FrameIndex & 1]) memcpy(s.uboMapped[m_FrameIndex & 1], &data, sizeof(data)); 1 与读取 % 3 错配 → 帧 2 读垃圾 → 闪烁）
        return;
    }

    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 cameraPosF = glm::vec3(cameraWorld);   // 2026-08-14：提前（视锥诊断 + snap 参考系共用）
    float previousSplitFar = nearP;
    int cascadeCount = 0;

    for (int c = 0; c < MAX_CASCADES; c++) {
        // 等比分裂（LimitlessSquare GetDirectionalShadowCascadeMaxDistance）：
        // maxDist(i) = base * (scale^(i+1) - 1) / (scale - 1)
        float cascadeMaxDistance = SPLIT_BASE * (std::pow(SPLIT_SCALE, (float)(c + 1)) - 1.0f) / (SPLIT_SCALE - 1.0f);
        float cascadeNear = glm::max(nearP, previousSplitFar);
        float cascadeFar  = glm::min(farP, cascadeMaxDistance);
        previousSplitFar = cascadeMaxDistance;

        if (cascadeFar <= cascadeNear) {
            s.valid[c] = false;
            s.splitFars[c] = -1.0f;
            continue;
        }

        // 视锥 8 角点（view space 构造 → world；mikan RH 相机看 -z）
        // ⚠️ 2026-08-14 最终修复：主渲染 proj 被 proj[1][1] *= -1（EngineMain:1160，Y 翻转使画面正立）——
        // 反投影必须直接用翻转后的负 proj[1][1]（view.y = ndc.y*(-z)/proj[1][1]：ndc'=+1 → view 下方 ✓）。
        // 曾用 abs(proj[1][1])（早期消费端"同式 uv=ndc*0.5+0.5"镜像时代的双镜像抵消"修复"）——
        // 消费端 uv 已改 0.5-ndc.y*0.5（正确行序）后，abs 使视锥上下颠倒 → lightView 对准镜像视锥 → shadowmap 内容错位
        glm::vec3 corners[8];
        for (int i = 0; i < 8; i++) {
            float vz = (i & 4) ? -cascadeFar : -cascadeNear;
            float xs = (i & 1) ? 1.0f : -1.0f;
            float ys = (i & 2) ? 1.0f : -1.0f;
            // 透视：view.x = ndc.x * (-z) / proj[0][0]；view.y = ndc.y * (-z) / proj[1][1]（proj[1][1] 已含 Y 翻转负号）
            glm::vec4 viewPt(xs * (-vz) / proj[0][0], ys * (-vz) / proj[1][1], vz, 1.0f);
            corners[i] = glm::vec3(invView * viewPt);
        }

        glm::vec3 frustumCenter = glm::vec3(0.0f);
        for (int i = 0; i < 8; i++) frustumCenter += corners[i];
        frustumCenter /= 8.0f;

        float boundingRadius = 0.0f;
        for (int i = 0; i < 8; i++) boundingRadius = glm::max(boundingRadius, glm::length(corners[i] - frustumCenter));

        // 包围盒范围（×1.1 + 量化 1/2048 稳定 + 2 guard texels 防采样越界）
        float halfExtent = glm::max(0.5f, boundingRadius) * 1.1f;
        const float extentQuantize = 2048.0f;
        halfExtent = glm::ceil(halfExtent * extentQuantize) / extentQuantize;
        const int guardTexels = 2;
        double texelWorldSize = (halfExtent * 2.0) / CASCADE_SIZE;
        halfExtent += (float)texelWorldSize * guardTexels;
        texelWorldSize = (halfExtent * 2.0) / CASCADE_SIZE;

        // 光基（f = -lightDir 指向场景；防共线）
        glm::vec3 f = -lightDirN;
        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        if (glm::abs(glm::dot(f, worldUp)) > 0.999f) worldUp = glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 right = glm::normalize(glm::cross(f, worldUp));
        glm::vec3 up = glm::cross(right, f);

        // 深度范围（caster 向光源方向扩展，防级联边缘 caster 截断阴影）
        float casterExtend = glm::max(32.0f, cascadeFar * 1.5f);

        // ⭐ 防阴影抖动（LimitlessSquare 原版语义，2026-08-15 修正移植错误）：
        // anchor 网格 = 阴影覆盖范围（halfExtent*2，原版 Graphics.cs:3058 anchorGridWorldSize）——
        // ⚠️ 曾误传 texelWorldSize（1 texel）→ anchor 每 texel/2 跳一次 → round 补偿不精确（±right_x 四舍五入 0/±1）
        //   → snappedCenter 残余跳变 → 移动相机阴影抖动。原版 grid=1024 texel：anchor 跳变时 round 变化 ±1024·right_x
        //   与 anchor 跳变精确抵消 → 连续。Snap 函数保持原版传参（frustumCenter 绝对 + anchor 相对——原版同式）。
        glm::vec3 anchorRelative = GetStableAnchorRelative(slot, c, cameraWorld, halfExtent * 2.0);
        glm::vec3 snappedCenter = SnapCenterToTexelStable(frustumCenter, anchorRelative, right, up, f, texelWorldSize);

        glm::vec3 lightPosition = snappedCenter - lightDirN * (cascadeFar + casterExtend + 32.0f);
        glm::mat4 lightView = glm::lookAt(lightPosition, snappedCenter, up);

        float minZ = std::numeric_limits<float>::infinity();
        float maxZ = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 8; i++) {
            glm::vec4 ls0 = lightView * glm::vec4(corners[i], 1.0f);
            glm::vec4 ls1 = lightView * glm::vec4(corners[i] - lightDirN * casterExtend, 1.0f);
            minZ = glm::min(minZ, glm::min(ls0.z, ls1.z));
            maxZ = glm::max(maxZ, glm::max(ls0.z, ls1.z));
        }

        // ⚠️ 本 TU 已定义 GLM_FORCE_DEPTH_ZERO_TO_ONE：glm::ortho 是 ZO（0..1 深度，近→0 远→1）——
        // 与 Vulkan viewport 深度直存语义匹配（附件 = ndc.z ∈ [0,1]），消费端 currentDepth = ndc.z 同式
        float zPadding = glm::max(4.0f, (cascadeFar - cascadeNear) * 0.5f);
        float nearPlane = glm::max(0.1f, -maxZ - zPadding);
        float farPlane  = glm::max(nearPlane + 1.0f, -minZ + zPadding);

        glm::mat4 lightProjection = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, nearPlane, farPlane);

        s.shadowMatrices[c] = lightProjection * lightView;
        s.splitFars[c] = cascadeFar;   // 原版同式（Lit.frag 消费端用真实 SplitRange.y）
        s.valid[c] = true;
        cascadeCount++;
    }

    // 上传 UBO（无效级联矩阵置 0、split -1；shader 端按 viewDepth 逐级联跳过）
    GpuCsmData data = {};
    for (int c = 0; c < MAX_CASCADES; c++) {
        data.shadowMatrices[c] = s.valid[c] ? s.shadowMatrices[c] : glm::mat4(0.0f);
        data.splitDepths[c] = s.splitFars[c];
    }
    data.params = glm::vec4((float)cascadeCount, cascadeCount > 0 ? 1.0f : 0.0f, 1.0f, 0.0f);
    if (s.uboMapped[m_FrameIndex % 3]) memcpy(s.uboMapped[m_FrameIndex % 3], &data, sizeof(data));   // 2026-08-17：% 3（3 帧槽——读写槽必须一致，此前 if (s.uboMapped[m_FrameIndex & 1]) memcpy(s.uboMapped[m_FrameIndex & 1], &data, sizeof(data)); 1 与读取 % 3 错配 → 帧 2 读垃圾 → 闪烁）

    // ⚠️ 2026-08-14 诊断（一次性）：UBO 矩阵元素 + CPU 模拟消费端变换（验证矩阵对准视锥中心）
    static bool s_matLogged = false;
    if (!s_matLogged && slot == 0 && s.valid[0]) {
        s_matLogged = true;
        const glm::mat4& m = s.shadowMatrices[0];
        LOGI("[CSM-MAT] UBO c0: m00=%.4f m11=%.4f m22=%.4f m33=%.4f row3=(%.4f,%.4f,%.4f,%.4f)",
             m[0][0], m[1][1], m[2][2], m[3][3], m[3][0], m[3][1], m[3][2], m[3][3]);
        glm::vec4 clip = m * glm::vec4(cameraPosF, 1.0f);
        LOGI("[CSM-MAT] camera→clip=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f) depth=%.3f",
             clip.x, clip.y, clip.z, clip.w,
             clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f, clip.z / clip.w * 0.5f + 0.5f);
    }
}

void CascadeShadowRenderer::BeginCascade(VkCommandBuffer cmd, int slot, int cascade, int width, int height)
{
    if (slot < 0 || slot >= MAX_SLOTS || cascade < 0 || cascade >= MAX_CASCADES) return;
    Slot& s = m_Slots[slot];
    if (!s.valid[cascade] || s.framebuffers[cascade] == VK_NULL_HANDLE) return;

    VkClearValue clear = {};
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = m_RenderPass;
    rpbi.framebuffer = s.framebuffers[cascade];
    rpbi.renderArea = { 0, 0, (uint32_t)width, (uint32_t)height };
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {};
    vp.width = (float)width;
    vp.height = (float)height;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = {};
    sc.extent = { (uint32_t)width, (uint32_t)height };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void CascadeShadowRenderer::EndCascade(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
}

void CascadeShadowRenderer::PrepareRender(VkCommandBuffer cmd, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    // ⚠️ 2026-08-14 修复：上帧 Finalize 后 layout=SHADER_READ_ONLY，本帧直接 begin render pass 写深度是 layout 违例；
    // 且多帧 in-flight 下本帧渲染可能与前帧合成采样并发——srcStage 用 ALL_COMMANDS 保守同步（每帧一次，不在热路径）
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = m_Slots[slot].image;
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    imb.subresourceRange.baseMipLevel = 0;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.baseArrayLayer = 0;
    imb.subresourceRange.layerCount = MAX_CASCADES;
    imb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
}

void CascadeShadowRenderer::Finalize(VkCommandBuffer cmd, int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    // 2D array：depth attachment → shader read（合成 pass 采样）
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = m_Slots[slot].image;
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    imb.subresourceRange.baseMipLevel = 0;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.baseArrayLayer = 0;
    imb.subresourceRange.layerCount = MAX_CASCADES;
    imb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // ⚠️ 2026-08-14 修复：深度写入发生在 LATE_FRAGMENT_TESTS（不是 FRAGMENT_SHADER）——
    // 旧 srcStage=FRAGMENT_SHADER_BIT 未同步深度写 → 合成采样可能读到旧帧内容（阴影漂移/滞后）
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
}
