#pragma once

#include <string>

namespace Core {

// Optional desktop RenderDoc integration.  The engine remains fully usable when
// RenderDoc is not injected; the capture request is simply reported as unavailable.
class RenderDocCapture {
public:
    static RenderDocCapture& GetInstance();

    // captureFrame is one-based and refers to the frame whose Present should be
    // captured.  A value <= 0 disables the integration for this process.
    void Configure(int captureFrame, const std::string& capturePathTemplate);
    void Initialize();
    void BeforeFramePresent(int oneBasedFrame);
    void Finalize();

    bool IsEnabled() const { return m_CaptureFrame > 0; }
    bool IsAvailable() const { return m_Available; }
    bool IsTriggered() const { return m_Triggered; }
    const std::string& GetStatus() const { return m_Status; }

private:
    RenderDocCapture() = default;

    int m_CaptureFrame = 0;
    std::string m_CapturePathTemplate;
    std::string m_Status = "disabled";
    void* m_Api = nullptr;
    bool m_Available = false;
    bool m_Triggered = false;
    bool m_Finalized = false;
};

} // namespace Core
