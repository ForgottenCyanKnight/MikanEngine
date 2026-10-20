#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Rendering/VoxLoader.h"
#include "Rendering/VoxFaceMaterial.h"
#include "Rendering/RayTracing/RayTracingValidationDevice.h"
#include <json.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

VkAllocationCallbacks* g_Allocator=nullptr;
VkInstance g_Instance=VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice=VK_NULL_HANDLE;
VkDevice g_Device=VK_NULL_HANDLE;
uint32_t g_QueueFamily=0;
VkQueue g_Queue=VK_NULL_HANDLE;
VkCommandPool g_CommandPool=VK_NULL_HANDLE;

namespace {
using VoxRTValidation::Check;
using VoxRTValidation::Require;
using Clock=std::chrono::steady_clock;
constexpr auto hostMemory=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
uint32_t SpreadMortonBits(uint32_t value){value=(value|(value<<16u))&0x030000ffu;value=(value|(value<<8u))&0x0300f00fu;value=(value|(value<<4u))&0x030c30c3u;return (value|(value<<2u))&0x09249249u;}
uint32_t Morton3D(uint32_t x,uint32_t y,uint32_t z){return SpreadMortonBits(x)|(SpreadMortonBits(y)<<1u)|(SpreadMortonBits(z)<<2u);}

struct Buffer {
    VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize size{};void* mapped{};
    void Create(VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags properties){
        size=bytes;VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=bytes;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(g_Device,&info,nullptr,&buffer),"create buffer");VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(g_Device,buffer,&requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice,&memoryProperties);uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<memoryProperties.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i))&&(memoryProperties.memoryTypes[i].propertyFlags&properties)==properties){type=i;break;}
        Require(type!=UINT32_MAX,"buffer memory type missing");VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=type;
        Check(vkAllocateMemory(g_Device,&allocate,nullptr,&memory),"allocate buffer memory");Check(vkBindBufferMemory(g_Device,buffer,memory,0),"bind buffer memory");
    }
    void* Map(){if(!mapped)Check(vkMapMemory(g_Device,memory,0,size,0,&mapped),"map buffer");return mapped;}
    ~Buffer(){if(mapped)vkUnmapMemory(g_Device,memory);if(buffer)vkDestroyBuffer(g_Device,buffer,nullptr);if(memory)vkFreeMemory(g_Device,memory,nullptr);}
};
struct Volume {
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};uint32_t mipLevels{};
    Volume()=default;Volume(const Volume&)=delete;Volume& operator=(const Volume&)=delete;
    Volume(Volume&& other)noexcept:image(other.image),memory(other.memory),view(other.view),mipLevels(other.mipLevels){other.image=VK_NULL_HANDLE;other.memory=VK_NULL_HANDLE;other.view=VK_NULL_HANDLE;}
    ~Volume(){if(view)vkDestroyImageView(g_Device,view,nullptr);if(image)vkDestroyImage(g_Device,image,nullptr);if(memory)vkFreeMemory(g_Device,memory,nullptr);}
};
struct Pipeline {
    VkDescriptorSetLayout setLayout{};VkDescriptorPool pool{};VkDescriptorSet set{};VkPipelineLayout layout{};VkPipeline pipeline{};
    Pipeline()=default;Pipeline(const Pipeline&)=delete;Pipeline& operator=(const Pipeline&)=delete;
    Pipeline(Pipeline&& other)noexcept:setLayout(other.setLayout),pool(other.pool),set(other.set),layout(other.layout),pipeline(other.pipeline){other.setLayout=VK_NULL_HANDLE;other.pool=VK_NULL_HANDLE;other.set=VK_NULL_HANDLE;other.layout=VK_NULL_HANDLE;other.pipeline=VK_NULL_HANDLE;}
    ~Pipeline(){if(pipeline)vkDestroyPipeline(g_Device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(g_Device,layout,nullptr);if(pool)vkDestroyDescriptorPool(g_Device,pool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,nullptr);}
};

std::vector<uint8_t> MakeMipData(const VoxFormat::Model& model,uint32_t& tx,uint32_t& ty,uint32_t& tz,uint32_t& mipLevels){
    auto padded=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};tx=padded(model.sizeX);ty=padded(model.sizeY);tz=padded(model.sizeZ);
    std::vector<uint8_t> data(size_t(tx)*ty*tz,0);
    for(const auto& v:model.voxels)data[(size_t(v.z)*ty+v.y)*tx+v.x]=v.colorIndex;
    std::vector<uint8_t> upload=data;uint32_t sx=tx,sy=ty,sz=tz,level=0;size_t previousOffset=0;
    for(;;){if(sx==1&&sy==1&&sz==1)break;const uint32_t nx=std::max(1u,sx/2),ny=std::max(1u,sy/2),nz=std::max(1u,sz/2);const size_t nextOffset=(upload.size()+3)&~size_t(3);upload.resize(nextOffset+size_t(nx)*ny*nz,0);
        for(uint32_t z=0;z<nz;++z)for(uint32_t y=0;y<ny;++y)for(uint32_t x=0;x<nx;++x){uint8_t occupied=0;for(uint32_t cz=z*2;cz<std::min(sz,z*2+2);++cz)for(uint32_t cy=y*2;cy<std::min(sy,y*2+2);++cy)for(uint32_t cx=x*2;cx<std::min(sx,x*2+2);++cx)occupied|=uint8_t(upload[previousOffset+(size_t(cz)*sy+cy)*sx+cx]!=0);upload[nextOffset+(size_t(z)*ny+y)*nx+x]=occupied;}
        previousOffset=nextOffset;sx=nx;sy=ny;sz=nz;++level;
    }
    mipLevels=level+1;return upload;
}

Volume UploadVolume(VoxRTValidation::Device& device,const VoxFormat::Model& model,Buffer& staging,uint32_t& tx,uint32_t& ty,uint32_t& tz){
    Volume result;uint32_t mipLevels=0;auto upload=MakeMipData(model,tx,ty,tz,mipLevels);result.mipLevels=mipLevels;
    staging.Create(upload.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,hostMemory);std::memcpy(staging.Map(),upload.data(),upload.size());
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_3D;image.format=VK_FORMAT_R8_UINT;image.extent={tx,ty,tz};image.mipLevels=mipLevels;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(g_Device,&image,nullptr,&result.image),"create 3D volume");VkMemoryRequirements req{};vkGetImageMemoryRequirements(g_Device,result.image,&req);VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice,&properties);uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<properties.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}Require(type!=UINT32_MAX,"image memory type missing");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;Check(vkAllocateMemory(g_Device,&allocation,nullptr,&result.memory),"allocate 3D volume memory");Check(vkBindImageMemory(g_Device,result.image,result.memory,0),"bind 3D volume memory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=result.image;view.viewType=VK_IMAGE_VIEW_TYPE_3D;view.format=VK_FORMAT_R8_UINT;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mipLevels,0,1};Check(vkCreateImageView(g_Device,&view,nullptr,&result.view),"create 3D volume view");
    std::vector<VkBufferImageCopy> copies;uint32_t sx=tx,sy=ty,sz=tz;VkDeviceSize offset=0;for(uint32_t level=0;level<mipLevels;++level){VkBufferImageCopy copy{};copy.bufferOffset=offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};copy.imageExtent={sx,sy,sz};copies.push_back(copy);offset=(offset+size_t(sx)*sy*sz+3)&~VkDeviceSize(3);sx=std::max(1u,sx/2);sy=std::max(1u,sy/2);sz=std::max(1u,sz/2);}
    auto cmd=device.Begin();VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=staging.buffer;host.offset=0;host.size=staging.size;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&host,0,nullptr);
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};toTransfer.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;toTransfer.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;toTransfer.srcQueueFamilyIndex=toTransfer.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toTransfer.image=result.image;toTransfer.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mipLevels,0,1};toTransfer.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&toTransfer);
    vkCmdCopyBufferToImage(cmd,staging.buffer,result.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};toSample.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;toSample.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;toSample.srcQueueFamilyIndex=toSample.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toSample.image=result.image;toSample.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mipLevels,0,1};toSample.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;toSample.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&toSample);device.SubmitAndWait(cmd);return result;
}

Pipeline CreatePipeline(Buffer& gridData,Buffer& pixels,Volume& volume,uint32_t ddaVariant){
    Pipeline p;VkDescriptorSetLayoutBinding bindings[3]{};bindings[0]={39,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};bindings[1]={40,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};bindings[2]={41,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=3;setInfo.pBindings=bindings;Check(vkCreateDescriptorSetLayout(g_Device,&setInfo,nullptr,&p.setLayout),"create descriptor layout");
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=1;poolInfo.poolSizeCount=2;poolInfo.pPoolSizes=sizes;Check(vkCreateDescriptorPool(g_Device,&poolInfo,nullptr,&p.pool),"create descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=p.pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&p.setLayout;Check(vkAllocateDescriptorSets(g_Device,&alloc,&p.set),"allocate descriptor set");
    VkDescriptorImageInfo imageInfos[8];for(auto& info:imageInfos)info={VK_NULL_HANDLE,volume.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo gridInfo{gridData.buffer,0,gridData.size},pixelInfo{pixels.buffer,0,pixels.size};
    VkWriteDescriptorSet writes[3]{};writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[0].dstSet=p.set;writes[0].dstBinding=39;writes[0].descriptorCount=8;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[0].pImageInfo=imageInfos;
    writes[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[1].dstSet=p.set;writes[1].dstBinding=40;writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&gridInfo;
    writes[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[2].dstSet=p.set;writes[2].dstBinding=41;writes[2].descriptorCount=1;writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[2].pBufferInfo=&pixelInfo;vkUpdateDescriptorSets(g_Device,3,writes,0,nullptr);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,80};VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&p.setLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;Check(vkCreatePipelineLayout(g_Device,&layout,nullptr,&p.layout),"create pipeline layout");
    const char* shaderFile=ddaVariant==5?"vox_rt_dda_benchmark_screen_bounds.comp.spv":ddaVariant==4?"vox_rt_dda_benchmark_morton_rays.comp.spv":ddaVariant==3?"vox_rt_dda_benchmark_morton.comp.spv":ddaVariant==2?"vox_rt_dda_benchmark_bitset.comp.spv":ddaVariant==1?"vox_rt_dda_benchmark_fast.comp.spv":"vox_rt_dda_benchmark.comp.spv";
    std::ifstream file(std::filesystem::path(MIKAN_DDA_SHADER_DIR)/shaderFile,std::ios::binary|std::ios::ate);Require(file.good(),"open DDA SPIR-V");const auto bytes=file.tellg();std::vector<uint32_t> code(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};moduleInfo.codeSize=size_t(bytes);moduleInfo.pCode=code.data();VkShaderModule module{};Check(vkCreateShaderModule(g_Device,&moduleInfo,nullptr,&module),"create DDA shader module");
    VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};create.layout=p.layout;create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&create,nullptr,&p.pipeline);vkDestroyShaderModule(g_Device,module,nullptr);Check(result,"create DDA compute pipeline");return p;
}
}

int main(int argc,char** argv){try{
    bool allowGpu=false;uint32_t ddaVariant=0;std::filesystem::path input;for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--allow-gpu")allowGpu=true;else if(arg=="--optimized-dda"){Require(ddaVariant==0,"choose only one DDA variant");ddaVariant=1;}else if(arg=="--bitset-dda"){Require(ddaVariant==0,"choose only one DDA variant");ddaVariant=2;}else if(arg=="--morton-dda"){Require(ddaVariant==0,"choose only one DDA variant");ddaVariant=3;}else if(arg=="--morton-rays"){Require(ddaVariant==0,"choose only one DDA variant");ddaVariant=4;}else if(arg=="--screen-bounds"){Require(ddaVariant==0,"choose only one DDA variant");ddaVariant=5;}else if(input.empty())input=std::filesystem::path(arg);else Require(false,"unexpected argument");}
    Require(!input.empty(),"usage: MikanVoxDdaPreview model.vox [--optimized-dda | --bitset-dda | --morton-dda | --morton-rays | --screen-bounds] [--allow-gpu]");VoxFormat::VoxData data;Require(VoxFormat::LoadVoxFile(std::filesystem::absolute(input).string(),data),"VOX load failed");Require(data.models.size()==1&&data.instances.empty(),"expected one assembled VOX model");const auto& model=data.models[0];Require(!model.voxels.empty(),"empty VOX model");
    uint8_t minX=255,minY=255,minZ=255,maxX=0,maxY=0,maxZ=0;for(const auto& v:model.voxels){minX=std::min(minX,v.x);minY=std::min(minY,v.y);minZ=std::min(minZ,v.z);maxX=std::max(maxX,v.x);maxY=std::max(maxY,v.y);maxZ=std::max(maxZ,v.z);}glm::vec3 dimensions(float(maxX-minX+1),float(maxZ-minZ+1),float(maxY-minY+1));glm::vec3 center(float(minX+maxX+1)*.5f,float(minY+maxY+1)*.5f,float(minZ+maxZ+1)*.5f);
    constexpr uint32_t width=640,height=480;constexpr size_t pixelCount=size_t(width)*height;VoxRTValidation::Device device;device.Initialize(!allowGpu,true,true);VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);const char* deviceType=properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU?"CPU":properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?"discrete GPU":properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?"integrated GPU":"Vulkan device";Buffer staging,gridData,pixels;uint32_t tx,ty,tz;auto volume=UploadVolume(device,model,staging,tx,ty,tz);std::vector<uint32_t> gridWords(16+24+256*4,0);auto* words=gridWords.data();words[0]=1;
    glm::mat4 gridToLocal(1);gridToLocal[0]=glm::vec4(1,0,0,0);gridToLocal[1]=glm::vec4(0,0,1,0);gridToLocal[2]=glm::vec4(0,1,0,0);gridToLocal[3]=glm::vec4(-center.x,-center.z,-center.y,1);const auto modelTransform=glm::scale(glm::mat4(1),glm::vec3(1,1,-1));const auto worldToGrid=glm::inverse(modelTransform*gridToLocal);std::memcpy(words+16,&worldToGrid,sizeof(worldToGrid));words[32]=model.sizeX;words[33]=model.sizeY;words[34]=model.sizeZ;words[35]=0;words[38]=3;
    auto floatBits=[](float value){return std::bit_cast<uint32_t>(value);};const size_t paletteBase=16+24;for(uint32_t i=0;i<256;++i){const auto& color=data.palette[i];const size_t p=paletteBase+size_t(i)*4;words[p]=floatBits(float(color.r)/255.0f);words[p+1]=floatBits(float(color.g)/255.0f);words[p+2]=floatBits(float(color.b)/255.0f);words[p+3]=PackVoxFaceMaterial(data,int(i));}
    const bool optimizedDda=ddaVariant!=0,bitsetDda=ddaVariant>=2,mortonMipDda=ddaVariant==3;
    if(optimizedDda){
        words[1]=tx;words[2]=ty;words[3]=tz;
        const size_t mipMetadata=16+24+256*4;gridWords.resize(mipMetadata+8,0);
        std::vector<uint8_t> occupancy(size_t(tx)*ty*tz,0);for(const auto& voxel:model.voxels)occupancy[(size_t(voxel.z)*ty+voxel.y)*tx+voxel.x]=1;
        uint32_t mx=tx,my=ty,mz=tz;size_t mipLevel=0,bitCursor=0;
        while(mx>1||my>1||mz>1){const uint32_t nx=std::max(1u,mx/2),ny=std::max(1u,my/2),nz=std::max(1u,mz/2);std::vector<uint8_t> next(size_t(nx)*ny*nz,0);
            for(uint32_t z=0;z<nz;++z)for(uint32_t y=0;y<ny;++y)for(uint32_t x=0;x<nx;++x){uint8_t occupied=0;for(uint32_t cz=z*2;cz<std::min(mz,z*2+2);++cz)for(uint32_t cy=y*2;cy<std::min(my,y*2+2);++cy)for(uint32_t cx=x*2;cx<std::min(mx,x*2+2);++cx)occupied|=occupancy[(size_t(cz)*my+cy)*mx+cx];next[(size_t(z)*ny+y)*nx+x]=occupied;}
            if(bitsetDda){const uint32_t side=std::max(nx,std::max(ny,nz));const size_t levelBits=mortonMipDda?size_t(side)*side*side:next.size();gridWords[mipMetadata+mipLevel]=uint32_t(bitCursor);const size_t payload=mipMetadata+8;gridWords.resize(payload+(bitCursor+levelBits+31)/32,0);
                for(uint32_t z=0;z<nz;++z)for(uint32_t y=0;y<ny;++y)for(uint32_t x=0;x<nx;++x)if(next[(size_t(z)*ny+y)*nx+x]){const size_t localIndex=mortonMipDda?Morton3D(x,y,z):(size_t(z)*ny+y)*nx+x;const size_t bitIndex=bitCursor+localIndex;gridWords[payload+bitIndex/32]|=1u<<(bitIndex&31u);}bitCursor+=levelBits;}
            else{gridWords[mipMetadata+mipLevel]=uint32_t(gridWords.size()-(mipMetadata+8));for(uint8_t occupied:next)gridWords.push_back(occupied);}
            occupancy=std::move(next);mx=nx;my=ny;mz=nz;++mipLevel;
        }
    }
    gridData.Create(gridWords.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,hostMemory);std::memcpy(gridData.Map(),gridWords.data(),gridWords.size()*4);pixels.Create(pixelCount*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,hostMemory);Pipeline pipeline=CreatePipeline(gridData,pixels,volume,ddaVariant);
    const float radius=glm::length(dimensions)*.5f,tangent=std::tan(glm::radians(45.f)*.5f),aspect=float(width)/height;const auto eye=glm::normalize(glm::vec3(1.35f,.95f,1.8f))*radius*3.2f;const auto forward=glm::normalize(-eye),right=glm::normalize(glm::cross(forward,glm::vec3(0,1,0))),up=glm::cross(right,forward);
    glm::vec4 screenBounds(-1.f,-1.f,1.f,1.f);
    if(ddaVariant==5){
        glm::vec2 minimum(std::numeric_limits<float>::infinity()),maximum(-std::numeric_limits<float>::infinity());bool safeToCull=true;
        const glm::mat4 gridToWorld=glm::inverse(worldToGrid);
        for(uint32_t z=0;z<2;++z)for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<2;++x){
            const glm::vec3 gridPoint(float(x)*model.sizeX,float(y)*model.sizeY,float(z)*model.sizeZ);
            const glm::vec3 worldPoint=glm::vec3(gridToWorld*glm::vec4(gridPoint,1.f));const glm::vec3 relative=worldPoint-eye;const float depth=glm::dot(relative,forward);
            if(depth<=1e-3f){safeToCull=false;continue;}
            const glm::vec2 uv(glm::dot(relative,right)/(depth*tangent*aspect),-glm::dot(relative,up)/(depth*tangent));minimum=glm::min(minimum,uv);maximum=glm::max(maximum,uv);
        }
        if(safeToCull){const glm::vec2 margin(4.f/width,4.f/height);screenBounds=glm::vec4(minimum-margin,maximum+margin);}
    }
    struct Camera{glm::vec4 eyeWidth,forwardHeight,rightScale,upScale,screenBounds;};Camera camera{glm::vec4(eye,float(width)),glm::vec4(forward,float(height)),glm::vec4(right,tangent*aspect),glm::vec4(up,tangent),screenBounds};
    auto dispatch=[&](){auto cmd=device.Begin();vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.layout,0,1,&pipeline.set,0,nullptr);vkCmdPushConstants(cmd,pipeline.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(camera),&camera);vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);VkBufferMemoryBarrier read{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};read.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;read.srcQueueFamilyIndex=read.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;read.buffer=pixels.buffer;read.offset=0;read.size=pixels.size;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&read,0,nullptr);device.SubmitAndWait(cmd);};
    for(int i=0;i<2;++i)dispatch();std::vector<uint32_t> values(pixelCount);std::vector<double> trials;for(int i=0;i<5;++i){const auto start=Clock::now();dispatch();std::memcpy(values.data(),pixels.Map(),pixels.size);trials.push_back(Ms(start));}const auto hitPixels=std::count_if(values.begin(),values.end(),[](uint32_t p){return (p>>24)!=0;});std::sort(trials.begin(),trials.end());
    const char* variantName=ddaVariant==5?"bitset SSBO mips + conservative projected model-bounds early-out":ddaVariant==4?"bitset SSBO mips + Morton ray scheduling":ddaVariant==3?"Morton bitset mip layout + vectorized stepping":ddaVariant==2?"bitset SSBO occupancy mips + vectorized stepping":ddaVariant==1?"flat SSBO occupancy mips + vectorized stepping":"production 3D texture occupancy mips";
    nlohmann::json report={{"model",input.filename().string()},{"model_voxels",model.voxels.size()},{"grid_dimensions_raw",{model.sizeX,model.sizeY,model.sizeZ}},{"grid_dimensions_padded",{tx,ty,tz}},{"mip_levels",volume.mipLevels},{"width",width},{"height",height},{"view",0},{"primary_rays",pixelCount},{"primary_hit_pixels",hitPixels},{"color_output","VOX palette albedo in RGBA8; alpha marks primary hit"},{"device",device.name},{"device_type",deviceType},{"allow_gpu",allowGpu},{"execution_method","production traceVoxelDDA GLSL in headless Vulkan compute"},{"dda_variant",variantName},{"dda_skip_empty_mips",true},{"setup_excluded_from_timing","VOX load, occupancy/mip generation and upload, CPU screen-bounds projection, descriptor and pipeline creation"},{"warmup_dispatches",2},{"timed_trials_ms",trials},{"median_dispatch_submit_readback_ms",trials[trials.size()/2]},{"validation_errors",device.validationErrors},{"validation_warnings",device.validationWarnings}};
    std::filesystem::create_directories("out/dda-knight-primary");std::ofstream out("out/dda-knight-primary/dda-report.json");out<<report.dump(2)<<'\n';std::ofstream raw("out/dda-knight-primary/knight-albedo.rgba",std::ios::binary);raw.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(uint32_t));std::vector<uint8_t> mask(pixelCount);for(size_t i=0;i<pixelCount;++i)mask[i]=uint8_t((values[i]>>24)!=0);std::ofstream maskRaw("out/dda-knight-primary/knight-mask.raw",std::ios::binary);maskRaw.write(reinterpret_cast<const char*>(mask.data()),mask.size());std::cout<<report.dump(2)<<'\n';Require(device.validationErrors==0,"Vulkan validation errors occurred");return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
