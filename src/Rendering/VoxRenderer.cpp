#include "Rendering/VoxRenderer.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "EngineConfig.h"
#include <iostream>
#include <limits>
#include <filesystem>
#include <functional>

// 初始化静态成员
std::unordered_map<size_t, VoxRenderer::MeshCacheEntry> VoxRenderer::s_meshCache;
std::mutex VoxRenderer::s_meshCacheMutex;

VoxRenderer::VoxRenderer()
    : m_VoxelSize(1.0f)
    , m_Loaded(false)
    , m_VoxelCount(0)
    , m_voxelDataHash(0)
    , m_useCachedMesh(false)
{
}

VoxRenderer::~VoxRenderer()
{
    std::cout << "[VoxRenderer] Destructor called for: " << m_FilePath << std::endl;
}

void VoxRenderer::Cleanup()
{
    // 清理体素纹理管理器
    m_Texture3DManager.Cleanup();
    
    m_RenderData.pipeline.Cleanup();
    m_RenderData.wireframePipeline.Cleanup();
    m_RenderData.meshPipeline.Cleanup();
    m_RenderData.meshWireframePipeline.Cleanup();
    m_RenderData.depthPipeline.Cleanup();
    m_RenderData.meshDepthPipeline.Cleanup();
    
    // 清空背面剔除缓存
    m_cullingCache.clear();
    
    // 清理全局网格缓存（只在最后一个 VoxRenderer 销毁时清理）
    {
        std::lock_guard<std::mutex> lock(s_meshCacheMutex);
        for (auto& [hash, entry] : s_meshCache) {
            // 内联清理 Vulkan 资源
            if (entry.meshData.vertexBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, entry.meshData.vertexBuffer, g_Allocator);
                entry.meshData.vertexBuffer = VK_NULL_HANDLE;
            }
            if (entry.meshData.vertexBufferMemory != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, entry.meshData.vertexBufferMemory, g_Allocator);
                entry.meshData.vertexBufferMemory = VK_NULL_HANDLE;
            }
            if (entry.meshData.indexBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, entry.meshData.indexBuffer, g_Allocator);
                entry.meshData.indexBuffer = VK_NULL_HANDLE;
            }
            if (entry.meshData.indexBufferMemory != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, entry.meshData.indexBufferMemory, g_Allocator);
                entry.meshData.indexBufferMemory = VK_NULL_HANDLE;
            }
            entry.isValid = false;
        }
        s_meshCache.clear();
    }
    
    if (m_RenderData.quadVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_RenderData.quadVertexBuffer, g_Allocator);
        m_RenderData.quadVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_RenderData.quadVertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_RenderData.quadVertexBufferMemory, g_Allocator);
        m_RenderData.quadVertexBufferMemory = VK_NULL_HANDLE;
    }
    
    if (m_RenderData.faceBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_RenderData.faceBuffer, g_Allocator);
        m_RenderData.faceBuffer = VK_NULL_HANDLE;
    }
    if (m_RenderData.faceBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_RenderData.faceBufferMemory, g_Allocator);
        m_RenderData.faceBufferMemory = VK_NULL_HANDLE;
    }
    
    if (m_RenderData.instanceBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_RenderData.instanceBuffer, g_Allocator);
        m_RenderData.instanceBuffer = VK_NULL_HANDLE;
    }
    if (m_RenderData.instanceBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_RenderData.instanceBufferMemory, g_Allocator);
        m_RenderData.instanceBufferMemory = VK_NULL_HANDLE;
    }
    
    if (m_MeshData.vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_MeshData.vertexBuffer, g_Allocator);
        m_MeshData.vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_MeshData.vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_MeshData.vertexBufferMemory, g_Allocator);
        m_MeshData.vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_MeshData.indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_MeshData.indexBuffer, g_Allocator);
        m_MeshData.indexBuffer = VK_NULL_HANDLE;
    }
    if (m_MeshData.indexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_MeshData.indexBufferMemory, g_Allocator);
        m_MeshData.indexBufferMemory = VK_NULL_HANDLE;
    }
    
    for (size_t i = 0; i < VoxelRenderData::MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_RenderData.meshInstanceBuffers[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, m_RenderData.meshInstanceBuffers[i], g_Allocator);
            m_RenderData.meshInstanceBuffers[i] = VK_NULL_HANDLE;
        }
        if (m_RenderData.meshInstanceBufferMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_RenderData.meshInstanceBufferMemories[i], g_Allocator);
            m_RenderData.meshInstanceBufferMemories[i] = VK_NULL_HANDLE;
        }
    }
    
    BaseRenderer::Cleanup();
    
    m_Faces.clear();
    m_VoxelGrid.clear();
    m_Loaded = false;
}

void VoxRenderer::Init(VkRenderPass renderPass)
{
    std::cout << "[VoxRenderer] Init started" << std::endl;
    
    CreateQuadVertexBuffer();
    CreateMeshInstanceBuffer(100);
    CreatePipeline(renderPass);
    
    std::cout << "[VoxRenderer] Init completed successfully" << std::endl;
}

void VoxRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
}

bool VoxRenderer::LoadVoxFile(const std::string& path, float voxelSize)
{
    m_FilePath = path;
    
    VoxFormat::VoxData voxData;
    if (!VoxFormat::LoadVoxFile(path, voxData)) {
        std::cerr << "[VoxRenderer] Failed to load VOX file: " << path << std::endl;
        return false;
    }
    
    return LoadFromVoxData(voxData, voxelSize);
}

bool VoxRenderer::LoadFromVoxData(const VoxFormat::VoxData& voxData, float voxelSize)
{
    m_VoxelSize = voxelSize;
    m_Faces.clear();
    m_VoxelGrid.clear();
    
    BuildVoxelFaces(voxData, voxelSize);
    
    if (m_Faces.empty()) {
        std::cerr << "[VoxRenderer] No faces generated" << std::endl;
        return false;
    }
    
    CreateFaceBuffer(m_Faces.size());
    CreateInstanceBuffer(1024 * 1024);
    
    BuildTriangleMesh();
    
    m_Loaded = true;
    std::cout << "[VoxRenderer] Loaded " << m_VoxelCount << " voxels, " 
              << m_Faces.size() << " faces" << std::endl;
    
    return true;
}

// Texture3D 管理器方法
VkDescriptorSet VoxRenderer::GetVoxelDescriptorSet() const {
    if (!m_Texture3DManagerInitialized) {
        return VK_NULL_HANDLE;
    }
    return m_Texture3DManager.GetDescriptorSet();
}

VkPipelineLayout VoxRenderer::GetVoxelPipelineLayout() const {
    if (!m_Texture3DManagerInitialized) {
        return VK_NULL_HANDLE;
    }
    return m_Texture3DManager.GetPipelineLayout();
}

VkSampler VoxRenderer::GetVoxelSampler() const {
    if (!m_Texture3DManagerInitialized) {
        return VK_NULL_HANDLE;
    }
    return m_Texture3DManager.GetSampler();
}

bool VoxRenderer::GenerateTexture3DCache()
{
    if (m_FilePath.empty()) {
        std::cerr << "[VoxRenderer::GenerateTexture3DCache] No vox file loaded!" << std::endl;
        return false;
    }
    
    std::string cachePath = GetTexture3DCachePath();
    if (cachePath.empty()) {
        return false;
    }
    
    // 先尝试加载现有缓存
    std::vector<VoxFormat::Color> palette;
    std::vector<uint8_t> voxelData;
    uint32_t sizeX, sizeY, sizeZ;
    
    if (m_Texture3DCacheGen.LoadCache(cachePath, voxelData, palette, sizeX, sizeY, sizeZ)) {
        // 缓存加载成功
        m_Texture3DData = std::move(voxelData);
        m_Texture3DPalette = std::move(palette);
        m_Texture3DSizeX = sizeX;
        m_Texture3DSizeY = sizeY;
        m_Texture3DSizeZ = sizeZ;
        
        std::cout << "[VoxRenderer::GenerateTexture3DCache] Texture3D cache loaded from: " << cachePath << std::endl;
        
        // 创建 GPU Texture3D
        if (!m_Texture3DManagerInitialized) {
            VoxelTexture3DManagerConfig config;
            config.maxTextures = 64;
            config.format = VK_FORMAT_R8_UINT;
            config.enableMipmaps = false;
            config.filter = VK_FILTER_NEAREST;
            
            if (!m_Texture3DManager.Initialize(config)) {
                std::cerr << "[VoxRenderer::GenerateTexture3DCache] Failed to initialize Texture3D manager!" << std::endl;
                return false;
            }
            m_Texture3DManagerInitialized = true;
        }
        
        // 创建 Texture3D
        m_VoxelTextureIndex = m_Texture3DManager.CreateTexture3D(
            m_FilePath,
            m_Texture3DData,
            m_Texture3DSizeX,
            m_Texture3DSizeY,
            m_Texture3DSizeZ
        );
        
        if (m_VoxelTextureIndex == UINT32_MAX) {
            std::cerr << "[VoxRenderer::GenerateTexture3DCache] Failed to create Texture3D!" << std::endl;
            return false;
        }
        
        return true;
    }
    
    // Android 不生成缓存，只读取
#ifdef ANDROID_BUILD
    std::cout << "[VoxRenderer::GenerateTexture3DCache] Cache not found on Android, skipping cache generation" << std::endl;
    return false;
#else
    // 缓存不存在或加载失败，生成新缓存
    std::cout << "[VoxRenderer::GenerateTexture3DCache] Cache miss, generating Texture3D cache..." << std::endl;
    
    // 重新加载 vox 数据来生成缓存
    VoxFormat::VoxData voxData;
    if (!VoxFormat::LoadVoxFile(m_FilePath, voxData)) {
        std::cerr << "[VoxRenderer::GenerateTexture3DCache] Failed to load vox file!" << std::endl;
        return false;
    }
    
    if (!m_Texture3DCacheGen.GenerateCache(voxData, m_FilePath, cachePath)) {
        std::cerr << "[VoxRenderer::GenerateTexture3DCache] Failed to generate cache!" << std::endl;
        return false;
    }
    
    // 加载刚生成的缓存
    return LoadTexture3DCache();
#endif
}

size_t VoxRenderer::ComputeVoxelDataHash(const VoxFormat::VoxData& voxData) const
{
    // 使用 FNV-1a 哈希算法
    const size_t FNV_PRIME = 1099511628211;
    const size_t FNV_OFFSET = 14695981039346656037;
    
    size_t hash = FNV_OFFSET;
    
    if (voxData.models.empty()) {
        return hash;
    }
    
    const auto& model = voxData.models[0];
    
    // 哈希体素数量
    hash ^= model.voxels.size();
    hash *= FNV_PRIME;
    
    // 哈希每个体素的位置和颜色
    for (const auto& voxel : model.voxels) {
        hash ^= static_cast<size_t>(voxel.x);
        hash *= FNV_PRIME;
        hash ^= static_cast<size_t>(voxel.y);
        hash *= FNV_PRIME;
        hash ^= static_cast<size_t>(voxel.z);
        hash *= FNV_PRIME;
        hash ^= static_cast<size_t>(voxel.colorIndex);
        hash *= FNV_PRIME;
    }
    
    return hash;
}

bool VoxRenderer::TryLoadMeshFromCache()
{
    std::lock_guard<std::mutex> lock(s_meshCacheMutex);
    
    auto it = s_meshCache.find(m_voxelDataHash);
    if (it != s_meshCache.end() && it->second.isValid) {
        // 从缓存中复制面数据
        m_Faces = it->second.faces;
        
        // 复制网格数据（注意：Vulkan 资源不能直接复制，需要重新创建）
        // 这里只复制元数据，实际 Vulkan 资源会在 BuildTriangleMesh 中创建
        m_MeshData.vertexCount = it->second.meshData.vertexCount;
        m_MeshData.indexCount = it->second.meshData.indexCount;
        m_MeshData.faceGroups = it->second.meshData.faceGroups;
        
        std::cout << "[VoxRenderer] Mesh cache hit! Hash: " << m_voxelDataHash 
                  << ", Faces: " << m_Faces.size() << std::endl;
        
        return true;
    }
    
    std::cout << "[VoxRenderer] Mesh cache miss. Hash: " << m_voxelDataHash << std::endl;
    return false;
}

void VoxRenderer::SaveMeshToCache()
{
    std::lock_guard<std::mutex> lock(s_meshCacheMutex);
    
    auto it = s_meshCache.find(m_voxelDataHash);
    if (it == s_meshCache.end()) {
        s_meshCache[m_voxelDataHash] = MeshCacheEntry();
        it = s_meshCache.find(m_voxelDataHash);
    }
    
    // 保存面数据
    it->second.hash = m_voxelDataHash;
    it->second.faces = m_Faces;
    it->second.meshData.vertexCount = m_MeshData.vertexCount;
    it->second.meshData.indexCount = m_MeshData.indexCount;
    it->second.meshData.faceGroups = m_MeshData.faceGroups;
    it->second.isValid = true;
    
    std::cout << "[VoxRenderer] Saved mesh to cache. Hash: " << m_voxelDataHash 
              << ", Faces: " << m_Faces.size() << std::endl;
}

int VoxRenderer::HasVoxelAt(int x, int y, int z) const
{
    if (x < 0 || x >= m_GridSize.x || y < 0 || y >= m_GridSize.y || z < 0 || z >= m_GridSize.z)
        return -1;
    return m_VoxelGrid[z * m_GridSize.x * m_GridSize.y + y * m_GridSize.x + x];
}

void VoxRenderer::BuildVoxelFaces(const VoxFormat::VoxData& voxData, float voxelSize)
{
    m_Faces.clear();
    
    if (voxData.models.empty()) {
        return;
    }
    
    const auto& model = voxData.models[0];
    m_VoxelCount = model.voxels.size();
    
    // 计算包围盒
    int minX = INT_MAX, minY = INT_MAX, minZ = INT_MAX;
    int maxX = INT_MIN, maxY = INT_MIN, maxZ = INT_MIN;
    
    for (const auto& voxel : model.voxels) {
        minX = std::min(minX, (int)voxel.x);
        minY = std::min(minY, (int)voxel.y);
        minZ = std::min(minZ, (int)voxel.z);
        maxX = std::max(maxX, (int)voxel.x);
        maxY = std::max(maxY, (int)voxel.y);
        maxZ = std::max(maxZ, (int)voxel.z);
    }
    
    float offsetX = (maxX + minX + 1) * voxelSize * 0.5f;
    float offsetY = (maxY + minY + 1) * voxelSize * 0.5f;
    float offsetZ = (maxZ + minZ + 1) * voxelSize * 0.5f;
    
    m_MinBounds = glm::vec3(minX * voxelSize - offsetX, minZ * voxelSize - offsetZ, minY * voxelSize - offsetY);
    m_MaxBounds = glm::vec3((maxX + 1) * voxelSize - offsetX, (maxZ + 1) * voxelSize - offsetZ, (maxY + 1) * voxelSize - offsetY);
    
    std::cout << "[VoxRenderer] Bounds: " << m_MinBounds.x << "," << m_MinBounds.y << "," << m_MinBounds.z 
              << " to " << m_MaxBounds.x << "," << m_MaxBounds.y << "," << m_MaxBounds.z << std::endl;
    
    // 计算体素数据哈希
    m_voxelDataHash = ComputeVoxelDataHash(voxData);
    
    // 尝试从缓存加载
    if (TryLoadMeshFromCache()) {
        m_useCachedMesh = true;
        // 缓存命中，直接使用缓存的面数据，跳过体素网格初始化和面生成
        return;
    }
    
    m_useCachedMesh = false;
    
    // 初始化体素网格（线性数组）
    m_GridSize = glm::ivec3(maxX + 1, maxY + 1, maxZ + 1);
    size_t gridSize = static_cast<size_t>(m_GridSize.x) * m_GridSize.y * m_GridSize.z;
    m_VoxelGrid.assign(gridSize, -1);
    
    // 填充实素网格
    for (const auto& voxel : model.voxels) {
        size_t index = voxel.z * m_GridSize.x * m_GridSize.y + 
                       voxel.y * m_GridSize.x + 
                       voxel.x;
        m_VoxelGrid[index] = static_cast<int8_t>(voxel.colorIndex);
    }
    
    size_t originalFaceCount = 0;
    
    for (const auto& voxel : model.voxels) {
        int x = voxel.x;
        int y = voxel.y;
        int z = voxel.z;
        
        const auto& c = voxData.palette[voxel.colorIndex];
        uint32_t r = static_cast<uint32_t>(c.r);
        uint32_t g = static_cast<uint32_t>(c.g);
        uint32_t b = static_cast<uint32_t>(c.b);
        
        if (!HasVoxelAt(x, y, z + 1)) originalFaceCount++;
        if (!HasVoxelAt(x, y, z - 1)) originalFaceCount++;
        if (!HasVoxelAt(x - 1, y, z)) originalFaceCount++;
        if (!HasVoxelAt(x + 1, y, z)) originalFaceCount++;
        if (!HasVoxelAt(x, y + 1, z)) originalFaceCount++;
        if (!HasVoxelAt(x, y - 1, z)) originalFaceCount++;
    }
    
    for (int faceDir = 0; faceDir < 6; faceDir++) {
        BuildGreedyMeshForFace(voxData, voxelSize, faceDir, offsetX, offsetY, offsetZ);
    }
    
    std::cout << "[VoxRenderer] Greedy meshing: " << originalFaceCount << " faces -> " << m_Faces.size() 
              << " faces (" << (100.0 * m_Faces.size() / originalFaceCount) << "%)" << std::endl;
}

void VoxRenderer::BuildGreedyMeshForFace(const VoxFormat::VoxData& voxData, float voxelSize, int faceDir,
                                          float offsetX, float offsetY, float offsetZ)
{
    const auto& model = voxData.models[0];
    
    int minX = INT_MAX, minY = INT_MAX, minZ = INT_MAX;
    int maxX = INT_MIN, maxY = INT_MIN, maxZ = INT_MIN;
    
    for (const auto& voxel : model.voxels) {
        minX = std::min(minX, (int)voxel.x);
        minY = std::min(minY, (int)voxel.y);
        minZ = std::min(minZ, (int)voxel.z);
        maxX = std::max(maxX, (int)voxel.x);
        maxY = std::max(maxY, (int)voxel.y);
        maxZ = std::max(maxZ, (int)voxel.z);
    }
    
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> slices;
    
    for (const auto& voxel : model.voxels) {
        int x = voxel.x;
        int y = voxel.y;
        int z = voxel.z;
        
        bool hasFace = false;
        int sliceCoord = 0;
        int u = 0, v = 0;
        
        switch (faceDir) {
            case 0:
                hasFace = (HasVoxelAt(x, y, z + 1) == -1);
                sliceCoord = z + 1;
                u = x;
                v = y;
                break;
            case 1:
                hasFace = (HasVoxelAt(x, y, z - 1) == -1);
                sliceCoord = z;
                u = x;
                v = y;
                break;
            case 2:
                hasFace = (HasVoxelAt(x - 1, y, z) == -1);
                sliceCoord = x;
                u = y;
                v = z;
                break;
            case 3:
                hasFace = (HasVoxelAt(x + 1, y, z) == -1);
                sliceCoord = x + 1;
                u = y;
                v = z;
                break;
            case 4:
                hasFace = (HasVoxelAt(x, y + 1, z) == -1);
                sliceCoord = y + 1;
                u = x;
                v = z;
                break;
            case 5:
                hasFace = (HasVoxelAt(x, y - 1, z) == -1);
                sliceCoord = y;
                u = x;
                v = z;
                break;
        }
        
        if (hasFace) {
            int colorIndex = voxel.colorIndex;
            slices[{sliceCoord, colorIndex}].push_back({u, v});
        }
    }
    
    for (auto& [key, positions] : slices) {
        int sliceCoord = key.first;
        int colorIndex = key.second;
        
        std::set<std::pair<int, int>> posSet(positions.begin(), positions.end());
        
        while (!posSet.empty()) {
            auto start = *posSet.begin();
            int u = start.first;
            int v = start.second;
            
            int width = 1;
            while (posSet.find({u + width, v}) != posSet.end()) {
                width++;
            }
            
            int height = 1;
            bool canExtend = true;
            while (canExtend) {
                for (int w = 0; w < width; w++) {
                    if (posSet.find({u + w, v + height}) == posSet.end()) {
                        canExtend = false;
                        break;
                    }
                }
                if (canExtend) height++;
            }
            
            for (int h = 0; h < height; h++) {
                for (int w = 0; w < width; w++) {
                    posSet.erase({u + w, v + h});
                }
            }
            
            VoxelFaceData face;
            face.size = glm::vec2(width * voxelSize, height * voxelSize);
            
            const auto& c = voxData.palette[colorIndex];
            uint32_t r = static_cast<uint32_t>(c.r);
            uint32_t g = static_cast<uint32_t>(c.g);
            uint32_t b = static_cast<uint32_t>(c.b);
            
            int mappedFaceDir = faceDir;
            switch (faceDir) {
                case 0: mappedFaceDir = 4; break;
                case 1: mappedFaceDir = 5; break;
                case 4: mappedFaceDir = 0; break;
                case 5: mappedFaceDir = 1; break;
            }
            
            face.data = mappedFaceDir | (r << 8) | (g << 16) | (b << 24);
            
            float worldU = (u + width * 0.5f) * voxelSize;
            float worldV = (v + height * 0.5f) * voxelSize;
            float worldSlice = sliceCoord * voxelSize;
            
            switch (faceDir) {
                case 0:
                case 1:
                    face.position = glm::vec3(worldU - offsetX, worldSlice - offsetZ, worldV - offsetY);
                    break;
                case 2:
                case 3:
                    face.position = glm::vec3(worldSlice - offsetX, worldV - offsetZ, worldU - offsetY);
                    break;
                case 4:
                case 5:
                    face.position = glm::vec3(worldU - offsetX, worldV - offsetZ, worldSlice - offsetY);
                    break;
            }
            
            m_Faces.push_back(face);
        }
    }
}

void VoxRenderer::BuildTriangleMesh()
{
    if (m_Faces.empty()) {
        return;
    }
    
    std::vector<VoxelMeshVertex> vertices;
    std::vector<uint32_t> indices;
    
    glm::vec3 boundsSize = m_MaxBounds - m_MinBounds;
    
    // 初始化 6 个面组
    for (int i = 0; i < 6; i++) {
        m_MeshData.faceGroups[i].faceDirection = i;
        m_MeshData.faceGroups[i].firstIndex = 0;
        m_MeshData.faceGroups[i].indexCount = 0;
        m_MeshData.faceGroups[i].faceCenter = glm::vec3(0.0f);
        m_MeshData.faceGroups[i].faceExtent = glm::vec3(0.0f);
    }
    
    // 按面方向分组构建网格
    for (int faceDir = 0; faceDir < 6; faceDir++) {
        size_t groupStartIndex = indices.size();
        glm::vec3 faceCenterSum(0.0f);
        size_t faceCount = 0;
        
        // 用于计算包围盒范围的边界
        glm::vec3 minPos(FLT_MAX), maxPos(-FLT_MAX);
        
        // 遍历所有面，只处理当前方向的面
        for (const auto& face : m_Faces) {
            uint32_t currentFaceDir = face.data & 0xFF;
            if (currentFaceDir != faceDir) continue;
            
            // 累加面的中心位置
            faceCenterSum += face.position;
            faceCount++;
            
            // 更新边界
            minPos = glm::min(minPos, face.position);
            maxPos = glm::max(maxPos, face.position);
            
            // 解包颜色（从 face.data 的高 24 位）
            uint32_t colorPacked = (face.data >> 8);
            uint8_t r = colorPacked & 0xFF;
            uint8_t g = (colorPacked >> 8) & 0xFF;
            uint8_t b = (colorPacked >> 16) & 0xFF;
            
            // 打包面方向和材质索引到 16 bits
            // 位分配：[0-2] 面方向 (3 bits), [3-15] 材质索引 (13 bits)
            uint8_t materialIndex = 0;  // 默认材质，后续可以从 face 数据中获取
            uint16_t packedData = ((faceDir & 0x07) | ((materialIndex & 0x1FFF) << 3));
            
            glm::vec3 corners[4];
            glm::vec3 basePos = face.position;
            glm::vec2 size = face.size;
            
            // 根据面方向计算 4 个角点
            switch (faceDir) {
                case 0: // +Z
                    corners[0] = basePos + glm::vec3(-size.x * 0.5f, -size.y * 0.5f, 0);
                    corners[1] = basePos + glm::vec3(size.x * 0.5f, -size.y * 0.5f, 0);
                    corners[2] = basePos + glm::vec3(size.x * 0.5f, size.y * 0.5f, 0);
                    corners[3] = basePos + glm::vec3(-size.x * 0.5f, size.y * 0.5f, 0);
                    break;
                case 1: // -Z
                    corners[0] = basePos + glm::vec3(-size.x * 0.5f, -size.y * 0.5f, 0);
                    corners[1] = basePos + glm::vec3(-size.x * 0.5f, size.y * 0.5f, 0);
                    corners[2] = basePos + glm::vec3(size.x * 0.5f, size.y * 0.5f, 0);
                    corners[3] = basePos + glm::vec3(size.x * 0.5f, -size.y * 0.5f, 0);
                    break;
                case 2: // -X
                    corners[0] = basePos + glm::vec3(0, -size.y * 0.5f, size.x * 0.5f);
                    corners[1] = basePos + glm::vec3(0, size.y * 0.5f, size.x * 0.5f);
                    corners[2] = basePos + glm::vec3(0, size.y * 0.5f, -size.x * 0.5f);
                    corners[3] = basePos + glm::vec3(0, -size.y * 0.5f, -size.x * 0.5f);
                    break;
                case 3: // +X
                    corners[0] = basePos + glm::vec3(0, -size.y * 0.5f, -size.x * 0.5f);
                    corners[1] = basePos + glm::vec3(0, size.y * 0.5f, -size.x * 0.5f);
                    corners[2] = basePos + glm::vec3(0, size.y * 0.5f, size.x * 0.5f);
                    corners[3] = basePos + glm::vec3(0, -size.y * 0.5f, size.x * 0.5f);
                    break;
                case 4: // +Y
                    corners[0] = basePos + glm::vec3(-size.x * 0.5f, 0, -size.y * 0.5f);
                    corners[1] = basePos + glm::vec3(-size.x * 0.5f, 0, size.y * 0.5f);
                    corners[2] = basePos + glm::vec3(size.x * 0.5f, 0, size.y * 0.5f);
                    corners[3] = basePos + glm::vec3(size.x * 0.5f, 0, -size.y * 0.5f);
                    break;
                case 5: // -Y
                    corners[0] = basePos + glm::vec3(-size.x * 0.5f, 0, size.y * 0.5f);
                    corners[1] = basePos + glm::vec3(-size.x * 0.5f, 0, -size.y * 0.5f);
                    corners[2] = basePos + glm::vec3(size.x * 0.5f, 0, -size.y * 0.5f);
                    corners[3] = basePos + glm::vec3(size.x * 0.5f, 0, size.y * 0.5f);
                    break;
            }
            
            uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
            
            // 添加 4 个顶点
            for (int i = 0; i < 4; i++) {
                glm::vec3 relativePos = corners[i] - m_MinBounds;
                
                vertices.push_back({
                    static_cast<unsigned char>(glm::clamp(relativePos.x / m_VoxelSize, 0.0f, 255.0f)),
                    static_cast<unsigned char>(glm::clamp(relativePos.y / m_VoxelSize, 0.0f, 255.0f)),
                    static_cast<unsigned char>(glm::clamp(relativePos.z / m_VoxelSize, 0.0f, 255.0f)),
                    r, g, b,
                    packedData
                });
            }
            
            // 添加 2 个三角形（6 个索引）
            indices.push_back(baseIndex + 0);
            indices.push_back(baseIndex + 2);
            indices.push_back(baseIndex + 1);
            
            indices.push_back(baseIndex + 0);
            indices.push_back(baseIndex + 3);
            indices.push_back(baseIndex + 2);
        }
        
        // 记录当前面组的索引范围
        if (indices.size() > groupStartIndex) {
            m_MeshData.faceGroups[faceDir].firstIndex = groupStartIndex;
            m_MeshData.faceGroups[faceDir].indexCount = indices.size() - groupStartIndex;
            // 计算平均中心位置
            if (faceCount > 0) {
                m_MeshData.faceGroups[faceDir].faceCenter = faceCenterSum / static_cast<float>(faceCount);
            }
            
            // 计算包围盒范围（用于动态调整背面剔除阈值）
            glm::vec3 extent = maxPos - minPos;
            m_MeshData.faceGroups[faceDir].faceExtent = extent;
            
            std::cout << "[VoxRenderer] Face group " << faceDir << ": " 
                      << m_MeshData.faceGroups[faceDir].indexCount << " indices, "
                      << "extent: (" << extent.x << ", " << extent.y << ", " << extent.z << ")" << std::endl;
        }
    }
    
    CreateMeshBuffers(vertices, indices);
    
    std::cout << "[VoxRenderer] Built triangle mesh with face groups: " << vertices.size() << " vertices, " 
              << indices.size() / 3 << " triangles" << std::endl;
    
    // 使用生成的顶点和索引构建 BVH
    BuildBVHFromMesh(vertices, indices);
    
    // 保存到缓存（如果之前是缓存未命中）
    if (!m_useCachedMesh) {
        SaveMeshToCache();
    }
}

void VoxRenderer::CreateQuadVertexBuffer()
{
    float vertices[] = {
        0.0f, 0.0f, 0.0f,  // 顶点 0 - 左下
        1.0f, 0.0f, 0.0f,  // 顶点 1 - 右下
        0.0f, 1.0f, 0.0f,  // 顶点 2 - 左上
        1.0f, 1.0f, 0.0f,  // 顶点 3 - 右上
    };
    
    VkDeviceSize bufferSize = sizeof(vertices);
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer!");
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate staging buffer memory!");
    }
    
    vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
    
    void* data;
    vkMapMemory(g_Device, stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices, (size_t)bufferSize);
    vkUnmapMemory(g_Device, stagingBufferMemory);
    
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_RenderData.quadVertexBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to create quad vertex buffer!");
    }
    
    vkGetBufferMemoryRequirements(g_Device, m_RenderData.quadVertexBuffer, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_RenderData.quadVertexBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_RenderData.quadVertexBuffer, g_Allocator);
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate quad vertex buffer memory!");
    }
    
    vkBindBufferMemory(g_Device, m_RenderData.quadVertexBuffer, m_RenderData.quadVertexBufferMemory, 0);
    
    CopyBuffer(stagingBuffer, m_RenderData.quadVertexBuffer, bufferSize);
    
    vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
}

void VoxRenderer::CreateFaceBuffer(size_t maxFaces)
{
    m_RenderData.maxFaceCount = maxFaces;
    m_RenderData.faceCount = maxFaces;
    
    VkDeviceSize bufferSize = sizeof(VoxelFaceData) * maxFaces;
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create staging buffer!");
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate staging buffer memory!");
    }
    
    vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
    
    void* data;
    vkMapMemory(g_Device, stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, m_Faces.data(), (size_t)bufferSize);
    vkUnmapMemory(g_Device, stagingBufferMemory);
    
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_RenderData.faceBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to create face buffer!");
    }
    
    vkGetBufferMemoryRequirements(g_Device, m_RenderData.faceBuffer, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_RenderData.faceBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_RenderData.faceBuffer, g_Allocator);
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate face buffer memory!");
    }
    
    vkBindBufferMemory(g_Device, m_RenderData.faceBuffer, m_RenderData.faceBufferMemory, 0);
    
    CopyBuffer(stagingBuffer, m_RenderData.faceBuffer, bufferSize);
    
    vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
}

void VoxRenderer::CreateInstanceBuffer(size_t maxInstances)
{
    m_RenderData.maxInstanceCount = maxInstances;
    m_RenderData.instanceCount = 0;
    
    VkDeviceSize bufferSize = sizeof(VoxelFaceInstanceData) * maxInstances;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_RenderData.instanceBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create instance buffer!");
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_RenderData.instanceBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_RenderData.instanceBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_RenderData.instanceBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate instance buffer memory!");
    }
    
    vkBindBufferMemory(g_Device, m_RenderData.instanceBuffer, m_RenderData.instanceBufferMemory, 0);
    
    vkMapMemory(g_Device, m_RenderData.instanceBufferMemory, 0, bufferSize, 0, &m_RenderData.mappedInstancePtr);
}

void VoxRenderer::CreateMeshBuffers(const std::vector<VoxelMeshVertex>& vertices, const std::vector<uint32_t>& indices)
{
    if (m_MeshData.vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_MeshData.vertexBuffer, g_Allocator);
        m_MeshData.vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_MeshData.vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_MeshData.vertexBufferMemory, g_Allocator);
        m_MeshData.vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_MeshData.indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_MeshData.indexBuffer, g_Allocator);
        m_MeshData.indexBuffer = VK_NULL_HANDLE;
    }
    if (m_MeshData.indexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_MeshData.indexBufferMemory, g_Allocator);
        m_MeshData.indexBufferMemory = VK_NULL_HANDLE;
    }
    
    VkDeviceSize vertexBufferSize = sizeof(VoxelMeshVertex) * vertices.size();
    VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();
    
    VkBuffer vertexStagingBuffer, indexStagingBuffer;
    VkDeviceMemory vertexStagingMemory, indexStagingMemory;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    bufferInfo.size = vertexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &vertexStagingBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex staging buffer!");
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, vertexStagingBuffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &vertexStagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate vertex staging memory!");
    }
    vkBindBufferMemory(g_Device, vertexStagingBuffer, vertexStagingMemory, 0);
    
    void* data;
    vkMapMemory(g_Device, vertexStagingMemory, 0, vertexBufferSize, 0, &data);
    memcpy(data, vertices.data(), (size_t)vertexBufferSize);
    vkUnmapMemory(g_Device, vertexStagingMemory);
    
    bufferInfo.size = indexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &indexStagingBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to create index staging buffer!");
    }
    
    vkGetBufferMemoryRequirements(g_Device, indexStagingBuffer, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &indexStagingMemory) != VK_SUCCESS) {
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate index staging memory!");
    }
    vkBindBufferMemory(g_Device, indexStagingBuffer, indexStagingMemory, 0);
    
    vkMapMemory(g_Device, indexStagingMemory, 0, indexBufferSize, 0, &data);
    memcpy(data, indices.data(), (size_t)indexBufferSize);
    vkUnmapMemory(g_Device, indexStagingMemory);
    
    bufferInfo.size = vertexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_MeshData.vertexBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, indexStagingMemory, g_Allocator);
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to create vertex buffer!");
    }
    
    vkGetBufferMemoryRequirements(g_Device, m_MeshData.vertexBuffer, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_MeshData.vertexBufferMemory) != VK_SUCCESS) {
        vkFreeMemory(g_Device, indexStagingMemory, g_Allocator);
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, m_MeshData.vertexBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate vertex buffer memory!");
    }
    vkBindBufferMemory(g_Device, m_MeshData.vertexBuffer, m_MeshData.vertexBufferMemory, 0);
    
    bufferInfo.size = indexBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_MeshData.indexBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, indexStagingMemory, g_Allocator);
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        throw std::runtime_error("Failed to create index buffer!");
    }
    
    vkGetBufferMemoryRequirements(g_Device, m_MeshData.indexBuffer, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_MeshData.indexBufferMemory) != VK_SUCCESS) {
        vkFreeMemory(g_Device, indexStagingMemory, g_Allocator);
        vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
        vkDestroyBuffer(g_Device, m_MeshData.indexBuffer, g_Allocator);
        throw std::runtime_error("Failed to allocate index buffer memory!");
    }
    vkBindBufferMemory(g_Device, m_MeshData.indexBuffer, m_MeshData.indexBufferMemory, 0);
    
    CopyBuffer(vertexStagingBuffer, m_MeshData.vertexBuffer, vertexBufferSize);
    CopyBuffer(indexStagingBuffer, m_MeshData.indexBuffer, indexBufferSize);
    
    vkFreeMemory(g_Device, indexStagingMemory, g_Allocator);
    vkFreeMemory(g_Device, vertexStagingMemory, g_Allocator);
    vkDestroyBuffer(g_Device, indexStagingBuffer, g_Allocator);
    vkDestroyBuffer(g_Device, vertexStagingBuffer, g_Allocator);
    
    m_MeshData.vertexCount = vertices.size();
    m_MeshData.indexCount = indices.size();
}

void VoxRenderer::UpdateInstanceBuffer(const std::vector<VoxelInstanceData>& instances)
{
    if (m_RenderData.mappedInstancePtr && !instances.empty()) {
        m_RenderData.instanceCount = instances.size();
        memcpy(m_RenderData.mappedInstancePtr, instances.data(), sizeof(VoxelInstanceData) * instances.size());
    }
}

void VoxRenderer::UpdateFaceInstanceBuffer(const std::vector<VoxelFaceInstanceData>& faceInstanceData)
{
    if (m_RenderData.mappedInstancePtr && !faceInstanceData.empty()) {
        m_RenderData.instanceCount = faceInstanceData.size();
        memcpy(m_RenderData.mappedInstancePtr, faceInstanceData.data(), sizeof(VoxelFaceInstanceData) * faceInstanceData.size());
    }
}

void VoxRenderer::CreateMeshInstanceBuffer(size_t maxInstances)
{
    VkDeviceSize bufferSize = sizeof(VoxelInstanceData) * maxInstances;
    m_RenderData.currentMeshInstanceBufferSize = maxInstances;
    
    for (size_t i = 0; i < VoxelRenderData::MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_RenderData.meshInstanceBuffers[i]) != VK_SUCCESS) {
            std::cerr << "[VoxRenderer] Failed to create mesh instance buffer!" << std::endl;
            return;
        }
        
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(g_Device, m_RenderData.meshInstanceBuffers[i], &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_RenderData.meshInstanceBufferMemories[i]) != VK_SUCCESS) {
            std::cerr << "[VoxRenderer] Failed to allocate mesh instance buffer memory!" << std::endl;
            vkDestroyBuffer(g_Device, m_RenderData.meshInstanceBuffers[i], g_Allocator);
            m_RenderData.meshInstanceBuffers[i] = VK_NULL_HANDLE;
            return;
        }
        
        vkBindBufferMemory(g_Device, m_RenderData.meshInstanceBuffers[i], m_RenderData.meshInstanceBufferMemories[i], 0);
        
        vkMapMemory(g_Device, m_RenderData.meshInstanceBufferMemories[i], 0, bufferSize, 0, &m_RenderData.meshInstanceBufferMapped[i]);
    }
}

void VoxRenderer::UpdateMeshInstanceBuffer(const std::vector<VoxelInstanceData>& instances)
{
    if (instances.empty()) {
        return;
    }
    
    uint32_t frameIndex = GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    
    if (instances.size() > m_RenderData.currentMeshInstanceBufferSize) {
        for (size_t i = 0; i < VoxelRenderData::MAX_FRAMES_IN_FLIGHT; i++) {
            if (m_RenderData.meshInstanceBufferMapped[i] != nullptr) {
                vkUnmapMemory(g_Device, m_RenderData.meshInstanceBufferMemories[i]);
                m_RenderData.meshInstanceBufferMapped[i] = nullptr;
            }
            if (m_RenderData.meshInstanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_RenderData.meshInstanceBuffers[i], g_Allocator);
                m_RenderData.meshInstanceBuffers[i] = VK_NULL_HANDLE;
            }
            if (m_RenderData.meshInstanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_RenderData.meshInstanceBufferMemories[i], g_Allocator);
                m_RenderData.meshInstanceBufferMemories[i] = VK_NULL_HANDLE;
            }
        }
        CreateMeshInstanceBuffer(instances.size() * 2);
        frameIndex = GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    }
    
    void* mappedData = m_RenderData.meshInstanceBufferMapped[frameIndex];
    if (mappedData == nullptr) {
        return;
    }
    
    VkDeviceSize bufferSize = sizeof(VoxelInstanceData) * instances.size();
    memcpy(mappedData, instances.data(), (size_t)bufferSize);
}

void VoxRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    if (g_CommandPool == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        return;
    }
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
    
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
}

void VoxRenderer::CreatePipeline(VkRenderPass renderPass)
{
    PipelineConfig config;
    config.vertShader = "voxel.vert.spv";
    config.fragShader = "voxel.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    config.cullMode = VK_CULL_MODE_NONE;
    config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    config.depthTest = true;
    config.depthWrite = true;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0
    };
    config.subpass = 1;               // MRT 几何 subpass（0=z-prepass）
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // z-prepass 后必须 <=（LESS 严格小于会剔除内部像素只剩剪影）
    
    VkVertexInputBindingDescription bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].stride = 3 * sizeof(float);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(VoxelFaceInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    
    VkVertexInputAttributeDescription attributes[12] = {};
    // 四边形顶点属性
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    
    // 面数据属性
    attributes[1].binding = 1;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(VoxelFaceInstanceData, faceData.position);
    
    attributes[2].binding = 1;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32_UINT;
    attributes[2].offset = offsetof(VoxelFaceInstanceData, faceData.data);
    
    attributes[3].binding = 1;
    attributes[3].location = 3;
    attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[3].offset = offsetof(VoxelFaceInstanceData, faceData.size);
    
    // 实例数据属性
    for (int i = 0; i < 4; i++) {
        attributes[4 + i].binding = 1;
        attributes[4 + i].location = 4 + i;
        attributes[4 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[4 + i].offset = offsetof(VoxelFaceInstanceData, instanceData.model) + i * sizeof(glm::vec4);
    }
    
    for (int i = 0; i < 4; i++) {
        attributes[8 + i].binding = 1;
        attributes[8 + i].location = 8 + i;
        attributes[8 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[8 + i].offset = offsetof(VoxelFaceInstanceData, instanceData.prevModel) + i * sizeof(glm::vec4);
    }
    
    config.vertexBindings.assign(bindings, bindings + 2);
    config.vertexAttributes.assign(attributes, attributes + 12);
    
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    // 计算新的 push constant 大小
    struct PushConstants {
        glm::mat4 projView;
        glm::mat4 prevProjView;
        glm::vec3 cameraPosition;
        float padding;
    };
    config.pushConstantRange.size = sizeof(PushConstants);
    config.usePushConstants = true;
    
    if (!m_RenderData.pipeline.Create(renderPass, VK_NULL_HANDLE, config)) {
        throw std::runtime_error("Failed to create voxel pipeline!");
    }
    
    // z-prepass depth-only（面片体素）：同顶点变换（voxel.vert），换 zprepass.frag 只写深度
    if (!g_UseSeparateMrtRenderPass) {
        PipelineConfig depthConfig = config;
        depthConfig.fragShader = "zprepass.frag.spv";
        depthConfig.colorAttachmentCount = kMainMrtZPrepassColorAttachmentCount;
        depthConfig.colorWriteMasks = { 0 };
        depthConfig.subpass = 0;   // z-prepass subpass
        if (!m_RenderData.depthPipeline.Create(renderPass, VK_NULL_HANDLE, depthConfig)) {
            throw std::runtime_error("Failed to create voxel depth pipeline!");
        }
    }
    
    PipelineConfig wireframeConfig = config;
    wireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;
    wireframeConfig.cullMode = VK_CULL_MODE_NONE;
    
    if (!m_RenderData.wireframePipeline.Create(renderPass, VK_NULL_HANDLE, wireframeConfig)) {
        throw std::runtime_error("Failed to create voxel wireframe pipeline!");
    }
    
    PipelineConfig meshConfig;
    meshConfig.vertShader = "voxel_mesh.vert.spv";
    meshConfig.fragShader = "voxel_mesh.frag.spv";
    meshConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    meshConfig.cullMode = VK_CULL_MODE_NONE;
    meshConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    meshConfig.depthTest = true;
    meshConfig.depthWrite = true;
    meshConfig.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    meshConfig.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0
    };
    meshConfig.subpass = 1;               // MRT 几何 subpass（0=z-prepass）
    meshConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // z-prepass 后必须 <=
    
    VkVertexInputBindingDescription meshBindings[2] = {};
    meshBindings[0].binding = 0;
    meshBindings[0].stride = sizeof(VoxelMeshVertex);
    meshBindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    meshBindings[1].binding = 1;
    meshBindings[1].stride = sizeof(VoxelInstanceData);
    meshBindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    
    VkVertexInputAttributeDescription meshAttributes[15] = {};
    meshAttributes[0].binding = 0;
    meshAttributes[0].location = 0;
    meshAttributes[0].format = VK_FORMAT_R8G8B8A8_UINT;
    meshAttributes[0].offset = 0;  // x, y, z, padding
    
    meshAttributes[1].binding = 0;
    meshAttributes[1].location = 1;
    meshAttributes[1].format = VK_FORMAT_R8G8B8A8_UINT;
    meshAttributes[1].offset = 3;  // r, g, b, padding
    
    meshAttributes[2].binding = 0;
    meshAttributes[2].location = 2;
    meshAttributes[2].format = VK_FORMAT_R16_UINT;
    meshAttributes[2].offset = 6;  // faceDirAndMaterial
    
    for (int i = 0; i < 4; i++) {
        meshAttributes[3 + i].binding = 1;
        meshAttributes[3 + i].location = 3 + i;
        meshAttributes[3 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        meshAttributes[3 + i].offset = offsetof(VoxelInstanceData, model) + sizeof(glm::vec4) * i;
    }
    
    for (int i = 0; i < 4; i++) {
        meshAttributes[7 + i].binding = 1;
        meshAttributes[7 + i].location = 7 + i;
        meshAttributes[7 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        meshAttributes[7 + i].offset = offsetof(VoxelInstanceData, prevModel) + sizeof(glm::vec4) * i;
    }
    
    meshAttributes[11].binding = 1;
    meshAttributes[11].location = 11;
    meshAttributes[11].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    meshAttributes[11].offset = offsetof(VoxelInstanceData, albedoColor);
    
    meshAttributes[12].binding = 1;
    meshAttributes[12].location = 12;
    meshAttributes[12].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    meshAttributes[12].offset = offsetof(VoxelInstanceData, materialData);
    
    meshAttributes[13].binding = 1;
    meshAttributes[13].location = 13;
    meshAttributes[13].format = VK_FORMAT_R32G32B32_SFLOAT;
    meshAttributes[13].offset = offsetof(VoxelInstanceData, worldMinBounds);
    
    meshAttributes[14].binding = 1;
    meshAttributes[14].location = 14;
    meshAttributes[14].format = VK_FORMAT_R32_SFLOAT;
    meshAttributes[14].offset = offsetof(VoxelInstanceData, voxelSize);
    
    meshConfig.vertexBindings.assign(meshBindings, meshBindings + 2);
    meshConfig.vertexAttributes.assign(meshAttributes, meshAttributes + 15);
    
    meshConfig.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    meshConfig.pushConstantRange.offset = 0;
    meshConfig.pushConstantRange.size = sizeof(VoxelMeshUniformData);
    meshConfig.usePushConstants = true;
    
    if (!m_RenderData.meshPipeline.Create(renderPass, VK_NULL_HANDLE, meshConfig)) {
        throw std::runtime_error("Failed to create voxel mesh pipeline!");
    }

    // z-prepass depth-only（mesh 体素）：同顶点变换（voxel_mesh.vert），换 zprepass.frag 只写深度
    if (!g_UseSeparateMrtRenderPass) {
        PipelineConfig meshDepthConfig = meshConfig;
        meshDepthConfig.fragShader = "zprepass.frag.spv";
        meshDepthConfig.colorAttachmentCount = kMainMrtZPrepassColorAttachmentCount;
        meshDepthConfig.colorWriteMasks = { 0 };
        meshDepthConfig.subpass = 0;   // z-prepass subpass
        if (!m_RenderData.meshDepthPipeline.Create(renderPass, VK_NULL_HANDLE, meshDepthConfig)) {
            throw std::runtime_error("Failed to create voxel mesh depth pipeline!");
        }
    }
    
    PipelineConfig meshWireframeConfig = meshConfig;
    meshWireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;
    meshWireframeConfig.cullMode = VK_CULL_MODE_NONE;
    
    if (!m_RenderData.meshWireframePipeline.Create(renderPass, VK_NULL_HANDLE, meshWireframeConfig)) {
        throw std::runtime_error("Failed to create voxel mesh wireframe pipeline!");
    }
}

void VoxRenderer::RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView, const glm::mat4& prevProjView,
                                       const glm::vec3& cameraPosition,
                                       const std::vector<VoxelInstanceData>& instances,
                                       bool depthOnly)
{
    if (!m_Loaded || m_Faces.empty()) {
        std::cerr << "[VoxRenderer] Not loaded or no faces!" << std::endl;
        return;
    }
    
    if (instances.empty()) {
        std::cerr << "[VoxRenderer] No instances!" << std::endl;
        return;
    }
    
    if (m_RenderData.quadVertexBuffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxRenderer] Quad vertex buffer is null!" << std::endl;
        return;
    }
    
    if (m_RenderData.faceBuffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxRenderer] Face buffer is null!" << std::endl;
        return;
    }
    
    if (m_RenderData.instanceBuffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxRenderer] Instance buffer is null!" << std::endl;
        return;
    }
    
    if (m_RenderData.pipeline.GetPipeline() == VK_NULL_HANDLE) {
        std::cerr << "[VoxRenderer] Pipeline is null!" << std::endl;
        return;
    }
    
    if (m_RenderData.pipeline.GetLayout() == VK_NULL_HANDLE) {
        std::cerr << "[VoxRenderer] Pipeline layout is null!" << std::endl;
        return;
    }
    
    size_t actualFaceCount = m_Faces.size();
    size_t instanceCount = instances.size();
    std::cout << "[VoxRenderer] Rendering " << actualFaceCount << " faces, " 
              << instanceCount << " instances" << std::endl;
    
    // 创建组合的实例数据
    std::vector<VoxelFaceInstanceData> faceInstanceData;
    faceInstanceData.reserve(actualFaceCount * instanceCount);
    
    for (size_t i = 0; i < instanceCount; i++) {
        for (size_t j = 0; j < actualFaceCount; j++) {
            VoxelFaceInstanceData data;
            data.faceData = m_Faces[j];
            data.instanceData = instances[i];
            faceInstanceData.push_back(data);
        }
    }
    
    // 更新实例缓冲区
    UpdateFaceInstanceBuffer(faceInstanceData);
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    const VkPipeline voxPipe = depthOnly ? m_RenderData.depthPipeline.GetPipeline()
                                          : m_RenderData.pipeline.GetPipeline();
    const VkPipelineLayout voxLayout = depthOnly ? m_RenderData.depthPipeline.GetLayout()
                                                 : m_RenderData.pipeline.GetLayout();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, voxPipe);
    
    // 绑定四边形顶点缓冲区和实例缓冲区
    VkBuffer vertexBuffers[] = {m_RenderData.quadVertexBuffer, m_RenderData.instanceBuffer};
    VkDeviceSize vertexOffsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, vertexOffsets);
    
    // 创建 push constants
    struct PushConstants {
        glm::mat4 projView;
        glm::mat4 prevProjView;
        glm::vec3 cameraPosition;
        float padding;
    } pushConstants;
    
    pushConstants.projView = projView;
    pushConstants.prevProjView = prevProjView;
    pushConstants.cameraPosition = cameraPosition;
    pushConstants.padding = 0.0f;
    
    // 推送 push constants
    vkCmdPushConstants(commandBuffer, voxLayout, 
                       VK_SHADER_STAGE_VERTEX_BIT, 0, 
                       sizeof(PushConstants), &pushConstants);
    
    // 绘制所有实例的所有面
    // 每个实例对应一个面和一个实例的组合
    vkCmdDraw(commandBuffer, 4, actualFaceCount * instanceCount, 0, 0);
}

void VoxRenderer::RenderMesh(VkCommandBuffer commandBuffer, int width, int height,
                              const glm::mat4& projView, const glm::mat4& prevProjView,
                              const glm::vec3& cameraPosition,
                              const std::vector<VoxelInstanceData>& instances)
{
    if (!m_Loaded || m_MeshData.indexCount == 0 || instances.empty()) {
        return;
    }
    
    UpdateMeshInstanceBuffer(instances);
    
    uint32_t frameIndex = GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderData.meshPipeline.GetPipeline());
    
    VoxelMeshUniformData pushConstants;
    pushConstants.projView = projView;
    pushConstants.prevProjView = prevProjView;
    pushConstants.cameraPosition = cameraPosition;
    pushConstants.padding = 0.0f;
    
    vkCmdPushConstants(commandBuffer, m_RenderData.meshPipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT,
                          0, sizeof(VoxelMeshUniformData), &pushConstants);
    
    VkBuffer vertexBuffers[] = {m_MeshData.vertexBuffer, m_RenderData.meshInstanceBuffers[frameIndex]};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    
    vkCmdBindIndexBuffer(commandBuffer, m_MeshData.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    
    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(m_MeshData.indexCount), static_cast<uint32_t>(instances.size()), 0, 0, 0);
}

namespace {
    constexpr glm::vec3 FACE_NORMALS[6] = {
        glm::vec3(0.0f, 0.0f, 1.0f),   // +Z
        glm::vec3(0.0f, 0.0f, -1.0f),  // -Z
        glm::vec3(-1.0f, 0.0f, 0.0f),  // -X
        glm::vec3(1.0f, 0.0f, 0.0f),   // +X
        glm::vec3(0.0f, 1.0f, 0.0f),   // +Y
        glm::vec3(0.0f, -1.0f, 0.0f)   // -Y
    };
}

void VoxRenderer::RenderMeshWithBackfaceCulling(VkCommandBuffer commandBuffer, int width, int height,
                                                 const glm::mat4& projView, const glm::mat4& prevProjView,
                                                 const glm::vec3& cameraPosition,
                                                 const std::vector<VoxelInstanceData>& instances,
                                                 bool enableBackfaceCulling,
                                                 bool depthOnly)
{
    if (!m_Loaded || m_MeshData.indexCount == 0 || instances.empty()) {
        return;
    }
    
    // 检查是否可以使用缓存
    bool useCache = enableBackfaceCulling && m_cullingCache.isCacheValid(cameraPosition, projView);
    
    // 按面方向分组：key = faceDirection, value = 可见该方向的实例索引列表
    struct FaceGroupInstances {
        size_t firstIndex;
        size_t indexCount;
        std::vector<uint32_t> instanceIndices;
    };
    
    std::array<FaceGroupInstances, 6> faceGroups;
    for (int i = 0; i < 6; i++) {
        faceGroups[i].firstIndex = m_MeshData.faceGroups[i].firstIndex;
        faceGroups[i].indexCount = m_MeshData.faceGroups[i].indexCount;
    }
    
    // 存储每个实例的可见面方向位掩码（用于缓存）
    std::vector<uint8_t> faceMasks;
    if (!useCache) {
        faceMasks.resize(instances.size(), 0);
    }
    
    // 检查缓存是否需要失效（实例数量变化）
    if (useCache && m_cullingCache.visibleFaceMasks.size() != instances.size()) {
        useCache = false;
        faceMasks.resize(instances.size(), 0);
        m_cullingCache.isValid = false;
    }
    
    if (useCache) {
        // ========== 使用缓存 ==========
        // 直接从缓存中读取每个实例的可见面方向
        for (size_t instIdx = 0; instIdx < instances.size() && instIdx < m_cullingCache.visibleFaceMasks.size(); instIdx++) {
            uint8_t visibleMask = m_cullingCache.visibleFaceMasks[instIdx];
            
            // 检查每个面方向
            for (int faceDir = 0; faceDir < 6; faceDir++) {
                if (visibleMask & (1 << faceDir)) {
                    faceGroups[faceDir].instanceIndices.push_back(static_cast<uint32_t>(instIdx));
                }
            }
        }
    } else {
        // ========== 重新计算背面剔除 ==========
        // 遍历所有实例，进行背面剔除并分组
        for (size_t instIdx = 0; instIdx < instances.size(); instIdx++) {
            const auto& instance = instances[instIdx];
            
            // 计算法线矩阵（考虑非均匀缩放）
            glm::mat3 normalMatrix = glm::mat3(instance.model);
            normalMatrix[0] = glm::normalize(normalMatrix[0]);
            normalMatrix[1] = glm::normalize(normalMatrix[1]);
            normalMatrix[2] = glm::normalize(normalMatrix[2]);
            
            // 计算世界空间的包围盒
            glm::vec3 localMin = m_MinBounds;
            glm::vec3 localMax = m_MaxBounds;
            
            glm::vec3 modelPos = glm::vec3(instance.model[3]);
            
            float scaleX = glm::length(glm::vec3(instance.model[0]));
            float scaleY = glm::length(glm::vec3(instance.model[1]));
            float scaleZ = glm::length(glm::vec3(instance.model[2]));
            
            glm::vec3 scaledLocalMin = localMin * glm::vec3(scaleX, scaleY, scaleZ);
            glm::vec3 scaledLocalMax = localMax * glm::vec3(scaleX, scaleY, scaleZ);
            
            glm::vec3 worldMin = modelPos + scaledLocalMin;
            glm::vec3 worldMax = modelPos + scaledLocalMax;
            
            // 检查相机是否在模型内部
            bool cameraInsideModel = (cameraPosition.x >= worldMin.x && cameraPosition.x <= worldMax.x &&
                                      cameraPosition.y >= worldMin.y && cameraPosition.y <= worldMax.y &&
                                      cameraPosition.z >= worldMin.z && cameraPosition.z <= worldMax.z);
            
            glm::vec3 modelCenter = (worldMin + worldMax) * 0.5f;
            float distanceToCamera = glm::length(cameraPosition - modelCenter);
            
            glm::vec3 extent = worldMax - worldMin;
            float boundingRadius = glm::length(extent) * 0.5f;
            
            // 距离剔除：如果相机距离模型足够远，才进行背面剔除
            bool shouldCull = !cameraInsideModel && (distanceToCamera >= 1.0f * boundingRadius);
            
            uint8_t visibleMask = 0;
            
            // 遍历 6 个面方向，进行背面剔除
            for (int faceDir = 0; faceDir < 6; faceDir++) {
                const auto& faceGroup = m_MeshData.faceGroups[faceDir];
                
                if (faceGroup.indexCount == 0) continue;
                
                bool isVisible = true;
                
                if (enableBackfaceCulling && shouldCull) {
                    // 计算世界空间的面法线
                    glm::vec3 faceNormal = FACE_NORMALS[faceDir];
                    faceNormal = normalMatrix * faceNormal;
                    faceNormal = glm::normalize(faceNormal);
                    
                    // 计算面的世界空间位置
                    glm::vec3 faceLocalPos = faceGroup.faceCenter;
                    glm::vec3 faceWorldPos = glm::vec3(instance.model * glm::vec4(faceLocalPos, 1.0f));
                    
                    // 计算到相机的方向
                    glm::vec3 toCamera = cameraPosition - faceWorldPos;
                    float toCameraLength = glm::length(toCamera);
                    if (toCameraLength > 0.0f) {
                        toCamera = toCamera / toCameraLength;
                    }
                    
                    // 点积判断是否为背面
                    float dotProduct = glm::dot(faceNormal, toCamera);
                    isVisible = (dotProduct > -0.3f);  // 稍微宽松的阈值，避免边缘闪烁
                }
                
                if (isVisible) {
                    faceGroups[faceDir].instanceIndices.push_back(static_cast<uint32_t>(instIdx));
                    visibleMask |= (1 << faceDir);
                }
            }
            
            faceMasks[instIdx] = visibleMask;
        }
    }
    
    // 检查是否有任何可见面
    bool hasVisibleFaces = false;
    for (int i = 0; i < 6; i++) {
        if (!faceGroups[i].instanceIndices.empty()) {
            hasVisibleFaces = true;
            break;
        }
    }
    
    if (!hasVisibleFaces) {
        return;
    }
    
    // 更新实例缓冲区（使用原始 instances 向量）
    UpdateMeshInstanceBuffer(instances);
    
    uint32_t frameIndex = GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        depthOnly ? m_RenderData.meshDepthPipeline.GetPipeline() : m_RenderData.meshPipeline.GetPipeline());
    
    VoxelMeshUniformData pushConstants;
    pushConstants.projView = projView;
    pushConstants.prevProjView = prevProjView;
    pushConstants.cameraPosition = cameraPosition;
    pushConstants.padding = 0.0f;
    
    vkCmdPushConstants(commandBuffer,
        depthOnly ? m_RenderData.meshDepthPipeline.GetLayout() : m_RenderData.meshPipeline.GetLayout(),
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VoxelMeshUniformData), &pushConstants);
    
    VkBuffer vertexBuffers[] = {m_MeshData.vertexBuffer, m_RenderData.meshInstanceBuffers[frameIndex]};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    
    vkCmdBindIndexBuffer(commandBuffer, m_MeshData.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    
    // 为每个面方向批量绘制（所有相同方向的可见面用一个命令）
    for (int faceDir = 0; faceDir < 6; faceDir++) {
        auto& faceGroup = faceGroups[faceDir];
        
        if (faceGroup.instanceIndices.empty()) {
            continue;
        }
        
        // 使用该方向的第一个实例索引作为 firstInstance
        uint32_t firstInstance = faceGroup.instanceIndices[0];
        uint32_t instanceCount = static_cast<uint32_t>(faceGroup.instanceIndices.size());
        
        vkCmdDrawIndexed(commandBuffer, 
                        static_cast<uint32_t>(faceGroup.indexCount),
                        instanceCount,
                        static_cast<uint32_t>(faceGroup.firstIndex), 
                        0, 
                        firstInstance);
    }
    
    // 更新缓存（只有重新计算时才更新）
    if (!useCache) {
        updateCullingCache(cameraPosition, projView, instances.size(), faceMasks);
    }
}

void VoxRenderer::updateCullingCache(const glm::vec3& cameraPos, const glm::mat4& projMatrix,
                                     size_t instanceCount, const std::vector<uint8_t>& faceMasks)
{
    m_cullingCache.cachedCameraPosition = cameraPos;
    m_cullingCache.cachedProjMatrix = projMatrix;
    m_cullingCache.visibleFaceMasks = faceMasks;
    m_cullingCache.isValid = true;
}

void VoxRenderer::RenderMeshWireframe(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView, const glm::mat4& prevProjView,
                                       const glm::vec3& cameraPosition,
                                       const std::vector<VoxelInstanceData>& instances)
{
    if (!m_Loaded || m_MeshData.indexCount == 0 || instances.empty()) {
        return;
    }
    
    UpdateMeshInstanceBuffer(instances);
    
    uint32_t frameIndex = GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderData.meshWireframePipeline.GetPipeline());
    
    VoxelMeshUniformData pushConstants;
    pushConstants.projView = projView;
    pushConstants.prevProjView = prevProjView;
    pushConstants.cameraPosition = cameraPosition;
    pushConstants.padding = 0.0f;
    
    vkCmdPushConstants(commandBuffer, m_RenderData.meshWireframePipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT,
                          0, sizeof(VoxelMeshUniformData), &pushConstants);
    
    VkBuffer vertexBuffers[] = {m_MeshData.vertexBuffer, m_RenderData.meshInstanceBuffers[frameIndex]};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    
    vkCmdBindIndexBuffer(commandBuffer, m_MeshData.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    
    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(m_MeshData.indexCount), static_cast<uint32_t>(instances.size()), 0, 0, 0);
}

void VoxRenderer::RenderWireframe(VkCommandBuffer commandBuffer, int width, int height,
                                   const glm::mat4& projView, const glm::mat4& prevProjView,
                                   const glm::vec3& cameraPosition,
                                   const std::vector<VoxelInstanceData>& instances)
{
    if (!m_Loaded || m_Faces.empty() || instances.empty()) {
        return;
    }
    
    size_t actualFaceCount = m_Faces.size();
    size_t instanceCount = instances.size();
    
    // 创建组合的实例数据
    std::vector<VoxelFaceInstanceData> faceInstanceData;
    faceInstanceData.reserve(actualFaceCount * instanceCount);
    
    for (size_t i = 0; i < instanceCount; i++) {
        for (size_t j = 0; j < actualFaceCount; j++) {
            VoxelFaceInstanceData data;
            data.faceData = m_Faces[j];
            data.instanceData = instances[i];
            faceInstanceData.push_back(data);
        }
    }
    
    // 更新实例缓冲区
    UpdateFaceInstanceBuffer(faceInstanceData);
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderData.wireframePipeline.GetPipeline());
    
    // 绑定四边形顶点缓冲区和实例缓冲区
    VkBuffer vertexBuffers[] = {m_RenderData.quadVertexBuffer, m_RenderData.instanceBuffer};
    VkDeviceSize vertexOffsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, vertexOffsets);
    
    // 创建 push constants
    struct PushConstants {
        glm::mat4 projView;
        glm::mat4 prevProjView;
        glm::vec3 cameraPosition;
        float padding;
    } pushConstants;
    
    pushConstants.projView = projView;
    pushConstants.prevProjView = prevProjView;
    pushConstants.cameraPosition = cameraPosition;
    pushConstants.padding = 0.0f;
    
    // 推送 push constants
    vkCmdPushConstants(commandBuffer, m_RenderData.wireframePipeline.GetLayout(), 
                       VK_SHADER_STAGE_VERTEX_BIT, 0, 
                       sizeof(PushConstants), &pushConstants);
    
    // 绘制所有实例的所有面
    vkCmdDraw(commandBuffer, 4, actualFaceCount * instanceCount, 0, 0);
}

// ============================================================================
// BVH 相关方法实现
// ============================================================================

void VoxRenderer::BuildBVHFromMesh(const std::vector<VoxelMeshVertex>& meshVertices, 
                                    const std::vector<uint32_t>& meshIndices)
{
    if (meshVertices.empty() || meshIndices.empty()) {
        std::cerr << "[VoxRenderer::BuildBVHFromMesh] Cannot build BVH: empty mesh!" << std::endl;
        return;
    }
    
    std::cout << "[VoxRenderer::BuildBVHFromMesh] Starting BVH construction..." << std::endl;
    
    // 尝试从缓存加载 BVH
    if (LoadBVHCache()) {
        std::cout << "[VoxRenderer::BuildBVHFromMesh] BVH loaded from cache successfully!" << std::endl;
        return;
    }
    
    std::cout << "[VoxRenderer::BuildBVHFromMesh] Cache miss, building BVH from scratch..." << std::endl;
    
    // 使用压缩的顶点格式构建 BVH
    // 由于 ModelBVH 需要 BVHVertex 格式，我们暂时还是用完整格式
    // TODO: 修改 ModelBVH 支持自定义顶点格式
    std::vector<BVHVertex> bvhVertices;
    std::vector<uint32_t> bvhIndices;
    
    bvhVertices.reserve(meshVertices.size());
    bvhIndices = meshIndices;
    
    // 转换顶点：从压缩的体素坐标到世界坐标
    for (const auto& voxelVertex : meshVertices) {
        BVHVertex vertex;
        
        // 从压缩的体素坐标还原到世界坐标
        vertex.position.x = voxelVertex.x * m_VoxelSize + m_MinBounds.x;
        vertex.position.y = voxelVertex.y * m_VoxelSize + m_MinBounds.y;
        vertex.position.z = voxelVertex.z * m_VoxelSize + m_MinBounds.z;
        
        vertex.normal = glm::vec3(0.0f);
        vertex.texCoords = glm::vec2(0.0f);
        
        bvhVertices.push_back(vertex);
    }
    
    // 计算法线
    for (size_t i = 0; i < bvhIndices.size(); i += 3) {
        BVHVertex& v0 = bvhVertices[bvhIndices[i]];
        BVHVertex& v1 = bvhVertices[bvhIndices[i + 1]];
        BVHVertex& v2 = bvhVertices[bvhIndices[i + 2]];
        
        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));
        
        v0.normal = normal;
        v1.normal = normal;
        v2.normal = normal;
    }
    
    // 使用 ModelBVH 的算法构建 BVH
    m_BVH.build(bvhVertices, bvhIndices);
    
    // 打印 BVH 统计信息
    const auto& nodes = m_BVH.getNodes();
    const auto& triangles = m_BVH.getTriangles();
    const auto& vertices = m_BVH.getVertices();
    
    int leafNodes = 0;
    int totalTrianglesInLeaves = 0;
    int maxDepth = 0;
    
    for (const auto& node : nodes) {
        if (node.isLeaf) {
            leafNodes++;
            totalTrianglesInLeaves += node.count;
            maxDepth = std::max(maxDepth, node.depth);
        }
    }
    
    std::cout << "=== Voxel Mesh BVH Statistics ===" << std::endl;
    std::cout << "  Input vertices: " << meshVertices.size() << std::endl;
    std::cout << "  Input triangles: " << meshIndices.size() / 3 << std::endl;
    std::cout << "  BVH vertices: " << vertices.size() << std::endl;
    std::cout << "  BVH triangles: " << triangles.size() << std::endl;
    std::cout << "  BVH nodes: " << nodes.size() << std::endl;
    std::cout << "  Leaf nodes: " << leafNodes << std::endl;
    std::cout << "  Max depth: " << maxDepth << std::endl;
    std::cout << "  Avg triangles per leaf: " << (leafNodes > 0 ? (float)totalTrianglesInLeaves / leafNodes : 0) << std::endl;
    
    // 计算内存占用
    size_t nodeMemory = nodes.size() * sizeof(ModelBVHNode);
    size_t triangleMemory = triangles.size() * sizeof(Triangle);
    size_t vertexMemory = vertices.size() * sizeof(BVHVertex);
    size_t totalMemory = nodeMemory + triangleMemory + vertexMemory;
    
    std::cout << "  Memory usage:" << std::endl;
    std::cout << "    - BVH nodes: " << (nodeMemory / 1024) << " KB" << std::endl;
    std::cout << "    - Triangles: " << (triangleMemory / 1024) << " KB" << std::endl;
    std::cout << "    - Vertices: " << (vertexMemory / 1024) << " KB" << std::endl;
    std::cout << "    - Total: " << (totalMemory / 1024) << " KB (" << totalMemory << " bytes)" << std::endl;
    std::cout << "=================================" << std::endl;
    
    // 保存到缓存
    SaveBVHCache();
    
    std::cout << "[VoxRenderer::BuildBVHFromMesh] BVH construction completed!" << std::endl;
    
    // 生成 Texture3D 缓存
    GenerateTexture3DCache();
}

std::string VoxRenderer::GetBVHCachePath() const
{
    if (m_FilePath.empty()) {
        return "";
    }
    
    // 获取 vox 文件所在目录
    std::filesystem::path path(m_FilePath);
    std::filesystem::path parentDir = path.parent_path();
    std::string stem = path.stem().string();
    
    // 在 vox 文件同级目录下的 bvh 子目录
    std::filesystem::path cacheDir = parentDir / "bvh";
    
#ifdef ANDROID_BUILD
    // Android 只读取 assets 中的缓存，不创建目录
    std::string cacheFile = stem + "_bvh.bin";
    return (cacheDir / cacheFile).string();
#else
    // 确保目录存在
    std::filesystem::create_directories(cacheDir);
    
    // 缓存文件名：{modelname}_bvh.bin
    std::string cacheFile = stem + "_bvh.bin";
    
    return (cacheDir / cacheFile).string();
#endif
}

bool VoxRenderer::LoadBVHCache()
{
    std::string cachePath = GetBVHCachePath();
    if (cachePath.empty()) {
        return false;
    }
    
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "[VoxRenderer::LoadBVHCache] Cache file not found: " << cachePath << std::endl;
        return false;
    }
    
    // 读取文件头
    uint32_t nodeCount, triangleCount, rootIdx, version;
    
    file.read(reinterpret_cast<char*>(&nodeCount), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&triangleCount), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&rootIdx), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    
    if (file.fail()) {
        std::cerr << "[VoxRenderer::LoadBVHCache] Failed to read cache header!" << std::endl;
        file.close();
        return false;
    }
    
    // 检查版本号（只支持 v3 超压缩格式）
    if (version != 3) {
        std::cout << "[VoxRenderer::LoadBVHCache] Unsupported cache version: " << version << " (only v3 supported)" << std::endl;
        file.close();
        return false;
    }
    
    // 验证文件大小
    // v3: 16 bytes/节点（超压缩）
    struct UltraCompactBVHNode {
        uint8_t boundsMin[3];
        uint8_t boundsMax[3];
        uint32_t nodeData;
        uint32_t startCount;
        uint8_t depth;
        uint8_t count;
        uint8_t padding[2];
    };
    
    uint64_t expectedFileSize = sizeof(uint32_t) * 4 +
        nodeCount * sizeof(UltraCompactBVHNode) +
        triangleCount * sizeof(Triangle);
    
    file.seekg(0, std::ios::end);
    uint64_t actualFileSize = file.tellg();
    file.seekg(sizeof(uint32_t) * 4, std::ios::beg);
    
    if (actualFileSize != expectedFileSize) {
        std::cerr << "[VoxRenderer::LoadBVHCache] Cache file size mismatch!" << std::endl;
        std::cerr << "  Expected: " << expectedFileSize << " bytes" << std::endl;
        std::cerr << "  Actual: " << actualFileSize << " bytes" << std::endl;
        file.close();
        return false;
    }
    
    // 读取 v3 超压缩 BVH 节点
    std::cout << "[VoxRenderer::LoadBVHCache] Decompressing BVH nodes (v3 ultra-compact)..." << std::endl;
    
    std::vector<UltraCompactBVHNode> ultraCompactNodes(nodeCount);
    file.read(reinterpret_cast<char*>(ultraCompactNodes.data()), nodeCount * sizeof(UltraCompactBVHNode));
    
    // 读取三角形
    std::vector<Triangle> loadedTriangles(triangleCount);
    file.read(reinterpret_cast<char*>(loadedTriangles.data()), triangleCount * sizeof(Triangle));
    
    if (file.fail()) {
        std::cerr << "[VoxRenderer::LoadBVHCache] Failed to read cache data!" << std::endl;
        file.close();
        return false;
    }
    
    file.close();
    
    // 从超压缩格式还原 BVH 节点
    std::vector<ModelBVHNode> loadedNodes(nodeCount);
    
    for (size_t i = 0; i < nodeCount; i++) {
        const auto& ultra = ultraCompactNodes[i];
        
        glm::vec3 minVox(ultra.boundsMin[0], ultra.boundsMin[1], ultra.boundsMin[2]);
        glm::vec3 maxVox(ultra.boundsMax[0], ultra.boundsMax[1], ultra.boundsMax[2]);
        
        loadedNodes[i].boundsMin = minVox * m_VoxelSize + m_MinBounds;
        loadedNodes[i].boundsMax = maxVox * m_VoxelSize + m_MinBounds;
        
        loadedNodes[i].isLeaf = (ultra.nodeData >> 31) & 1;
        loadedNodes[i].depth = ultra.depth;
        loadedNodes[i].startIndex = (ultra.startCount >> 16) & 0xFFFF;
        loadedNodes[i].count = ultra.startCount & 0xFFFF;
        
        // 从相对偏移还原子节点索引
        if (!loadedNodes[i].isLeaf) {
            uint32_t childOffset = ultra.nodeData & 0x7FFFFFFF;
            loadedNodes[i].left = static_cast<int>(i) + static_cast<int>(childOffset);
            loadedNodes[i].right = loadedNodes[i].left + 1;
        } else {
            loadedNodes[i].left = -1;
            loadedNodes[i].right = -1;
        }
    }
    
    // 重建顶点（从 faces）
    std::cout << "[VoxRenderer::LoadBVHCache] Rebuilding vertices from faces..." << std::endl;
    
    std::vector<BVHVertex> bvhVertices;
    std::vector<uint32_t> bvhIndices;
    
    for (const auto& face : m_Faces) {
        uint32_t faceDir = face.data & 0xFF;
        glm::vec3 corners[4];
        glm::vec3 basePos = face.position;
        glm::vec2 size = face.size;
        
        switch (faceDir) {
            case 0:
                corners[0] = basePos + glm::vec3(-size.x * 0.5f, -size.y * 0.5f, 0);
                corners[1] = basePos + glm::vec3(size.x * 0.5f, -size.y * 0.5f, 0);
                corners[2] = basePos + glm::vec3(size.x * 0.5f, size.y * 0.5f, 0);
                corners[3] = basePos + glm::vec3(-size.x * 0.5f, size.y * 0.5f, 0);
                break;
            case 1:
                corners[0] = basePos + glm::vec3(-size.x * 0.5f, -size.y * 0.5f, 0);
                corners[1] = basePos + glm::vec3(-size.x * 0.5f, size.y * 0.5f, 0);
                corners[2] = basePos + glm::vec3(size.x * 0.5f, size.y * 0.5f, 0);
                corners[3] = basePos + glm::vec3(size.x * 0.5f, -size.y * 0.5f, 0);
                break;
            case 2:
                corners[0] = basePos + glm::vec3(0, -size.y * 0.5f, size.x * 0.5f);
                corners[1] = basePos + glm::vec3(0, size.y * 0.5f, size.x * 0.5f);
                corners[2] = basePos + glm::vec3(0, size.y * 0.5f, -size.x * 0.5f);
                corners[3] = basePos + glm::vec3(0, -size.y * 0.5f, -size.x * 0.5f);
                break;
            case 3:
                corners[0] = basePos + glm::vec3(0, -size.y * 0.5f, -size.x * 0.5f);
                corners[1] = basePos + glm::vec3(0, size.y * 0.5f, -size.x * 0.5f);
                corners[2] = basePos + glm::vec3(0, size.y * 0.5f, size.x * 0.5f);
                corners[3] = basePos + glm::vec3(0, -size.y * 0.5f, size.x * 0.5f);
                break;
            case 4:
                corners[0] = basePos + glm::vec3(-size.x * 0.5f, 0, -size.y * 0.5f);
                corners[1] = basePos + glm::vec3(-size.x * 0.5f, 0, size.y * 0.5f);
                corners[2] = basePos + glm::vec3(size.x * 0.5f, 0, size.y * 0.5f);
                corners[3] = basePos + glm::vec3(size.x * 0.5f, 0, -size.y * 0.5f);
                break;
            case 5:
                corners[0] = basePos + glm::vec3(-size.x * 0.5f, 0, size.y * 0.5f);
                corners[1] = basePos + glm::vec3(-size.x * 0.5f, 0, -size.y * 0.5f);
                corners[2] = basePos + glm::vec3(size.x * 0.5f, 0, -size.y * 0.5f);
                corners[3] = basePos + glm::vec3(size.x * 0.5f, 0, size.y * 0.5f);
                break;
        }
        
        uint32_t baseIndex = static_cast<uint32_t>(bvhVertices.size());
        
        for (int i = 0; i < 4; i++) {
            BVHVertex vertex;
            vertex.position = corners[i];
            vertex.normal = glm::vec3(0.0f);
            vertex.texCoords = glm::vec2(0.0f);
            bvhVertices.push_back(vertex);
        }
        
        bvhIndices.push_back(baseIndex + 0);
        bvhIndices.push_back(baseIndex + 2);
        bvhIndices.push_back(baseIndex + 1);
        
        bvhIndices.push_back(baseIndex + 0);
        bvhIndices.push_back(baseIndex + 3);
        bvhIndices.push_back(baseIndex + 2);
    }
    
    // 计算法线
    for (size_t i = 0; i < bvhIndices.size(); i += 3) {
        BVHVertex& v0 = bvhVertices[bvhIndices[i]];
        BVHVertex& v1 = bvhVertices[bvhIndices[i + 1]];
        BVHVertex& v2 = bvhVertices[bvhIndices[i + 2]];
        
        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));
        
        v0.normal = normal;
        v1.normal = normal;
        v2.normal = normal;
    }
    
    // 使用加载的节点和三角形，以及重建的顶点初始化 m_BVH
    ModelBVH newBVH;
    newBVH.build(bvhVertices, bvhIndices);
    
    // 替换节点和三角形
    auto& nodes = const_cast<std::vector<ModelBVHNode>&>(newBVH.getNodes());
    auto& triangles = const_cast<std::vector<Triangle>&>(newBVH.getTriangles());
    
    nodes = std::move(loadedNodes);
    triangles = std::move(loadedTriangles);
    
    m_BVH = std::move(newBVH);
    
    std::cout << "[VoxRenderer::LoadBVHCache] BVH cache loaded successfully!" << std::endl;
    std::cout << "  Nodes: " << nodeCount << " x 16 bytes (v3 ultra-compact)" << std::endl;
    std::cout << "  Triangles: " << triangleCount << std::endl;
    std::cout << "  Vertices rebuilt: " << bvhVertices.size() << std::endl;
    
    return true;
}

bool VoxRenderer::LoadTexture3DCache()
{
    std::string cachePath = GetTexture3DCachePath();
    if (cachePath.empty()) {
        return false;
    }
    
    std::vector<VoxFormat::Color> palette;
    std::vector<uint8_t> voxelData;
    uint32_t sizeX, sizeY, sizeZ;
    
    if (!m_Texture3DCacheGen.LoadCache(cachePath, voxelData, palette, sizeX, sizeY, sizeZ)) {
        return false;
    }
    
    m_Texture3DData = std::move(voxelData);
    m_Texture3DPalette = std::move(palette);
    m_Texture3DSizeX = sizeX;
    m_Texture3DSizeY = sizeY;
    m_Texture3DSizeZ = sizeZ;
    
    std::cout << "[VoxRenderer::LoadTexture3DCache] Texture3D cache loaded: " 
              << sizeX << "x" << sizeY << "x" << sizeZ << std::endl;
    
    return true;
}

std::string VoxRenderer::GetTexture3DCachePath() const
{
    if (m_FilePath.empty()) {
        return "";
    }
    
    return VoxelCache::Texture3DCacheGenerator::GetCachePath(m_FilePath);
}

bool VoxRenderer::SaveBVHCache()
{
#ifdef ANDROID_BUILD
    // Android 不保存 BVH 缓存
    return false;
#else
    if (!m_BVH.IsValid()) {
        std::cerr << "[VoxRenderer::SaveBVHCache] Cannot save invalid BVH!" << std::endl;
        return false;
    }
    
    std::string cachePath = GetBVHCachePath();
    if (cachePath.empty()) {
        std::cerr << "[VoxRenderer::SaveBVHCache] Invalid cache path!" << std::endl;
        return false;
    }
    
    std::cout << "[VoxRenderer::SaveBVHCache] Saving BVH cache to: " << cachePath << std::endl;
    
    std::ofstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[VoxRenderer::SaveBVHCache] Failed to create cache file!" << std::endl;
        return false;
    }
    
    const auto& nodes = m_BVH.getNodes();
    const auto& triangles = m_BVH.getTriangles();
    
    // 使用超压缩格式保存 BVH
    // 由于体素 mesh 最大 255x，我们可以使用 uint8 存储体素坐标
    
    // 写入文件头（v3 超压缩格式）
    uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
    uint32_t triangleCount = static_cast<uint32_t>(triangles.size());
    uint32_t rootIdx = static_cast<uint32_t>(m_BVH.getRootIndex());
    uint32_t version = 3; // v3: 16 bytes/节点（超压缩格式）
    
    file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&triangleCount), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&rootIdx), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));
    
    // 超压缩 BVH 节点 - 仅 16 bytes!
    // 原始节点：64 bytes -> v3 超压缩节点：16 bytes (使用 uint8 存储体素坐标)
    struct UltraCompactBVHNode {
        uint8_t boundsMin[3];     // 3 bytes (体素坐标 0-255)
        uint8_t boundsMax[3];     // 3 bytes
        uint32_t nodeData;        // 4 bytes (编码 isLeaf, childOffset)
        uint32_t startCount;      // 4 bytes (编码 startIndex, count)
        uint8_t depth;            // 1 byte
        uint8_t count;            // 1 byte (叶子节点三角形数)
        uint8_t padding[2];       // 2 bytes
    };
    
    std::vector<UltraCompactBVHNode> ultraCompactNodes(nodeCount);
    
    for (size_t i = 0; i < nodeCount; i++) {
        const auto& node = nodes[i];
        
        // 将世界坐标转换为体素坐标 (0-255)
        glm::vec3 minVox = (node.boundsMin - m_MinBounds) / m_VoxelSize;
        glm::vec3 maxVox = (node.boundsMax - m_MinBounds) / m_VoxelSize;
        
        ultraCompactNodes[i].boundsMin[0] = static_cast<uint8_t>(glm::clamp(minVox.x, 0.0f, 255.0f));
        ultraCompactNodes[i].boundsMin[1] = static_cast<uint8_t>(glm::clamp(minVox.y, 0.0f, 255.0f));
        ultraCompactNodes[i].boundsMin[2] = static_cast<uint8_t>(glm::clamp(minVox.z, 0.0f, 255.0f));
        
        ultraCompactNodes[i].boundsMax[0] = static_cast<uint8_t>(glm::clamp(maxVox.x, 0.0f, 255.0f));
        ultraCompactNodes[i].boundsMax[1] = static_cast<uint8_t>(glm::clamp(maxVox.y, 0.0f, 255.0f));
        ultraCompactNodes[i].boundsMax[2] = static_cast<uint8_t>(glm::clamp(maxVox.z, 0.0f, 255.0f));
        
        // 编码节点数据
        // bit 31: isLeaf
        // bit 30-0: childOffset (相对偏移，非叶子节点指向 left 子节点索引 - 当前索引)
        uint32_t childOffset = 0;
        if (!node.isLeaf) {
            childOffset = static_cast<uint32_t>(node.left) - static_cast<uint32_t>(i);
        }
        ultraCompactNodes[i].nodeData = (static_cast<uint32_t>(node.isLeaf) << 31) |
                                        (childOffset & 0x7FFFFFFF);
        
        // 编码 startIndex 和 count
        ultraCompactNodes[i].startCount = (static_cast<uint32_t>(node.startIndex & 0xFFFF) << 16) |
                                          (static_cast<uint32_t>(node.count & 0xFFFF));
        
        ultraCompactNodes[i].depth = static_cast<uint8_t>(node.depth);
        ultraCompactNodes[i].count = static_cast<uint8_t>(node.count);
        ultraCompactNodes[i].padding[0] = 0;
        ultraCompactNodes[i].padding[1] = 0;
    }
    
    // 写入超压缩 BVH 节点
    file.write(reinterpret_cast<const char*>(ultraCompactNodes.data()), nodeCount * sizeof(UltraCompactBVHNode));
    
    // 写入三角形索引（使用 32-bit）
    file.write(reinterpret_cast<const char*>(triangles.data()), triangleCount * sizeof(Triangle));
    
    if (file.fail()) {
        std::cerr << "[VoxRenderer::SaveBVHCache] Failed to write cache data!" << std::endl;
        file.close();
        return false;
    }
    
    file.close();
    
    // 计算文件大小
    uint64_t fileSize = sizeof(uint32_t) * 4 +
        nodeCount * sizeof(UltraCompactBVHNode) +
        triangleCount * sizeof(Triangle);
    
    std::cout << "[VoxRenderer::SaveBVHCache] BVH cache saved successfully!" << std::endl;
    std::cout << "  File size: " << (fileSize / 1024) << " KB (" << fileSize << " bytes)" << std::endl;
    std::cout << "  Nodes: " << nodeCount << " x " << sizeof(UltraCompactBVHNode) << " bytes" << std::endl;
    std::cout << "  Triangles: " << triangleCount << " x " << sizeof(Triangle) << " bytes" << std::endl;
    std::cout << "  Compression: " << (100.0f * fileSize / (nodeCount * sizeof(ModelBVHNode) + triangleCount * sizeof(Triangle))) << "%" << std::endl;
    
    return true;
#endif
}
