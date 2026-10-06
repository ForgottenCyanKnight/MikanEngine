#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstring>

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
    std::vector<uint32_t>& instances,std::array<VkDescriptorImageInfo,8>& textures,
    std::array<VkDescriptorImageInfo,8>& mrTextures,std::array<VkDescriptorImageInfo,8>& emissiveTextures){
    textures.fill({fallbackSampler,fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    mrTextures.fill({fallbackSampler,fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    emissiveTextures.fill({fallbackSampler,fallbackSky.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    std::vector<uint32_t> geometries;
    std::unordered_map<const ModelRayTracingGeometry*,uint32_t> offsets;
    uint32_t textureCount=0,mrCount=0,emissiveCount=0;
    for(size_t index=0;index<hits.size();++index){
        const auto& hit=hits[index];if(!hit.modelGeometry)continue;
        const auto* geometry=hit.modelGeometry.get();auto [it,inserted]=offsets.emplace(geometry,uint32_t(geometries.size()/12));
        if(inserted){
            const auto vertices=geometry->VertexAddress(),indices=geometry->IndexAddress();
            geometries.insert(geometries.end(),{uint32_t(vertices),uint32_t(vertices>>32),uint32_t(indices),uint32_t(indices>>32),geometry->VertexCount(),geometry->IndexCount(),0,0});
            const glm::vec4 emission(hit.emissiveFactor,0);
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
            if(slot==mrTextures.size()){
                static bool reported=false;if(!reported){LOGW("[HardwareRT] metallicRoughness table supports 8 unique textures; excess models use scalar fallback");reported=true;}
            }else{
                if(slot==mrCount)mrTextures[mrCount++]={hit.mrSampler,hit.mrView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                instances[base+10]=slot;
            }
        }
        instances[base+11]=UINT32_MAX;
        if(hit.emissiveView&&hit.emissiveSampler){
            uint32_t slot=0;
            while(slot<emissiveCount&&(emissiveTextures[slot].imageView!=hit.emissiveView||emissiveTextures[slot].sampler!=hit.emissiveSampler))++slot;
            if(slot<emissiveTextures.size()){
                if(slot==emissiveCount)emissiveTextures[emissiveCount++]={hit.emissiveSampler,hit.emissiveView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                instances[base+11]=slot|(IsSrgb(hit.emissiveFormat)?0u:256u);
            }else return false; // Do not turn texture emission into a uniform glowing surface.
        }
        if(!hit.albedoView || !hit.albedoSampler)continue;
        uint32_t slot=0;
        while(slot<textureCount && (textures[slot].imageView!=hit.albedoView || textures[slot].sampler!=hit.albedoSampler))++slot;
        if(slot==textures.size()){
            static bool reported=false;if(!reported){LOGW("[HardwareRT] initial albedo table supports 8 unique textures; excess materials use base color");reported=true;}
            continue;
        }
        if(slot==textureCount){textures[textureCount++]={hit.albedoSampler,hit.albedoView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
        // UNORM base-color images need explicit sRGB decoding. SRGB views already decode in hardware.
        instances[base+7]=slot|(IsSrgb(hit.albedoFormat)?0u:256u);
    }
    static uint32_t loggedMR=UINT32_MAX;
    if(mrCount!=loggedMR){LOGI("[HardwareRT] metallicRoughness textures bound: %u",mrCount);loggedMR=mrCount;}
    static uint32_t loggedEmission=UINT32_MAX;
    if(emissiveCount!=loggedEmission){LOGI("[HardwareRT] emissive textures bound: %u",emissiveCount);loggedEmission=emissiveCount;}
    return Upload(frame.models,frame.cachedModels,geometries);
}
