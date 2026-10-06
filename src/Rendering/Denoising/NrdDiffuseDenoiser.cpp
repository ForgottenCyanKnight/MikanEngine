#include "Rendering/Denoising/DiffuseDenoiser.h"
#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include <NRD.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace mikan::denoising {
namespace {
constexpr uint32_t MaxDispatches = 128;
constexpr nrd::Identifier DiffuseId = 0;
void Check(VkResult r) { if(r != VK_SUCCESS) throw std::runtime_error("Vulkan resource creation failed"); }
VkFormat Format(nrd::Format f) {
    static constexpr VkFormat table[] = {
        VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16_SINT,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT,VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32_SINT,VK_FORMAT_R32G32B32_SFLOAT,
        VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,VK_FORMAT_A2B10G10R10_UINT_PACK32,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32
    };
    static_assert(std::size(table)==static_cast<size_t>(nrd::Format::MAX_NUM));
    return table[static_cast<size_t>(f)];
}
struct Image { Texture texture; VkDeviceMemory memory{}; };
struct Pipeline { VkDescriptorSetLayout resources{}; VkPipelineLayout layout{}; VkPipeline pipeline{}; };
struct Slot { VkDescriptorPool pool{}; VkBuffer constants{}; VkDeviceMemory memory{}; void* mapped{}; };
struct alignas(16) PrepareConstants { glm::mat4 invProjection; glm::vec4 extent; glm::vec4 jitter; glm::vec4 signal; glm::vec4 grid; };

class NrdDiffuse final : public IDiffuseDenoiser {
    bool m_Specular=false;
    Device m_Device{};
    VkExtent2D m_Extent{};
    nrd::Instance* m_Instance{};
    std::vector<Pipeline> m_Pipelines;
    std::vector<Image> m_Permanent, m_Transient;
    Image m_Signal, m_ViewZ, m_Normal, m_Motion, m_PackedOutput, m_Output, m_Dummy;
    Pipeline m_Prepare, m_Unpack;
    VkDescriptorSetLayout m_Common{};
    std::array<VkSampler,2> m_Samplers{};
    std::vector<Slot> m_Slots;
    VkDeviceSize m_Stride{};
    glm::mat4 m_PrevView{1},m_PrevProjection{1};
    glm::vec2 m_PrevJitter{0};
    uint32_t m_FrameIndex{};
    bool m_Reset=true, m_Initialized=false, m_Warned=false;
    uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(m_Device.physical,&props);
        for(uint32_t i=0;i<props.memoryTypeCount;++i)
            if((bits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("Required memory type unavailable");
    }
    void CreateImage(Image& img,VkFormat format,uint32_t factor=1) {
        VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(m_Device.physical,format,&p);
        constexpr auto needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if((p.optimalTilingFeatures&needed)!=needed) throw std::runtime_error("NRD texture format unsupported");
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format;
        ci.extent={(m_Extent.width+factor-1)/factor,(m_Extent.height+factor-1)/factor,1};
        ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(vkCreateImage(m_Device.logical,&ci,m_Device.allocator,&img.texture.image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_Device.logical,img.texture.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size; ai.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(m_Device.logical,&ai,m_Device.allocator,&img.memory));
        Check(vkBindImageMemory(m_Device.logical,img.texture.image,img.memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=img.texture.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        Check(vkCreateImageView(m_Device.logical,&vi,m_Device.allocator,&img.texture.view));
        img.texture.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    }
    void DestroyImage(Image& img) {
        if(img.texture.view) vkDestroyImageView(m_Device.logical,img.texture.view,m_Device.allocator);
        if(img.texture.image) vkDestroyImage(m_Device.logical,img.texture.image,m_Device.allocator);
        if(img.memory) vkFreeMemory(m_Device.logical,img.memory,m_Device.allocator);
        img={};
    }
    void Transition(VkCommandBuffer cmd,Image& img,VkImageLayout target) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=img.texture.layout; b.newLayout=target;
        b.srcAccessMask=b.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask=target==VK_IMAGE_LAYOUT_GENERAL?VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=img.texture.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        img.texture.layout=target;
    }
    void ComputeBarrier(VkCommandBuffer cmd) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);
    }
    void CreatePipeline(Pipeline& p,const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                        const uint32_t* code,size_t size,const char* entry,bool common) {
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount=static_cast<uint32_t>(bindings.size()); si.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(m_Device.logical,&si,m_Device.allocator,&p.resources));
        VkDescriptorSetLayout layouts[]={p.resources,m_Common};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount=common?2u:1u; li.pSetLayouts=layouts;
        Check(vkCreatePipelineLayout(m_Device.logical,&li,m_Device.allocator,&p.layout));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=size; sm.pCode=code;
        VkShaderModule module{}; Check(vkCreateShaderModule(m_Device.logical,&sm,m_Device.allocator,&module));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module=module; ci.stage.pName=entry; ci.layout=p.layout;
        auto result=vkCreateComputePipelines(m_Device.logical,VK_NULL_HANDLE,1,&ci,m_Device.allocator,&p.pipeline);
        vkDestroyShaderModule(m_Device.logical,module,m_Device.allocator); Check(result);
    }
    void AdapterPipeline(Pipeline& p,const char* name,const std::vector<VkDescriptorType>& types) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for(uint32_t i=0;i<types.size();++i) bindings.push_back({i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        std::ifstream f(EngineConfig::GetShaderPath(name),std::ios::binary|std::ios::ate);
        if(!f) throw std::runtime_error("Denoiser adapter shader not found; build CompileShaders");
        const auto bytes=static_cast<size_t>(f.tellg());
        if(!bytes||bytes%4) throw std::runtime_error("Invalid SPIRV adapter shader");
        std::vector<uint32_t> code(bytes/4); f.seekg(0); f.read(reinterpret_cast<char*>(code.data()),bytes);
        if(!f) throw std::runtime_error("Cannot read adapter shader");
        CreatePipeline(p,bindings,code.data(),bytes,"main",false);
    }
    void DestroyPipeline(Pipeline& p) {
        if(p.pipeline) vkDestroyPipeline(m_Device.logical,p.pipeline,m_Device.allocator);
        if(p.layout) vkDestroyPipelineLayout(m_Device.logical,p.layout,m_Device.allocator);
        if(p.resources) vkDestroyDescriptorSetLayout(m_Device.logical,p.resources,m_Device.allocator);
        p={};
    }
    VkDescriptorSet Allocate(Slot& slot,VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=slot.pool; ai.descriptorSetCount=1; ai.pSetLayouts=&layout;
        VkDescriptorSet set{}; Check(vkAllocateDescriptorSets(m_Device.logical,&ai,&set)); return set;
    }
    void ImageDescriptor(VkDescriptorSet set,uint32_t binding,VkDescriptorType type,const Texture& tex) {
        VkDescriptorImageInfo info{type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?m_Samplers[0]:VK_NULL_HANDLE,tex.view,tex.layout};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set; write.dstBinding=binding; write.descriptorCount=1; write.descriptorType=type; write.pImageInfo=&info;
        vkUpdateDescriptorSets(m_Device.logical,1,&write,0,nullptr);
    }
    void Constants(VkDescriptorSet set,uint32_t binding,Slot& slot,uint32_t index,const void* data,size_t bytes) {
        if(bytes>m_Stride) throw std::runtime_error("NRD constants exceed allocation");
        std::memcpy(static_cast<char*>(slot.mapped)+index*m_Stride,data,bytes);
        VkDescriptorBufferInfo info{slot.constants,index*m_Stride,m_Stride};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set; write.dstBinding=binding; write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo=&info;
        vkUpdateDescriptorSets(m_Device.logical,1,&write,0,nullptr);
    }
    Image& Resource(const nrd::ResourceDesc& r) {
        switch(r.type) {
            case nrd::ResourceType::PERMANENT_POOL:return m_Permanent.at(r.indexInPool);
            case nrd::ResourceType::TRANSIENT_POOL:return m_Transient.at(r.indexInPool);
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:return m_Signal;
            case nrd::ResourceType::IN_VIEWZ:return m_ViewZ;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:return m_Normal;
            case nrd::ResourceType::IN_MV:return m_Motion;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:return m_PackedOutput;
            default:return m_Dummy; // Disabled optional guides must still have valid descriptors.
        }
    }
public:
    explicit NrdDiffuse(Signal signal):m_Specular(signal==Signal::Specular){}
    ~NrdDiffuse() override {
        if(!m_Device.logical) return;
        for(auto& p:m_Pipelines) DestroyPipeline(p);
        DestroyPipeline(m_Prepare); DestroyPipeline(m_Unpack);
        if(m_Common) vkDestroyDescriptorSetLayout(m_Device.logical,m_Common,m_Device.allocator);
        for(auto& s:m_Slots) {
            if(s.mapped) vkUnmapMemory(m_Device.logical,s.memory);
            if(s.pool) vkDestroyDescriptorPool(m_Device.logical,s.pool,m_Device.allocator);
            if(s.constants) vkDestroyBuffer(m_Device.logical,s.constants,m_Device.allocator);
            if(s.memory) vkFreeMemory(m_Device.logical,s.memory,m_Device.allocator);
        }
        for(auto& i:m_Permanent) DestroyImage(i);
        for(auto& i:m_Transient) DestroyImage(i);
        for(auto* i:{&m_Signal,&m_ViewZ,&m_Normal,&m_Motion,&m_PackedOutput,&m_Output,&m_Dummy}) DestroyImage(*i);
        for(auto s:m_Samplers) if(s) vkDestroySampler(m_Device.logical,s,m_Device.allocator);
        if(m_Instance) nrd::DestroyInstance(*m_Instance);
    }
    bool Initialize(const Device& device,VkExtent2D extent) override {
        m_Device=device; m_Extent=extent;
        try {
            if(!device.logical||!device.physical||!extent.width||!extent.height||extent.width>65535||extent.height>65535) return false;
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(device.physical,&features);
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device.physical,&properties);
            if(properties.apiVersion<VK_API_VERSION_1_2||!features.shaderStorageImageExtendedFormats||!features.shaderStorageImageWriteWithoutFormat)
                throw std::runtime_error("NRD requires Vulkan 1.2 and extended/unformatted storage image writes");
            uint32_t num=0; vkGetPhysicalDeviceQueueFamilyProperties(device.physical,&num,nullptr);
            std::vector<VkQueueFamilyProperties> queues(num); vkGetPhysicalDeviceQueueFamilyProperties(device.physical,&num,queues.data());
            if(device.queueFamily>=num||!(queues[device.queueFamily].queueFlags&VK_QUEUE_COMPUTE_BIT)) return false;
            const auto* lib=nrd::GetLibraryDesc();
            if(lib->normalEncoding!=nrd::NormalEncoding::RGBA16_SNORM||lib->roughnessEncoding!=nrd::RoughnessEncoding::LINEAR)
                throw std::runtime_error("NRD encoding mismatch; rebuild with engine CMake configuration");
            nrd::DenoiserDesc denoiser{DiffuseId,m_Specular?nrd::Denoiser::REBLUR_SPECULAR:nrd::Denoiser::REBLUR_DIFFUSE};
            nrd::InstanceCreationDesc create{}; create.denoisers=&denoiser; create.denoisersNum=1;
            if(nrd::CreateInstance(create,m_Instance)!=nrd::Result::SUCCESS) throw std::runtime_error("NRD instance creation failed");
            const auto* desc=nrd::GetInstanceDesc(*m_Instance);
            if(desc->samplersNum!=2) throw std::runtime_error("Unsupported NRD sampler count");
            if(desc->resourcesSpaceIndex!=0||desc->constantBufferAndSamplersSpaceIndex!=1) throw std::runtime_error("Unsupported NRD descriptor spaces");
            for(uint32_t i=0;i<2;++i) {
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                si.magFilter=si.minFilter=i?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
                si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                Check(vkCreateSampler(device.logical,&si,device.allocator,&m_Samplers[i]));
            }
            std::vector<VkDescriptorSetLayoutBinding> cb;
            for(uint32_t i=0;i<desc->samplersNum;++i) cb.push_back({lib->spirvBindingOffsets.samplerOffset+desc->samplersBaseRegisterIndex+i,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&m_Samplers[i]});
            cb.push_back({lib->spirvBindingOffsets.constantBufferOffset+desc->constantBufferRegisterIndex,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ci.bindingCount=static_cast<uint32_t>(cb.size()); ci.pBindings=cb.data();
            Check(vkCreateDescriptorSetLayout(device.logical,&ci,device.allocator,&m_Common));
            m_Pipelines.resize(desc->pipelinesNum);
            for(uint32_t i=0;i<desc->pipelinesNum;++i) {
                const auto& pd=desc->pipelines[i]; std::vector<VkDescriptorSetLayoutBinding> bindings;
                for(uint32_t j=0;j<pd.resourceRangesNum;++j) {
                    const auto& range=pd.resourceRanges[j]; const bool storage=range.descriptorType==nrd::DescriptorType::STORAGE_TEXTURE;
                    const uint32_t base=(storage?lib->spirvBindingOffsets.storageTextureAndBufferOffset:lib->spirvBindingOffsets.textureOffset)+desc->resourcesBaseRegisterIndex;
                    for(uint32_t k=0;k<range.descriptorsNum;++k) bindings.push_back({base+k,storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
                }
                CreatePipeline(m_Pipelines[i],bindings,static_cast<const uint32_t*>(pd.computeShaderSPIRV.bytecode),pd.computeShaderSPIRV.size,desc->shaderEntryPoint,true);
            }
            m_Permanent.resize(desc->permanentPoolSize); m_Transient.resize(desc->transientPoolSize);
            for(uint32_t i=0;i<desc->permanentPoolSize;++i) CreateImage(m_Permanent[i],Format(desc->permanentPool[i].format),desc->permanentPool[i].downsampleFactor);
            for(uint32_t i=0;i<desc->transientPoolSize;++i) CreateImage(m_Transient[i],Format(desc->transientPool[i].format),desc->transientPool[i].downsampleFactor);
            for(auto* i:{&m_Signal,&m_Normal,&m_Motion,&m_PackedOutput,&m_Output,&m_Dummy}) CreateImage(*i,VK_FORMAT_R16G16B16A16_SFLOAT);
            CreateImage(m_ViewZ,VK_FORMAT_R32_SFLOAT);
            AdapterPipeline(m_Prepare,"denoiser_prepare.comp.spv",{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER});
            AdapterPipeline(m_Unpack,"denoiser_unpack.comp.spv",{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(device.physical,&props);
            const auto alignment=std::max<VkDeviceSize>(16,props.limits.minUniformBufferOffsetAlignment);
            const auto size=std::max<size_t>(sizeof(PrepareConstants),desc->constantBufferMaxDataSize);
            m_Stride=(size+alignment-1)/alignment*alignment;
            m_Slots.resize(std::max(1u,device.framesInFlight));
            for(auto& s:m_Slots) {
                VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4096},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2048},{VK_DESCRIPTOR_TYPE_SAMPLER,256},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,128},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8}};
                VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=MaxDispatches*2+2; pi.poolSizeCount=5; pi.pPoolSizes=ps;
                Check(vkCreateDescriptorPool(device.logical,&pi,device.allocator,&s.pool));
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=m_Stride*(MaxDispatches+1); bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                Check(vkCreateBuffer(device.logical,&bi,device.allocator,&s.constants));
                VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device.logical,s.constants,&req);
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                Check(vkAllocateMemory(device.logical,&ai,device.allocator,&s.memory));
                Check(vkBindBufferMemory(device.logical,s.constants,s.memory,0));
                Check(vkMapMemory(device.logical,s.memory,0,VK_WHOLE_SIZE,0,&s.mapped));
            }
            const char* antiFirefly=std::getenv("MIKAN_HWRT_ANTIFIREFLY");
            nrd::ReblurSettings settings{}; settings.enableAntiFirefly=m_Specular||!antiFirefly||antiFirefly[0]!='0';
            if(nrd::SetDenoiserSettings(*m_Instance,DiffuseId,&settings)!=nrd::Result::SUCCESS) throw std::runtime_error("NRD settings failed");
            m_Initialized=true;
            LOGI("[Denoiser] NRD REBLUR_%s ready (%ux%u), antiFirefly=%u",m_Specular?"SPECULAR":"DIFFUSE",extent.width,extent.height,uint32_t(settings.enableAntiFirefly));
            return true;
        } catch(const std::exception& e) { LOGW("[Denoiser] NRD initialization failed: %s",e.what()); return false; }
    }
    bool Record(VkCommandBuffer cmd,const Frame& frame,const DiffuseInputs& in) override {
        if(!m_Initialized||frame.frameSlot>=m_Slots.size()||!in.radianceHitDistance.view||!in.depth.view||!in.worldNormal.view||!in.motion.view) { m_Reset=true; return false; }
        if(frame.resetHistory) m_Reset=true;
        try {
            nrd::CommonSettings common{};
            std::memcpy(common.viewToClipMatrix,glm::value_ptr(frame.projection),64);
            std::memcpy(common.worldToViewMatrix,glm::value_ptr(frame.view),64);
            std::memcpy(common.viewToClipMatrixPrev,glm::value_ptr(m_Reset?frame.projection:m_PrevProjection),64);
            std::memcpy(common.worldToViewMatrixPrev,glm::value_ptr(m_Reset?frame.view:m_PrevView),64);
            const glm::vec2 jitterPixels=frame.jitterNdc*glm::vec2(frame.sourceExtent.width,frame.sourceExtent.height)*0.5f;
            common.cameraJitter[0]=jitterPixels.x; common.cameraJitter[1]=jitterPixels.y;
            common.cameraJitterPrev[0]=m_Reset?jitterPixels.x:m_PrevJitter.x; common.cameraJitterPrev[1]=m_Reset?jitterPixels.y:m_PrevJitter.y;
            // Jitter is expressed in signal pixels, not source pixels (half-resolution SSGI).
            common.cameraJitter[0]*=float(m_Extent.width)/std::max(1u,frame.sourceExtent.width);
            common.cameraJitter[1]*=float(m_Extent.height)/std::max(1u,frame.sourceExtent.height);
            if(frame.guidePixelStride){
                const float stride=float(frame.guidePixelStride);
                // even/even full-res sample centers are half a full-res pixel
                // left/up of a centered 2x2 grid, in addition to TAA jitter.
                common.cameraJitter[0]=(jitterPixels.x-(stride-1.0f)*0.5f)/stride;
                common.cameraJitter[1]=(jitterPixels.y-(stride-1.0f)*0.5f)/stride;
            }
            common.cameraJitterPrev[0]=m_Reset?common.cameraJitter[0]:m_PrevJitter.x;
            common.cameraJitterPrev[1]=m_Reset?common.cameraJitter[1]:m_PrevJitter.y;
            for(uint32_t i=0;i<2;++i) {
                const auto dimension=static_cast<uint16_t>(i?m_Extent.height:m_Extent.width);
                common.resourceSize[i]=common.resourceSizePrev[i]=common.rectSize[i]=common.rectSizePrev[i]=dimension;
            }
            common.frameIndex=m_FrameIndex;
            common.accumulationMode=(m_Reset||frame.resetHistory)?nrd::AccumulationMode::CLEAR_AND_RESTART:nrd::AccumulationMode::CONTINUE;
            if(nrd::SetCommonSettings(*m_Instance,common)!=nrd::Result::SUCCESS) throw std::runtime_error("NRD common settings failed");
            const nrd::DispatchDesc* dispatches{}; uint32_t count{};
            if(nrd::GetComputeDispatches(*m_Instance,&DiffuseId,1,dispatches,count)!=nrd::Result::SUCCESS||count>=MaxDispatches) throw std::runtime_error("NRD dispatch retrieval failed");
            auto& slot=m_Slots[frame.frameSlot]; Check(vkResetDescriptorPool(m_Device.logical,slot.pool,0));
            // Make graphics-produced guides and signal visible to compute without changing their layouts.
            VkMemoryBarrier producer{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; producer.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; producer.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&producer,0,nullptr,0,nullptr);
            for(auto& i:m_Permanent) Transition(cmd,i,VK_IMAGE_LAYOUT_GENERAL);
            for(auto& i:m_Transient) Transition(cmd,i,VK_IMAGE_LAYOUT_GENERAL);
            for(auto* i:{&m_Signal,&m_ViewZ,&m_Normal,&m_Motion,&m_PackedOutput,&m_Output}) Transition(cmd,*i,VK_IMAGE_LAYOUT_GENERAL);
            // NRD skips out-of-range sky pixels. Keep skipped output defined,
            // including when a surface moves away and exposes the sky.
            VkClearColorValue clearOutput{};
            VkImageSubresourceRange outputRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd,m_PackedOutput.texture.image,VK_IMAGE_LAYOUT_GENERAL,&clearOutput,1,&outputRange);
            ComputeBarrier(cmd);
            if(m_Dummy.texture.layout==VK_IMAGE_LAYOUT_UNDEFINED) {
                Transition(cmd,m_Dummy,VK_IMAGE_LAYOUT_GENERAL);
                VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(cmd,m_Dummy.texture.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
                ComputeBarrier(cmd);
            }
            auto prepare=Allocate(slot,m_Prepare.resources);
            const Texture textures[]={in.radianceHitDistance,in.depth,in.worldNormal,in.motion,m_Signal.texture,m_ViewZ.texture,m_Normal.texture,m_Motion.texture};
            for(uint32_t i=0;i<8;++i) ImageDescriptor(prepare,i,i<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,textures[i]);
            PrepareConstants pc{glm::inverse(frame.projection),glm::vec4(m_Extent.width,m_Extent.height,frame.sourceExtent.width,frame.sourceExtent.height),glm::vec4(frame.jitterNdc,in.linearViewDepth?1.0f:0.0f,in.unpackedWorldNormal?1.0f:0.0f),glm::vec4(m_Specular?1.0f:0.0f,3.0f,0.1f,20.0f),glm::vec4(float(frame.guidePixelStride),0,0,0)};
            Constants(prepare,8,slot,0,&pc,sizeof(pc));
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_Prepare.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_Prepare.layout,0,1,&prepare,0,nullptr);
            vkCmdDispatch(cmd,(m_Extent.width+7)/8,(m_Extent.height+7)/8,1); ComputeBarrier(cmd);
            const auto* desc=nrd::GetInstanceDesc(*m_Instance); const auto& offsets=nrd::GetLibraryDesc()->spirvBindingOffsets;
            for(uint32_t i=0;i<count;++i) {
                const auto& d=dispatches[i]; auto& pipeline=m_Pipelines.at(d.pipelineIndex);
                VkDescriptorSet sets[]={Allocate(slot,pipeline.resources),Allocate(slot,m_Common)};
                uint32_t textureIndex=0,storageIndex=0;
                for(uint32_t j=0;j<d.resourcesNum;++j) {
                    const auto& r=d.resources[j]; const bool storage=r.descriptorType==nrd::DescriptorType::STORAGE_TEXTURE;
                    const uint32_t binding=(storage?offsets.storageTextureAndBufferOffset+storageIndex++:offsets.textureOffset+textureIndex++)+desc->resourcesBaseRegisterIndex;
                    ImageDescriptor(sets[0],binding,storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,Resource(r).texture);
                }
                if(d.constantBufferDataSize) Constants(sets[1],offsets.constantBufferOffset+desc->constantBufferRegisterIndex,slot,i+1,d.constantBufferData,d.constantBufferDataSize);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);
                vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.layout,0,2,sets,0,nullptr);
                vkCmdDispatch(cmd,d.gridWidth,d.gridHeight,1); ComputeBarrier(cmd);
            }
            auto unpack=Allocate(slot,m_Unpack.resources);
            ImageDescriptor(unpack,0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,m_PackedOutput.texture);
            ImageDescriptor(unpack,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,m_Output.texture);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_Unpack.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m_Unpack.layout,0,1,&unpack,0,nullptr);
            vkCmdDispatch(cmd,(m_Extent.width+7)/8,(m_Extent.height+7)/8,1);
            Transition(cmd,m_Output,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_PrevView=frame.view; m_PrevProjection=frame.projection;
            m_PrevJitter={common.cameraJitter[0],common.cameraJitter[1]}; ++m_FrameIndex; m_Reset=false;
            return true;
        } catch(const std::exception& e) {
            if(!m_Warned) LOGW("[Denoiser] NRD dispatch failed: %s",e.what());
            m_Warned=true; m_Reset=true; return false;
        }
    }
    Texture Output() const override { return m_Output.texture; }
    void ResetHistory() override { m_Reset=true; }
};
} // namespace
std::unique_ptr<IDiffuseDenoiser> CreateNrdDiffuseDenoiser(Signal signal) { return std::make_unique<NrdDiffuse>(signal); }
} // namespace mikan::denoising
