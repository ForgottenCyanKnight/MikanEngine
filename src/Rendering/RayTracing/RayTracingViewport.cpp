#include "Core/EngineGlobal.h"
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
#include "EmissiveLightTree.h"
#include "SoftwareQuadBvh.h"

namespace {
// Compare fields, not struct padding. RayTracingScene replaces immutable geometry generations.
bool SameSceneInputs(const std::vector<RayTracingHitInstance>& a,const std::vector<RayTracingHitInstance>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];
        if(x.rayMask!=y.rayMask || x.entity!=y.entity || x.geometry!=y.geometry || x.modelGeometry!=y.modelGeometry || x.materialFlags!=y.materialFlags ||
           x.albedoView!=y.albedoView || x.albedoSampler!=y.albedoSampler || x.albedoFormat!=y.albedoFormat ||
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
    const ModelRenderer& renderer,const glm::mat4& model,const glm::vec3& instanceColor,float strength,
    std::vector<mikan::rt::lights::HitKey>& keys,uint32_t instance) {
    const auto& mesh=renderer.GetMeshData();
    if(mesh.subMeshes.size()!=1)return;  // RT geometry only covers the first single submesh
    const auto& vertices=mesh.subMeshes[0].vertices;
    const auto& indices=mesh.subMeshes[0].indices;
    const glm::vec3 radiance=instanceColor*strength;
    for(size_t t=0;t+2<indices.size();t+=3) {
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
        else if(hit.modelGeometry&&hit.modelGeometry->Source()){const auto bounds=hit.modelGeometry->Source()->GetAABB();lo=bounds.min;hi=bounds.max;}
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
    if(!EnsureFrame(frame,width,height))return VK_NULL_HANDLE;
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
        inputSlot.inputs.reset();inputSlot.serial=serial;inputSlot.checked=false;
    }
    // An empty/unavailable scene releases the active version. In-flight slots
    // still retain their references; camera-only sky views keep the cached scene.
    if(!tlas && latestSceneInputs && latestSceneInputs->owner==&scene)latestSceneInputs.reset();
    bool valid=tlas && hits && HasPrimaryGeometry(hits,proj*view,width,height);
    bool prepareInputs=false;
    if(valid && !inputSlot.checked){
        inputSlot.checked=true;
        if(latestSceneInputs && latestSceneInputs->owner==&scene && SameSceneInputs(latestSceneInputs->sourceHits,*hits)){
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
        sceneData.serial=serial;sceneData.owner=&scene;sceneData.ready=false;
        std::vector<uint32_t> quads,ranges,instances,quadMaterials,attributes,attributeHeaders;
        std::vector<VoxPlaneRange> planes;
        std::vector<glm::vec4> emissiveLightData(1,glm::vec4(0));  // [0].x = 数量
        std::vector<mikan::rt::lights::HitKey> emitterKeys;
        std::unordered_map<const VoxRayTracingGeometry*,uint32_t> offsets;
        std::array<VkDescriptorImageInfo,8> albedoTextures{},mrTextures{},emissiveTextures{};
    
        if(valid)for(const auto& hit:*hits){
            const size_t base=instances.size();instances.resize(base+12);std::memcpy(instances.data()+base,&hit.color,16);
            instances[base+5]=hit.materialFlags; // Preserve mirror/emissive flags in both traversal paths.
            std::memcpy(instances.data()+base+8,&hit.materialParams,16);
            // Entity-level emissive strength (half in bits[31:16]) feeds the light list.
            const float instanceEmissive=(hit.materialFlags&2u)?glm::unpackHalf1x16(uint16_t(hit.materialFlags>>16)):0.0f;
            if(hit.modelGeometry){
                if(sampleEmissiveTriangles && instanceEmissive>0.0f && hit.modelGeometry->Source())
                    AppendModelEmissiveLights(emissiveLightData,*hit.modelGeometry,*hit.modelGeometry->Source(),hit.model,glm::vec3(hit.color),instanceEmissive,emitterKeys,uint32_t(base/12));
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
            }
            // std430: vec4 color + uvec4 geometry offsets. No descriptor indexing requirement.
            instances[base+4]=it->second;
        }
                AppendVoxPlaneFooter(quads,std::move(planes));
        // Binding 19 retains legacy material words first, then palette/mapping/R8 data.
        const uint32_t attributeOffset=uint32_t(quadMaterials.size());
        for(size_t i=3;i<ranges.size();i+=4)if(ranges[i])ranges[i]+=attributeOffset;
        for(uint32_t h:attributeHeaders)for(uint32_t k=0;k<3;++k)attributes[h+k]+=attributeOffset;
        quadMaterials.insert(quadMaterials.end(),attributes.begin(),attributes.end());
        if(valid)valid=PrepareModelInputs(sceneData,*hits,instances,albedoTextures,mrTextures,emissiveTextures);
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
        const char* treeOption=std::getenv("MIKAN_HWRT_NEE_LIGHT_SELECTION");
        const bool primeTree=!treeOption||!*treeOption||std::strcmp(treeOption,"tree_prime")==0;
        if(valid && !mikan::rt::lights::Append(emissiveLightData,emitterCount,std::move(emitterKeys),primeTree)){
            LOGE("[HardwareRT] light tree/index build failed; rejecting incomplete scene inputs");valid=false;
        }
        std::vector<uint32_t> emissiveLightWords(emissiveLightData.size()*4);
        if(emissiveLightData.size()>0)std::memcpy(emissiveLightWords.data(),emissiveLightData.data(),emissiveLightData.size()*sizeof(glm::vec4));
        if(valid)valid=Upload(sceneData.quads,sceneData.cachedQuads,quads)&&Upload(sceneData.ranges,sceneData.cachedRanges,ranges)&&Upload(sceneData.instances,sceneData.cachedInstances,instances)&&Upload(sceneData.quadMaterials,sceneData.cachedQuadMaterials,quadMaterials)&&Upload(sceneData.emissiveLights,sceneData.cachedEmissiveLights,emissiveLightWords);
        std::vector<uint32_t> softwareWords(4u,0u);
        if(valid&&softwareQuadBvhEnabled&&!mikan::rt::softquad::Build(*hits,softwareWords)){
            LOGE("[SoftwareQuadBVH] VOX-only path: unsupported model or singular instance transform");valid=false;
        }
        if(valid)valid=Upload(sceneData.softwareBvh,sceneData.cachedSoftwareBvh,softwareWords);
        sceneData.albedoTextures=albedoTextures;sceneData.mrTextures=mrTextures;sceneData.emissiveTextures=emissiveTextures;
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
    if(!frame.environment.GetBuffer() && !frame.environment.Create(sizeof(glm::vec4),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return VK_NULL_HANDLE;
    frame.environment.Write(&environment.tintIntensity,sizeof(glm::vec4));
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.image=frame.output.GetImage();image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.oldLayout=frame.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;image.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    image.srcAccessMask=frame.initialized?VK_ACCESS_SHADER_READ_BIT:0;image.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,frame.initialized?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
    if(valid){
        VkDescriptorBufferInfo buffers[]={{sceneData.quads.GetBuffer(),0,VK_WHOLE_SIZE},{sceneData.ranges.GetBuffer(),0,VK_WHOLE_SIZE},{sceneData.instances.GetBuffer(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&tlas;
        VkDescriptorImageInfo output{};output.imageView=frame.output.GetView();output.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[5]{};
        for(uint32_t i=0;i<5;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:(i==4?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);if(i==0)writes[i].pNext=&as;else if(i==4)writes[i].pImageInfo=&output;else writes[i].pBufferInfo=&buffers[i-1];}
        vkUpdateDescriptorSets(g_Device,5,writes,0,nullptr);
        VkDescriptorBufferInfo softwareInfo{sceneData.softwareBvh.GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet softwareWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};softwareWrite.dstSet=frame.descriptor;softwareWrite.dstBinding=41;softwareWrite.descriptorCount=1;softwareWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;softwareWrite.pBufferInfo=&softwareInfo;
        vkUpdateDescriptorSets(g_Device,1,&softwareWrite,0,nullptr);
        VkDescriptorBufferInfo modelInfo{sceneData.models.GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet modelWrites[4]{};
        for(uint32_t i=0;i<4;++i){modelWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};modelWrites[i].dstSet=frame.descriptor;modelWrites[i].dstBinding=i==0?8u:(i==1?9u:(i==2?21u:38u));modelWrites[i].descriptorCount=i?8u:1u;
            modelWrites[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if(i==0)modelWrites[i].pBufferInfo=&modelInfo;else modelWrites[i].pImageInfo=(i==1?albedoTextures.data():(i==2?mrTextures.data():emissiveTextures.data()));}
        vkUpdateDescriptorSets(g_Device,4,modelWrites,0,nullptr);
        // RR/SR guide preparation also reads the tagged grid records. Bind a
        // valid empty header in triangle mode, independent of shader branches.
        const auto* ddaInput=scene.GetDdaGrid(slot,serial);if(!ddaInput)return VK_NULL_HANDLE;
        VkDescriptorBufferInfo ddaInputInfo{ddaInput->GetBuffer(),0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet ddaInputWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};ddaInputWrite.dstSet=frame.descriptor;ddaInputWrite.dstBinding=40;ddaInputWrite.descriptorCount=1;ddaInputWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;ddaInputWrite.pBufferInfo=&ddaInputInfo;
        vkUpdateDescriptorSets(g_Device,1,&ddaInputWrite,0,nullptr);
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
            vkUpdateDescriptorSets(g_Device,2,ddaWrites,0,nullptr);
        }        if(!quadMaterials.empty()){
            VkDescriptorBufferInfo quadMaterialInfo{sceneData.quadMaterials.GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet quadMaterialWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            quadMaterialWrite.dstSet=frame.descriptor;quadMaterialWrite.dstBinding=19;quadMaterialWrite.descriptorCount=1;
            quadMaterialWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;quadMaterialWrite.pBufferInfo=&quadMaterialInfo;
            vkUpdateDescriptorSets(g_Device,1,&quadMaterialWrite,0,nullptr);
        }
        {
            VkDescriptorBufferInfo emissiveInfo{sceneData.emissiveLights.GetBuffer(),0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet emissiveWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            emissiveWrite.dstSet=frame.descriptor;emissiveWrite.dstBinding=20;emissiveWrite.descriptorCount=1;
            emissiveWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;emissiveWrite.pBufferInfo=&emissiveInfo;
            vkUpdateDescriptorSets(g_Device,1,&emissiveWrite,0,nullptr);
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
    if(lightSignature!=history.reservoirLightSignature){history.reservoirValid=false;history.giReservoirValid=false;}
    history.reservoirLightSignature=lightSignature;
    // Sun direction/intensity and sky settings may animate every frame.
    // Let denoisers reject obsolete radiance instead of restarting accumulation.
    const uint32_t lightCountTotal=valid?sceneData.lightCount:0;
    static const bool rtxdiVendor=[](){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&p);return p.vendorID==0x10DE||p.vendorID==0x1002;}();
    static const bool rtxdiForcedOff=[](){const char* v=std::getenv("MIKAN_HWRT_RTXDI");return !v||v[0]!='1';}();
    frame.rtxdiEnabled=rtxdiAvailable&&rtxdiVendor&&!rtxdiForcedOff&&!mikan::rt::UseUnbiasedSpatialRestir()&&!mikan::rt::UseFreshDiffuseExperiment()&&valid&&lightCountTotal>0;
    const std::vector<RayTracingHitInstance> emptyHits;
    static const bool traceHistory=[](){const char* v=std::getenv("MIKAN_HWRT_TRACE_HISTORY");return v&&v[0]=='1';}();
    const auto previousSerial=history.serial;
    const bool lightingInvalid=!history.reservoirValid;
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
    VkDescriptorBufferInfo reservoirBuffers[]={
        {history.reservoirs[history.reservoirRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.reservoirs[1-history.reservoirRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.giReservoirs[history.reservoirRead].GetBuffer(),0,VK_WHOLE_SIZE},
        {history.giReservoirs[1-history.reservoirRead].GetBuffer(),0,VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo reservoirTemporalInfo={history.reservoirTemporal.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet reservoirWrites[5]{};
    for(uint32_t i=0;i<4;++i){
        reservoirWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};reservoirWrites[i].dstSet=frame.descriptor;
        reservoirWrites[i].dstBinding=i<2?24+i:30+i-2;reservoirWrites[i].descriptorCount=1;
        reservoirWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;reservoirWrites[i].pBufferInfo=&reservoirBuffers[i];
    }
    reservoirWrites[4]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};reservoirWrites[4].dstSet=frame.descriptor;
    reservoirWrites[4].dstBinding=37;reservoirWrites[4].descriptorCount=1;
    reservoirWrites[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;reservoirWrites[4].pBufferInfo=&reservoirTemporalInfo;
    vkUpdateDescriptorSets(g_Device,5,reservoirWrites,0,nullptr);
    // Serialize previous reads and writes with this view's ping-pong history.
    VkMemoryBarrier reservoirReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reservoirReady.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    reservoirReady.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&reservoirReady,0,nullptr,0,nullptr);
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
    vkUpdateDescriptorSets(g_Device,1,&taaGuideWrite,0,nullptr);
    VkImageMemoryBarrier giBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    giBarrier.image=frame.gi.GetImage();giBarrier.oldLayout=frame.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    giBarrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;giBarrier.srcAccessMask=frame.initialized?VK_ACCESS_SHADER_READ_BIT:0;
    giBarrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;giBarrier.srcQueueFamilyIndex=giBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    giBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&giBarrier);
    VkDescriptorImageInfo giInfo{VK_NULL_HANDLE,frame.gi.GetView(),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet giWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};giWrite.dstSet=frame.descriptor;
    giWrite.dstBinding=27;giWrite.descriptorCount=1;giWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;giWrite.pImageInfo=&giInfo;
    vkUpdateDescriptorSets(g_Device,1,&giWrite,0,nullptr);
    const bool sky=environment.IsValid();
    VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,frame.output.GetView(),VK_IMAGE_LAYOUT_GENERAL},
        {sky?environment.sampler:fallbackSampler,sky?environment.panorama:fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo environmentBuffers[]={{sky?environment.irradiance:fallbackIrradiance.GetBuffer(),0,144},{frame.environment.GetBuffer(),0,sizeof(glm::vec4)}};
    VkWriteDescriptorSet writes[4]{};
    for(uint32_t i=0;i<4;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptor;writes[i].dstBinding=4+i;writes[i].descriptorCount=1;
        writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:(i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        if(i<2)writes[i].pImageInfo=&images[i];else writes[i].pBufferInfo=&environmentBuffers[i-2];}
    vkUpdateDescriptorSets(g_Device,4,writes,0,nullptr);
    VkDescriptorBufferInfo noiseInfo{stbnSamples.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet noiseWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    noiseWrite.dstSet=frame.descriptor;noiseWrite.dstBinding=17;noiseWrite.descriptorCount=1;
    noiseWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;noiseWrite.pBufferInfo=&noiseInfo;
    vkUpdateDescriptorSets(g_Device,1,&noiseWrite,0,nullptr);
    const bool physicalSun=sky && environment.transmittance;
    VkDescriptorImageInfo sunImage{physicalSun?environment.sampler:fallbackSampler,
        physicalSun?environment.transmittance:fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet sunWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};sunWrite.dstSet=frame.descriptor;
    sunWrite.dstBinding=18;sunWrite.descriptorCount=1;sunWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;sunWrite.pImageInfo=&sunImage;
    vkUpdateDescriptorSets(g_Device,1,&sunWrite,0,nullptr);
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
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,valid?(mikan::rt::GetNeeDirectSamples()==1u?performancePipeline:(mikan::rt::GetNeeDirectSamples()==4u?balancedPipeline:pipeline)):skyPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.descriptor,0,nullptr);
    static const bool tileCull=[](){const char* v=std::getenv("MIKAN_HWRT_TILE_CULL");return !v||v[0]!='0';}();
    if(valid){
        // Reset the indirect command (x=0,y=z=1); tile payload is overwritten.
        const auto primaryTiming=Core::g_VulkanGpuProfiler.BeginScope(cmd,(timingPrefix+"primary_hits").c_str(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkCmdFillBuffer(cmd,frame.activeTiles.GetBuffer(),0,4,0);
        vkCmdFillBuffer(cmd,frame.activeTiles.GetBuffer(),4,8,1);
        VkBufferMemoryBarrier tileReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        tileReset.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;tileReset.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        tileReset.srcQueueFamilyIndex=tileReset.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        tileReset.buffer=frame.activeTiles.GetBuffer();tileReset.offset=0;tileReset.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&tileReset,0,nullptr);
        VkImageMemoryBarrier positionBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        positionBarrier.image=frame.rtxdiWorldPos.GetImage();
        positionBarrier.oldLayout=frame.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;
        positionBarrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        positionBarrier.srcAccessMask=frame.initialized?VK_ACCESS_SHADER_READ_BIT:0;
        positionBarrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        positionBarrier.srcQueueFamilyIndex=positionBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        positionBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&positionBarrier);
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
        if(frame.rtxdiEnabled){
        Core::VulkanGpuScope rtxdiTiming(cmd,timingPrefix+"rtxdi_sdk");
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,rtxdiLayout,0,1,&frame.rtxdiDescriptor,0,nullptr);
        for(uint32_t pass=0;pass<4;++pass){
            Core::VulkanGpuScope passTiming(cmd,timingPrefix+(pass==0?"rtxdi_presample":(pass==1?"rtxdi_initial":(pass==2?"rtxdi_temporal":"rtxdi_spatial"))));
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,rtxdiPipelines[pass]);
            if(pass==0)vkCmdDispatch(cmd,(frame.rtxdiRisEntries+63u)/64u,1,1);
            else if(tileCull)vkCmdDispatchIndirect(cmd,frame.activeTiles.GetBuffer(),0);
            else vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&samplingReady,0,nullptr,0,nullptr);
        }
        }
        push.light.w=tileCull?0.0f:-1.0f;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mikan::rt::GetNeeDirectSamples()==1u?performancePipeline:(mikan::rt::GetNeeDirectSamples()==4u?balancedPipeline:pipeline));
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frame.descriptor,0,nullptr);
        static bool reported=false;if(!reported){
            LOGI("[HardwareRT] DI path=%s",frame.rtxdiEnabled?"RTXDI SDK: initial RIS + spatial reuse":(history.restirEnabled?"experimental fresh RIS; RTXDI SDK disabled":"ordinary NEE/MIS; RTXDI SDK disabled"));
            reported=true;
        }
    }
    {
    static const bool primarySunOnly=[] {const char* v=std::getenv("MIKAN_HWRT_PRIMARY_SUN_ONLY");return v&&(v[0]=='1'||v[0]=='2');}();
    Core::VulkanGpuScope lightingTiming(cmd,timingPrefix+(valid?(primarySunOnly?"lighting_primary_sun":"lighting_di_gi_specular"):"sky_only"));
    const bool corrected=mikan::rt::GetRestirEstimatorRestir()&&history.restirEnabled&&history.restirGIEnabled;
    const bool spatial=valid&&corrected&&history.restirEnabled&&history.restirGIEnabled&&!frame.rtxdiEnabled;
    if(spatial)push.light.w=-2.0f;
    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    if(valid&&tileCull&&!spatial)vkCmdDispatchIndirect(cmd,frame.activeTiles.GetBuffer(),0);
    else vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    if(spatial){
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        push.light.w=-3.0f;
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    }
    }
    history.reservoirRead=1-history.reservoirRead;history.reservoirValid=valid;
    history.giReservoirValid=valid&&history.restirGIEnabled;
    auto& capture=Core::PipelineCapture::GetInstance();
    if(capture.Wants(serial,viewSlot)){
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
    frame.initialized=true;return resolved;
}
