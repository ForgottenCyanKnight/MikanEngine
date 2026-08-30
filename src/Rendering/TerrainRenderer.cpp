#include "TerrainRenderer.h"

#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "ECS/SceneECS.h"
#include "TexturePool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace {

constexpr size_t kInitialDescriptorSets = 512;

VkVertexInputBindingDescription MakeVertexBinding(uint32_t binding, uint32_t stride, VkVertexInputRate inputRate) {
    VkVertexInputBindingDescription desc{};
    desc.binding = binding;
    desc.stride = stride;
    desc.inputRate = inputRate;
    return desc;
}

VkVertexInputAttributeDescription MakeVertexAttribute(uint32_t location, uint32_t binding,
                                                       VkFormat format, uint32_t offset) {
    VkVertexInputAttributeDescription desc{};
    desc.location = location;
    desc.binding = binding;
    desc.format = format;
    desc.offset = offset;
    return desc;
}

uint32_t MortonPart1By1(uint32_t value) {
    value &= 0x0000ffffu;
    value = (value | (value << 8u)) & 0x00ff00ffu;
    value = (value | (value << 4u)) & 0x0f0f0f0fu;
    value = (value | (value << 2u)) & 0x33333333u;
    value = (value | (value << 1u)) & 0x55555555u;
    return value;
}

uint32_t MortonCode2D(uint32_t x, uint32_t z) {
    return MortonPart1By1(x) | (MortonPart1By1(z) << 1u);
}

} // namespace

void TerrainChunkManager::Configure(const glm::vec2& worldSize, int chunkCount,
                                    float minHeight, float maxHeight,
                                    float viewDistance, float lod0Distance, float lod1Distance,
                                    int maxLod) {
    m_WorldSize = glm::max(worldSize, glm::vec2(1.0f));
    m_ChunkCount = std::clamp(chunkCount, 1, 256);
    m_ViewDistance = std::max(0.0f, viewDistance);
    m_Lod0Distance = std::max(0.0f, lod0Distance);
    m_Lod1Distance = std::max(m_Lod0Distance, lod1Distance);
    m_MaxLod = std::clamp(maxLod, 0, 2);

    m_Chunks.clear();
    m_Chunks.reserve(static_cast<size_t>(m_ChunkCount) * static_cast<size_t>(m_ChunkCount));
    for (auto& visible : m_Visible) {
        visible.clear();
    }

    const glm::vec2 terrainOrigin = -m_WorldSize * 0.5f;
    const float chunkCountFloat = static_cast<float>(m_ChunkCount);
    const float lowHeight = std::min(minHeight, maxHeight) - 1.0f;
    const float highHeight = std::max(minHeight, maxHeight) + 1.0f;

    for (int z = 0; z < m_ChunkCount; ++z) {
        for (int x = 0; x < m_ChunkCount; ++x) {
            TerrainChunk chunk;
            chunk.x = x;
            chunk.z = z;
            const glm::vec2 normalizedMin(static_cast<float>(x) / chunkCountFloat,
                                          static_cast<float>(z) / chunkCountFloat);
            const glm::vec2 normalizedMax(static_cast<float>(x + 1) / chunkCountFloat,
                                          static_cast<float>(z + 1) / chunkCountFloat);
            // 用和 shader 相同的全局分区公式计算 CPU 包围盒，避免边界
            // 因为先除后乘/先乘后除的舍入差异而出现错误剔除。
            chunk.origin = terrainOrigin + normalizedMin * m_WorldSize;
            chunk.size = (normalizedMax - normalizedMin) * m_WorldSize;
            chunk.uvOrigin = glm::vec2(static_cast<float>(x) / static_cast<float>(m_ChunkCount),
                                       static_cast<float>(z) / static_cast<float>(m_ChunkCount));
            chunk.uvSize = glm::vec2(1.0f / static_cast<float>(m_ChunkCount));
            chunk.localBounds = AABB(
                glm::vec3(chunk.origin.x, lowHeight, chunk.origin.y),
                glm::vec3(chunk.origin.x + chunk.size.x, highHeight, chunk.origin.y + chunk.size.y));
            m_Chunks.push_back(chunk);
        }
    }

    // 以 Morton/Z-order 保存 Chunk，令相邻空间块在 CPU 遍历、可见列表
    // 构建和后续流式加载中保持更好的局部性。它不改变 LOD 判定规则。
    std::sort(m_Chunks.begin(), m_Chunks.end(), [](const TerrainChunk& lhs,
                                                   const TerrainChunk& rhs) {
        const uint32_t lhsCode = MortonCode2D(static_cast<uint32_t>(lhs.x),
                                              static_cast<uint32_t>(lhs.z));
        const uint32_t rhsCode = MortonCode2D(static_cast<uint32_t>(rhs.x),
                                              static_cast<uint32_t>(rhs.z));
        return lhsCode < rhsCode;
    });
}

void TerrainChunkManager::UpdateVisibility(const glm::vec3& cameraPosition,
                                           const glm::mat4& modelMatrix,
                                           const std::array<Plane, 6>& frustumPlanes,
                                           bool useFrustumCulling) {
    for (auto& visible : m_Visible) {
        visible.clear();
        visible.reserve(m_Chunks.size() / 2 + 1);
    }

    struct SelectedChunk {
        const TerrainChunk* chunk = nullptr;
        int lod = 0;
    };
    std::vector<SelectedChunk> selected;
    selected.reserve(m_Chunks.size());
    std::vector<int8_t> lodGrid(static_cast<size_t>(m_ChunkCount) * m_ChunkCount, -1);

    // 第一遍只做可见性和 LOD 判定，保留规则网格坐标供第二遍查询邻居。
    for (const TerrainChunk& chunk : m_Chunks) {
        const AABB worldBounds = chunk.localBounds.Transform(modelMatrix);
        if (useFrustumCulling && !worldBounds.IsInsideFrustum(frustumPlanes)) {
            continue;
        }

        const float distance = glm::distance(cameraPosition, worldBounds.GetCenter());
        if (m_ViewDistance > 0.0f && distance > m_ViewDistance + worldBounds.GetMaxDimension() * 0.5f) {
            continue;
        }

        int lod = 0;
        if (m_MaxLod >= 1 && distance >= m_Lod0Distance) {
            lod = 1;
        }
        if (m_MaxLod >= 2 && distance >= m_Lod1Distance) {
            lod = 2;
        }
        lod = std::clamp(lod, 0, m_MaxLod);

        lodGrid[static_cast<size_t>(chunk.z) * m_ChunkCount + chunk.x] =
            static_cast<int8_t>(lod);
        selected.push_back({&chunk, lod});
    }

    auto coarserDelta = [&](int x, int z, int lod) -> uint32_t {
        if (x < 0 || z < 0 || x >= m_ChunkCount || z >= m_ChunkCount) {
            return 0;
        }
        const int neighborLod = lodGrid[static_cast<size_t>(z) * m_ChunkCount + x];
        if (neighborLod < 0) {
            return 0;
        }
        return static_cast<uint32_t>(std::clamp(neighborLod - lod, 0, 3));
    };

    // 每条边用 2 bit 保存“邻居比本块粗多少级”：-Z、+X、+Z、-X。
    // 顶点着色器据此折叠细网格边缘顶点，替代会产生地下黑墙的 skirt。
    for (const SelectedChunk& selection : selected) {
        const TerrainChunk& chunk = *selection.chunk;
        const int lod = selection.lod;
        const uint32_t edgeLodDeltas =
            coarserDelta(chunk.x, chunk.z - 1, lod) |
            (coarserDelta(chunk.x + 1, chunk.z, lod) << 2u) |
            (coarserDelta(chunk.x, chunk.z + 1, lod) << 4u) |
            (coarserDelta(chunk.x - 1, chunk.z, lod) << 6u);
        TerrainChunkInstance instance;
        instance.originSize = glm::vec4(chunk.origin.x, chunk.origin.y, chunk.size.x, chunk.size.y);
        // 将整数网格坐标传给 GPU，由 shader 统一计算全局坐标。
        // 相邻块的边界分别变成 (x + 1) / count 与 (x + 1) / count，
        // 不再依赖两条不同的浮点加法路径。
        instance.uvRect = glm::vec4(static_cast<float>(chunk.x),
                                    static_cast<float>(chunk.z),
                                    static_cast<float>(m_ChunkCount), 0.0f);
        instance.params.x = static_cast<float>(lod);
        instance.params.y = static_cast<float>(edgeLodDeltas);
        m_Visible[static_cast<size_t>(lod)].push_back(instance);
    }
}

const std::vector<TerrainChunkInstance>& TerrainChunkManager::GetVisible(int lod) const {
    static const std::vector<TerrainChunkInstance> empty;
    if (lod < 0 || lod >= static_cast<int>(m_Visible.size())) {
        return empty;
    }
    return m_Visible[static_cast<size_t>(lod)];
}

size_t TerrainChunkManager::GetVisibleCount() const {
    size_t count = 0;
    for (const auto& visible : m_Visible) {
        count += visible.size();
    }
    return count;
}

TerrainRenderer::~TerrainRenderer() {
    Cleanup();
}

void TerrainRenderer::Init(VkRenderPass renderPass) {
    m_RenderPass = renderPass;

    // render pass 在交换链重建后会变化，管线需要跟随重建；地形资源和 descriptor set 可以复用。
    m_Pipeline.Cleanup();
    m_WireframePipeline.Cleanup();
    m_DepthPipeline.Cleanup();
    m_CsmDepthPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;

    if (!CreateDescriptorResources() || !CreatePipelines()) {
        std::printf("[TerrainRenderer] initialization failed\n");
        return;
    }
    std::printf("[TerrainRenderer] initialized\n");
}

void TerrainRenderer::Cleanup() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    for (auto& [entity, resource] : m_Resources) {
        if (resource) {
            DestroyResource(*resource);
        }
    }
    m_Resources.clear();
    m_PreparedResources.clear();
    m_VisibleChunkCount = 0;

    m_Pipeline.Cleanup();
    m_WireframePipeline.Cleanup();
    m_DepthPipeline.Cleanup();
    m_CsmDepthPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;
    DestroyDescriptorResources();

    if (m_OwnedWhiteFallback && g_TexturePool) {
        g_TexturePool->Release("white");
    }
    m_OwnedWhiteFallback = false;
    m_RenderPass = VK_NULL_HANDLE;
}

void TerrainRenderer::Prepare(const std::vector<ECS::Entity>& rootEntities,
                              const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes,
                              bool useFrustumCulling) {
    m_PreparedResources.clear();
    m_VisibleChunkCount = 0;

    std::vector<ECS::Entity> terrainEntities;
    for (ECS::Entity root : rootEntities) {
        CollectTerrainEntities(root, terrainEntities);
    }

    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::unordered_set<ECS::Entity> activeEntities;
    activeEntities.reserve(terrainEntities.size());

    for (ECS::Entity entity : terrainEntities) {
        if (!coordinator.HasComponent<ECS::TerrainComponent>(entity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            continue;
        }

        const ECS::TerrainComponent& settings = coordinator.GetComponent<ECS::TerrainComponent>(entity);
        if (!settings.enabled || settings.heightmapPath.empty()) {
            continue;
        }

        Resource* resource = EnsureResource(entity, settings);
        if (!resource) {
            continue;
        }

        activeEntities.insert(entity);
        resource->model = sceneECS.GetWorldMatrix(entity);
        resource->chunks.UpdateVisibility(cameraPosition, resource->model,
                                          frustumPlanes, useFrustumCulling);
        m_PreparedResources.push_back(resource);
        m_VisibleChunkCount += resource->chunks.GetVisibleCount();
    }

    bool hasStaleResources = false;
    for (const auto& [entity, resource] : m_Resources) {
        if (activeEntities.find(entity) == activeEntities.end()) {
            hasStaleResources = true;
            break;
        }
    }
    if (hasStaleResources && g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    for (auto it = m_Resources.begin(); it != m_Resources.end();) {
        if (activeEntities.find(it->first) == activeEntities.end()) {
            if (it->second) {
                DestroyResource(*it->second);
            }
            it = m_Resources.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainRenderer::PrepareFromScene(const glm::vec3& cameraPosition,
                                       const std::array<Plane, 6>& frustumPlanes,
                                       bool useFrustumCulling) {
    Prepare(ECS::SceneECS::GetInstance().GetRootEntities(), cameraPosition,
            frustumPlanes, useFrustumCulling);
}

void TerrainRenderer::CollectTerrainEntities(ECS::Entity entity,
                                              std::vector<ECS::Entity>& entities) const {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::TerrainComponent>(entity)) {
        const auto& terrain = coordinator.GetComponent<ECS::TerrainComponent>(entity);
        const bool visible = !coordinator.HasComponent<ECS::RenderComponent>(entity) ||
                             coordinator.GetComponent<ECS::RenderComponent>(entity).visible;
        if (terrain.enabled && visible) {
            entities.push_back(entity);
        }
    }

    for (ECS::Entity child : ECS::SceneECS::GetInstance().GetChildren(entity)) {
        CollectTerrainEntities(child, entities);
    }
}

TerrainRenderer::Resource* TerrainRenderer::EnsureResource(
    ECS::Entity entity, const ECS::TerrainComponent& settings) {
    auto it = m_Resources.find(entity);
    if (it != m_Resources.end() && it->second && SettingsEqual(it->second->settings, settings)) {
        // wireframe 只影响管线选择、不参与 SettingsEqual，避免切换开关触发整资源重建；
        // 这里直接同步最新值，渲染循环按它选择线框/实体管线。
        it->second->settings.wireframe = settings.wireframe;
        return it->second.get();
    }

    if (it != m_Resources.end()) {
        // 编辑器修改路径或布局时旧 descriptor/纹理可能仍被本帧 GPU 使用。
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }
        if (it->second) {
            DestroyResource(*it->second);
        }
        m_Resources.erase(it);
    }

    std::unique_ptr<Resource> resource = CreateResource(entity, settings);
    if (!resource) {
        return nullptr;
    }
    Resource* result = resource.get();
    m_Resources.emplace(entity, std::move(resource));
    return result;
}

std::unique_ptr<TerrainRenderer::Resource> TerrainRenderer::CreateResource(
    ECS::Entity entity, const ECS::TerrainComponent& settings) {
    if (!g_TexturePool || !EnsureWhiteFallback() || settings.heightmapPath.empty()) {
        return nullptr;
    }

    auto resource = std::make_unique<Resource>();
    resource->entity = entity;
    resource->settings = settings;
    resource->heightmapKey = MakeTextureKey(entity, "height");
    const std::string heightmapPath = EngineConfig::GetFullPath(settings.heightmapPath.c_str());

    if (!g_TexturePool->LoadHeightmap16(resource->heightmapKey, heightmapPath,
                                        SamplerType::LinearClamp) ||
        !g_TexturePool->GetTexture(resource->heightmapKey)) {
        std::printf("[TerrainRenderer] failed to load heightmap for entity %u: %s\n",
                    static_cast<unsigned>(entity), settings.heightmapPath.c_str());
        return nullptr;
    }
    resource->ownedTextureKeys.push_back(resource->heightmapKey);

    for (int layer = 0; layer < 4; ++layer) {
        const std::string& path = GetLayerPath(settings, layer);
        resource->layerKeys[static_cast<size_t>(layer)] = "white";
        if (path.empty()) {
            continue;
        }

        const std::string slot = "layer" + std::to_string(layer);
        const std::string key = MakeTextureKey(entity, slot.c_str());
        const std::string layerPath = EngineConfig::GetFullPath(path.c_str());
        if (g_TexturePool->LoadTexture2D(key, layerPath, SamplerType::LinearRepeat) &&
            g_TexturePool->GetTexture(key)) {
            resource->layerKeys[static_cast<size_t>(layer)] = key;
            resource->ownedTextureKeys.push_back(key);
        } else {
            std::printf("[TerrainRenderer] layer %d failed for entity %u, using white fallback: %s\n",
                        layer, static_cast<unsigned>(entity), path.c_str());
        }
    }

    resource->controlKey = "white";
    if (!settings.controlMapPath.empty()) {
        const std::string controlKey = MakeTextureKey(entity, "control");
        const std::string controlPath = EngineConfig::GetFullPath(settings.controlMapPath.c_str());
        if (g_TexturePool->LoadTexture2D(controlKey, controlPath, SamplerType::LinearClamp) &&
            g_TexturePool->GetTexture(controlKey)) {
            resource->controlKey = controlKey;
            resource->ownedTextureKeys.push_back(controlKey);
            resource->useControlMap = true;
        } else {
            std::printf("[TerrainRenderer] control map failed for entity %u, using procedural blend: %s\n",
                        static_cast<unsigned>(entity), settings.controlMapPath.c_str());
        }
    }

    const float height0 = settings.heightOffset;
    const float height1 = settings.heightOffset + settings.heightScale;
    resource->chunks.Configure(settings.worldSize, settings.chunkCount,
                               std::min(height0, height1), std::max(height0, height1),
                               settings.viewDistance, settings.lod0Distance,
                               settings.lod1Distance, settings.maxLod);

    int baseResolution = std::clamp(settings.patchResolution, 3, 129);
    if ((baseResolution & 1) == 0) {
        --baseResolution;
    }
    for (int lod = 0; lod < 3; ++lod) {
        const uint32_t resolution = static_cast<uint32_t>(((baseResolution - 1) >> lod) + 1);
        if (!BuildPatch(resource->patches[static_cast<size_t>(lod)], resolution)) {
            DestroyResource(*resource);
            return nullptr;
        }
    }

    // 先用小容量启动，按本帧实际可见 Chunk 增长，避免 chunkCount=256
    // 时为每个地形预先分配 65536 个实例槽位。
    constexpr size_t kInitialInstanceCapacity = 64;
    const size_t chunkCapacity = resource->chunks.GetChunks().size();
    const size_t initialInstanceCapacity = std::max<size_t>(
        1, std::min(chunkCapacity, kInitialInstanceCapacity));
    if (!CreateInstanceBuffers(*resource, initialInstanceCapacity) ||
        !CreateUniformBuffers(*resource) ||
        !CreateDescriptorSets(*resource)) {
        DestroyResource(*resource);
        return nullptr;
    }

    return resource;
}

void TerrainRenderer::DestroyResource(Resource& resource) {
    if (m_DescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> sets;
        sets.reserve(resource.descriptorSets.size());
        for (VkDescriptorSet& set : resource.descriptorSets) {
            if (set != VK_NULL_HANDLE) {
                sets.push_back(set);
                set = VK_NULL_HANDLE;
            }
        }
        if (!sets.empty()) {
            vkFreeDescriptorSets(g_Device, m_DescriptorPool,
                                 static_cast<uint32_t>(sets.size()), sets.data());
        }
    }

    for (auto& uniform : resource.uniformBuffers) {
        uniform.reset();
    }
    for (auto& instanceBuffer : resource.instanceBuffers) {
        instanceBuffer.Cleanup();
    }
    for (auto& instanceBuffer : resource.csmInstanceBuffers) {
        instanceBuffer.Cleanup();
    }
    resource.csmInstanceCapacity = 0;
    for (auto& patch : resource.patches) {
        patch.Cleanup();
    }

    if (g_TexturePool) {
        for (const std::string& key : resource.ownedTextureKeys) {
            g_TexturePool->Release(key);
        }
    }
    resource.ownedTextureKeys.clear();
}

bool TerrainRenderer::SettingsEqual(const ECS::TerrainComponent& lhs,
                                    const ECS::TerrainComponent& rhs) const {
    return lhs.enabled == rhs.enabled &&
           lhs.heightmapPath == rhs.heightmapPath &&
           lhs.worldSize.x == rhs.worldSize.x && lhs.worldSize.y == rhs.worldSize.y &&
           lhs.heightScale == rhs.heightScale && lhs.heightOffset == rhs.heightOffset &&
           lhs.chunkCount == rhs.chunkCount && lhs.patchResolution == rhs.patchResolution &&
           lhs.viewDistance == rhs.viewDistance &&
           lhs.lod0Distance == rhs.lod0Distance && lhs.lod1Distance == rhs.lod1Distance &&
           lhs.maxLod == rhs.maxLod && lhs.materialTiling == rhs.materialTiling &&
           lhs.blendSharpness == rhs.blendSharpness &&
           lhs.layer0Path == rhs.layer0Path && lhs.layer1Path == rhs.layer1Path &&
           lhs.layer2Path == rhs.layer2Path && lhs.layer3Path == rhs.layer3Path &&
           lhs.controlMapPath == rhs.controlMapPath;
}

bool TerrainRenderer::EnsureWhiteFallback() {
    if (!g_TexturePool) {
        return false;
    }
    if (g_TexturePool->GetTexture("white")) {
        return true;
    }

    const std::string materialPath = EngineConfig::GetEngineTexturePath("material.png");
    if (!g_TexturePool->LoadTexture2D("white", materialPath, SamplerType::LinearRepeat)) {
        std::printf("[TerrainRenderer] unable to create white texture fallback: %s\n", materialPath.c_str());
        return false;
    }
    m_OwnedWhiteFallback = true;
    return g_TexturePool->GetTexture("white") != nullptr;
}

bool TerrainRenderer::CreateDescriptorResources() {
    if (m_DescriptorLayout != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (i == 1) {
            // 高度图同时供顶点位移和片元级连续法线/坡度计算使用。
            bindings[i].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        }
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_DescriptorLayout) != VK_SUCCESS) {
        m_DescriptorLayout = VK_NULL_HANDLE;
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets * 6);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = static_cast<uint32_t>(kInitialDescriptorSets);
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
        m_DescriptorPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void TerrainRenderer::DestroyDescriptorResources() {
    if (g_Device == VK_NULL_HANDLE) {
        m_DescriptorPool = VK_NULL_HANDLE;
        m_DescriptorLayout = VK_NULL_HANDLE;
        return;
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
    }
}

bool TerrainRenderer::CreatePipelines() {
    if (m_RenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }

    const std::array<VkVertexInputBindingDescription, 2> vertexBindings = {
        MakeVertexBinding(0, sizeof(TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX),
        MakeVertexBinding(1, sizeof(TerrainChunkInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    const std::array<VkVertexInputAttributeDescription, 4> vertexAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, position)),
        MakeVertexAttribute(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, originSize)),
        MakeVertexAttribute(2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, uvRect)),
        MakeVertexAttribute(3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, params))
    };

    // Vulkan 的 primitiveRestartEnable 不通过 VkPhysicalDeviceFeatures 暴露；
    // 对 triangle strip 是核心输入装配能力。
    m_PrimitiveRestartSupported = true;

    PipelineConfig geometryConfig;
    geometryConfig.vertShader = "terrain.vert.spv";
    geometryConfig.fragShader = "terrain.frag.spv";
    geometryConfig.topology = m_PrimitiveRestartSupported
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    geometryConfig.primitiveRestartEnable = m_PrimitiveRestartSupported;
    geometryConfig.cullMode = VK_CULL_MODE_BACK_BIT;
    geometryConfig.depthTest = true;
    geometryConfig.depthWrite = !g_EnableZPrepass;
    geometryConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    geometryConfig.colorAttachmentCount = 4;
    geometryConfig.subpass = 1;
    geometryConfig.vertexBindings.assign(vertexBindings.begin(), vertexBindings.end());
    geometryConfig.vertexAttributes.assign(vertexAttributes.begin(), vertexAttributes.end());
    if (!m_Pipeline.Create(m_RenderPass, m_DescriptorLayout, geometryConfig)) {
        return false;
    }

    PipelineConfig depthConfig = geometryConfig;
    depthConfig.vertShader = "terrain_depth.vert.spv";
    depthConfig.fragShader = "terrain_depth.frag.spv";
    depthConfig.depthWrite = true;
    depthConfig.depthCompareOp = VK_COMPARE_OP_LESS;
    depthConfig.colorAttachmentCount = 0;
    depthConfig.subpass = 0;
    if (!m_DepthPipeline.Create(m_RenderPass, m_DescriptorLayout, depthConfig)) {
        m_Pipeline.Cleanup();
        return false;
    }

    // 线框模式管线（VK_POLYGON_MODE_LINE，需要设备特性 fillModeNonSolid）。
    // 关闭背面剔除，避免反面 patch 的线框缺失；创建失败仅降级为实体渲染，
    // 不影响常规管线（例如不支持 fillModeNonSolid 的移动端设备）。
    PipelineConfig wireframeConfig = geometryConfig;
    wireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;
    wireframeConfig.cullMode = VK_CULL_MODE_NONE;
    if (!m_WireframePipeline.Create(m_RenderPass, m_DescriptorLayout, wireframeConfig)) {
        std::printf("[TerrainRenderer] wireframe pipeline creation failed - wireframe mode disabled\n");
    }
    return true;
}

bool TerrainRenderer::EnsureCsmDepthPipeline(VkRenderPass shadowRenderPass) {
    if (shadowRenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }
    if (m_CsmDepthPipeline.GetPipeline() != VK_NULL_HANDLE &&
        m_CsmRenderPass == shadowRenderPass) {
        return true;
    }

    m_CsmDepthPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;

    const std::array<VkVertexInputBindingDescription, 2> vertexBindings = {
        MakeVertexBinding(0, sizeof(TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX),
        MakeVertexBinding(1, sizeof(TerrainChunkInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    const std::array<VkVertexInputAttributeDescription, 4> vertexAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, position)),
        MakeVertexAttribute(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, originSize)),
        MakeVertexAttribute(2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, uvRect)),
        MakeVertexAttribute(3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, params))
    };

    PipelineConfig config;
    config.vertShader = "terrain_csm_depth.vert.spv";
    config.fragShader = "terrain_depth.frag.spv";
    config.topology = m_PrimitiveRestartSupported
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.primitiveRestartEnable = m_PrimitiveRestartSupported;
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = true;
    config.depthWrite = true;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.colorAttachmentCount = 0;
    config.subpass = 0;
    config.vertexBindings.assign(vertexBindings.begin(), vertexBindings.end());
    config.vertexAttributes.assign(vertexAttributes.begin(), vertexAttributes.end());
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4);
    config.depthBiasEnable = true;
    config.depthBiasConstantFactor = 2.0f;
    config.depthBiasClamp = 0.0f;
    config.depthBiasSlopeFactor = 2.0f;

    if (!m_CsmDepthPipeline.Create(shadowRenderPass, m_DescriptorLayout, config)) {
        return false;
    }
    m_CsmRenderPass = shadowRenderPass;
    return true;
}

bool TerrainRenderer::BuildPatch(TerrainPatch& patch, uint32_t resolution) {
    patch.Cleanup();
    if (resolution < 2) {
        return false;
    }

    const size_t vertexCount = static_cast<size_t>(resolution) * resolution;
    std::vector<TerrainVertex> vertices(vertexCount);
    std::vector<uint32_t> indices;
    constexpr uint32_t kPrimitiveRestartIndex = std::numeric_limits<uint32_t>::max();
    if (m_PrimitiveRestartSupported) {
        const size_t interiorStripIndices = static_cast<size_t>(resolution - 1) *
                                            static_cast<size_t>(resolution) * 2u +
                                            static_cast<size_t>(resolution - 2);
        indices.reserve(interiorStripIndices);
    } else {
        const size_t quadCount = static_cast<size_t>(resolution - 1) * (resolution - 1);
        indices.reserve(quadCount * 6u);
    }

    for (uint32_t z = 0; z < resolution; ++z) {
        for (uint32_t x = 0; x < resolution; ++x) {
            const size_t index = static_cast<size_t>(z) * resolution + x;
            const glm::vec2 uv(static_cast<float>(x) / static_cast<float>(resolution - 1),
                               static_cast<float>(z) / static_cast<float>(resolution - 1));
            vertices[index].position = uv;
        }
    }

    if (m_PrimitiveRestartSupported) {
        // 每一行是一个独立 triangle strip，行间用 primitive restart 分隔。
        // 顺序 [top-left, bottom-left, top-right, bottom-right] 与原三角形
        // 列表保持相同的朝向，且避免为换行添加退化三角形。
        for (uint32_t z = 0; z + 1 < resolution; ++z) {
            if (z > 0) {
                indices.push_back(kPrimitiveRestartIndex);
            }
            for (uint32_t x = 0; x < resolution; ++x) {
                indices.push_back(z * resolution + x);
                indices.push_back((z + 1) * resolution + x);
            }
        }
    } else {
        // 极少数不支持 primitiveRestart 的设备回退到 triangle list，
        // 避免 UINT32_MAX 被当成普通顶点索引。
        for (uint32_t z = 0; z + 1 < resolution; ++z) {
            for (uint32_t x = 0; x + 1 < resolution; ++x) {
                const uint32_t a = z * resolution + x;
                const uint32_t b = a + 1;
                const uint32_t d = a + resolution;
                const uint32_t c = d + 1;
                indices.push_back(a);
                indices.push_back(c);
                indices.push_back(b);
                indices.push_back(a);
                indices.push_back(d);
                indices.push_back(c);
            }
        }
    }

    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!patch.vertexBuffer.Create(static_cast<VkDeviceSize>(vertices.size() * sizeof(TerrainVertex)),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
        return false;
    }
    patch.vertexBuffer.Write(vertices.data(), static_cast<VkDeviceSize>(vertices.size() * sizeof(TerrainVertex)));

    if (!patch.indexBuffer.Create(static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, memory)) {
        patch.Cleanup();
        return false;
    }
    patch.indexBuffer.Write(indices.data(), static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)));
    patch.indexCount = static_cast<uint32_t>(indices.size());
    patch.resolution = resolution;
    return true;
}

bool TerrainRenderer::CreateInstanceBuffers(Resource& resource, size_t capacity) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    resource.instanceCapacity = std::max<size_t>(1, capacity);
    for (auto& buffer : resource.instanceBuffers) {
        buffer.Cleanup();
        if (!buffer.Create(static_cast<VkDeviceSize>(resource.instanceCapacity * sizeof(TerrainChunkInstance)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.instanceBuffers) {
                cleanup.Cleanup();
            }
            resource.instanceCapacity = 0;
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::CreateUniformBuffers(Resource& resource) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& uniform : resource.uniformBuffers) {
        uniform = std::make_unique<VulkanBuffer>();
        if (!uniform->Create(sizeof(TerrainUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.uniformBuffers) {
                cleanup.reset();
            }
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::CreateDescriptorSets(Resource& resource) {
    if (g_Device == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE ||
        m_DescriptorPool == VK_NULL_HANDLE || !g_TexturePool) {
        return false;
    }

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(m_DescriptorLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = kFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, resource.descriptorSets.data()) != VK_SUCCESS) {
        resource.descriptorSets.fill(VK_NULL_HANDLE);
        return false;
    }

    auto getImage = [](const std::string& name, VkDescriptorImageInfo& image) -> bool {
        const TextureInfo* texture = g_TexturePool ? g_TexturePool->GetTexture(name) : nullptr;
        if (!texture || texture->imageView == VK_NULL_HANDLE) {
            return false;
        }
        image.imageView = texture->imageView;
        image.sampler = g_TexturePool->GetSampler(name);
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return image.sampler != VK_NULL_HANDLE;
    };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = resource.uniformBuffers[frame]->GetBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(TerrainUniformData);

        std::array<VkDescriptorImageInfo, 6> images{};
        if (!getImage(resource.heightmapKey, images[0])) {
            return false;
        }
        for (int layer = 0; layer < 4; ++layer) {
            if (!getImage(resource.layerKeys[static_cast<size_t>(layer)], images[static_cast<size_t>(layer + 1)])) {
                return false;
            }
        }
        if (!getImage(resource.controlKey, images[5])) {
            return false;
        }

        std::array<VkWriteDescriptorSet, 7> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = resource.descriptorSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        for (uint32_t binding = 1; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = resource.descriptorSets[frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding - 1];
        }
        vkUpdateDescriptorSets(g_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}

bool TerrainRenderer::EnsureInstanceCapacity(Resource& resource, size_t visibleCount) {
    if (visibleCount <= resource.instanceCapacity) {
        return true;
    }
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    const size_t newCapacity = std::max(visibleCount, std::max<size_t>(1, resource.instanceCapacity * 2));
    return CreateInstanceBuffers(resource, newCapacity);
}

bool TerrainRenderer::EnsureCsmInstanceCapacity(Resource& resource, size_t visibleCount) {
    if (visibleCount <= resource.csmInstanceCapacity) {
        return true;
    }
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const size_t newCapacity = std::max(
        visibleCount, std::max<size_t>(1, resource.csmInstanceCapacity * 2));
    for (auto& buffer : resource.csmInstanceBuffers) {
        buffer.Cleanup();
        if (!buffer.Create(static_cast<VkDeviceSize>(newCapacity * sizeof(TerrainChunkInstance)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.csmInstanceBuffers) {
                cleanup.Cleanup();
            }
            resource.csmInstanceCapacity = 0;
            return false;
        }
    }
    resource.csmInstanceCapacity = newCapacity;
    return true;
}

void TerrainRenderer::UpdateUniform(Resource& resource,
                                    const glm::mat4& projView,
                                    const glm::mat4& prevProjView,
                                    const glm::vec3& cameraPosition) {
    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    TerrainUniformData uniform;
    uniform.projView = projView;
    uniform.prevProjView = prevProjView;
    uniform.model = resource.model;
    uniform.prevModel = resource.hasPreviousModel ? resource.previousModel : resource.model;
    uniform.normalMatrix = glm::transpose(glm::inverse(resource.model));
    uniform.heightParams = glm::vec4(resource.settings.heightScale,
                                     resource.settings.heightOffset,
                                     std::max(0.001f, resource.settings.materialTiling),
                                     std::max(0.001f, resource.settings.worldSize.x));
    uniform.materialParams = glm::vec4(std::max(0.001f, resource.settings.worldSize.y),
                                       resource.useControlMap ? 1.0f : 0.0f,
                                       std::max(0.01f, resource.settings.blendSharpness),
                                       0.0f);
    uniform.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    uniform.taaJitter = glm::vec4(g_CurrentTAAJitter, 0.0f, 0.0f);
    resource.uniformBuffers[frame]->Write(&uniform, sizeof(uniform));
}

void TerrainRenderer::Render(VkCommandBuffer commandBuffer, int width, int height,
                             const glm::mat4& projView,
                             const glm::mat4& prevProjView,
                             const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, prevProjView, cameraPosition, false);
}

void TerrainRenderer::RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                                         const glm::mat4& projView,
                                         const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, projView, cameraPosition, true);
}

void TerrainRenderer::RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& shadowProjView,
                                     const glm::vec3& cameraPosition) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 ||
        m_PreparedResources.empty() || m_CsmDepthPipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }

    const VkPipeline pipeline = m_CsmDepthPipeline.GetPipeline();
    const VkPipelineLayout pipelineLayout = m_CsmDepthPipeline.GetLayout();
    if (pipelineLayout == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &shadowProjView);

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        const size_t visibleCount = resource->chunks.GetVisibleCount();
        if (visibleCount == 0 || !EnsureCsmInstanceCapacity(*resource, visibleCount)) {
            continue;
        }

        std::vector<TerrainChunkInstance> instances;
        instances.reserve(visibleCount);
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            const float edgeIntervals = static_cast<float>(
                std::max(resource->patches[static_cast<size_t>(lod)].resolution, 2u) - 1u);
            for (TerrainChunkInstance instance : visible) {
                instance.params.z = edgeIntervals;
                instances.push_back(instance);
            }
        }
        resource->csmInstanceBuffers[frame].Write(
            instances.data(), static_cast<VkDeviceSize>(instances.size() * sizeof(TerrainChunkInstance)));
        // The CSM vertex shader takes the cascade matrix via push constants;
        // the UBO still supplies the terrain model and heightmap parameters.
        UpdateUniform(*resource, shadowProjView, shadowProjView, cameraPosition);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &resource->descriptorSets[frame], 0, nullptr);

        uint32_t firstInstance = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            if (visible.empty()) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {
                resource->patches[static_cast<size_t>(lod)].vertexBuffer.GetBuffer(),
                resource->csmInstanceBuffers[frame].GetBuffer()
            };
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer,
                                 resource->patches[static_cast<size_t>(lod)].indexBuffer.GetBuffer(),
                                 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(commandBuffer,
                             resource->patches[static_cast<size_t>(lod)].indexCount,
                             static_cast<uint32_t>(visible.size()),
                             0, 0, firstInstance);
            firstInstance += static_cast<uint32_t>(visible.size());
        }
    }
}

void TerrainRenderer::RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView,
                                     const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     bool depthOnly) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 || m_PreparedResources.empty()) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // viewport/scissor 是 dynamic state，所有管线一致，循环外设置一次。
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        // 主渲染按 resource 的 wireframe 标志选择线框/实体管线；
        // 深度 pass 固定使用深度管线（线框只影响颜色显示，不影响深度/CSM）。
        VkPipeline pipeline = m_Pipeline.GetPipeline();
        VkPipelineLayout pipelineLayout = m_Pipeline.GetLayout();
        if (depthOnly) {
            pipeline = m_DepthPipeline.GetPipeline();
            pipelineLayout = m_DepthPipeline.GetLayout();
        } else if (resource->settings.wireframe &&
                   m_WireframePipeline.GetPipeline() != VK_NULL_HANDLE) {
            pipeline = m_WireframePipeline.GetPipeline();
            pipelineLayout = m_WireframePipeline.GetLayout();
        }
        if (pipeline == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE) {
            continue;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        const size_t visibleCount = resource->chunks.GetVisibleCount();
        if (visibleCount == 0 || !EnsureInstanceCapacity(*resource, visibleCount)) {
            continue;
        }

        std::vector<TerrainChunkInstance> instances;
        instances.reserve(visibleCount);
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            const float edgeIntervals = static_cast<float>(
                std::max(resource->patches[static_cast<size_t>(lod)].resolution, 2u) - 1u);
            for (TerrainChunkInstance instance : visible) {
                instance.params.z = edgeIntervals;
                instances.push_back(instance);
            }
        }
        resource->instanceBuffers[frame].Write(
            instances.data(), static_cast<VkDeviceSize>(instances.size() * sizeof(TerrainChunkInstance)));
        UpdateUniform(*resource, projView, prevProjView, cameraPosition);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &resource->descriptorSets[frame], 0, nullptr);

        uint32_t firstInstance = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            if (visible.empty()) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {
                resource->patches[static_cast<size_t>(lod)].vertexBuffer.GetBuffer(),
                resource->instanceBuffers[frame].GetBuffer()
            };
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer,
                                 resource->patches[static_cast<size_t>(lod)].indexBuffer.GetBuffer(),
                                 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(commandBuffer,
                             resource->patches[static_cast<size_t>(lod)].indexCount,
                             static_cast<uint32_t>(visible.size()),
                             0, 0, firstInstance);
            firstInstance += static_cast<uint32_t>(visible.size());
        }

        if (!depthOnly) {
            resource->previousModel = resource->model;
            resource->hasPreviousModel = true;
        }
    }
}

std::string TerrainRenderer::MakeTextureKey(ECS::Entity entity, const char* slot) {
    return "__terrain_" + std::to_string(static_cast<uint32_t>(entity)) + "_" + slot;
}

const std::string& TerrainRenderer::GetLayerPath(const ECS::TerrainComponent& settings, int layer) {
    switch (layer) {
    case 0: return settings.layer0Path;
    case 1: return settings.layer1Path;
    case 2: return settings.layer2Path;
    case 3: return settings.layer3Path;
    default: {
        static const std::string empty;
        return empty;
    }
    }
}
