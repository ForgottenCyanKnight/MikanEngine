// Log.h - 分级日志系统（Phase 0.2）
// 替换 printf / fprintf(stderr) / std::cout / std::cerr 的混杂输出。
// 用法（printf 风格，与旧 LOGI/LOGD/LOGE 宏兼容）：
//   LOGD("detail %d", x);   // 调试（默认不输出到文件，见 SetLogLevel）
//   LOGI("info %s", s);     // 常规
//   LOGW("warn %d", x);     // 警告
//   LOGE("error: %d", err); // 错误
//   LOGF("fatal");          // 致命（写后立即 flush）
// 输出目标：
//   - 桌面：控制台（stdout/stderr）+ engine.log（滚动）+ OutputDebugString
//   - Android：logcat（tag "MikanEngine"）
// 编译期关闭全部日志：定义 MIKAN_LOG_DISABLED。

#ifndef CORE_LOG_H
#define CORE_LOG_H

#include "Platform/Export.h"

namespace Core {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4,
    Count,
};

// 输出一条分级日志（printf 风格格式化）。线程安全。
MIKAN_API void LogMessage(LogLevel level, const char* fmt, ...);

// 运行期最小级别过滤（低于该级别的调用直接丢弃；默认 Info）。
// 设置 Debug 可让 LOGD 也写入 engine.log，便于深挖问题。
MIKAN_API void SetLogLevel(LogLevel minLevel);
MIKAN_API LogLevel GetLogLevel();

// engine.log 滚动文件开关（默认开启；headless 测试可用 --no-log-file 关闭时调用此函数置 false）
MIKAN_API void SetLogFileEnabled(bool enabled);
MIKAN_API bool IsLogFileEnabled();

// 关闭时 flush 并释放文件句柄（引擎退出前调用）
MIKAN_API void ShutdownLog();

} // namespace Core

#ifdef MIKAN_LOG_DISABLED
    #define LOGD(...) ((void)0)
    #define LOGI(...) ((void)0)
    #define LOGW(...) ((void)0)
    #define LOGE(...) ((void)0)
    #define LOGF(...) ((void)0)
#else
    #define LOGD(...) ::Core::LogMessage(::Core::LogLevel::Debug, __VA_ARGS__)
    #define LOGI(...) ::Core::LogMessage(::Core::LogLevel::Info,  __VA_ARGS__)
    #define LOGW(...) ::Core::LogMessage(::Core::LogLevel::Warn,  __VA_ARGS__)
    #define LOGE(...) ::Core::LogMessage(::Core::LogLevel::Error, __VA_ARGS__)
    #define LOGF(...) ::Core::LogMessage(::Core::LogLevel::Fatal, __VA_ARGS__)
#endif

#endif // CORE_LOG_H
