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
#include <cstring>
#include <fstream>
#include "Core/Utf8Path.h"
#include "json.hpp"

namespace {
float ImportedEmissionScale(const std::string& path){
    static std::unordered_map<std::string,float> scales;
    const auto resolved=ProjectManager::GetInstance().ResolveAssetPath(path);
    auto [it,inserted]=scales.try_emplace(resolved,1.0f);
    if(inserted){
        try{
            std::ifstream input(Utf8Path(resolved+".import.json"));
            if(input){const auto metadata=nlohmann::json::parse(input);
                if(metadata.value("emissionUnits",std::string())=="radiometric"){
                    it->second=1.0f/kModelEmissionGain;
                    LOGI("[HardwareRT] Blender radiometric emission units: %s",path.c_str());
                }
            }
        }catch(const std::exception& error){LOGW("[HardwareRT] invalid material import metadata: %s",error.what());}
    }
    return it->second;
}
// Explicit PMX material rules; parse once per model, outside the vertex loop.
glm::vec2 HairHighlightParams(const std::string& path,const std::string& material,const std::string& texture){
    static std::unordered_map<std::string,nlohmann::json> configs;
    const auto resolved=ProjectManager::GetInstance().ResolveAssetPath(path);
    auto [it,inserted]=configs.try_emplace(resolved,nlohmann::json::array());
    if(inserted){
        try {std::ifstream input(Utf8Path(resolved+".import.json"));
            if(input){auto j=nlohmann::json::parse(input);if(j.contains("pbrMaterials")&&j["pbrMaterials"].is_array())it->second=j["pbrMaterials"];}}
        catch(const std::exception& e){LOGW("[HardwareRT hair] invalid config: %s",e.what());}
    }
    float value=0,shift=0;
    for(const auto& rule:it->second){
        if(!rule.is_object())continue;
        const bool match=rule.value("materialName",std::string())==material ||
            (!texture.empty()&&rule.value("diffuseTexture",std::string())==Utf8String(Utf8Path(texture).filename()));
        if(match&&rule.contains("hairAnisotropy")&&rule["hairAnisotropy"].is_number()){
            const float candidate=rule["hairAnisotropy"].get<float>();
            if(std::isfinite(candidate))value=std::clamp(candidate,-0.9f,0.9f);
            if(rule.contains("hairBandShift")&&rule["hairBandShift"].is_number()){
                float candidateShift=rule["hairBandShift"].get<float>();
                if(std::isfinite(candidateShift))shift=std::clamp(candidateShift,-1.0f,1.0f);
            }
        }
    }
    static const float scale=[](){const char* text=std::getenv("MIKAN_HWRT_HAIR_SCALE");if(!text)return 1.0f;char* end=nullptr;float v=std::strtof(text,&end);return end!=text&&*end==0&&std::isfinite(v)?std::clamp(v,0.0f,1.0f):1.0f;}();
    value*=scale;
    static std::unordered_set<std::string> reported;
    if(value!=0&&reported.insert(resolved+"#"+material).second)LOGI("[HardwareRT hair] material=%s anisotropy=%.3f UV-V direction",material.c_str(),value);
    return glm::vec2(value,shift);
}
}

#include "Core/CpuStageTrace.h"

bool RayTracingScene::AppendModels(VkCommandBuffer cmd,const RenderWorld& world,
    const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,
    Frame& frame,std::vector<VkAccelerationStructureInstanceKHR>& instances,
    VkDeviceSize& remainingCompactBudget,uint32_t& compactCopies){
    static const bool disableAlphaTest=[](){
        const char* value=std::getenv("MIKAN_HWRT_PROFILE_NO_ALPHA");
        const bool disabled=Core::VulkanGpuProfiler::IsRequested() && value && value[0]=='1';
        if(disabled)LOGW("[HardwareRT][Cost] Alpha MASK disabled for profiling; foliage is opaque. Restart without MIKAN_HWRT_PROFILE_NO_ALPHA to restore.");
        return disabled;
    }();
    std::unordered_set<std::string> live,liveDynamic;
    static std::unordered_set<std::string> unsupported;
    const auto engineCube=std::filesystem::path(ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb")).lexically_normal();
    const auto& caps=GetRayTracingDeviceCapabilities();
    for(const auto& group:world.modelGroups){
        const auto& path=group.modelPath.empty()?group.rendererKey:group.modelPath;
        if(std::filesystem::path(path).lexically_normal()==engineCube)continue;
        // PMX owns complete per-entity geometry; glTF keeps shared geometry and
        // a lightweight pose renderer at group.rendererKey.
        auto found=models.find(group.rendererKey);
        ModelRenderer* pose=found==models.end()?nullptr:found->second.get();
        if(!pose||!pose->GetMeshData().isMmd)found=models.find(path);
        if(found==models.end()||!found->second||!found->second->HasModelLoaded())continue;
        auto& renderer=*found->second;const auto& mesh=renderer.GetMeshData();
        if(mesh.subMeshes.empty())continue;
        if(!pose)pose=&renderer;
        const bool dynamic=mesh.isMmd||!mesh.bones.empty();
        mikan::rt::RayTracingDeformedPose* deformed=nullptr;
        if(dynamic){
            liveDynamic.insert(group.rendererKey);
            deformed=&deformedPoses[group.rendererKey];
            if(!PrepareDeformedPose(renderer,*pose,frame.serial,*deformed))return false;
        }else live.insert(path);
        // One BLAS per opaque submesh; each (entity, submesh) pair becomes one
        // TLAS instance with its own hit table entry and material bindings.
        // Looking up the static cache does not allocate an entry for dynamic assets.
        auto* cached=dynamic?nullptr:&modelAssets[path];
        auto* dynamicCached=dynamic?&frame.dynamicModels[group.rendererKey]:nullptr;
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
            if(dynamic){
                if(dynamicCached->size()!=mesh.subMeshes.size())dynamicCached->resize(mesh.subMeshes.size());
                auto& part=(*dynamicCached)[subIndex];
                if(!part||!part->MatchesDynamic(renderer,subIndex)){
                    // Keep failed/retired generations until this submission's fence.
                    auto replacement=std::make_shared<DynamicModelRayTracingGeometry>();
                    frame.modelBuilds.push_back(replacement);part=std::move(replacement);
                }
                if(!part->RecordPose(cmd,renderer,subIndex,deformed->current[subIndex],deformed->previous[subIndex],frame.dynamicBoundsChanged))return false;
                geometry=part;
            }else if(subIndex<cached->size() && (*cached)[subIndex] && (*cached)[subIndex]->Matches(renderer,subIndex,allowPartition))
                geometry=(*cached)[subIndex];
            if(!geometry){
                auto replacement=std::make_shared<ModelRayTracingGeometry>();frame.modelBuilds.push_back(replacement);
                if(!replacement->RecordBuild(cmd,renderer,subIndex,allowPartition)){LOGE("[HardwareRT] static model BLAS failed: %s submesh=%u",path.c_str(),subIndex);return false;}
                geometry=std::move(replacement);
            }
            parts[subIndex]=geometry;
            if(geometry->FullyTransparent())continue;
            if(!dynamic && compactCopies<2){std::shared_ptr<AccelerationStructure> retired;
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
                // Opacity blending is not dielectric transmission. Only an
                // explicit imported transmission factor may enter the glass path.
                // Transported in color.a (0 = opaque). Mask bit 8 = transparent so
                // shadow (2) / GI (1) rays pass through glass; camera/refraction rays
                // trace with mask 1|8. Alpha-masked (mode 1) stays fully opaque.
                float transmission=0.0f;
                if(!mesh.isMmd && gpuSub.diffuseTransmissionFactor>0.0f)transmission=glm::clamp(gpuSub.diffuseTransmissionFactor,0.35f,1.0f);
                instance.mask=(transmission>0.0f)?8u:(1u|((!data->hasRenderFlags || data->render.castShadow)?2u:0u));
                instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=geometry->Address();
                RayTracingHitInstance hit;hit.entity=uint32_t(entity);hit.model=model;hit.modelGeometry=geometry;hit.subMesh=subIndex;
                hit.deformationRevision=dynamic?deformed->revision:0;
                hit.color=geometry->BaseColor()*(data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1));
                hit.color.a=transmission;
                const auto& mmdMaterials=renderer.GetMmdMaterials();
                const auto* mmdMaterial=mesh.isMmd && sourceSub.materialIndex>=0 && size_t(sourceSub.materialIndex)<mmdMaterials.size()?&mmdMaterials[sourceSub.materialIndex]:nullptr;
                const float mmdOpacity=mesh.isMmd?glm::clamp(mmdMaterial?mmdMaterial->m_diffuse.a:geometry->BaseColor().a,0.0f,1.0f):1.0f;
                if(mesh.isMmd && mmdOpacity<0.5f)continue;
                if(mmdMaterial)hit.color=glm::vec4(glm::vec3(mmdMaterial->m_diffuse)*(data->hasMaterial?data->material.albedoColor:glm::vec3(1)),0);
                if(transmission>0.0f)hit.rayMask=8u;
                // bit 0: ideal mirror; bit 1 + bits[31:16] half: entity-level emissive.
                // Scaled by kModelEmissionGain to match the voxel emission power.
                const float entityEmissive=(data->hasMaterial?data->material.emissiveIntensity:0.0f)*kModelEmissionGain;
                const float metallic=data->hasMaterial && data->material.metallic>=0.0f
                    ?data->material.metallic:(gpuSub.metallic>=0.0f?gpuSub.metallic:0.0f);
                const float roughness=data->hasMaterial && data->material.roughness>=0.0f
                    ?data->material.roughness:(gpuSub.roughness>=0.0f?gpuSub.roughness:1.0f);
                hit.materialParams=glm::vec4(metallic,roughness,0,0);
                // Match PMX's authored specular-power roughness when available.
                if(mesh.isMmd && sourceSub.roughness>=0.0f)hit.materialParams.y=sourceSub.roughness;
                if(mesh.isMmd){
                    if(sourceSub.metallic>=0.0f)hit.materialParams.x=sourceSub.metallic;
                    const auto hair=HairHighlightParams(path,sourceSub.materialName,gpuSub.diffuseTexturePath);
                    hit.materialParams.z=hair.x;hit.materialParams.w=hair.y;
                }
                hit.materialFlags=metallic>=.999f && roughness<=.001f?1u:0u;
                if(mesh.isMmd)hit.materialFlags|=8u; // UV-derived PMX cotangent normal-map frame.
                // All ray types alpha-test MASK candidates at cutoff 0.5.
                // FBX and other non-glTF
                // assets lose the MASK flag (alphaMode stays 0), so the albedo
                // texture's probed alphaCutout flag recovers the cutout semantics.
                // BLEND currently uses the same binary coverage approximation
                // as MASK in RT; opaque alpha must still shade and cast shadows.
                if(!disableAlphaTest && (sourceSub.alphaMode>=1||((mesh.isMmd||sourceSub.alphaMode<=0)&&texture&&texture->alphaCutout))){
                    hit.materialFlags|=4u;
                    // BLEND stochastic coverage (vox_rt_alpha.glsl): accept
                    // probability = texel alpha; TAA converges the soft gradient.
                    if(sourceSub.alphaMode==2)hit.materialFlags|=16u;
                    // Opaque instances commit in hardware; only MASK produces candidates.
                    if(geometry->NeedsAlphaCandidates() && !geometry->HasOpacityPartition())instance.flags|=VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
                    static std::unordered_set<std::string> reportedAT;
                    if(reportedAT.insert(path).second)
                        LOGW("[HardwareRT] alphaTest enabled (alphaMode=%d, texture cutout probe): %s",sourceSub.alphaMode,path.c_str());
                }
                if(entityEmissive>0.0f)hit.materialFlags|=2u|(uint32_t(glm::packHalf1x16(entityEmissive))<<16);
                if(disableAlphaTest)instance.flags|=VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
                if(importedMaterial)hit.emissiveFactor=importedMaterial->emissiveFactor*ImportedEmissionScale(path);
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
        if(cached)*cached=std::move(parts);
        if(skippedAlpha && unsupported.insert(path+"#alpha").second)
            LOGW("[HardwareRT] alpha materials use RT coverage testing; fractional BLEND is approximated by cutoff 0.5: %s",path.c_str());
    }
    for(auto it=modelAssets.begin();it!=modelAssets.end();)if(!live.contains(it->first))it=modelAssets.erase(it);else ++it;
    for(auto it=frame.dynamicModels.begin();it!=frame.dynamicModels.end();)if(!liveDynamic.contains(it->first))it=frame.dynamicModels.erase(it);else ++it;
    for(auto it=deformedPoses.begin();it!=deformedPoses.end();)if(!liveDynamic.contains(it->first))it=deformedPoses.erase(it);else ++it;
    return true;
}

bool RayTracingScene::PrepareDeformedPose(ModelRenderer& geometry,ModelRenderer& pose,uint64_t serial,mikan::rt::RayTracingDeformedPose& result){
    const auto& mesh=geometry.GetMeshData();const auto& gpu=geometry.GetSubMeshes();
    if(gpu.size()!=mesh.subMeshes.size()||gpu.empty())return false;
    if(result.serial==serial&&result.source==&geometry&&result.poseSource==&pose)return true;
    const bool reset=result.source!=&geometry||result.poseSource!=&pose||result.sourceBuffer!=gpu[0].vertexBuffer||
        result.current.size()!=mesh.subMeshes.size()||result.serial==UINT64_MAX||serial!=result.serial+1;
    const bool mmd=geometry.HasMmdDeformedPose();
    const auto& matrices=pose.GetBoneMatrices();
    const bool matricesChanged=result.matrices.size()!=matrices.size()||(!matrices.empty()&&
        std::memcmp(result.matrices.data(),matrices.data(),matrices.size()*sizeof(glm::mat4))!=0);
    const uint64_t mmdRevision=mmd?geometry.GetMmdDeformationRevision():UINT64_MAX;
    const bool changed=reset||result.mmdRevision!=mmdRevision||(!mmd&&matricesChanged);
    result.previous=result.current;
    if(changed){
        Core::CpuStageTrace timing("rt.dynamic_deformation");
        result.current.resize(mesh.subMeshes.size());
        for(size_t s=0;s<mesh.subMeshes.size();++s){
            auto vertices=std::make_shared<std::vector<Vertex>>();
            if(mmd){
                const auto& deformed=geometry.GetMmdDeformedVertices();
                if(s>=deformed.size()||deformed[s].size()!=mesh.subMeshes[s].vertices.size())return false;
                *vertices=deformed[s];
            }else{
                vertices->reserve(mesh.subMeshes[s].vertices.size());
                for(const auto& v:mesh.subMeshes[s].vertices)vertices->push_back(mikan::rt::SkinRayTracingVertex(v,matrices));
            }
            // Texture convention belongs to PMX, even before VMD evaluates a pose.
            if(mesh.isMmd){
                // PMX raster samples (u, 1-v), including imported UV-origin overrides
                // and animated UV morphs. Convert only the RT-owned snapshot so
                // albedo, alpha candidates and normal maps share that convention.
                for(auto& vertex:*vertices){
                    auto uv=UnpackHalf2(vertex.TexCoords);
                    uv.y=1.0f-uv.y;
                    vertex.TexCoords=PackHalf2(uv);
                }
            }
            result.current[s]=std::move(vertices);
        }
        ++result.revision;
    }
    if(reset)result.previous=result.current;
    result.matrices=matrices;result.mmdRevision=mmd?geometry.GetMmdDeformationRevision():UINT64_MAX;
    result.source=&geometry;result.poseSource=&pose;result.sourceBuffer=gpu[0].vertexBuffer;result.serial=serial;
    return true;
}
