// Log.cpp - 分级日志系统实现（Phase 0.2）
// 目标输出：控制台 / engine.log（滚动）/ OutputDebugString / Android logcat。
// 约定：
//   - engine.log 写 exe 当前工作目录（与 crash_log.txt 同策略），UTF-8，追加模式；
//   - 超过 4MB 自动归档为 engine.old.log（覆盖旧归档）后继续写；
//   - 线程安全（std::mutex）；Fatal 级别写后立即 flush，保证崩溃前落盘。

#include "Core/Log.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Core {

namespace {

constexpr const char* kLevelTags[] = { "DBG", "INF", "WRN", "ERR", "FTL" };
constexpr const char* kLogFileName = "log/engine.log";          // 2026-08-16：日志集中到 log/ 子目录（工作目录不再散落 .log）
constexpr const char* kLogOldFileName = "log/engine.old.log";
constexpr std::uintmax_t kMaxLogFileSize = 4 * 1024 * 1024; // 4MB
constexpr int kMaxMessageLength = 2048;

std::mutex g_logMutex;
LogLevel g_minLevel = LogLevel::Info;
bool g_fileEnabled = true;
std::ofstream g_logFile;

const char* LevelTag(LogLevel level) {
    int i = static_cast<int>(level);
    if (i < 0 || i >= static_cast<int>(LogLevel::Count)) i = static_cast<int>(LogLevel::Info);
    return kLevelTags[i];
}

int AndroidPriority(LogLevel level) {
#ifdef __ANDROID__
    switch (level) {
        case LogLevel::Debug: return ANDROID_LOG_DEBUG;
        case LogLevel::Info:  return ANDROID_LOG_INFO;
        case LogLevel::Warn:  return ANDROID_LOG_WARN;
        case LogLevel::Error: return ANDROID_LOG_ERROR;
        case LogLevel::Fatal: return ANDROID_LOG_FATAL;
        default:              return ANDROID_LOG_INFO;
    }
#else
    (void)level;
    return 0;
#endif
}

// 当前时间戳 [HH:MM:SS.mmm]
std::string Timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

// 打开日志文件（追加模式）；若超限则先归档
void EnsureFileOpen() {
    if (g_logFile.is_open()) return;
    std::error_code ec;
    // 2026-08-16：日志集中到 log/ 子目录——自动创建（cwd 可能没有 log/）
    std::filesystem::create_directories("log", ec);
    ec.clear();
    std::uintmax_t sz = std::filesystem::file_size(kLogFileName, ec);
    if (!ec && sz > kMaxLogFileSize) {
        std::filesystem::remove(kLogOldFileName, ec);
        std::filesystem::rename(kLogFileName, kLogOldFileName, ec);
    }
    g_logFile.open(kLogFileName, std::ios::out | std::ios::app | std::ios::binary);
}

} // namespace

void LogMessage(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_minLevel)) return;

    char body[kMaxMessageLength];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';

    std::string line = "[" + Timestamp() + "][" + LevelTag(level) + "] " + body;

    std::lock_guard<std::mutex> lock(g_logMutex);

#ifdef __ANDROID__
    __android_log_print(AndroidPriority(level), "MikanEngine", "%s", line.c_str());
#else
    // 控制台：错误/致命走 stderr，其余走 stdout
    FILE* out = (level >= LogLevel::Error) ? stderr : stdout;
    std::fprintf(out, "%s\n", line.c_str());
    std::fflush(out);

#ifdef _WIN32
    // 便于 IDE 调试窗口/远程抓取（无控制台进程也能看到）
    OutputDebugStringA((line + "\n").c_str());
#endif
#endif

    if (g_fileEnabled && level >= LogLevel::Info) {
        EnsureFileOpen();
        if (g_logFile.is_open()) {
            g_logFile << line << '\n';
            if (level == LogLevel::Fatal) g_logFile.flush(); // 致命错误立即落盘
        }
    }
}

void SetLogLevel(LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_minLevel = minLevel;
}

LogLevel GetLogLevel() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_minLevel;
}

void SetLogFileEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_fileEnabled = enabled;
    if (!enabled && g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
}

bool IsLogFileEnabled() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_fileEnabled;
}

void ShutdownLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
}

} // namespace Core
