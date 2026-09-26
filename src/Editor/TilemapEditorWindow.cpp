// TilemapEditorWindow.cpp - 内置瓦片地图编辑窗口(L0 切片器 + L1 瓦片绘制)
#include "Editor/TilemapEditorWindow.h"
#include "Editor/EditorUiScale.h"
#include "Core/I18n.h"
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
#include <cstring>
#include <fstream>
#include <functional>
#include <cmath>

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
    const float topY = (float)(ts.margin + r * (ts.tileHeight + ts.spacing));
    u0 = (ts.margin + c * (ts.tileWidth + ts.spacing)) / imgW;
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
} // namespace

void TilemapEditorWindow::Render(bool& showWindow) {
    if (!showWindow) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f * EditorUi::GetUiScale(), 700.0f * EditorUi::GetUiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(I18n::WindowTitle("瓦片编辑器", "editor.tilemap").c_str(), &showWindow)) {
        ImGui::End();
        return;
    }

    RenderSlicer();
    ImGui::Separator();
    if (m_tilesetReady) {
        RenderPalette();
        ImGui::Separator();
        RenderCanvas();
        ImGui::Separator();
    } else {
        ImGui::TextDisabled(Tr("请先在上方切片生成 tileset"));
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

    if (ImGui::Button(Tr("切片生成 tileset")) || (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_tilesetReady == false)) {
        SliceTileset();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(m_tilesetReady ? Tr("已就绪") : "");

    // ===== 图片预览 + 网格线(按当前分割数据实时绘制) =====
    {
        // 路径变化 → 重新加载预览纹理(按路径哈希命名, 避免 TexturePool 同名复用旧图)
        const std::string hash = "__slicer_preview_" + std::to_string(std::hash<std::string>{}(m_imagePath));
        if (hash != m_lastPreviewTex) {
            m_lastPreviewTex = hash;
            m_previewTexName = hash;
            const std::string full = ProjectManager::GetInstance().ResolveAssetPath(m_imagePath);
            if (Renderer2D::GetInstance().LoadTexture(m_previewTexName, full)) {
                ReadImageSize(full, m_previewW, m_previewH);
            }
        }
        VkDescriptorSet ds = Renderer2D::GetInstance().GetTexture(m_previewTexName);
        if (ds != VK_NULL_HANDLE && m_previewW > 0 && m_previewH > 0) {
            ImGui::TextUnformatted(Tr("预览(红网格线 = 当前分割)"));
            static bool s_precise = false; // 精确模式: 按瓦片尺寸+边距对齐素材
            ImGui::Checkbox(Tr("精确对齐(用 tileW/margin/spacing)"), &s_precise);
            const float availW = ImGui::GetContentRegionAvail().x;
            const float scale = std::min(availW / (float)m_previewW, 340.0f / (float)m_previewH);
            const ImVec2 disp((float)m_previewW * scale, (float)m_previewH * scale);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            // 纹理上传 y 翻转: UV 翻转显示原图正向(原图顶部在显示顶部)
            ImGui::Image((ImTextureID)ds, disp, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            const ImVec2 p1(p0.x + disp.x, p0.y + disp.y);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 gridCol = IM_COL32(255, 70, 70, 200);
            const int cols = std::max(m_cols, 1);
            const int rows = std::max(m_rows, 1);
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

bool TilemapEditorWindow::SliceTileset() {
    auto& pm = ProjectManager::GetInstance();
    const std::string imgPath = pm.ResolveAssetPath(m_imagePath);
    int imgW = 0, imgH = 0;
    if (!ReadImageSize(imgPath, imgW, imgH)) {
        LOGE("[TileEditor] cannot read image: %s", imgPath.c_str());
        return false;
    }
    if (m_tsName.empty()) m_tsName = "mytileset";
    if (m_cols < 1) m_cols = 1;
    if (m_rows < 1) m_rows = 1;

    nlohmann::json j;
    j["name"] = m_tsName;
    j["image"] = m_imagePath;
    j["tileWidth"] = m_tileW;
    j["tileHeight"] = m_tileH;
    j["margin"] = m_margin;
    j["spacing"] = m_spacing;
    j["columns"] = m_cols;
    j["tileCount"] = m_cols * m_rows;
    j["imageWidth"] = imgW;
    j["imageHeight"] = imgH;
    j["collidable"] = nlohmann::json::array();

    const std::string rel = "tilesets/" + m_tsName + ".tileset.json";
    const std::string full = pm.ResolveAssetPath(rel);
    std::ofstream ofs(full);
    if (!ofs) {
        LOGE("[TileEditor] cannot write: %s", full.c_str());
        return false;
    }
    ofs << j.dump(2);
    ofs.close();

    // 注册纹理 + 加载 tileset 数据
    Renderer2D::GetInstance().LoadTexture(m_tsName, imgPath);
    std::string err;
    if (!Tmx::LoadTilesetJson(full, m_ts, err)) {
        LOGE("[TileEditor] tileset load failed: %s", err.c_str());
        return false;
    }
    m_tilesetReady = true;
    m_selectedTile = 0;
    m_erase = false;
    // 新建/重置空地图(尺寸保持)
    m_gids.assign((size_t)m_mapW * m_mapH, 0);
    m_dirty = false;
    LOGI("[TileEditor] tileset '%s' sliced: %dx%d tiles (%dx%d)", m_tsName.c_str(),
         m_cols, m_rows, imgW, imgH);
    return true;
}

// ===== 瓦片面板 =====
void TilemapEditorWindow::RenderPalette() {
    ImGui::TextUnformatted(Tr("瓦片面板(点击选择, Ctrl+点击 = 勾选碰撞)"));
    const float cell = 36.0f;
    VkDescriptorSet tex = Renderer2D::GetInstance().GetTexture(m_ts.name);
    const int total = m_cols * m_rows;
    ImGui::BeginChild("palette", ImVec2(0, 0), false);

    for (int i = 0; i < total; ++i) {
        float u0, u1, vT, vB;
        TileUV(m_ts, i, u0, u1, vT, vB);
        const bool isCollidable = m_ts.collidable.count(i) > 0;
        ImGui::PushID(i);
        const ImVec2 size(cell, cell);
        // 用 ImageButton: 瓦片纹理; 选中高亮边框
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.35f, 0.4f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.45f, 0.5f, 1.0f));
        const bool clicked = ImGui::ImageButton("", (ImTextureID)tex, size, ImVec2(u0, vB), ImVec2(u1, vT));
        ImGui::PopStyleColor(3);
        if (clicked) {
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+点击: 切换碰撞标记
                if (isCollidable) m_ts.collidable.erase(i);
                else m_ts.collidable.insert(i);
            } else {
                m_selectedTile = i;
                m_erase = false;
            }
        }
        if (m_selectedTile == i) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                IM_COL32(255, 200, 60, 255), 2.0f, 0, 2.0f);
        }
        if (isCollidable) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                IM_COL32(255, 60, 60, 200), 0.0f, 0, 1.5f);
        }
        ImGui::PopID();
        if ((i + 1) % std::max(m_cols, 1) != 0) ImGui::SameLine();
    }
    ImGui::EndChild();

    ImGui::Text(Tr("当前瓦片: %d %s | 碰撞瓦片: %zu 个"), m_selectedTile,
                m_ts.collidable.count(m_selectedTile) ? "(碰撞)" : "", m_ts.collidable.size());
    ImGui::SameLine();
    if (ImGui::Checkbox(Tr("擦除模式"), &m_erase)) {
        if (m_erase) m_selectedTile = 0;
    }
}

// ===== 地图画布 =====
void TilemapEditorWindow::RenderCanvas() {
    ImGui::TextUnformatted(Tr("地图画布(点击铺/擦瓦片)"));
    ImGui::InputInt(Tr("地图宽"), &m_mapW);
    ImGui::SameLine();
    ImGui::InputInt(Tr("地图高"), &m_mapH);
    if (m_mapW < 1) m_mapW = 1;
    if (m_mapH < 1) m_mapH = 1;
    if (ImGui::Button(Tr("新建空地图"))) {
        m_gids.assign((size_t)m_mapW * m_mapH, 0);
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("保存 .tmap.json"))) SaveTilemap();
    ImGui::SameLine();
    if (ImGui::Button(Tr("应用到场景实体"))) ApplyToScene();
    ImGui::SameLine();
    if (m_dirty) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "未保存");

    if (m_gids.size() != (size_t)m_mapW * m_mapH) {
        m_gids.assign((size_t)m_mapW * m_mapH, 0);
    }

    VkDescriptorSet tex = Renderer2D::GetInstance().GetTexture(m_ts.name);
    const float cell = 26.0f;
    ImGui::BeginChild("canvas", ImVec2(0, 0), true);
    for (int row = 0; row < m_mapH; ++row) {
        for (int col = 0; col < m_mapW; ++col) {
            const int idx = row * m_mapW + col;
            const int gid = m_gids[idx];
            ImGui::PushID(10000 + idx);
            bool clicked = false;
            if (gid > 0) {
                const int local = gid - m_ts.firstGid;
                float u0, u1, vT, vB;
                TileUV(m_ts, local, u0, u1, vT, vB);
                clicked = ImGui::ImageButton("", (ImTextureID)tex, ImVec2(cell, cell),
                                             ImVec2(u0, vB), ImVec2(u1, vT));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.09f, 0.11f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.22f, 0.26f, 1.0f));
                clicked = ImGui::Button("##empty", ImVec2(cell, cell));
                ImGui::PopStyleColor(3);
            }
            if (clicked) {
                if (m_erase) m_gids[idx] = 0;
                else m_gids[idx] = m_selectedTile + m_ts.firstGid;
                m_dirty = true;
            }
            // 显示网格线
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                IM_COL32(60, 60, 70, 90));
            ImGui::PopID();
            if (col + 1 < m_mapW) ImGui::SameLine();
        }
    }
    ImGui::EndChild();
}

bool TilemapEditorWindow::SaveTilemap() {
    if (m_gids.size() != (size_t)m_mapW * m_mapH) {
        LOGE("[TileEditor] map size mismatch");
        return false;
    }
    nlohmann::json j;
    j["tileset"] = m_ts.name;
    j["width"] = m_mapW;
    j["height"] = m_mapH;
    j["tileWidth"] = m_ts.tileWidth;
    j["tileHeight"] = m_ts.tileHeight;
    nlohmann::json layer;
    layer["name"] = "Ground";
    layer["width"] = m_mapW;
    layer["height"] = m_mapH;
    layer["gids"] = m_gids;
    j["layers"] = nlohmann::json::array();
    j["layers"].push_back(layer);

    const std::string full = ProjectManager::GetInstance().ResolveAssetPath(m_mapFile);
    std::ofstream ofs(full);
    if (!ofs) {
        LOGE("[TileEditor] cannot write: %s", full.c_str());
        return false;
    }
    ofs << j.dump(2);
    ofs.close();
    m_dirty = false;
    LOGI("[TileEditor] saved: %s", m_mapFile.c_str());
    return true;
}

void TilemapEditorWindow::ApplyToScene() {
    // 应用到场景中第一个带 TilemapComponent 的实体(Map 实体), 并热加载
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    ECS::Entity found = ECS::INVALID_ENTITY;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (found != ECS::INVALID_ENTITY) return;
        if (coordinator.HasComponent<ECS::TilemapComponent>(e)) { found = e; return; }
        for (const auto& c : sceneECS.GetChildren(e)) visit(c);
    };
    for (const auto& r : sceneECS.GetRootEntities()) visit(r);

    if (found == ECS::INVALID_ENTITY) {
        LOGW("[TileEditor] 场景中没有带 TilemapComponent 的实体(请在层级面板添加)");
        return;
    }
    m_target = found;
    auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(found);
    // 确保已保存
    if (m_dirty) SaveTilemap();
    tc.tilemapFile = m_mapFile;
    tc.tmxPath.clear();
    tc.textureOverride.clear();
    TilemapSystem::GetInstance().ClearColliders(found);
    if (TilemapSystem::GetInstance().LoadTilemap(found)) {
        LOGI("[TileEditor] 已应用到场景实体, 地图热加载完成");
    } else {
        LOGE("[TileEditor] 热加载失败");
    }
}

} // namespace Editor
