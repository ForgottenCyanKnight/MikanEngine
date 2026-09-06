#include "Core/RenderDocCapture.h"

#include <cstdint>
#include <cstdio>

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
        std::fprintf(stderr,
                     "[RenderDoc] requested frame %d, but renderdoc.dll is not injected\n",
                     m_CaptureFrame);
        return;
    }

    auto getApi = reinterpret_cast<GetApiFn>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (getApi == nullptr) {
        m_Status = "api_missing";
        std::fprintf(stderr, "[RenderDoc] renderdoc.dll is loaded but RENDERDOC_GetAPI is missing\n");
        return;
    }

    void* apiStorage = nullptr;
    if (getApi(kRenderDocApiVersion100, &apiStorage) == 0 || apiStorage == nullptr) {
        m_Status = "api_unsupported";
        std::fprintf(stderr, "[RenderDoc] API 1.0.0 is not available in the injected RenderDoc\n");
        return;
    }

    auto* api = static_cast<RenderDocApi100*>(apiStorage);
    m_Api = api;
    m_Available = api->TriggerCapture != nullptr;
    if (!m_Available) {
        m_Status = "trigger_missing";
        std::fprintf(stderr, "[RenderDoc] TriggerCapture is unavailable\n");
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
    std::fprintf(stderr, "[RenderDoc] ready: API %d.%d.%d, capture frame %d\n",
                 major, minor, patch, m_CaptureFrame);
#else
    m_Status = "unsupported_platform";
    std::fprintf(stderr, "[RenderDoc] desktop capture integration is unavailable on this platform\n");
#endif
}

void RenderDocCapture::BeforeFramePresent(int oneBasedFrame) {
    if (!IsEnabled() || m_Triggered || oneBasedFrame != m_CaptureFrame) {
        return;
    }

#if defined(_WIN32)
    if (!m_Available || m_Api == nullptr) {
        m_Status = "not_available";
        std::fprintf(stderr, "[RenderDoc] cannot trigger frame %d: status=%s\n",
                     oneBasedFrame, m_Status.c_str());
        return;
    }

    auto* api = static_cast<RenderDocApi100*>(m_Api);
    api->TriggerCapture();
    m_Triggered = true;
    m_Status = "triggered";
    std::fprintf(stderr, "[RenderDoc] TriggerCapture requested before frame %d\n", oneBasedFrame);
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
    std::fprintf(stderr, "[RenderDoc] finalize: status=%s, triggered=%s\n",
                 m_Status.c_str(), m_Triggered ? "true" : "false");
}

} // namespace Core
