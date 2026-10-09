#include "Core/CpuStageTrace.h"
#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>
#include <cstring>
#include "Rendering/VoxelDDARegistry.h"
#include <cmath>
#include <unordered_set>
#include <filesystem>
#include <cstdlib>
bool RayTracingScene::SetEnabled(bool value){enabled=value && GetRayTracingDeviceCapabilities().rayQuery;return enabled;}
void RayTracingScene::Cleanup(){frames.clear();assets.clear();modelAssets.clear();converter.Cleanup();enabled=false;}
VkAccelerationStructureKHR RayTracingScene::GetTlas(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return enabled && it!=frames.end() && it->second->serial==serial && it->second->ready?it->second->tlas.Handle():VK_NULL_HANDLE;
}
const std::vector<RayTracingHitInstance>* RayTracingScene::GetHitInstances(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return GetTlas(slot,serial)?&it->second->hitInstances:nullptr;
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
    if(frame.serial==serial)return;
    // This slot's fence has completed: it is now safe to release build inputs and old references.
    for(auto& geometry:frame.builds)geometry->ReleaseBuildInputs();frame.builds.clear();
    for(auto& geometry:frame.modelBuilds)geometry->ReleaseBuildInputs();frame.modelBuilds.clear();
    frame.tlas.ReleaseScratch();frame.hitInstances.clear();frame.blasGenerations.clear();frame.ready=false;frame.serial=serial;
    static const VkDeviceSize compactBudget=[](){const char* value=std::getenv("MIKAN_VOX_BLAS_COMPACT_BUDGET_MB");
        return VkDeviceSize(value?std::clamp(std::strtoul(value,nullptr,10),1ul,1024ul):64ul)*1024u*1024u;}();
    VkDeviceSize remainingCompactBudget=compactBudget;uint32_t compactCopies=0;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::unordered_set<std::string> liveAssets;
    const auto& caps=GetRayTracingDeviceCapabilities();
    const glm::mat4 flipZ=glm::scale(glm::mat4(1),glm::vec3(1,1,-1));
    bool failed=false;
    auto groups=world.voxGroups;
    RenderVoxGroup cubes;cubes.voxPath="builtin:rt-unit-cube";
    const auto engineCube=std::filesystem::path(ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb")).lexically_normal();
    for(const auto& entity:world.entities)if(entity.hasMesh && !entity.hasVoxel &&
        (entity.mesh.type==RenderMeshType::Cube || (entity.mesh.type==RenderMeshType::Model && std::filesystem::path(entity.mesh.modelPath).lexically_normal()==engineCube)))cubes.entities.push_back(entity.entity);
    if(!cubes.entities.empty())groups.push_back(std::move(cubes));

    const auto& ddaGrids=mikan::render::VoxelDDARegistry::Get().Grids();
    const bool ddaOn=mikan::render::VoxelDDARegistry::BootEnabled();
    std::vector<uint32_t> ddaPaletteOffsets(ddaGrids.size(),0);
    std::vector<glm::vec4> ddaPalette;
    if(ddaOn)for(size_t g=0;g<ddaGrids.size();++g){
        ddaPaletteOffsets[g]=uint32_t(ddaPalette.size());
        ddaPalette.insert(ddaPalette.end(),ddaGrids[g].palette.begin(),ddaGrids[g].palette.end());
    }
    std::vector<uint32_t> ddaRecords;
    for(const auto& group:groups) {
        const auto found=renderers.find(group.voxPath);if(found==renderers.end() || !found->second || !found->second->HasLoaded()){failed=true;continue;}
        std::vector<std::pair<const VoxRenderer*,glm::mat4>> surfaces;
        found->second->VisitSurfaces([&](const VoxRenderer& mesh,const glm::mat4& transform){surfaces.emplace_back(&mesh,transform);});
        uint32_t componentIndex=0;
        for(const auto& [surface,componentTransform]:surfaces){
        const auto component=componentIndex++;
        const auto& renderer=*surface;
        const auto assetKey=group.voxPath+"#"+std::to_string(reinterpret_cast<uintptr_t>(surface));
        liveAssets.insert(assetKey);
        auto& geometry=assets[assetKey];
        if(!geometry || geometry->Source()!=&renderer || geometry->Revision()!=renderer.GetGeometryRevision()) {
            auto replacement=std::make_shared<VoxRayTracingGeometry>();
            // Keep even a failed build alive until this slot's fence: compute may already be recorded.
            frame.builds.push_back(replacement);
            if(!replacement->RecordBuild(cmd,renderer,converter)){LOGE("[HardwareRT] vox BLAS build failed: %s",group.voxPath.c_str());failed=true;continue;}
            geometry=std::move(replacement);
        }
        if(compactCopies<2){std::shared_ptr<AccelerationStructure> retired;
            if(geometry->RecordCompaction(cmd,remainingCompactBudget,retired)){
                frame.blasGenerations.push_back(std::move(retired));++compactCopies;
            }
        }
        frame.blasGenerations.push_back(geometry->Blas());
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
            const bool ddaOwned=ddaOn&&gridId<8u&&gridId<ddaGrids.size()&&ddaGrids[gridId].imageView;
            instance.mask=ddaOwned?4u:(1u|((!data->hasRenderFlags||data->render.castShadow)?2u:0u));            instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference=geometry->Address();instances.push_back(instance);
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
            }
        }
    }
        } // Component instances share the source mesh BLAS.
    for(auto it=assets.begin();it!=assets.end();)if(!liveAssets.contains(it->first))it=assets.erase(it);else ++it;
    if(!AppendStaticModels(cmd,world,models,frame,instances))failed=true;
    // Refresh grid instances even when the TLAS update takes the cached fast path.
    std::vector<uint32_t> ddaWords(16,0);ddaWords[0]=uint32_t(ddaRecords.size()/24);
    ddaWords.insert(ddaWords.end(),ddaRecords.begin(),ddaRecords.end());
    const size_t paletteBase=ddaWords.size();ddaWords.resize(paletteBase+ddaPalette.size()*4);
    if(!ddaPalette.empty())std::memcpy(ddaWords.data()+paletteBase,ddaPalette.data(),ddaPalette.size()*sizeof(glm::vec4));
    const VkDeviceSize ddaBytes=ddaWords.size()*sizeof(uint32_t);
    if(frame.ddaGrid.GetSize()<ddaBytes){frame.ddaGrid.Cleanup();if(!frame.ddaGrid.Create(ddaBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return;}
    frame.ddaGrid.Write(ddaWords.data(),ddaBytes);
    static uint32_t loggedDdaCount=UINT32_MAX;if(ddaOn&&loggedDdaCount!=ddaWords[0]){LOGI("[HardwareRT DDA] active instances=%u; raw palette/materials; mixed triangle fallback",ddaWords[0]);loggedDdaCount=ddaWords[0];}
    if(failed || instances.empty()){frame.cachedInstances.clear();return;}
    if(instances.size()==frame.cachedInstances.size() && frame.tlas.Handle() && std::memcmp(instances.data(),frame.cachedInstances.data(),instances.size()*sizeof(instances[0]))==0) {
        frame.ready=true;RayTracingQueryBarrier(cmd);return;
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
    frame.cachedInstances=std::move(instances);frame.ready=true;RayTracingQueryBarrier(cmd);
}
