#pragma once
#include "Platform/Export.h"

#include <cstdint>
#include <string>

namespace TerrainPaintPersistence {

// ===== 地形笔刷产物持久化 =====
// 显式保存场景前调用：遍历场景里所有地形实体，把有笔刷改动的四张图
// （雕刻高度图 16-bit / 涂色控制图 RGBA8 / 草密度图 R8 / 水位图 R8）写出为
// PNG 到 <场景目录>/terrain_paint/<场景名>_<实体id>_<height|control|grass|water>.png，
// 并把产物路径（项目相对）回填到 TerrainComponent，随后 SceneSerializer
// 会把它们一并写进场景 JSON。重载时渲染与物理碰撞都优先走产物。
//
// 返回写出的文件数（0 = 没有需要保存的改动）。个别文件失败不中断，
// 首个错误写入 errorMessage 并继续保存其余文件。
MIKAN_API uint32_t SaveTerrainPaintData(const std::string& sceneFilePath,
                                        std::string* errorMessage = nullptr);

} // namespace TerrainPaintPersistence
