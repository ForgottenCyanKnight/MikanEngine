#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/RenderWorld.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Core/Log.h"
#include <glm/packing.hpp>
#include <filesystem>
#include <unordered_set>
#include <cmath>

bool RayTracingScene::AppendStaticModels(VkCommandBuffer cmd,const RenderWorld& world,
    const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,
    Frame& frame,std::vector<VkAccelerationStructureInstanceKHR>& instances){
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
        std::vector<std::shared_ptr<ModelRayTracingGeometry>> parts;
        bool skippedAlpha=false;
        for(uint32_t subIndex=0;subIndex<mesh.subMeshes.size();++subIndex){
            const auto& sourceSub=mesh.subMeshes[subIndex];
            if(sourceSub.alphaMode>0){skippedAlpha=true;continue;}
            const auto& gpuSub=renderer.GetSubMeshes()[subIndex];
            std::shared_ptr<ModelRayTracingGeometry> geometry;
            for(const auto& candidate:cached)if(candidate && candidate->Matches(renderer,subIndex)){geometry=candidate;break;}
            if(!geometry){
                auto replacement=std::make_shared<ModelRayTracingGeometry>();frame.modelBuilds.push_back(replacement);
                if(!replacement->RecordBuild(cmd,renderer,subIndex)){LOGE("[HardwareRT] static model BLAS failed: %s submesh=%u",path.c_str(),subIndex);return false;}
                geometry=std::move(replacement);
            }
            parts.push_back(geometry);
            const MaterialTextureInfo* importedMaterial=nullptr;
            if(sourceSub.materialIndex>=0&&size_t(sourceSub.materialIndex)<mesh.materialTextures.size())
                importedMaterial=&mesh.materialTextures[size_t(sourceSub.materialIndex)];
            else for(const auto& candidate:mesh.materialTextures)if(candidate.materialName==sourceSub.materialName){importedMaterial=&candidate;break;}
            const auto* texture=g_TexturePool&&!gpuSub.diffuseTexturePath.empty()?g_TexturePool->GetTexture(gpuSub.diffuseTexturePath):nullptr;
            for(const auto entity:group.entities){
                const auto* data=world.Find(entity);if(!data || !data->visible || !data->hasTransform || (data->hasRenderFlags && !data->render.visible))continue;
                const auto& model=data->transform.worldMatrix;const float determinant=glm::determinant(glm::mat3(model));
                if(!std::isfinite(determinant) || std::abs(determinant)<1e-8f)continue;
                if(instances.size()>=caps.limits.maxInstanceCount || instances.size()>=0x1000000u)return false;
                VkAccelerationStructureInstanceKHR instance{};
                for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)instance.transform.matrix[row][col]=model[col][row];
                instance.instanceCustomIndex=uint32_t(frame.hitInstances.size());
                instance.mask=1u|((!data->hasRenderFlags || data->render.castShadow)?2u:0u);
                instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=geometry->Address();
                RayTracingHitInstance hit;hit.entity=uint32_t(entity);hit.model=model;hit.modelGeometry=geometry;hit.subMesh=subIndex;
                hit.color=geometry->BaseColor()*(data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1));
                // bit 0: ideal mirror; bit 1 + bits[31:16] half: entity-level emissive.
                const float entityEmissive=data->hasMaterial?data->material.emissiveIntensity:0.0f;
                const float metallic=data->hasMaterial?data->material.metallic:0.0f;
                const float roughness=data->hasMaterial?data->material.roughness:1.0f;
                hit.materialParams=glm::vec4(metallic,roughness,0,0);
                hit.materialFlags=metallic>=.999f && roughness<=.001f?1u:0u;
                if(entityEmissive>0.0f)hit.materialFlags|=2u|(uint32_t(glm::packHalf1x16(entityEmissive))<<16);
                if(importedMaterial)hit.emissiveFactor=importedMaterial->emissiveFactor;
                if(g_TexturePool&&!gpuSub.emissiveTexturePath.empty()){
                    if(const auto* emission=g_TexturePool->GetTexture(gpuSub.emissiveTexturePath)){
                        hit.emissiveView=emission->imageView;hit.emissiveSampler=g_TexturePool->GetSampler(gpuSub.emissiveTexturePath);hit.emissiveFormat=emission->format;
                    }
                }
                const auto* albedo=texture;
                if(data->hasMaterial && g_TexturePool && !data->material.albedoPath.empty()){
                    if(const auto* override=g_TexturePool->GetTexture(data->material.albedoPath))albedo=override;
                }
                if(albedo && (!data->hasMaterial || data->material.useAlbedoTexture)){
                    hit.albedoView=albedo->imageView;hit.albedoSampler=g_TexturePool->GetSamplerByType(albedo->samplerType);hit.albedoFormat=albedo->format;
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
            LOGW("[HardwareRT] alphaMode submeshes are skipped in hardware ray tracing: %s",path.c_str());
    }
    for(auto it=modelAssets.begin();it!=modelAssets.end();)if(!live.contains(it->first))it=modelAssets.erase(it);else ++it;
    return true;
}
