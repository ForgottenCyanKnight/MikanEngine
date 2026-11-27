#include "Editor/TilemapEditorWindow.h"
#include "Editor/EditorUiScale.h"
#include "Core/AtomicFile.h"
#include "Core/I18n.h"
#include "Core/Utf8Path.h"
#include "Editor/AssetPathPicker.h"
#include "Core/ProjectManager.h"
#include "Core/TilemapSystem.h"
#include "Core/Log.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Rendering/Renderer2D.h"
#include <imgui/imgui.h>
#include <SDL3_image/SDL_image.h>
#include <json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cmath>
#include <iterator>

namespace Editor {

TilemapEditorWindow& TilemapEditorWindow::GetInstance() {
    static TilemapEditorWindow instance;
    return instance;
}

namespace {
// 瓦片 local id → ImGui UV(与 TilemapSystem 渲染同规则: 上传 y 翻转, v=1 原图顶部)
void TileUV(const Tmx::Tileset& ts, int local, float& u0, float& u1, float& vTop, float& vBottom) {
    const int c = local % std::max(ts.columns, 1);
    const int r = local / std::max(ts.columns, 1);
    const float imgW = ts.imageWidth > 0 ? (float)ts.imageWidth : 1.0f;
    const float imgH = ts.imageHeight > 0 ? (float)ts.imageHeight : 1.0f;
    const float topY = ts.margin + r * (static_cast<float>(ts.tileHeight) + ts.spacing);
    u0 = (ts.margin + c * (static_cast<float>(ts.tileWidth) + ts.spacing)) / imgW;
    u1 = u0 + ts.tileWidth / imgW;
    vTop = 1.0f - topY / imgH;                    // 瓦片顶部
    vBottom = vTop - ts.tileHeight / imgH;        // 瓦片底部
}

// 读图片尺寸(IMG_Load)
bool ReadImageSize(const std::string& path, int& w, int& h) {
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) return false;
    w = s->w;
    h = s->h;
    SDL_DestroySurface(s);
    return true;
}
bool ReadSource(const std::string& path, std::string& source) {
    std::ifstream file(Utf8Path(path), std::ios::binary);
    if (!file) return false;
    source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool MayReplace(const std::string& path, const std::string& source) {
    std::error_code error;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(Utf8Path(path), error))) {
        return false;
    }
    if (source.empty()) {
        const bool exists = std::filesystem::exists(Utf8Path(path), error);
        return !exists && !error;
    }
    std::string current;
    return ReadSource(path, current) && current == source;
}

bool EnsureParentDirectory(const std::string& path) {
    const auto parent = Utf8Path(path).parent_path();
    if (parent.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    return !error;
}

bool HasSuffix(std::string path, const std::string& suffix) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return path.ends_with(suffix);
}

bool ValidTilesetName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos;
}

std::string TilesetAssetPath(const std::string& name) {
    return "tilesets/" + name + ".tileset.json";
}

ECS::Entity FindMapEntity(const std::string& preferredPath) {
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    const auto selected = scene.GetSelectedEntity();
    if (selected != ECS::INVALID_ENTITY && coordinator.HasComponent<ECS::TilemapComponent>(selected)) {
        return selected;
    }

    ECS::Entity first = ECS::INVALID_ENTITY;
    ECS::Entity matching = ECS::INVALID_ENTITY;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity entity) {
        if (matching != ECS::INVALID_ENTITY) return;
        if (coordinator.HasComponent<ECS::TilemapComponent>(entity)) {
            if (first == ECS::INVALID_ENTITY) first = entity;
            const auto& tilemap = coordinator.GetComponent<ECS::TilemapComponent>(entity);
            const auto& path = tilemap.tilemapFile.empty() ? tilemap.tmxPath : tilemap.tilemapFile;
            if (!preferredPath.empty() && path == preferredPath) matching = entity;
        }
        for (const auto child : scene.GetChildren(entity)) visit(child);
    };
    for (const auto root : scene.GetRootEntities()) visit(root);
    return matching != ECS::INVALID_ENTITY ? matching : first;
}
} // namespace

void TilemapEditorWindow::Render(bool& showWindow) {
    if (!showWindow) return;
    const float scale = EditorUi::GetUiScale();
    ImGui::SetNextWindowSize(ImVec2(740.0f * scale, 680.0f * scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(I18n::WindowTitle("瓦片编辑器", "editor.tilemap").c_str(), &showWindow)) {
        ImGui::End();
        return;
    }

    const auto& project = ProjectManager::GetInstance();
    if (m_projectRoot != project.GetProjectRoot()) {
        if (HasChanges()) LOGW("[TileEditor] Project changed with an unsaved tilemap");
        m_projectRoot = project.GetProjectRoot();
        m_map = {};
        m_mapPath.clear();
        m_openPath.clear();
        m_mapSource.clear();
        m_tilesetSource.clear();
        m_atlasTexture.clear();
        m_lastPreviewTex.clear();
        m_status.clear();
        m_dirty = m_tilesetDirty = false;
    }
    if (!project.HasActiveProject()) {
        ImGui::TextDisabled("%s", Tr("请先打开项目以编辑瓦片地图"));
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(HasChanges());
    RenderAssetPathInput(Tr("地图文件"), m_openPath, AssetPathKind::Tilemap);
    ImGui::BeginDisabled(m_openPath.empty());
    if (ImGui::Button(Tr("打开地图"))) OpenMap(m_openPath);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto entity = FindMapEntity(m_mapPath);
    ImGui::BeginDisabled(entity == ECS::INVALID_ENTITY);
    if (ImGui::Button(Tr("打开场景地图"))) {
        const auto& tilemap = ECS::Coordinator::GetInstance().GetComponent<ECS::TilemapComponent>(entity);
        OpenMap(tilemap.tilemapFile.empty() ? tilemap.tmxPath : tilemap.tilemapFile);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (!m_map.layers.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("保存"))) SaveTilemap();
        ImGui::SameLine();
        if (ImGui::Button(Tr("应用到场景实体"))) ApplyToScene();
        if (HasChanges()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", Tr("未保存"));
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("放弃修改"))) {
                m_map = {};
                m_dirty = m_tilesetDirty = false;
                m_status.clear();
                if (!m_mapSource.empty()) OpenMap(m_mapPath);
            }
        }
    }
    if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.c_str());

    ImGui::BeginDisabled(HasChanges());
    if (ImGui::CollapsingHeader(Tr("精灵切片器"))) RenderSlicer();
    ImGui::EndDisabled();
    if (m_map.layers.empty()) {
        ImGui::TextDisabled("%s", Tr("打开 TMX 或原生地图，也可使用切片器新建地图"));
    } else {
        ImGui::TextUnformatted(m_mapPath.c_str());
        if (m_nativeMap && m_mapSource.empty()) {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s", m_mapPath.c_str());
            if (ImGui::InputText(Tr("保存路径"), path, sizeof(path))) m_mapPath = path;
            ImGui::InputInt(Tr("地图宽"), &m_newWidth);
            ImGui::InputInt(Tr("地图高"), &m_newHeight);
            if (ImGui::Button(Tr("调整地图尺寸"))) {
                m_newWidth = std::clamp(m_newWidth, 1, 512);
                m_newHeight = std::clamp(m_newHeight, 1, 512);
                for (auto& layer : m_map.layers) {
                    std::vector<int> resized(static_cast<size_t>(m_newWidth) * m_newHeight, 0);
                    const int rows = std::min(layer.height, m_newHeight);
                    const int columns = std::min(layer.width, m_newWidth);
                    for (int row = 0; row < rows; ++row) {
                        std::copy_n(layer.gids.begin() + static_cast<size_t>(row) * layer.width,
                                    columns, resized.begin() + static_cast<size_t>(row) * m_newWidth);
                    }
                    layer.width = m_newWidth;
                    layer.height = m_newHeight;
                    layer.gids = std::move(resized);
                }
                m_map.width = m_newWidth;
                m_map.height = m_newHeight;
            }
        }
        RenderLayers();
        RenderPalette();
        RenderCanvas();
    }
    ImGui::End();
}

// ===== L0 精灵切片器 =====
void TilemapEditorWindow::RenderSlicer() {
    ImGui::TextUnformatted(Tr("精灵切片器: spritesheet → tileset 资源"));
    RenderAssetPathInput("源图(相对资产根)", m_imagePath, AssetPathKind::Texture);

    char nameBuf[128];
    std::strncpy(nameBuf, m_tsName.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = 0;
    ImGui::InputText(Tr("tileset 名称"), nameBuf, sizeof(nameBuf));
    m_tsName = nameBuf;

    ImGui::InputInt(Tr("列数"), &m_cols);
    ImGui::InputInt(Tr("行数"), &m_rows);
    ImGui::InputInt(Tr("瓦片宽"), &m_tileW);
    ImGui::InputInt(Tr("瓦片高"), &m_tileH);
    ImGui::InputInt(Tr("边距 margin"), &m_margin);
    ImGui::InputInt(Tr("间距 spacing"), &m_spacing);

    if (ImGui::Button(Tr("切片生成 tileset"))) {
        SliceTileset();
    }

    // ===== 图片预览 + 网格线(按当前分割数据实时绘制) =====
    {
        // 路径变化 → 重新加载预览纹理(按路径哈希命名, 避免 TexturePool 同名复用旧图)
        const std::string hash = "__slicer_preview_" + std::to_string(std::hash<std::string>{}(ProjectManager::GetInstance().ResolveAssetPath(m_imagePath)));
        if (hash != m_lastPreviewTex) {
            m_lastPreviewTex = hash;
            m_previewTexName = hash;
            const std::string full = ProjectManager::GetInstance().ResolveAssetPath(m_imagePath);
            m_previewW = m_previewH = 0;
            if (Renderer2D::GetInstance().LoadTexture(m_previewTexName, full))
                ReadImageSize(full, m_previewW, m_previewH);
        }
        VkDescriptorSet ds = Renderer2D::GetInstance().GetTexture(m_previewTexName);
        if (ds != VK_NULL_HANDLE && m_previewW > 0 && m_previewH > 0) {
            ImGui::TextUnformatted(Tr("预览(红网格线 = 当前分割)"));
            static bool s_precise = false; // 精确模式: 按瓦片尺寸+边距对齐素材
            ImGui::Checkbox(Tr("精确对齐(用 tileW/margin/spacing)"), &s_precise);
            const float availW = ImGui::GetContentRegionAvail().x;
            const float scale = std::clamp(availW / (float)m_previewW,
                                           0.01f, std::max(0.01f, 340.0f / (float)m_previewH));
            const ImVec2 disp((float)m_previewW * scale, (float)m_previewH * scale);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            // 纹理上传 y 翻转: UV 翻转显示原图正向(原图顶部在显示顶部)
            ImGui::Image((ImTextureID)ds, disp, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            const ImVec2 p1(p0.x + disp.x, p0.y + disp.y);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 gridCol = IM_COL32(255, 70, 70, 200);
            const int cols = std::clamp(m_cols, 1, 512);
            const int rows = std::clamp(m_rows, 1, 512);
            if (s_precise) {
                // 精确: margin + i*(tileW+spacing)(素材带边距/间距时的精确瓦片边界)
                for (int i = 0; i <= cols; ++i) {
                    const float x = p0.x + ((float)m_margin + (float)i * (m_tileW + m_spacing)) * scale;
                    if (x >= p0.x && x <= p1.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), gridCol, 1.0f);
                }
                for (int i = 0; i <= rows; ++i) {
                    const float y = p0.y + ((float)m_margin + (float)i * (m_tileH + m_spacing)) * scale;
                    if (y >= p0.y && y <= p1.y) dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), gridCol, 1.0f);
                }
            } else {
                // 动态等分: 整图按 cols×rows 均分(改行列线数立即变化)
                for (int i = 0; i <= cols; ++i) {
                    const float x = p0.x + (float)i * disp.x / (float)cols;
                    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), gridCol, 1.0f);
                }
                for (int i = 0; i <= rows; ++i) {
                    const float y = p0.y + (float)i * disp.y / (float)rows;
                    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), gridCol, 1.0f);
                }
            }
            ImGui::Text(Tr("图片 %dx%d | 分割 %dx%d 瓦片(%d 条竖线, %d 条横线)"),
                        m_previewW, m_previewH, cols, rows, cols + 1, rows + 1);
        }
    }
}

bool TilemapEditorWindow::PrepareAtlas(Tmx::Tileset& atlas, std::string& texture) {
    const std::string path = ProjectManager::GetInstance().ResolveAssetPath(atlas.imagePath);
    int imageWidth = 0;
    int imageHeight = 0;
    if (!ReadImageSize(path, imageWidth, imageHeight)) {
        m_status = std::string(Tr("无法读取图集")) + " " + path;
        return false;
    }
    if ((atlas.imageWidth > 0 && atlas.imageWidth != imageWidth) ||
        (atlas.imageHeight > 0 && atlas.imageHeight != imageHeight)) {
        m_status = Tr("图集图片尺寸与元数据不一致");
        return false;
    }
    atlas.imageWidth = imageWidth;
    atlas.imageHeight = imageHeight;
    if (atlas.columns <= 0 || atlas.tileCount <= 0 || atlas.tileWidth <= 0 ||
        atlas.tileHeight <= 0 || atlas.margin < 0 || atlas.spacing < 0) {
        m_status = Tr("图集的网格参数无效");
        return false;
    }
    const int columns = std::min(atlas.columns, atlas.tileCount);
    const int rows = (atlas.tileCount - 1) / atlas.columns + 1;
    const int64_t requiredWidth = 2LL * atlas.margin + static_cast<int64_t>(columns) * atlas.tileWidth +
                                  static_cast<int64_t>(columns - 1) * atlas.spacing;
    const int64_t requiredHeight = 2LL * atlas.margin + static_cast<int64_t>(rows) * atlas.tileHeight +
                                   static_cast<int64_t>(rows - 1) * atlas.spacing;
    if (requiredWidth > atlas.imageWidth || requiredHeight > atlas.imageHeight) {
        m_status = Tr("图集的网格参数超出图片边界");
        return false;
    }
    texture = "__tile_editor_atlas_" + std::to_string(std::hash<std::string>{}(path));
    if (!Renderer2D::GetInstance().LoadTexture(texture, path)) {
        m_status = std::string(Tr("无法读取图集")) + " " + path;
        return false;
    }
    return true;
}

void TilemapEditorWindow::OpenMap(const std::string& path) {
    const auto relative = NormalizeAssetPath(path);
    const bool native = HasSuffix(relative, ".tmap.json");
    if (!native && !HasSuffix(relative, ".tmx")) {
        m_status = Tr("选择 TMX 或 .tmap.json 地图文件");
        return;
    }

    auto& project = ProjectManager::GetInstance();
    const auto full = project.ResolveAssetPath(relative);
    Tmx::Map map;
    std::string source, atlasSource, error;
    if (native) {
        if (!ReadSource(full, source) || !Tmx::LoadTilemapJson(full, map, error) ||
            map.tilesets.size() != 1 || !ValidTilesetName(map.tilesets.front().name)) {
            m_status = error.empty() ? Tr("无法打开原生地图") : error;
            return;
        }
        const auto atlasPath = project.ResolveAssetPath(TilesetAssetPath(map.tilesets.front().name));
        Tmx::Tileset tileset;
        if (!ReadSource(atlasPath, atlasSource) || !Tmx::LoadTilesetJson(atlasPath, tileset, error)) {
            m_status = error.empty() ? Tr("无法读取图集") : error;
            return;
        }
        // Runtime looks up the atlas by the filename reference even if its display name differs.
        tileset.name = map.tilesets.front().name;
        if (map.tileWidth <= 0) map.tileWidth = tileset.tileWidth;
        if (map.tileHeight <= 0) map.tileHeight = tileset.tileHeight;
        map.tilesets.front() = std::move(tileset);
    } else if (!Tmx::LoadEditable(full, map, source, error)) {
        m_status = error;
        return;
    }

    if (map.layers.empty() || map.tilesets.size() != 1 ||
        std::any_of(map.layers.begin(), map.layers.end(), [](const Tmx::Layer& layer) {
            return layer.width <= 0 || layer.height <= 0 ||
                   layer.gids.size() != static_cast<size_t>(layer.width) * layer.height;
        })) {
        m_status = Tr("地图图层尺寸无效或缺少图层");
        return;
    }

    std::string texture;
    if (!PrepareAtlas(map.tilesets.front(), texture)) return;
    const auto& atlas = map.tilesets.front();
    const auto hasInvalidGid = [&atlas](const Tmx::Layer& layer) {
        return std::any_of(layer.gids.begin(), layer.gids.end(), [&atlas](int gid) {
            return gid != 0 && (gid < atlas.firstGid ||
                                static_cast<int64_t>(gid) - atlas.firstGid >= atlas.tileCount);
        });
    };
    if (std::any_of(map.layers.begin(), map.layers.end(), hasInvalidGid)) {
        m_status = Tr("地图包含图集以外的瓦片编号");
        return;
    }

    m_map = std::move(map);
    m_mapPath = m_openPath = relative;
    m_mapSource = std::move(source);
    m_tilesetSource = std::move(atlasSource);
    m_atlasTexture = std::move(texture);
    m_nativeMap = native;
    m_newWidth = m_map.width;
    m_newHeight = m_map.height;
    m_selectedTile = m_activeLayer = 0;
    m_showHidden = m_erase = false;
    m_dirty = m_tilesetDirty = false;
    m_status.clear();
}

bool TilemapEditorWindow::SliceTileset() {
    if (!ValidTilesetName(m_tsName)) {
        m_status = Tr("请输入有效的图集名称");
        return false;
    }
    auto& project = ProjectManager::GetInstance();
    const auto relative = TilesetAssetPath(m_tsName);
    const auto path = project.ResolveAssetPath(relative);
    if (!MayReplace(path, "")) {
        m_status = Tr("图集已存在，请使用另一个名称");
        return false;
    }

    Tmx::Tileset tileset;
    tileset.name = m_tsName;
    tileset.imagePath = m_imagePath;
    tileset.columns = std::clamp(m_cols, 1, 512);
    tileset.tileCount = tileset.columns * std::clamp(m_rows, 1, 512);
    tileset.tileWidth = m_tileW;
    tileset.tileHeight = m_tileH;
    tileset.margin = m_margin;
    tileset.spacing = m_spacing;
    std::string texture;
    if (!PrepareAtlas(tileset, texture)) return false;

    const nlohmann::json json = {
        {"name", tileset.name}, {"image", tileset.imagePath},
        {"tileWidth", tileset.tileWidth}, {"tileHeight", tileset.tileHeight},
        {"margin", tileset.margin}, {"spacing", tileset.spacing},
        {"columns", tileset.columns}, {"tileCount", tileset.tileCount},
        {"imageWidth", tileset.imageWidth}, {"imageHeight", tileset.imageHeight},
        {"collidable", nlohmann::json::array()}
    };
    const auto contents = json.dump(2) + "\n";
    if (!EnsureParentDirectory(path) || !Core::WriteAssetFileAtomically(path, contents, "tileset")) {
        m_status = std::string(Tr("无法保存地图资源")) + " " + path;
        return false;
    }

    m_atlasTexture = std::move(texture);
    m_tilesetSource = contents;
    m_map.tilesets = {std::move(tileset)};
    NewMap();
    return true;
}

void TilemapEditorWindow::NewMap() {
    const auto& atlas = m_map.tilesets.front();
    m_map.width = m_newWidth = std::clamp(m_newWidth, 1, 512);
    m_map.height = m_newHeight = std::clamp(m_newHeight, 1, 512);
    m_map.tileWidth = atlas.tileWidth;
    m_map.tileHeight = atlas.tileHeight;
    m_map.layers = {{"Ground", m_map.width, m_map.height,
                     std::vector<int>(static_cast<size_t>(m_map.width) * m_map.height, 0)}};
    m_mapPath = "maps/editor_map.tmap.json";
    m_mapSource.clear();
    m_nativeMap = true;
    m_selectedTile = m_activeLayer = 0;
    m_showHidden = m_erase = false;
    m_dirty = true;
    m_tilesetDirty = false;
    m_status.clear();
}

void TilemapEditorWindow::RenderLayers() {
    ImGui::TextUnformatted(Tr("地图图层"));
    ImGui::SetNextItemWidth(180.0f * EditorUi::GetUiScale());
    if (ImGui::BeginCombo("##tilemap-layer", m_map.layers[m_activeLayer].name.c_str())) {
        for (int i = static_cast<int>(m_map.layers.size()) - 1; i >= 0; --i) {
            ImGui::PushID(i);
            const auto& layer = m_map.layers[i];
            const auto label = layer.name + (layer.visible ? "" : std::string(" (") + Tr("隐藏") + ")");
            if (ImGui::Selectable(label.c_str(), i == m_activeLayer)) {
                m_activeLayer = i;
                if (!layer.visible) m_showHidden = true;
            }
            if (i == m_activeLayer) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(Tr("游戏中显示"), &m_map.layers[m_activeLayer].visible)) {
        m_dirty = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox(Tr("预览隐藏图层"), &m_showHidden);
    if (!m_nativeMap) return;

    char name[256];
    std::snprintf(name, sizeof(name), "%s", m_map.layers[m_activeLayer].name.c_str());
    ImGui::SetNextItemWidth(160.0f * EditorUi::GetUiScale());
    if (ImGui::InputText(Tr("图层名称"), name, sizeof(name))) {
        m_map.layers[m_activeLayer].name = name;
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("添加图层"))) {
        const int width = m_map.width;
        const int height = m_map.height;
        m_map.layers.push_back({
            std::string(Tr("图层")) + " " + std::to_string(m_map.layers.size() + 1),
            width, height, std::vector<int>(static_cast<size_t>(width) * height, 0)});
        m_activeLayer = static_cast<int>(m_map.layers.size()) - 1;
        m_dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(HasChanges());
    if (ImGui::Button(Tr("新建空地图"))) NewMap();
    ImGui::EndDisabled();
}

void TilemapEditorWindow::RenderPalette() {
    ImGui::TextUnformatted(m_nativeMap ? Tr("瓦片面板(点击选择, Ctrl+点击 = 勾选碰撞)") : Tr("瓦片面板"));
    auto& atlas = m_map.tilesets.front();
    const ImTextureID texture = (ImTextureID)Renderer2D::GetInstance().GetTexture(m_atlasTexture);
    const float cell = 32.0f * EditorUi::GetUiScale();
    const int columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x /
                                    (cell + 2 * ImGui::GetStyle().FramePadding.x +
                                     ImGui::GetStyle().ItemSpacing.x)), 1, atlas.columns);
    const int rows = (atlas.tileCount - 1) / columns + 1;
    const float rowHeight = cell + 2 * ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("palette", ImVec2(0, std::min(rows, 3) * rowHeight +
                                           ImGui::GetStyle().WindowPadding.y), ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(rows, rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int column = 0; column < columns; ++column) {
                const int local = row * columns + column;
                if (local >= atlas.tileCount) break;
                if (column > 0) ImGui::SameLine();
                float u0, u1, vTop, vBottom;
                TileUV(atlas, local, u0, u1, vTop, vBottom);
                ImGui::PushID(local);
                if (ImGui::ImageButton("tile", texture, ImVec2(cell, cell),
                                       ImVec2(u0, vTop), ImVec2(u1, vBottom))) {
                    if (ImGui::GetIO().KeyCtrl && m_nativeMap) {
                        if (atlas.collidable.erase(local) == 0) atlas.collidable.insert(local);
                        m_tilesetDirty = true;
                    } else {
                        m_selectedTile = local;
                        m_erase = false;
                    }
                }
                const auto min = ImGui::GetItemRectMin();
                const auto max = ImGui::GetItemRectMax();
                if (atlas.collidable.count(local)) {
                    ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 60, 60, 220));
                }
                if (local == m_selectedTile && !m_erase) {
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x + 2, min.y + 2),
                                                       ImVec2(max.x - 2, max.y - 2),
                                                       IM_COL32(255, 200, 60, 255), 2.0f, 0, 2.0f);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("GID %d", local + atlas.firstGid);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::Checkbox(Tr("擦除模式"), &m_erase);
}

void TilemapEditorWindow::RenderCanvas() {
    ImGui::TextDisabled("%s", Tr("在当前图层绘制，右键擦除"));
    const float scale = 24.0f * EditorUi::GetUiScale() / std::max(m_map.tileWidth, m_map.tileHeight);
    const float cellWidth = std::max(1.0f, m_map.tileWidth * scale);
    const float cellHeight = std::max(1.0f, m_map.tileHeight * scale);
    int width = m_map.width;
    int height = m_map.height;
    for (const auto& layer : m_map.layers) {
        width = std::max(width, layer.width);
        height = std::max(height, layer.height);
    }
    auto& activeLayer = m_map.layers[m_activeLayer];
    const auto& atlas = m_map.tilesets.front();
    const ImTextureID texture = (ImTextureID)Renderer2D::GetInstance().GetTexture(m_atlasTexture);
    if (!ImGui::BeginChild("canvas", ImVec2(0, std::max(130.0f, ImGui::GetContentRegionAvail().y)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("map", ImVec2(width * cellWidth, height * cellHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered();
    const auto mouse = ImGui::GetIO().MousePos;
    const int column = canvasHovered ? static_cast<int>(std::floor((mouse.x - origin.x) / cellWidth)) : -1;
    const int row = canvasHovered ? static_cast<int>(std::floor((mouse.y - origin.y) / cellHeight)) : -1;
    const bool hovered = column >= 0 && column < activeLayer.width && row >= 0 && row < activeLayer.height;
    if (hovered && (activeLayer.visible || m_showHidden)) {
        const bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (ImGui::IsItemActive() && (erase || ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            auto& gid = activeLayer.gids[static_cast<size_t>(row) * activeLayer.width + column];
            const int next = erase || m_erase ? 0 : atlas.firstGid + m_selectedTile;
            if (gid != next) {
                gid = next;
                m_dirty = true;
            }
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto viewMin = draw->GetClipRectMin();
    const auto viewMax = draw->GetClipRectMax();
    const int firstColumn = std::clamp(static_cast<int>((viewMin.x - origin.x) / cellWidth), 0, width);
    const int firstRow = std::clamp(static_cast<int>((viewMin.y - origin.y) / cellHeight), 0, height);
    const int lastColumn = std::clamp(static_cast<int>((viewMax.x - origin.x) / cellWidth) + 1, 0, width);
    const int lastRow = std::clamp(static_cast<int>((viewMax.y - origin.y) / cellHeight) + 1, 0, height);
    const auto point = [origin, cellWidth, cellHeight](int x, int y) {
        return ImVec2(origin.x + x * cellWidth, origin.y + y * cellHeight);
    };
    draw->AddRectFilled(point(firstColumn, firstRow), point(lastColumn, lastRow), IM_COL32(25, 26, 30, 255));
    for (const auto& layer : m_map.layers) {
        if (!layer.visible && !m_showHidden) continue;
        const ImU32 tint = layer.visible ? IM_COL32_WHITE : IM_COL32(255, 220, 145, 180);
        for (int y = firstRow; y < std::min(lastRow, layer.height); ++y) {
            for (int x = firstColumn; x < std::min(lastColumn, layer.width); ++x) {
                const int gid = layer.gids[static_cast<size_t>(y) * layer.width + x];
                if (gid == 0) continue;
                float u0, u1, vTop, vBottom;
                TileUV(atlas, gid - atlas.firstGid, u0, u1, vTop, vBottom);
                draw->AddImage(texture, point(x, y), point(x + 1, y + 1),
                               ImVec2(u0, vTop), ImVec2(u1, vBottom), tint);
            }
        }
    }
    const ImU32 grid = IM_COL32(110, 115, 130, 130);
    for (int x = firstColumn; x <= lastColumn; ++x)
        draw->AddLine(point(x, firstRow), point(x, lastRow), grid);
    for (int y = firstRow; y <= lastRow; ++y)
        draw->AddLine(point(firstColumn, y), point(lastColumn, y), grid);
    if (hovered) {
        draw->AddRect(point(column, row), point(column + 1, row + 1), IM_COL32(255, 200, 60, 255), 0, 0, 2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            const int gid = activeLayer.gids[static_cast<size_t>(row) * activeLayer.width + column];
            ImGui::SetTooltip("%s (%d, %d) | GID %d", activeLayer.name.c_str(), column, row, gid);
        }
    }
    ImGui::EndChild();
}

bool TilemapEditorWindow::SaveNativeMap() {
    auto& project = ProjectManager::GetInstance();
    const auto path = project.ResolveAssetPath(m_mapPath);
    const auto& atlas = m_map.tilesets.front();
    const auto atlasPath = project.ResolveAssetPath(TilesetAssetPath(atlas.name));
    if (!HasSuffix(m_mapPath, ".tmap.json") || path.empty()) {
        m_status = Tr("选择 TMX 或 .tmap.json 地图文件");
        return false;
    }
    if ((m_dirty && !MayReplace(path, m_mapSource)) ||
        (m_tilesetDirty && !MayReplace(atlasPath, m_tilesetSource))) {
        m_status = Tr("目标已存在或文件已被外部修改，请重新打开或另选路径");
        return false;
    }

    try {
        if (m_tilesetDirty) {
            auto atlasJson = nlohmann::json::parse(m_tilesetSource);
            atlasJson["collidable"] = atlas.collidable;
            auto contents = atlasJson.dump(2) + "\n";
            if (!Core::WriteAssetFileAtomically(atlasPath, contents, "tileset")) {
                m_status = std::string(Tr("无法保存地图资源")) + " " + atlasPath;
                return false;
            }
            m_tilesetSource = std::move(contents);
            m_tilesetDirty = false;
        }
        if (m_dirty) {
            const bool newFile = m_mapSource.empty();
            auto json = newFile ? nlohmann::json::object() : nlohmann::json::parse(m_mapSource);
            json["tileset"] = atlas.name;
            json["width"] = m_map.width;
            json["height"] = m_map.height;
            json["tileWidth"] = m_map.tileWidth;
            json["tileHeight"] = m_map.tileHeight;
            for (size_t i = 0; i < m_map.layers.size(); ++i) {
                const auto& layer = m_map.layers[i];
                auto& jsonLayer = json["layers"][i];
                jsonLayer["name"] = layer.name;
                jsonLayer["width"] = layer.width;
                jsonLayer["height"] = layer.height;
                jsonLayer["visible"] = layer.visible;
                jsonLayer["gids"] = layer.gids;
            }
            auto contents = json.dump(2) + "\n";
            if (!EnsureParentDirectory(path) || !Core::WriteAssetFileAtomically(path, contents, "tilemap")) {
                m_status = std::string(Tr("无法保存地图资源")) + " " + path;
                return false;
            }
            m_mapSource = std::move(contents);
            if (newFile && !project.RegisterProjectAsset(path)) {
                LOGW("[TileEditor] Could not register the saved map as a project asset: %s", path.c_str());
            }
        }
    } catch (const nlohmann::json::exception& exception) {
        m_status = exception.what();
        return false;
    }
    return true;
}

bool TilemapEditorWindow::SaveTilemap() {
    if (m_map.layers.empty()) return false;
    if (m_nativeMap) {
        if (!SaveNativeMap()) return false;
    } else if (m_dirty) {
        const auto full = ProjectManager::GetInstance().ResolveAssetPath(m_mapPath);
        if (!Tmx::SaveEditable(full, m_map, m_mapSource, m_status)) return false;
    }
    m_dirty = false;
    m_status = Tr("地图已保存");
    return true;
}

void TilemapEditorWindow::ApplyToScene() {
    const auto entity = FindMapEntity(m_mapPath);
    if (entity == ECS::INVALID_ENTITY) {
        m_status = Tr("场景中没有瓦片地图组件");
        return;
    }
    if (HasChanges() && !SaveTilemap()) return;
    auto& component = ECS::Coordinator::GetInstance().GetComponent<ECS::TilemapComponent>(entity);
    const auto previousTmx = component.tmxPath;
    const auto previousNative = component.tilemapFile;
    const auto previousOverride = component.textureOverride;
    auto& system = TilemapSystem::GetInstance();
    system.ClearColliders(entity);
    component.loaded = false;
    component.tmxPath = m_nativeMap ? "" : m_mapPath;
    component.tilemapFile = m_nativeMap ? m_mapPath : "";
    if (component.tmxPath != previousTmx || component.tilemapFile != previousNative) {
        component.textureOverride.clear();
    }
    if (system.LoadTilemap(entity)) {
        m_status = Tr("地图已应用到场景");
    } else {
        system.ClearColliders(entity);
        component.loaded = false;
        component.tmxPath = previousTmx;
        component.tilemapFile = previousNative;
        component.textureOverride = previousOverride;
        if (!previousTmx.empty() || !previousNative.empty()) system.LoadTilemap(entity);
        m_status = Tr("无法将地图应用到场景");
    }
}

} // namespace Editor
