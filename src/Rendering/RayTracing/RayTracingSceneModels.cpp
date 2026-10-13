#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/RenderWorld.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include <cstdlib>
#include <glm/packing.hpp>
#include <filesystem>
#include <unordered_set>
#include <cmath>

bool RayTracingScene::AppendStaticModels(VkCommandBuffer cmd,const RenderWorld& world,
    const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,
    Frame& frame,std::vector<VkAccelerationStructureInstanceKHR>& instances,
    VkDeviceSize& remainingCompactBudget,uint32_t& compactCopies){
    static const bool disableAlphaTest=[](){
        const char* value=std::getenv("MIKAN_HWRT_PROFILE_NO_ALPHA");
        const bool disabled=Core::VulkanGpuProfiler::IsRequested() && value && value[0]=='1';
        if(disabled)LOGW("[HardwareRT][Cost] Alpha MASK disabled for profiling; foliage is opaque. Restart without MIKAN_HWRT_PROFILE_NO_ALPHA to restore.");
        return disabled;
    }();
    std::unordered_set<std::string> live;
    static std::unordered_set<std::string> unsupported;
    const auto engineCube=std::filesystem::path(ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb")).lexically_normal();
    const auto& caps=GetRayTracingDeviceCapabilities();
    for(const auto& group:world.modelGroups){
        const auto& path=group.modelPath.empty()?group.rendererKey:group.modelPath;
        if(std::filesystem::path(path).lexically_normal()==engineCube)continue;
        const auto found=models.find(path);if(found==models.end() || !found->second || !found->second->HasModelLoaded())continue;
        auto& renderer=*found->second;const auto& mesh=renderer.GetMeshData();
        if(mesh.isMmd || !mesh.bones.empty() || mesh.subMeshes.empty()){
            if(unsupported.insert(path).second)LOGW("[HardwareRT] currently supports static opaque models only (no MMD/skinning): %s",path.c_str());
            continue;
        }
        live.insert(path);
        // One BLAS per opaque submesh; each (entity, submesh) pair becomes one
        // TLAS instance with its own hit table entry and material bindings.
        auto& cached=modelAssets[path];
        // A shared BLAS must also preserve entities that explicitly veto the
        // source albedo/alpha texture. Keep its complete geometry in that case.
        bool allowPartition=true;
        for(const auto entity:group.entities){const auto* data=world.Find(entity);
            if(data && data->hasMaterial && !data->material.useAlbedoTexture && !data->material.albedoPath.empty())allowPartition=false;
        }
        // Cache slots have the same stable index as the imported submeshes.
        // Scanning every cached BLAS for every submesh made static preparation
        // quadratic even when no primary rays could hit the scene.
        std::vector<std::shared_ptr<ModelRayTracingGeometry>> parts(mesh.subMeshes.size());
        bool skippedAlpha=false;
        for(uint32_t subIndex=0;subIndex<mesh.subMeshes.size();++subIndex){
            const auto& sourceSub=mesh.subMeshes[subIndex];
            if(sourceSub.alphaMode>0)skippedAlpha=true; // reported below; transparent submeshes now enter the BLAS with mask bit 8
            const auto& gpuSub=renderer.GetSubMeshes()[subIndex];
            std::shared_ptr<ModelRayTracingGeometry> geometry;
            if(subIndex<cached.size() && cached[subIndex] && cached[subIndex]->Matches(renderer,subIndex,allowPartition))
                geometry=cached[subIndex];
            if(!geometry){
                auto replacement=std::make_shared<ModelRayTracingGeometry>();frame.modelBuilds.push_back(replacement);
                if(!replacement->RecordBuild(cmd,renderer,subIndex,allowPartition)){LOGE("[HardwareRT] static model BLAS failed: %s submesh=%u",path.c_str(),subIndex);return false;}
                geometry=std::move(replacement);
            }
            parts[subIndex]=geometry;
            if(geometry->FullyTransparent())continue;
            if(compactCopies<2){std::shared_ptr<AccelerationStructure> retired;
                if(geometry->RecordCompaction(cmd,remainingCompactBudget,retired)){
                    frame.blasGenerations.push_back(std::move(retired));++compactCopies;
                }
            }
            // Snapshot the exact generation used by this frame's TLAS. An asset
            // wrapper may switch to a compacted BLAS while older slots are in flight.
            frame.blasGenerations.push_back(geometry->Blas());
            parts[subIndex]=geometry;
            const MaterialTextureInfo* importedMaterial=nullptr;
            if(sourceSub.materialIndex>=0&&size_t(sourceSub.materialIndex)<mesh.materialTextures.size())
                importedMaterial=&mesh.materialTextures[size_t(sourceSub.materialIndex)];
            else for(const auto& candidate:mesh.materialTextures)if(candidate.materialName==sourceSub.materialName){importedMaterial=&candidate;break;}
            const auto* texture=g_TexturePool&&!gpuSub.diffuseTexturePath.empty()?g_TexturePool->GetTexture(gpuSub.diffuseTexturePath):nullptr;
            if(!texture){
                static std::unordered_set<std::string> reported;
                const bool empty=gpuSub.diffuseTexturePath.empty();
                if(reported.insert(path+"#"+std::to_string(subIndex)+(empty?"#empty":"#miss")).second)
                    LOGW("[HardwareRT] submesh %u albedo %s: \"%s\"",subIndex,empty?"path empty":"texture lookup miss",gpuSub.diffuseTexturePath.c_str());
            }
            for(const auto entity:group.entities){
                const auto* data=world.Find(entity);if(!data || !data->visible || !data->hasTransform || (data->hasRenderFlags && !data->render.visible))continue;
                const auto& model=data->transform.worldMatrix;const float determinant=glm::determinant(glm::mat3(model));
                if(!std::isfinite(determinant) || std::abs(determinant)<1e-8f)continue;
                if(instances.size()>=caps.limits.maxInstanceCount || instances.size()>=0x1000000u)return false;
                VkAccelerationStructureInstanceKHR instance{};
                for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)instance.transform.matrix[row][col]=model[col][row];
                instance.instanceCustomIndex=uint32_t(frame.hitInstances.size());
                // Dielectric transmission: alpha-blended submeshes refract in the RT
                // path; glTF KHR_materials_transmission takes precedence when present.
                // Transported in color.a (0 = opaque). Mask bit 8 = transparent so
                // shadow (2) / GI (1) rays pass through glass; camera/refraction rays
                // trace with mask 1|8. Alpha-masked (mode 1) stays fully opaque.
                float transmission=0.0f;
                if(sourceSub.alphaMode==2)transmission=gpuSub.diffuseTransmissionFactor>0.0f?glm::clamp(gpuSub.diffuseTransmissionFactor,0.35f,1.0f):0.9f;
                else if(gpuSub.diffuseTransmissionFactor>0.0f)transmission=glm::clamp(gpuSub.diffuseTransmissionFactor,0.35f,1.0f);
                instance.mask=(transmission>0.0f)?8u:(1u|((!data->hasRenderFlags || data->render.castShadow)?2u:0u));
                instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=geometry->Address();
                RayTracingHitInstance hit;hit.entity=uint32_t(entity);hit.model=model;hit.modelGeometry=geometry;hit.subMesh=subIndex;
                hit.color=geometry->BaseColor()*(data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1));
                hit.color.a=transmission;
                if(transmission>0.0f)hit.rayMask=8u;
                // bit 0: ideal mirror; bit 1 + bits[31:16] half: entity-level emissive.
                // Scaled by kModelEmissionGain to match the voxel emission power.
                const float entityEmissive=(data->hasMaterial?data->material.emissiveIntensity:0.0f)*kModelEmissionGain;
                const float metallic=data->hasMaterial?data->material.metallic:0.0f;
                const float roughness=data->hasMaterial?data->material.roughness:1.0f;
                hit.materialParams=glm::vec4(metallic,roughness,0,0);
                hit.materialFlags=metallic>=.999f && roughness<=.001f?1u:0u;
                // All ray types alpha-test MASK candidates at cutoff 0.5.
                // FBX and other non-glTF
                // assets lose the MASK flag (alphaMode stays 0), so the albedo
                // texture's probed alphaCutout flag recovers the cutout semantics.
                if(!disableAlphaTest && (sourceSub.alphaMode==1||(sourceSub.alphaMode<=0&&texture&&texture->alphaCutout))){
                    hit.materialFlags|=4u;
                    // Opaque instances commit in hardware; only MASK produces candidates.
                    if(geometry->NeedsAlphaCandidates() && !geometry->HasOpacityPartition())instance.flags|=VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
                    static std::unordered_set<std::string> reportedAT;
                    if(reportedAT.insert(path).second)
                        LOGW("[HardwareRT] alphaTest enabled (alphaMode=%d, texture cutout probe): %s",sourceSub.alphaMode,path.c_str());
                }
                if(entityEmissive>0.0f)hit.materialFlags|=2u|(uint32_t(glm::packHalf1x16(entityEmissive))<<16);
                if(disableAlphaTest)instance.flags|=VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
                if(importedMaterial)hit.emissiveFactor=importedMaterial->emissiveFactor;
                if(g_TexturePool&&!gpuSub.emissiveTexturePath.empty()){
                    if(const auto* emission=g_TexturePool->GetTexture(gpuSub.emissiveTexturePath)){
                        hit.emissiveView=emission->imageView;hit.emissiveSampler=g_TexturePool->GetSampler(gpuSub.emissiveTexturePath);hit.emissiveFormat=emission->format;
                    }
                }
                // Raster semantics: per-submesh model texture wins; entity-level albedoPath
                // is only a FALLBACK when the submesh has no diffuse texture of its own.
                // useAlbedoTexture only vetoes when the entity has its own explicit path —
                // unconfigured (false + empty path, the serialized default) shows model textures.
                const auto* albedo=texture;
                if(!albedo && data->hasMaterial && data->material.useAlbedoTexture && g_TexturePool && !data->material.albedoPath.empty()){
                    if(const auto* fallback=g_TexturePool->GetTexture(data->material.albedoPath))albedo=fallback;
                }
                if(albedo && (!data->hasMaterial || data->material.useAlbedoTexture || data->material.albedoPath.empty())){
                    hit.albedoView=albedo->imageView;hit.albedoSampler=g_TexturePool->GetSamplerByType(albedo->samplerType);hit.albedoFormat=albedo->format;
                }
                if(g_TexturePool&&!gpuSub.normalTexturePath.empty()){
                    if(const auto* normal=g_TexturePool->GetTexture(gpuSub.normalTexturePath)){
                        hit.normalView=normal->imageView;hit.normalSampler=g_TexturePool->GetSampler(gpuSub.normalTexturePath);hit.normalFormat=normal->format;
                    }
                }
                // glTF metallicRoughness: combined image fills both paths, g=roughness b=metallic.
                if(g_TexturePool){
                    const std::string& mrPath=gpuSub.metallicTexturePath.empty()?gpuSub.roughnessTexturePath:gpuSub.metallicTexturePath;
                    const auto* mr=mrPath.empty()?nullptr:g_TexturePool->GetTexture(mrPath);
                    if(mr){hit.mrView=mr->imageView;hit.mrSampler=g_TexturePool->GetSamplerByType(mr->samplerType);}
                }
                instances.push_back(instance);frame.hitInstances.push_back(std::move(hit));
            }
        }
        cached=std::move(parts);
        if(skippedAlpha && unsupported.insert(path+"#alpha").second)
            LOGW("[HardwareRT] alphaMode submeshes enter ray tracing as dielectric glass (mask bit 8): %s",path.c_str());
    }
    for(auto it=modelAssets.begin();it!=modelAssets.end();)if(!live.contains(it->first))it=modelAssets.erase(it);else ++it;
    return true;
}
