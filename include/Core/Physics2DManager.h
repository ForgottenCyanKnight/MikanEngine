#pragma once
// Physics2DManager.h - 2D 物理管理器(Box2D 3.x 句柄式 API 封装)
// 封装 b2World 生命周期与 Step;坐标约定: b2Vec2 = 画布坐标(左下原点, y 向下, 像素),
// 与 Canvas2D/TransformComponent 直接映射(Physics2DSystem 负责 米↔像素 换算)。
// 刚体/碰撞体由 ECS 组件(RigidBody2DComponent/Collider2DComponent)经 Physics2DSystem 驱动。
#include "Platform/Export.h"
#include <glm/glm.hpp>

struct b2WorldId;

class MIKAN_API Physics2DManager {
public:
    static Physics2DManager& GetInstance();

    // 创建 b2World(默认重力 (0, 980) 像素/s², 约 9.8m/s² @ 100px/m)
    void Initialize();
    void Shutdown();
    bool IsInitialized() const;

    // 推进物理
    void Update(float deltaTime);

    void SetGravity(glm::vec2 gravity);
    glm::vec2 GetGravity() const;

    // 暴露 b2WorldId(供 Physics2DSystem 创建/销毁刚体)
    b2WorldId* GetWorldIdPtr();

private:
    Physics2DManager() = default;
    ~Physics2DManager() = default;
    Physics2DManager(const Physics2DManager&) = delete;
    Physics2DManager& operator=(const Physics2DManager&) = delete;

    void* m_worldStorage[8] = {}; // 容纳 b2WorldId(值类型, 避免头文件依赖 box2d)
    bool m_initialized = false;
};
