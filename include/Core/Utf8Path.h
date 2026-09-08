// Utf8Path.h - 跨平台/跨标准稳定的 UTF-8 文件系统路径转换
// 背景：
//  - C++20 弃用了 std::filesystem::u8path()，且 fs::path::u8string()/generic_u8string()
//    改为返回 std::u8string（char8_t）；
//  - NDK r25 (libc++ 14) 的 fs::path 甚至不接受 std::u8string 构造源。
// 因此这里完全不依赖 stdlib 的 char8_t 路径 API：
//  - Windows：UTF-8 <-> UTF-16 经 WinAPI 转换（与原 u8path/u8string 字节语义一致）；
//  - POSIX（Android/Linux）：原生窄编码即 UTF-8，直接构造/返回。
// 引擎内部路径统一为 UTF-8 编码。C++17/20 双标准可用。
#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
// 注意：此处不得定义 WIN32_LEAN_AND_MEAN——它会让 windows.h 不再带出
// <shellapi.h> 等，公共头不应改变 windows.h 对后续代码的可见内容
// （NOMINMAX 已由根 CMakeLists 全局定义，无需在此处理）。
#include <windows.h>

namespace mikanpath {

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                        static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), len);
    return out;
}

} // namespace mikanpath
#endif // _WIN32

// 由 UTF-8 编码的 std::string 构造 fs::path（替代已弃用的 std::filesystem::u8path）
inline std::filesystem::path Utf8Path(const std::string& utf8) {
#if defined(_WIN32)
    return std::filesystem::path(mikanpath::Utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

// fs::path → UTF-8 std::string（不做窄编码页转换，中文名路径安全）
inline std::string Utf8String(const std::filesystem::path& path) {
#if defined(_WIN32)
    return mikanpath::WideToUtf8(path.native());
#else
    return path.native();
#endif
}

// fs::path（generic 格式，统一 '/' 分隔符）→ UTF-8 std::string
inline std::string GenericUtf8String(const std::filesystem::path& path) {
#if defined(_WIN32)
    return mikanpath::WideToUtf8(path.generic_wstring());
#else
    return path.generic_string();
#endif
}
