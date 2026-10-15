#include "Core/CpuStageTrace.h"
#include "Core/EngineGlobal.h"
#include "Core/RenderGlobals.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/VoxelDDARegistry.h"
#include "Core/DlssFrameGeneration.h"
#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include "Core/PipelineCapture.h"
#include "Rendering/VoxRenderer.h"
#include "Rendering/ModelRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include <glm/packing.hpp>
#include <cstring>
#include <limits>
#include "Rendering/AtmosphereLUT.h"
#include <cmath>
#include "EmissiveLightIndex.h"
#include "SoftwareQuadBvh.h"

namespace {
// Compare fields, not struct padding. RayTracingScene replaces immutable geometry generations.
bool SameSceneInputs(const std::vector<RayTracingHitInstance>& a,const std::vector<RayTracingHitInstance>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];
        // Vertex deformation alone does not change buffer addresses/material tables.
        // Animated emitters do change the CPU light distribution and must rebuild.
        if(x.deformationRevision!=y.deformationRevision && ((x.materialFlags&2u)!=0u || x.emissiveFactor!=glm::vec3(0)))return false;
        if(x.rayMask!=y.rayMask || x.HistoryKey()!=y.HistoryKey() || x.geometry!=y.geometry || x.modelGeometry!=y.modelGeometry || x.materialFlags!=y.materialFlags ||
           x.albedoView!=y.albedoView || x.albedoSampler!=y.albedoSampler || x.albedoFormat!=y.albedoFormat ||
           x.normalView!=y.normalView || x.normalSampler!=y.normalSampler || x.normalFormat!=y.normalFormat ||
           x.mrView!=y.mrView || x.mrSampler!=y.mrSampler ||
           x.emissiveView!=y.emissiveView || x.emissiveSampler!=y.emissiveSampler || x.emissiveFormat!=y.emissiveFormat || x.emissiveFactor!=y.emissiveFactor ||
           std::memcmp(&x.model,&y.model,sizeof(glm::mat4)) ||
           std::memcmp(&x.color,&y.color,sizeof(glm::vec4)) ||
           std::memcmp(&x.materialParams,&y.materialParams,sizeof(glm::vec4)))return false;
    }
    return true;
}
}

// 世界空间发光三角形光源（实体 emissiveIntensity>0 的模型/内置 cube，以及
// MATT/MATL emissive quad），供 NEE 直接光照采样。布局：lights[0].x = 数量；
// 其后每光源 4 个 vec4：radiance.rgb+三角形面积 / p0 / p1 / p2（外侧绕向）。
// All emissive triangles participate; do not truncate the light distribution.
// Identical sRGB transfer function to vox_rt_color.glsl. Decode palette bytes
// once into a lookup table; emitter construction shares RT's linear radiance.
static glm::vec3 DecodeVoxLinearColor(uint32_t appearance){
    static const std::array<float,256> table=[](){
        std::array<float,256> values{};
        for(size_t i=0;i<values.size();++i){const float s=float(i)/255.0f;
            values[i]=s<=.04045f?s/12.92f:std::pow((s+.055f)/1.055f,2.4f);}
        return values;
    }();
    return {table[(appearance>>8)&255u],table[(appearance>>16)&255u],table[(appearance>>24)&255u]};
}
static bool AppendEmissiveTriangle(std::vector<glm::vec4>& lights,const glm::vec3& p0,
    const glm::vec3& p1,const glm::vec3& p2,const glm::vec3& radiance,
    std::vector<mikan::rt::lights::HitKey>& keys,uint32_t instance,uint32_t geometry,uint32_t primitive) {

    const glm::vec3 cross=glm::cross(p1-p0,p2-p0);
    const float area=glm::length(cross)*0.5f;
    if(area<=1e-10f)return true;
    keys.push_back({instance,geometry,primitive,uint32_t((lights.size()-1)/4)});
    lights.push_back(glm::vec4(radiance,area));
    lights.push_back(glm::vec4(p0,0));
    lights.push_back(glm::vec4(p1,0));
    lights.push_back(glm::vec4(p2,0));
    return true;
}
// Power/area mixture retains support for dim emitters while preferring flux.
// Store the actual float CDF interval, used by both NEE and hit-emission MIS.
static void BuildEmissiveAreaDistribution(std::vector<glm::vec4>& lights) {
    const size_t count=(lights.size()-1)/4;
    double totalArea=0.0,totalPower=0.0;
    for(size_t i=0;i<count;++i){
        const auto& light=lights[1+i*4];
        totalArea+=double(light.w);
        totalPower+=double(light.w)*glm::dot(glm::max(glm::vec3(light),glm::vec3(0)),glm::vec3(.2126f,.7152f,.0722f));
    }
    double cumulative=0.0;float previous=0.0f;
    for(size_t i=0;i<count;++i){
        const size_t base=1+i*4;const auto& light=lights[base];
        const double areaProbability=double(light.w)/totalArea;
        const double power=double(light.w)*glm::dot(glm::max(glm::vec3(light),glm::vec3(0)),glm::vec3(.2126f,.7152f,.0722f));
        cumulative+=totalPower>0.0?0.9*power/totalPower+0.1*areaProbability:areaProbability;
        const float upper=i+1==count?1.0f:float(cumulative);
        lights[base+1].w=upper;lights[base+2].w=upper-previous;previous=upper;
    }
    lights[0].x=float(count);
}

static void AppendVoxEmissiveLights(std::vector<glm::vec4>& lights,const VoxRayTracingGeometry& geometry,
    const glm::mat4& model,const glm::vec3& instanceColor,float instanceEmissive,
    std::vector<mikan::rt::lights::HitKey>& keys,uint32_t instance) {
    const auto* source=geometry.Source();
    if(!source)return;
    if(instanceEmissive<=0.0f && !source->HasEmissiveQuads())return;
    const auto& quads=geometry.Quads();
    const auto& materials=geometry.Materials();
    if(quads.empty() || materials.size()!=quads.size())return;
    const float voxelSize=source->GetVoxelSize();
    const glm::vec3 minBounds=source->GetMinBounds();
    uint32_t geometryIndex=0;
    for(const auto& range:geometry.Ranges()) {
        for(uint32_t i=0;i<range.quadCount;++i) {

            const uint32_t qi=range.firstQuad+i;
            const uint32_t material=materials[qi];
            const float quadStrength=(material&2u)?glm::unpackHalf1x16(uint16_t(material>>16)):0.0f;
            const float strength=quadStrength+instanceEmissive;
            if(strength<=0.0f)continue;
            const auto decoded=source->DecodeQuad(qi);const uint32_t g=decoded.first,a=decoded.second;
            // Exact corners and diagonal from vox_rt_expand.comp. Half identity must
            // remain correct on all six faces, mirrored and nonuniform transforms.
            const auto corners=mikan::rt::lights::VoxCorners(g,a,range.direction,voxelSize,minBounds,model);
            const glm::vec3 radiance=DecodeVoxLinearColor(a)*instanceColor*strength;
            if(!AppendEmissiveTriangle(lights,corners[0],corners[2],corners[1],radiance,keys,instance,geometryIndex,i*2))return;
            AppendEmissiveTriangle(lights,corners[0],corners[3],corners[2],radiance,keys,instance,geometryIndex,i*2+1);
        }
        ++geometryIndex;
    }
}
static void AppendModelEmissiveLights(std::vector<glm::vec4>& lights,const ModelRayTracingGeometry& geometry,
    const ModelRenderer& renderer,const glm::mat4& model,const glm::vec3& tint,float strength,
    std::vector<mikan::rt::lights::HitKey>& keys,uint32_t instance) {
    const auto& mesh=renderer.GetMeshData();
    const auto subIndex=geometry.SubMeshIndex();
    if(subIndex>=mesh.subMeshes.size())return;  // RT geometry covers exactly one submesh
    const auto deformed=geometry.DeformedVertices();
    const auto vertices=geometry.IsDynamic()?deformed:std::span<const Vertex>(mesh.subMeshes[subIndex].vertices);
    const auto& indices=mesh.subMeshes[subIndex].indices;
    // 灯表规模护栏：灯具 submesh 本身是高模（Bistro 单灯 2.3k~3.6k tris，整景
    // 44k 灯导致帧率骤降、相机移动时每帧重建树+重传 2.75MB）。灯具照收，但每个
    // submesh 最多 64 个跨步代表三角形——同材质三角形发光相近，N EE 采样代表性
    // 不变，灯表规模从 44k 降到 ~数百。
    const size_t triangleCount=indices.size()/3;
    size_t stride=1;
    if(triangleCount>64u){
        stride=triangleCount/64u+1u;
        static std::unordered_set<std::string> reportedSkips;
        if(reportedSkips.insert(mesh.subMeshes[subIndex].name+"#"+std::to_string(subIndex)).second)
            LOGI("[HardwareRT] emissive submesh decimated for NEE (tris=%zu stride=%zu): %s",
                 triangleCount,stride,mesh.subMeshes[subIndex].name.c_str());
    }
    // Emissive tint: entity emissive tints by instance color; texture-emissive
    // (emissiveFactor) lamps keep their authored color.
    const glm::vec3 radiance=tint*strength;
    for(size_t t=0;t+2<indices.size();t+=3*stride) {
        if(geometry.DropsPrimitive(uint32_t(t/3)))continue;
        if(!AppendEmissiveTriangle(lights,
            model*glm::vec4(vertices[indices[t]].Position,1),
            model*glm::vec4(vertices[indices[t+1]].Position,1),
            model*glm::vec4(vertices[indices[t+2]].Position,1),radiance,keys,instance,0,uint32_t(t/3)))return;
    }
}


// Conservative clip-space AABB rejection. Unknown bounds remain visible.
// A false result proves that no primary ray can hit any scene instance.
static bool HasPrimaryGeometry(const std::vector<RayTracingHitInstance>* hits,const glm::mat4& vp,uint32_t width,uint32_t height){
    if(!hits)return false;
    for(const auto& hit:*hits){
        glm::vec3 lo,hi;
        if(hit.geometry&&hit.geometry->Source()){lo=hit.geometry->Source()->GetMinBounds();hi=hit.geometry->Source()->GetMaxBounds();}
        else if(hit.modelGeometry&&hit.modelGeometry->GetDeformedBounds(lo,hi)){}
        else if(hit.modelGeometry&&hit.modelGeometry->Source()){
            // Each hit instance is one submesh, not the complete imported model.
            // Whole-model bounds can keep an otherwise sky-only view active.
            const auto& parts=hit.modelGeometry->Source()->GetBVHData().subMeshAABBs;
            if(hit.subMesh>=parts.size())return true;
            const auto& bounds=parts[hit.subMesh];lo=bounds.min;hi=bounds.max;
        }
        else return true;
        for(int i=0;i<3;++i)if(!std::isfinite(lo[i])||!std::isfinite(hi[i])||hi[i]<lo[i])return true;
        lo-=glm::vec3(.001f);hi+=glm::vec3(.001f);
        const glm::mat4 m=vp*hit.model;bool outside[6]={true,true,true,true,true,true};
        const float sx=1.0f+2.0f/float(width),sy=1.0f+2.0f/float(height);
        for(int i=0;i<8;++i){
            const glm::vec4 c=m*glm::vec4((i&1)?hi.x:lo.x,(i&2)?hi.y:lo.y,(i&4)?hi.z:lo.z,1);
            for(int k=0;k<4;++k)if(!std::isfinite(c[k]))return true;
            outside[0]&=c.x < -sx*c.w;outside[1]&=c.x > sx*c.w;
            outside[2]&=c.y < -sy*c.w;outside[3]&=c.y > sy*c.w;
            outside[4]&=c.z < -1e-4f;outside[5]&=c.z > c.w+1e-4f;
        }
        bool rejected=false;for(bool plane:outside)rejected|=plane;
        if(!rejected)return true;
    }
    return false;
}

VkImageView RayTracingViewport::Record(VkCommandBuffer cmd,RayTracingScene& scene,uint32_t slot,uint64_t serial,
    uint32_t viewSlot,uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& proj,
    const glm::vec3& sun,const glm::vec3& radiance,const RayTracingEnvironment& environment){
    Core::CpuStageTrace cpuStageTrace("rt.viewport_record");
    const std::string timingPrefix="rt.view"+std::to_string(viewSlot)+".";
    Core::VulkanGpuScope viewportTiming(cmd,timingPrefix+"viewport_total");
    if(!cmd || !width || !height || !Initialize() || !EnsureEnvironment(cmd))return VK_NULL_HANDLE;
    if(!stbnUploaded){
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        VkBufferCopy copy{0,0,stbnSamples.GetSize()};vkCmdCopyBuffer(cmd,stbnUpload.GetBuffer(),stbnSamples.GetBuffer(),1,&copy);
        VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        ready.srcQueueFamilyIndex=ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ready.buffer=stbnSamples.GetBuffer();ready.offset=0;ready.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&ready,0,nullptr);
        stbnUploaded=true;
    }
    auto& pointer=frames[(uint64_t(slot)<<32)|viewSlot];if(!pointer)pointer=std::make_unique<Frame>();auto& frame=*pointer;
    // The calling frame slot's fence has completed before any resize/upload/descriptor mutation.
    if(frame.width!=width || frame.height!=height)frame.outputInitialized=false;
    if(!EnsureFrame(frame,width,height))return VK_NULL_HANDLE;
    if(!frame.outputInitialized)frame.descriptorBindings.clear();
    // This frame slot is fence-complete. Compare descriptor values, never the
    // contents of the resources: uniforms/history remain free to change.
    bool cacheDescriptors=false;
    auto updateDescriptors=[&](uint32_t count,const VkWriteDescriptorSet* writes){
        if(!cacheDescriptors){vkUpdateDescriptorSets(g_Device,count,writes,0,nullptr);return;}
        Core::CpuStageTrace descriptorTrace("rt.descriptor_updates");
        std::vector<VkWriteDescriptorSet> changed;changed.reserve(count);
        for(uint32_t i=0;i<count;++i){
            const auto& write=writes[i];std::string key;
            auto append=[&](const auto& value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));};
            append(write.dstSet);append(write.dstArrayElement);append(write.descriptorType);append(write.descriptorCount);
            if(write.pImageInfo)for(uint32_t j=0;j<write.descriptorCount;++j){
                const auto& info=write.pImageInfo[j];append(info.sampler);append(info.imageView);append(info.imageLayout);
            }
            if(write.pBufferInfo)for(uint32_t j=0;j<write.descriptorCount;++j){
                const auto& info=write.pBufferInfo[j];append(info.buffer);append(info.offset);append(info.range);
            }
            if(write.descriptorType==VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR){
                const auto* as=static_cast<const VkWriteDescriptorSetAccelerationStructureKHR*>(write.pNext);
                for(uint32_t j=0;j<as->accelerationStructureCount;++j)append(as->pAccelerationStructures[j]);
            }
            auto& previous=frame.descriptorBindings[write.dstBinding];
            if(previous!=key){previous=std::move(key);changed.push_back(write);}
        }
        if(!changed.empty())vkUpdateDescriptorSets(g_Device,uint32_t(changed.size()),changed.data(),0,nullptr);
    };
    auto tlas=scene.GetTlas(slot,serial);const auto* hits=scene.GetHitInstances(slot,serial);
    // Opt-in finite regression: vary a material while GPU slots overlap, without editing assets.
    static const bool validateVersions=[](){const char* v=std::getenv("MIKAN_HWRT_VALIDATE_INPUT_VERSIONS");return v && v[0]=='1';}();
    std::vector<RayTracingHitInstance> validationHits;
    if(validateVersions && hits && !hits->empty()){
        validationHits=*hits;validationHits.front().color.r=(serial/2)%2?.375f:1.0f;hits=&validationHits;
    }
    auto& inputSlot=sceneInputs[slot];
    if(inputSlot.serial!=serial){
        // This slot's fence covers old descriptors in all views, including hidden ones.
        // Retain the fenced slot table: dynamic vertex addresses are slot-local
        // but stable across animation poses. Static tables remain shared.
        inputSlot.serial=serial;inputSlot.checked=false;
    }
    // An empty/unavailable scene releases the active version. In-flight slots
    // still retain their references; camera-only sky views keep the cached scene.
    if(!tlas && latestSceneInputs && latestSceneInputs->owner==&scene)latestSceneInputs.reset();
    if(!tlas)inputSlot.inputs.reset();
    bool valid=false;
    {
        Core::CpuStageTrace visibilityTrace("rt.primary_visibility");
        const auto revision=scene.GetHitRevision(slot,serial);
        const glm::mat4 vp=proj*view;
        const bool reuse=!validateVersions && frame.visibilityKnown &&
            frame.visibilityOwner==&scene && frame.visibilityTlas==tlas &&
            frame.visibilityRevision==revision && frame.visibilityWidth==width &&
            frame.visibilityHeight==height &&
            std::memcmp(&frame.visibilityViewProjection,&vp,sizeof(vp))==0;
        if(reuse){
            Core::CpuStageTrace cacheHit("rt.primary_visibility_cache_hit");
            valid=frame.visibilityResult;
        }else{
            valid=tlas && hits && HasPrimaryGeometry(hits,vp,width,height);
            frame.visibilityOwner=&scene;frame.visibilityTlas=tlas;
            frame.visibilityRevision=revision;frame.visibilityViewProjection=vp;
            frame.visibilityWidth=width;frame.visibilityHeight=height;
            frame.visibilityResult=valid;frame.visibilityKnown=true;
        }
    }
    bool prepareInputs=false;
    if(valid && !inputSlot.checked){
        inputSlot.checked=true;
        if(inputSlot.inputs && inputSlot.inputs->owner==&scene && SameSceneInputs(inputSlot.inputs->sourceHits,*hits)){
            Core::CpuStageTrace slotCacheHit("rt.slot_inputs_cache_hit");
        }else if(latestSceneInputs && latestSceneInputs->owner==&scene && SameSceneInputs(latestSceneInputs->sourceHits,*hits)){
            inputSlot.inputs=latestSceneInputs;
        }else{
            inputSlot.inputs=latestSceneInputs&&latestSceneInputs->owner==&scene?std::make_shared<SceneInputs>(*latestSceneInputs):std::make_shared<SceneInputs>();prepareInputs=true;
            inputSlot.inputs->version=++nextSceneInputVersion;
        }
    }
    // Sky-first views leave checked=false for later geometry-visible views.
    if(!inputSlot.inputs)inputSlot.inputs=std::make_shared<SceneInputs>();
    auto& sceneData=*inputSlot.inputs;frame.sceneInputs=&sceneData;
    // Quality experiment: restore emitter candidates for self ReSTIR DI/GI.
    // MIKAN_HWRT_EMISSIVE_TRIANGLES=0 retains the earlier no-emitter A/B mode.
    static const bool sampleEmissiveTriangles=[](){const char* v=std::getenv("MIKAN_HWRT_EMISSIVE_TRIANGLES");return !v||v[0]!='0';}();
    static const bool reportedEmissiveSampling=[](){LOGI("[HardwareRT] emissive triangle sampling=%s (MIKAN_HWRT_EMISSIVE_TRIANGLES=0 disables)",sampleEmissiveTriangles?"ON":"OFF");return true;}();
    (void)reportedEmissiveSampling;
    if(prepareInputs){
        Core::CpuStageTrace inputsTrace("rt.rebuild_shared_inputs");
        sceneData.serial=serial;sceneData.owner=&scene;sceneData.ready=false;
        std::vector<uint32_t> quads,ranges,instances,quadMaterials,attributes,attributeHeaders;
        std::vector<VoxPlaneRange> planes;
        std::vector<glm::vec4> emissiveLightData(1,glm::vec4(0));  // [0].x = 数量
        std::vector<mikan::rt::lights::HitKey> emitterKeys;
        std::unordered_map<const VoxRayTracingGeometry*,uint32_t> offsets;
        std::vector<VkDescriptorImageInfo> albedoTextures,mrTextures,emissiveTextures,normalTextures;
    
        if(valid)for(const auto& hit:*hits){
            const size_t base=instances.size();instances.resize(base+12);std::memcpy(instances.data()+base,&hit.color,16);
            instances[base+5]=hit.materialFlags; // Preserve mirror/emissive flags in both traversal paths.
            std::memcpy(instances.data()+base+8,&hit.materialParams,16);
            // Entity-level emissive strength (half in bits[31:16]) feeds the light list.
            const float instanceEmissive=(hit.materialFlags&2u)?glm::unpackHalf1x16(uint16_t(hit.materialFlags>>16)):0.0f;
            // Entity-level emissive strength (half in bits[31:16]) feeds the light
            // list, pre-scaled by kModelEmissionGain at pack time. Texture-emissive
            // models (emissiveFactor) also become NEE lights — Bistro-style lamp
            // materials rely on emissive maps with entity intensity 0.
            if(hit.modelGeometry){
                const float textureEmissive=hit.emissiveView?glm::max(glm::max(hit.emissiveFactor.x,hit.emissiveFactor.y),hit.emissiveFactor.z)*kModelEmissionGain:0.0f;
                const float strength=glm::max(instanceEmissive,textureEmissive);
                if(sampleEmissiveTriangles && strength>0.0f && hit.modelGeometry->Source()){
                    const glm::vec3 tint=instanceEmissive>=textureEmissive?glm::vec3(hit.color):hit.emissiveFactor;
                    AppendModelEmissiveLights(emissiveLightData,*hit.modelGeometry,*hit.modelGeometry->Source(),hit.model,tint,strength,emitterKeys,uint32_t(base/12));
                }
                continue;
            }
            const auto* geometry=hit.geometry.get();
            if(sampleEmissiveTriangles)AppendVoxEmissiveLights(emissiveLightData,*geometry,hit.model,glm::vec3(hit.color),instanceEmissive,emitterKeys,uint32_t(base/12));
            auto [it,inserted]=offsets.emplace(geometry,uint32_t(ranges.size()/4));
            if(inserted){const auto& source=geometry->Quads();const auto first=uint32_t(quads.size());
                if(source.size()>UINT32_MAX-quads.size()){valid=false;break;}
                const auto& materialWords=geometry->Materials();
                if(materialWords.size()!=source.size()){valid=false;break;}
                for(const auto& q:geometry->Source()->GetQuads())quads.push_back(q.geometry);
                for(auto plane:geometry->Source()->GetPlaneRanges()){plane.firstQuad+=first;planes.push_back(plane);}
                quadMaterials.insert(quadMaterials.end(),materialWords.begin(),materialWords.end());
                                uint32_t attributeBase=0;
                if(!geometry->Attributes().empty()){
                    const uint32_t offset=uint32_t(attributes.size());attributeBase=offset+1u;
                    attributes.insert(attributes.end(),geometry->Attributes().begin(),geometry->Attributes().end());
                    for(uint32_t k=0;k<3;++k)attributes[offset+k]+=offset;
                    attributes[offset+3]=first;attributeHeaders.push_back(offset);
                }
                for(const auto& r:geometry->Ranges()){ranges.push_back(first+r.firstQuad);ranges.push_back(r.quadCount);ranges.push_back(r.direction);ranges.push_back(attributeBase);}
                const glm::vec4 grid(geometry->Source()->GetMinBounds(),geometry->Source()->GetVoxelSize());
                const auto words=glm::floatBitsToUint(grid);for(uint32_t k=0;k<4;++k)ranges.push_back(words[k]);
                const char* quadOption=std::getenv("MIKAN_VOX_HW_QUAD_AABB");
                if(quadOption&&std::strcmp(quadOption,"1")==0){
                    // GPU query-only cache: two decoded quads per uvec4 (8 B/quad).
                    // Canonical asset/mesh compression remains unchanged.
                    const uint32_t table=uint32_t(ranges.size()/4)+1u;
                    ranges.insert(ranges.end(),{table,first,0u,0u});
                    for(uint32_t q=0;q<source.size();++q){const auto decoded=geometry->Source()->DecodeQuad(q);ranges.push_back(decoded.first);ranges.push_back(decoded.second);}
                    if(source.size()&1u)ranges.insert(ranges.end(),{0u,0u});
                }
            }
            // std430: vec4 color + uvec4 geometry offsets. No descriptor indexing requirement.
            instances[base+4]=it->second;
            instances[base+7]=it->second+uint32_t(geometry->Ranges().size());
        }
                AppendVoxPlaneFooter(quads,std::move(planes));
        // Binding 19 retains legacy material words first, then palette/mapping/R8 data.
        const uint32_t attributeOffset=uint32_t(quadMaterials.size());
        for(const auto& entry:offsets)for(size_t k=0;k<entry.first->Ranges().size();++k){auto& word=ranges[(entry.second+k)*4+3];if(word)word+=attributeOffset;}
        for(uint32_t h:attributeHeaders)for(uint32_t k=0;k<3;++k)attributes[h+k]+=attributeOffset;
        quadMaterials.insert(quadMaterials.end(),attributes.begin(),attributes.end());
        if(valid){
            valid=PrepareModelInputs(sceneData,*hits,instances,albedoTextures,mrTextures,emissiveTextures,normalTextures);
            // Unthrottled enough to diagnose texture binding races; one line per rebuild only.
            static uint32_t loggedPrepVersion=0;
            if(sceneData.version!=loggedPrepVersion || albedoTextures.empty()){
                LOGI("[HardwareRT][PrepModel] version=%llu hits=%zu albedo=%zu mr=%zu emissive=%zu valid=%u",
                    static_cast<unsigned long long>(sceneData.version),hits->size(),albedoTextures.size(),mrTextures.size(),emissiveTextures.size(),uint32_t(valid));
                loggedPrepVersion=sceneData.version;
            }
        }
        BuildEmissiveAreaDistribution(emissiveLightData);
        const uint32_t emitterCount=uint32_t((emissiveLightData.size()-1)/4);
        {
            // One-shot diagnostics: surface emissive materials reached the light list or not.
            static uint32_t loggedLightCount=UINT32_MAX;
            const uint32_t lightCount=uint32_t((emissiveLightData.size()-1)/4);
            if(valid && lightCount!=loggedLightCount){
                uint32_t quadEmissive=0;for(size_t i=0;i<attributeOffset;++i)if(quadMaterials[i]&2u)++quadEmissive;
                LOGI("[HardwareRT] emissive quads=%u lights=%u mirrorQuads=%u",quadEmissive,lightCount,[&]{uint32_t n=0;for(size_t i=0;i<attributeOffset;++i)if(quadMaterials[i]&1u)++n;return n;}());
                loggedLightCount=lightCount;
            }
        }
        if(valid && !mikan::rt::lights::AppendIndex(emissiveLightData,emitterCount,std::move(emitterKeys))){
            LOGE("[HardwareRT] emissive primitive index build failed; rejecting incomplete scene inputs");valid=false;
        }
        std::vector<uint32_t> emissiveLightWords(emissiveLightData.size()*4);
        if(emissiveLightData.size()>0)std::memcpy(emissiveLightWords.data(),emissiveLightData.data(),emissiveLightData.size()*sizeof(glm::vec4));
        if(valid)valid=Upload(sceneData.quads,sceneData.cachedQuads,quads)&&Upload(sceneData.ranges,sceneData.cachedRanges,ranges)&&Upload(sceneData.instances,sceneData.cachedInstances,instances)&&Upload(sceneData.quadMaterials,sceneData.cachedQuadMaterials,quadMaterials)&&Upload(sceneData.emissiveLights,sceneData.cachedEmissiveLights,emissiveLightWords);
        std::vector<uint32_t> softwareWords(4u,0u);
        if(valid&&softwareQuadBvhEnabled&&!mikan::rt::softquad::Build(*hits,softwareWords)){
            LOGE("[SoftwareQuadBVH] VOX-only path: unsupported model or singular instance transform");valid=false;
        }
        if(valid)valid=Upload(sceneData.softwareBvh,sceneData.cachedSoftwareBvh,softwareWords);
        sceneData.albedoTextures=std::move(albedoTextures);sceneData.mrTextures=std::move(mrTextures);sceneData.emissiveTextures=std::move(emissiveTextures);sceneData.normalTextures=std::move(normalTextures);
        sceneData.lightCount=emitterCount;
        sceneData.lightSignature=1469598103934665603ull;
        for(const auto word:emissiveLightWords)sceneData.lightSignature=(sceneData.lightSignature^word)*1099511628211ull;
        sceneData.ready=valid;
        if(valid){sceneData.sourceHits=*hits;latestSceneInputs=inputSlot.inputs;}
    }
    valid=valid && sceneData.ready;
    static const bool traceInputs=[](){const char* v=std::getenv("MIKAN_HWRT_TRACE_SHARED_INPUTS");return v && v[0]=='1';}();
    static uint32_t tracedInputs=0;
    if(traceInputs && tracedInputs<12){
        LOGI("[HardwareRT][SharedInputs] serial=%llu slot=%u view=%u prepared=%u valid=%u lights=%u version=%llu",static_cast<unsigned long long>(serial),slot,viewSlot,uint32_t(prepareInputs),uint32_t(valid),valid?sceneData.lightCount:0u,static_cast<unsigned long long>(sceneData.version));
        LOGI("[HardwareRT][SharedInputsBuffers] quads=%p instances=%p models=%p",reinterpret_cast<void*>(sceneData.quads.GetBuffer()),reinterpret_cast<void*>(sceneData.instances.GetBuffer()),reinterpret_cast<void*>(sceneData.models.GetBuffer()));
        ++tracedInputs;
    }
    const auto& quadMaterials=sceneData.cachedQuadMaterials;
    const auto& albedoTextures=sceneData.albedoTextures;
    const auto& mrTextures=sceneData.mrTextures;
    const auto& emissiveTextures=sceneData.emissiveTextures;
    const auto& normalTextures=sceneData.normalTextures;
    if(!frame.environment.GetBuffer() && !frame.environment.Create(sizeof(glm::vec4),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return VK_NULL_HANDLE;
    frame.environment.Write(&environment.tintIntensity,sizeof(glm::vec4));
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.image=frame.output.GetImage();image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.oldLayout=frame.outputInitialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;image.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    image.srcAccessMask=frame.outputInitialized?VK_ACCESS_SHADER_READ_BIT:0;image.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,frame.outputInitialized?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
    if(!valid){
        cacheDescriptors=true;
        Core::CpuStageTrace skyTrace("rt.sky_record");
        // No sky shader invocation reads geometry, reservoirs or denoiser guides.
        // Invalidate temporal state once, then leave those resources untouched.
        if(auto it=histories.find(viewSlot);it!=histories.end() && it->second){
            auto& history=*it->second;
            if(history.serial!=UINT64_MAX){
                if(history.denoiser)history.denoiser->ResetHistory();
                if(history.specularDenoiser)history.specularDenoiser->ResetHistory();
            }
            history.serial=UINT64_MAX;history.lightingValid=false;history.ptValid=false;
            history.irradianceValid=false;history.taaInitialized[0]=history.taaInitialized[1]=false;
        }
        frame.rrResolved=VK_NULL_HANDLE;
        const bool sky=environment.IsValid();
        VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,frame.output.GetView(),VK_IMAGE_LAYOUT_GENERAL},
            {sky?environment.sampler:fallbackSampler,sky?environment.panorama:fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkDescriptorBufferInfo buffers[]={{sky?environment.irradiance:fallbackIrradiance.GetBuffer(),0,144},{frame.environment.GetBuffer(),0,sizeof(glm::vec4)}};
        VkWriteDescriptorSet writes[4]{};
        for(uint32_t i=0;i<4;++i){
            auto& write=writes[i];write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.descriptor;
            write.dstBinding=4+i;write.descriptorCount=1;
            write.descriptorType=i==0?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:(i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            if(i<2)write.pImageInfo=&images[i];else write.pBufferInfo=&buffers[i-2];
        }
        updateDescriptors(4,writes);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
        struct SkyPush {glm::mat4 inverseViewProj;glm::vec4 eye,sun,light,extent;};
        const SkyPush push{glm::inverse(proj*view),glm::vec4(glm::vec3(glm::inverse(view)[3]),environment.altitudeMeters),glm::vec4(sun,0),glm::vec4(radiance,0),glm::vec4(width,height,sky?1:0,kSceneExposure)};
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,skyPipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.descriptor,0,nullptr);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        {Core::VulkanGpuScope skyTiming(cmd,timingPrefix+"sky_only");vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);}
        Core::PipelineCapture::GetInstance().Record(cmd,frame.output.GetImage(),VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_GENERAL,width,height,serial,viewSlot,"lighting-base-hdr","engine/shaders/glsl/vox_rt_sky.comp","hdr");
        image.oldLayout=VK_IMAGE_LAYOUT_GENERAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
        frame.outputInitialized=true;return frame.output.GetView();
    }
    if(valid){
        // Full lighting writes these bindings directly; retire the sky cache.
        frame.descriptorBindings.clear();
        VkDescriptorBufferInfo buffers[]={{sceneData.quads.GetBuffer(),0,VK_WHOLE_SIZE},{sceneData.ranges.GetBuffer(),0,VK_WHOLE_SIZE},{sceneData.instances.GetBuffer(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&tlas;
        VkDescriptorImageInfo output{};output.imageView=frame.output.GetView();output.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[5]{};
        for(uint32_t i=0;i<5;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:(i==4?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);if(i==0)writes[i].pNext=&as;else if(i==4)writes[i].pImageInfo=&output;else writes[i].pBufferInfo=&buffers[i-1];}
        updateDescriptors(5,writes);
        VkDescriptorBufferInfo softwareInfo{sceneData.softwareBvh.GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet softwareWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};softwareWrite.dstSet=frame.descriptor;softwareWrite.dstBinding=41;softwareWrite.descriptorCount=1;softwareWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;softwareWrite.pBufferInfo=&softwareInfo;
        updateDescriptors(1,&softwareWrite);
        VkDescriptorBufferInfo modelInfo{sceneData.models.GetBuffer(),0,VK_WHOLE_SIZE};
        // Bindless tables: write only the unique textures in use. Shaders
        // access slots strictly below these counts (PARTIALLY_BOUND layout).
        const uint32_t textureCounts[4]={uint32_t(albedoTextures.size()),uint32_t(mrTextures.size()),uint32_t(emissiveTextures.size()),uint32_t(normalTextures.size())};
        VkWriteDescriptorSet modelWrites[5]{};uint32_t modelWriteCount=0;
        modelWrites[modelWriteCount++]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};modelWrites[0].dstSet=frame.descriptor;modelWrites[0].dstBinding=8;modelWrites[0].descriptorCount=1;modelWrites[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;modelWrites[0].pBufferInfo=&modelInfo;
        for(uint32_t i=0;i<4;++i){
            if(!textureCounts[i])continue;
            VkWriteDescriptorSet& write=modelWrites[modelWriteCount++];
            write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.descriptor;
            write.dstBinding=i==0?9u:(i==1?21u:(i==2?38u:58u));write.descriptorCount=textureCounts[i];
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo=(i==0?albedoTextures.data():(i==1?mrTextures.data():(i==2?emissiveTextures.data():normalTextures.data())));
        }
        updateDescriptors(modelWriteCount,modelWrites);
        // RR/SR guide preparation also reads the tagged grid records. Bind a
        // valid empty header in triangle mode, independent of shader branches.
        const auto* ddaInput=scene.GetDdaGrid(slot,serial);if(!ddaInput)return VK_NULL_HANDLE;
        VkDescriptorBufferInfo ddaInputInfo{ddaInput->GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet ddaInputWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};ddaInputWrite.dstSet=frame.descriptor;ddaInputWrite.dstBinding=40;ddaInputWrite.descriptorCount=1;ddaInputWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;ddaInputWrite.pBufferInfo=&ddaInputInfo;
        updateDescriptors(1,&ddaInputWrite);
        if(voxelDDAEnabled){
            const auto* gridBuffer=scene.GetDdaGrid(slot,serial);if(!gridBuffer)return VK_NULL_HANDLE;
            if(ddaRegistryVersion==0){
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=ddaDummyImage;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);ddaRegistryVersion=1;
            }
            const auto& grids=mikan::render::VoxelDDARegistry::Get().Grids();
            VkDescriptorImageInfo images[8]{};for(uint32_t i=0;i<8;++i)images[i]={VK_NULL_HANDLE,i<grids.size()&&grids[i].imageView?(VkImageView)grids[i].imageView:ddaDummyView,VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo gridInfo{gridBuffer->GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet ddaWrites[2]{};for(auto& w:ddaWrites){w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=frame.descriptor;}
            ddaWrites[0].dstBinding=39;ddaWrites[0].descriptorCount=8;ddaWrites[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;ddaWrites[0].pImageInfo=images;
            ddaWrites[1].dstBinding=40;ddaWrites[1].descriptorCount=1;ddaWrites[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;ddaWrites[1].pBufferInfo=&gridInfo;
            updateDescriptors(2,ddaWrites);
        }        if(!quadMaterials.empty()){
            VkDescriptorBufferInfo quadMaterialInfo{sceneData.quadMaterials.GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet quadMaterialWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            quadMaterialWrite.dstSet=frame.descriptor;quadMaterialWrite.dstBinding=19;quadMaterialWrite.descriptorCount=1;
            quadMaterialWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;quadMaterialWrite.pBufferInfo=&quadMaterialInfo;
            updateDescriptors(1,&quadMaterialWrite);
        }
        {
            VkDescriptorBufferInfo emissiveInfo{sceneData.emissiveLights.GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet emissiveWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            emissiveWrite.dstSet=frame.descriptor;emissiveWrite.dstBinding=20;emissiveWrite.descriptorCount=1;
            emissiveWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;emissiveWrite.pBufferInfo=&emissiveInfo;
            updateDescriptors(1,&emissiveWrite);
        }
        // Material uploads originally target raster fragment reads. Also make them visible to compute.
        VkMemoryBarrier sampled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};sampled.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;sampled.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&sampled,0,nullptr,0,nullptr);
        RayTracingQueryBarrier(cmd);
    }
    auto& historyPointer=histories[viewSlot];if(!historyPointer)historyPointer=std::make_unique<ViewHistory>();
    auto& history=*historyPointer;
    const auto dlssOutput=mikan::denoising::GetDlssOutputResolution(viewSlot,{width,height});
    if(history.outputExtent.width!=dlssOutput.width||history.outputExtent.height!=dlssOutput.height){
        history.rayReconstruction.reset();history.superResolution.reset();history.rrAttempted=history.srAttempted=false;history.serial=UINT64_MAX;history.outputExtent=dlssOutput;
    }
    const uint64_t lightSignature=valid?sceneData.lightSignature:1469598103934665603ull;
    // Reservoir entries can refer to changed light indices or cached radiance.
    // Invalidate those sampling caches without clearing NRD/RR/SR/TAA history.
    if(lightSignature!=history.transportLightSignature){history.lightingValid=false;history.ptValid=false;}
    history.transportLightSignature=lightSignature;
    // PT stores selected path contributions: invalidate when lighting inputs change.
    uint64_t ptLighting=lightSignature;
    auto mixPt=[&](float v){uint32_t bits;std::memcpy(&bits,&v,sizeof(bits));ptLighting=(ptLighting^bits)*1099511628211ull;};
    for(int c=0;c<3;++c){mixPt(sun[c]);mixPt(radiance[c]);}
    for(int c=0;c<4;++c)mixPt(environment.tintIntensity[c]);
    mixPt(environment.altitudeMeters);
    if(ptLighting!=history.lightingSignature){history.ptValid=false;history.irradianceValid=false;}
    history.lightingSignature=ptLighting;
    // Sun direction/intensity and sky settings may animate every frame.
    // Let denoisers reject obsolete radiance instead of restarting accumulation.
    const uint32_t lightCountTotal=valid?sceneData.lightCount:0;
    const std::vector<RayTracingHitInstance> emptyHits;
    static const bool traceHistory=[](){const char* v=std::getenv("MIKAN_HWRT_TRACE_HISTORY");return v&&v[0]=='1';}();
    const auto previousSerial=history.serial;
    const bool lightingInvalid=!history.lightingValid;
    if(!PrepareDenoising(frame,history,valid?*hits:emptyHits,serial,width,height,view,proj,lightCountTotal))return VK_NULL_HANDLE;
    if(traceHistory){
        // Diagnostic counters only; rendering resources and temporal state are per view.
        static std::unordered_map<uint32_t,uint32_t> reports;
        if(reports[viewSlot]++<64)LOGI("[HardwareRT][History] view=%u serial=%llu previous=%llu reset=%u lightingInvalid=%u valid=%u altitude=%.5f history=%p nrd=%p set=%p temporal=%p depth=%p motion=%p",
            viewSlot,static_cast<unsigned long long>(serial),static_cast<unsigned long long>(previousSerial),
            uint32_t(frame.taaReset),uint32_t(lightingInvalid),uint32_t(valid),environment.altitudeMeters,
            static_cast<void*>(&history),static_cast<void*>(history.denoiser.get()),
            reinterpret_cast<void*>(frame.descriptor),reinterpret_cast<void*>(frame.temporal.GetBuffer()),
            reinterpret_cast<void*>(frame.viewZ.GetImage()),reinterpret_cast<void*>(frame.motion.GetImage()));
    }
    // Serialize previous reads and writes with this view's ping-pong history.
    VkDescriptorBufferInfo ptBuffers[]={
        {history.ptHistory[history.ptRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.ptScratch[0].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.ptScratch[1].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.ptHistory[1-history.ptRead].GetBuffer(),0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet ptWrites[4]{};
    for(uint32_t i=0;i<4;++i){ptWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ptWrites[i].dstSet=frame.descriptor;ptWrites[i].dstBinding=42+i;
        ptWrites[i].descriptorCount=1;ptWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;ptWrites[i].pBufferInfo=&ptBuffers[i];}
    updateDescriptors(4,ptWrites);
    VkDescriptorBufferInfo enhancedBuffers[]={
        {history.enhancedQueue.GetBuffer(),0,VK_WHOLE_SIZE},
        {history.enhancedWork.GetBuffer(),0,VK_WHOLE_SIZE},
        {enhancedPairs.GetBuffer(),0,VK_WHOLE_SIZE},
        {history.enhancedDuplicates.GetBuffer(),0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet enhancedWrites[4]{};
    for(uint32_t i=0;i<4;++i){enhancedWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};enhancedWrites[i].dstSet=frame.descriptor;
        enhancedWrites[i].dstBinding=49+i;enhancedWrites[i].descriptorCount=1;enhancedWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;enhancedWrites[i].pBufferInfo=&enhancedBuffers[i];}
    updateDescriptors(4,enhancedWrites);
    VkDescriptorBufferInfo cacheBuffers[]={
        {history.irradianceCache[history.irradianceRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.irradianceCache[1-history.irradianceRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.irradianceLocks.GetBuffer(),0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet cacheWrites[3]{};
    for(uint32_t i=0;i<3;++i){cacheWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};cacheWrites[i].dstSet=frame.descriptor;
        cacheWrites[i].dstBinding=46+i;cacheWrites[i].descriptorCount=1;cacheWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;cacheWrites[i].pBufferInfo=&cacheBuffers[i];}
    updateDescriptors(3,cacheWrites);
    if(valid&&history.irradianceActive){
        Core::VulkanGpuScope cacheTiming(cmd,timingPrefix+"irradiance_cache_prepare");
        VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        transfer.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        transfer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&transfer,0,nullptr,0,nullptr);
        if(!history.irradianceValid)vkCmdFillBuffer(cmd,cacheBuffers[0].buffer,0,ViewHistory::irradianceBytes,0);
        transfer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transfer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&transfer,0,nullptr,0,nullptr);
        VkBufferCopy copy{0,0,ViewHistory::irradianceBytes};vkCmdCopyBuffer(cmd,cacheBuffers[0].buffer,cacheBuffers[1].buffer,1,&copy);
        vkCmdFillBuffer(cmd,cacheBuffers[2].buffer,0,VK_WHOLE_SIZE,0);
        transfer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transfer.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&transfer,0,nullptr,0,nullptr);
    }
    VkMemoryBarrier ptReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    ptReady.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    ptReady.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ptReady,0,nullptr,0,nullptr);
    const VulkanImage* guides[]={&frame.diffuse,&frame.viewZ,&frame.normal,&frame.motion,&frame.material,&frame.specular,&frame.specularMaterial};
    VkImageMemoryBarrier guideBarriers[7]{};
    for(uint32_t i=0;i<7;++i){
        auto& barrier=guideBarriers[i];barrier={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=guides[i]->GetImage();
        barrier.oldLayout=frame.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=frame.initialized?VK_ACCESS_SHADER_READ_BIT:0;barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,7,guideBarriers);
    VkImageMemoryBarrier taaGuideBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    taaGuideBarrier.image=frame.taaGuide.GetImage();taaGuideBarrier.oldLayout=frame.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    taaGuideBarrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;taaGuideBarrier.srcAccessMask=frame.initialized?VK_ACCESS_SHADER_READ_BIT:0;
    taaGuideBarrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;taaGuideBarrier.srcQueueFamilyIndex=taaGuideBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    taaGuideBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&taaGuideBarrier);
    VkDescriptorImageInfo taaGuideInfo{VK_NULL_HANDLE,frame.taaGuide.GetView(),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet taaGuideWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};taaGuideWrite.dstSet=frame.descriptor;
    taaGuideWrite.dstBinding=26;taaGuideWrite.descriptorCount=1;taaGuideWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;taaGuideWrite.pImageInfo=&taaGuideInfo;
    updateDescriptors(1,&taaGuideWrite);
    const bool sky=environment.IsValid();
    VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,frame.output.GetView(),VK_IMAGE_LAYOUT_GENERAL},
        {sky?environment.sampler:fallbackSampler,sky?environment.panorama:fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo environmentBuffers[]={{sky?environment.irradiance:fallbackIrradiance.GetBuffer(),0,144},{frame.environment.GetBuffer(),0,sizeof(glm::vec4)}};
    VkWriteDescriptorSet writes[4]{};
    for(uint32_t i=0;i<4;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptor;writes[i].dstBinding=4+i;writes[i].descriptorCount=1;
        writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:(i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        if(i<2)writes[i].pImageInfo=&images[i];else writes[i].pBufferInfo=&environmentBuffers[i-2];}
    updateDescriptors(4,writes);
    VkDescriptorBufferInfo noiseInfo{stbnSamples.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet noiseWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    noiseWrite.dstSet=frame.descriptor;noiseWrite.dstBinding=17;noiseWrite.descriptorCount=1;
    noiseWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;noiseWrite.pBufferInfo=&noiseInfo;
    updateDescriptors(1,&noiseWrite);
    const bool physicalSun=sky && environment.transmittance;
    VkDescriptorImageInfo sunImage{physicalSun?environment.sampler:fallbackSampler,
        physicalSun?environment.transmittance:fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet sunWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};sunWrite.dstSet=frame.descriptor;
    sunWrite.dstBinding=18;sunWrite.descriptorCount=1;sunWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;sunWrite.pImageInfo=&sunImage;
    updateDescriptors(1,&sunWrite);
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};inputs.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;inputs.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&inputs,0,nullptr,0,nullptr);
    struct Push {glm::mat4 inverseViewProj;glm::vec4 eye,sun,light,extent;};static_assert(sizeof(Push)==128);
    // Positive UV jitter moves the ray sample, so clip-space geometry shifts negatively.
    glm::mat4 rayProjection=proj;
    for(int column=0;column<4;++column){
        rayProjection[column][0]-=2.0f*frame.taaJitter.x*proj[column][3];
        rayProjection[column][1]-=2.0f*frame.taaJitter.y*proj[column][3];
    }
    Push push{glm::inverse(rayProjection*view),glm::vec4(glm::vec3(glm::inverse(view)[3]),environment.altitudeMeters),glm::vec4(sun,physicalSun?1:0),glm::vec4(radiance,0),glm::vec4(width,height,sky?1:0,kSceneExposure)};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,valid?(mikan::rt::GetNeeDirectSamples()==1u?performancePipeline:(mikan::rt::GetNeeDirectSamples()==2u?balancedPipeline:pipeline)):skyPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.descriptor,0,nullptr);
    static const bool hybridSun=[](){const char* v=std::getenv("MIKAN_HWRT_HYBRID_SUN");const char* sun=std::getenv("MIKAN_HWRT_PRIMARY_SUN_ONLY");const char* full=std::getenv("MIKAN_HWRT_HYBRID");return ((full&&full[0]=='1')||(v&&v[0]=='1'&&sun&&sun[0]=='1'))&&Core::VulkanGpuProfiler::IsRequested();}();
    if(hybridSun){
        if(viewSlot!=1||width!=g_GameRenderTarget.GetWidth()||height!=g_GameRenderTarget.GetHeight()){
            LOGE("[Hybrid Sun] requires game-view MRT extent to match RT extent");return VK_NULL_HANDLE;
        }
        VkDescriptorImageInfo gbufferImages[5]{};
        VkWriteDescriptorSet gbufferWrites[5]{};
        for(uint32_t i=0;i<5;++i){
            gbufferImages[i]={g_GameRenderTarget.GetSampler(),i==0?g_GameRenderTarget.GetDepthImageView():g_GameRenderTarget.GetColorImageView(i-1),
                i==0?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& write=gbufferWrites[i];write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.descriptor;
            write.dstBinding=53+i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&gbufferImages[i];
        }
        updateDescriptors(5,gbufferWrites);
        VkDescriptorImageInfo transportImage{g_GameRenderTarget.GetSampler(),g_GameRenderTarget.GetCompositeImageView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet transportWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};transportWrite.dstSet=frame.descriptor;
        transportWrite.dstBinding=59;transportWrite.descriptorCount=1;transportWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;transportWrite.pImageInfo=&transportImage;
        updateDescriptors(1,&transportWrite);
    }
    static const bool tileCull=[](){const char* v=std::getenv("MIKAN_HWRT_TILE_CULL");return !v||v[0]!='0';}();
    if(valid&&!hybridSun){
        // Reset the indirect command (x=0,y=z=1); tile payload is overwritten.
        const auto primaryTiming=Core::g_VulkanGpuProfiler.BeginScope(cmd,(timingPrefix+"primary_hits").c_str(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if(const char* stats=std::getenv("MIKAN_VOX_QUAD_STATS");stats&&std::strcmp(stats,"1")==0){
            const VkDeviceSize offset=VkDeviceSize(width)*height*24u;
            vkCmdFillBuffer(cmd,frame.primaryHits.GetBuffer(),offset,6u*24u,0u);
            VkBufferMemoryBarrier reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            reset.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;reset.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            reset.srcQueueFamilyIndex=reset.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            reset.buffer=frame.primaryHits.GetBuffer();reset.offset=offset;reset.size=6u*24u;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&reset,0,nullptr);
        }
        vkCmdFillBuffer(cmd,frame.activeTiles.GetBuffer(),0,4,0);
        vkCmdFillBuffer(cmd,frame.activeTiles.GetBuffer(),4,8,1);
        VkBufferMemoryBarrier tileReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        tileReset.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;tileReset.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        tileReset.srcQueueFamilyIndex=tileReset.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        tileReset.buffer=frame.activeTiles.GetBuffer();tileReset.offset=0;tileReset.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&tileReset,0,nullptr);
        // DDA uses specialization constant 11; light.z remains the sun blue channel.

        push.light.w=1.0f;
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
        VkMemoryBarrier samplingReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        samplingReady.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        samplingReady.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&samplingReady,0,nullptr,0,nullptr);
        VkBufferMemoryBarrier tileReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        tileReady.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;tileReady.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
        tileReady.srcQueueFamilyIndex=tileReady.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        tileReady.buffer=frame.activeTiles.GetBuffer();tileReady.offset=0;tileReady.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&tileReady,0,nullptr);
        Core::g_VulkanGpuProfiler.EndScope(cmd,primaryTiming);
        push.light.w=tileCull?0.0f:-1.0f;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mikan::rt::GetNeeDirectSamples()==1u?performancePipeline:(mikan::rt::GetNeeDirectSamples()==2u?balancedPipeline:pipeline));
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.descriptor,0,nullptr);
        static bool reported=false;if(!reported){
            LOGI("[HardwareRT] DI path=%s",history.ptActive?"ReSTIR Enhanced Hybrid/Compact":"NEE/MIS");
            reported=true;
        }
    }
    if(hybridSun)push.light.w=-1.0f; // Full-screen G-buffer shading, no primary/tile dispatch.
    {
    static const bool primarySunOnly=[] {const char* v=std::getenv("MIKAN_HWRT_PRIMARY_SUN_ONLY");return v&&(v[0]=='1'||v[0]=='2');}();
    Core::VulkanGpuScope lightingTiming(cmd,timingPrefix+(valid?(primarySunOnly?"lighting_primary_sun":"lighting_di_gi_specular"):"sky_only"));
    if(valid&&history.ptActive){
            const char* stages[]={"enhanced_generate","enhanced_temporal_workload","enhanced_temporal_retrace","enhanced_temporal_merge",
                "enhanced_spatial_workload","enhanced_spatial_retrace","enhanced_spatial_shift","enhanced_spatial_merge","enhanced_resolve","enhanced_duplicate_map"};
            for(uint32_t step=0;step<10;++step){
                const uint32_t stage=step==0u?9u:step-1u;
                static const bool duplicateCap=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_PT_DUPLICATE_CAP");return !v||v[0]!='0';}();
                if(stage==9u&&(!history.ptValid||!mikan::rt::GetRestirTemporalReuse()||!duplicateCap))continue;
                if((stage==1u||stage==2u)&&(!history.ptValid||!mikan::rt::GetRestirTemporalReuse()))continue;
                static const bool spatialReuse=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_PT_SPATIAL");return !v||v[0]!='0';}();
                if(stage>=4u&&stage<=6u&&!spatialReuse)continue;
                Core::VulkanGpuScope stageTiming(cmd,timingPrefix+stages[stage]);
                if(stage==1u||stage==4u){
                    VkMemoryBarrier resetBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};resetBarrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                    resetBarrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&resetBarrier,0,nullptr,0,nullptr);
                    const uint32_t control[]={0u,1u,1u,0u};vkCmdUpdateBuffer(cmd,history.enhancedQueue.GetBuffer(),0,sizeof(control),control);
                    resetBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;resetBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&resetBarrier,0,nullptr,0,nullptr);
                }
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,enhancedPipelines[stage]);
                push.light.w=-8.0f-float(stage);vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
                if(stage==2u||stage==5u)vkCmdDispatchIndirect(cmd,history.enhancedQueue.GetBuffer(),0);
                else vkCmdDispatch(cmd,(width+7u)/8u,(height+7u)/8u,1);
                if(stage!=8u){VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    ready.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                    ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&ready,0,nullptr,0,nullptr);}
            }
        history.ptValid=true;
    }else{
    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    if(valid&&tileCull&&!hybridSun)vkCmdDispatchIndirect(cmd,frame.activeTiles.GetBuffer(),0);
    else vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    }
    }
    if(valid&&history.irradianceActive){
        if(frame.irradianceReadback.GetBuffer()){
            VkMemoryBarrier statsReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};statsReady.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;statsReady.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&statsReady,0,nullptr,0,nullptr);
            VkBufferCopy statsCopy{65536u*4u,0,16};vkCmdCopyBuffer(cmd,history.irradianceLocks.GetBuffer(),frame.irradianceReadback.GetBuffer(),1,&statsCopy);
            statsReady.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;statsReady.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&statsReady,0,nullptr,0,nullptr);
            frame.irradianceStatsPending=true;
        }
        history.irradianceRead=1-history.irradianceRead;history.irradianceValid=true;
    }
    if(!valid){history.ptValid=false;history.irradianceValid=false;}
    history.ptRead=1-history.ptRead;history.lightingValid=valid;
    auto& capture=Core::PipelineCapture::GetInstance();
    if(capture.Wants(serial,viewSlot)){
        if(hybridSun)capture.Record(cmd,g_GameRenderTarget.GetCompositeImage(),VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,width,height,serial,viewSlot,"raster-transport","engine/shaders/glsl/model.frag","hdr");
        auto snapshot=[&](const VulkanImage& resource,VkFormat format,const char* name,const char* interpretation){
            capture.Record(cmd,resource.GetImage(),format,VK_IMAGE_LAYOUT_GENERAL,width,height,serial,viewSlot,
                name,"engine/shaders/glsl/vox_rt_realtime.comp",interpretation);
        };
        snapshot(frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,"lighting-base-hdr","hdr");
        // The sky-only branch does not populate these lighting/geometry resources.
        if(valid){
            snapshot(frame.diffuse,VK_FORMAT_R16G16B16A16_SFLOAT,"lighting-diffuse-di-gi","hdr");
            snapshot(frame.specular,VK_FORMAT_R16G16B16A16_SFLOAT,"lighting-specular","hdr");
            snapshot(frame.normal,VK_FORMAT_R16G16B16A16_SFLOAT,"guide-world-normal","signed");
            snapshot(frame.viewZ,VK_FORMAT_R32_SFLOAT,"guide-view-depth","depth");
            snapshot(frame.motion,VK_FORMAT_R16G16_SFLOAT,"guide-motion","signed");
            snapshot(frame.material,VK_FORMAT_R16G16B16A16_SFLOAT,"guide-diffuse-material","unit");
            snapshot(frame.specularMaterial,VK_FORMAT_R16G16B16A16_SFLOAT,"guide-specular-material","unit");
        }
    }
    DenoiseAndComposite(cmd,frame,history,slot,viewSlot,view,proj,valid);
    image.oldLayout=VK_IMAGE_LAYOUT_GENERAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkImageView resolved;
    {
        Core::VulkanGpuScope resolveTiming(cmd,timingPrefix+"taa_or_upscale");
        resolved=ResolveTAA(cmd,frame,history,valid);
    }
    if(viewSlot==(g_RunMode==RunMode::Editor?0:1))Core::DlssFG::SubmitGuides(cmd,frame.taaGuide.GetView(),width,height,view,proj,glm::vec2(frame.taaJitter),frame.taaReset);
    if(capture.Wants(serial,viewSlot)){
        capture.Record(cmd,frame.output.GetImage(),VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            width,height,serial,viewSlot,frame.rrResolved?"base-hdr-rr-bypass":"composite-hdr",
            frame.rrResolved?"engine/shaders/glsl/vox_rt_realtime.comp":"engine/shaders/glsl/vox_rt_composite.comp","hdr");
        if(history.superResolution){
            const auto output=history.superResolution->Output();
            if(resolved==output.view && resolved)capture.Record(cmd,output.image,VK_FORMAT_R16G16B16A16_SFLOAT,
                output.layout,history.outputExtent.width,history.outputExtent.height,serial,viewSlot,
                "dlss-sr-resolved-hdr","DLSS SR Preset K (vendor evaluation)","hdr");
        }
        if(resolved==history.taaHistory[history.taaRead].GetView() && resolved){
            capture.Record(cmd,history.taaHistory[history.taaRead].GetImage(),VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,width,height,serial,viewSlot,
                "taa-hdr","engine/shaders/glsl/vox_rt_taa.comp","hdr");
        }
    }
    frame.initialized=true;frame.outputInitialized=true;return resolved;
}
