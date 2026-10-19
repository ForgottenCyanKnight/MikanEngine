// Headless host for the production NEE/MIS renderer and Bruneton atmosphere.
#define MIKAN_COMPLETE_PREVIEW
#include "VoxRayQueryPreview.cpp"
#include "Rendering/AtmosphereLUT.h"
#include <glm/gtc/type_ptr.hpp>
#ifdef MIKAN_PREVIEW_NRD
#include "Rendering/Denoising/DiffuseDenoiser.h"
#include <NRD.h>
#endif

namespace {
uint32_t accumulationFrames=32;
bool useNrd=false;
float cameraPitchDegrees=glm::degrees(std::atan2(.7f,glm::length(glm::vec2(1.15f,1.5f))));
struct Image {
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format;
    Image(uint32_t w,uint32_t h,VkFormat f,bool three=false):format(f){
        VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};c.imageType=three?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;c.format=f;c.extent={w,h,1};c.mipLevels=c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(g_Device,&c,nullptr,&image),"image");VkMemoryRequirements r;vkGetImageMemoryRequirements(g_Device,image,&r);VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice,&m);uint32_t type=0;while(type<m.memoryTypeCount&&!(r.memoryTypeBits&(1u<<type)))++type;Require(type<m.memoryTypeCount,"image memory type");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;Check(vkAllocateMemory(g_Device,&a,nullptr,&memory),"image memory");Check(vkBindImageMemory(g_Device,image,memory,0),"bind image");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.viewType=three?VK_IMAGE_VIEW_TYPE_3D:VK_IMAGE_VIEW_TYPE_2D;v.format=f;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};Check(vkCreateImageView(g_Device,&v,nullptr,&view),"image view");
    }
    ~Image(){vkDestroyImageView(g_Device,view,nullptr);vkDestroyImage(g_Device,image,nullptr);vkFreeMemory(g_Device,memory,nullptr);}
    void Init(VkCommandBuffer c){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);}
};
void ComputeBarrier(VkCommandBuffer c){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);}
struct FullPipeline {
    VkDescriptorSetLayout setLayout{};VkDescriptorPool pool{};VkDescriptorSet set{};VkPipelineLayout layout{};VkPipeline pipeline{};
    std::map<uint32_t,VkDescriptorType> types;
    FullPipeline(const char* shader,std::map<uint32_t,VkDescriptorType> bindings,uint32_t push):types(std::move(bindings)){
        std::vector<VkDescriptorSetLayoutBinding> list;std::map<VkDescriptorType,uint32_t> counts;
        for(auto [i,t]:types){uint32_t n=i==39?8:1;list.push_back({i,t,n,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});counts[t]+=n;}
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=list.size();si.pBindings=list.data();Check(vkCreateDescriptorSetLayout(g_Device,&si,nullptr,&setLayout),"full descriptor layout");
        std::vector<VkDescriptorPoolSize> sizes;for(auto [t,n]:counts)sizes.push_back({t,n});VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=sizes.size();pi.pPoolSizes=sizes.data();Check(vkCreateDescriptorPool(g_Device,&pi,nullptr,&pool),"full descriptor pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;Check(vkAllocateDescriptorSets(g_Device,&ai,&set),"full descriptors");
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,push};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;if(push){li.pushConstantRangeCount=1;li.pPushConstantRanges=&pc;}Check(vkCreatePipelineLayout(g_Device,&li,nullptr,&layout),"full pipeline layout");
        auto code=RendererUtils::ReadFile(Utf8String(std::filesystem::path(MIKAN_FULL_SHADER_DIR)/shader));auto module=RendererUtils::CreateShaderModule(code,shader);Require(module,"full shader");VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        std::cout<<"[Full] creating "<<shader<<'\n';Check(vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline),"full compute pipeline");vkDestroyShaderModule(g_Device,module,nullptr);
    }
    ~FullPipeline(){vkDestroyPipeline(g_Device,pipeline,nullptr);vkDestroyPipelineLayout(g_Device,layout,nullptr);vkDestroyDescriptorPool(g_Device,pool,nullptr);vkDestroyDescriptorSetLayout(g_Device,setLayout,nullptr);}
    void Buffer(uint32_t b,VulkanBuffer& buffer){BufferRaw(b,buffer.GetBuffer(),buffer.GetSize());}
    void BufferRaw(uint32_t b,VkBuffer buffer,VkDeviceSize size){VkDescriptorBufferInfo info{buffer,0,size};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=b;w.descriptorCount=1;w.descriptorType=types.at(b);w.pBufferInfo=&info;vkUpdateDescriptorSets(g_Device,1,&w,0,nullptr);}
    void Texture(uint32_t b,VkImageView view,VkSampler sampler={},VkImageLayout layout=VK_IMAGE_LAYOUT_GENERAL){uint32_t n=b==39?8:1;std::vector<VkDescriptorImageInfo> infos(n,{sampler,view,layout});VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=b;w.descriptorCount=n;w.descriptorType=types.at(b);w.pImageInfo=infos.data();vkUpdateDescriptorSets(g_Device,1,&w,0,nullptr);}
    void AS(VkAccelerationStructureKHR as){VkWriteDescriptorSetAccelerationStructureKHR a{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};a.accelerationStructureCount=1;a.pAccelerationStructures=&as;VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.pNext=&a;w.dstSet=set;w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;vkUpdateDescriptorSets(g_Device,1,&w,0,nullptr);}
    void Bind(VkCommandBuffer c){vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);}
};

nlohmann::json RenderComplete(VoxRTValidation::Device& device,const std::filesystem::path& output,uint32_t width,uint32_t height,VulkanBuffer& quads,VulkanBuffer& ranges,VulkanBuffer& materials,AccelerationStructure& tlas,const glm::vec3& dimensions){
    const auto start=Clock::now();const auto sun=glm::normalize(glm::vec3(-.65f,.8f,1.0f));
    AtmosphereLUT atmosphere;
    std::cout<<"[Full] initializing ORIGINAL atmosphere resources\n";
    Require(atmosphere.Init(g_Device,g_PhysicalDevice,128,64),"atmosphere init");
    VkCommandBuffer cmd{};
    constexpr auto S=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,U=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,T=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,I=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    std::map<uint32_t,VkDescriptorType> types{{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}};
    for(auto b:{1,2,3,8,16,17,19,20,34,35,36,40,41,42,43,44,45,46,47,48})types[b]=S;
    for(auto b:{6,7,15})types[b]=U;
    for(auto b:{4,10,11,12,13,14,22,23,26})types[b]=I;
    for(auto b:{5,9,18,21,38,39,53,54,55,56,57,58,59})types[b]=T;
    FullPipeline realtime("vox_rt_realtime.comp.spv",types,128);
    FullPipeline composite("vox_rt_composite.comp.spv",{{0,I},{1,T},{2,T},{3,T},{4,T}},0);
    FullPipeline accumulate("preview_accumulate.comp.spv",{{0,I},{1,S},{2,S},{3,I},{4,I},{5,S},{6,S},{7,S}},16);
    std::map<uint32_t,std::unique_ptr<Image>> images;
    for(auto b:{4,10,11,12,13,14,22,23,26})images[b]=std::make_unique<Image>(width,height,b==11?VK_FORMAT_R32_SFLOAT:b==13?VK_FORMAT_R16G16_SFLOAT:b==26?VK_FORMAT_R32G32B32A32_SFLOAT:VK_FORMAT_R16G16B16A16_SFLOAT);
    Image dummyUint(1,1,VK_FORMAT_R32_UINT,true); // DDA disabled; its descriptors still need valid integer 3D views below.
    VkSampler sampler{};VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sci.magFilter=sci.minFilter=VK_FILTER_NEAREST;sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sci.maxLod=0;Check(vkCreateSampler(g_Device,&sci,nullptr,&sampler),"preview sampler");
    struct SamplerCleanup{VkSampler s;~SamplerCleanup(){vkDestroySampler(g_Device,s,nullptr);}} samplerCleanup{sampler};
    VulkanBuffer zero,instances,previous,temporal,primary,tiles,sum,ldr,settings,stbn,giSum,giPixels,directPixels;
    std::vector<uint32_t> zeros(4096);Allocate(zero,zeros.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,zeros.data());
    struct Instance{glm::vec4 color;glm::uvec4 info;glm::vec4 material;};const Instance instance{glm::vec4(1,1,1,0),glm::uvec4(0),glm::vec4(0,1,0,0)};
    Allocate(instances,sizeof(instance),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&instance);glm::mat4 identity(1),transform(1);transform[2][2]=-1;
    Allocate(previous,sizeof(identity),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&identity);
    VulkanBuffer transforms;Allocate(transforms,sizeof(transform),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&transform);
    struct Temporal{glm::mat4 view,previousVP,previousView;glm::vec4 jitter,options,giOptions,sampling;};Temporal history{};Allocate(temporal,sizeof(history),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Allocate(primary,VkDeviceSize(width)*height*24,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Allocate(tiles,16+VkDeviceSize((width+7)/8)*((height+7)/8)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::vector<glm::vec4> blank(size_t(width)*height,glm::vec4(0));Allocate(sum,blank.size()*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,blank.data());Allocate(ldr,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Allocate(giSum,blank.size()*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,blank.data());
    Allocate(giPixels,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Allocate(directPixels,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto noise=RendererUtils::ReadFile("engine/textures/stbn/stbn_vec2_128x128x64.rgba8.bin");
    Require(noise.size()==4194304,"original STBN binary missing (Git LFS resource required)");
    Allocate(stbn,noise.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,noise.data());
    const glm::vec4 skySetting(1,1,1,1);Allocate(settings,16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&skySetting);
    realtime.AS(tlas.Handle());for(auto [b,t]:types)if(t==S||t==U)realtime.Buffer(b,zero);else if(t==T)realtime.Texture(b,images[4]->view,sampler);
    realtime.Texture(39,dummyUint.view,sampler);
    realtime.Buffer(1,quads);realtime.Buffer(2,ranges);realtime.Buffer(3,instances);realtime.Buffer(7,settings);realtime.Buffer(15,temporal);realtime.Buffer(16,previous);realtime.Buffer(17,stbn);realtime.Buffer(19,materials);realtime.Buffer(34,primary);realtime.Buffer(35,transforms);realtime.Buffer(36,tiles);
    realtime.BufferRaw(6,atmosphere.GetSkyCubeSHBuffer(),144);
    realtime.Texture(5,atmosphere.GetSkyRTView(),atmosphere.GetSkyRTSampler(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    realtime.Texture(18,atmosphere.GetTransmittanceView(),atmosphere.GetLUTSampler(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for(auto& [b,image]:images)realtime.Texture(b,image->view);
    composite.Texture(0,images[4]->view);composite.Texture(1,images[10]->view,sampler);composite.Texture(2,images[14]->view,sampler);composite.Texture(3,images[22]->view,sampler);composite.Texture(4,images[23]->view,sampler);
    accumulate.Texture(0,images[4]->view);accumulate.Buffer(1,sum);accumulate.Buffer(2,ldr);accumulate.Texture(3,images[10]->view);accumulate.Texture(4,images[14]->view);accumulate.Buffer(5,giSum);accumulate.Buffer(6,giPixels);accumulate.Buffer(7,directPixels);
    cmd=device.Begin();for(auto& [b,image]:images)image->Init(cmd);dummyUint.Init(cmd);device.SubmitAndWait(cmd);
    const float radius=glm::length(dimensions)*.5f;const float pitch=glm::radians(cameraPitchDegrees);const auto horizontal=glm::normalize(glm::vec2(1.15f,1.5f));
    const auto eye=glm::vec3(horizontal.x*std::cos(pitch),std::sin(pitch),horizontal.y*std::cos(pitch))*radius*3.0f;
    const float altitude=std::max(eye.y+200.f,0.f);
    const auto view=glm::lookAt(eye,glm::vec3(0),glm::vec3(0,1,0));
    struct Camera{glm::mat4 inverseVP;glm::vec4 eye,sun,light,extent;};
    Camera camera{};camera.eye=glm::vec4(eye,altitude);camera.sun=glm::vec4(sun,1);camera.light=glm::vec4(1,.96,.89,1);camera.extent=glm::vec4(width,height,1,kSceneExposure);
    history.view=history.previousView=view;history.sampling=glm::vec4(-4,0,0,0);history.giOptions=glm::vec4(0,1,1,0);
    std::vector<double> nrdTimes;
#ifdef MIKAN_PREVIEW_NRD
    using mikan::denoising::Texture;
    auto texture=[](Image& i){return Texture{i.image,i.view,VK_IMAGE_LAYOUT_GENERAL};};
    std::unique_ptr<mikan::denoising::IDiffuseDenoiser> diffuseDenoiser,specularDenoiser;
    std::unique_ptr<Image> filteredHDR;
    std::unique_ptr<FullPipeline> copyBase,filteredComposite,rawSingle,filteredSingle;
    VulkanBuffer rawFramePixels,filteredFramePixels;
    if(useNrd){
        const mikan::denoising::Device denoiserDevice{g_PhysicalDevice,g_Device,nullptr,g_QueueFamily,1};
        diffuseDenoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd);
        specularDenoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd,mikan::denoising::Signal::Specular);
        std::cout<<"[NRD] initializing production diffuse and specular REBLUR pipelines\n";
        Require(diffuseDenoiser&&diffuseDenoiser->Initialize(denoiserDevice,{width,height}),"NRD diffuse init failed");
        Require(specularDenoiser&&specularDenoiser->Initialize(denoiserDevice,{width,height}),"NRD specular init failed");
        filteredHDR=std::make_unique<Image>(width,height,VK_FORMAT_R16G16B16A16_SFLOAT);
        cmd=device.Begin();filteredHDR->Init(cmd);device.SubmitAndWait(cmd);
        copyBase=std::make_unique<FullPipeline>("preview_copy.comp.spv",std::map<uint32_t,VkDescriptorType>{{0,I},{1,I}},0);
        copyBase->Texture(0,images[4]->view);copyBase->Texture(1,filteredHDR->view);
        filteredComposite=std::make_unique<FullPipeline>("vox_rt_composite.comp.spv",std::map<uint32_t,VkDescriptorType>{{0,I},{1,T},{2,T},{3,T},{4,T}},0);
        filteredComposite->Texture(0,filteredHDR->view);filteredComposite->Texture(2,images[14]->view,sampler);filteredComposite->Texture(4,images[23]->view,sampler);
        Allocate(rawFramePixels,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Allocate(filteredFramePixels,VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        rawSingle=std::make_unique<FullPipeline>("preview_single.comp.spv",std::map<uint32_t,VkDescriptorType>{{0,I},{1,S}},0);
        filteredSingle=std::make_unique<FullPipeline>("preview_single.comp.spv",std::map<uint32_t,VkDescriptorType>{{0,I},{1,S}},0);
        rawSingle->Texture(0,images[4]->view);rawSingle->Buffer(1,rawFramePixels);
        filteredSingle->Texture(0,filteredHDR->view);filteredSingle->Buffer(1,filteredFramePixels);
    }
#endif
    const auto atmoStart=Clock::now();std::cout<<"[Full] generating FULL resolution 4-order Bruneton LUTs (CPU; potentially slow)\n";
    Require(atmosphere.Generate(g_CommandPool,g_Queue),"atmosphere generation");
    const auto atmoMs=Milliseconds(atmoStart);std::cout<<"[Full] atmosphere generated in "<<atmoMs<<" ms\n";
    cmd=device.Begin();atmosphere.DispatchSky(cmd,sun,altitude);atmosphere.DispatchPanoToCube(cmd,sun,altitude);atmosphere.DispatchSHProj(cmd,sun,altitude);device.SubmitAndWait(cmd);
    const auto traceStart=Clock::now();std::vector<double> frameTimes;
    for(uint32_t step=0;step<=accumulationFrames;++step){
        const bool probe=step==0;const uint32_t frame=probe?0:step-1,w=probe?64:width,h=probe?36:height;
        camera.extent.x=w;camera.extent.y=h;
        const auto frameStart=Clock::now();glm::mat4 projection=glm::perspectiveRH_ZO(glm::radians(34.f),float(width)/height,.1f,10000.f);projection[1][1]*=-1;const glm::mat4 unjitteredProjection=projection;
        auto radical=[](uint32_t n,uint32_t b){float v=0,f=1.f/b;while(n){v+=f*(n%b);n/=b;f/=b;}return v;};
        const glm::vec2 jitter(radical(frame+1,2)-.5f,radical(frame+1,3)-.5f);projection[2][0]+=2*jitter.x/w;projection[2][1]+=2*jitter.y/h;
        camera.inverseVP=glm::inverse(projection*view);history.previousVP=projection*view;history.options=glm::vec4(frame,1,1,0);history.jitter=glm::vec4(0);temporal.Write(&history,sizeof(history));
        uint32_t dispatch[]{0,1,1,0};tiles.Write(dispatch,sizeof(dispatch));
        cmd=device.Begin();HostBarrier(cmd);RayTracingQueryBarrier(cmd);realtime.Bind(cmd);camera.light.w=1;vkCmdPushConstants(cmd,realtime.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(camera),&camera);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);
        camera.light.w=-1;vkCmdPushConstants(cmd,realtime.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(camera),&camera);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);
#ifdef MIKAN_PREVIEW_NRD
        if(useNrd&&!probe){copyBase->Bind(cmd);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);}
#endif
        composite.Bind(cmd);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);
        accumulate.Bind(cmd);glm::uvec4 push(w,h,frame+1,probe||frame+1==accumulationFrames);vkCmdPushConstants(cmd,accumulate.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&push);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);
#ifdef MIKAN_PREVIEW_NRD
        const bool captureFrame=!probe&&(frame==0||frame+1==accumulationFrames);
        if(useNrd&&captureFrame){rawSingle->Bind(cmd);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);ComputeBarrier(cmd);}
#endif
        VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};hostRead.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;hostRead.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hostRead,0,nullptr,0,nullptr);
        device.SubmitAndWait(cmd);
#ifdef MIKAN_PREVIEW_NRD
        if(useNrd&&!probe){
            const auto nrdStart=Clock::now();cmd=device.Begin();
            mikan::denoising::Frame nrdFrame;nrdFrame.view=view;nrdFrame.projection=unjitteredProjection;nrdFrame.jitterNdc=2.f*jitter/glm::vec2(width,height);nrdFrame.sourceExtent={width,height};nrdFrame.frameSlot=0;nrdFrame.resetHistory=frame==0;
            mikan::denoising::DiffuseInputs inputs;inputs.depth=texture(*images[11]);inputs.worldNormal=texture(*images[12]);inputs.motion=texture(*images[13]);inputs.linearViewDepth=true;inputs.unpackedWorldNormal=true;
            inputs.radianceHitDistance=texture(*images[10]);Require(diffuseDenoiser->Record(cmd,nrdFrame,inputs),"NRD diffuse Record failed");
            inputs.radianceHitDistance=texture(*images[22]);Require(specularDenoiser->Record(cmd,nrdFrame,inputs),"NRD specular Record failed");
            const auto filtered=diffuseDenoiser->Output(),filteredSpecular=specularDenoiser->Output();
            filteredComposite->Texture(1,filtered.view,sampler,filtered.layout);filteredComposite->Texture(3,filteredSpecular.view,sampler,filteredSpecular.layout);
            filteredComposite->Bind(cmd);vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);ComputeBarrier(cmd);
            if(captureFrame){filteredSingle->Bind(cmd);vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);ComputeBarrier(cmd);}
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hostRead,0,nullptr,0,nullptr);
            device.SubmitAndWait(cmd);nrdTimes.push_back(Milliseconds(nrdStart));
            if(captureFrame){
                std::vector<uint32_t> captured(size_t(width)*height);
                auto save=[&](VulkanBuffer& buffer,const std::string& name){buffer.Map();std::memcpy(captured.data(),buffer.GetMappedPtr(),captured.size()*4);for(auto p:captured)Require((p>>24)==255,"NRD capture incomplete");SaveBmp(output/name,width,height,captured);};
                const std::string suffix=frame==0?"first":"last";save(rawFramePixels,"knight-raw-"+suffix+".bmp");save(filteredFramePixels,"knight-nrd-"+suffix+".bmp");
            }
            std::cout<<"[NRD] frame "<<frame+1<<" diffuse+specular+composite_ms="<<nrdTimes.back()<<'\n';
        }
#endif
        if(probe){ldr.Map();const auto* test=static_cast<const uint32_t*>(ldr.GetMappedPtr());for(uint32_t i=0;i<w*h;++i)Require((test[i]>>24)==255,"low-resolution probe incomplete");sum.Write(blank.data(),blank.size()*16);giSum.Write(blank.data(),blank.size()*16);std::cout<<"[Full] 64x36 GI probe passed; beginning final resolution\n";continue;}
        frameTimes.push_back(Milliseconds(frameStart));std::cout<<"[Full] frame "<<frame+1<<'/'<<accumulationFrames<<" ms="<<frameTimes.back()<<'\n';
    }
    ldr.Map();std::vector<uint32_t> image(size_t(width)*height);std::memcpy(image.data(),ldr.GetMappedPtr(),image.size()*4);for(auto p:image)Require((p>>24)==255,"incomplete output");SaveBmp(output/"knight-540p.bmp",width,height,image);
    giPixels.Map();std::memcpy(image.data(),giPixels.GetMappedPtr(),image.size()*4);SaveBmp(output/"knight-gi.bmp",width,height,image);
    directPixels.Map();std::memcpy(image.data(),directPixels.GetMappedPtr(),image.size()*4);SaveBmp(output/"knight-direct-only.bmp",width,height,image);
    giSum.Map();double giEnergy=0;size_t hitPixels=0;const auto* gi=static_cast<const glm::vec4*>(giSum.GetMappedPtr());for(size_t i=0;i<blank.size();++i){Require(std::isfinite(gi[i].x)&&std::isfinite(gi[i].y)&&std::isfinite(gi[i].z),"nonfinite GI");giEnergy+=gi[i].x+gi[i].y+gi[i].z;hitPixels+=gi[i].w>0;}
    Require(hitPixels>100&&giEnergy>0,"missing geometry or GI contribution");
    sum.Map();const auto* radiance=static_cast<const glm::vec4*>(sum.GetMappedPtr());double energy=0;for(size_t i=0;i<blank.size();++i){Require(std::isfinite(radiance[i].x)&&std::isfinite(radiance[i].y)&&std::isfinite(radiance[i].z),"nonfinite HDR");energy+=radiance[i].x+radiance[i].y+radiance[i].z;}Require(energy>0,"black HDR");
    std::ofstream hdr(output/"knight-linear-rgba32f.bin",std::ios::binary);std::vector<glm::vec4> average(blank.size());for(size_t i=0;i<blank.size();++i)average[i]=radiance[i]/float(accumulationFrames);hdr.write(reinterpret_cast<const char*>(average.data()),average.size()*16);
    return {{"camera_pitch_degrees",cameraPitchDegrees},{"camera_position",{eye.x,eye.y,eye.z}},{"camera_target",{0,0,0}},{"sky_panorama_width",128},{"sky_panorama_height",64},{"atmosphere_altitude_meters",altitude},{"width",width},{"height",height},{"accumulated_frames",accumulationFrames},{"primary_samples_per_pixel",accumulationFrames},{"gi_sample_budget_per_surface_pixel",accumulationFrames*4},{"gi_samples_per_frame",4},{"diffuse_continuations",1},{"max_mirror_continuations",2},{"atmosphere_orders",4},{"atmosphere_lut_ms",atmoMs},{"frame_ms",frameTimes},{"trace_total_ms",Milliseconds(traceStart)},{"total_ms",Milliseconds(start)},{"tonemap","hardware_rt_present.frag: rtDisplayMap + rtLinearToSrgb"},{"renderer","original vox_rt_realtime.comp NEE/MIS + vox_rt_composite.comp"},{"stbn",true},{"denoiser",useNrd},{"nrd_frame_ms",nrdTimes},{"nrd_frames_processed",nrdTimes.size()},{"nrd_external_frame_average",false},{"restir",false},{"hdr_finite",true},{"physical_sun",true},{"gi_energy_sum",giEnergy/accumulationFrames},{"geometry_pixels_union",hitPixels},{"gi_probe",true}};
}
}
int main(int argc,char** argv){try{
    Core::SetLogFileEnabled(false);std::cout<<std::unitbuf;Require(argc>=2,"usage: MikanVoxCompletePreview knight.vox [output] [width height frames pitch_degrees [nrd]]");std::filesystem::path output=argc>2?argv[2]:"out/knight-540p";uint32_t width=argc>3?std::stoul(argv[3]):960,height=argc>4?std::stoul(argv[4]):540;accumulationFrames=argc>5?std::stoul(argv[5]):32;if(argc>7){Require(std::string(argv[7])=="nrd","unknown render mode");useNrd=true;}
#ifndef MIKAN_PREVIEW_NRD
    Require(!useNrd,"NRD was not built; configure MIKAN_SOFTWARE_NRD=ON");
#endif
    if(argc>6)cameraPitchDegrees=std::stof(argv[6]);Require(std::isfinite(cameraPitchDegrees)&&std::abs(cameraPitchDegrees)<=80,"pitch must be finite and within -80..80 degrees");Require(width>=32&&height>=32&&width<=1920&&height<=1080&&accumulationFrames>0&&accumulationFrames<=256,"invalid size/frames");std::filesystem::create_directories(output);
    VoxRTValidation::Device device;device.Initialize(true,true,true);auto report=RenderModel(device,argv[1],output,width,height);report["device"]=device.name;report["device_type"]="CPU";device.Shutdown();report["validation_errors"]=device.validationErrors;report["validation_warnings"]=device.validationWarnings;std::ofstream(output/"complete-report.json")<<report.dump(2)<<'\n';Require(device.validationErrors==0,"validation errors");
    return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}}
