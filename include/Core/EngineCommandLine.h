#pragma once

#include <string>

namespace Core {

struct EngineCommandLineOptions {
    bool disableVoxelWorld = false;
    bool enableZPrepass = false;
    bool skipProjectManager = false;
    bool forceGameMode = false;
    bool headless = false;
    bool headlessEditor = false;
    bool headlessNoRender = false;
    bool prefabSelftest = false;
    bool physics2dSelftest = false;
    bool logDebug = false;
    bool cpuSkinning = false;
    bool invalidArguments = false;

    int headlessFrames = 0;
    float fixedDeltaSeconds = 0.0f;
    int renderDocCaptureFrame = 0;
    int screenshotFrame = 0;

    std::string renderDocCapturePath;
    std::string screenshotPath;
    std::string dumpStatePath;
    std::string dumpSchemaPath;
    std::string sceneArg;
    std::string gameArg;
};

// Parse process arguments without initializing SDL/Vulkan or touching runtime
// singletons. Validation messages are emitted here, while the caller decides
// the process exit code and applies engine-global switches.
bool ParseEngineCommandLine(int argc, char* argv[], EngineCommandLineOptions& options);

} // namespace Core
