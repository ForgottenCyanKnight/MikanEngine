// EntityPresets.cpp - 场景树创建预设表(Editor 侧数据,替代 HierarchyWindow 中硬编码的创建菜单)
#include "Editor/EntityPresets.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Core/ProjectManager.h"
#include "Camera.h"
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
static void PlaceInFrontOfCamera(ECS::Entity entity) {
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
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"球体", "3D", "球体",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Model;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/sphere.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"平面", "3D", "平面",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Plane;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/plane.glb");
        }});

    p.push_back({"圆柱", "3D", "圆柱",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Cylinder;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cylinder.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"圆锥", "3D", "圆锥",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Cone;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cone.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"胶囊", "3D", "胶囊",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Capsule;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/capsule.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"圆环", "3D", "圆环",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Torus;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/torus.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"棱锥", "3D", "棱锥",
        {typeid(ECS::MeshComponent).name(), typeid(ECS::RenderComponent).name(), typeid(ECS::MaterialComponent).name()},
        [&coordinator](ECS::Entity e) {
            auto& m = coordinator.GetComponent<ECS::MeshComponent>(e);
            m.type = ECS::MeshType::Pyramid;
            m.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/pyramid.glb");
            PlaceInFrontOfCamera(e);
        }});

    p.push_back({"相机", "3D", "相机", {typeid(ECS::CameraComponent).name()}, nullptr});

    p.push_back({"平行光", "3D", "平行光", {typeid(ECS::LightComponent).name()}, nullptr});

    p.push_back({"天空盒", "3D", "天空盒", {typeid(ECS::SkyboxComponent).name()}, nullptr});

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
