#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>
#include <cstring>
#include <cmath>
#include <unordered_set>
#include <filesystem>
bool RayTracingScene::SetEnabled(bool value){enabled=value && GetRayTracingDeviceCapabilities().rayQuery;return enabled;}
void RayTracingScene::Cleanup(){frames.clear();assets.clear();modelAssets.clear();converter.Cleanup();enabled=false;}
VkAccelerationStructureKHR RayTracingScene::GetTlas(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return enabled && it!=frames.end() && it->second->serial==serial && it->second->ready?it->second->tlas.Handle():VK_NULL_HANDLE;
}
const std::vector<RayTracingHitInstance>* RayTracingScene::GetHitInstances(uint32_t slot,uint64_t serial)const {
    const auto it=frames.find(slot);return GetTlas(slot,serial)?&it->second->hitInstances:nullptr;
}
void RayTracingScene::Prepare(VkCommandBuffer cmd,const RenderWorld& world,
    const std::unordered_map<std::string,std::unique_ptr<VoxRenderer>>& renderers,
    const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,uint32_t slot,uint64_t serial) {
    if(!enabled || !cmd)return;
    auto& pointer=frames[slot];if(!pointer)pointer=std::make_unique<Frame>();auto& frame=*pointer;
    if(frame.serial==serial)return;
    // This slot's fence has completed: it is now safe to release build inputs and old references.
    for(auto& geometry:frame.builds)geometry->ReleaseBuildInputs();frame.builds.clear();
    for(auto& geometry:frame.modelBuilds)geometry->ReleaseBuildInputs();frame.modelBuilds.clear();
    frame.tlas.ReleaseScratch();frame.hitInstances.clear();frame.ready=false;frame.serial=serial;
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
    for(const auto& group:groups) {
        const auto found=renderers.find(group.voxPath);if(found==renderers.end() || !found->second || !found->second->HasLoaded()){failed=true;continue;}
        const auto& renderer=*found->second;liveAssets.insert(group.voxPath);
        auto& geometry=assets[group.voxPath];
        if(!geometry || geometry->Source()!=&renderer || geometry->Revision()!=renderer.GetGeometryRevision()) {
            auto replacement=std::make_shared<VoxRayTracingGeometry>();
            // Keep even a failed build alive until this slot's fence: compute may already be recorded.
            frame.builds.push_back(replacement);
            if(!replacement->RecordBuild(cmd,renderer,converter)){LOGE("[HardwareRT] vox BLAS build failed: %s",group.voxPath.c_str());failed=true;continue;}
            geometry=std::move(replacement);
        }
        for(const auto entity:group.entities) {
            const auto* data=world.Find(entity);if(!data || !data->visible || !data->hasTransform || (data->hasRenderFlags && !data->render.visible))continue;
            // Built-in cube.glb's POSITION accessor spans [-1,1], unlike the unit quad cube [-.5,.5].
            const auto local=data->hasVoxel?flipZ:(data->mesh.type==RenderMeshType::Model?glm::scale(glm::mat4(1),glm::vec3(2)):glm::mat4(1));
            const auto model=data->transform.worldMatrix*local;
            const float determinant=glm::determinant(glm::mat3(model));if(!std::isfinite(determinant) || std::abs(determinant)<1e-8f)continue;
            if(instances.size()>=caps.limits.maxInstanceCount || instances.size()>=0x1000000u){failed=true;break;}
            VkAccelerationStructureInstanceKHR instance{};
            for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)instance.transform.matrix[row][col]=model[col][row];
            instance.instanceCustomIndex=uint32_t(frame.hitInstances.size());
            // bit 0: general rays; bit 1: shadow rays. Raster camera culling never enters this list.
            instance.mask=1u|((!data->hasRenderFlags || data->render.castShadow)?2u:0u);
            instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference=geometry->Address();instances.push_back(instance);
            // bit 0: ideal mirror; bit 1 + bits[31:16] half: entity-level emissive.
            const float entityEmissive=data->hasMaterial?data->material.emissiveIntensity:0.0f;
            uint32_t materialFlags=data->hasMaterial && data->material.metallic>=.999f && data->material.roughness<=.001f?1u:0u;
            if(entityEmissive>0.0f)materialFlags|=2u|(uint32_t(glm::packHalf1x16(entityEmissive))<<16);
            frame.hitInstances.push_back({uint32_t(entity),model,data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1),geometry,materialFlags});
        }
    }
    for(auto it=assets.begin();it!=assets.end();)if(!liveAssets.contains(it->first))it=assets.erase(it);else ++it;
    if(!AppendStaticModels(cmd,world,models,frame,instances))failed=true;
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
    const bool update=frame.tlas.Handle() && instances.size()==frame.cachedInstances.size();
    const auto flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if(!frame.tlas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,{&geometry,1},{&range,1},flags,update))return;
    frame.cachedInstances=std::move(instances);frame.ready=true;RayTracingQueryBarrier(cmd);
}
