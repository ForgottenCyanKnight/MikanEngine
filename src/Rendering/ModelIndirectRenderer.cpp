#include "Rendering/ModelIndirectRenderer.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/HiZHistory.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/RenderStats.h"
#include "Rendering/ModelRendererInternals.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/VulkanManager.h"
#include "Core/RenderGlobals.h"
#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <tuple>

extern RenderTarget g_GameRenderTarget;
namespace {
struct Geometry { uint32_t first, count; int32_t vertex; };
struct Source { ModelInstanceData instance; glm::vec4 lo, hi; };
struct SubMeshBounds { glm::vec4 lo, hi; };
static_assert(sizeof(SubMeshBounds)==32);
struct Params { glm::uvec4 counts; glm::mat4 hizViewProj; glm::uvec4 hizParams; };
static_assert(sizeof(Source)==224 && sizeof(ModelInstanceData)==192 && sizeof(VkDrawIndexedIndirectCommand)==20);
struct Draw { ModelRenderer* renderer; size_t subMesh; bool doubleSided; std::string materialKey; };
struct Frame {
    VulkanBuffer requests, commands, indices, params;
    VkDescriptorSet computeSet=VK_NULL_HANDLE, graphicsSet=VK_NULL_HANDLE;
    size_t requestCapacity=0, commandCapacity=0;
    uint64_t epoch=UINT64_MAX;
    std::vector<uint8_t> residentRequests, residentCommands;
    uint64_t topologyVersion=0, visibilityVersion=0;
    glm::mat4 cachedMainVP{}, cachedRasterVP{};
    bool cachedMainFrustum=false, cachedSubMeshCull=false, visibilityValid=false;
    std::vector<glm::uvec4> cachedRequests;
    std::vector<uint32_t> cachedCandidates;
    std::vector<Draw> draws;
    std::set<std::pair<ModelRenderer*,bool>> handled;
    uint32_t instanceCount=0;
};
struct SharedCull {
    VulkanBuffer sources, subMeshBounds, requests, candidates, visibility, results, params;
    VkBufferView sourceView=VK_NULL_HANDLE;
    std::vector<uint64_t> residentSourceVersions;
    std::vector<Source> sourceUploadScratch;
    std::vector<uint8_t> residentSubMeshBounds;
    uint64_t uploadEpoch=UINT64_MAX;
    VkDescriptorSet set=VK_NULL_HANDLE;
    size_t sourceCapacity=0, requestCapacity=0, commandCapacity=0;
    uint64_t epoch=UINT64_MAX, topologyVersion=0, visibilityVersion=0;
    glm::mat4 mainVP{};
    bool mainFrustum=false, subMeshCull=false, valid=false;
    std::vector<glm::uvec4> cpuRequests;
    std::vector<uint32_t> cpuCandidates;
    std::vector<uint8_t> residentRequests, residentCandidates;
};
std::map<uint32_t,std::unique_ptr<SharedCull>> sharedFrames;
VulkanBuffer vertices, indices, vertexStaging, indexStaging;
bool atlasPending=false;
uint64_t atlasEpoch=UINT64_MAX;
std::vector<VkBuffer> atlasSignature;
std::map<ModelRenderer*,std::vector<Geometry>> geometry;
std::map<uint32_t,std::unique_ptr<Frame>> frames;
VkDescriptorSetLayout instanceLayout=VK_NULL_HANDLE, computeLayout=VK_NULL_HANDLE;
VkDescriptorPool pool=VK_NULL_HANDLE;
VkPipelineLayout cullLayout=VK_NULL_HANDLE;
VkPipeline cullPipeline=VK_NULL_HANDLE;

// Stable slots remain independent of each viewport's compact candidate list.
struct Node {
    Source source{};
    ModelRenderer* renderer=nullptr;
    ECS::Entity entity=ECS::INVALID_ENTITY;
    uint64_t version=0, geometryRevision=0, materialRevision=UINT64_MAX;
    bool hasMaterial=false;
    std::vector<AABB> subBounds, tlasBounds;
};
struct BatchTemplate { ModelRenderer* renderer; bool doubleSided; std::vector<uint32_t> slots, commands; };
std::vector<Node> nodes;
std::map<std::pair<ModelRenderer*,ECS::Entity>,uint32_t> nodeSlots;
std::vector<uint32_t> freeSlots;
std::vector<SceneModelBatch> cachedBatches;
std::vector<BatchTemplate> batchTemplates;
std::vector<Draw> cachedDraws;
std::vector<VkDrawIndexedIndirectCommand> commandTemplates;
std::vector<SubMeshBounds> boundsTemplates;
uint64_t collectionEpoch=UINT64_MAX, batchSignature=0, templateSignature=0;
uint64_t topologyVersion=1, visibilityVersion=1, sourceSerial=1;
void Hash(uint64_t& h,uint64_t v) { h^=v+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2); }
uint64_t ResourceSignature(ModelRenderer* r) {
    uint64_t h=reinterpret_cast<uintptr_t>(r);
    Hash(h,r->HasModelLoaded());Hash(h,r->HasSkinning());Hash(h,r->HasAnimation());Hash(h,r->GetSubMeshes().size());
    for(const auto& sm:r->GetSubMeshes()) {
        Hash(h,reinterpret_cast<uintptr_t>(sm.vertexBuffer));Hash(h,reinterpret_cast<uintptr_t>(sm.indexBuffer));
        Hash(h,reinterpret_cast<uintptr_t>(sm.descriptorSet));Hash(h,sm.indexCount);
        Hash(h,sm.wrapMode);Hash(h,sm.alphaMode);Hash(h,sm.doubleSided);
        for(float v:{sm.metallic,sm.roughness,sm.ao,sm.mrValid,sm.alphaCutoff,sm.diffuseTransmissionFactor})Hash(h,std::bit_cast<uint32_t>(v));
        for(const auto* path:{&sm.diffuseTexturePath,&sm.normalTexturePath,&sm.roughnessTexturePath,&sm.metallicTexturePath,&sm.emissiveTexturePath})Hash(h,std::hash<std::string>{}(*path));
    }
    return h;
}
// Cache transformed TLAS bounds with the instance, then prune whole branches
// when a camera changes. Small models keep the cheaper flat AABB loop.
void QuerySubMeshes(const Node& instance, int index, const std::array<Plane,6>& mainPlanes,
                    const std::array<Plane,6>& scenePlanes, bool mainCull, bool sceneCull,
                    std::vector<size_t>& result) {
    const auto& tree=instance.renderer->GetBVHData().GetTopLevelNodes();
    if(index<0 || size_t(index)>=tree.size() || size_t(index)>=instance.tlasBounds.size())return;
    const auto& box=instance.tlasBounds[index];
    if((mainCull && !box.IsInsideFrustum(mainPlanes)) || (sceneCull && !box.IsInsideFrustum(scenePlanes)))return;
    const auto& node=tree[index];
    if(node.isLeaf){if(node.subMeshIndex>=0 && size_t(node.subMeshIndex)<instance.subBounds.size())result.push_back(size_t(node.subMeshIndex));return;}
    QuerySubMeshes(instance,node.left,mainPlanes,scenePlanes,mainCull,sceneCull,result);
    QuerySubMeshes(instance,node.right,mainPlanes,scenePlanes,mainCull,sceneCull,result);
}
bool Eligible(ModelRenderer* r) {
    if(!r || !r->HasModelLoaded() || r->HasSkinning() || r->HasAnimation() || r->GetMeshData().isMmd) return false;
    const auto& cpu=r->GetMeshData().subMeshes;
    const auto& gpu=r->GetSubMeshes();
    if(cpu.empty() || cpu.size()!=gpu.size()) return false;
    for(size_t i=0;i<cpu.size();++i)
        if(cpu[i].vertices.empty() || cpu[i].indices.empty() || !gpu[i].descriptorSet ||
           gpu[i].alphaMode>1 || !gpu[i].vertexBuffer || gpu[i].indexCount!=cpu[i].indices.size()) return false;
    return r->GetIndirectPipeline(false) && r->GetIndirectPipeline(true);
}
std::string MaterialKey(const SubMeshRenderData& sm, bool ds) {
    std::string k=sm.diffuseTexturePath+'\x1f'+sm.normalTexturePath+'\x1f'+sm.roughnessTexturePath+'\x1f'+
        sm.metallicTexturePath+'\x1f'+sm.emissiveTexturePath+'\x1f'+std::to_string(sm.wrapMode)+'|'+
        std::to_string(sm.alphaMode)+'|'+std::to_string(ds || sm.doubleSided);
    for(float v:{sm.metallic,sm.roughness,sm.ao,sm.mrValid,sm.alphaCutoff,sm.diffuseTransmissionFactor})
        k+='|'+std::to_string(std::bit_cast<uint32_t>(v));
    return k;
}
void ReleaseFrameBuffers(Frame& f) {
    f.requests.Cleanup(); f.commands.Cleanup(); f.indices.Cleanup(); f.params.Cleanup();
    f.requestCapacity=f.commandCapacity=0;
    f.residentRequests.clear();f.residentCommands.clear();f.visibilityValid=false;
}
bool EnsureCullPipeline() {
    if(cullPipeline) return true;
    VkDescriptorSetLayoutBinding bindings[10]{};
    for(uint32_t i=0;i<10;++i) bindings[i]={i,i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        (i==6?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=10; li.pBindings=bindings;
    if(!computeLayout && vkCreateDescriptorSetLayout(g_Device,&li,g_Allocator,&computeLayout)!=VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount=1; pi.pSetLayouts=&computeLayout; pi.pushConstantRangeCount=1; pi.pPushConstantRanges=&push;
    if(!cullLayout && vkCreatePipelineLayout(g_Device,&pi,g_Allocator,&cullLayout)!=VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,64},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,512},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,64},{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,32}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets=96; poolInfo.poolSizeCount=4; poolInfo.pPoolSizes=sizes;
    if(!pool && vkCreateDescriptorPool(g_Device,&poolInfo,g_Allocator,&pool)!=VK_SUCCESS) return false;
    const auto path=EngineConfig::GetShaderPath("model_indirect_cull.comp.spv");
    auto* io=SDL_IOFromFile(path.c_str(),"rb"); if(!io) return false;
    const auto bytes=SDL_GetIOSize(io); if(bytes<=0 || bytes%4){SDL_CloseIO(io);return false;}
    std::vector<uint32_t> code(static_cast<size_t>(bytes)/4);
    bool ok=SDL_ReadIO(io,code.data(),static_cast<size_t>(bytes))==static_cast<size_t>(bytes); SDL_CloseIO(io); if(!ok) return false;
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize=static_cast<size_t>(bytes); si.pCode=code.data();
    VkShaderModule shader=VK_NULL_HANDLE; if(vkCreateShaderModule(g_Device,&si,g_Allocator,&shader)!=VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout=cullLayout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=shader; ci.stage.pName="main";
    const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&ci,g_Allocator,&cullPipeline);
    vkDestroyShaderModule(g_Device,shader,g_Allocator); return result==VK_SUCCESS;
}
void UpdateResident(VkCommandBuffer cmd,VulkanBuffer& buffer,std::vector<uint8_t>& old,const void* data,size_t bytes,size_t stride) {
    const auto* input=static_cast<const uint8_t*>(data); const bool resized=old.size()!=bytes;
    for(size_t first=0;first<bytes;) {
        if(!resized && std::memcmp(old.data()+first,input+first,stride)==0){first+=stride;continue;}
        size_t end=first+stride;
        while(end<bytes && end-first+stride<=65536 && (resized || std::memcmp(old.data()+end,input+end,stride)!=0)) end+=stride;
        vkCmdUpdateBuffer(cmd,buffer.GetBuffer(),first,end-first,input+first);
        if(!resized) std::memcpy(old.data()+first,input+first,end-first);
        first=end;
    }
    if(resized) old.assign(input,input+bytes);
}
bool EnsureAtlas(const std::vector<ModelRenderer*>& resources,uint64_t epoch) {
    std::vector<VkBuffer> signature;
    for(auto* r:resources) for(const auto& sm:r->GetSubMeshes()){signature.push_back(sm.vertexBuffer);signature.push_back(sm.indexBuffer);}
    if(signature==atlasSignature && vertices.GetBuffer()) return true;
    // A buffer referenced by commands already recorded this frame cannot be destroyed.
    if(atlasEpoch==epoch && vertices.GetBuffer()) return true;
    vkDeviceWaitIdle(g_Device);
    vertices.Cleanup(); indices.Cleanup(); vertexStaging.Cleanup(); indexStaging.Cleanup(); geometry.clear(); atlasSignature.clear();
    std::vector<Vertex> verts; std::vector<uint32_t> inds;
    for(auto* r:resources) {
        auto& ranges=geometry[r];
        for(const auto& mesh:r->GetMeshData().subMeshes) {
            if(verts.size()+mesh.vertices.size()>INT32_MAX || inds.size()+mesh.indices.size()>UINT32_MAX){geometry.clear();return false;}
            ranges.push_back({static_cast<uint32_t>(inds.size()),static_cast<uint32_t>(mesh.indices.size()),static_cast<int32_t>(verts.size())});
            verts.insert(verts.end(),mesh.vertices.begin(),mesh.vertices.end()); inds.insert(inds.end(),mesh.indices.begin(),mesh.indices.end());
        }
    }
    if(verts.empty() || inds.empty()) return false;
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(!vertices.Create(verts.size()*sizeof(Vertex),VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
       !indices.Create(inds.size()*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
       !vertexStaging.Create(verts.size()*sizeof(Vertex),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host) ||
       !indexStaging.Create(inds.size()*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host)){geometry.clear();return false;}
    vertexStaging.Write(verts.data(),verts.size()*sizeof(Vertex)); indexStaging.Write(inds.data(),inds.size()*4);
    atlasSignature=std::move(signature); atlasPending=true; return true;
}
Frame* CurrentFrame(int slot,uint64_t epoch) {
    auto it=frames.find(GetCurrentFrameIndex()*2+static_cast<uint32_t>(slot));
    return slot>=0 && slot<=1 && it!=frames.end() && it->second->epoch==epoch ? it->second.get():nullptr;
}
}

VkDescriptorSetLayout ModelIndirectRenderer::GetInstanceLayout() {
    if(instanceLayout || !g_Device) return instanceLayout;
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount=1; info.pBindings=&binding;
    if(vkCreateDescriptorSetLayout(g_Device,&info,g_Allocator,&instanceLayout)!=VK_SUCCESS) return VK_NULL_HANDLE;
    return instanceLayout;
}

void ModelIndirectRenderer::Prepare(SceneRenderer& scene,VkCommandBuffer cmd,int width,int height,const glm::mat4& view,const glm::mat4& proj,int slot) {
    if(slot<0 || slot>1 || width<=0 || height<=0) return;
    const auto& world=scene.GetRenderWorld(); const uint64_t epoch=world.frameNumber;
    auto& holder=frames[GetCurrentFrameIndex()*2+static_cast<uint32_t>(slot)]; if(!holder) holder=std::make_unique<Frame>();
    auto& f=*holder; if(f.epoch==epoch) return; f.epoch=UINT64_MAX; f.handled.clear();
    VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(g_PhysicalDevice,&features);
    if(!features.drawIndirectFirstInstance || !EnsureCullPipeline()) return;
    if(collectionEpoch!=epoch) {
        uint64_t key=0x6d6f64656cull;
        for(const auto& group:world.modelGroups) {
            Hash(key,std::hash<std::string>{}(group.rendererKey));Hash(key,std::hash<std::string>{}(group.modelPath));
            auto* pose=scene.GetModelRendererForKey(group.rendererKey);auto* asset=scene.GetModelRenderer(group.modelPath);
            for(auto* r:{pose,asset}){Hash(key,reinterpret_cast<uintptr_t>(r));if(r){Hash(key,r->HasModelLoaded());Hash(key,r->HasDoubleSided());}}
            for(auto entity:group.entities){Hash(key,entity);const auto* d=world.Find(entity);if(d){Hash(key,d->hasTransform);Hash(key,d->hasRenderFlags);Hash(key,d->render.doubleSided);Hash(key,d->render.wireframe);}}
        }
        if(key!=batchSignature || cachedBatches.empty()){cachedBatches=scene.BuildModelBatches();batchSignature=key;}
        std::set<ModelRenderer*> unique;
        for(const auto& b:cachedBatches)if(!b.wireframe && Eligible(b.renderer))unique.insert(b.renderer);
        std::vector<ModelRenderer*> resources(unique.begin(),unique.end());
        if(resources.empty() || !EnsureAtlas(resources,epoch))return;
        std::map<ModelRenderer*,uint64_t> resourceKeys;
        uint64_t signature=key;for(auto* r:resources){const auto revision=ResourceSignature(r);resourceKeys[r]=revision;Hash(signature,revision);}
        const bool rebuild=signature!=templateSignature || batchTemplates.empty();
        if(rebuild) {
            ++topologyVersion;++visibilityVersion;batchTemplates.clear();cachedDraws.clear();commandTemplates.clear();boundsTemplates.clear();
            std::set<std::pair<ModelRenderer*,ECS::Entity>> active;
            for(const auto& b:cachedBatches)if(!b.wireframe && Eligible(b.renderer) && geometry.count(b.renderer))
                for(auto e:b.entities)if(const auto* d=world.Find(e);d && d->hasTransform)active.insert({b.renderer,e});
            for(auto it=nodeSlots.begin();it!=nodeSlots.end();) {
                if(!active.count(it->first)){freeSlots.push_back(it->second);nodes[it->second].renderer=nullptr;it=nodeSlots.erase(it);}else ++it;
            }
            for(const auto& b:cachedBatches) {
                auto* r=b.renderer;if(b.wireframe || !Eligible(r) || !geometry.count(r))continue;
                BatchTemplate batch{r,b.doubleSided};
                for(auto entity:b.entities) {
                    const auto* d=world.Find(entity);if(!d || !d->hasTransform)continue;
                    auto identity=std::make_pair(r,entity);auto it=nodeSlots.find(identity);
                    if(it==nodeSlots.end()) {
                        uint32_t index;
                        if(freeSlots.empty()){index=uint32_t(nodes.size());nodes.emplace_back();}else{index=freeSlots.back();freeSlots.pop_back();nodes[index]=Node{};}
                        it=nodeSlots.emplace(identity,index).first;nodes[index].renderer=r;nodes[index].entity=entity;
                    }
                    batch.slots.push_back(it->second);
                }
                const auto& ranges=geometry.at(r);
                for(size_t sm=0;sm<ranges.size();++sm) {
                    batch.commands.push_back(uint32_t(commandTemplates.size()));const auto& g=ranges[sm];commandTemplates.push_back({g.count,0,g.first,g.vertex,0});
                    cachedDraws.push_back({r,sm,b.doubleSided,MaterialKey(r->GetSubMeshes()[sm],b.doubleSided)});
                    const auto& box=r->GetSubMeshes()[sm].aabb;
                    boundsTemplates.push_back({glm::vec4(box.min,ranges.size()>1?1.0f:0.0f),glm::vec4(box.max,0)});
                }
                batchTemplates.push_back(std::move(batch));
            }
            std::vector<uint32_t> order(cachedDraws.size()),remap(order.size());for(uint32_t i=0;i<order.size();++i)order[i]=i;
            std::stable_sort(order.begin(),order.end(),[](auto a,auto b){return cachedDraws[a].materialKey<cachedDraws[b].materialKey;});
            auto oldDraws=std::move(cachedDraws);auto oldCommands=std::move(commandTemplates);auto oldBounds=std::move(boundsTemplates);
            cachedDraws.clear();commandTemplates.clear();boundsTemplates.clear();
            for(uint32_t i=0;i<order.size();++i){remap[order[i]]=i;cachedDraws.push_back(std::move(oldDraws[order[i]]));commandTemplates.push_back(oldCommands[order[i]]);boundsTemplates.push_back(oldBounds[order[i]]);}
            std::vector<uint32_t> capacities(commandTemplates.size());
            for(auto& batch:batchTemplates)for(auto& command:batch.commands){command=remap[command];capacities[command]=uint32_t(batch.slots.size());}
            uint32_t output=0;for(size_t i=0;i<commandTemplates.size();++i){commandTemplates[i].firstInstance=output;output+=capacities[i];}
            templateSignature=signature;
        }
        // The second viewport consumes the same snapshot, including prevModel.
        for(auto& node:nodes) {
            if(!node.renderer)continue;auto* r=node.renderer;const auto* d=world.Find(node.entity);if(!d)continue;
            const auto model=d->transform.worldMatrix;
            const bool resourceDirty=node.geometryRevision!=resourceKeys[r];
            const bool boundsDirty=resourceDirty || node.version==0 || std::memcmp(&node.source.instance.model,&model,sizeof(model))!=0;
            Source source=node.source;source.instance.model=model;
            const auto prev=scene.m_PrevModelMatrices.find(node.entity);source.instance.prevModel=prev==scene.m_PrevModelMatrices.end()?model:prev->second;
            const auto materialRevision=d->capturedComponentRevisions[static_cast<size_t>(RenderWorldCaptureComponent::Material)];
            if(resourceDirty || node.version==0 || node.materialRevision!=materialRevision || node.hasMaterial!=d->hasMaterial) {
            if(d->hasMaterial) {
                const auto& mat=d->material;source.instance.albedoColor=glm::vec4(mat.albedoColor,1);
                source.instance.materialData=glm::vec4(mat.metallic,mat.roughness,mat.ao,mat.emissiveIntensity);
                source.instance.textureFlags=glm::vec4(mat.useNormalTexture || r->HasNormalTexture(),mat.useAlbedoTexture && r->HasAlbedoTexture(),mat.useEmissiveTexture || r->HasEmissiveTexture(),r->HasRoughnessTexture() || r->HasMetallicTexture());
            }else{source.instance.albedoColor=glm::vec4(1);source.instance.materialData=glm::vec4(-1,-1,-1,0);source.instance.textureFlags=glm::vec4(0);}
            node.materialRevision=materialRevision;node.hasMaterial=d->hasMaterial;
            }
            node.geometryRevision=resourceKeys[r];
            if(boundsDirty) {
                const auto box=r->GetAABB().Transform(model);source.lo=glm::vec4(box.min,0);source.hi=glm::vec4(box.max,0);
                node.subBounds.clear();for(const auto& sm:r->GetSubMeshes())node.subBounds.push_back(sm.aabb.Transform(model));
                node.tlasBounds.clear();
                if(node.subBounds.size()>=16 && r->GetBVHData().HasTopLevelBVH())
                    for(const auto& n:r->GetBVHData().GetTopLevelNodes())node.tlasBounds.push_back(AABB(n.boundsMin,n.boundsMax).Transform(model));
                ++visibilityVersion;
            }
            if(node.version==0 || std::memcmp(&source,&node.source,sizeof(Source))!=0){node.source=source;node.version=++sourceSerial;}
        }
        collectionEpoch=epoch;
    }
    if(batchTemplates.empty() || !vertices.GetBuffer())return;
    if(atlasPending) {
        VkBufferCopy copy{0,0,vertices.GetSize()}; vkCmdCopyBuffer(cmd,vertexStaging.GetBuffer(),vertices.GetBuffer(),1,&copy);
        copy.size=indices.GetSize(); vkCmdCopyBuffer(cmd,indexStaging.GetBuffer(),indices.GetBuffer(),1,&copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        atlasPending=false; atlasEpoch=epoch;
    }
    glm::mat4 mainVP=proj*view; bool mainFrustum=false, subMeshCull=false;
    for(const auto& camera:world.cameras) if(camera.isMainCamera) {
        auto projection=camera.GetProjectionMatrix(float(g_GameRenderTarget.GetWidth())/float(std::max(1u,g_GameRenderTarget.GetHeight())));
        projection[1][1]*=-1; mainVP=projection*camera.GetViewMatrix();
        mainFrustum=camera.enableFrustumCulling; subMeshCull=camera.useSubMeshCulling; break;
    }
    const auto mainPlanes=AABBUtils::ExtractFrustumPlanes(mainVP), rasterPlanes=AABBUtils::ExtractFrustumPlanes(proj*view);
    const auto rasterVP=proj*view;
    const bool rebuildVisibility=!f.visibilityValid || f.topologyVersion!=topologyVersion || f.visibilityVersion!=visibilityVersion ||
        f.cachedMainFrustum!=mainFrustum || f.cachedSubMeshCull!=subMeshCull ||
        (mainFrustum && std::memcmp(&f.cachedMainVP,&mainVP,sizeof(mainVP))!=0) ||
        (slot==0 && std::memcmp(&f.cachedRasterVP,&rasterVP,sizeof(rasterVP))!=0);
    if(f.topologyVersion!=topologyVersion){f.draws=cachedDraws;f.topologyVersion=topologyVersion;}
    for(const auto& batch:batchTemplates)f.handled.insert({batch.renderer,batch.doubleSided});
    if(rebuildVisibility) {
        f.cachedCandidates.clear();f.cachedRequests.clear();
        for(const auto& batch:batchTemplates)for(size_t ordinal=0;ordinal<batch.slots.size();++ordinal) {
            const auto index=batch.slots[ordinal];
            const auto& node=nodes[index];const AABB bounds(glm::vec3(node.source.lo),glm::vec3(node.source.hi));
            if((mainFrustum && !bounds.IsInsideFrustum(mainPlanes)) || (slot==0 && !bounds.IsInsideFrustum(rasterPlanes)))continue;
            f.cachedCandidates.push_back(index);
            if(subMeshCull && !node.tlasBounds.empty() && (mainFrustum || slot==0)) {
                std::vector<size_t> visibleSubMeshes;
                QuerySubMeshes(node,node.renderer->GetBVHData().GetTLASRootIndex(),mainPlanes,rasterPlanes,mainFrustum,slot==0,visibleSubMeshes);
                // Stable order avoids request uploads caused solely by traversal order.
                std::sort(visibleSubMeshes.begin(),visibleSubMeshes.end());
                visibleSubMeshes.erase(std::unique(visibleSubMeshes.begin(),visibleSubMeshes.end()),visibleSubMeshes.end());
                for(auto sm:visibleSubMeshes)if(sm<batch.commands.size())f.cachedRequests.emplace_back(index,batch.commands[sm],commandTemplates[batch.commands[sm]].firstInstance+uint32_t(ordinal),0);
            }else for(size_t sm=0;sm<batch.commands.size();++sm) {
                if(subMeshCull && ((mainFrustum && !node.subBounds[sm].IsInsideFrustum(mainPlanes)) || (slot==0 && !node.subBounds[sm].IsInsideFrustum(rasterPlanes))))continue;
                f.cachedRequests.emplace_back(index,batch.commands[sm],commandTemplates[batch.commands[sm]].firstInstance+uint32_t(ordinal),0);
            }
        }
        f.cachedMainVP=mainVP;f.cachedRasterVP=rasterVP;f.cachedMainFrustum=mainFrustum;f.cachedSubMeshCull=subMeshCull;
        f.visibilityVersion=visibilityVersion;f.visibilityValid=true;
    }
    const auto& requests=f.cachedRequests;const auto& candidates=f.cachedCandidates;const auto& commands=commandTemplates;
    const size_t sourceCount=nodes.size();size_t outputCapacity=0;
    for(const auto& batch:batchTemplates)outputCapacity+=batch.slots.size()*batch.commands.size();
    f.instanceCount=uint32_t(candidates.size());
    if(requests.empty()){f.draws.clear();f.epoch=epoch;return;}
    if(f.draws.empty())f.draws=cachedDraws;
    bool dirty=false;
    VkPhysicalDeviceProperties limits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&limits);
    if(sourceCount>limits.limits.maxTexelBufferElements/14 || sourceCount*sizeof(Source)>limits.limits.maxStorageBufferRange || outputCapacity*16>limits.limits.maxStorageBufferRange || commands.size()*sizeof(SubMeshBounds)>limits.limits.maxStorageBufferRange){f.handled.clear();return;}
    const size_t ns=std::min(std::max(size_t(64),sourceCount*2),std::min(size_t(limits.limits.maxTexelBufferElements/14),size_t(limits.limits.maxStorageBufferRange/sizeof(Source)))), nr=std::min(std::max(size_t(64),outputCapacity*2),size_t(limits.limits.maxStorageBufferRange/16)), nc=std::min(std::max(size_t(64),commands.size()*2),size_t(limits.limits.maxStorageBufferRange/sizeof(SubMeshBounds)));
    if(f.requestCapacity<outputCapacity || f.commandCapacity<commands.size()) {
        vkDeviceWaitIdle(g_Device);ReleaseFrameBuffers(f);
        const auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(!f.requests.Create(nr*16,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !f.commands.Create(nc*20,storage|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !f.indices.Create(nr*4,storage|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !f.params.Create(sizeof(Params),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {f.handled.clear();return;}
        f.requestCapacity=nr;f.commandCapacity=nc;dirty=true;
    }
    VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; transfer.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;transfer.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&transfer,0,nullptr,0,nullptr);
    UpdateResident(cmd,f.requests,f.residentRequests,requests.data(),requests.size()*16,16);
    UpdateResident(cmd,f.commands,f.residentCommands,commands.data(),commands.size()*20,20);
    auto& sharedHolder=sharedFrames[GetCurrentFrameIndex()];if(!sharedHolder)sharedHolder=std::make_unique<SharedCull>();
    auto& shared=*sharedHolder;
    bool sharedDescriptorsDirty=false;
    if(shared.sourceCapacity<sourceCount || shared.requestCapacity<outputCapacity || shared.commandCapacity<commands.size()) {
        vkDeviceWaitIdle(g_Device);
        if(shared.sourceView)vkDestroyBufferView(g_Device,shared.sourceView,g_Allocator);shared.sourceView=VK_NULL_HANDLE;
        shared.sources.Cleanup();shared.subMeshBounds.Cleanup();shared.requests.Cleanup();shared.candidates.Cleanup();shared.visibility.Cleanup();shared.results.Cleanup();shared.params.Cleanup();
        shared.sourceCapacity=shared.requestCapacity=shared.commandCapacity=0;shared.uploadEpoch=UINT64_MAX;shared.epoch=UINT64_MAX;shared.valid=false;
        shared.residentRequests.clear();shared.residentCandidates.clear();shared.residentSourceVersions.clear();shared.residentSubMeshBounds.clear();
        const auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(!shared.sources.Create(ns*sizeof(Source),storage|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.subMeshBounds.Create(nc*sizeof(SubMeshBounds),storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.requests.Create(nr*16,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.candidates.Create(ns*4,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.visibility.Create(ns*4,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.results.Create(nr*4,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
           !shared.params.Create(sizeof(Params),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {f.handled.clear();return;}
        VkBufferViewCreateInfo vi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};vi.buffer=shared.sources.GetBuffer();vi.format=VK_FORMAT_R32G32B32A32_SFLOAT;vi.range=shared.sources.GetSize();
        if(vkCreateBufferView(g_Device,&vi,g_Allocator,&shared.sourceView)!=VK_SUCCESS){f.handled.clear();return;}
        shared.sourceCapacity=ns;shared.requestCapacity=nr;shared.commandCapacity=nc;sharedDescriptorsDirty=true;
    }
    // One snapshot and one dirty upload per in-flight frame; both viewport
    // descriptors refer to this immutable input until that frame's fence retires.
    if(shared.uploadEpoch!=epoch) {
    shared.residentSourceVersions.resize(sourceCount,0);
    // Coalesce contiguous dirty slots into <=64 KiB updates. Unchanged slots
    // and retired identities never trigger an upload or move their neighbours.
    for(size_t i=0;i<sourceCount;) {
        if(!nodes[i].renderer || shared.residentSourceVersions[i]==nodes[i].version){++i;continue;}
        const size_t first=i;shared.sourceUploadScratch.clear();
        while(i<sourceCount && nodes[i].renderer && shared.residentSourceVersions[i]!=nodes[i].version &&
              (shared.sourceUploadScratch.size()+1)*sizeof(Source)<=65536) {
            shared.sourceUploadScratch.push_back(nodes[i].source);shared.residentSourceVersions[i]=nodes[i].version;++i;
        }
        vkCmdUpdateBuffer(cmd,shared.sources.GetBuffer(),first*sizeof(Source),shared.sourceUploadScratch.size()*sizeof(Source),shared.sourceUploadScratch.data());
    }
    UpdateResident(cmd,shared.subMeshBounds,shared.residentSubMeshBounds,boundsTemplates.data(),boundsTemplates.size()*sizeof(SubMeshBounds),sizeof(SubMeshBounds));
        shared.uploadEpoch=epoch;
    }
    const bool computeShared=shared.epoch!=epoch;
    if(computeShared) {
        if(!shared.valid || shared.topologyVersion!=topologyVersion || shared.visibilityVersion!=visibilityVersion ||
           shared.mainFrustum!=mainFrustum || shared.subMeshCull!=subMeshCull ||
           (mainFrustum && std::memcmp(&shared.mainVP,&mainVP,sizeof(mainVP))!=0)) {
            shared.cpuRequests.clear();shared.cpuCandidates.clear();
            for(const auto& batch:batchTemplates)for(size_t ordinal=0;ordinal<batch.slots.size();++ordinal) {
                const auto index=batch.slots[ordinal];const auto& node=nodes[index];
                const AABB bounds(glm::vec3(node.source.lo),glm::vec3(node.source.hi));
                if(mainFrustum && !bounds.IsInsideFrustum(mainPlanes))continue;
                shared.cpuCandidates.push_back(index);
                std::vector<size_t> subMeshes;
                if(subMeshCull && mainFrustum && !node.tlasBounds.empty())
                    QuerySubMeshes(node,node.renderer->GetBVHData().GetTLASRootIndex(),mainPlanes,mainPlanes,true,false,subMeshes);
                else for(size_t sm=0;sm<batch.commands.size();++sm)
                    if(!subMeshCull || !mainFrustum || node.subBounds[sm].IsInsideFrustum(mainPlanes))subMeshes.push_back(sm);
                std::sort(subMeshes.begin(),subMeshes.end());subMeshes.erase(std::unique(subMeshes.begin(),subMeshes.end()),subMeshes.end());
                for(auto sm:subMeshes)if(sm<batch.commands.size())shared.cpuRequests.emplace_back(index,batch.commands[sm],commandTemplates[batch.commands[sm]].firstInstance+uint32_t(ordinal),0);
            }
            shared.topologyVersion=topologyVersion;shared.visibilityVersion=visibilityVersion;shared.mainVP=mainVP;
            shared.mainFrustum=mainFrustum;shared.subMeshCull=subMeshCull;shared.valid=true;
        }
        UpdateResident(cmd,shared.requests,shared.residentRequests,shared.cpuRequests.data(),shared.cpuRequests.size()*16,16);
        UpdateResident(cmd,shared.candidates,shared.residentCandidates,shared.cpuCandidates.data(),shared.cpuCandidates.size()*4,4);
    }
    auto& hiZ=scene.GetHiZShader(); const auto& history=hiZ.GetCullingHistory();
    bool useHiZ=scene.IsGameGrassHiZCullingEnabled() && hiZ.HasValidCullingData() && history.epoch!=UINT64_MAX && history.epoch+1==epoch &&
        HiZHistory::CanReuse(mainVP,history.viewProj,HiZHistory::OccluderRevision(world),history.revision,history.valid);
    Params params{};params.counts=glm::uvec4(candidates.size(),requests.size(),commands.size(),0);
    params.hizViewProj=HiZHistory::WithJitter(history.viewProj,history.jitter);
    params.hizParams=glm::uvec4(g_GameRenderTarget.GetWidth(),g_GameRenderTarget.GetHeight(),useHiZ?hiZ.GetCullingMipLevels():0,useHiZ?1:0);
    f.params.Write(&params,sizeof(params));
    if(computeShared){auto sharedParams=params;sharedParams.counts.x=uint32_t(shared.cpuCandidates.size());sharedParams.counts.y=uint32_t(shared.cpuRequests.size());shared.params.Write(&sharedParams,sizeof(sharedParams));}
    auto allocate=[&](VkDescriptorSetLayout layout,VkDescriptorSet& set) { if(set)return true;VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;dirty=true;return vkAllocateDescriptorSets(g_Device,&ai,&set)==VK_SUCCESS;};
    if(!allocate(computeLayout,f.computeSet)||!allocate(instanceLayout,f.graphicsSet)){f.handled.clear();return;}
    if(!shared.set){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&computeLayout;if(vkAllocateDescriptorSets(g_Device,&ai,&shared.set)!=VK_SUCCESS){f.handled.clear();return;}sharedDescriptorsDirty=true;}
    if(dirty || sharedDescriptorsDirty) {
        VkDescriptorBufferInfo infos[]={{f.params.GetBuffer(),0,sizeof(Params)},{shared.sources.GetBuffer(),0,VK_WHOLE_SIZE},{f.requests.GetBuffer(),0,VK_WHOLE_SIZE},
            {f.commands.GetBuffer(),0,VK_WHOLE_SIZE},{f.indices.GetBuffer(),0,VK_WHOLE_SIZE},{shared.visibility.GetBuffer(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[10]{};
        for(uint32_t i=0;i<6;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=f.computeSet;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}
        writes[6]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[6].dstSet=f.graphicsSet;writes[6].descriptorCount=1;writes[6].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;writes[6].pTexelBufferView=&shared.sourceView;
        VkDescriptorBufferInfo candidateInfo{shared.candidates.GetBuffer(),0,VK_WHOLE_SIZE};
        writes[7]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[7].dstSet=f.computeSet;writes[7].dstBinding=7;writes[7].descriptorCount=1;writes[7].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[7].pBufferInfo=&candidateInfo;
        VkDescriptorBufferInfo boundsInfo{shared.subMeshBounds.GetBuffer(),0,VK_WHOLE_SIZE};
        writes[8]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[8].dstSet=f.computeSet;writes[8].dstBinding=8;writes[8].descriptorCount=1;writes[8].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[8].pBufferInfo=&boundsInfo;
        VkDescriptorBufferInfo resultInfo{shared.results.GetBuffer(),0,VK_WHOLE_SIZE};
        writes[9]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[9].dstSet=f.computeSet;writes[9].dstBinding=9;writes[9].descriptorCount=1;writes[9].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[9].pBufferInfo=&resultInfo;
        vkUpdateDescriptorSets(g_Device,10,writes,0,nullptr);
    }
    // A shared buffer can grow while the other viewport retains its own
    // allocations. Refresh these bindings independently of local resize flags.
    VkDescriptorBufferInfo sharedInfos[]={{shared.visibility.GetBuffer(),0,VK_WHOLE_SIZE},{shared.results.GetBuffer(),0,VK_WHOLE_SIZE},
        {shared.sources.GetBuffer(),0,VK_WHOLE_SIZE},{shared.subMeshBounds.GetBuffer(),0,VK_WHOLE_SIZE},{shared.candidates.GetBuffer(),0,VK_WHOLE_SIZE}};
    const uint32_t sharedBindings[]={5,9,1,8,7};VkWriteDescriptorSet sharedWrites[6]{};
    for(uint32_t i=0;i<5;++i){sharedWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};sharedWrites[i].dstSet=f.computeSet;sharedWrites[i].dstBinding=sharedBindings[i];sharedWrites[i].descriptorCount=1;sharedWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;sharedWrites[i].pBufferInfo=&sharedInfos[i];}
    sharedWrites[5]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};sharedWrites[5].dstSet=f.graphicsSet;sharedWrites[5].descriptorCount=1;sharedWrites[5].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;sharedWrites[5].pTexelBufferView=&shared.sourceView;
    vkUpdateDescriptorSets(g_Device,6,sharedWrites,0,nullptr);
    VkDescriptorImageInfo image{g_GameRenderTarget.GetHiZSampler(),useHiZ?hiZ.GetHiZTextureViewForCulling():g_GameRenderTarget.GetDepthImageView(),
        useHiZ?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    if(!image.sampler || !image.imageView){f.handled.clear();return;}
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=f.computeSet;write.dstBinding=6;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
    vkUpdateDescriptorSets(g_Device,1,&write,0,nullptr);
    if(computeShared) {
        VkDescriptorBufferInfo infos[]={{shared.params.GetBuffer(),0,sizeof(Params)},{shared.sources.GetBuffer(),0,VK_WHOLE_SIZE},
            {shared.requests.GetBuffer(),0,VK_WHOLE_SIZE},{f.commands.GetBuffer(),0,VK_WHOLE_SIZE},{f.indices.GetBuffer(),0,VK_WHOLE_SIZE},
            {shared.visibility.GetBuffer(),0,VK_WHOLE_SIZE},{shared.candidates.GetBuffer(),0,VK_WHOLE_SIZE},
            {shared.subMeshBounds.GetBuffer(),0,VK_WHOLE_SIZE},{shared.results.GetBuffer(),0,VK_WHOLE_SIZE}};
        const uint32_t bindings[]={0,1,2,3,4,5,7,8,9};VkWriteDescriptorSet writes[10]{};
        for(uint32_t i=0;i<9;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=shared.set;writes[i].dstBinding=bindings[i];writes[i].descriptorCount=1;writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}
        writes[9]=write;writes[9].dstSet=shared.set;vkUpdateDescriptorSets(g_Device,10,writes,0,nullptr);
    }
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cullPipeline);
    if(computeShared) {
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cullLayout,0,1,&shared.set,0,nullptr);
        uint32_t phase=0;vkCmdPushConstants(cmd,cullLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);
        if(!shared.cpuCandidates.empty())vkCmdDispatch(cmd,(uint32_t(shared.cpuCandidates.size())+63)/64,1,1);
        ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        phase=3;vkCmdPushConstants(cmd,cullLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);
        if(!shared.cpuRequests.empty())vkCmdDispatch(cmd,(uint32_t(shared.cpuRequests.size())+63)/64,1,1);
        shared.epoch=epoch;
    }
    // Also supplies the dependency when the shared producer was recorded by the
    // other viewport on the same graphics queue earlier in this logical frame.
    ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cullLayout,0,1,&f.computeSet,0,nullptr);
    uint32_t phase=1;vkCmdPushConstants(cmd,cullLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);vkCmdDispatch(cmd,(params.counts.z+63)/64,1,1);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    phase=2;vkCmdPushConstants(cmd,cullLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);vkCmdDispatch(cmd,(params.counts.y+63)/64,1,1);
    ready.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&ready,0,nullptr,0,nullptr);
    f.epoch=epoch; atlasEpoch=epoch;
}

bool ModelIndirectRenderer::Handles(ModelRenderer* r,bool ds,int slot,uint64_t epoch) {
    const auto* f=CurrentFrame(slot,epoch);return f && f->handled.count({r,ds});
}
void ModelIndirectRenderer::Render(SceneRenderer& scene,RenderFrameContext& ctx) {
    auto* f=CurrentFrame(ctx.viewSlot,scene.GetRenderWorld().frameNumber);if(!f || f->draws.empty())return;
    const auto cmd=ctx.commandBuffer;
    VkViewport viewport{0,0,float(ctx.width),float(ctx.height),0,1};VkRect2D scissor{{0,0},{uint32_t(ctx.width),uint32_t(ctx.height)}};
    vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    VkBuffer buffers[]={vertices.GetBuffer(),f->indices.GetBuffer()};VkDeviceSize offsets[]={0,0};vkCmdBindVertexBuffers(cmd,0,2,buffers,offsets);
    vkCmdBindIndexBuffer(cmd,indices.GetBuffer(),0,VK_INDEX_TYPE_UINT32);
    ModelUniformData push{};push.projView=ctx.projView;push.prevProjView=ctx.prevProjView;push.cameraPosition=ctx.cameraPos;push.taaJitter=g_CurrentTAAJitter;
    VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(g_PhysicalDevice,&features);VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    const uint32_t limit=features.multiDrawIndirect?std::max(1u,properties.limits.maxDrawIndirectCount):1u;
    for(uint32_t first=0;first<f->draws.size();) {
        const auto& draw=f->draws[first];auto* r=draw.renderer;const auto layout=r->GetIndirectLayout(draw.doubleSided);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,r->GetIndirectPipeline(draw.doubleSided));
        const auto& sm=r->GetSubMeshes()[draw.subMesh];VkDescriptorSet sets[]={sm.descriptorSet,f->graphicsSet};
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,2,sets,1,&g_BoneDynamicOffset);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);PushSubMeshMaterialParams(cmd,layout,sm,draw.doubleSided);
        uint32_t end=first+1;while(end<f->draws.size() && f->draws[end].materialKey==draw.materialKey && f->draws[end].doubleSided==draw.doubleSided && end-first<limit)++end;
        vkCmdDrawIndexedIndirect(cmd,f->commands.GetBuffer(),VkDeviceSize(first)*20,end-first,20);first=end;
    }
    if(!scene.m_SuppressViewHistory) Rendering::RenderStats::Get().AddModelInstances(f->instanceCount);
}
void ModelIndirectRenderer::Cleanup() {
    if(!g_Device) return;
    for(auto& [key,f]:frames) ReleaseFrameBuffers(*f);
    nodes.clear();nodeSlots.clear();freeSlots.clear();cachedBatches.clear();batchTemplates.clear();cachedDraws.clear();commandTemplates.clear();boundsTemplates.clear();
    collectionEpoch=UINT64_MAX;batchSignature=templateSignature=0;topologyVersion=visibilityVersion=sourceSerial=1;
    for(auto& [key,c]:sharedFrames){if(c->sourceView)vkDestroyBufferView(g_Device,c->sourceView,g_Allocator);c->sources.Cleanup();c->subMeshBounds.Cleanup();c->requests.Cleanup();c->candidates.Cleanup();c->visibility.Cleanup();c->results.Cleanup();c->params.Cleanup();}
    sharedFrames.clear();
    frames.clear();vertices.Cleanup();indices.Cleanup();vertexStaging.Cleanup();indexStaging.Cleanup();geometry.clear();atlasSignature.clear();atlasPending=false;atlasEpoch=UINT64_MAX;
    if(cullPipeline)vkDestroyPipeline(g_Device,cullPipeline,g_Allocator);
    if(cullLayout)vkDestroyPipelineLayout(g_Device,cullLayout,g_Allocator);
    if(pool)vkDestroyDescriptorPool(g_Device,pool,g_Allocator);
    if(computeLayout)vkDestroyDescriptorSetLayout(g_Device,computeLayout,g_Allocator);
    if(instanceLayout)vkDestroyDescriptorSetLayout(g_Device,instanceLayout,g_Allocator);
    cullPipeline=VK_NULL_HANDLE;cullLayout=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;computeLayout=instanceLayout=VK_NULL_HANDLE;
}
