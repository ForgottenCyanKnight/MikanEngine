#pragma once
#include <algorithm>
#include <cmath>

namespace Animation {
// 120 Hz timeline ticks; low-rate renders batch ticks and high-rate renders wait.
// Animation evaluation needs only the newest pose, not every intermediate pose.
class MmdPlaybackClock {
public:
    float Advance(double elapsedSeconds) {
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0) return 0.0f;
        m_Remainder += elapsedSeconds;
        const double ticks = std::floor(m_Remainder * 120.0 + 1e-6);
        if (ticks < 1.0) return 0.0f;
        const double elapsed = ticks / 120.0;
        m_Remainder = std::max(0.0, m_Remainder - elapsed);
        return static_cast<float>(elapsed);
    }
    void Reset() { m_Remainder = 0.0; }
private:
    double m_Remainder = 0.0;
};
}
