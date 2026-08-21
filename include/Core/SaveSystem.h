#pragma once
// SaveSystem.h - 游戏存档/进度 JSON 文件系统(2D 玩法补齐计划 序4)
// 设计:
//   - 存档为 exe 旁 saves/<slot>.json 的 JSON 文本文件(首次调用自动创建目录);
//   - 写档原子化: 先写 <slot>.json.tmp 再原子替换, 中断/崩溃不会损坏旧档;
//   - 接口只暴露 std::string(JSON 文本), 游戏侧用 dependencies/json.hpp(nlohmann::json)
//     构造/解析对象: Save::Save("profile", j.dump()); auto j = nlohmann::json::parse(*Save::Load("profile"));
//   - slot 仅允许字母数字下划线(防路径穿越), 长度 ≤ 64。
#include "Platform/Export.h"
#include <optional>
#include <string>

namespace Save {

// 存档目录(exe 旁 saves/, 首次调用自动创建); 返回带尾部斜杠的路径
MIKAN_API const std::string& GetSaveDir();

// 写档(原子替换); 返回是否成功
MIKAN_API bool Save(const std::string& slot, const std::string& jsonContent);

// 读档; 存档不存在返回 nullopt
MIKAN_API std::optional<std::string> Load(const std::string& slot);

// 删除存档; 返回是否存在且删除成功
MIKAN_API bool Delete(const std::string& slot);

// 存档是否存在
MIKAN_API bool Exists(const std::string& slot);

} // namespace Save
