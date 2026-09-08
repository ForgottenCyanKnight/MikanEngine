#include <cstdio>
#include "Core/Utf8Path.h"
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

extern "C" __declspec(dllimport) int MikanGameplayTestMain(int argc, char* argv[]);

namespace {
char g_crashPath[4096] = "gameplay_crash_log.txt";

std::string WideToUtf8(const wchar_t* value) {
    if (!value) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                    value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        length = WideCharToMultiByte(CP_UTF8, 0,
                                     value, -1, nullptr, 0, nullptr, nullptr);
    }
    if (length <= 0) return {};

    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1,
                            result.data(), length, nullptr, nullptr) <= 0) {
        return {};
    }
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

void ConfigureCrashPath(int argc, char* argv[]) {
    std::string path = g_crashPath;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument.rfind("--crash-log=", 0) == 0) path = argument.substr(12);
        else if (argument == "--crash-log" && index + 1 < argc) path = argv[++index] ? argv[index] : path;
    }
    std::error_code error;
    const auto parent = Utf8Path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    strncpy_s(g_crashPath, sizeof(g_crashPath), path.c_str(), _TRUNCATE);
}

LONG WINAPI GameplayCrashHandler(EXCEPTION_POINTERS* info) {
    FILE* file = nullptr;
    const std::filesystem::path crashPath = Utf8Path(g_crashPath);
    _wfopen_s(&file, crashPath.c_str(), L"w");
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
#ifdef _WIN32
    // MSVC's narrow argv is decoded with the active ANSI code page.  The
    // engine APIs use UTF-8, so pass a UTF-8 copy reconstructed from argvW.
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv) {
        std::vector<std::string> utf8Storage;
        std::vector<char*> utf8Argv;
        utf8Storage.reserve(static_cast<size_t>(wideArgc));
        utf8Argv.reserve(static_cast<size_t>(wideArgc));
        for (int index = 0; index < wideArgc; ++index) {
            utf8Storage.push_back(WideToUtf8(wideArgv[index]));
            utf8Argv.push_back(utf8Storage.back().data());
        }
        const int result = [&]() {
            ConfigureCrashPath(wideArgc, utf8Argv.empty() ? nullptr : utf8Argv.data());
            SetUnhandledExceptionFilter(GameplayCrashHandler);
            return MikanGameplayTestMain(
                wideArgc, utf8Argv.empty() ? nullptr : utf8Argv.data());
        }();
        LocalFree(wideArgv);
        return result;
    }
#endif
    ConfigureCrashPath(argc, argv);
    SetUnhandledExceptionFilter(GameplayCrashHandler);
    return MikanGameplayTestMain(argc, argv);
}
#else
extern "C" int MikanGameplayTestMain(int argc, char* argv[]);
int main(int argc, char* argv[]) { return MikanGameplayTestMain(argc, argv); }
#endif
