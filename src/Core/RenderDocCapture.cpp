#include "Core/Log.h"
#include "Core/RenderDocCapture.h"

#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Core {

#if defined(_WIN32)
namespace {

// Keep this ABI declaration limited to RenderDoc API 1.0.0.  The first part of
// the public API struct is backwards compatible, so this avoids making the
// engine depend on a RenderDoc SDK header or link library at build time.
constexpr int kRenderDocApiVersion100 = 10000;

using GetApiVersionFn = void(__cdecl*)(int*, int*, int*);
using SetCaptureOptionU32Fn = int(__cdecl*)(int, uint32_t);
using SetCaptureOptionF32Fn = int(__cdecl*)(int, float);
using GetCaptureOptionU32Fn = uint32_t(__cdecl*)(int);
using GetCaptureOptionF32Fn = float(__cdecl*)(int);
using SetFocusToggleKeysFn = void(__cdecl*)(int*, int);
using SetCaptureKeysFn = void(__cdecl*)(int*, int);
using GetOverlayBitsFn = uint32_t(__cdecl*)();
using MaskOverlayBitsFn = void(__cdecl*)(uint32_t, uint32_t);
using RemoveHooksFn = void(__cdecl*)();
using UnloadCrashHandlerFn = void(__cdecl*)();
using SetCaptureFilePathTemplateFn = void(__cdecl*)(const char*);
using GetCaptureFilePathTemplateFn = const char*(__cdecl*)();
using GetNumCapturesFn = uint32_t(__cdecl*)();
using GetCaptureFn = uint32_t(__cdecl*)(uint32_t, char*, uint32_t*, uint64_t*);
using TriggerCaptureFn = void(__cdecl*)();
using IsTargetControlConnectedFn = uint32_t(__cdecl*)();
using LaunchReplayUIFn = uint32_t(__cdecl*)(uint32_t, const char*);
using SetActiveWindowFn = void(__cdecl*)(void*, void*);
using StartFrameCaptureFn = void(__cdecl*)(void*, void*);
using IsFrameCapturingFn = uint32_t(__cdecl*)();
using EndFrameCaptureFn = uint32_t(__cdecl*)(void*, void*);

struct RenderDocApi100 {
    GetApiVersionFn GetAPIVersion;
    SetCaptureOptionU32Fn SetCaptureOptionU32;
    SetCaptureOptionF32Fn SetCaptureOptionF32;
    GetCaptureOptionU32Fn GetCaptureOptionU32;
    GetCaptureOptionF32Fn GetCaptureOptionF32;
    SetFocusToggleKeysFn SetFocusToggleKeys;
    SetCaptureKeysFn SetCaptureKeys;
    GetOverlayBitsFn GetOverlayBits;
    MaskOverlayBitsFn MaskOverlayBits;
    RemoveHooksFn RemoveHooks;
    UnloadCrashHandlerFn UnloadCrashHandler;
    SetCaptureFilePathTemplateFn SetCaptureFilePathTemplate;
    GetCaptureFilePathTemplateFn GetCaptureFilePathTemplate;
    GetNumCapturesFn GetNumCaptures;
    GetCaptureFn GetCapture;
    TriggerCaptureFn TriggerCapture;
    IsTargetControlConnectedFn IsTargetControlConnected;
    LaunchReplayUIFn LaunchReplayUI;
    SetActiveWindowFn SetActiveWindow;
    StartFrameCaptureFn StartFrameCapture;
    IsFrameCapturingFn IsFrameCapturing;
    EndFrameCaptureFn EndFrameCapture;
};

using GetApiFn = int(__cdecl*)(int, void**);

} // namespace
#endif

RenderDocCapture& RenderDocCapture::GetInstance() {
    static RenderDocCapture instance;
    return instance;
}

void RenderDocCapture::Configure(int captureFrame, const std::string& capturePathTemplate) {
    m_CaptureFrame = captureFrame;
    m_CapturePathTemplate = capturePathTemplate;
    m_Status = m_CaptureFrame > 0 ? "configured" : "disabled";
    m_Api = nullptr;
    m_Available = false;
    m_Triggered = false;
    m_Finalized = false;
}

void RenderDocCapture::Initialize() {
    if (!IsEnabled() || m_Finalized) {
        return;
    }

#if defined(_WIN32)
    // renderdoccmd/qrenderdoc injects renderdoc.dll into the target.  Do not
    // LoadLibrary it ourselves: loading the DLL without RenderDoc's capture
    // injection would make the result look configured but not actually hook
    // Vulkan calls.
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr) {
        m_Status = "not_injected";
        LOGI(
                     "[RenderDoc] requested frame %d, but renderdoc.dll is not injected",
                     m_CaptureFrame);
        return;
    }

    auto getApi = reinterpret_cast<GetApiFn>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (getApi == nullptr) {
        m_Status = "api_missing";
        LOGE("[RenderDoc] renderdoc.dll is loaded but RENDERDOC_GetAPI is missing");
        return;
    }

    void* apiStorage = nullptr;
    if (getApi(kRenderDocApiVersion100, &apiStorage) == 0 || apiStorage == nullptr) {
        m_Status = "api_unsupported";
        LOGW("[RenderDoc] API 1.0.0 is not available in the injected RenderDoc");
        return;
    }

    auto* api = static_cast<RenderDocApi100*>(apiStorage);
    m_Api = api;
    m_Available = api->TriggerCapture != nullptr;
    if (!m_Available) {
        m_Status = "trigger_missing";
        LOGW("[RenderDoc] TriggerCapture is unavailable");
        return;
    }

    if (api->SetCaptureFilePathTemplate != nullptr && !m_CapturePathTemplate.empty()) {
        api->SetCaptureFilePathTemplate(m_CapturePathTemplate.c_str());
    }

    int major = 0;
    int minor = 0;
    int patch = 0;
    if (api->GetAPIVersion != nullptr) {
        api->GetAPIVersion(&major, &minor, &patch);
    }
    m_Status = "ready";
    LOGI("[RenderDoc] ready: API %d.%d.%d, capture frame %d",
                 major, minor, patch, m_CaptureFrame);
#else
    m_Status = "unsupported_platform";
    LOGW("[RenderDoc] desktop capture integration is unavailable on this platform");
#endif
}

void RenderDocCapture::BeforeFramePresent(int oneBasedFrame) {
    if (!IsEnabled() || m_Triggered || oneBasedFrame != m_CaptureFrame) {
        return;
    }

#if defined(_WIN32)
    if (!m_Available || m_Api == nullptr) {
        m_Status = "not_available";
        LOGE("[RenderDoc] cannot trigger frame %d: status=%s",
                     oneBasedFrame, m_Status.c_str());
        return;
    }

    auto* api = static_cast<RenderDocApi100*>(m_Api);
    api->TriggerCapture();
    m_Triggered = true;
    m_Status = "triggered";
    LOGI("[RenderDoc] TriggerCapture requested before frame %d", oneBasedFrame);
#endif
}

void RenderDocCapture::Finalize() {
    if (!IsEnabled() || m_Finalized) {
        return;
    }
    m_Finalized = true;
    if (!m_Triggered && m_Status == "ready") {
        m_Status = "frame_not_reached";
    }
    LOGI("[RenderDoc] finalize: status=%s, triggered=%s",
                 m_Status.c_str(), m_Triggered ? "true" : "false");
}

} // namespace Core
