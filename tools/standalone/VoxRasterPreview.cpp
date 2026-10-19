// Geometry-only Vulkan host. Uses the unmodified production voxel_quad.vert
// and voxel_mesh.frag, with SDL/X11 swapchain presentation and CPU readback.
// There is no lighting pass, postprocessing, compute culling or ray tracing.
#include "VoxSurfaceValidation.h"
#include "Rendering/VoxSurfaceAttributes.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <limits>

namespace {
using VoxValidation::Require;
uint32_t validationErrors=0, validationWarnings=0;
void Check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult="+std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){++validationErrors;std::cerr<<"VULKAN ERROR: "<<data->pMessage<<'\n';}
    else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){++validationWarnings;std::cerr<<"VULKAN WARNING: "<<data->pMessage<<'\n';}
    return VK_FALSE;
}
struct Context;
struct Buffer {
    VkDevice device{}; VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{}; VkDeviceSize bytes{};
    Buffer(Context& context,VkDeviceSize size,VkBufferUsageFlags usage,const void* data=nullptr);
    ~Buffer(){if(mapped)vkUnmapMemory(device,memory);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);}
    Buffer(const Buffer&)=delete;
};
struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
struct Capture {std::vector<uint8_t> rgba;std::vector<float> depth;uint64_t foreground=0;};
struct Context {
    SDL_Window* window{};
    VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};VkPhysicalDeviceProperties properties{};
    VkDevice device{};VkQueue queue{};uint32_t queueFamily=UINT32_MAX;
    VkSwapchainKHR swapchain{};VkFormat colorFormat{};VkExtent2D extent{};
    std::vector<VkImage> swapImages;std::vector<VkImageView> swapViews;std::vector<VkFramebuffer> framebuffers;
    Image normal,depth;
    VkRenderPass renderPass{};VkDescriptorSetLayout setLayout{};VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};
    VkDescriptorPool descriptorPool{};VkCommandPool commandPool{};
    VkSemaphore imageReady{},renderDone{};VkFence fence{};
    uint64_t presentedFrames=0;
    Context(uint32_t width,uint32_t height);
    ~Context(){Shutdown();}
    void Shutdown();
    uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(physical,&props);
        for(uint32_t i=0;i<props.memoryTypeCount;++i)if((bits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("required Vulkan memory type unavailable");
    }
    Image CreateImage(VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect);
    Capture Render(const VoxValidation::Baked& baked,const VoxValidation::Cell& minimum,
                   const VoxValidation::Cell& maximum,int angle,const std::filesystem::path& base);
};

Buffer::Buffer(Context& c,VkDeviceSize size,VkBufferUsageFlags usage,const void* data):device(c.device),bytes(size) {
    Require(size>0,"empty buffer");
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;info.usage=usage;
    Check(vkCreateBuffer(device,&info,nullptr,&buffer),"create buffer");
    VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=c.MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device,&allocation,nullptr,&memory),"allocate buffer memory");
    Check(vkBindBufferMemory(device,buffer,memory,0),"bind buffer memory");
    Check(vkMapMemory(device,memory,0,size,0,&mapped),"map buffer");
    if(data)std::memcpy(mapped,data,size);else std::memset(mapped,0,size);
}
Image Context::CreateImage(VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
    info.extent={extent.width,extent.height,1};info.mipLevels=info.arrayLayers=1;
    info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=usage;
    Check(vkCreateImage(device,&info,nullptr,&result.image),"create image");
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,result.image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device,&allocation,nullptr,&result.memory),"allocate image memory");
    Check(vkBindImageMemory(device,result.image,result.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=result.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.format=format;view.subresourceRange={aspect,0,1,0,1};
    Check(vkCreateImageView(device,&view,nullptr,&result.view),"create image view");return result;
}
VkShaderModule Shader(VkDevice device,const char* name) {
    std::ifstream file(std::filesystem::path(MIKAN_RASTER_SHADER_DIR)/name,std::ios::binary|std::ios::ate);
    Require(bool(file),"cannot read compiled production shader");
    const auto size=file.tellg();Require(size>0&&size%4==0,"bad SPIR-V size");
    std::vector<uint32_t> words(size_t(size)/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),size);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=size;info.pCode=words.data();
    VkShaderModule module{};Check(vkCreateShaderModule(device,&info,nullptr,&module),"create shader module");return module;
}

Context::Context(uint32_t width,uint32_t height) {
    Require(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
    window=SDL_CreateWindow("Mikan VOX geometry / CPU Vulkan",int(width),int(height),SDL_WINDOW_VULKAN);
    Require(window,SDL_GetError());
    uint32_t count=0;const auto* sdlExtensions=SDL_Vulkan_GetInstanceExtensions(&count);
    Require(sdlExtensions,SDL_GetError());
    std::vector<const char*> extensions(sdlExtensions,sdlExtensions+count);extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    uint32_t layerCount=0;Check(vkEnumerateInstanceLayerProperties(&layerCount,nullptr),"enumerate layers");
    std::vector<VkLayerProperties> layers(layerCount);Check(vkEnumerateInstanceLayerProperties(&layerCount,layers.data()),"enumerate layers");
    Require(std::any_of(layers.begin(),layers.end(),[](auto& layer){return std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")==0;}),
            "VK_LAYER_KHRONOS_validation is required for this validation host");
    const char* layer="VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="MikanGeometryValidation";app.apiVersion=VK_API_VERSION_1_1;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback=Debug;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pNext=&debug;create.pApplicationInfo=&app;
    create.enabledExtensionCount=uint32_t(extensions.size());create.ppEnabledExtensionNames=extensions.data();
    create.enabledLayerCount=1;create.ppEnabledLayerNames=&layer;
    Check(vkCreateInstance(&create,nullptr,&instance),"create instance");
    auto makeDebug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
    Require(makeDebug,"debug messenger extension missing");Check(makeDebug(instance,&debug,nullptr,&messenger),"create debug messenger");
    Require(SDL_Vulkan_CreateSurface(window,instance,nullptr,&surface),SDL_GetError());
    uint32_t deviceCount=0;Check(vkEnumeratePhysicalDevices(instance,&deviceCount,nullptr),"enumerate devices");
    std::vector<VkPhysicalDevice> devices(deviceCount);Check(vkEnumeratePhysicalDevices(instance,&deviceCount,devices.data()),"enumerate devices");
    for(auto candidate:devices) {
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(candidate,&props);
        if(props.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
        uint32_t familyCount=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&familyCount,families.data());
        for(uint32_t i=0;i<familyCount;++i) {
            VkBool32 present=VK_FALSE;Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present),"query presentation support");
            if((families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&present){physical=candidate;queueFamily=i;properties=props;break;}
        }
        if(physical)break;
    }
    Require(physical,"no CPU Vulkan device with SDL surface presentation support");
    Require(properties.limits.maxPushConstantsSize>=144,"production VOX shader requires 144 bytes of push constants");
    const float priority=1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=queueFamily;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    const char* swapExtension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queueInfo;
    deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=&swapExtension;
    Check(vkCreateDevice(physical,&deviceInfo,nullptr,&device),"create device");vkGetDeviceQueue(device,queueFamily,0,&queue);
    VkSurfaceCapabilitiesKHR caps{};Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"surface capabilities");
    Require(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT,"swapchain readback not supported");
    uint32_t formatCount=0;Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,nullptr),"surface formats");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,formats.data()),"surface formats");
    auto format=std::find_if(formats.begin(),formats.end(),[](auto f){return f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM;});
    Require(format!=formats.end(),"UNORM swapchain required for raw albedo output");colorFormat=format->format;
    extent=caps.currentExtent.width!=UINT32_MAX?caps.currentExtent:VkExtent2D{std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height)};
    uint32_t imageCount=caps.minImageCount+1;if(caps.maxImageCount)imageCount=std::min(imageCount,caps.maxImageCount);
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};swapInfo.surface=surface;
    swapInfo.minImageCount=imageCount;swapInfo.imageFormat=colorFormat;swapInfo.imageColorSpace=format->colorSpace;swapInfo.imageExtent=extent;
    swapInfo.imageArrayLayers=1;swapInfo.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapInfo.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;swapInfo.preTransform=caps.currentTransform;
    swapInfo.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&swapInfo.compositeAlpha))swapInfo.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
    swapInfo.presentMode=VK_PRESENT_MODE_FIFO_KHR;swapInfo.clipped=VK_TRUE;
    Check(vkCreateSwapchainKHR(device,&swapInfo,nullptr,&swapchain),"create swapchain");
    Check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,nullptr),"swapchain images");swapImages.resize(imageCount);
    Check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,swapImages.data()),"swapchain images");
    for(auto image:swapImages) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=colorFormat;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView handle{};
        Check(vkCreateImageView(device,&view,nullptr,&handle),"swapchain view");swapViews.push_back(handle);
    }
    normal=CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    depth=CreateImage(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    std::array<VkAttachmentDescription,3> attachments{};
    for(auto& a:attachments){a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;}
    attachments[0].format=colorFormat;attachments[1].format=VK_FORMAT_R32G32B32A32_SFLOAT;attachments[2].format=VK_FORMAT_D32_SFLOAT;
    std::array<VkAttachmentReference,5> colors{{{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED},{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED},{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED}}};
    VkAttachmentReference depthReference{2,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=uint32_t(colors.size());
    subpass.pColorAttachments=colors.data();subpass.pDepthStencilAttachment=&depthReference;
    std::array<VkSubpassDependency,2> dependencies{};
    dependencies[0]={VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0};
    dependencies[1]={0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,0};
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass.attachmentCount=uint32_t(attachments.size());pass.pAttachments=attachments.data();
    pass.subpassCount=1;pass.pSubpasses=&subpass;pass.dependencyCount=uint32_t(dependencies.size());pass.pDependencies=dependencies.data();
    Check(vkCreateRenderPass(device,&pass,nullptr,&renderPass),"create geometry renderpass");
    for(auto view:swapViews) {
        const VkImageView views[]{view,normal.view,depth.view};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.attachmentCount=3;fb.pAttachments=views;
        fb.width=extent.width;fb.height=extent.height;fb.layers=1;VkFramebuffer handle{};
        Check(vkCreateFramebuffer(device,&fb,nullptr,&handle),"create framebuffer");framebuffers.push_back(handle);
    }
    std::array<VkDescriptorSetLayoutBinding,3> bindings{{{0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=uint32_t(bindings.size());set.pBindings=bindings.data();
    Check(vkCreateDescriptorSetLayout(device,&set,nullptr,&setLayout),"create descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,144};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&setLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    Check(vkCreatePipelineLayout(device,&layout,nullptr,&pipelineLayout),"create pipeline layout");
    const auto vertex=Shader(device,"voxel_quad.vert.spv"),fragment=Shader(device,"voxel_mesh.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr};
    stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment,"main",nullptr};
    VkVertexInputBindingDescription binding{0,4,VK_VERTEX_INPUT_RATE_INSTANCE};VkVertexInputAttributeDescription attribute{3,0,VK_FORMAT_R32_UINT,0};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&binding;input.vertexAttributeDescriptionCount=1;input.pVertexAttributeDescriptions=&attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};VkRect2D scissor{{0,0},extent};
    VkPipelineViewportStateCreateInfo viewportInfo{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewportInfo.viewportCount=1;viewportInfo.pViewports=&viewport;viewportInfo.scissorCount=1;viewportInfo.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depthInfo.depthTestEnable=depthInfo.depthWriteEnable=VK_TRUE;depthInfo.depthCompareOp=VK_COMPARE_OP_LESS;
    std::array<VkPipelineColorBlendAttachmentState,5> blendAttachments{};for(auto& attachment:blendAttachments)attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=uint32_t(blendAttachments.size());blend.pAttachments=blendAttachments.data();
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};graphics.stageCount=uint32_t(stages.size());graphics.pStages=stages.data();
    graphics.pVertexInputState=&input;graphics.pInputAssemblyState=&assembly;graphics.pViewportState=&viewportInfo;graphics.pRasterizationState=&raster;
    graphics.pMultisampleState=&samples;graphics.pDepthStencilState=&depthInfo;graphics.pColorBlendState=&blend;graphics.layout=pipelineLayout;graphics.renderPass=renderPass;
    const auto pipelineResult=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&graphics,nullptr,&pipeline);
    vkDestroyShaderModule(device,vertex,nullptr);vkDestroyShaderModule(device,fragment,nullptr);Check(pipelineResult,"create production geometry pipeline");
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
    Check(vkCreateDescriptorPool(device,&pool,nullptr,&descriptorPool),"create descriptor pool");
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};commands.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;commands.queueFamilyIndex=queueFamily;
    Check(vkCreateCommandPool(device,&commands,nullptr,&commandPool),"create command pool");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};Check(vkCreateSemaphore(device,&semaphore,nullptr,&imageReady),"create semaphore");Check(vkCreateSemaphore(device,&semaphore,nullptr,&renderDone),"create semaphore");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};Check(vkCreateFence(device,&fenceInfo,nullptr,&fence),"create fence");
    std::cout<<"Vulkan CPU device: "<<properties.deviceName<<"; SDL driver: "<<SDL_GetCurrentVideoDriver()<<"; swapchain: "<<extent.width<<'x'<<extent.height<<'\n';
}

void SaveBmp(const std::filesystem::path& path,uint32_t width,uint32_t height,std::vector<uint8_t>& rgba) {
    SDL_Surface* surface=SDL_CreateSurfaceFrom(int(width),int(height),SDL_PIXELFORMAT_RGBA32,rgba.data(),int(width*4));
    Require(surface,SDL_GetError());const bool saved=SDL_SaveBMP(surface,Utf8String(path).c_str());SDL_DestroySurface(surface);Require(saved,SDL_GetError());
}
Capture Context::Render(const VoxValidation::Baked& baked,const VoxValidation::Cell& minimum,
                       const VoxValidation::Cell& maximum,int angle,const std::filesystem::path& base) {
    Require(!baked.quads.empty(),"cannot render an empty surface");
    std::vector<uint32_t> words;std::vector<VoxPlaneRange> planes;
    for(const auto& range:baked.ranges)for(uint32_t q=range.firstQuad;q<range.firstQuad+range.quadCount;++q) {
        const auto quad=baked.quads.at(q);const int d=int(range.direction);
        const uint32_t xyz[]{quad.geometry&255u,(quad.geometry>>8)&255u,(quad.geometry>>16)&255u};
        const int axis=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;
        Require(q==words.size(),"surface ranges are not contiguous");
        words.push_back(xyz[u]|(xyz[v]<<8)|((quad.geometry>>24)<<16)|((quad.appearance&255u)<<24));
        AppendVoxPlane(planes,q,xyz[axis],uint32_t(d));
    }
    const uint32_t lookup=uint32_t(words.size());words.resize(words.size()*2,0);
    AppendVoxRasterAttributes(words,baked.attributes,0,uint32_t(baked.quads.size()),lookup);
    AppendVoxPlaneFooter(words,planes,lookup);
    std::vector<uint32_t> indices;indices.reserve(baked.quads.size()*6);
    for(uint32_t q=0;q<baked.quads.size();++q)for(uint32_t corner:{0u,2u,1u,0u,3u,2u})indices.push_back(q*4+corner);
    glm::vec3 dimensions(float(maximum[0]-minimum[0]+1),float(maximum[2]-minimum[2]+1),float(maximum[1]-minimum[1]+1));
    const glm::mat4 model=glm::scale(glm::mat4(1),glm::vec3(1,1,-1));
    std::array<glm::vec4,12> source{};
    for(int i=0;i<4;++i){source[i]=model[i];source[i+4]=model[i];}
    source[8]=glm::vec4(1);source[10]=glm::vec4(-dimensions*.5f,1);
    Buffer sourceBuffer(*this,sizeof(source),VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,source.data());
    Buffer geometry(*this,words.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,words.data());
    Buffer index(*this,indices.size()*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices.data());
    const uint32_t directions[]{0,1,2,3,4,5};Buffer instances(*this,sizeof(directions),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,directions);
    const size_t pixelCount=size_t(extent.width)*extent.height;
    Buffer readColor(*this,pixelCount*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer readNormal(*this,pixelCount*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer readDepth(*this,pixelCount*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};viewInfo.buffer=sourceBuffer.buffer;viewInfo.format=VK_FORMAT_R32G32B32A32_SFLOAT;viewInfo.range=sourceBuffer.bytes;
    VkBufferView sourceView{};Check(vkCreateBufferView(device,&viewInfo,nullptr,&sourceView),"source texel view");
    Check(vkResetDescriptorPool(device,descriptorPool,0),"reset descriptor pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptorPool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&setLayout;
    VkDescriptorSet descriptor{};Check(vkAllocateDescriptorSets(device,&allocate,&descriptor),"allocate descriptors");
    VkDescriptorBufferInfo bufferInfo{geometry.buffer,0,geometry.bytes};std::array<VkWriteDescriptorSet,3> writes{};
    for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;if(i)writes[i].pBufferInfo=&bufferInfo;else writes[i].pTexelBufferView=&sourceView;}
    vkUpdateDescriptorSets(device,uint32_t(writes.size()),writes.data(),0,nullptr);
    uint32_t imageIndex=0;Check(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,imageReady,VK_NULL_HANDLE,&imageIndex),"acquire SDL swapchain image");
    Check(vkResetCommandPool(device,commandPool,0),"reset command pool");
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commandInfo.commandPool=commandPool;commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandInfo.commandBufferCount=1;
    VkCommandBuffer command{};Check(vkAllocateCommandBuffers(device,&commandInfo,&command),"allocate command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;Check(vkBeginCommandBuffer(command,&begin),"begin commands");
    std::array<VkClearValue,3> clear{};clear[0].color={{.055f,.07f,.10f,1}};clear[1].color={{0,0,0,0}};clear[2].depthStencil={1,0};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};render.renderPass=renderPass;render.framebuffer=framebuffers.at(imageIndex);render.renderArea={{0,0},extent};render.clearValueCount=uint32_t(clear.size());render.pClearValues=clear.data();
    vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor,0,nullptr);
    const float radius=glm::length(dimensions)*.5f;
    const glm::vec3 eye=glm::normalize(angle==0?glm::vec3(1.35f,.95f,1.8f):glm::vec3(-1.7f,.65f,-1.4f))*radius*3.2f;
    auto projection=glm::perspectiveRH_ZO(glm::radians(45.f),float(extent.width)/extent.height,radius*.01f,radius*10.f);projection[1][1]*=-1;
    const auto projView=projection*glm::lookAtRH(eye,glm::vec3(0),glm::vec3(0,1,0));
    struct Push {glm::mat4 current,previous;glm::vec4 camera;} push{projView,projView,glm::vec4(eye,0)};
    static_assert(sizeof(Push)==144);vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),&push);
    const VkDeviceSize zero=0;vkCmdBindVertexBuffers(command,0,1,&instances.buffer,&zero);vkCmdBindIndexBuffer(command,index.buffer,0,VK_INDEX_TYPE_UINT32);
    for(const auto& range:baked.ranges)vkCmdDrawIndexed(command,range.quadCount*6,1,range.firstQuad*6,0,range.direction);
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
    vkCmdCopyImageToBuffer(command,swapImages[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readColor.buffer,1,&copy);
    vkCmdCopyImageToBuffer(command,normal.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readNormal.buffer,1,&copy);
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;vkCmdCopyImageToBuffer(command,depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readDepth.buffer,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkImageMemoryBarrier present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};present.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;present.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    present.srcQueueFamilyIndex=present.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;present.image=swapImages[imageIndex];present.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};present.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&present);
    Check(vkEndCommandBuffer(command),"end commands");
    Check(vkResetFences(device,1,&fence),"reset fence");const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&imageReady;submit.pWaitDstStageMask=&waitStage;
    submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&renderDone;
    Check(vkQueueSubmit(queue,1,&submit,fence),"submit geometry draw");
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};presentInfo.waitSemaphoreCount=1;presentInfo.pWaitSemaphores=&renderDone;presentInfo.swapchainCount=1;presentInfo.pSwapchains=&swapchain;presentInfo.pImageIndices=&imageIndex;
    Check(vkQueuePresentKHR(queue,&presentInfo),"present to SDL window");++presentedFrames;
    Check(vkWaitForFences(device,1,&fence,VK_TRUE,30'000'000'000ull),"wait for readback");Check(vkQueueWaitIdle(queue),"wait for presentation");
    Capture capture;capture.rgba.resize(pixelCount*4);capture.depth.resize(pixelCount);std::memcpy(capture.rgba.data(),readColor.mapped,pixelCount*4);std::memcpy(capture.depth.data(),readDepth.mapped,pixelCount*4);
    if(colorFormat==VK_FORMAT_B8G8R8A8_UNORM)for(size_t i=0;i<pixelCount;++i)std::swap(capture.rgba[i*4],capture.rgba[i*4+2]);
    std::vector<uint8_t> normalRgba(pixelCount*4);const auto* normalValues=static_cast<const float*>(readNormal.mapped);
    for(size_t i=0;i<pixelCount;++i) {
        const float z=capture.depth[i];Require(std::isfinite(z)&&z>=0&&z<=1,"invalid depth readback");
        glm::vec3 n(0);if(z<1){++capture.foreground;const glm::vec2 oct(normalValues[i*4],normalValues[i*4+1]);n={oct.x,oct.y,1.f-std::abs(oct.x)-std::abs(oct.y)};
            if(n.z<0){const auto old=n;n.x=(1-std::abs(old.y))*(old.x<0?-1.f:1.f);n.y=(1-std::abs(old.x))*(old.y<0?-1.f:1.f);}n=glm::normalize(n);}
        for(int c=0;c<3;++c)normalRgba[i*4+c]=z<1?uint8_t(std::clamp((n[c]*.5f+.5f)*255.f,0.f,255.f)):capture.rgba[i*4+c];normalRgba[i*4+3]=255;
    }
    Require(capture.foreground>100,"rendered image is blank or too small");
    SaveBmp(base.string()+"_albedo.bmp",extent.width,extent.height,capture.rgba);
    SaveBmp(base.string()+"_normals.bmp",extent.width,extent.height,normalRgba);
    std::ofstream depthFile(base.string()+"_depth.f32",std::ios::binary);depthFile.write(reinterpret_cast<const char*>(capture.depth.data()),pixelCount*4);Require(depthFile.good(),"depth file write failed");
    vkDestroyBufferView(device,sourceView,nullptr);SDL_PumpEvents();return capture;
}

void Context::Shutdown() {
    if(device) {
        vkDeviceWaitIdle(device);
        if(fence)vkDestroyFence(device,fence,nullptr);if(imageReady)vkDestroySemaphore(device,imageReady,nullptr);if(renderDone)vkDestroySemaphore(device,renderDone,nullptr);
        if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        for(auto fb:framebuffers)vkDestroyFramebuffer(device,fb,nullptr);if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);
        for(auto* image:{&normal,&depth}){if(image->view)vkDestroyImageView(device,image->view,nullptr);if(image->image)vkDestroyImage(device,image->image,nullptr);if(image->memory)vkFreeMemory(device,image->memory,nullptr);}
        for(auto view:swapViews)vkDestroyImageView(device,view,nullptr);if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
        vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;
    }
    if(instance){if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));if(destroy)destroy(instance,messenger,nullptr);}vkDestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;}
    if(window){SDL_DestroyWindow(window);window=nullptr;}SDL_Quit();
}
} // namespace

int main(int argc,char** argv) {
    try {
        Core::SetLogFileEnabled(false);std::vector<std::filesystem::path> models;std::filesystem::path output="out/software-vulkan";bool strictComparison=false;
        for(int i=1;i<argc;++i){if(std::string(argv[i])=="--output"){Require(i+1<argc,"--output requires directory");output=Utf8Path(argv[++i]);}else if(std::string(argv[i])=="--strict-comparison")strictComparison=true;else models.push_back(std::filesystem::absolute(Utf8Path(argv[i])));}
        Require(!models.empty(),"usage: MikanVoxRasterPreview model.vox [more.vox ...] --output directory");std::filesystem::create_directories(output);
        Context context(640,480);
        nlohmann::json report={{"device",context.properties.deviceName},{"device_type","CPU"},{"sdl_video_driver",SDL_GetCurrentVideoDriver()},
            {"width",context.extent.width},{"height",context.extent.height},{"validation_enabled",true},
            {"vertex_shader","engine/shaders/glsl/voxel_quad.vert"},{"fragment_shader","engine/shaders/glsl/voxel_mesh.frag"},
            {"postprocessing",false},{"ray_tracing",false},{"lighting_pass",false},{"frames",nlohmann::json::array()}};
        bool imagesAgree=true,pixelsIdentical=true;
        for(const auto& path:models) {
            VoxFormat::VoxData data;Require(VoxFormat::LoadVoxFile(Utf8String(path),data),"VOX load failed");
            Require(data.instances.empty()&&data.models.size()==1,"preview currently accepts single assembled models only");
            const auto result=VoxValidation::Analyze(data,data.models[0]);
            for(int angle=0;angle<2;++angle) {
                const auto stem=Utf8String(path.stem())+"_view"+std::to_string(angle);
                const auto start=std::chrono::steady_clock::now();
                const auto baseline=context.Render(result.greedySurface,result.minimum,result.maximum,angle,output/(stem+"_greedy"));
                const auto final=context.Render(result.finalSurface,result.minimum,result.maximum,angle,output/(stem+"_backfill"));
                uint64_t differentPixels=0,maskMismatch=0,depthMismatch=0;uint32_t maxColorDifference=0;float maxDepthDifference=0;
                for(size_t p=0;p<baseline.depth.size();++p){bool different=false;maskMismatch+=(baseline.depth[p]<1)!=(final.depth[p]<1);
                    maxDepthDifference=std::max(maxDepthDifference,std::abs(baseline.depth[p]-final.depth[p]));
                    depthMismatch+=std::abs(baseline.depth[p]-final.depth[p])>=1e-5f;
                    for(int c=0;c<3;++c){const uint32_t difference=uint32_t(std::abs(int(baseline.rgba[p*4+c])-int(final.rgba[p*4+c])));maxColorDifference=std::max(maxColorDifference,difference);different|=difference!=0;}differentPixels+=different;}
                report["frames"].push_back({{"model",Utf8String(path.filename())},{"view",angle},{"greedy_quads",result.stats.greedyQuadCount},
                    {"backfill_quads",result.stats.finalQuadCount},{"foreground_pixels",final.foreground},{"different_albedo_pixels",differentPixels},
                    {"coverage_mismatch_pixels",maskMismatch},{"max_color_difference",maxColorDifference},{"max_depth_difference",maxDepthDifference},
                    {"depth_difference_pixels_over_1e_5",depthMismatch},
                    {"capture_pair_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()}});
                imagesAgree &= maskMismatch==0&&differentPixels==0&&maxDepthDifference<1e-5f;
                pixelsIdentical &= maskMismatch==0&&differentPixels==0;
            }
        }
        report["presented_frames"]=context.presentedFrames;report["albedo_pixel_identical"]=pixelsIdentical;report["strict_comparison_pass"]=imagesAgree;
        report["strict_comparison_required"]=strictComparison;context.Shutdown();report["validation_errors"]=validationErrors;report["validation_warnings"]=validationWarnings;
        std::ofstream file(output/"render-report.json");file<<report.dump(2)<<'\n';Require(file.good(),"report write failed");
        Require(validationErrors==0,"Vulkan validation errors occurred");
        if(strictComparison)Require(imagesAgree,"greedy/backfill raster images disagree; see render-report.json");
        std::cout<<report.dump(2)<<'\n';return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
