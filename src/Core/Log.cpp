// Log.cpp - 分级日志系统实现（Phase 0.2）
// 目标输出：控制台 / engine.log（滚动）/ OutputDebugString / Android logcat。
// 约定：
//   - engine.log 写 exe 当前工作目录（与 crash_log.txt 同策略），UTF-8，追加模式；
//   - 超过 4MB 自动归档为 engine.old.log（覆盖旧归档）后继续写；
//   - 线程安全。用两把职责单一的锁，见下面 g_historyMutex / g_sinkMutex 的注释；
//   - Error 及以上写后立即 flush，尽量保证崩溃前的内容落盘。

#include "Core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Core {

namespace {

constexpr const char* kLevelTags[] = { "DBG", "INF", "WRN", "ERR", "FTL" };
constexpr const char* kLogFileName = "log/engine.log";
constexpr const char* kLogOldFileName = "log/engine.old.log";
constexpr std::uintmax_t kMaxLogFileSize = 4 * 1024 * 1024; // 4MB
constexpr int kMaxMessageLength = 2048;
constexpr std::size_t kMaxBufferedRecords = 4096;

// 两把锁分工明确，不要合并：
//   g_historyMutex 只保护环形缓冲与序号。编辑器每帧都调 GetLogSnapshot()，
//                  它绝不能排在 I/O 后面。
//   g_sinkMutex    只保护终端与日志文件句柄。控制台的 fprintf + fflush 是系统调用，
//                  Windows 上还要过 conhost，耗时完全不可控。
std::mutex g_historyMutex;
std::mutex g_sinkMutex;

// 级别阈值与文件开关都是原子的：LogMessage 的快速判断不该先取锁。
std::atomic<int> g_minLevel{ static_cast<int>(LogLevel::Info) };
std::atomic<bool> g_fileEnabled{ true };

std::ofstream g_logFile;
std::deque<LogRecord> g_logRecords;
std::uint64_t g_logSequence = 0;

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

std::string ExtractSource(const char* body) {
    if (body != nullptr && body[0] == '[') {
        const char* closing = std::strchr(body + 1, ']');
        if (closing != nullptr && closing > body + 1) {
            return std::string(body + 1, closing);
        }
    }
    return "General";
}

// 打开日志文件（追加模式）；若超限则先归档。调用方必须持有 g_sinkMutex。
void EnsureFileOpen() {
    if (g_logFile.is_open()) return;
    std::error_code ec;
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
    char body[kMaxMessageLength];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';

    std::string line = "[" + Timestamp() + "][" + LevelTag(level) + "] " + body;

    // 内存历史独立于输出级别：编辑器 LogWindow 的「级别」下拉框会筛到 Debug，
    // 所以即使终端/文件被 g_minLevel 挡掉，这里也要照记。
    {
        LogRecord buffered;
        buffered.level = level;
        buffered.source = ExtractSource(body);
        buffered.text = line;
        std::lock_guard<std::mutex> lock(g_historyMutex);
        buffered.sequence = ++g_logSequence;
        g_logRecords.push_back(std::move(buffered));
        if (g_logRecords.size() > kMaxBufferedRecords) {
            g_logRecords.pop_front();
        }
    }

    if (static_cast<int>(level) < g_minLevel.load(std::memory_order_relaxed)) return;

#ifdef __ANDROID__
    __android_log_print(AndroidPriority(level), "MikanEngine", "%s", line.c_str());
#else
    // 终端与文件共用 g_sinkMutex，让同一行在两个 sink 里的先后保持一致。
    // 刻意不放进 g_historyMutex —— 编辑器每帧取快照，不能等 I/O。
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);

        // 控制台：错误及以上走 stderr，其余走 stdout
        FILE* out = (level >= LogLevel::Error) ? stderr : stdout;
        std::fprintf(out, "%s\n", line.c_str());
        std::fflush(out);

        if (g_fileEnabled.load(std::memory_order_relaxed) && level >= LogLevel::Info) {
            EnsureFileOpen();
            if (g_logFile.is_open()) {
                g_logFile << line << '\n';
                // 错误及以上立即落盘：崩溃时丢尾部日志的代价，远大于多几次 flush
                if (level >= LogLevel::Error) g_logFile.flush();
            }
        }
    }

#ifdef _WIN32
    // 放在锁外：OutputDebugString 会同步阻塞在调试器上，不该占着 sink 锁
    OutputDebugStringA((line + "\n").c_str());
#endif
#endif
}

std::uint64_t GetLogSequence() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return g_logSequence;
}

std::vector<LogRecord> GetLogSnapshot() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    return std::vector<LogRecord>(g_logRecords.begin(), g_logRecords.end());
}

void ClearLogHistory() {
    std::lock_guard<std::mutex> lock(g_historyMutex);
    g_logRecords.clear();
    ++g_logSequence;
}

void WriteRecentLogsRaw(std::FILE* stream, std::size_t maxRecords) {
    if (stream == nullptr) return;
    // 崩溃路径专用：拿不到锁说明有线程正卡在日志里（甚至就崩在锁内）。
    // 这时只留一行说明，绝不能让崩溃处理器挂死。
    std::unique_lock<std::mutex> lock(g_historyMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::fprintf(stream, "Recent log   : unavailable (log history mutex is held)\n");
        std::fflush(stream);
        return;
    }
    const std::size_t total = g_logRecords.size();
    const std::size_t begin = (total > maxRecords) ? (total - maxRecords) : 0;
    std::fprintf(stream, "Recent log   : %llu of %llu records\n",
        static_cast<unsigned long long>(total - begin),
        static_cast<unsigned long long>(total));
    for (std::size_t i = begin; i < total; ++i) {
        std::fprintf(stream, "  %s\n", g_logRecords[i].text.c_str());
    }
    std::fflush(stream);
}

void SetLogLevel(LogLevel minLevel) {
    g_minLevel.store(static_cast<int>(minLevel), std::memory_order_relaxed);
}

LogLevel GetLogLevel() {
    return static_cast<LogLevel>(g_minLevel.load(std::memory_order_relaxed));
}

void SetLogFileEnabled(bool enabled) {
    g_fileEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        if (g_logFile.is_open()) {
            g_logFile.flush();
            g_logFile.close();
        }
    }
}

bool IsLogFileEnabled() {
    return g_fileEnabled.load(std::memory_order_relaxed);
}

void ShutdownLog() {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
}

} // namespace Core
