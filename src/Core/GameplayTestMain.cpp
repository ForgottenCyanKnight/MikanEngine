#include "Core/GameplayRuntime.h"
#include "Core/ProjectManager.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" MIKAN_API int MikanGameplayTestMain(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::locale::global(std::locale(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>));
#endif

    std::string scenePath;
    std::string gameName;
    std::string dumpPath;
    int frames = 120;
    float fixedDelta = 1.0f / 60.0f;
    bool invalidArguments = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        auto nextValue = [&](const char* option) -> const char* {
            if (index + 1 >= argc || !argv[index + 1]) {
                std::fprintf(stderr, "[GameplayTest] ERROR: %s requires a value\n", option);
                invalidArguments = true;
                return "";
            }
            return argv[++index];
        };
        if (argument.rfind("--scene=", 0) == 0) scenePath = argument.substr(8);
        else if (argument == "--scene") scenePath = nextValue("--scene");
        else if (argument.rfind("--game=", 0) == 0) gameName = argument.substr(7);
        else if (argument == "--game") gameName = nextValue("--game");
        else if (argument.rfind("--dump-state=", 0) == 0) dumpPath = argument.substr(13);
        else if (argument == "--dump-state") dumpPath = nextValue("--dump-state");
        else if (argument == "--frames") {
            const char* value = nextValue("--frames");
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 1000000L) invalidArguments = true;
            else frames = static_cast<int>(parsed);
        } else if (argument == "--fixed-dt") {
            const char* value = nextValue("--fixed-dt");
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (!end || *end != '\0' || parsed <= 0.0f || parsed > 0.1f) invalidArguments = true;
            else fixedDelta = parsed;
        }
    }

    if (invalidArguments || scenePath.empty() || dumpPath.empty()) {
        std::fprintf(stderr,
            "[GameplayTest] ERROR: required: --scene <path> --dump-state <path>; "
            "--frames must be 1..1000000 and --fixed-dt must be (0,0.1]\n");
        return 64;
    }

    ProjectManager::GetInstance().Initialize(argc, argv);
    scenePath = ProjectManager::GetInstance().ResolveAssetPath(scenePath);
    std::error_code error;
    const std::filesystem::path dumpParent = std::filesystem::path(dumpPath).parent_path();
    if (!dumpParent.empty()) std::filesystem::create_directories(dumpParent, error);

    Core::GameplayRuntime runtime;
    if (!runtime.Initialize()) return 1;
    if (!runtime.LoadScene(scenePath, gameName)) return 2;

    std::fprintf(stderr, "[GameplayTest] running %d frames at fixed_dt=%.8f (no SDL Video, no Vulkan)\n",
        frames, fixedDelta);
    for (int frame = 0; frame < frames; ++frame) runtime.Tick(fixedDelta);

    if (!Core::GameplayRuntime::DumpState(dumpPath, frames, "gameplay-cpu", 1.0f / fixedDelta)) return 3;
    runtime.Shutdown();
    std::fprintf(stderr, "[GameplayTest] PASS\n");
    return 0;
}
