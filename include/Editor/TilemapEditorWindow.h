#pragma once
#include "Core/TmxLoader.h"
#include <string>

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

    void RenderSlicer();
    void RenderPalette();
    void RenderLayers();
    void RenderCanvas();
    void OpenMap(const std::string& path);
    bool PrepareAtlas(Tmx::Tileset& tileset, std::string& texture);
    bool SliceTileset();
    void NewMap();
    bool SaveNativeMap();
    bool SaveTilemap();
    void ApplyToScene();
    bool HasChanges() const { return m_dirty || m_tilesetDirty; }

    // 切片器参数
    std::string m_imagePath = "textures/tmw_desert_spacing.png";
    std::string m_tsName = "mytileset";
    int m_cols = 8, m_rows = 6, m_margin = 1, m_spacing = 1, m_tileW = 32, m_tileH = 32;

    // 预览(源图 + 网格线)
    std::string m_previewTexName;   // 当前预览纹理名(按路径哈希, 路径变化自动重载)
    std::string m_lastPreviewTex;
    int m_previewW = 0, m_previewH = 0;

    // 瓦片面板
    int m_selectedTile = 0;
    bool m_erase = false;

    Tmx::Map m_map;
    std::string m_projectRoot;
    std::string m_openPath;
    std::string m_mapPath;
    std::string m_mapSource;
    std::string m_tilesetSource;
    std::string m_atlasTexture;
    std::string m_status;
    int m_newWidth = 24, m_newHeight = 12;
    int m_activeLayer = 0;
    bool m_showHidden = false;
    bool m_nativeMap = true;
    bool m_dirty = false;
    bool m_tilesetDirty = false;
};

} // namespace Editor
