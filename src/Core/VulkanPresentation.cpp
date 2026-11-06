#include "Core/CpuStageTrace.h"
#include "Core/DlssFrameGeneration.h"
#include "Core/VulkanManager.h"

#include "Core/RenderGlobals.h"
#include "Core/ScreenshotCapture.h"
#include "Core/PipelineCapture.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanPresentation.h"
#include "Core/EngineGlobal.h"
#include "Core/Log.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <algorithm>
#include <SDL3/SDL.h>
extern SDL_Window* window;

namespace Core {namespace {
struct PresentJob {VkSwapchainKHR swapchain{};VkSemaphore complete{};uint32_t image{};};
struct PresentWorker {
    VkQueue queue{};std::mutex mutex;std::condition_variable changed;std::thread thread;
    PresentJob job;VkResult result=VK_SUCCESS;bool busy=false,stop=false;
    VkFence acquireFence{};VkSwapchainKHR prefetchedSwapchain{},checkedSwapchain{};
    VkSemaphore prefetchedAvailable{};uint32_t prefetchedImage{};VkResult prefetchedResult=VK_SUCCESS;
    bool prefetched=false,canPrefetch=false;
    void Run(){
        for(;;){PresentJob next;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return busy||stop;});if(!busy&&stop)return;next=job;}
            VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};info.waitSemaphoreCount=1;info.pWaitSemaphores=&next.complete;
            info.swapchainCount=1;info.pSwapchains=&next.swapchain;info.pImageIndices=&next.image;
            VkResult finished;
            {CpuStageTrace trace("swapchain.vk_present");finished=vkQueuePresentKHR(queue,&info);}
            {std::lock_guard lock(mutex);result=finished;busy=false;}changed.notify_all();
        }
    }
    ~PresentWorker(){if(thread.joinable()){{std::lock_guard lock(mutex);stop=true;}changed.notify_all();thread.join();}}
};
PresentWorker& Worker(){static PresentWorker worker;return worker;}
void ReportPresentationPath(const char* path){
    static const char* previous=nullptr;
    if(previous!=path){LOGI("[Presentation] active path: %s",path);previous=path;}
}
}
VkResult DrainPresentationQueue(){
    auto& worker=Worker();if(!worker.thread.joinable())return VK_SUCCESS;
    CpuStageTrace trace("swapchain.present_backpressure");
    std::unique_lock lock(worker.mutex);worker.changed.wait(lock,[&]{return !worker.busy;});
    const auto result=worker.result;worker.result=VK_SUCCESS;return result;
}
void ResetPresentationImage(){
    auto& worker=Worker();const auto pending=DrainPresentationQueue();
    if(pending==VK_ERROR_DEVICE_LOST)check_vk_result(pending);
    // An acquired-but-unsubmitted image can be retired with its swapchain.
    // Its acquisition must finish before the semaphore/fence can be destroyed.
    if(worker.acquireFence)check_vk_result(vkWaitForFences(g_Device,1,&worker.acquireFence,VK_TRUE,UINT64_MAX));
    worker.prefetched=false;worker.prefetchedSwapchain=VK_NULL_HANDLE;worker.checkedSwapchain=VK_NULL_HANDLE;
}
void ShutdownPresentationQueue(){
    auto& worker=Worker();if(!worker.thread.joinable())return;
    const auto result=DrainPresentationQueue();
    ResetPresentationImage();
    {std::lock_guard lock(worker.mutex);worker.stop=true;}worker.changed.notify_all();worker.thread.join();worker.queue=VK_NULL_HANDLE;
    if(worker.acquireFence){vkDestroyFence(g_Device,worker.acquireFence,g_Allocator);worker.acquireFence=VK_NULL_HANDLE;}
    if(result==VK_ERROR_DEVICE_LOST)check_vk_result(result);
}
void ConfigurePresentationQueue(VkQueue queue){
    ShutdownPresentationQueue();auto& worker=Worker();worker.queue=queue;worker.stop=false;
    if(queue&&queue!=g_Queue&&!DlssFG::IsInitialized()){
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        if(vkCreateFence(g_Device,&fence,g_Allocator,&worker.acquireFence)!=VK_SUCCESS){worker.queue=VK_NULL_HANDLE;return;}
        try{worker.thread=std::thread([&worker]{worker.Run();});}
        catch(const std::exception& error){
            LOGW("[Presentation] worker creation failed: %s; using synchronous presentation",error.what());
            vkDestroyFence(g_Device,worker.acquireFence,g_Allocator);worker.acquireFence=VK_NULL_HANDLE;worker.queue=VK_NULL_HANDLE;return;
        }
        LOGI("[Presentation] async worker ready; separate queue, at most one pending present; native main window");
    }
}
VkResult WaitForDeviceIdle(VkDevice device){
    const auto pending=DrainPresentationQueue();
    if(pending==VK_ERROR_OUT_OF_DATE_KHR)g_SwapChainRebuild=true;
    if(pending==VK_ERROR_DEVICE_LOST)check_vk_result(pending);
    return vkDeviceWaitIdle(device);
}
VkResult AcquirePresentationImage(VkSwapchainKHR swapchain,VkSemaphore available,uint32_t* image){
    auto& worker=Worker();
    if(worker.prefetched&&worker.prefetchedSwapchain==swapchain&&worker.prefetchedAvailable==available){
        *image=worker.prefetchedImage;worker.prefetched=false;return worker.prefetchedResult;
    }
    // A missing prefetch must never race vkQueuePresentKHR on this swapchain.
    const auto pending=DrainPresentationQueue();if(pending!=VK_SUCCESS&&pending!=VK_SUBOPTIMAL_KHR)return pending;
    return vkAcquireNextImageKHR(g_Device,swapchain,UINT64_MAX,available,VK_NULL_HANDLE,image);
}
VkResult PresentSwapchain(ImGui_ImplVulkanH_Window* window,const VkPresentInfoKHR& info){
    auto& worker=Worker();
    const auto pending=DrainPresentationQueue();
    if(pending!=VK_SUCCESS&&pending!=VK_SUBOPTIMAL_KHR)return pending;
    if(worker.thread.joinable()&&!DlssFG::IsInitialized()&&!g_VSyncEnabled&&
       (window->PresentMode==VK_PRESENT_MODE_IMMEDIATE_KHR||window->PresentMode==VK_PRESENT_MODE_MAILBOX_KHR)&&
       info.swapchainCount==1&&info.waitSemaphoreCount==1&&!info.pNext&&!info.pResults){
        if(worker.checkedSwapchain!=window->Swapchain){
            VkSurfaceCapabilitiesKHR cap{};const auto result=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice,window->Surface,&cap);
            worker.canPrefetch=result==VK_SUCCESS&&window->ImageCount>cap.minImageCount;
            worker.checkedSwapchain=window->Swapchain;
            LOGI("[Presentation] async prefetch=%u images=%u surfaceMinimum=%u",uint32_t(worker.canPrefetch),window->ImageCount,cap.minImageCount);
        }
        if(worker.canPrefetch&&worker.acquireFence){
            // Host access to a swapchain is externally synchronized: acquire
            // the next frame BEFORE starting its predecessor's present call.
            // The render thread then consumes this reservation without WSI calls.
            CpuStageTrace trace("swapchain.acquire_prefetch");
            auto result=vkWaitForFences(g_Device,1,&worker.acquireFence,VK_TRUE,UINT64_MAX);if(result!=VK_SUCCESS)return result;
            result=vkResetFences(g_Device,1,&worker.acquireFence);if(result!=VK_SUCCESS)return result;
            worker.prefetchedAvailable=window->FrameSemaphores[(window->SemaphoreIndex+1)%window->SemaphoreCount].ImageAcquiredSemaphore;
            result=vkAcquireNextImageKHR(g_Device,window->Swapchain,UINT64_MAX,worker.prefetchedAvailable,worker.acquireFence,&worker.prefetchedImage);
            if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR){
                // A failed acquire does not signal its fence. Replace it with
                // an already signaled fence so teardown cannot wait forever.
                vkDestroyFence(g_Device,worker.acquireFence,g_Allocator);worker.acquireFence=VK_NULL_HANDLE;
                VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
                vkCreateFence(g_Device,&fence,g_Allocator,&worker.acquireFence);return result;
            }
            worker.prefetchedResult=result;worker.prefetchedSwapchain=window->Swapchain;worker.prefetched=true;
            ReportPresentationPath(g_RunMode==RunMode::Editor?"async native editor":"async native game");
            {std::lock_guard lock(worker.mutex);worker.job={*info.pSwapchains,*info.pWaitSemaphores,*info.pImageIndices};worker.busy=true;}
            worker.changed.notify_one();return VK_SUCCESS;
        }
    }
    if(DlssFG::IsInitialized())ReportPresentationPath("sync: Streamline startup mode");
    else if(!worker.thread.joinable())ReportPresentationPath("sync: async disabled or second queue unavailable");
    else if(g_VSyncEnabled||window->PresentMode==VK_PRESENT_MODE_FIFO_KHR||window->PresentMode==VK_PRESENT_MODE_FIFO_RELAXED_KHR)ReportPresentationPath("sync: VSync/FIFO");
    else ReportPresentationPath("sync: insufficient images or extended present request");
    CpuStageTrace trace("swapchain.vk_present");return vkQueuePresentKHR(g_Queue,&info);
}
}

// 呈现一帧
void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (IsVulkanDeviceLost()) return;
    Core::CpuStageTrace cpuStageTrace("swapchain.present");
    // 暂停状态下不执行呈现
    if (g_IsPaused)
        return;

    // 交换链重建中不执行呈现
    if (g_SwapChainRebuild)
        return;

    // 获取渲染完成信号量
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->FrameIndex].RenderCompleteSemaphore;

    // 呈现信息
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;

    // The capture completion fence belongs to its staging slot, rather than
    // a swapchain frame fence that may be reset before the encoder consumes it.
    {Core::CpuStageTrace trace("swapchain.capture_notify");Core::ScreenshotCapture::GetInstance().NotifySubmitted();}
    // 呈现到屏幕
    {Core::CpuStageTrace trace("swapchain.present_begin_marker");Core::DlssFG::Marker(4);}
    VkResult err;
    {Core::CpuStageTrace trace("swapchain.present_dispatch");err = Core::PresentSwapchain(wd,info);}
    {Core::CpuStageTrace trace("swapchain.present_end_marker");Core::DlssFG::Marker(5);}
    // 一次性截图只在确有记录时等待队列，避免影响普通帧；失败/过期呈现时
    // 仍由 EngineMain 清理阶段再次 Finalize，保证不会遗留 staging 资源。
    {Core::CpuStageTrace trace("swapchain.screenshot_finalize");Core::ScreenshotCapture::GetInstance().Finalize();}
    {Core::CpuStageTrace trace("swapchain.pipeline_finalize");Core::PipelineCapture::GetInstance().Finalize();}
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        g_SwapChainRebuild = true;  // 全平台：OUT_OF_DATE 必须重建（Android 原被排除会导致死循环）
    // 注：桌面平台 VK_SUBOPTIMAL_KHR 不再单独触发 rebuild。
    //   resize 导致的 suboptimal 会由 SDL_EVENT_WINDOW_RESIZED 事件提前设置 g_SwapChainRebuild（EngineMain.cpp:L441），
    //   两者时序重合且连续多帧 suboptimal 叠加会触发重复重建抖动；仅保留 OUT_OF_DATE 硬错误 + RESIZED 事件触发，减少 resize 期间多次重建。
    //   HDR toggle 等非尺寸触发的 SUBOPTIMAL 仍可容忍使用（直到下一帧尺寸变化或下一次 OUT_OF_DATE）。
    #ifndef __ANDROID__
    // 旧: if (err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    #endif
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err == VK_ERROR_DEVICE_LOST) {
        // Device loss is terminal; recreating a swapchain cannot recover it.
        check_vk_result(err);
        g_SwapChainRebuild = false;
        return;
    }
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    // 更新信号量索引
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    static const uint64_t exitFrame=[](){const char* v=std::getenv("MIKAN_VK_PRESENT_EXIT_FRAME");return v?std::strtoull(v,nullptr,10):0ull;}();
    if(exitFrame){static uint64_t frames=0;if(++frames==exitFrame){
        LOGI("[Presentation test] requested quit after %llu presents",(unsigned long long)frames);
        SDL_Event event{};event.type=SDL_EVENT_QUIT;SDL_PushEvent(&event);
    }}
    // Opt-in deterministic integration exercise; never active in normal play
    // or performance sampling. Events use the same public settings as the UI.
    static const bool smoke=[](){const char* v=std::getenv("MIKAN_VK_PRESENT_SMOKE");return v&&v[0]=='1';}();
    if(smoke){
        static uint32_t frames=0;static const int width=wd->Width,height=wd->Height;++frames;
        switch(frames){
            case 60:LOGI("[Presentation smoke] vsync on");SetVSync(true);break;
            case 120:LOGI("[Presentation smoke] vsync off");SetVSync(false);break;
            case 180:LOGI("[Presentation smoke] double buffering");SetTripleBuffering(false);break;
            case 240:LOGI("[Presentation smoke] triple buffering");SetTripleBuffering(true);break;
            case 300:LOGI("[Presentation smoke] resize smaller");SDL_SetWindowSize(window,std::max(640,width-64),std::max(480,height-64));break;
            case 360:LOGI("[Presentation smoke] restore extent");SDL_SetWindowSize(window,width,height);break;
        }
    }
}

uint32_t GetCurrentFrameIndex()
{
    return g_MainWindowData.FrameIndex;
}
