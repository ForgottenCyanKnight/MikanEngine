#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <glm/packing.hpp>

namespace {
bool IsSrgb(VkFormat format){
    switch(format){
        case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:return true;
        default:return false;
    }
}
}
bool RayTracingViewport::PrepareModelInputs(SceneInputs& frame,const std::vector<RayTracingHitInstance>& hits,
    std::vector<uint32_t>& instances,std::vector<VkDescriptorImageInfo>& textures,
    std::vector<VkDescriptorImageInfo>& mrTextures,std::vector<VkDescriptorImageInfo>& emissiveTextures,std::vector<VkDescriptorImageInfo>& normalTextures){
    normalTextures.clear();normalTextures.reserve(64);
    textures.clear();mrTextures.clear();emissiveTextures.clear();textures.reserve(256);mrTextures.reserve(64);emissiveTextures.reserve(64);
    std::vector<uint32_t> geometries;
    std::map<std::tuple<const ModelRayTracingGeometry*,VkImageView,VkSampler,VkFormat>,uint32_t> offsets;
    std::vector<std::pair<uint32_t,VkDeviceAddress>> dynamicMotion;
    uint32_t textureCount=0,mrCount=0,emissiveCount=0;
    for(size_t index=0;index<hits.size();++index){
        const auto& hit=hits[index];if(!hit.modelGeometry)continue;
        const auto* geometry=hit.modelGeometry.get();uint32_t normalSlot=UINT32_MAX;
        if(hit.normalView&&hit.normalSampler){
            uint32_t slot=0;while(slot<normalTextures.size()&&(normalTextures[slot].imageView!=hit.normalView||normalTextures[slot].sampler!=hit.normalSampler))++slot;
            if(slot==normalTextures.size()){
                if(slot>=kModelTextureBindlessCapacity){LOGE("[HardwareRT] normal texture table capacity exceeded");return false;}
                normalTextures.push_back({hit.normalSampler,hit.normalView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
            normalSlot=slot;
        }
        auto [it,inserted]=offsets.emplace(std::make_tuple(geometry,hit.normalView,hit.normalSampler,hit.normalFormat),uint32_t(geometries.size()/12));
        if(inserted){
            if(geometry->IsDynamic())dynamicMotion.emplace_back(uint32_t(geometries.size()+7),geometry->PreviousVertexAddress());
            const auto vertices=geometry->VertexAddress(),indices=geometry->IndexAddress();
            geometries.insert(geometries.end(),{uint32_t(vertices),uint32_t(vertices>>32),uint32_t(indices),uint32_t(indices>>32),geometry->VertexCount(),geometry->IndexCount(),normalSlot,uint32_t(hit.normalFormat==VK_FORMAT_BC5_UNORM_BLOCK||hit.normalFormat==VK_FORMAT_BC5_SNORM_BLOCK)|(geometry->HasAlphaClassification()?2u:0u)|(geometry->HasOpacityPartition()?(4u|(geometry->OpaquePrimitiveCount()<<3u)):0u)});
            // emission.w carries explicitly authored anisotropy; no descriptor/record growth.
            const glm::vec4 emission(hit.emissiveFactor,glm::uintBitsToFloat(glm::packHalf2x16(glm::vec2(hit.materialParams.z,hit.materialParams.w))));
            const size_t offset=geometries.size();geometries.resize(offset+4);
            std::memcpy(geometries.data()+offset,&emission,sizeof(emission));
        }
        // 48-byte instance payload: color + info + metallic/roughness material params.
        const size_t base=index*12;instances[base+4]=it->second;instances[base+6]=1;instances[base+7]=UINT32_MAX;
        // metallicRoughness slot lives in material.z; 0xffffffff = none (scalar fallback).
        instances[base+10]=0xffffffffu;
        if(hit.mrView && hit.mrSampler){
            uint32_t slot=0;
            while(slot<mrCount && (mrTextures[slot].imageView!=hit.mrView || mrTextures[slot].sampler!=hit.mrSampler))++slot;
            if(slot==mrCount){
                if(mrCount<kModelTextureBindlessCapacity){mrTextures.push_back({hit.mrSampler,hit.mrView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});++mrCount;instances[base+10]=slot;}
                else{static bool reported=false;if(!reported){LOGW("[HardwareRT] bindless metallicRoughness table capacity %u reached; excess models use scalar fallback",kModelTextureBindlessCapacity);reported=true;}}
            }else instances[base+10]=slot;
        }
        instances[base+11]=UINT32_MAX;
        if(hit.emissiveView&&hit.emissiveSampler){
            uint32_t slot=0;
            while(slot<emissiveCount&&(emissiveTextures[slot].imageView!=hit.emissiveView||emissiveTextures[slot].sampler!=hit.emissiveSampler))++slot;
            if(slot==emissiveCount){
                if(emissiveCount>=kModelTextureBindlessCapacity)return false; // Do not turn texture emission into a uniform glowing surface.
                emissiveTextures.push_back({hit.emissiveSampler,hit.emissiveView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});++emissiveCount;
            }
            instances[base+11]=slot|(IsSrgb(hit.emissiveFormat)?0u:0x80000000u);
        }
        if(!hit.albedoView || !hit.albedoSampler)continue;
        uint32_t slot=0;
        while(slot<textureCount && (textures[slot].imageView!=hit.albedoView || textures[slot].sampler!=hit.albedoSampler))++slot;
        if(slot==textureCount){
            if(textureCount>=kModelTextureBindlessCapacity){
                static bool reported=false;if(!reported){LOGW("[HardwareRT] bindless albedo table capacity %u reached; excess materials use base color",kModelTextureBindlessCapacity);reported=true;}
                continue;
            }
            textures.push_back({hit.albedoSampler,hit.albedoView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});++textureCount;
        }
        // UNORM base-color images need explicit sRGB decoding. SRGB views already decode in hardware.
        instances[base+7]=slot|(IsSrgb(hit.albedoFormat)?0u:0x80000000u);
    }
    static uint32_t loggedAlbedo=UINT32_MAX;
    if(textureCount!=loggedAlbedo){LOGD("[HardwareRT] albedo textures bound: %u",textureCount);loggedAlbedo=textureCount;}
    static uint32_t loggedMR=UINT32_MAX;
    if(mrCount!=loggedMR){LOGD("[HardwareRT] metallicRoughness textures bound: %u",mrCount);loggedMR=mrCount;}
    static uint32_t loggedEmission=UINT32_MAX;
    if(emissiveCount!=loggedEmission){LOGD("[HardwareRT] emissive textures bound: %u",emissiveCount);loggedEmission=emissiveCount;}
    static uint32_t loggedNormals=UINT32_MAX;
    if(normalTextures.size()!=loggedNormals){LOGD("[HardwareRT] normal textures bound: %u",uint32_t(normalTextures.size()));loggedNormals=uint32_t(normalTextures.size());}
    // Models remains a 48-byte-record array in GLSL. Each dynamic extension
    // occupies a padded record so raw word addressing stays inside the buffer.
    for(const auto& [flagsWord,address]:dynamicMotion){
        const uint32_t record=uint32_t(geometries.size()/12);
        geometries[flagsWord]|=0x80000000u|(record<<3u);
        geometries.insert(geometries.end(),{uint32_t(address),uint32_t(address>>32),0,0,0,0,0,0,0,0,0,0});
    }
    return Upload(frame.models,frame.cachedModels,geometries);
}
