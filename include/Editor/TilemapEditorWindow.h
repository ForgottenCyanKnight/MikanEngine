#pragma once
// TilemapEditorWindow.h - 内置瓦片地图编辑窗口(L0 精灵切片器 + L1 瓦片绘制)
//  - 切片器: 输入 spritesheet + 网格参数 → 生成项目自产 tileset 资源(tilesets/<name>.tileset.json)
//  - 瓦片面板: 显示 tileset 所有瓦片, 点击选择当前瓦片 / 切换擦除
//  - 地图画布: ImGui ImageButton 网格, 点击铺/擦瓦片(实时修改 gid), 保存为 .tmap.json
//  - 应用场景: 写回场景中带 TilemapComponent 实体的 tilemapFile 并热加载
#include "ECS/Types.h"
#include "Core/TmxLoader.h"
#include <string>
#include <vector>

namespace Editor {

class TilemapEditorWindow {
public:
    static TilemapEditorWindow& GetInstance();

    void Render(bool& showWindow);

private:
    TilemapEditorWindow() = default;
    ~TilemapEditorWindow() = default;
    TilemapEditorWindow(const TilemapEditorWindow&) = delete;
    TilemapEditorWindow& operator=(const TilemapEditorWindow&) = delete;

    void RenderSlicer();    // 切片器区
    void RenderPalette();   // 瓦片面板
    void RenderCanvas();    // 地图画布
    bool SliceTileset();    // 生成 tileset 资源并加载
    bool SaveTilemap();
    void ApplyToScene();

    // 切片器参数
    std::string m_imagePath = "textures/tmw_desert_spacing.png";
    std::string m_tsName = "mytileset";
    int m_cols = 8, m_rows = 6, m_margin = 1, m_spacing = 1, m_tileW = 32, m_tileH = 32;
    bool m_tilesetReady = false;
    Tmx::Tileset m_ts;

    // 预览(源图 + 网格线)
    std::string m_previewTexName;   // 当前预览纹理名(按路径哈希, 路径变化自动重载)
    std::string m_lastPreviewTex;
    int m_previewW = 0, m_previewH = 0;

    // 瓦片面板
    int m_selectedTile = 0;   // 当前选择瓦片(local id, 0-based)
    bool m_erase = false;

    // 地图编辑
    int m_mapW = 24, m_mapH = 12;
    std::vector<int> m_gids;  // gid(1-based; 0=空), size = m_mapW*m_mapH
    std::string m_mapFile = "maps/editor_map.tmap.json";
    bool m_dirty = false;
    ECS::Entity m_target = ECS::INVALID_ENTITY;
};

} // namespace Editor
