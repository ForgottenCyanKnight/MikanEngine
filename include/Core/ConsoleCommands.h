#pragma once
// ConsoleCommands.h - 运行时命令控制台后端（命令解析与执行，UI 无关）
// 命令集面向玩法原型调试：实体枚举/选中/创建/删除、Transform 调整、
// 脚本参数按反射字段表即时读写（FieldMeta::offset 直写实例内存）。
// 编辑器侧 CommandConsoleWindow 只是薄 UI；本类不依赖 ImGui，可被探针程序直调验证。
#include "Platform/Export.h"

#include <string>
#include <vector>

namespace Core {

// 控制台输出行；level: 0=Info 1=Warn 2=Error
struct MIKAN_API ConsoleLine {
    std::string text;
    int level = 0;
};

class MIKAN_API ConsoleCommands {
public:
    // 解析并执行一行命令，结果行追加到 out（同时按级别写入日志系统，
    // 因此输出天然汇聚到编辑器 LogWindow）。未知命令输出 usage 提示(Warn)。
    static void Execute(const std::string& line, std::vector<ConsoleLine>& out);
};

} // namespace Core
