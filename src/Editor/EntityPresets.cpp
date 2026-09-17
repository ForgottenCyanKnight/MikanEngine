// EntityPresets.cpp - 场景树创建预设表(Editor 侧数据,替代 HierarchyWindow 中硬编码的创建菜单)
#include "Editor/EntityPresets.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Core/ProjectManager.h"
#include "Camera.h"
#include <cmath>
#include <typeinfo>

extern MIKAN_API Camera g_Camera;

namespace Editor {

// 2D 对象创建后挂到选中 Canvas 或场景第一个 Canvas 下(与旧 HierarchyWindow 逻辑一致)
static void AttachToCanvas(ECS::Entity entity) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    ECS::Entity sel = sceneECS.GetSelectedEntity();
    ECS::Entity canvas = ECS::INVALID_ENTITY;
    if (sel != ECS::INVALID_ENTITY && coordinator.HasComponent<ECS::Canvas2DComponent>(sel)) {
        canvas = sel;
    } else {
        for (auto e : sceneECS.GetRootEntities()) {
            if (coordinator.HasComponent<ECS::Canvas2DComponent>(e)) {
                canvas = e;
                break;
            }
        }
    }
    if (canvas != ECS::INVALID_ENTITY) {
        sceneECS.SetParent(entity, canvas);
    }
    sceneECS.SetSelectedEntity(entity);
}

// 3D 对象创建后放到相机前方 5 单位(与旧 HierarchyWindow 行为一致),并选中
void PlaceInFrontOfCamera(ECS::Entity entity) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    sceneECS.SetPosition(entity, g_Camera.Position + g_Camera.Front * 5.0f);
    sceneECS.SetSelectedEntity(entity);
}

// 预设表(static 初始化一次;组件名用 typeid 名,与 ComponentRegistry key 一致)
static const std::vector<EntityPreset> g_Presets = [] {
    std::vector<EntityPreset> p;
    auto& coordinator = ECS::Coordinator::GetInstance();

    // ===== 3D =====
    p.push_back({"空对象", "3D", "空对象", {}, nullptr});

    p.push_back({"立方体", "3D", "立方体",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb");
        }});

    p.push_back({"球体", "3D", "球体",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/sphere.glb");
        }});

    p.push_back({"平面", "3D", "平面",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/plane.glb");
        }});

    p.push_back({"圆柱", "3D", "圆柱",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cylinder.glb");
        }});

    p.push_back({"圆锥", "3D", "圆锥",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cone.glb");
        }});

    p.push_back({"胶囊", "3D", "胶囊",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/capsule.glb");
        }});

    p.push_back({"圆环", "3D", "圆环",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/torus.glb");
        }});

    p.push_back({"棱锥", "3D", "棱锥",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/pyramid.glb");
        }});

    p.push_back({"相机", "3D", "相机", {typeid(ECS::CameraComponent).name()}, nullptr});

    p.push_back({"平行光", "3D", "平行光", {typeid(ECS::LightComponent).name()}, nullptr});

    p.push_back({"天空盒", "3D", "天空盒", {typeid(ECS::SkyboxComponent).name()}, nullptr});

    // 体积云控制器：创建一个不带网格的空物体，仅通过 CloudVolumeComponent
    // 接管后处理云参数；选中后可在属性面板实时预览和调节。
    p.push_back({"体积云", "3D", "CloudVolume",
        {typeid(ECS::CloudVolumeComponent).name()}, nullptr});

    // ===== 地形 =====
    // 高度图地形：heightmapPath 留空时由 TerrainRenderer 生成一张程序化平坦高度图，
    // 所以新建出来就是一块平面，不需要用户预先准备 16-bit PNG。
    // 选中后可在属性面板开启"地形编辑模式"，用笔刷在场景视图里雕刻高低。
    // heightOffset = -heightScale/2 配合平坦基准 0.5，让平坦面正好落在 y = 0。
    // 单独占一个分类而不并入 "3D"：避免走"放到相机前方 5 单位"的通用逻辑，
    // 否则 256 单位的平面会把相机埋进去。
    p.push_back({"高度图地形", "地形", "高度图地形",
        {typeid(ECS::TerrainComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& terrain = coordinator.GetComponent<ECS::TerrainComponent>(e);
            terrain.heightmapPath.clear();
            terrain.heightScale = 64.0f;
            terrain.heightOffset = -32.0f;
            // 预设直接挂上项目里的原型材质（图层0 草 / 1 岩 / 2 土 / 3 沙）。
            // 不挂的话四个图层会全部退化成白色回退纹理，高度笔刷还看得出来，
            // 但材质笔刷刷上去是"白涂白"——完全看不出任何变化。
            // 这些是项目相对路径；换到没有这套资源的项目时会退回白模并打日志。
            terrain.layer0Path = "terrain/prototype/materials/leafy_grass_diff_1k.jpg";
            terrain.layer1Path = "terrain/prototype/materials/rock_ground_diff_1k.jpg";
            terrain.layer2Path = "terrain/prototype/materials/dirt_diff_1k.jpg";
            terrain.layer3Path = "terrain/prototype/materials/coast_sand_01_diff_1k.jpg";
            // 与岛屿地形同一档纹理密度（192 次 / 1400 单位 ≈ 0.137 次/单位）。
            terrain.materialTiling = 36.0f;
            ECS::SceneECS::GetInstance().SetPosition(
                e, glm::vec3(std::floor(g_Camera.Position.x), 0.0f,
                             std::floor(g_Camera.Position.z)));
        }});

    // ===== 2D =====
    p.push_back({"Canvas（2D 画布）", "2D", "Canvas",
        {typeid(ECS::Canvas2DComponent).name()}, nullptr});

    p.push_back({"2D 精灵", "2D", "2D 精灵",
        {typeid(ECS::Sprite2DComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& s = coordinator.GetComponent<ECS::Sprite2DComponent>(e);
            s.isUI = true;
            s.type = ECS::Sprite2DComponent::Type::Rect;
            s.width = 64.0f; s.height = 64.0f;
            s.color = glm::vec4(1.0f);
            AttachToCanvas(e);
        }});

    p.push_back({"2D 文本", "2D", "2D 文本",
        {typeid(ECS::TextComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& t = coordinator.GetComponent<ECS::TextComponent>(e);
            t.isUI = true;
            t.text = "文本";
            t.fontSize = 24.0f;
            t.renderMode = ECS::TextComponent::RenderMode::Msdf;
            AttachToCanvas(e);
        }});

    p.push_back({"2D 按钮", "2D", "2D 按钮",
        {typeid(ECS::ButtonComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& b = coordinator.GetComponent<ECS::ButtonComponent>(e);
            b.isUI = true;
            b.width = 160.0f; b.height = 48.0f;
            b.text = "按钮";
            AttachToCanvas(e);
        }});

    p.push_back({"2D 相机", "2D", "2D 相机",
        {typeid(ECS::Camera2DComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& c = coordinator.GetComponent<ECS::Camera2DComponent>(e);
            c.enabled = true;
            c.center = glm::vec2(0.0f);
            c.zoom = 1.0f;
        }});

    p.push_back({"九宫格精灵", "2D", "九宫格",
        {typeid(ECS::Slice9Component).name()},
        [&coordinator](ECS::Entity e) {
            auto& s9 = coordinator.GetComponent<ECS::Slice9Component>(e);
            s9.isUI = true;
            s9.width = 256.0f; s9.height = 256.0f;
            s9.border = glm::vec4(16.0f);
            AttachToCanvas(e);
        }});

    return p;
}();

const std::vector<EntityPreset>& GetEntityPresets() {
    return g_Presets;
}

} // namespace Editor
