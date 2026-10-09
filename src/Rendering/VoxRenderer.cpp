#include "Core/CpuStageTrace.h"
#include "Rendering/VoxRenderer.h"
#include "Rendering/VoxSurfaceAttributes.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "RayTracing/VoxSurfaceBake.h"
#include "Rendering/VoxelDDARegistry.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "EngineConfig.h"
#include <glm/packing.hpp>
#include <iostream>
#include <limits>
#include <algorithm>
#include <filesystem>
#include <functional>
#include "Rendering/RenderStats.h"
#include "Core/LogStream.h"
#include "Core/ProjectManager.h"

namespace {
bool StaticContactBackfillRequested() {
    // Enabled by default; freeze the setting for this process, including reloads.
    static const bool requested=[](){const char* value=std::getenv("MIKAN_VOX_STATIC_CONTACT_BACKFILL");return !value||std::string(value)!="0";}();
    return requested;
}
}
#include "VoxCompiledSurface.inl"

static uint32_t PackVoxFaceMaterial(const VoxFormat::VoxData&,int);
namespace {
    // One immutable uint16 pattern for every vox model, viewport and shadow cascade.
    VulkanBuffer sharedQuadIndices;
    size_t sharedQuadIndexUsers=0;
    void AcquireQuadIndices() {
        if(!sharedQuadIndices.GetBuffer()) {
            std::vector<uint16_t> indices(VOX_INDEX_SEGMENT_INDICES);
            constexpr uint16_t corners[6]={0,2,1,0,3,2};
            for(size_t q=0;q<VOX_INDEX_SEGMENT_VERTICES/4;++q)
                for(size_t i=0;i<6;++i)indices[q*6+i]=uint16_t(q*4+corners[i]);
            if(!sharedQuadIndices.Create(indices.size()*sizeof(uint16_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                throw std::runtime_error("Failed to allocate shared vox quad indices");
            sharedQuadIndices.Write(indices.data(),indices.size()*sizeof(uint16_t));
        }
        ++sharedQuadIndexUsers;
    }
}
VkBuffer VoxRenderer::GetSharedQuadIndexBuffer() { return sharedQuadIndices.GetBuffer(); }

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
    LOGSTREAM(Info) << "[VoxRenderer] Destructor called for: " << m_FilePath << std::endl;
}

void VoxRenderer::Cleanup()
{
    ClearComposite();
    // 清理体素纹理管理器
    m_Texture3DManager.Cleanup();

    mikan::render::VoxelDDARegistry::Get().Unregister(m_FilePath);

    m_DirectQuadPipeline.Cleanup(); m_DirectQuadWirePipeline.Cleanup();
    m_DirectQuads.Cleanup();
    if(m_SharedQuadIndicesAcquired) {
        m_SharedQuadIndicesAcquired=false;
        if(--sharedQuadIndexUsers==0)sharedQuadIndices.Cleanup();
    }
    if(m_DirectQuadPool)vkDestroyDescriptorPool(g_Device,m_DirectQuadPool,g_Allocator);
    if(m_DirectQuadLayout)vkDestroyDescriptorSetLayout(g_Device,m_DirectQuadLayout,g_Allocator);
    m_DirectQuadPool=VK_NULL_HANDLE;m_DirectQuadLayout=VK_NULL_HANDLE;m_DirectQuadDescriptor=VK_NULL_HANDLE;
    m_QuadPipeline.Cleanup();
    if(m_QuadSetLayout && g_Device)vkDestroyDescriptorSetLayout(g_Device,m_QuadSetLayout,g_Allocator);
    m_QuadSetLayout=VK_NULL_HANDLE;m_Quads.clear();m_QuadMaterials.clear();m_SurfaceAttributes.clear();m_QuadEncodingValid=false;
    m_CsmPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;
    for (auto& buffer : m_CsmInstanceBuffers) buffer.Cleanup();
    m_CsmInstanceCapacity = 0;
    m_CsmInstanceEpochs.fill(UINT64_MAX);

    // 清空背面剔除缓存
    m_cullingCache.clear();

    // 清理全局网格缓存（只在最后一个 VoxRenderer 销毁时清理）
    {
        std::lock_guard<std::mutex> lock(s_meshCacheMutex);
        for (auto& [hash, entry] : s_meshCache) {
            entry.isValid = false;
        }
        s_meshCache.clear();
    }

    for (auto& uploads : m_RenderData.meshInstanceUploads) {
        for (auto& upload : uploads) {
            DestroyMeshInstanceUpload(upload);
        }
        uploads.clear();
    }
    m_RenderData.meshInstanceUploadFrameSerial = UINT64_MAX;
    for (uint32_t& cursor : m_RenderData.meshInstanceUploadCursor) cursor = 0;

    BaseRenderer::Cleanup();

    m_Faces.clear();
    m_FaceMaterials.clear();
    m_VoxelGrid.clear();
    m_Loaded = false;
}

void VoxRenderer::Init(VkRenderPass renderPass)
{
    m_CompositeRenderPass=renderPass;
    LOGSTREAM(Info) << "[VoxRenderer] Init started" << std::endl;

    if(!m_SharedQuadIndicesAcquired) {
        AcquireQuadIndices();m_SharedQuadIndicesAcquired=true;
    }

    CreateMeshInstanceBuffer(100);
    CreatePipeline(renderPass);

    LOGSTREAM(Info) << "[VoxRenderer] Init completed successfully" << std::endl;
}

void VoxRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
}

void VoxRenderer::SetUniformSurfaceMaterials(bool required) {
    if(m_UniformSurfaceMaterials==required)return;
    if(m_Loaded&&std::filesystem::path(m_FilePath).extension()==".voxmesh"){if(required)throw std::runtime_error("Compiled VOX requires --uniform-materials for entity emission");return;}
    m_UniformSurfaceMaterials=required;
    if(m_Loaded&&!m_FilePath.empty()){
        vkDeviceWaitIdle(g_Device);
        const std::string path=m_FilePath;
        if(!LoadVoxFile(path,m_VoxelSize))throw std::runtime_error("Failed to rebuild VOX emission surfaces");
    }
}
bool VoxRenderer::LoadVoxFile(const std::string& path, float voxelSize)
{
    Core::CpuStageTrace cpuStageTrace("vox.load_file");
    m_FilePath = path;
    if(std::filesystem::path(path).extension()==".voxmesh")return LoadCompiledSurface(path,voxelSize);

    VoxFormat::VoxData voxData;
    const bool parsed=[&]{Core::CpuStageTrace parseTrace("vox.parse_source");return VoxFormat::LoadVoxFile(path,voxData);}();
    if (!parsed) {
        LOGSTREAM(Error) << "[VoxRenderer] Failed to load VOX file: " << path << std::endl;
        return false;
    }

    const bool loaded=LoadFromVoxData(voxData,voxelSize);
#ifndef __ANDROID__
    // Only this imported assembly is cooked; no project-wide asset conversion.
    if(loaded&&IsComposite()&&!StaticContactBackfillRequested()){
        try {
            auto output=std::filesystem::u8path(ProjectManager::GetInstance().ResolveAssetPath(path));output.replace_extension(".voxmesh");
            if(!std::filesystem::exists(output)){
                const auto words=EncodeCompiledSurface();
                std::ofstream out(output,std::ios::binary|std::ios::trunc);
                for(auto word:words)for(int b=0;b<4;++b)out.put(char(word>>(b*8)));
                out.flush();if(!out)throw std::runtime_error("Cannot write assembly cache");
                LOGI("[VOX assembly] cooked sidecar: %s",output.string().c_str());
            }
        }catch(const std::exception& e){LOGW("[VOX assembly] cache write skipped: %s",e.what());}
    }
#endif
    return loaded;
}

bool VoxRenderer::LoadFromVoxData(const VoxFormat::VoxData& voxData, float voxelSize)
{
    Core::CpuStageTrace cpuStageTrace("vox.preprocess");
    ClearComposite();m_Loaded=false;
    if(!voxData.instances.empty())return LoadComposite(voxData,voxelSize);
    m_VoxelSize = voxelSize;
    m_Faces.clear();
    m_FaceMaterials.clear();
    m_VoxelGrid.clear();

    BuildVoxelFaces(voxData, voxelSize);

    if (m_Faces.empty()) {
        LOGSTREAM(Warn) << "[VoxRenderer] No faces generated" << std::endl;
        return false;
    }


    BuildTriangleMesh();

    // Build from the same merged VOX data as the canonical greedy mesh. Disk
    // texture caches may contain reordered palettes or unmerged source models.
    if(mikan::render::VoxelDDARegistry::BootEnabled()&&!voxData.models.empty()&&!m_FilePath.empty()){
        const auto& volume=voxData.models[0];
        const uint32_t sx=volume.sizeX,sy=volume.sizeY,sz=volume.sizeZ;
        // Power-of-two storage makes every mip cell an exact aligned block.
        auto padded=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};
        const uint32_t tx=padded(sx),ty=padded(sy),tz=padded(sz);
        std::vector<uint8_t> cells(size_t(tx)*ty*tz,0);
        glm::ivec3 minimum(INT_MAX),maximum(INT_MIN);
        for(const auto& v:volume.voxels){
            minimum=glm::min(minimum,glm::ivec3(v.x,v.y,v.z));maximum=glm::max(maximum,glm::ivec3(v.x,v.y,v.z));
            if(v.x<sx&&v.y<sy&&v.z<sz)cells[(size_t(v.z)*ty+v.y)*tx+v.x]=v.colorIndex;
        }
        if(!m_Texture3DManagerInitialized){
            VoxelTexture3DManagerConfig config;config.maxTextures=64;config.format=VK_FORMAT_R8_UINT;config.enableMipmaps=true;config.filter=VK_FILTER_NEAREST;
            m_Texture3DManagerInitialized=m_Texture3DManager.Initialize(config);
        }
        if(m_Texture3DManagerInitialized){
            const auto texture=m_Texture3DManager.CreateTexture3D(m_FilePath+":hwrt-dda",cells,tx,ty,tz);
            if(texture!=UINT32_MAX)if(const auto* image=m_Texture3DManager.GetTexture3D(texture)){
                std::vector<glm::vec4> palette(256);
                for(int i=0;i<256;++i){const auto c=voxData.palette[i];palette[i]=glm::vec4(c.r/255.0f,c.g/255.0f,c.b/255.0f,glm::uintBitsToFloat(PackVoxFaceMaterial(voxData,i)));}
                const glm::vec3 center=glm::vec3(minimum+maximum+glm::ivec3(1))*.5f;
                glm::mat4 gridToLocal(1);gridToLocal[0]=glm::vec4(voxelSize,0,0,0);gridToLocal[1]=glm::vec4(0,0,voxelSize,0);gridToLocal[2]=glm::vec4(0,voxelSize,0,0);
                gridToLocal[3]=glm::vec4(-center.x*voxelSize,-center.z*voxelSize,-center.y*voxelSize,1);
                m_DdaGridId=mikan::render::VoxelDDARegistry::Get().Register(m_FilePath,(void*)image->imageView,sx,sy,sz,palette,gridToLocal);
                LOGSTREAM(Info)<<"[HardwareRT DDA] registered id="<<m_DdaGridId<<" dims="<<sx<<"x"<<sy<<"x"<<sz<<" file="<<m_FilePath<<std::endl;
            }
        }
    }
    m_Loaded = true;
    LOGSTREAM(Info) << "[VoxRenderer] Loaded " << m_VoxelCount << " voxels, "
              << m_Faces.size() << " faces" << std::endl;

    return true;
}

void VoxRenderer::ClearComposite() {
    for(auto& child:m_Submeshes)child->Cleanup();
    m_Submeshes.clear();m_SurfaceInstances.clear();
}
void VoxRenderer::FinishComposite(float voxelSize) {
    m_VoxelSize=voxelSize;m_VoxelCount=0;m_HasEmissiveQuads=false;
    m_MinBounds=glm::vec3(std::numeric_limits<float>::max());m_MaxBounds=-m_MinBounds;
    m_Faces.clear();m_FaceMaterials.clear();m_Quads.clear();m_PlaneRanges.clear();m_SurfaceAttributes.clear();m_QuadMaterials.clear();
    m_MeshData={};m_QuadEncodingValid=false;m_VoxelGrid.clear();m_BVH=ModelBVH{};
    for(const auto& instance:m_SurfaceInstances){
        const auto& leaf=*m_Submeshes.at(instance.modelIndex);m_VoxelCount+=leaf.GetVoxelCount();m_HasEmissiveQuads|=leaf.HasEmissiveQuads();
        for(int corner=0;corner<8;++corner){glm::vec3 point;
            for(int axis=0;axis<3;++axis)point[axis]=(corner&(1<<axis))?leaf.GetMaxBounds()[axis]:leaf.GetMinBounds()[axis];
            point=glm::vec3(instance.transform*glm::vec4(point,1));m_MinBounds=glm::min(m_MinBounds,point);m_MaxBounds=glm::max(m_MaxBounds,point);
        }
    }
    m_Loaded=true;++m_GeometryRevision;
}
bool VoxRenderer::LoadComposite(const VoxFormat::VoxData& data,float voxelSize) {
    try {
        if(!(voxelSize>0)||!std::isfinite(voxelSize))throw std::runtime_error("Invalid VOX voxel size");
        std::map<uint32_t,uint32_t> remap;
        for(const auto& instance:data.instances){
            if(!remap.contains(instance.modelIndex)){
                const auto index=uint32_t(m_Submeshes.size());remap.emplace(instance.modelIndex,index);
                auto child=std::make_unique<VoxRenderer>();child->m_UniformSurfaceMaterials=m_UniformSurfaceMaterials;child->m_CompositePreview=IsPreviewPipeline();
                m_Submeshes.push_back(std::move(child));auto& leaf=*m_Submeshes.back();
                // Empty file paths prevent per-component cache name collisions.
                if(m_CompositeRenderPass)leaf.Init(m_CompositeRenderPass);
                if(!leaf.LoadFromVoxData(ComponentData(data,instance.modelIndex),voxelSize))throw std::runtime_error("VOX component load failed");
            }
            auto placed=instance;placed.modelIndex=remap.at(instance.modelIndex);
            for(int axis=0;axis<3;++axis)placed.transform[3][axis]*=voxelSize;
            m_SurfaceInstances.push_back(placed);
        }
        FinishComposite(voxelSize);
        LOGI("[VOX assembly] meshes=%zu instances=%zu voxels=%zu",m_Submeshes.size(),m_SurfaceInstances.size(),m_VoxelCount);return true;
    }catch(const std::exception& e){ClearComposite();m_Loaded=false;LOGE("[VOX assembly] %s",e.what());return false;}
}
std::vector<VoxelInstanceData> VoxRenderer::ComponentInstances(uint32_t mesh,const std::vector<VoxelInstanceData>& parents) const {
    std::vector<VoxelInstanceData> result;
    for(const auto& placement:m_SurfaceInstances)if(placement.modelIndex==mesh)for(auto instance:parents){
        instance.model*=placement.transform;instance.prevModel*=placement.transform;
        instance.worldMinBounds=m_Submeshes[mesh]->GetMinBounds();instance.voxelSize=m_VoxelSize;
        result.push_back(instance);
    }
    return result;
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
        LOGSTREAM(Warn) << "[VoxRenderer::GenerateTexture3DCache] No vox file loaded!" << std::endl;
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

        LOGSTREAM(Info) << "[VoxRenderer::GenerateTexture3DCache] Texture3D cache loaded from: " << cachePath << std::endl;

        // 创建 GPU Texture3D
        if (!m_Texture3DManagerInitialized) {
            VoxelTexture3DManagerConfig config;
            config.maxTextures = 64;
            config.format = VK_FORMAT_R8_UINT;
            config.enableMipmaps = mikan::render::VoxelDDARegistry::BootEnabled();
            config.filter = VK_FILTER_NEAREST;

            if (!m_Texture3DManager.Initialize(config)) {
                LOGSTREAM(Error) << "[VoxRenderer::GenerateTexture3DCache] Failed to initialize Texture3D manager!" << std::endl;
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
            LOGSTREAM(Error) << "[VoxRenderer::GenerateTexture3DCache] Failed to create Texture3D!" << std::endl;
            return false;
        }
        return true;
    }

    // Android 不生成缓存，只读取
#ifdef ANDROID_BUILD
    LOGSTREAM(Info) << "[VoxRenderer::GenerateTexture3DCache] Cache not found on Android, skipping cache generation" << std::endl;
    return false;
#else
    // 缓存不存在或加载失败，生成新缓存
    LOGSTREAM(Info) << "[VoxRenderer::GenerateTexture3DCache] Cache miss, generating Texture3D cache..." << std::endl;

    // 重新加载 vox 数据来生成缓存
    VoxFormat::VoxData voxData;
    if (!VoxFormat::LoadVoxFile(m_FilePath, voxData)) {
        LOGSTREAM(Error) << "[VoxRenderer::GenerateTexture3DCache] Failed to load vox file!" << std::endl;
        return false;
    }

    if (!m_Texture3DCacheGen.GenerateCache(voxData, m_FilePath, cachePath)) {
        LOGSTREAM(Error) << "[VoxRenderer::GenerateTexture3DCache] Failed to generate cache!" << std::endl;
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

    // Palette RGB used to be absent from this key, despite faces retaining it.
    for(const auto& color:voxData.palette)for(uint8_t value:{color.r,color.g,color.b,color.a}){
        hash^=value;hash*=FNV_PRIME;
    }
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

    // Invalidate cached face material words for the VOX emission x128 preview.
    hash ^= static_cast<size_t>(0x454D4938);
    hash *= FNV_PRIME;

    // 哈希 MATT 材质：同体素数据但金属/发光属性不同的文件不能共用网格缓存
    if (voxData.hasMaterials) {
        for (int i = 0; i < 256; ++i) {
            const auto& material = voxData.materials[i];
            const uint32_t packed = uint32_t(material.type) |
                uint32_t(glm::packHalf1x16(material.power)) << 16;
            hash ^= uint32_t(glm::packHalf1x16(material.metallic)) | (uint32_t(glm::packHalf1x16(material.rough)) << 16);
            hash *= FNV_PRIME;
            hash ^= static_cast<size_t>(packed);
            hash *= FNV_PRIME;
        }
    }
    else {
        hash ^= static_cast<size_t>(0x4E4F4D54);  // "TOMN" 标记无 MATT
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
        m_FaceMaterials = it->second.faceMaterials;
        m_SurfaceAttributes = it->second.surfaceAttributes;
        m_HasEmissiveQuads = it->second.hasEmissiveQuads;

        // 复制网格数据（注意：Vulkan 资源不能直接复制，需要重新创建）
        // 这里只复制元数据，实际 Vulkan 资源会在 BuildTriangleMesh 中创建
        m_MeshData.vertexCount = it->second.meshData.vertexCount;
        m_MeshData.indexCount = it->second.meshData.indexCount;
        m_MeshData.faceGroups = it->second.meshData.faceGroups;

        LOGSTREAM(Info) << "[VoxRenderer] Mesh cache hit! Hash: " << m_voxelDataHash
                  << ", Faces: " << m_Faces.size() << std::endl;

        return true;
    }

    LOGSTREAM(Info) << "[VoxRenderer] Mesh cache miss. Hash: " << m_voxelDataHash << std::endl;
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
    it->second.faceMaterials = m_FaceMaterials;
    it->second.surfaceAttributes = m_SurfaceAttributes;
    it->second.hasEmissiveQuads = m_HasEmissiveQuads;
    it->second.meshData.vertexCount = m_MeshData.vertexCount;
    it->second.meshData.indexCount = m_MeshData.indexCount;
    it->second.meshData.faceGroups = m_MeshData.faceGroups;
    it->second.isValid = true;

    LOGSTREAM(Info) << "[VoxRenderer] Saved mesh to cache. Hash: " << m_voxelDataHash
              << ", Faces: " << m_Faces.size() << std::endl;
}

int VoxRenderer::HasVoxelAt(int x, int y, int z) const
{
    if (x < 0 || x >= m_GridSize.x || y < 0 || y >= m_GridSize.y || z < 0 || z >= m_GridSize.z)
        return -1;
    const uint8_t value=m_VoxelGrid[z * m_GridSize.x * m_GridSize.y + y * m_GridSize.x + x];return value?int(value):-1;
}

void VoxRenderer::BuildVoxelFaces(const VoxFormat::VoxData& voxData, float voxelSize)
{
    Core::CpuStageTrace cpuStageTrace("vox.exterior_greedy_bake");
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

    LOGSTREAM(Info) << "[VoxRenderer] Bounds: " << m_MinBounds.x << "," << m_MinBounds.y << "," << m_MinBounds.z
              << " to " << m_MaxBounds.x << "," << m_MaxBounds.y << "," << m_MaxBounds.z << std::endl;

    // 计算体素数据哈希
    const bool supported=!m_UniformSurfaceMaterials;
    const bool backfill=StaticContactBackfillRequested()&&supported;
    if(StaticContactBackfillRequested()&&!supported)
        LOGI("[VOX contact backfill] disabled: entity emission or uniform surfaces; all surfaces may emit");
    m_voxelDataHash=ComputeVoxelDataHash(voxData);
    auto hashCombine=[&](size_t value){m_voxelDataHash^=value;m_voxelDataHash*=size_t(1099511628211ull);};
    hashCombine(0x42464c03u); // Compact one-word attribute descriptors.
    hashCombine(m_UniformSurfaceMaterials);hashCombine(backfill);
    uint32_t sizeBits;std::memcpy(&sizeBits,&voxelSize,sizeof(sizeBits));hashCombine(sizeBits);

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
    m_VoxelGrid.assign(gridSize, 0);

    // 填充实素网格
    for (const auto& voxel : model.voxels) {
        size_t index = voxel.z * m_GridSize.x * m_GridSize.y +
                       voxel.y * m_GridSize.x +
                       voxel.x;
        m_VoxelGrid[index] = static_cast<uint8_t>(voxel.colorIndex);
    }

    // Extract unit exterior faces directly. The only greedy pass below produces
    // the canonical surface used by rasterization, picking and hardware RT.
    std::vector<VoxRtQuad> unitQuads; m_QuadMaterials.clear();m_SurfaceAttributes.clear();
    for(int d=0;d<6;++d){
        auto& group=m_MeshData.faceGroups[d];group.faceDirection=d;group.firstIndex=uint32_t(unitQuads.size())*6;
        const int rawAxis=d<2?1:(d<4?0:2),sign=(d==0||d==3||d==4)?1:-1;
        for(const auto& voxel:model.voxels){
            int n[3]{voxel.x,voxel.y,voxel.z};n[rawAxis]+=sign;
            if(HasVoxelAt(n[0],n[1],n[2])!=-1)continue;
            const auto c=voxData.palette[voxel.colorIndex];
            uint32_t x=voxel.x-minX,y=voxel.z-minZ,z=voxel.y-minY;
            unitQuads.push_back({x|(y<<8)|(z<<16),(uint32_t(c.r)<<8)|(uint32_t(c.g)<<16)|(uint32_t(c.b)<<24)});
            m_QuadMaterials.push_back(PackVoxFaceMaterial(voxData,voxel.colorIndex));
        }
        group.indexCount=uint32_t(unitQuads.size())*6-group.firstIndex;
    }
    const size_t exposed=unitQuads.size();
    std::vector<VoxRtQuad> merged;std::vector<uint32_t> materials;std::vector<VoxRayTracingRange> ranges;
    struct Input { const std::vector<VoxRtQuad>& q; const std::vector<uint32_t>& m; const VoxelMeshData& mesh; const auto& GetQuads()const{return q;} const auto& GetQuadMaterials()const{return m;} const auto& GetMeshData()const{return mesh;} };
    BakeVoxSurfaces(Input{unitQuads,m_QuadMaterials,m_MeshData},!m_UniformSurfaceMaterials,merged,materials,ranges,m_SurfaceAttributes);
    if(backfill){
        auto ordinaryVoxel=[&](const std::array<int,3>& p){
            const int index=HasVoxelAt(p[0],p[1],p[2]);if(index<0)return false;
            if(!voxData.hasMaterials)return true;
            const auto type=voxData.materials[index].type;return type!=2&&type!=3;
        };
        const VoxContactQuery contact=[&](int d,int plane,int u,int v){
            const auto pair=VoxContactRawPair(d,plane,u,v,minX,minY,minZ);
            return ordinaryVoxel(pair[0])&&ordinaryVoxel(pair[1]);
        };
        const VoxContactQuery surfaceEligible=[&](int d,int plane,int u,int v){
            return ordinaryVoxel(VoxContactRawPair(d,plane,u,v,minX,minY,minZ)[0]);
        };
        std::vector<VoxRtQuad> candidate;std::vector<uint32_t> candidateMaterials,candidateAttributes;
        std::vector<VoxRayTracingRange> candidateRanges;VoxContactBackfillStats stats;
        BakeVoxSurfaces(Input{unitQuads,m_QuadMaterials,m_MeshData},true,candidate,candidateMaterials,candidateRanges,candidateAttributes,contact,&stats,surfaceEligible);
        // Keep the existing uniform source-emitter triangles and their power/area.
        // Direction is part of the signature even when local origins coincide.
        auto emitterSignature=[](const auto& quads,const auto& words,const auto& groups){
            std::vector<std::array<uint32_t,4>> signature;
            for(const auto& group:groups)for(uint32_t q=group.firstQuad;q<group.firstQuad+group.quadCount;++q)
                if(words[q]&2u)signature.push_back({group.direction,quads[q].geometry,quads[q].appearance,words[q]});
            std::sort(signature.begin(),signature.end());return signature;
        };
        const auto emitters=emitterSignature(merged,materials,ranges);
        if(emitters!=emitterSignature(candidate,candidateMaterials,candidateRanges))
            throw std::runtime_error("VOX contact backfill changed source-emitter geometry or attributes");
        const bool accept=candidate.size()<merged.size();
        LOGI("[VOX contact backfill] baseline=%zu candidate=%zu selected=%s baselineAttributes=%zu candidateAttributes=%zu candidates=%zu hidden=%zu finalHidden=%zu real=%zu audited=%zu preservedEmissiveQuads=%zu; fixed opaque contacts, external nearest-hit only",
            merged.size(),candidate.size(),accept?"backfill":"baseline",m_SurfaceAttributes.size()*4,candidateAttributes.size()*4,
            stats.candidates,stats.emittedHidden,accept?stats.emittedHidden:0,stats.sourceReal,stats.emittedReal,emitters.size());
        if(accept){merged=std::move(candidate);materials=std::move(candidateMaterials);ranges=std::move(candidateRanges);m_SurfaceAttributes=std::move(candidateAttributes);}
    }
    m_Faces.clear();m_FaceMaterials=std::move(materials);
    for(const auto& range:ranges)for(uint32_t i=range.firstQuad;i<range.firstQuad+range.quadCount;++i){
        const auto q=merged[i];const int d=range.direction;
        glm::vec3 p(float(q.geometry&255u),float((q.geometry>>8)&255u),float((q.geometry>>16)&255u));
        if(d==0)p.z+=1;else if(d==3)p.x+=1;else if(d==4)p.y+=1;
        glm::vec2 extent(float((q.geometry>>24)+1u),float((q.appearance&255u)+1u));
        p+=d<2?glm::vec3(extent*.5f,0):(d<4?glm::vec3(0,extent.y*.5f,extent.x*.5f):glm::vec3(extent.x*.5f,0,extent.y*.5f));
        VoxelFaceData face;face.position=m_MinBounds+p*voxelSize;face.size=extent*voxelSize;face.data=uint32_t(d);m_Faces.push_back(face);
    }
    m_Quads.clear();m_QuadMaterials.clear();
    LOGI("[VOX shared surface] greedy passes=%d exterior=%zu quads=%zu attributes=%zu bytes",backfill?2:1,exposed,m_Faces.size(),m_SurfaceAttributes.size()*4);
}

// 每面材质字（与 m_Faces/m_Quads 平行）：bit0 理想镜面（MATT 金属），
// bit1 自发光（MATT emissive），bits[15:8] 调色板索引，bits[31:16] 自发光强度 half。
// 无 MATT 或普通调色板索引保持 0：金属度 0、自发光 0。


static uint32_t PackVoxFaceMaterial(const VoxFormat::VoxData& voxData, int colorIndex)
{
    if (colorIndex <= 0 || colorIndex >= 256 || !voxData.hasMaterials)return 0;
    const auto& material = voxData.materials[colorIndex];
    uint32_t word = uint32_t(colorIndex) << 8;
    if (material.type == 1)word |= 1u;
    if (material.type == 4) {
        // bit2 PBR; bits[15:8] roughness UNORM8; high half metallic.
        word = 4u | (uint32_t(glm::clamp(material.rough, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
            (uint32_t(glm::packHalf1x16(material.metallic)) << 16);
    }
    if (material.type == 3) {
        word |= 2u;
        // MagicaVoxel 的 emissive glow 可能为 0，发光材质至少取 1 以可见。
        constexpr float kVoxEmissionScale = 128.0f; // Preview gain; preserve source VOX material values.
        const float power = (material.power > 0.0f ? material.power : 1.0f) * kVoxEmissionScale;
        word |= uint32_t(glm::packHalf1x16(power)) << 16;
    }
    return word;
}

void VoxRenderer::BuildTriangleMesh()
{
    Core::CpuStageTrace cpuStageTrace("vox.mesh_pack_upload");
    if (m_Faces.empty()) {
        return;
    }

    m_Quads.clear();m_PlaneRanges.clear();m_QuadMaterials.clear();m_QuadEncodingValid=true;
    // Picking uses the renderer bounds; hardware RT builds its own BLAS.
    // Keep draw counts without expanding a second CPU triangle mesh/BVH.
    size_t indexCount=0;
    m_BVH=ModelBVH{};

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
        size_t groupStartIndex = indexCount;
        glm::vec3 faceCenterSum(0.0f);
        size_t faceCount = 0;

        // 用于计算包围盒范围的边界
        glm::vec3 minPos(FLT_MAX), maxPos(-FLT_MAX);

        // 遍历所有面，只处理当前方向的面
        for (size_t faceIndex = 0; faceIndex < m_Faces.size(); ++faceIndex) {
            const auto& face = m_Faces[faceIndex];
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

            glm::vec3 quadMin=corners[0],quadMax=corners[0];
            for(int c=1;c<4;++c){quadMin=glm::min(quadMin,corners[c]);quadMax=glm::max(quadMax,corners[c]);}
            const auto grid=(quadMin-m_MinBounds)/m_VoxelSize;
            const auto extent=(quadMax-quadMin)/m_VoxelSize;
            glm::ivec3 origin=glm::ivec3(glm::round(grid));
            const int width=int(glm::round(faceDir<2?extent.x:(faceDir<4?extent.z:extent.x)));
            const int height=int(glm::round(faceDir<4?extent.y:extent.z));
            bool valid=width>=1 && width<=256 && height>=1 && height<=256 && materialIndex==0;
            for(int c=0;c<3;++c)valid=valid && std::abs(grid[c]-float(origin[c]))<0.001f;
            // Positive face planes lie one voxel beyond their owning voxel origin.
            if(faceDir==0)--origin.z;
            else if(faceDir==3)--origin.x;
            else if(faceDir==4)--origin.y;
            for(int c=0;c<3;++c)valid=valid && origin[c]>=0 && origin[c]<=255;
            m_QuadEncodingValid=m_QuadEncodingValid && valid;
            const int axis=faceDir<2?2:(faceDir<4?0:1),uAxis=faceDir<2?0:(faceDir<4?2:0),vAxis=faceDir<4?1:2;
            AppendVoxPlane(m_PlaneRanges,uint32_t(m_Quads.size()),uint32_t(origin[axis]),uint32_t(faceDir));
            if(valid)m_Quads.push_back({uint32_t(origin[uAxis])|(uint32_t(origin[vAxis])<<8)|(uint32_t(width-1)<<16)|(uint32_t(height-1)<<24)});
            else m_Quads.push_back({0}); // Preserve ordering until the invalid encoding is reported.
            m_QuadMaterials.push_back(m_FaceMaterials.size()>faceIndex?m_FaceMaterials[faceIndex]:0u);
            indexCount+=6;
        }

        // 记录当前面组的索引范围
        if (indexCount > groupStartIndex) {
            m_MeshData.faceGroups[faceDir].firstIndex = groupStartIndex;
            m_MeshData.faceGroups[faceDir].indexCount = indexCount - groupStartIndex;
            // 计算平均中心位置
            if (faceCount > 0) {
                m_MeshData.faceGroups[faceDir].faceCenter = faceCenterSum / static_cast<float>(faceCount);
            }

            // 计算包围盒范围（用于动态调整背面剔除阈值）
            glm::vec3 extent = maxPos - minPos;
            m_MeshData.faceGroups[faceDir].faceExtent = extent;

            LOGSTREAM(Info) << "[VoxRenderer] Face group " << faceDir << ": "
                      << m_MeshData.faceGroups[faceDir].indexCount << " indices, "
                      << "extent: (" << extent.x << ", " << extent.y << ", " << extent.z << ")" << std::endl;
        }
    }


        m_HasEmissiveQuads=false;
        for(const uint32_t w:m_QuadMaterials)if(w&2u){m_HasEmissiveQuads=true;break;}

        if(!HasValidQuads())throw std::runtime_error("Vox quad encoding exceeds the supported 256-voxel range");
        m_MeshData.vertexCount=m_Quads.size()*4;m_MeshData.indexCount=indexCount;
        std::vector<uint32_t> rasterWords;for(const auto& q:m_Quads)rasterWords.push_back(q.geometry);
        const uint32_t lookup=uint32_t(rasterWords.size());rasterWords.resize(rasterWords.size()+m_Quads.size(),0u);
        AppendVoxRasterAttributes(rasterWords,m_SurfaceAttributes,0,uint32_t(m_Quads.size()),lookup);AppendVoxPlaneFooter(rasterWords,m_PlaneRanges,lookup);
        m_DirectQuads.Cleanup();
        if(!m_DirectQuads.Create(rasterWords.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            throw std::runtime_error("Failed to allocate vox quad geometry");
        m_DirectQuads.Write(rasterWords.data(),rasterWords.size()*4);
        if(m_DirectQuadDescriptor) {
            VkDescriptorBufferInfo buffer{m_DirectQuads.GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=m_DirectQuadDescriptor;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffer;
            vkUpdateDescriptorSets(g_Device,1,&write,0,nullptr);write.dstBinding=2;vkUpdateDescriptorSets(g_Device,1,&write,0,nullptr);
        }


    LOGSTREAM(Info) << "[VoxRenderer] Packed " << m_Quads.size() << " quads, "
              << indexCount / 3 << " draw triangles; legacy CPU BVH disabled, bounds picking retained" << std::endl;
    static uint64_t nextRevision=0;m_GeometryRevision=++nextRevision;

    // 保存到缓存（如果之前是缓存未命中）
    if (!m_useCachedMesh) {
        SaveMeshToCache();
    }
}


bool VoxRenderer::PrepareCsmInstances(VkRenderPass renderPass,
    const std::vector<VoxelInstanceData>& instances, uint64_t renderEpoch)
{
    if(IsComposite()){
        bool prepared=false;
        for(uint32_t mesh=0;mesh<m_Submeshes.size();++mesh)
            prepared|=m_Submeshes[mesh]->PrepareCsmInstances(renderPass,ComponentInstances(mesh,instances),renderEpoch);
        return prepared;
    }
    if (!m_Loaded || instances.empty() || m_MeshData.indexCount == 0) return false;
    if (m_CsmRenderPass != renderPass || m_CsmPipeline.GetPipeline() == VK_NULL_HANDLE) {
        m_CsmPipeline.Cleanup();
        PipelineConfig config = m_CsmMeshConfig;
        config.fragShader = "voxel_csm.frag.spv";
        config.subpass = 0;
        config.colorAttachmentCount = 0;
        config.colorWriteMasks.clear();
        config.cullMode = VK_CULL_MODE_NONE;
        config.depthTest = true;
        config.depthWrite = true;
        config.depthCompareOp = VK_COMPARE_OP_LESS;
        config.depthBiasEnable = true;
        config.depthBiasConstantFactor = 2.0f;
        config.depthBiasSlopeFactor = 2.0f;
        if (!m_CsmPipeline.Create(renderPass, m_DirectQuadLayout, config)) return false;
        m_CsmRenderPass = renderPass;
    }
    if (instances.size() > m_CsmInstanceCapacity) {
        vkDeviceWaitIdle(g_Device);
        m_CsmInstanceCapacity = 0;
        m_CsmInstanceEpochs.fill(UINT64_MAX);
        for (auto& buffer : m_CsmInstanceBuffers) {
            buffer.Cleanup();
            if (!buffer.Create(instances.size() * sizeof(VoxelInstanceData),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
        }
        m_CsmInstanceCapacity = instances.size();
    }
    const uint32_t frame = GetCurrentFrameIndex() % m_CsmInstanceBuffers.size();
    if (m_CsmInstanceEpochs[frame] != renderEpoch) {
        m_CsmInstanceBuffers[frame].Write(instances.data(), instances.size() * sizeof(VoxelInstanceData));
        m_CsmInstanceEpochs[frame] = renderEpoch;
    }
    return true;
}

void VoxRenderer::RenderCsmDepth(VkCommandBuffer commandBuffer, const glm::mat4& shadowMatrix,
    const std::vector<VoxelInstanceData>& instances, const std::array<Plane, 6>& cascadePlanes)
{
    if(IsComposite()){
        for(uint32_t mesh=0;mesh<m_Submeshes.size();++mesh)
            m_Submeshes[mesh]->RenderCsmDepth(commandBuffer,shadowMatrix,ComponentInstances(mesh,instances),cascadePlanes);
        return;
    }
    if (m_CsmPipeline.GetPipeline() == VK_NULL_HANDLE || instances.empty()) return;
    const uint32_t frame = GetCurrentFrameIndex() % m_CsmInstanceBuffers.size();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_CsmPipeline.GetPipeline());
    VoxelMeshUniformData push{};
    push.projView = shadowMatrix;
    push.prevProjView = shadowMatrix;
    vkCmdPushConstants(commandBuffer, m_CsmPipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT,
                      0, sizeof(push), &push);

        vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,m_CsmPipeline.GetLayout(),0,1,&m_DirectQuadDescriptor,0,nullptr);
        const auto buffer=m_CsmInstanceBuffers[frame].GetBuffer();const VkDeviceSize offset=0;
        vkCmdBindVertexBuffers(commandBuffer,1,1,&buffer,&offset);
        vkCmdBindIndexBuffer(commandBuffer,GetSharedQuadIndexBuffer(),0,VK_INDEX_TYPE_UINT16);

    const AABB localBounds(m_MinBounds, m_MaxBounds);
    // Consecutive surviving instances remain one instanced draw; no per-cascade uploads.
    size_t first = 0;
    while (first < instances.size()) {
        if (!localBounds.Transform(instances[first].model).IsInsideFrustum(cascadePlanes)) {
            ++first;
            continue;
        }
        size_t end = first + 1;
        while (end < instances.size() &&
            localBounds.Transform(instances[end].model).IsInsideFrustum(cascadePlanes)) ++end;

            for(const auto& group:m_MeshData.faceGroups) {
                if(!group.indexCount)continue;
                push.padding=float(group.faceDirection);
                vkCmdPushConstants(commandBuffer,m_CsmPipeline.GetLayout(),VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
                ForEachVoxIndexSegment(group.firstIndex,group.indexCount,[&](size_t index,size_t count,size_t) {
                    vkCmdDrawIndexed(commandBuffer,uint32_t(count),uint32_t(end-first),0,int32_t(index/6*4),uint32_t(first));
                });
            }
                first = end;
    }
}


void VoxRenderer::CreateMeshInstanceBuffer(size_t maxInstances)
{
    m_RenderData.currentMeshInstanceBufferSize = maxInstances;
    for (size_t i = 0; i < VoxelRenderData::MAX_FRAMES_IN_FLIGHT; i++) {
        VoxelRenderData::MeshInstanceUpload upload;
        if (!CreateMeshInstanceUpload(upload, maxInstances)) {
            LOGSTREAM(Error) << "[VoxRenderer] Failed to create mesh instance buffer!" << std::endl;
            DestroyMeshInstanceUpload(upload);
            return;
        }
        m_RenderData.meshInstanceUploads[i].push_back(upload);
    }
}

bool VoxRenderer::CreateMeshInstanceUpload(VoxelRenderData::MeshInstanceUpload& upload,
                                            size_t maxInstances)
{
    const VkDeviceSize bufferSize = sizeof(VoxelInstanceData) * maxInstances;
    upload.capacity = maxInstances;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &upload.buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, upload.buffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &upload.memory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, upload.buffer, g_Allocator);
        upload.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(g_Device, upload.buffer, upload.memory, 0);
    if (vkMapMemory(g_Device, upload.memory, 0, bufferSize, 0, &upload.mapped) != VK_SUCCESS) {
        vkFreeMemory(g_Device, upload.memory, g_Allocator);
        vkDestroyBuffer(g_Device, upload.buffer, g_Allocator);
        upload.memory = VK_NULL_HANDLE;
        upload.buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VoxRenderer::DestroyMeshInstanceUpload(VoxelRenderData::MeshInstanceUpload& upload)
{
    if (upload.mapped != nullptr && upload.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(g_Device, upload.memory);
    }
    if (upload.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, upload.buffer, g_Allocator);
    }
    if (upload.memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, upload.memory, g_Allocator);
    }
    upload = {};
}


VkBuffer VoxRenderer::UpdateMeshInstanceBuffer(const std::vector<VoxelInstanceData>& instances)
{
    if (instances.empty()) {
        return VK_NULL_HANDLE;
    }

    const uint32_t frameIndex =
        GetCurrentFrameIndex() % VoxelRenderData::MAX_FRAMES_IN_FLIGHT;
    const uint64_t frameSerial = GetCurrentFrameSerial();
    if (m_RenderData.meshInstanceUploadFrameSerial != frameSerial) {
        m_RenderData.meshInstanceUploadFrameSerial = frameSerial;
        for (uint32_t& cursor : m_RenderData.meshInstanceUploadCursor) cursor = 0;
    }

    const uint32_t uploadIndex = m_RenderData.meshInstanceUploadCursor[frameIndex]++;
    auto& uploads = m_RenderData.meshInstanceUploads[frameIndex];
    if (uploadIndex >= uploads.size()) {
        uploads.emplace_back();
    }

    VoxelRenderData::MeshInstanceUpload& upload = uploads[uploadIndex];
    if (instances.size() > upload.capacity) {
        DestroyMeshInstanceUpload(upload);
        const size_t newCapacity = std::max(instances.size() * 2, size_t(1));
        if (!CreateMeshInstanceUpload(upload, newCapacity)) {
            return VK_NULL_HANDLE;
        }
        m_RenderData.currentMeshInstanceBufferSize =
            std::max(m_RenderData.currentMeshInstanceBufferSize, newCapacity);
    }
    if (upload.mapped == nullptr) {
        return VK_NULL_HANDLE;
    }

    const VkDeviceSize bufferSize = sizeof(VoxelInstanceData) * instances.size();
    memcpy(upload.mapped, instances.data(), static_cast<size_t>(bufferSize));
    return upload.buffer;
}


void VoxRenderer::CreatePipeline(VkRenderPass renderPass)
{
    PipelineConfig meshConfig;
    meshConfig.vertShader = "voxel_quad_direct.vert.spv";
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
        VK_COLOR_COMPONENT_R_BIT, // Opaque vox geometry contributes to the Hi-Z source.
        0
    };
    meshConfig.subpass = 1;               // MRT 几何 subpass（0=z-prepass）
    meshConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // z-prepass 后必须 <=

    VkVertexInputBindingDescription meshBindings[2] = {};
    meshBindings[1].binding = 1;
    meshBindings[1].stride = sizeof(VoxelInstanceData);
    meshBindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription meshAttributes[15] = {};
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

    meshConfig.vertexBindings.assign(meshBindings+1, meshBindings+2);
    meshConfig.vertexAttributes.assign(meshAttributes+3, meshAttributes+15);

    meshConfig.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    meshConfig.pushConstantRange.offset = 0;
    meshConfig.pushConstantRange.size = sizeof(VoxelMeshUniformData);
    meshConfig.usePushConstants = true;
    if(IsPreviewPipeline()) {
        meshConfig.fragShader="voxel_preview.frag.spv";
        meshConfig.colorAttachmentCount=1;meshConfig.colorWriteMasks.resize(1);meshConfig.subpass=0;
    }

        VkDescriptorSetLayoutBinding binding[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount=2;setInfo.pBindings=binding;
        if(!m_DirectQuadLayout && vkCreateDescriptorSetLayout(g_Device,&setInfo,g_Allocator,&m_DirectQuadLayout)!=VK_SUCCESS)
            throw std::runtime_error("Failed to create direct vox quad layout");
        if(!m_DirectQuadPool) {
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
            if(vkCreateDescriptorPool(g_Device,&pool,g_Allocator,&m_DirectQuadPool)!=VK_SUCCESS)throw std::runtime_error("Failed to create vox quad pool");
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=m_DirectQuadPool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&m_DirectQuadLayout;
            if(vkAllocateDescriptorSets(g_Device,&alloc,&m_DirectQuadDescriptor)!=VK_SUCCESS)throw std::runtime_error("Failed to allocate vox quad descriptor");
        }
        VkDescriptorBufferInfo buffer{m_DirectQuads.GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=m_DirectQuadDescriptor;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffer;
        if(m_DirectQuads.GetBuffer()){vkUpdateDescriptorSets(g_Device,1,&write,0,nullptr);write.dstBinding=2;vkUpdateDescriptorSets(g_Device,1,&write,0,nullptr);}
        PipelineConfig direct=meshConfig;direct.vertShader="voxel_quad_direct.vert.spv";
        m_CsmMeshConfig=direct;
        if(!m_DirectQuadPipeline.Create(renderPass,m_DirectQuadLayout,direct))throw std::runtime_error("Failed to create direct vox quad pipeline");
        direct.polygonMode=VK_POLYGON_MODE_LINE;
        if(!m_DirectQuadWirePipeline.Create(renderPass,m_DirectQuadLayout,direct))throw std::runtime_error("Failed to create vox quad wireframe pipeline");


    VkDescriptorSetLayoutBinding quadBindings[]={{0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo quadSet{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};quadSet.bindingCount=3;quadSet.pBindings=quadBindings;
    if(!m_QuadSetLayout && vkCreateDescriptorSetLayout(g_Device,&quadSet,g_Allocator,&m_QuadSetLayout)==VK_SUCCESS) {
        PipelineConfig quadConfig=meshConfig;quadConfig.vertShader="voxel_quad.vert.spv";
        quadConfig.vertexBindings={{0,sizeof(uint32_t),VK_VERTEX_INPUT_RATE_INSTANCE}};
        quadConfig.vertexAttributes={{3,0,VK_FORMAT_R32_UINT,0}};
        if(!m_QuadPipeline.Create(renderPass,m_QuadSetLayout,quadConfig))throw std::runtime_error("Failed to create vox quad MDI pipeline");
    }

}

void VoxRenderer::RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView, const glm::mat4& prevProjView,
                                       const glm::vec3& cameraPosition,
                                       const std::vector<VoxelInstanceData>& instances)
{
    RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,instances,false);
}

void VoxRenderer::RenderMesh(VkCommandBuffer commandBuffer, int width, int height,
                              const glm::mat4& projView, const glm::mat4& prevProjView,
                              const glm::vec3& cameraPosition,
                              const std::vector<VoxelInstanceData>& instances)
{
    RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,instances,false);
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
                                                 bool enableBackfaceCulling)
{
    RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,instances,false);
}


void VoxRenderer::RenderMeshWireframe(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView, const glm::mat4& prevProjView,
                                       const glm::vec3& cameraPosition,
                                       const std::vector<VoxelInstanceData>& instances)
{
    RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,instances,true);
}

void VoxRenderer::RenderWireframe(VkCommandBuffer commandBuffer, int width, int height,
                                   const glm::mat4& projView, const glm::mat4& prevProjView,
                                   const glm::vec3& cameraPosition,
                                   const std::vector<VoxelInstanceData>& instances)
{
    RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,instances,true);
}

// ============================================================================
// BVH 相关方法实现
// ============================================================================

void VoxRenderer::BuildBVHFromMesh(const std::vector<VoxPickingVertex>& meshVertices,
                                    const std::vector<uint32_t>& meshIndices)
{
    if (meshVertices.empty() || meshIndices.empty()) {
        LOGSTREAM(Error) << "[VoxRenderer::BuildBVHFromMesh] Cannot build BVH: empty mesh!" << std::endl;
        return;
    }

    LOGSTREAM(Info) << "[VoxRenderer::BuildBVHFromMesh] Starting BVH construction..." << std::endl;

    // 尝试从缓存加载 BVH
    if (m_SurfaceAttributes.empty() && LoadBVHCache()) {
        LOGSTREAM(Info) << "[VoxRenderer::BuildBVHFromMesh] BVH loaded from cache successfully!" << std::endl;
        return;
    }

    LOGSTREAM(Info) << "[VoxRenderer::BuildBVHFromMesh] Cache miss, building BVH from scratch..." << std::endl;

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

    LOGSTREAM(Info) << "=== Voxel Mesh BVH Statistics ===" << std::endl;
    LOGSTREAM(Info) << "  Input vertices: " << meshVertices.size() << std::endl;
    LOGSTREAM(Info) << "  Input triangles: " << meshIndices.size() / 3 << std::endl;
    LOGSTREAM(Info) << "  BVH vertices: " << vertices.size() << std::endl;
    LOGSTREAM(Info) << "  BVH triangles: " << triangles.size() << std::endl;
    LOGSTREAM(Info) << "  BVH nodes: " << nodes.size() << std::endl;
    LOGSTREAM(Info) << "  Leaf nodes: " << leafNodes << std::endl;
    LOGSTREAM(Info) << "  Max depth: " << maxDepth << std::endl;
    LOGSTREAM(Info) << "  Avg triangles per leaf: " << (leafNodes > 0 ? (float)totalTrianglesInLeaves / leafNodes : 0) << std::endl;

    // 计算内存占用
    size_t nodeMemory = nodes.size() * sizeof(ModelBVHNode);
    size_t triangleMemory = triangles.size() * sizeof(Triangle);
    size_t vertexMemory = vertices.size() * sizeof(BVHVertex);
    size_t totalMemory = nodeMemory + triangleMemory + vertexMemory;

    LOGSTREAM(Info) << "  Memory usage:" << std::endl;
    LOGSTREAM(Info) << "    - BVH nodes: " << (nodeMemory / 1024) << " KB" << std::endl;
    LOGSTREAM(Info) << "    - Triangles: " << (triangleMemory / 1024) << " KB" << std::endl;
    LOGSTREAM(Info) << "    - Vertices: " << (vertexMemory / 1024) << " KB" << std::endl;
    LOGSTREAM(Info) << "    - Total: " << (totalMemory / 1024) << " KB (" << totalMemory << " bytes)" << std::endl;
    LOGSTREAM(Info) << "=================================" << std::endl;

    // 保存到缓存
    if(m_SurfaceAttributes.empty())SaveBVHCache();

    LOGSTREAM(Info) << "[VoxRenderer::BuildBVHFromMesh] BVH construction completed!" << std::endl;

    // 生成 Texture3D 缓存
    if(std::filesystem::path(m_FilePath).extension()!=".voxmesh")GenerateTexture3DCache();
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
        LOGSTREAM(Warn) << "[VoxRenderer::LoadBVHCache] Cache file not found: " << cachePath << std::endl;
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
        LOGSTREAM(Error) << "[VoxRenderer::LoadBVHCache] Failed to read cache header!" << std::endl;
        file.close();
        return false;
    }

    // 检查版本号（只支持 v3 超压缩格式）
    if (version != 3) {
        LOGSTREAM(Error) << "[VoxRenderer::LoadBVHCache] Unsupported cache version: " << version << " (only v3 supported)" << std::endl;
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
        LOGSTREAM(Warn) << "[VoxRenderer::LoadBVHCache] Cache file size mismatch!" << std::endl;
        LOGSTREAM(Warn) << "  Expected: " << expectedFileSize << " bytes" << std::endl;
        LOGSTREAM(Warn) << "  Actual: " << actualFileSize << " bytes" << std::endl;
        file.close();
        return false;
    }

    // 读取 v3 超压缩 BVH 节点
    LOGSTREAM(Info) << "[VoxRenderer::LoadBVHCache] Decompressing BVH nodes (v3 ultra-compact)..." << std::endl;

    std::vector<UltraCompactBVHNode> ultraCompactNodes(nodeCount);
    file.read(reinterpret_cast<char*>(ultraCompactNodes.data()), nodeCount * sizeof(UltraCompactBVHNode));

    // 读取三角形
    std::vector<Triangle> loadedTriangles(triangleCount);
    file.read(reinterpret_cast<char*>(loadedTriangles.data()), triangleCount * sizeof(Triangle));

    if (file.fail()) {
        LOGSTREAM(Error) << "[VoxRenderer::LoadBVHCache] Failed to read cache data!" << std::endl;
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
    LOGSTREAM(Info) << "[VoxRenderer::LoadBVHCache] Rebuilding vertices from faces..." << std::endl;

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

    LOGSTREAM(Info) << "[VoxRenderer::LoadBVHCache] BVH cache loaded successfully!" << std::endl;
    LOGSTREAM(Info) << "  Nodes: " << nodeCount << " x 16 bytes (v3 ultra-compact)" << std::endl;
    LOGSTREAM(Info) << "  Triangles: " << triangleCount << std::endl;
    LOGSTREAM(Info) << "  Vertices rebuilt: " << bvhVertices.size() << std::endl;

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

    LOGSTREAM(Info) << "[VoxRenderer::LoadTexture3DCache] Texture3D cache loaded: "
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
        LOGSTREAM(Error) << "[VoxRenderer::SaveBVHCache] Cannot save invalid BVH!" << std::endl;
        return false;
    }

    std::string cachePath = GetBVHCachePath();
    if (cachePath.empty()) {
        LOGSTREAM(Warn) << "[VoxRenderer::SaveBVHCache] Invalid cache path!" << std::endl;
        return false;
    }

    LOGSTREAM(Info) << "[VoxRenderer::SaveBVHCache] Saving BVH cache to: " << cachePath << std::endl;

    std::ofstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        LOGSTREAM(Error) << "[VoxRenderer::SaveBVHCache] Failed to create cache file!" << std::endl;
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
        LOGSTREAM(Error) << "[VoxRenderer::SaveBVHCache] Failed to write cache data!" << std::endl;
        file.close();
        return false;
    }

    file.close();

    // 计算文件大小
    uint64_t fileSize = sizeof(uint32_t) * 4 +
        nodeCount * sizeof(UltraCompactBVHNode) +
        triangleCount * sizeof(Triangle);

    LOGSTREAM(Info) << "[VoxRenderer::SaveBVHCache] BVH cache saved successfully!" << std::endl;
    LOGSTREAM(Info) << "  File size: " << (fileSize / 1024) << " KB (" << fileSize << " bytes)" << std::endl;
    LOGSTREAM(Info) << "  Nodes: " << nodeCount << " x " << sizeof(UltraCompactBVHNode) << " bytes" << std::endl;
    LOGSTREAM(Info) << "  Triangles: " << triangleCount << " x " << sizeof(Triangle) << " bytes" << std::endl;
    LOGSTREAM(Info) << "  Compression: " << (100.0f * fileSize / (nodeCount * sizeof(ModelBVHNode) + triangleCount * sizeof(Triangle))) << "%" << std::endl;

    return true;
#endif
}

void VoxRenderer::RenderQuadsDirect(VkCommandBuffer commandBuffer,int width,int height,
    const glm::mat4& projView,const glm::mat4& prevProjView,const glm::vec3& cameraPosition,
    const std::vector<VoxelInstanceData>& instances,bool wireframe)
{
    if(IsComposite()){
        for(uint32_t mesh=0;mesh<m_Submeshes.size();++mesh)
            m_Submeshes[mesh]->RenderQuadsDirect(commandBuffer,width,height,projView,prevProjView,cameraPosition,ComponentInstances(mesh,instances),wireframe);
        return;
    }
    if(!m_Loaded || !HasValidQuads() || instances.empty())return;
    auto& pipeline=wireframe?m_DirectQuadWirePipeline:m_DirectQuadPipeline;
    if(!pipeline.GetPipeline())return;
    const VkBuffer buffer=UpdateMeshInstanceBuffer(instances);if(!buffer)return;
    VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D scissor{{0,0},{uint32_t(width),uint32_t(height)}};
    vkCmdSetViewport(commandBuffer,0,1,&viewport);vkCmdSetScissor(commandBuffer,0,1,&scissor);
    vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.GetPipeline());
    vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.GetLayout(),0,1,&m_DirectQuadDescriptor,0,nullptr);
    const VkDeviceSize offset=0;vkCmdBindVertexBuffers(commandBuffer,1,1,&buffer,&offset);
    vkCmdBindIndexBuffer(commandBuffer,GetSharedQuadIndexBuffer(),0,VK_INDEX_TYPE_UINT16);
    VoxelMeshUniformData push{projView,prevProjView,cameraPosition,0};
    for(const auto& group:m_MeshData.faceGroups) {
        if(!group.indexCount)continue;
        push.padding=float(group.faceDirection);
        vkCmdPushConstants(commandBuffer,pipeline.GetLayout(),VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
        ForEachVoxIndexSegment(group.firstIndex,group.indexCount,[&](size_t index,size_t count,size_t) {
            vkCmdDrawIndexed(commandBuffer,uint32_t(count),uint32_t(instances.size()),0,int32_t(index/6*4),0);
        });
    }
}

std::pair<uint32_t,uint32_t> VoxRenderer::DecodeQuad(uint32_t index) const {
    auto it=std::upper_bound(m_PlaneRanges.begin(),m_PlaneRanges.end(),index,[](uint32_t q,const VoxPlaneRange& p){return q<p.firstQuad;});
    if(it==m_PlaneRanges.begin())throw std::runtime_error("Missing VOX plane");--it;
    auto decoded=DecodeVoxQuad(m_Quads.at(index).geometry,it->planeDirection);
    if(!m_SurfaceAttributes.empty()){
        const auto& a=m_SurfaceAttributes;const uint32_t descriptor=a[a[1]+index];uint32_t id=descriptor&0x7fffffffu;
        if(descriptor&0x80000000u)id=(a[a[2]+id/4]>>((id%4)*8))&255u;
        decoded.second|=a[a[0]+id*2];
    }
    return decoded;
}
