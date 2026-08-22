#include "Core/RuntimeCapabilities.h"

namespace Core {
namespace {
RuntimeCapabilities g_capabilities{};
}

const RuntimeCapabilities& GetRuntimeCapabilities() {
    return g_capabilities;
}

void SetRuntimeCapabilities(const RuntimeCapabilities& capabilities) {
    g_capabilities = capabilities;
}

void UseFullEngineCapabilities() {
    g_capabilities = RuntimeCapabilities{};
}

void UseGameplayTestCapabilities() {
    g_capabilities.profile = RuntimeProfile::GameplayTest;
    g_capabilities.rendering = false;
    g_capabilities.audioDevice = false;
    g_capabilities.inputDevices = false;
}

} // namespace Core
