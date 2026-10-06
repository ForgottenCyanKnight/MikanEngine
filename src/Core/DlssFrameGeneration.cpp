#include "Core/DlssFrameGeneration.h"
#include "Core/VulkanContext.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Rendering/PostProcessChain.h"
#include "Core/Log.h"
#include "Rendering/RendererBase.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cwchar>
#if defined(MIKAN_ENABLE_DLSS_FG) && defined(_WIN32)
// This implementation must retain access to native Vulkan entry points.
#undef vkCreateInstance
#undef vkCreateDevice
#undef vkCreateSwapchainKHR
#undef vkDestroySwapchainKHR
#undef vkGetSwapchainImagesKHR
#undef vkAcquireNextImageKHR
#undef vkQueuePresentKHR
#undef vkDeviceWaitIdle
#undef vkDestroySurfaceKHR
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <filesystem>
#include <vector>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <glm/gtc/type_ptr.hpp>
namespace {
HMODULE module{}; bool initialized=false,supported=false,enabled=false,requested=false,guidesReady=false,hudlessReady=false;
bool inputFailure=false; bool editorRectValid=false; sl::Extent editorRect{}; int previousOwner=-1;
VkInstance trackedInstance{}; VkDevice trackedDevice{};
std::wstring libraryPath,pluginPath,logPath; std::string libraryUtf8;
PFN_vkGetInstanceProcAddr proxyInstance{};PFN_vkGetDeviceProcAddr proxyDevice{};
PFun_slShutdown* shutdownSL{}; PFun_slIsFeatureSupported* supportedSL{};
PFun_slGetFeatureFunction* featureFunction{};PFun_slGetNewFrameToken* newToken{};
PFun_slSetConstants* setConstants{};PFun_slSetTagForFrame* setTags{};
PFun_slDLSSGSetOptions* setOptions{};PFun_slDLSSGGetState* getState{};
PFun_slReflexSetOptions* reflexOptions{};PFun_slReflexSleep* reflexSleep{};
PFun_slPCLSetMarker* pclMarker{};sl::FrameToken* token{};
glm::mat4 previousVP(1);bool previousValid=false;
VulkanImage depth,motion,hudless;uint32_t guideWidth{},guideHeight{},colorWidth{},colorHeight{};
VkFormat colorFormat{};bool guideInitialized=false,colorInitialized=false;
VkDescriptorSetLayout descriptorLayout{};VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};
VkDescriptorPool descriptorPool{};VkDescriptorSet descriptor{};VkSampler sampler{};
bool Check(sl::Result r,const char* operation){if(r==sl::Result::eOk)return true;LOGW("[DLSS FG] %s failed (SL=%u)",operation,unsigned(r));return false;}
void Log(sl::LogType type,const char* message){if(type==sl::LogType::eError)LOGE("[Streamline FG] %s",message);else if(type==sl::LogType::eWarn)LOGW("[Streamline FG] %s",message);else LOGD("[Streamline FG] %s",message);}
template<class T>T* Load(const char* name){return reinterpret_cast<T*>(GetProcAddress(module,name));}
template<class T>T* Feature(sl::Feature feature,const char* name){void* p{};if(!Check(featureFunction(feature,name,p),name))return nullptr;return reinterpret_cast<T*>(p);}
bool Signed(const std::wstring& path){
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;const auto result=WinVerifyTrust(nullptr,&action,&data);
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);return result==ERROR_SUCCESS;
}
void Transition(VkCommandBuffer cmd,VkImage image,VkImageLayout oldLayout,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst){
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;
    barrier.oldLayout=oldLayout;barrier.newLayout=next;barrier.srcAccessMask=src;barrier.dstAccessMask=dst;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
}
bool Image(VulkanImage& image,uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage){
    image.Cleanup();return image.Create(w,h,format,VK_IMAGE_TILING_OPTIMAL,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)&&image.CreateView(format,VK_IMAGE_ASPECT_COLOR_BIT);
}
sl::Resource Resource(VulkanImage& image,uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage){
    sl::Resource r{sl::ResourceType::eTex2d,reinterpret_cast<void*>(image.GetImage()),reinterpret_cast<void*>(image.GetMemory()),reinterpret_cast<void*>(image.GetView()),VK_IMAGE_LAYOUT_GENERAL};
    r.width=w;r.height=h;r.nativeFormat=format;r.mipLevels=r.arrayLayers=1;r.flags=0;r.usage=usage;return r;
}
bool PreparePipeline(){
    if(pipeline)return true;
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=3;si.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(g_Device,&si,g_Allocator,&descriptorLayout)!=VK_SUCCESS)return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,32};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&descriptorLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(g_Device,&li,g_Allocator,&pipelineLayout)!=VK_SUCCESS)return false;
    auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("dlss_fg_prepare.comp.spv"));auto shader=RendererUtils::CreateShaderModule(code,"dlss_fg_prepare.comp.spv");if(!shader)return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";
    const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&ci,g_Allocator,&pipeline);vkDestroyShaderModule(g_Device,shader,g_Allocator);if(result!=VK_SUCCESS)return false;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
    if(vkCreateDescriptorPool(g_Device,&pi,g_Allocator,&descriptorPool)!=VK_SUCCESS)return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptorPool;ai.descriptorSetCount=1;ai.pSetLayouts=&descriptorLayout;
    if(vkAllocateDescriptorSets(g_Device,&ai,&descriptor)!=VK_SUCCESS)return false;
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sm.magFilter=sm.minFilter=VK_FILTER_NEAREST;sm.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sm.addressModeU=sm.addressModeV=sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(g_Device,&sm,g_Allocator,&sampler)==VK_SUCCESS;
}
}
namespace Core::DlssFG {
void SetRequested(bool value){requested=value;inputFailure=false;previousValid=false;}
bool IsRequested(){return requested;}
bool IsSupported(){return supported;}
bool IsEnabled(){return enabled;}
bool IsInitialized(){return initialized;}
bool Initialize(){
    const char* request=std::getenv("MIKAN_DLSS_FG");
    // Explicit zero preserves native presentation for captures. Otherwise preload
    // the proxy before Vulkan creation so the panel can enable FG during play.
    requested=request && request[0]=='1';
    if(request && request[0]=='0')return false;
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);pluginPath=std::filesystem::path(exe).parent_path().wstring();libraryPath=pluginPath+L"\\sl.interposer.dll";
    if(!Signed(libraryPath)){LOGW("[DLSS FG] missing/unsigned Streamline runtime; keeping native presentation");return false;}
    module=LoadLibraryExW(libraryPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)return false;
    auto init=Load<PFun_slInit>("slInit");shutdownSL=Load<PFun_slShutdown>("slShutdown");supportedSL=Load<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    featureFunction=Load<PFun_slGetFeatureFunction>("slGetFeatureFunction");newToken=Load<PFun_slGetNewFrameToken>("slGetNewFrameToken");setConstants=Load<PFun_slSetConstants>("slSetConstants");setTags=Load<PFun_slSetTagForFrame>("slSetTagForFrame");
    proxyInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr"));proxyDevice=reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module,"vkGetDeviceProcAddr"));
    if(!init||!shutdownSL||!supportedSL||!featureFunction||!newToken||!setConstants||!setTags||!proxyInstance||!proxyDevice){LOGW("[DLSS FG] missing Streamline exports");FreeLibrary(module);module=nullptr;return false;}
    const sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};const wchar_t* paths[]={pluginPath.c_str()};
    logPath=pluginPath+L"\\log";sl::Preferences preferences{};preferences.featuresToLoad=features;preferences.numFeaturesToLoad=3;
    preferences.pathsToPlugins=paths;preferences.numPathsToPlugins=1;preferences.pathToLogsAndData=logPath.c_str();preferences.logMessageCallback=Log;
    preferences.renderAPI=sl::RenderAPI::eVulkan;preferences.engine=sl::EngineType::eCustom;preferences.engineVersion="MikanEngine-2026.10";
    preferences.projectId="8b0362d7-316e-4609-b91f-658fd865d909";
    preferences.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    if(!Check(init(preferences,sl::kSDKVersion),"initialize")){FreeLibrary(module);module=nullptr;return false;}
    initialized=true;
    const int bytes=WideCharToMultiByte(CP_UTF8,0,libraryPath.c_str(),-1,nullptr,0,nullptr,nullptr);libraryUtf8.resize(bytes);WideCharToMultiByte(CP_UTF8,0,libraryPath.c_str(),-1,libraryUtf8.data(),bytes,nullptr,nullptr);
    LOGI("[DLSS FG] Streamline initialized; runtime 2x toggle ready, requested=%s",requested?"on":"off");return true;
}
const char* VulkanLibraryPath(){return initialized?libraryUtf8.c_str():nullptr;}
void DeviceReady(VkPhysicalDevice physical){
    if(!initialized)return;VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(physical,&p);
    sl::AdapterInfo adapter{};adapter.vkPhysicalDevice=physical;
    supported=p.vendorID==0x10de&&Check(supportedSL(sl::kFeatureDLSS_G,adapter),"device capability");
    if(!supported){LOGW("[DLSS FG] unavailable on selected device/driver/OS/HAGS; normal frames retained");return;}
    setOptions=Feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G,"slDLSSGSetOptions");getState=Feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G,"slDLSSGGetState");
    reflexOptions=Feature<PFun_slReflexSetOptions>(sl::kFeatureReflex,"slReflexSetOptions");reflexSleep=Feature<PFun_slReflexSleep>(sl::kFeatureReflex,"slReflexSleep");pclMarker=Feature<PFun_slPCLSetMarker>(sl::kFeaturePCL,"slPCLSetMarker");
    supported=setOptions&&getState&&reflexOptions&&reflexSleep&&pclMarker;if(!supported)return;
    sl::ReflexOptions r{};r.mode=sl::ReflexMode::eLowLatency;Check(reflexOptions(r),"Reflex options");
    LOGI("[DLSS FG] device capability passed; Reflex ready");
}
void BeginFrame(uint32_t serial,bool gameFrame){
    if(!supported)return;guidesReady=hudlessReady=false;
    if(!Check(newToken(token,&serial),"frame token")){token=nullptr;return;}
    const int owner=g_RunMode==RunMode::Editor?0:1; if(owner!=previousOwner){inputFailure=false;previousValid=false;previousOwner=owner;} editorRectValid=false;
    const bool want=requested&&(gameFrame||(g_RunMode==RunMode::Editor&&g_ShowSceneView))&&g_SwapChain.UsesHardwareRayTracing()&&!g_IsPaused&&!g_VSyncEnabled&&!inputFailure;
    if(requested&&gameFrame&&g_VSyncEnabled){static bool warned=false;if(!warned){LOGW("[DLSS FG] Vulkan FG requires VSync disabled; retaining real frames");warned=true;}}
    if(want!=enabled){sl::DLSSGOptions o{};o.mode=want?sl::DLSSGMode::eOn:sl::DLSSGMode::eOff;o.numFramesToGenerate=1;
        if(Check(setOptions(sl::ViewportHandle(0),o),"set 2x options")){enabled=want;previousValid=false;g_SwapChainRebuild=true;LOGI("[DLSS FG] %s; swapchain recreation requested",enabled?"2x enabled":"disabled");}}
    if(enabled)Check(reflexSleep(*token),"Reflex sleep");Marker(0);
}
void SetEditorViewportRect(uint32_t x,uint32_t y,uint32_t w,uint32_t h){
    if(editorRect.left!=x||editorRect.top!=y||editorRect.width!=w||editorRect.height!=h){previousValid=false;inputFailure=false;}
    editorRect={x,y,w,h};editorRectValid=w>0&&h>0;
}
void Marker(uint32_t marker){if(supported&&token&&pclMarker)Check(pclMarker(static_cast<sl::PCLMarker>(marker),*token),"latency marker");}
void SubmitGuides(VkCommandBuffer cmd,VkImageView guide,uint32_t w,uint32_t h,const glm::mat4& view,const glm::mat4& projection,const glm::vec2& jitter,bool reset){
    if(!enabled||!token||!guide)return;
    if(!PreparePipeline()){LOGW("[DLSS FG] guide pipeline unavailable");return;}
    const auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if(guideWidth!=w||guideHeight!=h){if(!Image(depth,w,h,VK_FORMAT_R32_SFLOAT,usage)||!Image(motion,w,h,VK_FORMAT_R16G16_SFLOAT,usage))return;guideWidth=w;guideHeight=h;guideInitialized=false;previousValid=false;}
    Transition(cmd,depth.GetImage(),guideInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    Transition(cmd,motion.GetImage(),guideInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    VkDescriptorImageInfo infos[]={{sampler,guide,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,depth.GetView(),VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,motion.GetView(),VK_IMAGE_LAYOUT_GENERAL}};VkWriteDescriptorSet writes[3]{};
    for(uint32_t i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&infos[i];}vkUpdateDescriptorSets(g_Device,3,writes,0,nullptr);
    struct Push{glm::vec4 projection,extent;};Push push{{projection[2][2],projection[3][2],projection[2][3],projection[3][3]},{w,h,0,0}};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptor,0,nullptr);vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);
    Transition(cmd,depth.GetImage(),VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    Transition(cmd,motion.GetImage(),VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);guideInitialized=true;
    sl::Constants c{};const auto vp=projection*view;const auto camera=glm::inverse(view);const auto toPrevious=previousVP*glm::inverse(vp);const auto invProjection=glm::inverse(projection);const auto invPrevious=glm::inverse(toPrevious);const glm::mat4 identity(1);
    // GLM column-vector matrices have the same bytes as SL row-vector matrices.
    std::memcpy(&c.cameraViewToClip,glm::value_ptr(projection),64);std::memcpy(&c.clipToCameraView,glm::value_ptr(invProjection),64);std::memcpy(&c.clipToPrevClip,glm::value_ptr(toPrevious),64);std::memcpy(&c.prevClipToClip,glm::value_ptr(invPrevious),64);std::memcpy(&c.clipToLensClip,glm::value_ptr(identity),64);
    c.jitterOffset={-jitter.x*w,-jitter.y*h};c.mvecScale={1,1};c.cameraPinholeOffset={0,0};
    c.cameraPos={camera[3].x,camera[3].y,camera[3].z};c.cameraUp={camera[1].x,camera[1].y,camera[1].z};c.cameraRight={camera[0].x,camera[0].y,camera[0].z};c.cameraFwd={-camera[2].x,-camera[2].y,-camera[2].z};
    c.cameraNear=projection[3][2]/projection[2][2];c.cameraFar=projection[3][2]/(projection[2][2]+1.0f);c.cameraFOV=2*std::atan(1/std::abs(projection[1][1]));c.cameraAspectRatio=std::abs(projection[1][1]/projection[0][0]);
    c.depthInverted=sl::Boolean::eFalse;c.cameraMotionIncluded=sl::Boolean::eTrue;c.motionVectors3D=sl::Boolean::eFalse;c.reset=reset||!previousValid?sl::Boolean::eTrue:sl::Boolean::eFalse;
    static unsigned diagnosticFrames=0;if(diagnosticFrames++<4)LOGD("[DLSS FG] guides: %ux%u reset=%u near=%g far=%g jitter=(%g,%g)",w,h,unsigned(c.reset),c.cameraNear,c.cameraFar,c.jitterOffset.x,c.jitterOffset.y);
    if(!Check(setConstants(c,*token,sl::ViewportHandle(0)),"camera constants"))return;
    auto d=Resource(depth,w,h,VK_FORMAT_R32_SFLOAT,usage);auto m=Resource(motion,w,h,VK_FORMAT_R16G16_SFLOAT,usage);sl::Extent extent{0,0,w,h};
    sl::ResourceTag tags[]={{&d,sl::kBufferTypeDepth,sl::ResourceLifecycle::eOnlyValidNow,&extent},{&m,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eOnlyValidNow,&extent}};
    guidesReady=Check(setTags(*token,sl::ViewportHandle(0),tags,2,reinterpret_cast<sl::CommandBuffer*>(cmd)),"depth/motion tags");previousVP=vp;previousValid=guidesReady;
}
void CaptureHudless(VkCommandBuffer cmd,VkImage backbuffer,VkFormat format,uint32_t w,uint32_t h){
    if(!enabled||!token||!guidesReady)return; if(g_RunMode==RunMode::Editor&&(!editorRectValid||editorRect.left+editorRect.width>w||editorRect.top+editorRect.height>h))return;
    const auto usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    if(colorWidth!=w||colorHeight!=h||colorFormat!=format){if(!Image(hudless,w,h,format,usage))return;colorWidth=w;colorHeight=h;colorFormat=format;colorInitialized=false;}
    Transition(cmd,backbuffer,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    Transition(cmd,hudless.GetImage(),colorInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={w,h,1};vkCmdCopyImage(cmd,backbuffer,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudless.GetImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    Transition(cmd,backbuffer,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    Transition(cmd,hudless.GetImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);colorInitialized=true;
    auto resource=Resource(hudless,w,h,format,usage);sl::Extent extent{0,0,w,h};sl::Extent presentExtent=g_RunMode==RunMode::Editor?editorRect:extent;sl::ResourceTag tag{&resource,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eOnlyValidNow,&presentExtent};hudlessReady=Check(setTags(*token,sl::ViewportHandle(0),&tag,1,reinterpret_cast<sl::CommandBuffer*>(cmd)),"HUD-less tag");sl::ResourceTag backbufferExtent{nullptr,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&presentExtent};Check(setTags(*token,sl::ViewportHandle(0),&backbufferExtent,1,reinterpret_cast<sl::CommandBuffer*>(cmd)),"backbuffer extent");
}
void Shutdown(){
    if(!initialized)return;if(trackedDevice)vkDeviceWaitIdle(trackedDevice);
    if(enabled&&setOptions){sl::DLSSGOptions o{};setOptions(sl::ViewportHandle(0),o);}shutdownSL();initialized=supported=enabled=false;
    depth.Cleanup();motion.Cleanup();hudless.Cleanup();if(sampler)vkDestroySampler(g_Device,sampler,g_Allocator);if(descriptorPool)vkDestroyDescriptorPool(g_Device,descriptorPool,g_Allocator);if(pipeline)vkDestroyPipeline(g_Device,pipeline,g_Allocator);if(pipelineLayout)vkDestroyPipelineLayout(g_Device,pipelineLayout,g_Allocator);if(descriptorLayout)vkDestroyDescriptorSetLayout(g_Device,descriptorLayout,g_Allocator);
    sampler={};descriptorPool={};pipeline={};pipelineLayout={};descriptorLayout={};token=nullptr;previousValid=false;
    // SDL may still hold Streamline's Vulkan dispatch addresses until SDL_Quit.
    // Keep the signed module loaded until process termination.
}
}
VkResult VKAPI_CALL MikanFG_CreateInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* a,VkInstance* out){auto f=initialized?reinterpret_cast<PFN_vkCreateInstance>(proxyInstance(nullptr,"vkCreateInstance")):vkCreateInstance;const auto r=f(info,a,out);if(r==VK_SUCCESS)trackedInstance=*out;return r;}
VkResult VKAPI_CALL MikanFG_CreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* a,VkDevice* out){
    if(initialized){
        // Streamline records the instance -> physical-device association during enumeration.
        // ImGui selects physical devices through the native loader; register that enumeration
        // before the device proxy initializes plugins, otherwise its instance table is null.
        auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proxyInstance(trackedInstance,"vkEnumeratePhysicalDevices"));
        uint32_t count=0;if(!enumerate || enumerate(trackedInstance,&count,nullptr)!=VK_SUCCESS)return VK_ERROR_INITIALIZATION_FAILED;
        std::vector<VkPhysicalDevice> adapters(count);
        if(enumerate(trackedInstance,&count,adapters.data())!=VK_SUCCESS)return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto f=initialized?reinterpret_cast<PFN_vkCreateDevice>(proxyInstance(trackedInstance,"vkCreateDevice")):vkCreateDevice;const auto r=f(p,info,a,out);if(r==VK_SUCCESS)trackedDevice=*out;return r;}
VkResult VKAPI_CALL MikanFG_CreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks* a,VkSwapchainKHR* out){auto f=initialized?reinterpret_cast<PFN_vkCreateSwapchainKHR>(proxyDevice(d,"vkCreateSwapchainKHR")):vkCreateSwapchainKHR;return f(d,info,a,out);}
void VKAPI_CALL MikanFG_DestroySwapchain(VkDevice d,VkSwapchainKHR s,const VkAllocationCallbacks* a){auto f=initialized?reinterpret_cast<PFN_vkDestroySwapchainKHR>(proxyDevice(d,"vkDestroySwapchainKHR")):vkDestroySwapchainKHR;f(d,s,a);}
VkResult VKAPI_CALL MikanFG_GetSwapchainImages(VkDevice d,VkSwapchainKHR s,uint32_t* n,VkImage* images){auto f=initialized?reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(proxyDevice(d,"vkGetSwapchainImagesKHR")):vkGetSwapchainImagesKHR;return f(d,s,n,images);}
VkResult VKAPI_CALL MikanFG_AcquireNextImage(VkDevice d,VkSwapchainKHR s,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t* index){auto f=initialized?reinterpret_cast<PFN_vkAcquireNextImageKHR>(proxyDevice(d,"vkAcquireNextImageKHR")):vkAcquireNextImageKHR;return f(d,s,timeout,sem,fence,index);}
VkResult VKAPI_CALL MikanFG_Present(VkQueue q,const VkPresentInfoKHR* info){
    // Incomplete inputs must never generate a frame from stale tags.
    if(enabled&&(!guidesReady||!hudlessReady)){sl::DLSSGOptions o{};setOptions(sl::ViewportHandle(0),o);enabled=false;inputFailure=true;previousValid=false;g_SwapChainRebuild=true;LOGW("[DLSS FG] incomplete frame inputs; disabled safely until viewport mode or extent changes");}
    auto f=initialized?reinterpret_cast<PFN_vkQueuePresentKHR>(proxyDevice(trackedDevice,"vkQueuePresentKHR")):vkQueuePresentKHR;const auto result=f(q,info);
    if(enabled&&getState){sl::DLSSGState state{};if(Check(getState(sl::ViewportHandle(0),state,nullptr),"presentation state")){static unsigned samples=0; static sl::DLSSGStatus previousStatus=sl::DLSSGStatus::eAll;if(samples++<4 || state.status!=previousStatus) LOGD("[DLSS FG] presentation state: frames=%u status=0x%x minDimension=%u maxGenerated=%u",state.numFramesActuallyPresented,unsigned(state.status),state.minWidthOrHeight,state.numFramesToGenerateMax);previousStatus=state.status;static bool reported=false;if(!reported&&state.status==sl::DLSSGStatus::eOk&&state.numFramesActuallyPresented>1){LOGI("[DLSS FG] generated-frame presentation verified: presented=%u status=0x%x",state.numFramesActuallyPresented,unsigned(state.status));reported=true;}}}
    return result;
}
VkResult VKAPI_CALL MikanFG_DeviceWaitIdle(VkDevice d){auto f=initialized?reinterpret_cast<PFN_vkDeviceWaitIdle>(proxyDevice(d,"vkDeviceWaitIdle")):vkDeviceWaitIdle;return f(d);}
void VKAPI_CALL MikanFG_DestroySurface(VkInstance i,VkSurfaceKHR s,const VkAllocationCallbacks* a){auto f=initialized?reinterpret_cast<PFN_vkDestroySurfaceKHR>(proxyInstance(i,"vkDestroySurfaceKHR")):vkDestroySurfaceKHR;f(i,s,a);}
#else
namespace Core::DlssFG {
bool Initialize(){return false;}const char* VulkanLibraryPath(){return nullptr;}void DeviceReady(VkPhysicalDevice){}
void SetRequested(bool){}bool IsRequested(){return false;}bool IsSupported(){return false;}bool IsEnabled(){return false;}bool IsInitialized(){return false;}
void SetEditorViewportRect(uint32_t,uint32_t,uint32_t,uint32_t){}void BeginFrame(uint32_t,bool){}void Marker(uint32_t){}void SubmitGuides(VkCommandBuffer,VkImageView,uint32_t,uint32_t,const glm::mat4&,const glm::mat4&,const glm::vec2&,bool){}
void CaptureHudless(VkCommandBuffer,VkImage,VkFormat,uint32_t,uint32_t){}void Shutdown(){}
}
#endif
