#pragma once

#include "Platform/Export.h"

namespace Core {

// Runtime services that may be unavailable in an isolated gameplay test.
// The default is the full engine profile; MikanTestRunner switches to GameplayTest.
enum class RuntimeProfile {
    FullEngine,
    GameplayTest
};

struct MIKAN_API RuntimeCapabilities {
    RuntimeProfile profile = RuntimeProfile::FullEngine;
    bool rendering = true;
    bool audioDevice = true;
    bool inputDevices = true;
};

MIKAN_API const RuntimeCapabilities& GetRuntimeCapabilities();
MIKAN_API void SetRuntimeCapabilities(const RuntimeCapabilities& capabilities);
MIKAN_API void UseFullEngineCapabilities();
MIKAN_API void UseGameplayTestCapabilities();

} // namespace Core
