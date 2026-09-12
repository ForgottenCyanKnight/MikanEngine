#define GLM_ENABLE_EXPERIMENTAL
#include "ECS/SceneECS.h"
#include "ECS/Systems/RenderSystem.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "Core/Physics2DSystem.h"
#include "Core/PlayerControllerSystem.h"
#include "Core/Log.h"
#include "Rendering/SceneRenderer.h"
#include "EngineConfig.h"
#include "ECS/Systems/VmdSystem.h"
#include <algorithm>
#include <cmath>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
// 全局声明（勿放 namespace 内——否则变 ECS::g_SceneRenderer 8 字节 COMMON）
extern ::SceneRenderer g_SceneRenderer;

namespace ECS {

namespace {

// 从矩阵恢复本地 TRS。层级重挂接必须同时保持平移、旋转和缩放，
// 仅修正 position 会在父节点有旋转/非均匀缩放时产生明显跳变。
bool ApplyTransformMatrix(TransformComponent& transform, const glm::mat4& matrix) {
    glm::vec3 scale(1.0f);
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 translation(0.0f);
    glm::vec3 skew(0.0f);
    glm::vec4 perspective(0.0f);
    if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
        return false;
    }

    const auto finiteVec3 = [](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    const bool finiteRotation = std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
                                std::isfinite(rotation.z) && std::isfinite(rotation.w);
    const float rotationLengthSquared = glm::dot(rotation, rotation);
    if (!finiteVec3(scale) || !finiteVec3(translation) || !finiteRotation ||
        !std::isfinite(rotationLengthSquared) || rotationLengthSquared < 1e-8f) {
        return false;
    }

    transform.position = translation;
    transform.rotation = glm::normalize(rotation);
    transform.scale = scale;
    transform.MarkDirty();
    return true;
}

} // namespace

void SceneECS::Init() {
    auto& coordinator = Coordinator::GetInstance();
    coordinator.Init();

    // 注册所有组件
    coordinator.RegisterComponent<NameComponent>();
    coordinator.RegisterComponent<LockOnTargetComponent>();
    coordinator.RegisterComponent<TransformComponent>();
    coordinator.RegisterComponent<HierarchyComponent>();
    coordinator.RegisterComponent<MeshComponent>();
    coordinator.RegisterComponent<RenderComponent>();
    coordinator.RegisterComponent<ColliderComponent>();
    coordinator.RegisterComponent<RigidBodyComponent>();
    coordinator.RegisterComponent<PlayerControllerComponent>();
    coordinator.RegisterComponent<CameraComponent>();
    coordinator.RegisterComponent<LightComponent>();
    coordinator.RegisterComponent<SkyboxComponent>();
    coordinator.RegisterComponent<CloudVolumeComponent>();
    coordinator.RegisterComponent<Camera2DComponent>();
    coordinator.RegisterComponent<RigidBody2DComponent>();
    coordinator.RegisterComponent<Collider2DComponent>();
    coordinator.RegisterComponent<Contact2DComponent>();
    coordinator.RegisterComponent<SpriteAnimationComponent>();
    coordinator.RegisterComponent<TilemapComponent>();
    coordinator.RegisterComponent<AnimatorComponent>();
    coordinator.RegisterComponent<VmdPlayerComponent>();
    coordinator.RegisterComponent<ScriptComponent>();
    coordinator.RegisterComponent<MaterialComponent>();
    coordinator.RegisterComponent<TerrainComponent>();
    coordinator.RegisterComponent<WaterComponent>();
    coordinator.RegisterComponent<VoxModelComponent>();
    coordinator.RegisterComponent<WorldComponent>();
    coordinator.RegisterComponent<Sprite2DComponent>();
    coordinator.RegisterComponent<Canvas2DComponent>();
    coordinator.RegisterComponent<TextComponent>();
    coordinator.RegisterComponent<ButtonComponent>();
    coordinator.RegisterComponent<Slice9Component>();
    coordinator.RegisterComponent<TweenComponent>();
    coordinator.RegisterComponent<AudioSourceComponent>();

    // 注册全部组件的元数据(供编辑器通用展示/增删组件,数据驱动)
    RegisterAllComponentMeta();

    // 注册并配置渲染系统
    m_RenderSystem = coordinator.RegisterSystem<RenderSystem>();
    {
        Signature signature;
        signature.set(coordinator.GetComponentType<TransformComponent>());
        signature.set(coordinator.GetComponentType<MeshComponent>());
        signature.set(coordinator.GetComponentType<RenderComponent>());
        coordinator.SetSystemSignature<RenderSystem>(signature);
    }

    // 第三人称玩家控制器：实体挂载 Transform + RigidBody + PlayerController
    // 后自动进入该系统，系统本身不创建碰撞体，避免和 PhysicsSystem 重复管理。
    coordinator.RegisterSystem<PlayerControllerSystem>();
    {
        Signature signature;
        signature.set(coordinator.GetComponentType<TransformComponent>());
        signature.set(coordinator.GetComponentType<RigidBodyComponent>());
        signature.set(coordinator.GetComponentType<PlayerControllerComponent>());
        coordinator.SetSystemSignature<PlayerControllerSystem>(signature);
    }

    // 注意：场景文件将在初始化完成后加载，而不是在 Init 中立即加载
    // 这是为了确保所有系统（如物理系统、渲染系统）都完全初始化
}

void SceneECS::Shutdown() {
    VmdSystem::GetInstance().Clear();
    // ECS 会自动清理
}

Entity SceneECS::CreateEmpty(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = coordinator.CreateEntity();

    coordinator.AddComponent<NameComponent>(entity, {name});
    coordinator.AddComponent<TransformComponent>(entity, {});
    coordinator.AddComponent<HierarchyComponent>(entity, {});

    MarkRootsDirty(); // 新实体可能是根节点
    MarkEntitySetDirty(); // 实体集合变化,外部缓存(相机列表等)需失效
    return entity;
}

Entity SceneECS::CreateCube(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});

    return entity;
}

Entity SceneECS::CreateSphere(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/sphere.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});

    return entity;
}

Entity SceneECS::CreatePlane(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/plane.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true, false, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreateCylinder(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cylinder.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreateCone(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cone.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreateCapsule(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/capsule.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreateTorus(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/torus.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreatePyramid(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    MeshComponent mesh;
    mesh.type = MeshType::Model;
    mesh.modelPath = ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/pyramid.glb");
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    coordinator.AddComponent<RenderComponent>(entity, {true, true, true});
    coordinator.AddComponent<MaterialComponent>(entity, {});

    return entity;
}

Entity SceneECS::CreateCamera(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    CameraComponent camera;
    camera.isMainCamera = false;
    camera.enableFrustumCulling = true;
    camera.showFrustumWireframe = true;
    camera.useSubMeshCulling = true;
    camera.showBVHWireframe = false;
    coordinator.AddComponent<CameraComponent>(entity, camera);

    return entity;
}

Entity SceneECS::CreateLight(const std::string& name, LightComponent::Type type) {
    auto& coordinator = Coordinator::GetInstance();
    Entity entity = CreateEmpty(name);

    coordinator.AddComponent<LightComponent>(entity, {type});

    return entity;
}

void SceneECS::DestroyEntity(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.IsAlive(entity)) return;

    MarkRootsDirty(); // 销毁可能移除根节点
    MarkEntitySetDirty(); // 实体集合变化,外部缓存(相机列表等)需失效

    // 先移除所有子对象的父引用
    if (coordinator.HasComponent<HierarchyComponent>(entity)) {
        auto& hierarchy = coordinator.GetComponent<HierarchyComponent>(entity);
        for (Entity child : hierarchy.children) {
            if (coordinator.HasComponent<HierarchyComponent>(child)) {
                coordinator.GetComponent<HierarchyComponent>(child).parent = INVALID_ENTITY;
                // 子节点父链变化,世界矩阵需失效(由根变独立)
                if (coordinator.HasComponent<TransformComponent>(child)) {
                    coordinator.GetComponent<TransformComponent>(child).MarkDirty();
                }
            }
        }

        // 从父对象的子列表中移除
        if (hierarchy.parent != INVALID_ENTITY &&
            coordinator.HasComponent<HierarchyComponent>(hierarchy.parent)) {
            auto& parentHierarchy = coordinator.GetComponent<HierarchyComponent>(hierarchy.parent);
            auto it = std::find(parentHierarchy.children.begin(), parentHierarchy.children.end(), entity);
            if (it != parentHierarchy.children.end()) {
                parentHierarchy.children.erase(it);
            }
        }
    }

    // 如果选中的是这个实体，清除选择
    if (m_SelectedEntity == entity) {
        m_SelectedEntity = INVALID_ENTITY;
    }

    // 2D 物理: 先销毁实体的 b2Body 再移除组件, 防止幽灵体残留 + 实体 ID 复用后
    // 接触事件错映射(Contact2DComponent 回调内销毁实体同样走这里, 安全)。
    // 必须在 coordinator.DestroyEntity(移除组件)之前调用, 此时 RigidBody2DComponent 仍可读。
    if (coordinator.HasComponent<RigidBody2DComponent>(entity)) {
        Physics2DSystem::GetInstance().RemoveBody(entity);
    }

    coordinator.DestroyEntity(entity);
}

void SceneECS::SetName(Entity entity, const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<NameComponent>(entity)) {
        coordinator.GetComponent<NameComponent>(entity).name = name;
    }
}

std::string SceneECS::GetName(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<NameComponent>(entity)) {
        return coordinator.GetComponent<NameComponent>(entity).name;
    }
    return "Unknown";
}

void SceneECS::SetSelectedEntity(Entity entity) {
    // 选中状态仅由 m_SelectedEntity 维护(SelectedComponent 冗余标记已移除);
    // gizmo/编辑器通过 GetSelectedEntity() 读取,不需要组件位。
    m_SelectedEntity = entity;
}

void SceneECS::ClearSelection() {
    SetSelectedEntity(INVALID_ENTITY);
}

void SceneECS::SetParent(Entity child, Entity parent) {
    auto& coordinator = Coordinator::GetInstance();

    if (!coordinator.IsAlive(child) ||
        (parent != INVALID_ENTITY && !coordinator.IsAlive(parent))) return;
    if (!coordinator.HasComponent<HierarchyComponent>(child)) return;
    if (parent != INVALID_ENTITY && !coordinator.HasComponent<HierarchyComponent>(parent)) return;

    MarkRootsDirty(); // 父级变更可能改变根集合
    MarkEntitySetDirty(); // 层级变化会改变树的收集结果,外部缓存需失效

    // 防循环: parent 不能是 child 自身或其子孙(否则形成环, 导致遍历/渲染死循环、实体消失)
    if (child == parent) {
        printf("[SceneECS] SetParent rejected (self-parenting): %u\n", (uint32_t)child);
        return;
    }
    Entity cursor = parent;
    while (cursor != INVALID_ENTITY) {
        if (cursor == child) {
            printf("[SceneECS] SetParent rejected (would create cycle): %u -> %u\n",
                   (uint32_t)child, (uint32_t)parent);
            return;
        }
        cursor = coordinator.GetComponent<HierarchyComponent>(cursor).parent;
    }

    auto& childHierarchy = coordinator.GetComponent<HierarchyComponent>(child);

    // 保持完整世界变换(Unity 行为): 设为子级时重算本地 TRS, 使世界变换不瞬移。
    // 只修正 position 会在父节点有旋转/缩放时改变子节点的朝向和尺寸。
    const glm::mat4 oldWorld = GetWorldMatrix(child);

    // 从旧父对象中移除
    if (childHierarchy.parent != INVALID_ENTITY &&
        coordinator.HasComponent<HierarchyComponent>(childHierarchy.parent)) {
        auto& oldParentHierarchy = coordinator.GetComponent<HierarchyComponent>(childHierarchy.parent);
        auto it = std::find(oldParentHierarchy.children.begin(), oldParentHierarchy.children.end(), child);
        if (it != oldParentHierarchy.children.end()) {
            oldParentHierarchy.children.erase(it);
        }
    }

    // 设置新父对象
    childHierarchy.parent = parent;

    // 添加到新父对象的子列表并反解新的本地 TRS
    if (parent != INVALID_ENTITY) {
        auto& parentHierarchy = coordinator.GetComponent<HierarchyComponent>(parent);
        parentHierarchy.children.push_back(child);

        // local = inverse(parentWorld) * oldWorld，完整保持世界平移/旋转/缩放。
        const glm::mat4 parentWorld = GetWorldMatrix(parent);
        if (coordinator.HasComponent<TransformComponent>(child)) {
            auto& t = coordinator.GetComponent<TransformComponent>(child);
            if (!ApplyTransformMatrix(t, glm::inverse(parentWorld) * oldWorld)) {
                // 父矩阵退化时无法稳定分解，至少保留世界平移，避免实体直接跳到原点。
                t.position = glm::vec3(oldWorld[3]);
                t.MarkDirty();
                printf("[SceneECS] SetParent warning: failed to decompose local transform for %u\n",
                       static_cast<uint32_t>(child));
            }
        }
    } else if (coordinator.HasComponent<TransformComponent>(child)) {
        // RemoveParent 也必须把旧世界矩阵恢复成本地矩阵，否则解除父级后会
        // 把原本的 local TRS 当作根节点变换，造成位置/旋转/缩放突变。
        auto& t = coordinator.GetComponent<TransformComponent>(child);
        if (!ApplyTransformMatrix(t, oldWorld)) {
            t.position = glm::vec3(oldWorld[3]);
            t.MarkDirty();
            printf("[SceneECS] RemoveParent warning: failed to decompose world transform for %u\n",
                   static_cast<uint32_t>(child));
        }
    }
}

void SceneECS::RemoveParent(Entity child) {
    SetParent(child, INVALID_ENTITY);
}

Entity SceneECS::GetParent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.IsAlive(entity) && coordinator.HasComponent<HierarchyComponent>(entity)) {
        return coordinator.GetComponent<HierarchyComponent>(entity).parent;
    }
    return INVALID_ENTITY;
}

const std::vector<Entity>& SceneECS::GetChildren(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.IsAlive(entity) && coordinator.HasComponent<HierarchyComponent>(entity)) {
        return coordinator.GetComponent<HierarchyComponent>(entity).children;
    }
    static const std::vector<Entity> emptyChildren;
    return emptyChildren;
}

void SceneECS::SetVisible(Entity entity, bool visible) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<RenderComponent>(entity)) {
        coordinator.GetComponent<RenderComponent>(entity).visible = visible; return;
    }
    if (coordinator.HasComponent<Sprite2DComponent>(entity)) {
        coordinator.GetComponent<Sprite2DComponent>(entity).visible = visible; return;
    }
    if (coordinator.HasComponent<TextComponent>(entity)) {
        coordinator.GetComponent<TextComponent>(entity).visible = visible; return;
    }
    if (coordinator.HasComponent<ButtonComponent>(entity)) {
        coordinator.GetComponent<ButtonComponent>(entity).visible = visible; return;
    }
    if (coordinator.HasComponent<Slice9Component>(entity)) {
        coordinator.GetComponent<Slice9Component>(entity).visible = visible; return;
    }
}

bool SceneECS::IsVisible(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<RenderComponent>(entity))
        return coordinator.GetComponent<RenderComponent>(entity).visible;
    if (coordinator.HasComponent<Sprite2DComponent>(entity))
        return coordinator.GetComponent<Sprite2DComponent>(entity).visible;
    if (coordinator.HasComponent<TextComponent>(entity))
        return coordinator.GetComponent<TextComponent>(entity).visible;
    if (coordinator.HasComponent<ButtonComponent>(entity))
        return coordinator.GetComponent<ButtonComponent>(entity).visible;
    if (coordinator.HasComponent<Slice9Component>(entity))
        return coordinator.GetComponent<Slice9Component>(entity).visible;
    return true;  // 无可见性组件的实体（容器如 Canvas）默认可见
}

void SceneECS::SetPosition(Entity entity, const glm::vec3& position) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<TransformComponent>(entity);
        t.position = position;
        t.MarkDirty();
    }
}

glm::vec3 SceneECS::GetPosition(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        return coordinator.GetComponent<TransformComponent>(entity).position;
    }
    return glm::vec3(0.0f);
}

void SceneECS::SetRotation(Entity entity, const glm::quat& rotation) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<TransformComponent>(entity);
        t.rotation = rotation;
        t.MarkDirty();
    }
}

void SceneECS::SetRotationEuler(Entity entity, const glm::vec3& euler) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        coordinator.GetComponent<TransformComponent>(entity).SetEulerAngles(euler);
    }
}

glm::vec3 SceneECS::GetRotationEuler(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        return coordinator.GetComponent<TransformComponent>(entity).GetEulerAngles();
    }
    return glm::vec3(0.0f);
}

void SceneECS::SetScale(Entity entity, const glm::vec3& scale) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<TransformComponent>(entity);
        t.scale = scale;
        t.MarkDirty();
    }
}

glm::vec3 SceneECS::GetScale(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        return coordinator.GetComponent<TransformComponent>(entity).scale;
    }
    return glm::vec3(1.0f);
}

glm::mat4 SceneECS::GetWorldMatrix(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<TransformComponent>(entity)) {
        return glm::mat4(1.0f);
    }
    auto& t = coordinator.GetComponent<TransformComponent>(entity);

    // 父链:先确保父的世界矩阵与 worldVersion 已更新(递归,命中缓存时 O(1))
    Entity parent = GetParent(entity);
    uint64_t parentWorldVersion = 0;
    if (parent != INVALID_ENTITY) {
        if (coordinator.HasComponent<TransformComponent>(parent)) {
            GetWorldMatrix(parent); // 递归计算并缓存父(内部会更新父的 worldVersion)
            parentWorldVersion = coordinator.GetComponent<TransformComponent>(parent).worldVersion;
        } else {
            parent = INVALID_ENTITY; // 父实体无 Transform:退化为根
        }
    }

    // 版本组合 = 父世界版本 + 本地修改版本;两者任一变化都会触发重算
    const uint64_t expectedVersion = parentWorldVersion + t.localVersion;
    if (t.worldCacheValid && t.cachedParentEntity == parent && t.cachedWorldVersion == expectedVersion) {
        return t.cachedWorldMatrix; // 缓存命中
    }

    // 重算并写回缓存
    glm::mat4 localMatrix = t.GetModelMatrix();
    glm::mat4 worldMatrix = localMatrix;
    if (parent != INVALID_ENTITY) {
        worldMatrix = coordinator.GetComponent<TransformComponent>(parent).cachedWorldMatrix * localMatrix;
    }
    t.cachedWorldMatrix = worldMatrix;
    t.cachedWorldVersion = expectedVersion;
    t.cachedParentEntity = parent;
    t.worldVersion = expectedVersion; // 供子节点判断父是否变化
    t.worldCacheValid = true;
    return worldMatrix;
}

glm::vec3 SceneECS::GetWorldPosition(Entity entity) {
    glm::mat4 worldMatrix = GetWorldMatrix(entity);
    return glm::vec3(worldMatrix[3]);
}

const std::vector<Entity>& SceneECS::GetRootEntities() {
    // 根实体缓存：仅在创建/层级变更（MarkRootsDirty）后重建，避免每帧遍历全部实体槽
    if (m_RootsDirty) {
        auto& coordinator = Coordinator::GetInstance();
        m_RootEntitiesCache.clear();

        // 遍历所有实体，找出没有父对象的
        for (Entity entity = 0; entity < MAX_ENTITIES; ++entity) {
            // 由 EntityManager 统一判断存活，不再把 NameComponent 当作实体哨兵。
            if (!coordinator.IsAlive(entity)) {
                continue;
            }
            
            if (coordinator.HasComponent<HierarchyComponent>(entity)) {
                if (coordinator.GetComponent<HierarchyComponent>(entity).parent == INVALID_ENTITY) {
                    m_RootEntitiesCache.push_back(entity);
                }
            }
        }
        m_RootsDirty = false;
    }

    return m_RootEntitiesCache;
}

const std::vector<Entity>& SceneECS::GetHierarchyEntities() {
    if (!m_HierarchyEntitiesDirty) {
        return m_HierarchyEntitiesCache;
    }

    m_HierarchyEntitiesCache.clear();
    const auto& roots = GetRootEntities();
    m_HierarchyEntitiesCache.reserve(roots.size());

    // 迭代式先序遍历：只在层级/实体集合变化后执行一次，避免递归收集器
    // 对同一棵场景树分别执行模型、体素、相机和灯光 DFS。
    std::vector<Entity> pending;
    pending.reserve(roots.size());
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        pending.push_back(*it);
    }

    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        m_HierarchyEntitiesCache.push_back(entity);

        const auto& children = GetChildren(entity);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            pending.push_back(*it);
        }
    }

    m_HierarchyEntitiesDirty = false;
    return m_HierarchyEntitiesCache;
}

std::vector<Entity> SceneECS::QueryByName(const std::string& name) {
    auto& coordinator = Coordinator::GetInstance();
    std::vector<Entity> results;

    // 场景实体都通过 SceneECS 创建并挂有 HierarchyComponent。使用已缓存的
    // 存活实体列表，把原来的 O(MAX_ENTITIES) 查询降为 O(live entities)，
    // 同时避免访问已销毁槽位。
    for (Entity entity : GetHierarchyEntities()) {
        if (coordinator.HasComponent<NameComponent>(entity)) {
            if (coordinator.GetComponent<NameComponent>(entity).name == name) {
                results.push_back(entity);
            }
        }
    }

    return results;
}

Entity SceneECS::FindByName(const std::string& name) {
    auto results = QueryByName(name);
    return results.empty() ? INVALID_ENTITY : results[0];
}

void SceneECS::Update(float deltaTime) {
    // 更新所有系统
    if (m_RenderSystem) {
        m_RenderSystem->Update(deltaTime);
    }
}

void SceneECS::Render(VkCommandBuffer commandBuffer) {
    if (m_RenderSystem) {
        auto renderSystem = std::dynamic_pointer_cast<RenderSystem>(m_RenderSystem);
        if (renderSystem) {
            renderSystem->Render(commandBuffer);
        }
    }
}

} // namespace ECS
