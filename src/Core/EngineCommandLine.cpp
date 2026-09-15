#include "Core/EngineCommandLine.h"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace Core {
namespace {

bool ParseBoundedInt(const char* value, long minValue, long maxValue, int& out)
{
    char* end = nullptr;
    const long parsed = std::strtol(value ? value : "", &end, 10);
    if (!end || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool ParseBoundedFloat(const char* value, float minExclusive, float maxInclusive, float& out)
{
    char* end = nullptr;
    const float parsed = std::strtof(value ? value : "", &end);
    if (!end || *end != '\0' || parsed <= minExclusive || parsed > maxInclusive) {
        return false;
    }
    out = parsed;
    return true;
}

} // namespace

bool ParseEngineCommandLine(int argc, char* argv[], EngineCommandLineOptions& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--no-voxel-world") {
            options.disableVoxelWorld = true;
        }
        if (a == "--zprepass") {
            options.enableZPrepass = true;
        }
        if (a == "--no-project-manager") {
            options.skipProjectManager = true;
        }
        if (a == "--no-editor") {
            options.forceGameMode = true;
        }
        if (a == "--headless") {
            options.headless = true;
        }
        if (a == "--headless-editor") {
            options.headless = true;
            options.headlessEditor = true;
        }
        if (a == "--headless-no-render") {
            options.headless = true;
            options.headlessNoRender = true;
        }
        if (a == "--log-debug") {
            options.logDebug = true;
        }
        if (a == "--frames" && i + 1 < argc) {
            const char* value = argv[i + 1] ? argv[i + 1] : "";
            if (!ParseBoundedInt(value, 1, 1000000L, options.headlessFrames)) {
                std::fprintf(stderr,
                    "[Headless] ERROR: --frames must be an integer in [1, 1000000], got '%s'\n",
                    value);
                options.invalidArguments = true;
            }
            ++i;
        }
        if (a == "--fixed-dt" && i + 1 < argc) {
            const char* value = argv[i + 1] ? argv[i + 1] : "";
            if (!ParseBoundedFloat(value, 0.0f, 0.1f, options.fixedDeltaSeconds)) {
                std::fprintf(stderr,
                    "[Headless] ERROR: --fixed-dt must be in (0, 0.1], got '%s'\n",
                    value);
                options.invalidArguments = true;
            }
            ++i;
        }
        if (a.rfind("--screenshot-frame=", 0) == 0) {
            const char* value = a.c_str() + std::string("--screenshot-frame=").size();
            if (!ParseBoundedInt(value, 1, 1000000L, options.screenshotFrame)) {
                std::fprintf(stderr,
                    "[Screenshot] ERROR: --screenshot-frame must be an integer in [1, 1000000], got '%s'\n",
                    value);
                options.invalidArguments = true;
            }
        } else if (a == "--screenshot-frame") {
            if (i + 1 >= argc || !argv[i + 1] ||
                std::string(argv[i + 1]).rfind("--", 0) == 0) {
                std::fprintf(stderr,
                    "[Screenshot] ERROR: --screenshot-frame requires a value in [1, 1000000]\n");
                options.invalidArguments = true;
            } else {
                const char* value = argv[i + 1];
                if (!ParseBoundedInt(value, 1, 1000000L, options.screenshotFrame)) {
                    std::fprintf(stderr,
                        "[Screenshot] ERROR: --screenshot-frame must be an integer in [1, 1000000], got '%s'\n",
                        value);
                    options.invalidArguments = true;
                }
                ++i;
            }
        }
        if (a.rfind("--screenshot-path=", 0) == 0) {
            options.screenshotPath = a.substr(std::string("--screenshot-path=").size());
            if (options.screenshotPath.empty()) {
                std::fprintf(stderr,
                    "[Screenshot] ERROR: --screenshot-path requires a non-empty path\n");
                options.invalidArguments = true;
            }
        } else if (a == "--screenshot-path") {
            if (i + 1 >= argc || !argv[i + 1] ||
                std::string(argv[i + 1]).rfind("--", 0) == 0) {
                std::fprintf(stderr,
                    "[Screenshot] ERROR: --screenshot-path requires a non-empty path\n");
                options.invalidArguments = true;
            } else {
                options.screenshotPath = argv[++i];
            }
        }
        if (a.rfind("--renderdoc-capture-frame=", 0) == 0) {
            const char* value = a.c_str() + std::string("--renderdoc-capture-frame=").size();
            if (!ParseBoundedInt(value, 1, 1000000L, options.renderDocCaptureFrame)) {
                std::fprintf(stderr,
                    "[RenderDoc] ERROR: --renderdoc-capture-frame must be an integer in [1, 1000000], got '%s'\n",
                    value);
                options.invalidArguments = true;
            }
        } else if (a == "--renderdoc-capture-frame") {
            if (i + 1 >= argc || !argv[i + 1] ||
                std::string(argv[i + 1]).rfind("--", 0) == 0) {
                std::fprintf(stderr,
                    "[RenderDoc] ERROR: --renderdoc-capture-frame requires a value in [1, 1000000]\n");
                options.invalidArguments = true;
            } else {
                const char* value = argv[i + 1];
                if (!ParseBoundedInt(value, 1, 1000000L, options.renderDocCaptureFrame)) {
                    std::fprintf(stderr,
                        "[RenderDoc] ERROR: --renderdoc-capture-frame must be an integer in [1, 1000000], got '%s'\n",
                        value);
                    options.invalidArguments = true;
                }
                ++i;
            }
        }
        if (a.rfind("--renderdoc-capture-path=", 0) == 0) {
            options.renderDocCapturePath = a.substr(
                std::string("--renderdoc-capture-path=").size());
        } else if (a == "--renderdoc-capture-path") {
            if (i + 1 >= argc || !argv[i + 1] ||
                std::string(argv[i + 1]).rfind("--", 0) == 0) {
                std::fprintf(stderr,
                    "[RenderDoc] ERROR: --renderdoc-capture-path requires a non-empty path\n");
                options.invalidArguments = true;
            } else {
                options.renderDocCapturePath = argv[++i];
            }
        }
        if (a.rfind("--dump-state=", 0) == 0) {
            options.dumpStatePath = a.substr(13);
        } else if (a == "--dump-state" && i + 1 < argc) {
            options.dumpStatePath = argv[i + 1] ? argv[i + 1] : "";
            ++i;
        }
        if (a.rfind("--dump-schema=", 0) == 0) {
            options.dumpSchemaPath = a.substr(14);
        } else if (a == "--dump-schema" && i + 1 < argc) {
            options.dumpSchemaPath = argv[i + 1] ? argv[i + 1] : "";
            ++i;
        }
        if (a == "--prefab-selftest") {
            options.prefabSelftest = true;
        }
        if (a == "--phys2d-selftest") {
            options.physics2dSelftest = true;
        }
        if (a == "--cpu-skinning") {
            options.cpuSkinning = true;
        }
        if (a.rfind("--scene=", 0) == 0) {
            options.sceneArg = a.substr(8);
        } else if (a == "--scene" && i + 1 < argc) {
            options.sceneArg = argv[i + 1] ? argv[i + 1] : "";
            ++i;
        }
        if (a.rfind("--game=", 0) == 0) {
            options.gameArg = a.substr(7);
        } else if (a == "--game" && i + 1 < argc) {
            options.gameArg = argv[i + 1] ? argv[i + 1] : "";
            ++i;
        }
    }
    return true;
}

} // namespace Core
