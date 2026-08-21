// EngineAssets.h - 引擎必需资源清单与启动校验
// 区分"引擎本体必需"与"游戏可选资源"：
//   - 引擎必需（A 类）：shaders/spv/*（glsl 源一一对应）、fonts/simhei.ttf、
//     textures/bluenoise.png、textures/atmo_lut1.png、textures/atmo_lut2.ktx2、
//     textures/white.png、postprocess_chain.json
//   - 编辑器必需（B 类，Editor.dll 存在时）：textures/folder1.png、folder2.png、file.png、material.png
//   - 其余（models/audio/maps/场景 JSON 等）= 游戏可选，不在此校验（validate_scene 负责引用检查）
// 返回缺失项列表（逻辑名，如 "shader: fullscreen.frag.spv"）；空 = 全部就绪。

#pragma once
#include "Platform/Export.h"

#include <string>
#include <vector>

namespace EngineAssets {

MIKAN_API std::vector<std::string> ValidateEngineAssets();

} // namespace EngineAssets
