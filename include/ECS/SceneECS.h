#pragma once
#include "Platform/Export.h"

#include "ECS/ECS.h"
#include <string>
#include <memory>

namespace ECS {

// 前向声明
class RenderSystem;

// ECS 场景管理器 - 提供高层API方便使用ECS
class MIKAN_API SceneECS {
public:
    static SceneECS& GetInstance();

    void Init();
    void Shutdown();
    void LoadDefaultScene();  // 在初始化完成后加载默认场景

    // 创建基本对象
    Entity CreateEmpty(const std::string& name = "Empty");
    Entity CreateCube(const std::string& name = "Cube");
    Entity CreateSphere(const std::string& name = "Sphere");
    Entity CreatePlane(const std::string& name = "Plane");
    Entity CreateCamera(const std::string& name = "Camera");
    Entity CreateLight(const std::string& name = "Light", LightComponent::Type type = LightComponent::Type::Directional);

    // 对象操作
    void DestroyEntity(Entity entity);
    void SetName(Entity entity, const std::string& name);
    std::string GetName(Entity entity);

    // 选中管理
    void SetSelectedEntity(Entity entity);
    Entity GetSelectedEntity() const { return m_SelectedEntity; }
    void ClearSelection();

    // 层级管理
    void SetParent(Entity child, Entity parent);
    void RemoveParent(Entity child);
    Entity GetParent(Entity entity);
    std::vector<Entity> GetChildren(Entity entity);

    // 可见性
    void SetVisible(Entity entity, bool visible);
    bool IsVisible(Entity entity);

    // 变换操作
    void SetPosition(Entity entity, const glm::vec3& position);
    glm::vec3 GetPosition(Entity entity);
    void SetRotation(Entity entity, const glm::quat& rotation);
    void SetRotationEuler(Entity entity, const glm::vec3& euler);
    glm::vec3 GetRotationEuler(Entity entity);
    void SetScale(Entity entity, const glm::vec3& scale);
    glm::vec3 GetScale(Entity entity);

    // 获取世界矩阵（考虑父对象）
    glm::mat4 GetWorldMatrix(Entity entity);
    glm::vec3 GetWorldPosition(Entity entity);

    // 获取所有根实体（没有父对象的）
    std::vector<Entity> GetRootEntities();

    // 查询实体
    std::vector<Entity> QueryByName(const std::string& name);
    Entity FindByName(const std::string& name);

    // 系统更新
    void Update(float deltaTime);
    void Render(VkCommandBuffer commandBuffer);

    // 实体集合版本(实体创建/销毁/层级变更时递增;外部缓存据此判断实体集合是否变化)
    uint32_t GetEntitySetVersion() const { return m_EntitySetVersion; }

    // 当前场景关联的游戏模块名(场景文件顶层 "game" 键;加载后由引擎自动激活)
    void SetSceneGameModule(const std::string& m) { m_SceneGameModule = m; }
    const std::string& GetSceneGameModule() const { return m_SceneGameModule; }

private:
    SceneECS() = default;
    ~SceneECS() = default;
    SceneECS(const SceneECS&) = delete;
    SceneECS& operator=(const SceneECS&) = delete;

    Entity m_SelectedEntity = INVALID_ENTITY;

    // 根实体缓存（GetRootEntities 优化：避免每帧遍历全部实体槽；创建/层级变更时置脏）
    std::vector<Entity> m_RootEntitiesCache;
    bool m_RootsDirty = true;
    void MarkRootsDirty() { m_RootsDirty = true; }

    // 实体集合版本:创建/销毁实体或层级变更时递增。
    // 供 SceneRenderer 的相机实体帧缓存判断"实体集合是否变化"——重载场景(清空+重建)
    // 会改变实体集合,缓存必须据此失效,否则缓存中会残留已销毁实体的悬垂 ID。
    uint32_t m_EntitySetVersion = 0;
    void MarkEntitySetDirty() { ++m_EntitySetVersion; }

    // 缓存系统引用
    std::shared_ptr<System> m_RenderSystem;

    // 当前场景游戏模块名(顶层 "game" 键)
    std::string m_SceneGameModule;
};

} // namespace ECS
