#include "Rendering/RayTracing/RayTracingValidation.h"
#include "RayTracingValidationDevice.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/ProjectManager.h"
#include "Core/EngineConfig.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
namespace {
using namespace VoxRTValidation;
struct QueryPipeline {
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    ~QueryPipeline(){if(g_Device){if(pipeline)vkDestroyPipeline(g_Device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(g_Device,layout,nullptr);if(pool)vkDestroyDescriptorPool(g_Device,pool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,nullptr);}}
    VkDescriptorSet Initialize(VkAccelerationStructureKHR tlas,VkBuffer quads,VkBuffer ranges,VkBuffer output){
        VkDescriptorSetLayoutBinding bindings[4]{};
        for(uint32_t i=0;i<4;++i)bindings[i]={i,i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=4;set.pBindings=bindings;
        Check(vkCreateDescriptorSetLayout(g_Device,&set,nullptr,&setLayout),"Query descriptor layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=1;poolInfo.poolSizeCount=2;poolInfo.pPoolSizes=sizes;
        Check(vkCreateDescriptorPool(g_Device,&poolInfo,nullptr,&pool),"Query pool");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;
        VkDescriptorSet descriptor=VK_NULL_HANDLE;Check(vkAllocateDescriptorSets(g_Device,&alloc,&descriptor),"Query descriptor");
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};as.accelerationStructureCount=1;as.pAccelerationStructures=&tlas;
        VkDescriptorBufferInfo buffers[]={{quads,0,VK_WHOLE_SIZE},{ranges,0,VK_WHOLE_SIZE},{output,0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4]{};
        for(uint32_t i=0;i<4;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;if(i)writes[i].pBufferInfo=&buffers[i-1];else writes[i].pNext=&as;}
        vkUpdateDescriptorSets(g_Device,4,writes,0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,64};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
        Check(vkCreatePipelineLayout(g_Device,&layoutInfo,nullptr,&layout),"Query pipeline layout");
        auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rt_validate.comp.spv"));
        VkShaderModule shader=RendererUtils::CreateShaderModule(code,"vox_rt_validate.comp");Require(shader!=VK_NULL_HANDLE,"Query shader unavailable");
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
        const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline);vkDestroyShaderModule(g_Device,shader,nullptr);Check(result,"Query compute pipeline");return descriptor;
    }
};
struct LoadedVox {VoxRenderer renderer;~LoadedVox(){renderer.Cleanup();}};
}
int RunVoxRayTracingValidation(int argc,char** argv){
    try {
        Require(argc>=3,"Usage: MikanVoxRTValidate.exe input.vox output.png [width height]");
        const std::string input=std::filesystem::absolute(argv[1]).string(),output=std::filesystem::absolute(argv[2]).string();
        const uint32_t width=argc>3?uint32_t(std::stoul(argv[3])):1024,height=argc>4?uint32_t(std::stoul(argv[4])):768;
        Require(width>=16 && height>=16 && width<=4096 && height<=4096,"Image dimensions must be 16..4096");
        ProjectManager::GetInstance().Initialize(argc,argv);
        Device device;device.Initialize();
        // Load data without a renderer file path: validation must not rewrite project BVH/3D caches.
        VoxFormat::VoxData voxData;Require(VoxFormat::LoadVoxFile(input,voxData),"Load vox failed");
        LoadedVox loaded;Require(loaded.renderer.LoadFromVoxData(voxData,1),"Generate vox quads failed");
        VoxRayTracingConverter converter;VoxRayTracingGeometry geometry;AccelerationStructure tlas;
        VulkanBuffer instances,ranges,pixels;
        const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        Require(instances.Create(sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,host),"Allocate TLAS input");
        auto cmd=device.Begin();Require(geometry.RecordBuild(cmd,loaded.renderer,converter),"Vox quad -> triangle BLAS failed");
        // Match the active vox raster path's local Z flip. No camera culling enters the BLAS.
        VkAccelerationStructureInstanceKHR instance{};instance.transform.matrix[0][0]=1;instance.transform.matrix[1][1]=1;instance.transform.matrix[2][2]=-1;
        instance.mask=1;instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=geometry.Address();instances.Write(&instance,sizeof(instance));
        RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT);RayTracingBuildBarrier(cmd);
        VkAccelerationStructureGeometryKHR description{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};description.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
        description.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;description.geometry.instances.data.deviceAddress=instances.GetDeviceAddress();
        VkAccelerationStructureBuildRangeInfoKHR buildRange{1,0,0,0};
        Require(tlas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,{&description,1},{&buildRange,1},VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR),"Build TLAS failed");
        RayTracingQueryBarrier(cmd);device.SubmitAndWait(cmd);geometry.ReleaseBuildInputs();tlas.ReleaseScratch();
        std::vector<glm::uvec4> rangeData;for(const auto& range:geometry.Ranges())rangeData.emplace_back(range.firstQuad,range.quadCount,range.direction,0);
        Require(ranges.Create(rangeData.size()*sizeof(glm::uvec4),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host),"Allocate geometry mapping");ranges.Write(rangeData.data(),rangeData.size()*sizeof(glm::uvec4));
        Require(pixels.Create(VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host),"Allocate image output");
        QueryPipeline pipeline;auto descriptor=pipeline.Initialize(tlas.Handle(),geometry.QuadBuffer(),ranges.GetBuffer(),pixels.GetBuffer());
        glm::vec3 center=loaded.renderer.GetCenter();center.z=-center.z;
        const float radius=glm::length(loaded.renderer.GetMaxBounds()-loaded.renderer.GetMinBounds())*.5f;
        const float aspect=float(width)/height,tangent=std::tan(glm::radians(42.f)*.5f);
        const auto eye=center+glm::normalize(glm::vec3(1.15f,.7f,1.5f))*radius*3.3f;
        const auto forward=glm::normalize(center-eye),right=glm::normalize(glm::cross(forward,glm::vec3(0,1,0))),up=glm::cross(right,forward);
        std::array<glm::vec4,4> camera={glm::vec4(eye,float(width)),glm::vec4(forward,float(height)),glm::vec4(right,tangent*aspect),glm::vec4(up,tangent)};
        cmd=device.Begin();RayTracingQueryBarrier(cmd);
        VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};inputs.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;inputs.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&inputs,0,nullptr,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.layout,0,1,&descriptor,0,nullptr);
        vkCmdPushConstants(cmd,pipeline.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(camera),camera.data());vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);device.SubmitAndWait(cmd);
        pixels.Map();std::filesystem::create_directories(std::filesystem::path(output).parent_path());
        SDL_Surface* surface=SDL_CreateSurfaceFrom(int(width),int(height),SDL_PIXELFORMAT_RGBA32,pixels.GetMappedPtr(),int(width*4));Require(surface!=nullptr,"Create PNG surface");
        const bool saved=IMG_SavePNG(surface,output.c_str());SDL_DestroySurface(surface);Require(saved,"Save PNG failed");
        std::ofstream report(output+".txt");report<<"GPU: "<<device.name<<"\nInput: "<<input<<"\nQuads: "<<loaded.renderer.GetQuads().size()<<"\nTriangles: "<<loaded.renderer.GetQuads().size()*2<<"\nResolution: "<<width<<'x'<<height<<"\nPrimary samples: 4/pixel\nCommands: compute expansion, BLAS build, TLAS build, compute Ray Query\nRaster draw calls: 0\n";
        std::cout<<"Saved: "<<output<<"; quads="<<loaded.renderer.GetQuads().size()<<"; raster draw calls=0\n";return 0;
    }catch(const std::exception& error){std::cerr<<"Hardware RT validation failed: "<<error.what()<<'\n';return 1;}
}
