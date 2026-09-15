#pragma once

#include "Core/Log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Editor {

class LogWindow {
public:
    static LogWindow& GetInstance();

    void Render(bool& showWindow);

private:
    LogWindow() = default;

    void RefreshSnapshot();
    bool PassesFilter(const Core::LogRecord& record) const;

    std::vector<Core::LogRecord> m_records;
    std::uint64_t m_snapshotSequence = 0;
    int m_levelFilter = 0; // 0 = 全部，其余值对应 LogLevel + 1
    std::string m_sourceFilter;
    char m_search[256] = {};
    bool m_autoScroll = true;
};

} // namespace Editor
