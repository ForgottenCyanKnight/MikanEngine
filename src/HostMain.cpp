// HostMain.cpp - thin host executable
// Loads Game.dll (linked at build time) and forwards to MikanEngineMain.
// Editor.dll is optional and detected by Game.dll at startup.
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

extern "C" __declspec(dllimport) int MikanEngineMain(int argc, char* argv[]);

#ifdef _WIN32
namespace {

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

} // namespace
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    // The MSVC narrow argv uses the active ANSI code page.  Rebuild it from
    // the Unicode Windows command line before it reaches the UTF-8 engine API.
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
        const int result = MikanEngineMain(
            wideArgc, utf8Argv.empty() ? nullptr : utf8Argv.data());
        LocalFree(wideArgv);
        return result;
    }
#endif
    return MikanEngineMain(argc, argv);
}
