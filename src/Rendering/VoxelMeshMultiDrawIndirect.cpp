#include "Rendering/VoxSurfaceAttributes.h"
#include "Rendering/VoxelMeshMultiDrawIndirect.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "VulkanManager.h"
#include "Rendering/RendererBase.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/HiZHistory.h"
#include "AABB.h"
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <thread>
#include <future>
#include <execution>
#include <mutex>
#include "Rendering/RenderStats.h"
#include "Core/LogStream.h"

extern SceneRenderer g_SceneRenderer;
extern RenderTarget g_GameRenderTarget;

namespace {
    constexpr glm::vec3 FACE_NORMALS[6] = {
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f)
    };
}

VoxelMeshMultiDrawIndirect::VoxelMeshMultiDrawIndirect()
    : m_supportsMDI(false),m_supportsComputeShader(false),m_totalVertices(0),m_totalIndices(0),
      m_totalInstances(0),m_geometryDataDirty(true) {}
VoxelMeshMultiDrawIndirect::~VoxelMeshMultiDrawIndirect() {
    for(auto& entry:m_GpuFrames)
        if(entry.second->sourceView && g_Device)vkDestroyBufferView(g_Device,entry.second->sourceView,g_Allocator);
    m_GpuFrames.clear();
    if(g_Device) {
        if(m_IndirectCullPipeline)vkDestroyPipeline(g_Device,m_IndirectCullPipeline,g_Allocator);
        if(m_IndirectCullLayout)vkDestroyPipelineLayout(g_Device,m_IndirectCullLayout,g_Allocator);
        if(m_IndirectCullPool)vkDestroyDescriptorPool(g_Device,m_IndirectCullPool,g_Allocator);
        if(m_IndirectCullSetLayout)vkDestroyDescriptorSetLayout(g_Device,m_IndirectCullSetLayout,g_Allocator);
    }
}

bool VoxelMeshMultiDrawIndirect::Initialize(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices)
{
    m_supportsMDI=CheckMultiDrawIndirectSupport();
    m_supportsComputeShader=CheckComputeShaderSupport();
    return m_supportsComputeShader;
}

bool VoxelMeshMultiDrawIndirect::CheckMultiDrawIndirectSupport()
{
    if (g_PhysicalDevice == VK_NULL_HANDLE) {
        LOGSTREAM(Error) << "[VoxelMeshMultiDrawIndirect] g_PhysicalDevice is null, cannot check MDI support!" << std::endl;
        return false;
    }
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &features);
    return features.multiDrawIndirect;
}

bool VoxelMeshMultiDrawIndirect::CheckComputeShaderSupport()
{
    if (g_PhysicalDevice == VK_NULL_HANDLE) {
        LOGSTREAM(Error) << "[VoxelMeshMultiDrawIndirect] g_PhysicalDevice is null, cannot check compute shader support!" << std::endl;
        return false;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);

    // 检查是否支持计算着色器
    bool supportsCompute = (properties.limits.maxComputeWorkGroupCount[0] > 0);

    return supportsCompute;
}


void VoxelMeshMultiDrawIndirect::AddVoxelModel(uint64_t entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color)
{
    // 检查是否需要扩容实例和绘制命令缓冲区


    VoxelModelData modelData;
    modelData.entityId = entityId;
    modelData.voxPath = voxPath;
    modelData.transform = transform;
    modelData.color = color;
    modelData.firstInstance = m_totalInstances;
    modelData.instanceCount = 1;
    modelData.visible = true;  // 新添加的模型默认可见

    // 初始化脏标记
    modelData.transformDirty = true;
    modelData.colorDirty = true;
    modelData.staticDirty = true;

    // 暂时设置为 0，在 MergeGeometryData 中会更新
    modelData.firstVertex = 0;
    modelData.vertexCount = 0;
    modelData.firstIndex = 0;
    modelData.indexCount = 0;

    auto existingGroup = m_GroupSlots.find(voxPath);
    if (existingGroup != m_GroupSlots.end()) {
        auto& group = m_rendererGroups[existingGroup->second];
        m_ModelSlots[entityId] = {existingGroup->second, group.models.size()};
        group.models.push_back(modelData);
        group.totalInstances++;
    } else {
        RendererGroup group;
        group.voxPath = voxPath; group.renderer = renderer;
        group.models.push_back(modelData); group.totalInstances = 1;
        m_GroupSlots[voxPath] = m_rendererGroups.size();
        m_ModelSlots[entityId] = {m_rendererGroups.size(), 0};
        m_rendererGroups.push_back(std::move(group));
    }
    // 更新总实例数量
    m_totalInstances += modelData.instanceCount;

    m_geometryDataDirty = true;
}

void VoxelMeshMultiDrawIndirect::UpdateVoxelModel(uint64_t entityId, const std::string& voxPath,
    const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color,
    bool visible, const glm::mat4* previousModel)
{
    auto it = m_ModelSlots.find(entityId);
    if (it != m_ModelSlots.end()) {
        auto& model = m_rendererGroups[it->second.first].models[it->second.second];
        if (model.voxPath == voxPath && m_rendererGroups[it->second.first].renderer == renderer) {
            model.visible = visible;
            model.cachedInstanceData.prevModel = previousModel ? *previousModel : transform;
            model.transformDirty |= model.transform != transform;
            model.colorDirty |= model.color != color;
            model.transform = transform;
            model.color = color;
            return;
        }
        model.visible = false; // Resource changes migrate the instance to its new batch.
        m_ModelSlots.erase(it);
    }
    AddVoxelModel(entityId, voxPath, renderer, transform, color);
    it = m_ModelSlots.find(entityId);
    if (it == m_ModelSlots.end()) return; // Allocation failure must not recurse.
    auto& model = m_rendererGroups[it->second.first].models[it->second.second];
    model.visible = visible;
    model.cachedInstanceData.prevModel = previousModel ? *previousModel : transform;
}
// 执行 GPU 剔除


void VoxelMeshMultiDrawIndirect::MergeGeometryData()
{
        if(!m_geometryDataDirty)return;
        m_meshCache.clear();m_totalVertices=0;m_totalIndices=0;m_totalInstances=0;
        for(auto& group:m_rendererGroups) {
            auto it=m_meshCache.find(group.renderer);
            if(it==m_meshCache.end()) {
                const auto& data=group.renderer->GetMeshData();
                it=m_meshCache.emplace(group.renderer,MeshCacheEntry{m_totalVertices,data.vertexCount,m_totalIndices,data.indexCount}).first;
                m_totalVertices+=data.vertexCount;m_totalIndices+=data.indexCount;
            }
            const auto& entry=it->second;
            for(auto& model:group.models) {
                model.firstVertex=entry.firstVertex;model.vertexCount=entry.vertexCount;
                model.firstIndex=entry.firstIndex;model.indexCount=entry.indexCount;
                model.firstInstance=m_totalInstances++;model.instanceCount=1;
            }
        }
        m_geometryDataDirty=false;return;

}


void VoxelMeshMultiDrawIndirect::Render(VkCommandBuffer commandBuffer, int width, int height,
    const glm::mat4& projView, const glm::mat4& prevProjView, const glm::mat4& cullProjView,
    const glm::vec3& cameraPosition, bool useDualFrustumCulling, bool enableBackfaceCulling, int viewSlot)
{
    const uint32_t key = GetCurrentFrameIndex() * 2 + static_cast<uint32_t>(viewSlot);
    auto it = m_GpuFrames.find(key);
    if (it == m_GpuFrames.end() || it->second->epoch != g_SceneRenderer.GetRenderWorld().frameNumber ||
        it->second->drawCount == 0 || m_rendererGroups.empty()) return;
    GpuFrame& frame = *it->second;
    const VoxRenderer* renderer = m_rendererGroups.front().renderer;
    if (!renderer || !renderer->GetQuadPipeline() || !frame.useQuads) return;
    VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    const auto pipeline=renderer->GetQuadPipeline();
    const auto layout=renderer->GetQuadPipelineLayout();
    const auto descriptor=frame.quadDescriptor;
    vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&descriptor,0,nullptr);
    struct Push { glm::mat4 current, previous; glm::vec4 camera; } push{projView,prevProjView,glm::vec4(cameraPosition,0)};
    vkCmdPushConstants(commandBuffer,layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
    VkDeviceSize offsets[]{0,0};
    const auto instanceBuffer=frame.instances.GetBuffer();vkCmdBindVertexBuffers(commandBuffer,0,1,&instanceBuffer,offsets);
    vkCmdBindIndexBuffer(commandBuffer,VoxRenderer::GetSharedQuadIndexBuffer(),0,VK_INDEX_TYPE_UINT16);
    static bool reported=false;if(!reported){reported=true;LOGSTREAM(Info)<<"[VoxQuad] active: 4 bytes/quad + shared plane runs, 4 generated vertices + shared uint16 indices, GPU culling + indexed MDI"<<std::endl;}
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);
    // Respect both multiDrawIndirect support and maxDrawIndirectCount.
    const uint32_t batchSize = m_supportsMDI ? std::max(1u, properties.limits.maxDrawIndirectCount) : 1u;
    for (uint32_t first = 0; first < frame.drawCount;) {
        const uint32_t count = std::min(batchSize, frame.drawCount - first);
        vkCmdDrawIndexedIndirect(commandBuffer,frame.quadCommands.GetBuffer(),VkDeviceSize(first)*sizeof(VkDrawIndexedIndirectCommand),count,sizeof(VkDrawIndexedIndirectCommand));
        first += count;
    }
}
bool VoxelMeshMultiDrawIndirect::CreateIndirectCullPipeline()
{
    if (m_IndirectCullPipeline) return true;
    if (!m_supportsComputeShader) return false;
    const std::string path = EngineConfig::GetShaderPath("voxel_indirect_cull.comp.spv");
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    const Sint64 length = SDL_GetIOSize(io);
    if (length <= 0 || length % 4 != 0) { SDL_CloseIO(io); return false; }
    std::vector<uint32_t> code(static_cast<size_t>(length) / 4);
    const bool loaded = SDL_ReadIO(io, code.data(), static_cast<size_t>(length)) == static_cast<size_t>(length);
    SDL_CloseIO(io);
    if (!loaded) return false;
    VkDescriptorSetLayoutBinding bindings[7]{};
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
            (i == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 7; setInfo.pBindings = bindings;
    if (!m_IndirectCullSetLayout && vkCreateDescriptorSetLayout(g_Device, &setInfo, g_Allocator, &m_IndirectCullSetLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &m_IndirectCullSetLayout;
    VkPushConstantRange phaseRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &phaseRange;
    if (!m_IndirectCullLayout && vkCreatePipelineLayout(g_Device, &layoutInfo, g_Allocator, &m_IndirectCullLayout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 128}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 256; poolInfo.poolSizeCount = 4; poolInfo.pPoolSizes = sizes;
    if (!m_IndirectCullPool && vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_IndirectCullPool) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = static_cast<size_t>(length); shaderInfo.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shaderInfo, g_Allocator, &shader) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = m_IndirectCullLayout;
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipelineInfo.stage.module = shader; pipelineInfo.stage.pName = "main";
    const VkResult result = vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_IndirectCullPipeline);
    vkDestroyShaderModule(g_Device, shader, g_Allocator);
    return result == VK_SUCCESS;
}

bool VoxelMeshMultiDrawIndirect::HasPreparedGpuResults(int viewSlot, uint64_t epoch) const
{
    if (viewSlot < 0 || viewSlot > 1) return false;
    auto it = m_GpuFrames.find(GetCurrentFrameIndex() * 2 + static_cast<uint32_t>(viewSlot));
    return it != m_GpuFrames.end() && it->second->epoch == epoch && it->second->quadDescriptor != VK_NULL_HANDLE;
}
bool VoxelMeshMultiDrawIndirect::BeginGpuCollection(uint64_t epoch)
{
    if (m_GpuCollectionEpoch == epoch) return false;
    m_GpuCollectionEpoch = epoch;
    for (auto& group : m_rendererGroups)
        for (auto& model : group.models) model.visible = false;
    return true;
}

void VoxelMeshMultiDrawIndirect::PrepareGpuCull(VkCommandBuffer commandBuffer, int viewSlot, uint64_t epoch,
    const glm::mat4& rasterProjView, const glm::mat4& mainProjView,
    bool mainFrustum, bool sceneFrustum, const glm::vec3& directionCamera)
{
    if (viewSlot < 0 || viewSlot > 1) return;
    bool topologyChanged = false;
    for (auto& group : m_rendererGroups) {
        const size_t oldSize = group.models.size();
        group.models.erase(std::remove_if(group.models.begin(), group.models.end(),
            [](const VoxelModelData& model) { return !model.visible; }), group.models.end());
        group.totalInstances = group.models.size();
        topologyChanged |= oldSize != group.models.size();
    }
    m_rendererGroups.erase(std::remove_if(m_rendererGroups.begin(), m_rendererGroups.end(),
        [](const RendererGroup& group) { return group.models.empty(); }), m_rendererGroups.end());
    if (topologyChanged) {
        m_geometryDataDirty = true;
        m_ModelSlots.clear();
        m_GroupSlots.clear();
        for (size_t g = 0; g < m_rendererGroups.size(); ++g) m_GroupSlots[m_rendererGroups[g].voxPath] = g;
        for (size_t g = 0; g < m_rendererGroups.size(); ++g)
            for (size_t i = 0; i < m_rendererGroups[g].models.size(); ++i)
                m_ModelSlots[m_rendererGroups[g].models[i].entityId] = {g, i};
    }
    for(auto& group:m_rendererGroups)if(group.geometryRevision!=group.renderer->GetGeometryRevision()){group.geometryRevision=group.renderer->GetGeometryRevision();m_geometryDataDirty=true;}
    const bool quadGeometryChanged=m_geometryDataDirty;
    MergeGeometryData();
    struct Source { InstanceData instance; glm::vec4 localMax; };
    struct Params { glm::uvec4 counts; glm::vec4 planes[12]; glm::vec4 camera;
        glm::mat4 hizViewProj; glm::uvec4 hizParams; };
    static_assert(sizeof(InstanceData) == 176 && sizeof(Source) == 192 && sizeof(FaceDrawCommand) == 32);
    std::vector<Source> sources;
    std::vector<glm::uvec4> candidates;
    std::vector<FaceDrawCommand> commands;
    sources.reserve(m_totalInstances); candidates.reserve(m_totalInstances * 2);
    uint32_t outputCapacity = 0;
    bool hasSegmentedDraws = false;
    for (auto& group : m_rendererGroups) {
        for (const auto& model : group.models) {
            InstanceData instance{};
            instance.model = model.transform;
            instance.prevModel = model.cachedInstanceData.prevModel;
            instance.albedoColor = model.color;
            instance.materialData = glm::vec4(0, 1, 1, 0);
            instance.worldMinBounds = group.renderer->GetMinBounds();
            instance.voxelSize = group.renderer->GetVoxelSize();
            sources.push_back({instance, glm::vec4(group.renderer->GetMaxBounds(), 0)});
        }
        glm::uvec4 commandA(UINT32_MAX), commandB(UINT32_MAX);
        for (uint32_t face = 0; face < 6; ++face) {
            const auto& geometry = group.renderer->GetMeshData().faceGroups[face];
            if (!geometry.indexCount) continue;
            const auto& model = group.models.front();
            uint32_t index = static_cast<uint32_t>(commands.size());
            (face < 4 ? commandA[face] : commandB[face - 4]) = index;
            ForEachVoxIndexSegment(geometry.firstIndex, geometry.indexCount,
                [&](size_t first, size_t count, size_t vertex) {
                    hasSegmentedDraws |= commands.size() != index;
                    commands.push_back({static_cast<uint32_t>(count), 0,
                        static_cast<uint32_t>(model.firstIndex + first),
                        static_cast<int32_t>(model.firstVertex + vertex), outputCapacity, face, 1, index});
                });
            outputCapacity += static_cast<uint32_t>(group.models.size());
        }
        for (size_t i = 0; i < group.models.size(); ++i) {
            candidates.push_back(commandA); candidates.push_back(commandB);
        }
    }
    const uint32_t key = GetCurrentFrameIndex() * 2 + static_cast<uint32_t>(viewSlot);
    auto& entry = m_GpuFrames[key];
    if (!entry) entry = std::make_unique<GpuFrame>();
    GpuFrame& frame = *entry;
    frame.epoch = UINT64_MAX; frame.drawCount = 0;
    if (sources.empty() || commands.empty()) { frame.epoch = epoch; return; }
    if (!CreateIndirectCullPipeline()) {
        static bool warned = false;
        if (!warned) { LOGSTREAM(Error) << "[Vox] Indexed GPU culling pipeline unavailable" << std::endl; warned = true; }
        return;
    }
    bool descriptorsDirty = false;
    if (frame.capacity < sources.size() || frame.commandCapacity < commands.size()) {
        vkDeviceWaitIdle(g_Device);
        if (frame.sourceView) vkDestroyBufferView(g_Device, frame.sourceView, g_Allocator);
        frame.sourceView = VK_NULL_HANDLE;
        frame.sources.Cleanup(); frame.candidates.Cleanup(); frame.commands.Cleanup(); frame.instances.Cleanup(); frame.params.Cleanup();frame.quadCommands.Cleanup();
        frame.residentSources.clear(); frame.residentCandidates.clear(); frame.residentCommands.clear();
        frame.capacity = 0; frame.commandCapacity = 0;
        const size_t capacity = std::max(sources.size() * 2, size_t(64));
        const size_t commandCapacity = std::max(commands.size() * 2, size_t(64));
        const auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!frame.sources.Create(capacity * sizeof(Source), storage | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, device) ||
            !frame.candidates.Create(capacity * 2 * sizeof(glm::uvec4), storage, device) ||
            !frame.commands.Create(commandCapacity * sizeof(FaceDrawCommand), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, device) ||
            !frame.quadCommands.Create(commandCapacity*sizeof(VkDrawIndexedIndirectCommand),storage|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,device) ||
            !frame.instances.Create(capacity * 6 * sizeof(uint32_t), storage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, device) ||
            !frame.params.Create(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return;
        VkBufferViewCreateInfo view{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        view.buffer = frame.sources.GetBuffer(); view.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        view.range = capacity * sizeof(Source);
        if (vkCreateBufferView(g_Device, &view, g_Allocator, &frame.sourceView) != VK_SUCCESS) return;
        frame.capacity = capacity; frame.commandCapacity = commandCapacity;
        descriptorsDirty = true;
    }
    if(quadGeometryChanged || m_QuadAtlas.empty()) {
        ++m_QuadAtlasVersion;m_QuadAtlasValid=true;m_QuadAtlas.assign(m_totalIndices/6,VoxQuad{});
        for(const auto& group:m_rendererGroups) {
            const auto cache=m_meshCache.find(group.renderer);
            if(cache==m_meshCache.end() || !group.renderer->HasValidQuads() || !group.renderer->GetQuadPipeline()){m_QuadAtlasValid=false;break;}
            const auto& quads=group.renderer->GetQuads();const size_t offset=cache->second.firstIndex/6;
            if(cache->second.indexCount/6!=quads.size() || offset+quads.size()>m_QuadAtlas.size()){m_QuadAtlasValid=false;break;}
            std::copy(quads.begin(),quads.end(),m_QuadAtlas.begin()+offset);
        }
    }
    if(quadGeometryChanged || m_QuadAtlasWords.empty()) {
        m_QuadAtlasWords.clear();
        for(const auto& q:m_QuadAtlas)m_QuadAtlasWords.push_back(q.geometry);
        const uint32_t lookup=uint32_t(m_QuadAtlasWords.size());m_QuadAtlasWords.resize(m_QuadAtlasWords.size()+m_QuadAtlas.size(),0u);
        if(m_QuadAtlasValid)for(const auto& group:m_rendererGroups){const auto& cache=m_meshCache.at(group.renderer);
            AppendVoxRasterAttributes(m_QuadAtlasWords,group.renderer->GetSurfaceAttributes(),uint32_t(cache.firstIndex/6),uint32_t(cache.indexCount/6),lookup);}
        std::vector<VoxPlaneRange> planes;
        if(m_QuadAtlasValid)for(const auto& group:m_rendererGroups){const auto first=uint32_t(m_meshCache.at(group.renderer).firstIndex/6);for(auto plane:group.renderer->GetPlaneRanges()){plane.firstQuad+=first;planes.push_back(plane);}}
        AppendVoxPlaneFooter(m_QuadAtlasWords,std::move(planes),lookup);
    }
    const auto& quadAtlas=m_QuadAtlasWords;frame.useQuads=m_QuadAtlasValid;
    VkPhysicalDeviceProperties quadLimits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&quadLimits);
    if(quadAtlas.size()*sizeof(uint32_t)>quadLimits.limits.maxStorageBufferRange || m_totalIndices>INT32_MAX)frame.useQuads=false;
    if(frame.useQuads && frame.quadCapacity<quadAtlas.size()) {
        vkDeviceWaitIdle(g_Device);frame.quads.Cleanup();frame.residentQuads.clear();frame.quadCapacity=0;frame.quadVersion=0;
        if(!frame.quads.Create(quadAtlas.size()*sizeof(uint32_t),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))frame.useQuads=false;
        else frame.quadCapacity=quadAtlas.size();
    }
    // Buffers remain in device-local memory. Camera-only frames upload no scene data.
    // vkCmdUpdateBuffer embeds dirty data into the command stream, preserving view/frame ordering.
    VkMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0, nullptr, 0, nullptr);
    auto updateResident = [&](VulkanBuffer& buffer, std::vector<uint8_t>& previous,
                              const void* data, size_t bytes, size_t stride) {
        const auto* input = static_cast<const uint8_t*>(data);
        const bool resized = previous.size() != bytes;
        for (size_t offset = 0; offset < bytes;) {
            if (!resized && std::memcmp(previous.data() + offset, input + offset, stride) == 0) {
                offset += stride; continue;
            }
            size_t end = offset + stride;
            while (end < bytes && end - offset + stride <= 65536 &&
                (resized || std::memcmp(previous.data() + end, input + end, stride) != 0)) end += stride;
            vkCmdUpdateBuffer(commandBuffer, buffer.GetBuffer(), offset, end - offset, input + offset);
            if (!resized) std::memcpy(previous.data() + offset, input + offset, end - offset);
            offset = end;
        }
        if (resized) previous.assign(input, input + bytes);
    };
    updateResident(frame.sources, frame.residentSources, sources.data(), sources.size() * sizeof(Source), sizeof(Source));
    updateResident(frame.candidates, frame.residentCandidates, candidates.data(), candidates.size() * sizeof(glm::uvec4), sizeof(glm::uvec4));
    updateResident(frame.commands, frame.residentCommands, commands.data(), commands.size() * sizeof(FaceDrawCommand), sizeof(FaceDrawCommand));
    auto& hiZ = g_SceneRenderer.GetHiZShader();
    const auto& history = hiZ.GetCullingHistory();
    const bool useHiZ = g_SceneRenderer.IsGameGrassHiZCullingEnabled() &&
        hiZ.IsInitialized() && hiZ.HasValidCullingData() && hiZ.GetCullingMipLevels() > 0 &&
        history.epoch != UINT64_MAX && history.epoch + 1 == epoch &&
        HiZHistory::CanReuse(mainProjView, history.viewProj,
            HiZHistory::OccluderRevision(g_SceneRenderer.GetRenderWorld()), history.revision, history.valid);
    Params params{};
    params.counts = glm::uvec4(static_cast<uint32_t>(sources.size()), mainFrustum, sceneFrustum, static_cast<uint32_t>(commands.size()));
    params.camera = glm::vec4(directionCamera, 1);
    const auto mainPlanes = AABBUtils::ExtractFrustumPlanes(mainProjView);
    const auto scenePlanes = AABBUtils::ExtractFrustumPlanes(rasterProjView);
    for (int i = 0; i < 6; ++i) {
        params.planes[i] = glm::vec4(mainPlanes[i].normal, mainPlanes[i].distance);
        params.planes[i + 6] = glm::vec4(scenePlanes[i].normal, scenePlanes[i].distance);
    }
    params.hizViewProj = HiZHistory::WithJitter(history.viewProj, history.jitter);
    params.hizParams = glm::uvec4(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
                                useHiZ ? hiZ.GetCullingMipLevels() : 0u, useHiZ ? 1u : 0u);
    frame.params.Write(&params, sizeof(params));
    VkMemoryBarrier uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);
    if (!frame.descriptor) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = m_IndirectCullPool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &m_IndirectCullSetLayout;
        if (vkAllocateDescriptorSets(g_Device, &allocate, &frame.descriptor) != VK_SUCCESS) return;
        descriptorsDirty = true;
    }

    if (descriptorsDirty) {
        VkDescriptorBufferInfo infos[]{
            {frame.params.GetBuffer(), 0, sizeof(Params)}, {frame.sources.GetBuffer(), 0, VK_WHOLE_SIZE},
            {frame.candidates.GetBuffer(), 0, VK_WHOLE_SIZE}, {frame.commands.GetBuffer(), 0, VK_WHOLE_SIZE},
            {frame.instances.GetBuffer(), 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = frame.descriptor; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(g_Device,5,writes,0,nullptr);
    }
    VkDescriptorBufferInfo quadCommandInfo{frame.quadCommands.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet quadCommandWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};quadCommandWrite.dstSet=frame.descriptor;quadCommandWrite.dstBinding=6;
    quadCommandWrite.descriptorCount=1;quadCommandWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;quadCommandWrite.pBufferInfo=&quadCommandInfo;
    vkUpdateDescriptorSets(g_Device,1,&quadCommandWrite,0,nullptr);
    if(frame.useQuads) {
        if(frame.quadVersion!=m_QuadAtlasVersion){updateResident(frame.quads,frame.residentQuads,quadAtlas.data(),quadAtlas.size()*sizeof(uint32_t),sizeof(uint32_t));frame.quadVersion=m_QuadAtlasVersion;}
        // This upload was recorded after the earlier transfer barrier.
        VkMemoryBarrier quadReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};quadReady.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;quadReady.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&quadReady,0,nullptr,0,nullptr);
        if(!frame.quadDescriptor) {
            auto layout=m_rendererGroups.front().renderer->GetQuadSetLayout();VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=m_IndirectCullPool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
            if(vkAllocateDescriptorSets(g_Device,&ai,&frame.quadDescriptor)!=VK_SUCCESS)frame.useQuads=false;
        }
        if(frame.useQuads) {
            VkDescriptorBufferInfo info{frame.quads.GetBuffer(),0,quadAtlas.size()*sizeof(uint32_t)};VkWriteDescriptorSet writes[3]{};
            writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[0].dstSet=frame.quadDescriptor;writes[0].descriptorCount=1;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;writes[0].pTexelBufferView=&frame.sourceView;
            writes[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[1].dstSet=frame.quadDescriptor;writes[1].dstBinding=1;writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&info;
            writes[2]=writes[1];writes[2].dstBinding=2;
            vkUpdateDescriptorSets(g_Device,3,writes,0,nullptr);
        }
    }
    // The image rotates each logical frame, so update this binding even for resident geometry.
    VkDescriptorImageInfo hizImage{};
    hizImage.sampler = g_GameRenderTarget.GetHiZSampler();
    hizImage.imageView = useHiZ ? hiZ.GetHiZTextureViewForCulling() : g_GameRenderTarget.GetDepthImageView();
    hizImage.imageLayout = useHiZ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (hizImage.imageView == VK_NULL_HANDLE || hizImage.sampler == VK_NULL_HANDLE) return;
    VkWriteDescriptorSet hizWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    hizWrite.dstSet = frame.descriptor; hizWrite.dstBinding = 5; hizWrite.descriptorCount = 1;
    hizWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; hizWrite.pImageInfo = &hizImage;
    vkUpdateDescriptorSets(g_Device, 1, &hizWrite, 0, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_IndirectCullPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_IndirectCullLayout, 0, 1, &frame.descriptor, 0, nullptr);
    uint32_t phase = 1;
    vkCmdPushConstants(commandBuffer, m_IndirectCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(phase), &phase);
    vkCmdDispatch(commandBuffer, (params.counts.w + 63) / 64, 1, 1);
    VkMemoryBarrier reset{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reset.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; reset.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &reset, 0, nullptr, 0, nullptr);
    phase = 0;
    vkCmdPushConstants(commandBuffer, m_IndirectCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(phase), &phase);
    vkCmdDispatch(commandBuffer, (params.counts.x + 63) / 64, 1, 1);
    // Small models need no propagation pass. Segments share their direction's list.
    if (hasSegmentedDraws) {
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &reset, 0, nullptr, 0, nullptr);
        phase = 2;
        vkCmdPushConstants(commandBuffer, m_IndirectCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(phase), &phase);
        vkCmdDispatch(commandBuffer, (params.counts.w + 63) / 64, 1, 1);
    }
    if(frame.useQuads) {
        vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&reset,0,nullptr,0,nullptr);
        phase=3;vkCmdPushConstants(commandBuffer,m_IndirectCullLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(phase),&phase);
        vkCmdDispatch(commandBuffer,(params.counts.w+63)/64,1,1);
    }
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 1, &ready, 0, nullptr, 0, nullptr);
    frame.epoch = epoch; frame.drawCount = static_cast<uint32_t>(commands.size());
}
void VoxelMeshMultiDrawIndirect::Clear()
{
    m_rendererGroups.clear();m_ModelSlots.clear();m_GroupSlots.clear();m_meshCache.clear();
    m_totalVertices=0;m_totalIndices=0;m_totalInstances=0;m_geometryDataDirty=true;
    m_QuadAtlasWords.clear();m_QuadAtlas.clear();m_QuadAtlasValid=false;++m_QuadAtlasVersion;
    m_GpuCollectionEpoch=UINT64_MAX;
    for(auto& entry:m_GpuFrames){entry.second->epoch=UINT64_MAX;entry.second->drawCount=0;}
}

// ================================================================================
// GPU Culling Implementation
// ================================================================================

// 创建 GPU 剔除资源


// 创建计算着色器管线


// 创建描述符集
