#pragma once

#include <chrono>
#include <cstdlib>

namespace SceneRenderCpuProfile {

using Clock = std::chrono::steady_clock;

struct FrameTiming {
    double totalMs = 0.0;
    double prepareMs = 0.0;
    double geometryMs = 0.0;
    double rootsMs = 0.0;
    double modelCollectMs = 0.0;
    double voxCollectMs = 0.0;
    double cameraCollectMs = 0.0;
    double lightCollectMs = 0.0;
};

inline bool IsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_CPU_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

inline double Milliseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

inline FrameTiming& CurrentFrame()
{
    thread_local FrameTiming timing;
    return timing;
}

} // namespace SceneRenderCpuProfile
