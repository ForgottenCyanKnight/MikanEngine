// TilemapSystem.cpp - 瓦片地图系统(TMX 导入 + 渲染 + Box2D 碰撞)
#include "Core/TilemapSystem.h"
#include "Core/TmxLoader.h"
#include "Core/ProjectManager.h"
#include "Core/Physics2DManager.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TexturePool.h"
#include "Core/RenderGlobals.h"
#include "box2d/box2d.h"
#include <cstring>
#include <iostream>

namespace {
constexpr float kPixelsPerMeter = 100.0f; // 与 Physics2DSystem 一致

// 纹理名由图片路径派生(与 TexturePool 注册名一致; 全路径用 SDL_GetBasePath 时不同, 演示用相对资产名)
std::string TextureNameFromPath(const std::string& imagePath) {
    // 取文件名去扩展名作为纹理名(如 "tmw_desert_spacing")
    std::string name = imagePath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}
} // namespace

TilemapSystem& TilemapSystem::GetInstance() {
    static TilemapSystem instance;
    return instance;
}

bool TilemapSystem::LoadTilemap(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TilemapComponent>(e)) return false;
    auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(e);
    if (tc.tmxPath.empty() && tc.tilemapFile.empty()) {
        std::cerr << "[Tilemap] empty tmxPath/tilemapFile" << std::endl;
        return false;
    }

    // 解析地图: 自产 .tmap.json 优先, 否则 TMX
    const std::string path = ProjectManager::GetInstance().ResolveAssetPath(
        !tc.tilemapFile.empty() ? tc.tilemapFile : tc.tmxPath);
    std::string err;
    if (!tc.tilemapFile.empty()) {
        if (!Tmx::LoadTilemapJson(path, tc.map, err)) {
            std::cerr << "[Tilemap] load failed: " << err << std::endl;
            return false;
        }
        // 自产格式: tileset 资源按名称单独加载(与地图同目录的 <name>.tileset.json)
        const std::string tsName = tc.map.tilesets.empty() ? "" : tc.map.tilesets.front().name;
        if (tsName.empty()) { std::cerr << "[Tilemap] tileset ref missing" << std::endl; return false; }
        const std::string tsRel = "assets/tilesets/" + tsName + ".tileset.json";
        Tmx::Tileset ts;
        if (!Tmx::LoadTilesetJson(ProjectManager::GetInstance().ResolveAssetPath(tsRel), ts, err)) {
            std::cerr << "[Tilemap] tileset load failed: " << err << std::endl;
            return false;
        }
        tc.map.tilesets.front() = std::move(ts);
        // 地图瓦片尺寸默认取 tileset(缺省时)
        if (tc.map.tileWidth <= 0) tc.map.tileWidth = ts.tileWidth;
        if (tc.map.tileHeight <= 0) tc.map.tileHeight = ts.tileHeight;
    } else {
        if (!Tmx::Load(path, tc.map, err)) {
            std::cerr << "[Tilemap] load failed: " << err << std::endl;
            return false;
        }
    }
    if (tc.map.tilesets.empty()) {
        std::cerr << "[Tilemap] no tileset in map" << std::endl;
        return false;
    }

    // 加载图集纹理(取第一个 tileset 的图片)
    const Tmx::Tileset& ts = tc.map.tilesets.front();
    if (ts.imagePath.empty()) {
        std::cerr << "[Tilemap] tileset has no image" << std::endl;
        return false;
    }
    tc.textureName = TextureNameFromPath(ts.imagePath);
    if (!tc.textureOverride.empty()) {
        tc.textureName = tc.textureOverride; // 调色板变体/外部纹理覆盖
    }
    const std::string texPath = ProjectManager::GetInstance().ResolveAssetPath(ts.imagePath);
    // 注意: 必须用 Renderer2D::LoadTexture 注册(渲染走 Renderer2D::GetTexture,
    // 其纹理表与 g_TexturePool 独立; 直接用 TexturePool 会查不到 → 白纹理)
    if (!Renderer2D::GetInstance().LoadTexture(tc.textureName, texPath)) {
        std::cerr << "[Tilemap] texture load failed: " << texPath << std::endl;
        return false;
    }

    // 生成 Box2D 碰撞体
    if (tc.generateColliders) {
        GenerateColliders(e);
    }

    tc.loaded = true;
    std::cout << "[Tilemap] loaded: " << tc.tmxPath << " (" << tc.map.width << "x" << tc.map.height
              << ", " << tc.map.layers.size() << " layers, texture='" << tc.textureName
              << "', collidable=" << (tc.generateColliders ? ts.collidable.size() : 0) << " tiles)" << std::endl;
    // 临时诊断: 打印 tileset 数据与首瓦片 UV(排查"瓦片重复/采样尺寸不对")
    if (!tc.map.layers.empty() && !tc.map.layers[0].gids.empty()) {
        const Tmx::Tileset& ts0 = tc.map.tilesets.front();
        const int gid0 = tc.map.layers[0].gids[0];
        const int local0 = gid0 - ts0.firstGid;
        const int c0 = local0 % ts0.columns, r0 = local0 / ts0.columns;
        const float u0d = (ts0.margin + c0 * (ts0.tileWidth + ts0.spacing)) / (float)ts0.imageWidth;
        const float vTd = 1.0f - (ts0.margin + r0 * (ts0.tileHeight + ts0.spacing)) / (float)ts0.imageHeight;
        printf("[Tilemap] diag: img=%dx%d cols=%d tw=%d th=%d firstGid=%d | 瓦片0 gid=%d local=%d c=%d r=%d u0=%.4f vTop=%.4f\n",
               ts0.imageWidth, ts0.imageHeight, ts0.columns, ts0.tileWidth, ts0.tileHeight,
               ts0.firstGid, gid0, local0, c0, r0, u0d, vTd);
    }
    return true;
}

void TilemapSystem::GenerateColliders(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(e);
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);

    // 清理旧的
    for (void* p : tc.colliderBodies) {
        if (!p) continue;
        b2BodyId body;
        std::memcpy(&body, &p, sizeof(body));
        if (b2Body_IsValid(body)) b2DestroyBody(body);
    }
    tc.colliderBodies.clear();

    b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
    if (!b2World_IsValid(world)) return;

    const float tw = static_cast<float>(tc.map.tileWidth);
    const float th = static_cast<float>(tc.map.tileHeight);
    // 实体 Transform.position = 地图左上角(画布坐标, y 向下)
    const glm::vec2 origin(t.position.x, t.position.y);

    for (const auto& layer : tc.map.layers) {
        if (!layer.visible) continue;
        for (int row = 0; row < layer.height; ++row) {
            for (int col = 0; col < layer.width; ++col) {
                const int gid = layer.gids[row * layer.width + col];
                if (gid <= 0) continue;
                const Tmx::Tileset* ts = tc.map.FindTileset(gid);
                if (!ts) continue;
                const int localId = gid - ts->firstGid;
                if (ts->collidable.count(localId) == 0) continue;

                // 瓦片中心世界坐标(像素) → 米
                const float cx = origin.x + (col + 0.5f) * tw;
                const float cy = origin.y + (row + 0.5f) * th;
                b2BodyDef bd = b2DefaultBodyDef();
                bd.type = b2_staticBody;
                bd.position = { cx / kPixelsPerMeter, cy / kPixelsPerMeter };
                bd.userData = (void*)(uintptr_t)e;
                b2BodyId body = b2CreateBody(world, &bd);
                b2ShapeDef sd = b2DefaultShapeDef();
                b2Polygon poly = b2MakeBox(tw * 0.5f / kPixelsPerMeter, th * 0.5f / kPixelsPerMeter);
                b2CreatePolygonShape(body, &sd, &poly);

                void* storage = nullptr;
                std::memcpy(&storage, &body, sizeof(body));
                tc.colliderBodies.push_back(storage);
            }
        }
    }
    std::cout << "[Tilemap] generated " << tc.colliderBodies.size() << " collider bodies" << std::endl;
}

void TilemapSystem::Render(Renderer2D& r2d, ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TilemapComponent>(e)) return;
    auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(e);
    if (!tc.loaded || tc.map.layers.empty()) return;

    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
    const glm::vec2 origin(t.position.x, t.position.y);
    const float tw = static_cast<float>(tc.map.tileWidth);
    const float th = static_cast<float>(tc.map.tileHeight);
    VkDescriptorSet tex = r2d.GetTexture(tc.textureName);

    for (const auto& layer : tc.map.layers) {
        if (!layer.visible) continue;
        for (int row = 0; row < layer.height; ++row) {
            for (int col = 0; col < layer.width; ++col) {
                const int gid = layer.gids[row * layer.width + col];
                if (gid <= 0) continue;
                const Tmx::Tileset* ts = tc.map.FindTileset(gid);
                if (!ts) continue;
                const int localId = gid - ts->firstGid;

                // 源矩形(考虑 margin/spacing/columns); 纹理 y 翻转(上传时原图顶部→v=1)
                const int c = localId % ts->columns;
                const int r = localId / ts->columns;
                const float imgW = static_cast<float>(ts->imageWidth > 0 ? ts->imageWidth : 1);
                const float imgH = static_cast<float>(ts->imageHeight > 0 ? ts->imageHeight : 1);
                const float topY = static_cast<float>(ts->margin + r * (ts->tileHeight + ts->spacing));
                const float u0 = (ts->margin + c * (ts->tileWidth + ts->spacing)) / imgW;
                const float u1 = u0 + ts->tileWidth / imgW;
                const float vBottom = 1.0f - (topY + ts->tileHeight) / imgH; // 帧底部(uv0.y)
                const float vTop = 1.0f - topY / imgH;                       // 帧顶部(uv1.y)

                const glm::vec2 pos(origin.x + col * tw, origin.y + row * th);
                // DrawSprite: uv0=左上(屏幕顶部采样), uv1=右下; 顶部显示帧顶部 vTop
                r2d.DrawSprite(pos, glm::vec2(tw, th), tex, { u0, vTop }, { u1, vBottom },
                               glm::vec4(1.0f), tc.layer);
            }
        }
    }
}

void TilemapSystem::ClearColliders(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TilemapComponent>(e)) return;
    auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(e);
    for (void* p : tc.colliderBodies) {
        if (!p) continue;
        b2BodyId body;
        std::memcpy(&body, &p, sizeof(body));
        if (b2Body_IsValid(body)) b2DestroyBody(body);
    }
    tc.colliderBodies.clear();
}

void TilemapSystem::ClearAll() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::TilemapComponent>(e)) ClearColliders(e);
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
}

void TilemapSystem::LoadAllFromScene() {
    ClearAll();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::TilemapComponent>(e)) {
            LoadTilemap(e);
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
}

void TilemapSystem::RenderDebugColliders(Renderer2D& r2d) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    constexpr float kPpm = 100.0f;
    const glm::vec4 col(1.0f, 0.3f, 0.3f, 0.9f); // 红色: 瓦片碰撞体
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::TilemapComponent>(e)) {
            auto& tc = coordinator.GetComponent<ECS::TilemapComponent>(e);
            for (void* p : tc.colliderBodies) {
                if (!p) continue;
                b2BodyId body;
                std::memcpy(&body, &p, sizeof(body));
                if (!b2Body_IsValid(body)) continue;
                b2ShapeId shape;
                if (b2Body_GetShapes(body, &shape, 1) > 0) {
                    const b2AABB aabb = b2Shape_GetAABB(shape);
                    const glm::vec2 min(aabb.lowerBound.x * kPpm, aabb.lowerBound.y * kPpm);
                    const glm::vec2 max(aabb.upperBound.x * kPpm, aabb.upperBound.y * kPpm);
                    const glm::vec2 center = (min + max) * 0.5f;
                    const glm::vec2 size = max - min;
                    const float t = 2.0f;
                    r2d.DrawRect({ center.x - size.x * 0.5f, center.y - size.y * 0.5f - t }, { size.x, t }, col, 19);
                    r2d.DrawRect({ center.x - size.x * 0.5f, center.y + size.y * 0.5f }, { size.x, t }, col, 19);
                    r2d.DrawRect({ center.x - size.x * 0.5f - t, center.y - size.y * 0.5f }, { t, size.y }, col, 19);
                    r2d.DrawRect({ center.x + size.x * 0.5f, center.y - size.y * 0.5f }, { t, size.y }, col, 19);
                }
            }
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
}
