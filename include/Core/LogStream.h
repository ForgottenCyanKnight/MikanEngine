// LogStream.h - 把 ostream 风格的链式输出接进日志设施
//
// 为什么需要它：历史上大量代码写的是
//     std::cout << "[Foo] loaded " << count << " of " << total << std::endl;
// 硬改写成 printf 风格需要推断每个参数的实参类型（%s 还是 %zu 还是 %d），
// 推错就是悄无声息的 varargs UB。LogSink 直接把 << 链原样转交 ostringstream，
// 类型由编译器自己挑重载，不可能写错，多行语句也不用拆。
//
//   LOGSTREAM(Info)  << "[Foo] loaded " << count << " of " << total << std::endl;
//   LOGSTREAM(Error) << "[Bar] open failed: " << path;
//
// 约定：
//   - 每条 LOGSTREAM 语句 = 一条日志（析构时统一提交），自带时间戳与级别前缀；
//   - std::endl / std::flush / std::hex 这类操纵符会被吞掉，不必手工删；
//   - 新代码优先用 printf 风格的 LOGx；LOGSTREAM 主要用于承接历史遗留语句。
//
// 只有需要它的翻译单元才包含本头文件 —— <sstream> 偏重，别塞进 Log.h。

#ifndef CORE_LOGSTREAM_H
#define CORE_LOGSTREAM_H

#include "Core/Log.h"

#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace Core {

class LogSink {
public:
    explicit LogSink(LogLevel level) : m_level(level) {}
    ~LogSink() {
        std::string text = m_stream.str();
        if (!text.empty()) {
            LogMessage(m_level, "%s", text.c_str());
        }
    }

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    template <typename T>
    LogSink& operator<<(const T& value) {
        m_stream << value;
        return *this;
    }

    // 吞掉 std::endl / std::flush / std::hex 等操纵符：日志自带换行，
    // 而进制切换之类的状态在“一条语句即一条日志”的模型里没有意义。
    LogSink& operator<<(std::ostream& (*)(std::ostream&)) { return *this; }

private:
    LogLevel m_level;
    std::ostringstream m_stream;
};

} // namespace Core

#define LOGSTREAM(level) ::Core::LogSink(::Core::LogLevel::level)

#endif // CORE_LOGSTREAM_H
