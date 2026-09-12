#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanLightingCulling.h"

#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "SceneRenderer.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

// 收集场景所有 Point 型 LightComponent → GPU 布局（vec4 position_range / vec4 color_intensity）
// 与 fullscreen.frag binding 11 UBO（std140，32×32B + int count + padding = 1040B）对齐
static constexpr int MAX_POINT_LIGHTS = 32;
struct GpuPointLight {
    glm::vec4 position_range;    // xyz = 世界位置（Transform），w = range
    glm::vec4 color_intensity;   // rgb = 颜色，w = intensity
    glm::vec4 shadow_info;
};
// FrameRender 阴影渲染直接消费——两处必须同源，否则 UBO 槽号与渲染列表错位
static SceneRenderer::ShadowLight g_shadowLightList[PointShadowRenderer::MAX_SHADOW_LIGHTS];
static int g_shadowLightCount = 0;
static int CollectPointLights(GpuPointLight* out, int maxCount,
                              const RenderWorld& world,
                              SceneRenderer::ShadowLight* shadowOut = nullptr, int maxShadow = 0) {
    int n = 0;
    int shadowSlot = 0;
    for (const RenderLightData& light : world.lights) {
        if (n >= maxCount) break;
        if (light.type != RenderLightType::Point) continue;

        const glm::vec3 worldPosition = light.position;
        out[n].position_range = glm::vec4(
            worldPosition, light.range > 0.0f ? light.range : 1.0f);
        out[n].color_intensity = glm::vec4(light.color, light.intensity);
        out[n].shadow_info = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
        if (light.castShadow && shadowOut && shadowSlot < maxShadow) {
            out[n].shadow_info.x = static_cast<float>(shadowSlot);
            shadowOut[shadowSlot].position = worldPosition;
            shadowOut[shadowSlot].range = light.range > 0.0f ? light.range : 1.0f;
            shadowSlot++;
        }
        n++;
    }
    return n;
}
// 点光源 UBO（惰性创建 + 常驻映射；每帧写满 32 个槽 + count，未使用槽清零）
static VkBuffer GetPointLightBuffer(void** mappedPtr) {
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    static void* mapped = nullptr;
    if (buf != VK_NULL_HANDLE) {
        if (mappedPtr) *mappedPtr = mapped;
        return buf;
    }
    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = MAX_POINT_LIGHTS * (uint32_t)sizeof(GpuPointLight) + 16;   // 32×32 + count/padding
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i; break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindBufferMemory(g_Device, buf, mem, 0);
    if (vkMapMemory(g_Device, mem, 0, binfo.size, 0, &mapped) == VK_SUCCESS) {
        memset(mapped, 0, binfo.size);   // 初始清零（未用槽）
    }
    if (mappedPtr) *mappedPtr = mapped;
    return buf;
}
// 每帧：收集场景点光源 → 写 UBO（未用槽保持 0），返回 buffer 供 descriptor 绑定
static VkBuffer UpdatePointLightBuffer(const RenderWorld& world) {
    void* mapped = nullptr;
    VkBuffer buf = GetPointLightBuffer(&mapped);
    if (!buf || !mapped) return buf;
    GpuPointLight pls[MAX_POINT_LIGHTS] = {};
    int n = CollectPointLights(pls, MAX_POINT_LIGHTS, world,
                               g_shadowLightList, PointShadowRenderer::MAX_SHADOW_LIGHTS);
    g_shadowLightCount = 0;
    for (int i = 0; i < n; i++) {
        if (pls[i].shadow_info.x >= 0.0f) g_shadowLightCount++;
    }
    memcpy(mapped, pls, sizeof(pls));
    int* countPtr = (int*)((char*)mapped + MAX_POINT_LIGHTS * (int)sizeof(GpuPointLight));
    *countPtr = n;
    return buf;
}

VkBuffer UpdatePointLightBuffer() {
    return UpdatePointLightBuffer(g_SceneRenderer.GetRenderWorld());
}

// 12×12 屏幕 tile × 24 深度切片（指数分割），view 空间 AABB——与 cluster_cull.comp / fullscreen.frag 严格一致
// CPU 每帧算 AABB（2.7 万次求交 <0.1ms）→ GPU compute 球-AABB cull → grid SSBO 供合成 pass 查询
static constexpr int CLUSTER_X = 12, CLUSTER_Y = 12, CLUSTER_Z = 24;
static constexpr int CLUSTER_COUNT = CLUSTER_X * CLUSTER_Y * CLUSTER_Z;   // 3456
static constexpr uint32_t CLUSTER_GRID_SIZE = 16 + 3456 * 168;            // params vec4 + Cluster[3456]（std430）

struct GpuCluster {
    glm::vec4 minPoint;   // view 空间 AABB
    glm::vec4 maxPoint;
    uint32_t count;
    uint32_t pad;
    uint32_t lightIndices[MAX_POINT_LIGHTS];
};

// scene/game 两套（各自相机的 cluster 独立；pipeline/ds layout 共用）
static bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** mapped);   // 定义见下方
static VkDescriptorSet EnsureClusterDescriptorSet(VkBuffer aabbBuf, VkBuffer gridBuf);   // 定义见下方
struct ClusterCulling {
    VkBuffer aabbBuf = VK_NULL_HANDLE, gridBuf = VK_NULL_HANDLE;
    VkDeviceMemory aabbMem = VK_NULL_HANDLE, gridMem = VK_NULL_HANDLE;
    void* aabbMapped = nullptr;
    void* gridMapped = nullptr;
    VkDescriptorSet ds = VK_NULL_HANDLE;

    bool Ensure() {
        if (aabbBuf && gridBuf && ds) return true;
        // AABB SSBO：3456 × 2 × vec4
        if (!aabbBuf) {
            if (!CreateHostBuffer(3456 * 2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, aabbBuf, aabbMem, &aabbMapped)) return false;
        }
        // grid SSBO：头部 params（CPU 写）+ Cluster[3456]（GPU cull 写）
        if (!gridBuf) {
            if (!CreateHostBuffer(CLUSTER_GRID_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gridBuf, gridMem, &gridMapped)) return false;
            memset(gridMapped, 0, CLUSTER_GRID_SIZE);
        }
        if (!ds) ds = EnsureClusterDescriptorSet(aabbBuf, gridBuf);
        return ds != VK_NULL_HANDLE;
    }
};

static ClusterCulling g_SceneCluster, g_GameCluster;
static VkPipelineLayout g_ClusterPipeLayout = VK_NULL_HANDLE;
static VkPipeline g_ClusterPipeline = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_ClusterDSLayout = VK_NULL_HANDLE;

// 通用 host-visible buffer 创建（与 GetPointLightBuffer 同款）
static bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** mapped) {
    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = size;
    binfo.usage = usage;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i; break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return false;
    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_Device, buf, mem, 0);
    return vkMapMemory(g_Device, mem, 0, size, 0, mapped) == VK_SUCCESS;
}

// compute pipeline + descriptor（惰性，一次；binding 0=光源 UBO、1=AABB SSBO、2=grid SSBO）
static bool EnsureClusterPipeline() {
    if (g_ClusterPipeline) return true;
    // 跨平台读取 spv：Android 上 APK assets 不能走 std::ifstream，同 VoxelMeshGPUCulling 用 SDL IO
    const std::string spvPath = EngineConfig::GetShaderPath("cluster_cull.comp.spv");
    std::vector<char> code;
    if (SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb")) {
        Sint64 sz = SDL_GetIOSize(io);
        if (sz > 0) {
            code.resize((size_t)sz);
            if (SDL_ReadIO(io, code.data(), (size_t)sz) != (size_t)sz) code.clear();
        }
        SDL_CloseIO(io);
    }
    if (code.empty()) { LOGE("[ClusterCulling] shader not found: cluster_cull.comp.spv"); return false; }
    VkShaderModule sm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = code.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    if (vkCreateShaderModule(g_Device, &sci, g_Allocator, &sm) != VK_SUCCESS) return false;

    // descriptor set layout：0 UBO 光源、1 SSBO AABB（readonly）、2 SSBO grid（writeonly）
    VkDescriptorSetLayoutBinding cb[3] = {};
    cb[0].binding = 0; cb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; cb[0].descriptorCount = 1; cb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cb[1].binding = 1; cb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cb[1].descriptorCount = 1; cb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cb[2].binding = 2; cb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cb[2].descriptorCount = 1; cb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dli = {};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 3;
    dli.pBindings = cb;
    if (vkCreateDescriptorSetLayout(g_Device, &dli, g_Allocator, &g_ClusterDSLayout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli = {};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_ClusterDSLayout;
    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 64;   // mat4
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_Device, &pli, g_Allocator, &g_ClusterPipeLayout) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo cpi = {};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = g_ClusterPipeLayout;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ClusterPipeline) != VK_SUCCESS) return false;
    vkDestroyShaderModule(g_Device, sm, g_Allocator);
    return true;
}

// 每实例 descriptor set（光源 buffer 共用；AABB/grid 各自）
static VkDescriptorSet EnsureClusterDescriptorSet(VkBuffer aabbBuf, VkBuffer gridBuf) {
    if (!EnsureClusterPipeline()) return VK_NULL_HANDLE;
    VkDescriptorPoolSize ps[3] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 2;
    VkDescriptorPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_Device, &pci, g_Allocator, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_ClusterDSLayout;
    if (vkAllocateDescriptorSets(g_Device, &ai, &ds) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkDescriptorBufferInfo infoLight = {};
    infoLight.buffer = GetPointLightBuffer(nullptr);
    infoLight.offset = 0;
    infoLight.range = 1552;   // 32×(16×3) + 16（GpuPointLight 48B×3 vec4 + count）
    VkDescriptorBufferInfo infoAABB = {};
    infoAABB.buffer = aabbBuf;
    infoAABB.offset = 0;
    infoAABB.range = 3456 * 2 * 16;
    VkDescriptorBufferInfo infoGrid = {};
    infoGrid.buffer = gridBuf;
    infoGrid.offset = 0;
    infoGrid.range = CLUSTER_GRID_SIZE;
    VkWriteDescriptorSet wr[3] = {};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = ds; wr[0].dstBinding = 0;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].descriptorCount = 1; wr[0].pBufferInfo = &infoLight;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = ds; wr[1].dstBinding = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].descriptorCount = 1; wr[1].pBufferInfo = &infoAABB;
    wr[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[2].dstSet = ds; wr[2].dstBinding = 2;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].descriptorCount = 1; wr[2].pBufferInfo = &infoGrid;
    vkUpdateDescriptorSets(g_Device, 3, wr, 0, nullptr);
    return ds;
}

// CPU 算 cluster AABB（view 空间）→ 写 AABB SSBO + grid 头部 params（near/far/screenW/H）
static void ComputeClusterAABB(ClusterCulling& cc, const glm::mat4& proj, float screenW, float screenH) {
    if (!cc.aabbMapped || !cc.gridMapped) return;
    //   m22=(f+n)/(n-f)、m32=2fn/(n-f) → near = m32/(m22-1)、far = m32/(m22+1)（勿用 0-1 约定公式！）
    const float m22 = proj[2][2], m32 = proj[3][2];
    const float nearP = m32 / (m22 - 1.0f);
    const float farP = m32 / (m22 + 1.0f);
    const float tanH = 1.0f / proj[1][1];
    const float tanW = tanH * (proj[1][1] / proj[0][0]);
    // ⚠️ shader 布局是分离数组（vec4 minPts[3456]; vec4 maxPts[3456];）——必须分离写！
    // 曾交错写（min0,max0,min1,max1...）→ 每个 cluster 读到别的 cluster 的 AABB → 剔除错乱（网格遮罩）
    glm::vec4* aabbMin = (glm::vec4*)cc.aabbMapped;
    glm::vec4* aabbMax = (glm::vec4*)cc.aabbMapped + CLUSTER_COUNT;
    for (int tz = 0; tz < CLUSTER_Z; tz++) {
        const float zNear = nearP * powf(farP / nearP, tz / (float)CLUSTER_Z);
        const float zFar = nearP * powf(farP / nearP, (tz + 1) / (float)CLUSTER_Z);
        for (int ty = 0; ty < CLUSTER_Y; ty++) {
            for (int tx = 0; tx < CLUSTER_X; tx++) {
                glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
                for (int cy = 0; cy < 2; cy++) {
                    const float ndcY = (ty + cy) * 2.0f / CLUSTER_Y - 1.0f;
                    for (int cx = 0; cx < 2; cx++) {
                        const float ndcX = (tx + cx) * 2.0f / CLUSTER_X - 1.0f;
                        for (int cz = 0; cz < 2; cz++) {
                            const float z = cz ? -zFar : -zNear;   // view z（负朝前）
                            glm::vec3 p(ndcX * tanW * -z, ndcY * tanH * -z, z);
                            mn = glm::min(mn, p); mx = glm::max(mx, p);
                        }
                    }
                }
                const int idx = tz * (CLUSTER_X * CLUSTER_Y) + ty * CLUSTER_X + tx;
                aabbMin[idx] = glm::vec4(mn, 1.0f);
                aabbMax[idx] = glm::vec4(mx, 1.0f);
            }
        }
    }
    ((glm::vec4*)cc.gridMapped)[0] = glm::vec4(nearP, farP, screenW, screenH);   // params 头部（cull 不碰）
}

// 每帧：算 AABB + dispatch cull（必须在合成 render pass 前、光源 UBO 更新后）
static constexpr int CLUSTER_MIN_LIGHTS = 16;
static void DispatchClusterCull(VkCommandBuffer cmd, ClusterCulling& cc, const glm::mat4& view, const glm::mat4& proj, float screenW, float screenH) {
    void* plMapped = nullptr;
    GetPointLightBuffer(&plMapped);
    if (plMapped) {
        const int cnt = *(int*)((char*)plMapped + MAX_POINT_LIGHTS * (int)sizeof(GpuPointLight));
        if (cnt <= CLUSTER_MIN_LIGHTS) return;   // 少量光源：不 dispatch（grid 残留旧数据，fragment 不读）
    }
    if (!EnsureClusterPipeline()) return;
    if (!cc.Ensure()) return;
    ComputeClusterAABB(cc, proj, screenW, screenH);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ClusterPipeline);
    vkCmdPushConstants(cmd, g_ClusterPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, &view);   // 世界→view（cull 光源变换）
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ClusterPipeLayout, 0, 1, &cc.ds, 0, nullptr);
    vkCmdDispatch(cmd, (CLUSTER_COUNT + 127) / 128, 1, 1);   // 3456/128 = 27 groups
    // compute 写 grid → 合成 pass fragment 读：必须 buffer barrier（否则读到旧 count=0）
    VkBufferMemoryBarrier bmb = {};
    bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = cc.gridBuf;
    bmb.offset = 0;
    bmb.size = CLUSTER_GRID_SIZE;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &bmb, 0, nullptr);
}

// 合成 quad 绑定用的 grid buffer（惰性 Ensure；合成 pass 读 cull 结果）
VkBuffer GetSceneClusterGridBuffer() { return g_SceneCluster.Ensure() ? g_SceneCluster.gridBuf : VK_NULL_HANDLE; }
VkBuffer GetGameClusterGridBuffer() { return g_GameCluster.Ensure() ? g_GameCluster.gridBuf : VK_NULL_HANDLE; }


void DispatchSceneClusterCull(VkCommandBuffer commandBuffer,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              float screenW,
                              float screenH)
{
    DispatchClusterCull(commandBuffer, g_SceneCluster, view, proj, screenW, screenH);
}

void DispatchGameClusterCull(VkCommandBuffer commandBuffer,
                             const glm::mat4& view,
                             const glm::mat4& proj,
                             float screenW,
                             float screenH)
{
    DispatchClusterCull(commandBuffer, g_GameCluster, view, proj, screenW, screenH);
}

void RenderPointShadowMaps(VkCommandBuffer commandBuffer)
{
    if (g_shadowLightCount <= 0) return;
    g_SceneRenderer.RenderPointShadowMaps(
        commandBuffer, g_shadowLightList, g_shadowLightCount,
        PointShadowRenderer::SHADOW_MAP_SIZE);
}

