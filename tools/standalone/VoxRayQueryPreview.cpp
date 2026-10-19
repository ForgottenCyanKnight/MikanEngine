// Pure ray-traced visibility, direct diffuse lighting and shadow queries using
// the engine's ORIGINAL vox_rt_validate.comp. No graphics pipeline or draw calls.
#include "VoxSurfaceValidation.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/VulkanGpuProfiler.h"
#include "Rendering/RayTracing/AccelerationStructure.h"
#include "Rendering/RayTracing/RayTracingValidationDevice.h"
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

// The dedicated validation process owns the same globals consumed by the
// production buffer/AS implementation; it never initializes EngineGlobals.
VkAllocationCallbacks* g_Allocator=nullptr;
VkInstance g_Instance=VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice=VK_NULL_HANDLE;
VkDevice g_Device=VK_NULL_HANDLE;
uint32_t g_QueueFamily=0;
VkQueue g_Queue=VK_NULL_HANDLE;
VkCommandPool g_CommandPool=VK_NULL_HANDLE;

namespace {
using VoxRTValidation::Require;
using VoxRTValidation::Check;
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
constexpr auto hostMemory=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
void Allocate(VulkanBuffer& buffer,VkDeviceSize bytes,VkBufferUsageFlags usage,const void* data=nullptr){
    Require(buffer.Create(bytes,usage,hostMemory),"production VulkanBuffer allocation failed");
    if(data)buffer.Write(data,bytes);
}
void HostBarrier(VkCommandBuffer cmd){
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);
}
struct ComputePipeline {
    VkDescriptorSetLayout setLayout{};VkDescriptorPool pool{};VkDescriptorSet set{};
    VkPipelineLayout layout{};VkPipeline pipeline{};
    ComputePipeline(const char* shader,uint32_t bufferBindings,uint32_t pushBytes,bool acceleration=false){
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for(uint32_t i=0;i<bufferBindings;++i)bindings.push_back({i,(acceleration&&i==0)?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=uint32_t(bindings.size());setInfo.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(g_Device,&setInfo,nullptr,&setLayout),"descriptor layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,bufferBindings},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=1;poolInfo.poolSizeCount=acceleration?2:1;poolInfo.pPoolSizes=sizes;
        Check(vkCreateDescriptorPool(g_Device,&poolInfo,nullptr,&pool),"descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&setLayout;
        Check(vkAllocateDescriptorSets(g_Device,&allocate,&set),"descriptor set");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,pushBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
        Check(vkCreatePipelineLayout(g_Device,&layoutInfo,nullptr,&layout),"compute layout");
        const auto code=RendererUtils::ReadFile(Utf8String(std::filesystem::path(MIKAN_RT_SHADER_DIR)/shader));
        const auto module=RendererUtils::CreateShaderModule(code,shader);Require(module,"production shader module failed");
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};create.layout=layout;
        create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&create,nullptr,&pipeline);vkDestroyShaderModule(g_Device,module,nullptr);Check(result,"compute pipeline");
    }
    ~ComputePipeline(){if(pipeline)vkDestroyPipeline(g_Device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(g_Device,layout,nullptr);if(pool)vkDestroyDescriptorPool(g_Device,pool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,nullptr);}
    void Buffers(std::span<VulkanBuffer*> buffers,VkAccelerationStructureKHR as=VK_NULL_HANDLE){
        std::vector<VkDescriptorBufferInfo> infos;for(auto* b:buffers)infos.push_back({b->GetBuffer(),0,b->GetSize()});
        VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&as;
        std::vector<VkWriteDescriptorSet> writes;
        if(as){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.pNext=&acceleration;w.dstSet=set;w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;writes.push_back(w);}
        for(uint32_t i=0;i<buffers.size();++i){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=i+(as?1:0);w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&infos[i];writes.push_back(w);}
        vkUpdateDescriptorSets(g_Device,uint32_t(writes.size()),writes.data(),0,nullptr);
    }
    void Bind(VkCommandBuffer cmd){vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);}
};

void SaveBmp(const std::filesystem::path& path,uint32_t width,uint32_t height,std::vector<uint32_t>& pixels){
    auto* surface=SDL_CreateSurfaceFrom(int(width),int(height),SDL_PIXELFORMAT_RGBA32,pixels.data(),int(width*4));Require(surface,SDL_GetError());
    const bool saved=SDL_SaveBMP(surface,Utf8String(path).c_str());SDL_DestroySurface(surface);Require(saved,SDL_GetError());
}

#ifdef MIKAN_COMPLETE_PREVIEW
nlohmann::json RenderComplete(VoxRTValidation::Device&,const std::filesystem::path&,uint32_t,uint32_t,VulkanBuffer&,VulkanBuffer&,VulkanBuffer&,AccelerationStructure&,const glm::vec3&);
#endif
nlohmann::json RenderModel(VoxRTValidation::Device& device,const std::filesystem::path& path,const std::filesystem::path& output,uint32_t width,uint32_t height){
    const auto totalStart=Clock::now();
    VoxFormat::VoxData data;Require(VoxFormat::LoadVoxFile(Utf8String(std::filesystem::absolute(path)),data),"VOX loader failed");
    Require(data.models.size()==1&&data.instances.empty(),"single assembled model required");
    const auto bakedStart=Clock::now();const auto result=VoxValidation::Analyze(data,data.models[0]);const auto bakeMs=Milliseconds(bakedStart);
    const auto& baked=result.finalSurface;const auto count=baked.quads.size();Require(count>0,"empty VOX surface");
#ifdef MIKAN_COMPLETE_PREVIEW
    std::cout<<"[Full] "<<path.filename()<<": "<<count*2<<" triangles; "<<width<<'x'<<height<<"; production primary visibility + NEE GI\n";
#else
    std::cout<<"[RayQuery] "<<path.filename()<<": "<<count*2<<" triangles; "<<width<<'x'<<height<<"; 4 primary samples/pixel\n";
#endif
    std::vector<uint32_t> geometry;std::vector<VoxPlaneRange> planes;
    for(const auto& range:baked.ranges)for(uint32_t q=range.firstQuad;q<range.firstQuad+range.quadCount;++q){
        const auto quad=baked.quads.at(q);const int d=int(range.direction),axis=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;
        const uint32_t xyz[]{quad.geometry&255u,(quad.geometry>>8)&255u,(quad.geometry>>16)&255u};
        Require(q==geometry.size(),"noncontiguous quad ranges");geometry.push_back(xyz[u]|(xyz[v]<<8)|((quad.geometry>>24)<<16)|((quad.appearance&255u)<<24));AppendVoxPlane(planes,q,xyz[axis],uint32_t(d));
    }
    AppendVoxPlaneFooter(geometry,planes);
    std::vector<uint32_t> materials=baked.materials;const auto header=uint32_t(materials.size());
    materials.insert(materials.end(),baked.attributes.begin(),baked.attributes.end());for(int i=0;i<3;++i)materials[header+i]+=header;
    std::vector<glm::uvec4> ranges;for(auto r:baked.ranges)ranges.emplace_back(r.firstQuad,r.quadCount,r.direction,header+1);
    VulkanBuffer quads,positions,indices,rangeBuffer,attributeBuffer,instanceBuffer,pixels;
    Allocate(quads,geometry.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,geometry.data());
    const auto buildInput=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    Allocate(positions,count*4*12,buildInput);Allocate(indices,count*6*4,buildInput);
    Allocate(rangeBuffer,ranges.size()*sizeof(glm::uvec4),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,ranges.data());
    Allocate(attributeBuffer,materials.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,materials.data());
    Allocate(instanceBuffer,sizeof(VkAccelerationStructureInstanceKHR),buildInput);
    Allocate(pixels,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ComputePipeline expand("vox_rt_expand.comp.spv",3,32);
    VulkanBuffer* expandBuffers[]{&quads,&positions,&indices};expand.Buffers(expandBuffers);
    const glm::vec3 dimensions(result.maximum[0]-result.minimum[0]+1,result.maximum[2]-result.minimum[2]+1,result.maximum[1]-result.minimum[1]+1);
    const auto buildStart=Clock::now();
    auto cmd=device.Begin();HostBarrier(cmd);expand.Bind(cmd);
    struct ExpandPush{glm::vec4 minSize;glm::uvec4 range;};
    for(const auto& range:baked.ranges){ExpandPush push{glm::vec4(-dimensions*.5f,1),glm::uvec4(range.firstQuad,range.quadCount,range.direction,0)};
        vkCmdPushConstants(cmd,expand.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(range.quadCount+63)/64,1,1);}
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    std::vector<VkAccelerationStructureGeometryKHR> descriptions;std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;
    for(const auto& range:baked.ranges){
        VkAccelerationStructureGeometryKHR description{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};description.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;description.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t=description.geometry.triangles;t.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;t.vertexData.deviceAddress=positions.GetDeviceAddress();t.vertexStride=12;t.maxVertex=uint32_t(count*4-1);
        t.indexType=VK_INDEX_TYPE_UINT32;t.indexData.deviceAddress=indices.GetDeviceAddress();descriptions.push_back(description);buildRanges.push_back({range.quadCount*2,range.firstQuad*6*4,0,0});
    }
    AccelerationStructure blas,tlas;
    Require(blas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,descriptions,buildRanges,VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR),"production BLAS build failed");
    device.SubmitAndWait(cmd);blas.ReleaseScratch();
    VkAccelerationStructureInstanceKHR instance{};instance.transform.matrix[0][0]=instance.transform.matrix[1][1]=1;instance.transform.matrix[2][2]=-1;
    instance.mask=1;
#ifdef MIKAN_COMPLETE_PREVIEW
    instance.mask=3; // Production sun/NEE shadows use mask 2; primary/GI use mask 1.
#endif
    instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;instance.accelerationStructureReference=blas.Address();instanceBuffer.Write(&instance,sizeof(instance));
    cmd=device.Begin();RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT);RayTracingBuildBarrier(cmd);
    VkAccelerationStructureGeometryKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};top.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;top.geometry.instances.data.deviceAddress=instanceBuffer.GetDeviceAddress();
    const VkAccelerationStructureBuildRangeInfoKHR topRange{1,0,0,0};
    Require(tlas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,{&top,1},{&topRange,1},VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR),"production TLAS build failed");
    RayTracingQueryBarrier(cmd);device.SubmitAndWait(cmd);tlas.ReleaseScratch();const auto buildMs=Milliseconds(buildStart);
#ifdef MIKAN_COMPLETE_PREVIEW
    return RenderComplete(device,output,width,height,quads,rangeBuffer,attributeBuffer,tlas,dimensions);
#else
    ComputePipeline query("vox_rt_validate.comp.spv",5,64,true);VulkanBuffer* queryBuffers[]{&quads,&rangeBuffer,&pixels,&attributeBuffer};query.Buffers(queryBuffers,tlas.Handle());
    nlohmann::json frames=nlohmann::json::array();
    auto dispatch=[&](const std::array<glm::vec4,4>& camera){
        pixels.Map();Require(pixels.GetMappedPtr(),"output mapping failed");std::memset(pixels.GetMappedPtr(),0xa5,size_t(pixels.GetSize()));
        auto command=device.Begin();HostBarrier(command);RayTracingQueryBarrier(command);query.Bind(command);
        vkCmdPushConstants(command,query.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(camera),camera.data());vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
        VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};read.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
        device.SubmitAndWait(command);std::vector<uint32_t> image(size_t(width)*height);std::memcpy(image.data(),pixels.GetMappedPtr(),size_t(pixels.GetSize()));
        for(auto pixel:image)Require((pixel>>24)==255,"shader failed to write all pixels");return image;
    };
    const float radius=glm::length(dimensions)*.5f,tangent=std::tan(glm::radians(42.f)*.5f),aspect=float(width)/height;
    for(int view=0;view<2;++view){
        const auto eye=glm::normalize(view==0?glm::vec3(1.15f,.7f,1.5f):glm::vec3(-1.5f,.8f,-1.2f))*radius*3.3f;
        const auto forward=glm::normalize(-eye),right=glm::normalize(glm::cross(forward,glm::vec3(0,1,0))),up=glm::cross(right,forward);
        std::array<glm::vec4,4> camera{glm::vec4(eye,float(width)),glm::vec4(forward,float(height)),glm::vec4(right,tangent*aspect),glm::vec4(up,tangent)};
        auto away=camera;away[1]=glm::vec4(-forward,float(height));
        const auto controlStart=Clock::now();const auto control=dispatch(away);const auto controlMs=Milliseconds(controlStart);
        Require(std::all_of(control.begin(),control.end(),[&](auto p){return p==control.front();}),"away-facing negative control hit geometry");
        const auto renderStart=Clock::now();auto image=dispatch(camera);const auto renderMs=Milliseconds(renderStart);
        const auto foreground=std::count_if(image.begin(),image.end(),[&](auto p){return p!=control.front();});Require(foreground>100,"ray query output is blank");
        const auto filename=Utf8String(path.stem())+"_rt_view"+std::to_string(view)+".bmp";SaveBmp(output/filename,width,height,image);
        frames.push_back({{"view",view},{"foreground_pixels",foreground},{"negative_control","pass"},{"negative_control_dispatch_ms",controlMs},
            {"ray_query_dispatch_ms",renderMs},{"primary_rays",uint64_t(width)*height*4},{"bmp",filename}});
        std::cout<<"[RayQuery] view="<<view<<" render_ms="<<renderMs<<" foreground="<<foreground<<'\n';
    }
    return {{"model",Utf8String(path.filename())},{"voxel_count",data.models[0].voxels.size()},{"triangles",count*2},
        {"blas_bytes",blas.StorageBytes()},{"tlas_bytes",tlas.StorageBytes()},{"cpu_bake_and_audit_ms",bakeMs},{"expand_and_as_build_ms",buildMs},
        {"model_total_ms",Milliseconds(totalStart)},{"frames",frames}};
#endif
}
}

#ifndef MIKAN_COMPLETE_PREVIEW
int main(int argc,char** argv){
    try{
        Core::SetLogFileEnabled(false);std::cout<<std::unitbuf;uint32_t width=128,height=96;std::filesystem::path output="out/software-ray-query";std::vector<std::filesystem::path> models;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--output"){Require(i+1<argc,"missing output");output=Utf8Path(argv[++i]);}
            else if(arg=="--size"){Require(i+2<argc,"missing width/height");width=uint32_t(std::stoul(argv[++i]));height=uint32_t(std::stoul(argv[++i]));}
            else models.push_back(Utf8Path(arg));}
        Require(!models.empty()&&width>=16&&height>=16&&width<=512&&height<=512,"usage: MikanVoxRayQueryPreview models.vox --size 128 96 --output directory (16..512)");
        std::filesystem::create_directories(output);const auto totalStart=Clock::now();VoxRTValidation::Device device;device.Initialize(true,true);
        Require(GetRayTracingDeviceCapabilities().rayQuery&&GetRayTracingDeviceCapabilities().accelerationStructure,"required RT capabilities missing");
        nlohmann::json report={{"device",device.name},{"device_type","CPU"},{"width",width},{"height",height},{"samples_per_pixel",4},
            {"buffer_device_address",GetRayTracingDeviceCapabilities().bufferDeviceAddress},
            {"acceleration_structure",GetRayTracingDeviceCapabilities().accelerationStructure},{"ray_query",GetRayTracingDeviceCapabilities().rayQuery},
            {"execution_method","compute shader inline Ray Query"},{"vkCmdTraceRaysKHR",false},
            {"primary_visibility","Vulkan Ray Query over BLAS/TLAS"},{"shadow_visibility","Vulkan Ray Query"},{"rasterization",false},{"postprocessing",false},
            {"display_gamma_in_original_shader",true},{"global_illumination",false},{"denoising",false},{"shader","engine/shaders/glsl/vox_rt_validate.comp"},
            {"geometry_shader","engine/shaders/glsl/vox_rt_expand.comp"},{"validation_enabled",true},{"models",nlohmann::json::array()}};
        for(const auto& path:models)report["models"].push_back(RenderModel(device,path,output,width,height));
        Core::g_VulkanGpuProfiler.Shutdown();device.Shutdown();report["validation_errors"]=device.validationErrors;report["validation_warnings"]=device.validationWarnings;report["total_ms"]=Milliseconds(totalStart);
        std::ofstream file(output/"ray-query-report.json");file<<report.dump(2)<<'\n';Require(file.good(),"report write failed");Require(!device.validationErrors,"Vulkan validation errors occurred");
        std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}

#endif
