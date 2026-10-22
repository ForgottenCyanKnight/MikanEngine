#include "Core/CpuStageTrace.h"
#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/ModelRenderer.h"
#include "../ModelRendererInternals.h"
#include "Rendering/TexturePool.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>
#include <cstring>
#include <bit>
#include "Rendering/VoxelDDARegistry.h"
#include <cmath>
#include <unordered_set>
#include <filesystem>
#include <cstdlib>
bool RayTracingScene::SetEnabled(bool value){enabled=value && GetRayTracingDeviceCapabilities().rayQuery;return enabled;}
void RayTracingScene::Cleanup(){frames.clear();assets.clear();sharedVoxBlas.clear();quadDags.clear();modelAssets.clear();deformedPoses.clear();converter.Cleanup();enabled=false;modelCompactionSettled=false;geometryEpoch=0;}
VkAccelerationStructureKHR RayTracingScene::GetTlas(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return enabled && it!=frames.end() && it->second->serial==serial && it->second->ready?it->second->tlas.Handle():VK_NULL_HANDLE;
}
const std::vector<RayTracingHitInstance>* RayTracingScene::GetHitInstances(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return GetTlas(slot,serial)?&it->second->hitInstances:nullptr;
}
uint64_t RayTracingScene::GetHitRevision(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return GetTlas(slot,serial)?it->second->hitRevision:0;
}
const VulkanBuffer* RayTracingScene::GetDdaGrid(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return GetTlas(slot,serial)&&it->second->ddaGrid.GetBuffer()?&it->second->ddaGrid:nullptr;
}
void RayTracingScene::Prepare(VkCommandBuffer cmd,const RenderWorld& world,
    const std::unordered_map<std::string,std::unique_ptr<VoxRenderer>>& renderers,
    const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,uint32_t slot,uint64_t serial) {
    Core::CpuStageTrace cpuStageTrace("rt.scene_prepare");
    if(!enabled || !cmd)return;
    auto& pointer=frames[slot];if(!pointer)pointer=std::make_unique<Frame>();auto& frame=*pointer;
    // Model textures register asynchronously on the raster side. Snapshot the
    // pool generation BEFORE collecting hits; if any texture registers during
    // or after this build, the next Prepare re-collects hits so newly arrived
    // albedo/MR/emissive views reach the hit table (otherwise bindings freeze
    // at whatever happened to be loaded at first build time).
    const uint32_t textureGen=g_textureLoadGeneration.load(std::memory_order_relaxed);
    if(frame.serial==serial && frame.textureGen==textureGen)return;
    // This frame slot's fence covers every serialization copy recorded with its serial.
    for(const auto& as:frame.blasGenerations)if(as)as->CompleteDiskCacheAfterFence(frame.serial);
    // This slot's fence has completed: it is now safe to release build inputs and old references.
    for(auto& geometry:frame.builds){
        geometry->ReleaseBuildInputs();
        const char* release=std::getenv("MIKAN_VOX_RELEASE_BUILD_BUFFER");
        if(!release||release[0]!='0')geometry->ReleaseSceneBuildBuffer();
    }frame.builds.clear();
    for(auto& geometry:frame.modelBuilds)geometry->ReleaseBuildInputs();frame.modelBuilds.clear();
    frame.tlas.ReleaseScratch();
    const char* memoryStats=std::getenv("MIKAN_VOX_MEMORY_STATS");
    if(memoryStats&&memoryStats[0]=='1'&&serial%120==0){
        std::unordered_set<const AccelerationStructure*> unique;
        std::unordered_set<const VoxRayTracingGeometry*> surfaces;
        VkDeviceSize blasBytes=0,scratchBytes=0,tlasBytes=0,inputBytes=0,instanceBytes=0,ddaBytes=0;
        auto addBlas=[&](const std::shared_ptr<AccelerationStructure>& as){
            if(as&&as->Handle()&&unique.insert(as.get()).second){blasBytes+=as->StorageBytes();scratchBytes+=as->ScratchBytes();}
        };
        auto addSurface=[&](const std::shared_ptr<VoxRayTracingGeometry>& g){
            if(g&&surfaces.insert(g.get()).second){inputBytes+=g->ResidentInputBytes();addBlas(g->Blas());addBlas(g->BrickBlas());}
        };
        for(const auto& [key,g]:assets)addSurface(g);
        for(const auto& [key,f]:frames){
            tlasBytes+=f->tlas.StorageBytes();scratchBytes+=f->tlas.ScratchBytes();
            instanceBytes+=f->instances.GetSize();ddaBytes+=f->ddaGrid.GetSize();
            for(const auto& as:f->blasGenerations)addBlas(as);
            for(const auto& hit:f->hitInstances)addSurface(hit.geometry);
        }
        LOGI("[VOX resident memory] serial=%llu surfaces=%zu uniqueAS=%zu blas=%llu tlas=%llu scratch=%llu inputs=%llu instances=%llu dda=%llu; buffer sizes, includes retained AS generations",
            (unsigned long long)serial,surfaces.size(),unique.size(),(unsigned long long)blasBytes,(unsigned long long)tlasBytes,
            (unsigned long long)scratchBytes,(unsigned long long)inputBytes,(unsigned long long)instanceBytes,(unsigned long long)ddaBytes);
    }
    // Compare entity/asset inputs before entering the per-submesh collector.
    // Camera/light changes do not affect the geometry or material hit table.
    // Static VOX+DDA scenes can reuse the per-slot hit table, TLAS, and DDA
    // records; renderer revisions and the registry version below invalidate
    // the cache when source surfaces, grids, or palettes change.
    const bool ddaOn=mikan::render::VoxelDDARegistry::BootEnabled();
    const auto& ddaRegistry=mikan::render::VoxelDDARegistry::Get();
    bool staticCacheEligible=world.voxGroups.empty() && assets.empty() && !ddaOn;
    if(ddaOn && !world.voxGroups.empty())
        staticCacheEligible=world.modelGroups.empty(); // Keep this fast path scoped to VOX+DDA scenes.
    std::string staticInputKey;
    auto append=[&](const auto& value){staticInputKey.append(reinterpret_cast<const char*>(&value),sizeof(value));};
    auto appendString=[&](const std::string& value){append(value.size());staticInputKey.append(value);};
    append(textureGen);
    append(ModelRendererDetail::MaterialBindingGeneration());
    append(world.entitySetVersion);
    if(ddaOn){
        append(ddaRegistry.Version());
        append(world.voxGroups.size());
        for(const auto& group:world.voxGroups){
            appendString(group.voxPath);
            append(group.entities.size());
            for(const auto entity:group.entities)append(entity);
            const auto found=renderers.find(group.voxPath);
            const auto* renderer=found==renderers.end()?nullptr:found->second.get();
            append(renderer);
            const bool loaded=renderer&&renderer->HasLoaded();append(loaded);
            if(loaded){append(renderer->GetGeometryRevision());append(renderer->GetDdaGridId());}
            else staticCacheEligible=false;
        }
    }
    for(const auto& entity:world.entities){
        if(!entity.hasMesh && !entity.hasVoxel)continue;
        append(entity.entity);append(entity.visible);append(entity.hasTransform);
        staticInputKey.append(reinterpret_cast<const char*>(&entity.transform.worldMatrix),sizeof(glm::mat4));
        for(const auto component:{RenderWorldCaptureComponent::Mesh,RenderWorldCaptureComponent::Material,
                                 RenderWorldCaptureComponent::RenderFlags,RenderWorldCaptureComponent::Voxel}){
            const auto index=static_cast<size_t>(component);
            append(entity.capturedComponentPresence[index]);append(entity.capturedComponentRevisions[index]);
        }
    }
    for(const auto& group:world.modelGroups){
        appendString(group.rendererKey);appendString(group.modelPath);
        const auto& path=group.modelPath.empty()?group.rendererKey:group.modelPath;
        const auto found=models.find(path);
        const auto poseFound=models.find(group.rendererKey);
        const auto* renderer=found==models.end()?(poseFound==models.end()?nullptr:poseFound->second.get()):found->second.get();
        append(renderer);
        if(renderer){
            const auto& mesh=renderer->GetMeshData();
            append(mesh.subMeshes.data());append(mesh.subMeshes.size());
            if(!mesh.animations.empty() || !mesh.bones.empty() || mesh.isMmd)staticCacheEligible=false;
        }
    }
    if(staticCacheEligible && modelCompactionSettled && frame.ready && frame.tlas.Handle() &&
       frame.geometryEpoch==geometryEpoch && frame.staticInputKey==staticInputKey){
        Core::CpuStageTrace cacheTrace("rt.scene_cache_hit");
        frame.serial=serial;
        // Keep exact BLAS generation references alive for this cached TLAS.
        RayTracingQueryBarrier(cmd);
        return;
    }
    frame.dynamicBoundsChanged=false;
    frame.hitRevision=++nextHitRevision;
    frame.hitInstances.clear();frame.blasGenerations.clear();frame.ready=false;frame.serial=serial;frame.textureGen=textureGen;
    static const VkDeviceSize compactBudget=[](){const char* value=std::getenv("MIKAN_VOX_BLAS_COMPACT_BUDGET_MB");
        return VkDeviceSize(value?std::clamp(std::strtoul(value,nullptr,10),1ul,1024ul):64ul)*1024u*1024u;}();
    VkDeviceSize remainingCompactBudget=compactBudget;uint32_t compactCopies=0,cacheRecordings=0;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::unordered_set<std::string> liveAssets;
    std::unordered_set<const VoxRenderer*> liveDagSources;
    const auto& caps=GetRayTracingDeviceCapabilities();
    const glm::mat4 flipZ=glm::scale(glm::mat4(1),glm::vec3(1,1,-1));
    bool failed=false;
    auto groups=world.voxGroups;
    RenderVoxGroup cubes;cubes.voxPath="builtin:rt-unit-cube";
    const auto engineCube=std::filesystem::path(ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb")).lexically_normal();
    for(const auto& entity:world.entities)if(entity.hasMesh && !entity.hasVoxel &&
        (entity.mesh.type==RenderMeshType::Cube || (entity.mesh.type==RenderMeshType::Model && std::filesystem::path(entity.mesh.modelPath).lexically_normal()==engineCube)))cubes.entities.push_back(entity.entity);
    if(!cubes.entities.empty())groups.push_back(std::move(cubes));

    const auto& ddaGrids=ddaRegistry.Grids();
    const bool brickOn=mikan::render::VoxelDDARegistry::BrickEnabled();
    std::vector<uint32_t> ddaPaletteOffsets(ddaGrids.size(),0);
    std::vector<glm::vec4> ddaPalette;
    if(ddaOn)for(size_t g=0;g<ddaGrids.size();++g){
        ddaPaletteOffsets[g]=uint32_t(ddaPalette.size());
        ddaPalette.insert(ddaPalette.end(),ddaGrids[g].palette.begin(),ddaGrids[g].palette.end());
    }
    std::vector<uint32_t> ddaRecords;
    std::vector<std::pair<uint32_t,const std::vector<uint32_t>*>> brickPayloads;
    std::unordered_set<const VoxRenderer*> selectedDagSources;
    uint32_t remainingDagOwners=0;
    if(const char* request=std::getenv("MIKAN_VOX_QUAD_DAG");request&&request[0]=='1'){
        std::unordered_set<const VoxRenderer*> sources;
        for(const auto& group:groups){const auto it=renderers.find(group.voxPath);if(it!=renderers.end()&&it->second)it->second->VisitSurfaces([&](const auto& mesh,const auto&){sources.insert(&mesh);});}
        remainingDagOwners=sources.size()<480u?480u-uint32_t(sources.size()):0u;
    }
    for(const auto& group:groups) {
        const auto found=renderers.find(group.voxPath);if(found==renderers.end() || !found->second || !found->second->HasLoaded()){failed=true;continue;}
        struct Surface {const VoxRenderer* renderer;glm::mat4 transform;const mikan::rt::VoxDagSurface* subset=nullptr;};
        std::vector<Surface> surfaces;
        found->second->VisitSurfaces([&](const VoxRenderer& mesh,const glm::mat4& transform){
            const bool firstSource=liveDagSources.insert(&mesh).second;
            const char* request=std::getenv("MIKAN_VOX_QUAD_DAG");const char* quadAabb=std::getenv("MIKAN_VOX_HW_QUAD_AABB");
            const char* shareOption=std::getenv("MIKAN_VOX_SHARED_BLAS");
            const char* software=std::getenv("MIKAN_HWRT_SOFTWARE_QUAD_BVH");
            const bool dag=request&&request[0]=='1'&&!(software&&software[0]=='1')&&(!shareOption||shareOption[0]!='0')&&!mikan::render::VoxelDDARegistry::BootEnabled()&&!(quadAabb&&quadAabb[0]=='1');
            if(dag&&mesh.GetQuads().size()>=128){
                const char* option=std::getenv("MIKAN_VOX_QUAD_DAG_EDGE");const uint32_t edge=option?uint32_t(std::atoi(option)):9u;
                auto& cache=quadDags[&mesh];
                if(cache.revision!=mesh.GetGeometryRevision()||cache.edge!=edge){
                    cache.revision=mesh.GetGeometryRevision();cache.edge=edge;cache.data={};
                    try{
                        std::vector<mikan::rt::DagQuad> input;input.reserve(mesh.GetQuads().size());
                        for(const auto& p:mesh.GetPlaneRanges())for(uint32_t q=p.firstQuad;q<p.firstQuad+p.quadCount;++q){
                            const auto d=mesh.DecodeQuad(q);input.push_back({d.first&255u,(d.first>>8)&255u,(d.first>>16)&255u,(d.first>>24)+1u,(d.second&255u)+1u,p.planeDirection>>16,q});
                        }
                        std::vector<mikan::rt::DagQuad> eligible;eligible.reserve(input.size());
                        for(const auto& q:input){const uint32_t u=q.face<2?q.x:(q.face<4?q.z:q.x),v=q.face<4?q.y:q.z;
                            if(edge&&(u%edge)+q.width<=edge&&(v%edge)+q.height<=edge)eligible.push_back(q);
                        }
                        cache.data=mikan::rt::CompileVoxQuadDag(eligible,edge);
                        const char* budget=std::getenv("MIKAN_VOX_QUAD_DAG_MAX_INSTANCES");
                        const uint32_t maxInstances=budget?uint32_t(std::clamp(std::atoi(budget),1,4096)):256u;
                        mikan::rt::SelectVoxDagCut(cache.data,input,maxInstances);
                        const bool accept=cache.data.uniqueQuads*100<input.size()*95&&cache.data.uniqueBlocks<=128&&cache.data.surfaces.size()<=4096;
                        LOGI("[VOX quad DAG] edge=%u quads=%zu nodes=%zu blocks=%zu uniqueBlocks=%u uniqueQuads=%zu selected=%u; whole quads, no cuts",
                            edge,input.size(),cache.data.nodes.size(),cache.data.surfaces.size(),cache.data.uniqueBlocks,cache.data.uniqueQuads,uint32_t(accept));
                        if(!accept)cache.data={};
                    }catch(const std::exception& e){LOGW("[VOX quad DAG] fallback: %s",e.what());cache.data={};}
                }
                const uint32_t extra=cache.data.uniqueBlocks?cache.data.uniqueBlocks-1u:0u;
                const bool budgetFits=selectedDagSources.contains(&mesh)||(firstSource&&extra<=remainingDagOwners);
                if(!cache.data.surfaces.empty()&&budgetFits){
                    if(firstSource){remainingDagOwners-=extra;selectedDagSources.insert(&mesh);}
                    for(const auto& part:cache.data.surfaces){const glm::vec3 offset(part.origin[0],part.origin[1],part.origin[2]);
                        surfaces.push_back({&mesh,transform*glm::translate(glm::mat4(1),offset*mesh.GetVoxelSize()),&part});}
                    return;
                }
            }
            surfaces.push_back({&mesh,transform,nullptr});
        });
        uint32_t componentIndex=0;
        for(const auto& [surface,componentTransform,subset]:surfaces){
        const auto component=componentIndex++;
        const auto& renderer=*surface;
        const auto assetKey=group.voxPath+"#"+std::to_string(reinterpret_cast<uintptr_t>(surface))+(subset?("#dag"+std::to_string(subset->node)+":"+std::to_string(subset->origin[0])+","+std::to_string(subset->origin[1])+","+std::to_string(subset->origin[2])):"");
        liveAssets.insert(assetKey);
        auto& geometry=assets[assetKey];
        if(!geometry || geometry->Source()!=&renderer || geometry->Revision()!=renderer.GetGeometryRevision()) {
            auto replacement=std::make_shared<VoxRayTracingGeometry>();
            // Share the immutable hardware triangle BLAS, not the surface's
            // attributes, primitive mapping or spatial instance identity.
            static const bool shareRequested=[](){const char* v=std::getenv("MIKAN_VOX_SHARED_BLAS");return !v||v[0]!='0';}();
            const char* quadAabb=std::getenv("MIKAN_VOX_HW_QUAD_AABB");
            const bool share=shareRequested&&!mikan::render::VoxelDDARegistry::BootEnabled()&&!(quadAabb&&quadAabb[0]=='1');
            mikan::rt::VoxGeometryKey key;
            bool reused=false;
            if(share){
                key=VoxRayTracingGeometry::MakeGeometryKey(renderer,subset);
                auto& bucket=sharedVoxBlas[key.hash];
                for(auto it=bucket.begin();it!=bucket.end();){
                    auto owner=it->lock();
                    if(!owner){it=bucket.erase(it);continue;}
                    if(owner->GeometryKey()==key){reused=replacement->RecordShared(renderer,owner,subset);break;}
                    ++it;
                }
            }
            // Keep even a failed build alive until this slot's fence: compute may already be recorded.
            frame.builds.push_back(replacement);
            if(!reused&&!replacement->RecordBuild(cmd,renderer,converter,std::move(key),subset)){LOGE("[HardwareRT] vox BLAS build failed: %s",group.voxPath.c_str());failed=true;continue;}
            if(share&&!reused)sharedVoxBlas[replacement->GeometryKey().hash].push_back(replacement);
            geometry=std::move(replacement);
        }
        if(compactCopies<2){std::shared_ptr<AccelerationStructure> retired;
            if(geometry->RecordCompaction(cmd,remainingCompactBudget,retired)){
                frame.blasGenerations.push_back(std::move(retired));++compactCopies;
            }
        }
        frame.blasGenerations.push_back(geometry->Blas());
        if(cacheRecordings<1&&geometry->CompactionFinished()&&geometry->Blas()->RecordSaveDiskCache(cmd,serial))++cacheRecordings;
        if(geometry->BrickAddress())frame.blasGenerations.push_back(geometry->BrickBlas());
        for(const auto entity:group.entities) {
            const auto* data=world.Find(entity);if(!data || !data->visible || !data->hasTransform || (data->hasRenderFlags && !data->render.visible))continue;
            // Built-in cube.glb's POSITION accessor spans [-1,1], unlike the unit quad cube [-.5,.5].
            const auto local=data->hasVoxel?flipZ:(data->mesh.type==RenderMeshType::Model?glm::scale(glm::mat4(1),glm::vec3(2)):glm::mat4(1));
            const auto model=data->transform.worldMatrix*local*componentTransform;
            const float determinant=glm::determinant(glm::mat3(model));if(!std::isfinite(determinant) || std::abs(determinant)<1e-8f)continue;
            if(instances.size()>=caps.limits.maxInstanceCount || instances.size()>=0x1000000u){failed=true;break;}
            VkAccelerationStructureInstanceKHR instance{};
            for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)instance.transform.matrix[row][col]=model[col][row];
            instance.instanceCustomIndex=uint32_t(frame.hitInstances.size());
            // bit 0: general rays; bit 1: shadow rays. Raster camera culling never enters this list.
            const uint32_t gridId=renderer.GetDdaGridId();
            const bool ddaOwned=ddaOn&&gridId<mikan::render::kVoxelDdaMaxGrids&&gridId<ddaGrids.size()&&ddaGrids[gridId].imageView&&(!brickOn||geometry->BrickAddress());
            instance.mask=ddaOwned?4u:(1u|((!data->hasRenderFlags||data->render.castShadow)?2u:0u));            instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference=geometry->Address();
            if(!ddaOwned)instances.push_back(instance);
            else if(!brickOn){
                // Software DDA owns visibility. A masked AABB keeps the TLAS
                // valid in voxel-only scenes without a triangle BLAS.
                auto placeholder=instance;placeholder.mask=0;
                const auto gridModel=model*ddaGrids[gridId].gridToLocal;
                for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)placeholder.transform.matrix[row][col]=gridModel[col][row];
                placeholder.accelerationStructureReference=geometry->BrickAddress();instances.push_back(placeholder);
            }
            // bit 0: ideal mirror; bit 1 + bits[31:16] half: entity-level emissive.
            const float entityEmissive=data->hasMaterial?data->material.emissiveIntensity:0.0f;
            uint32_t materialFlags=data->hasMaterial && data->material.metallic>=.999f && data->material.roughness<=.001f?1u:0u;
            if(entityEmissive>0.0f)materialFlags|=2u|(uint32_t(glm::packHalf1x16(entityEmissive))<<16);
            const uint32_t hitIndex=uint32_t(frame.hitInstances.size());
            frame.hitInstances.push_back({uint32_t(entity),model,data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1),geometry,materialFlags});
            frame.hitInstances.back().component=component;
            frame.hitInstances.back().rayMask=1u|((!data->hasRenderFlags||data->render.castShadow)?2u:0u);
            if(ddaOwned){
                const auto& grid=ddaGrids[gridId];const glm::mat4 worldToGrid=glm::inverse(model*grid.gridToLocal);
                const size_t base=ddaRecords.size();ddaRecords.resize(base+24,0);
                std::memcpy(ddaRecords.data()+base,&worldToGrid,64);
                ddaRecords[base+16]=grid.sizeX;ddaRecords[base+17]=grid.sizeY;ddaRecords[base+18]=grid.sizeZ;ddaRecords[base+19]=gridId;
                ddaRecords[base+20]=ddaPaletteOffsets[gridId];ddaRecords[base+21]=hitIndex;ddaRecords[base+22]=(!data->hasRenderFlags||data->render.castShadow)?2u:0u;
                if(brickOn){
                    if(instances.size()>=caps.limits.maxInstanceCount){failed=true;break;}
                    auto brickInstance=instance;brickInstance.mask=frame.hitInstances.back().rayMask;
                    brickInstance.instanceCustomIndex=uint32_t(base/24);
                    const auto worldFromGrid=model*grid.gridToLocal;
                    for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)brickInstance.transform.matrix[row][col]=worldFromGrid[col][row];
                    brickInstance.accelerationStructureReference=geometry->BrickAddress();instances.push_back(brickInstance);
                    brickPayloads.push_back({uint32_t(base/24),&geometry->BrickWords()});
                }
            }
        }
    }
        } // Component instances share the source mesh BLAS.
    for(auto it=assets.begin();it!=assets.end();)if(!liveAssets.contains(it->first))it=assets.erase(it);else ++it;
    std::erase_if(quadDags,[&](const auto& entry){return !liveDagSources.contains(entry.first);});
    // Weak intern entries must not accumulate after asset unload/hot reload.
    for(auto it=sharedVoxBlas.begin();it!=sharedVoxBlas.end();){
        auto& bucket=it->second;
        std::erase_if(bucket,[](const auto& weak){return weak.expired();});
        if(bucket.empty())it=sharedVoxBlas.erase(it);else ++it;
    }
    if(!AppendModels(cmd,world,models,frame,instances,remainingCompactBudget,compactCopies))failed=true;
    if(compactCopies)++geometryEpoch;
    modelCompactionSettled=true;
    for(const auto& [path,parts]:modelAssets)for(const auto& part:parts)
        if(part && !part->CompactionFinished())modelCompactionSettled=false;
    auto cacheInputs=[&](){
        frame.staticInputKey=staticCacheEligible?staticInputKey:std::string{};
        frame.geometryEpoch=geometryEpoch;
    };
    // Refresh grid instances even when the TLAS update takes the cached fast path.
    std::vector<uint32_t> ddaWords(16,0);ddaWords[0]=uint32_t(ddaRecords.size()/24);
    ddaWords.insert(ddaWords.end(),ddaRecords.begin(),ddaRecords.end());
    const size_t paletteBase=ddaWords.size();ddaWords.resize(paletteBase+ddaPalette.size()*4);
    if(!ddaPalette.empty())std::memcpy(ddaWords.data()+paletteBase,ddaPalette.data(),ddaPalette.size()*sizeof(glm::vec4));
    for(const auto& [record,words]:brickPayloads){
        ddaWords[16+record*24+23]=uint32_t(ddaWords.size());ddaWords.insert(ddaWords.end(),words->begin(),words->end());
    }
    if(ddaOn&&mikan::render::VoxelDDARegistry::BitsetEnabled()){
        std::unordered_map<uint32_t,uint32_t> mipOffsets;
        for(uint32_t record=0;record<ddaWords[0];++record){
            const uint32_t b=16+record*24,gridId=ddaWords[b+19];
            auto [it,inserted]=mipOffsets.emplace(gridId,uint32_t(ddaWords.size()));
            if(inserted){const auto& mips=ddaGrids[gridId].bitsetMips;
                if(mips.empty()){LOGE("[HardwareRT DDA] missing occupancy payload for grid %u",gridId);return;}
                ddaWords.insert(ddaWords.end(),mips.begin(),mips.end());
            }
            ddaWords[b+23]=it->second;
        }
    }
    // Top-level broadphase: world-space AABBs (8 words each) per record and per
    // 16-record chunk, appended after all other payloads. The shader rejects
    // whole chunks and records with one slab test before touching the 24-word
    // record or the worldToGrid matrix. Header words 13/14/15 hold the record
    // bounds base, chunk bounds base and chunk count; a 0 record base means the
    // section is absent and the shader falls back to the linear scan. Words
    // 1..3 stay reserved for the bitset-mip path.
    if(ddaOn&&ddaWords[0]>0u){
        const uint32_t recordCount=ddaWords[0];
        const uint32_t boundsBase=uint32_t(ddaWords.size());
        ddaWords.resize(size_t(boundsBase)+size_t(recordCount)*8u,0);
        for(uint32_t record=0;record<recordCount;++record){
            const uint32_t b=16+record*24;
            glm::mat4 worldToGrid;
            for(uint32_t i=0;i<16;++i)worldToGrid[i>>2][i&3]=std::bit_cast<float>(ddaWords[size_t(b)+i]);
            const glm::mat4 worldFromGrid=glm::inverse(worldToGrid);
            const glm::vec3 dims(static_cast<float>(ddaWords[b+16]),static_cast<float>(ddaWords[b+17]),static_cast<float>(ddaWords[b+18]));
            glm::vec3 minimum(1e30f),maximum(-1e30f);
            for(uint32_t corner=0;corner<8;++corner){
                const glm::vec4 p=worldFromGrid*glm::vec4((corner&1u)!=0u?dims.x:0.f,(corner&2u)!=0u?dims.y:0.f,(corner&4u)!=0u?dims.z:0.f,1.f);
                minimum=glm::min(minimum,glm::vec3(p)/p.w);maximum=glm::max(maximum,glm::vec3(p)/p.w);
            }
            for(uint32_t a=0;a<3;++a)ddaWords[size_t(boundsBase)+size_t(record)*8u+a]=std::bit_cast<uint32_t>(minimum[a]);
            for(uint32_t a=0;a<3;++a)ddaWords[size_t(boundsBase)+size_t(record)*8u+3u+a]=std::bit_cast<uint32_t>(maximum[a]);
        }
        const uint32_t chunkCount=(recordCount+15u)/16u;
        const uint32_t chunkBase=boundsBase+recordCount*8u;
        ddaWords.resize(size_t(chunkBase)+size_t(chunkCount)*8u,0);
        for(uint32_t chunk=0;chunk<chunkCount;++chunk){
            glm::vec3 minimum(1e30f),maximum(-1e30f);
            const uint32_t last=glm::min(chunk*16u+16u,recordCount);
            for(uint32_t record=chunk*16u;record<last;++record)
                for(uint32_t a=0;a<3;++a){
                    minimum[a]=glm::min(minimum[a],std::bit_cast<float>(ddaWords[size_t(boundsBase)+size_t(record)*8u+a]));
                    maximum[a]=glm::max(maximum[a],std::bit_cast<float>(ddaWords[size_t(boundsBase)+size_t(record)*8u+3u+a]));
                }
            for(uint32_t a=0;a<3;++a)ddaWords[size_t(chunkBase)+size_t(chunk)*8u+a]=std::bit_cast<uint32_t>(minimum[a]);
            for(uint32_t a=0;a<3;++a)ddaWords[size_t(chunkBase)+size_t(chunk)*8u+3u+a]=std::bit_cast<uint32_t>(maximum[a]);
        }
        ddaWords[13]=boundsBase;ddaWords[14]=chunkBase;ddaWords[15]=chunkCount;
    }
    if(ddaOn){
        static uint32_t loggedDdaRegistryVersion=UINT32_MAX;
        if(loggedDdaRegistryVersion!=ddaRegistry.Version()){
            uint32_t live=0, payload=0;
            for(const auto& grid:ddaGrids){
                if(grid.imageView)++live;
                if(!grid.bitsetMips.empty())++payload;
            }
            LOGI("[HardwareRT DDA] registry grids=%zu live=%u payload=%u records=%u naadf=%u",
                ddaGrids.size(),live,payload,ddaWords[0],uint32_t(mikan::render::VoxelDDARegistry::NaadfEnabled()));
            loggedDdaRegistryVersion=ddaRegistry.Version();
        }
    }
    const VkDeviceSize ddaBytes=ddaWords.size()*sizeof(uint32_t);
    if(frame.ddaGrid.GetSize()<ddaBytes){frame.ddaGrid.Cleanup();if(!frame.ddaGrid.Create(ddaBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return;}
    frame.ddaGrid.Write(ddaWords.data(),ddaBytes);
    static uint32_t loggedDdaCount=UINT32_MAX;if(ddaOn&&loggedDdaCount!=ddaWords[0]){LOGI("[HardwareRT DDA] active instances=%u; raw palette/materials; mixed triangle fallback",ddaWords[0]);loggedDdaCount=ddaWords[0];}
    if(failed || instances.empty()){frame.cachedInstances.clear();return;}
    if(!frame.dynamicBoundsChanged && instances.size()==frame.cachedInstances.size() && frame.tlas.Handle() && std::memcmp(instances.data(),frame.cachedInstances.data(),instances.size()*sizeof(instances[0]))==0) {
        frame.ready=true;cacheInputs();RayTracingQueryBarrier(cmd);return;
    }
    const VkDeviceSize bytes=instances.size()*sizeof(VkAccelerationStructureInstanceKHR);
    if(frame.instances.GetSize()<bytes){frame.instances.Cleanup();if(!frame.instances.Create(bytes,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return;}
    frame.instances.Write(instances.data(),bytes);
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT);RayTracingBuildBarrier(cmd);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;geometry.geometry.instances.data.deviceAddress=frame.instances.GetDeviceAddress();
    VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(instances.size()),0,0,0};
    bool sameBlasAddresses=instances.size()==frame.cachedInstances.size();
    if(sameBlasAddresses)for(size_t i=0;i<instances.size();++i)
        sameBlasAddresses&=instances[i].accelerationStructureReference==frame.cachedInstances[i].accelerationStructureReference;
    // A compact copy changes BLAS generations: rebuild rather than refit a
    // source TLAS whose links may already have retired after its slot fence.
    const bool update=frame.tlas.Handle() && sameBlasAddresses;
    const auto flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if(!frame.tlas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,{&geometry,1},{&range,1},flags,update))return;
    if(!update)LOGI("[HardwareRT TLAS memory] slot=%u instances=%zu storage=%llu scratch=%llu compactCopies=%u",
        slot,instances.size(),(unsigned long long)frame.tlas.StorageBytes(),(unsigned long long)frame.tlas.ScratchBytes(),compactCopies);
    frame.cachedInstances=std::move(instances);frame.ready=true;cacheInputs();RayTracingQueryBarrier(cmd);
}
