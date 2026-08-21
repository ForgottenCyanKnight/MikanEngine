#pragma once
// TmxLoader.h - Tiled 编辑器 TMX 地图解析器(正交地图)
// 支持: CSV 与 base64(zlib) 两种图层编码; 内嵌 tileset 与外部 .tsx 引用;
//       tileset 瓦片属性 collision(碰撞标记); spacing/margin 网格。
// 依赖: tinyxml2(XML) + miniz(zlib inflate)。
#include "Platform/Export.h"
#include <set>
#include <string>
#include <vector>

namespace Tmx {

struct Tileset {
    int firstGid = 1;
    std::string name;
    int tileWidth = 0, tileHeight = 0;
    int margin = 0, spacing = 0;      // Tiled 网格边距/间距(像素)
    int columns = 0, tileCount = 0;
    std::string imagePath;            // 图集图片路径(相对 .tmx/.tsx 所在目录)
    int imageWidth = 0, imageHeight = 0;
    std::set<int> collidable;         // 本地瓦片 id(0-based): 属性 collision=true
};

struct Layer {
    std::string name;
    int width = 0, height = 0;
    std::vector<int> gids;            // 全局瓦片 id(gid; 0 = 空), size = width*height
    bool visible = true;
};

struct Map {
    int width = 0, height = 0;
    int tileWidth = 0, tileHeight = 0;
    std::vector<Tileset> tilesets;
    std::vector<Layer> layers;

    // gid(全局) → 所在 tileset; 空瓦片(gid==0)返回 nullptr
    const Tileset* FindTileset(int gid) const;
};

// 解析 TMX 文件; 成功返回 true, 失败返回 false 并填 err
MIKAN_API bool Load(const std::string& tmxPath, Map& out, std::string& err);

// 解析引擎自产 tileset 资源(JSON, 由编辑器切片器生成): 填充 Tileset(含 imageWidth/Height)
MIKAN_API bool LoadTilesetJson(const std::string& jsonPath, Tileset& out, std::string& err);

// 解析引擎自产瓦片地图资源(JSON): 引用 tileset 资源名, 填充 Map(tilesets/layers)
MIKAN_API bool LoadTilemapJson(const std::string& jsonPath, Map& out, std::string& err);

// 工具: base64 → 字节(zlib 解压前)
MIKAN_API std::vector<unsigned char> DecodeBase64(const std::string& b64);

} // namespace Tmx
