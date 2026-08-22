#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" __declspec(dllimport) int MikanGameplayTestMain(int argc, char* argv[]);

namespace {
char g_crashPath[4096] = "gameplay_crash_log.txt";

void ConfigureCrashPath(int argc, char* argv[]) {
    std::string path = g_crashPath;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument.rfind("--crash-log=", 0) == 0) path = argument.substr(12);
        else if (argument == "--crash-log" && index + 1 < argc) path = argv[++index] ? argv[index] : path;
    }
    std::error_code error;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    strncpy_s(g_crashPath, sizeof(g_crashPath), path.c_str(), _TRUNCATE);
}

LONG WINAPI GameplayCrashHandler(EXCEPTION_POINTERS* info) {
    FILE* file = nullptr;
    fopen_s(&file, g_crashPath, "w");
    if (file) {
        std::fprintf(file, "=== Mikan gameplay test crash ===\n");
        std::fprintf(file, "Exception code : 0x%08X\n", info->ExceptionRecord->ExceptionCode);
        std::fprintf(file, "Exception addr : %p\n", info->ExceptionRecord->ExceptionAddress);
        void* stack[40] = {};
        const USHORT count = CaptureStackBackTrace(0, 40, stack, nullptr);
        for (USHORT index = 0; index < count; ++index) std::fprintf(file, "  [%02u] %p\n", index, stack[index]);
        std::fclose(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
}

int main(int argc, char* argv[]) {
    ConfigureCrashPath(argc, argv);
    SetUnhandledExceptionFilter(GameplayCrashHandler);
    return MikanGameplayTestMain(argc, argv);
}
#else
extern "C" int MikanGameplayTestMain(int argc, char* argv[]);
int main(int argc, char* argv[]) { return MikanGameplayTestMain(argc, argv); }
#endif
