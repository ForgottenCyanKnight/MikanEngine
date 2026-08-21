#pragma once
// Physics2DSystem.h - 2D 物理系统: 实体(RigidBody2D + Collider2D)↔ b2Body 双向同步
// 每帧: 为带 RigidBody2DComponent 的实体创建/复用 b2Body(fixture 按 Collider2D),
//       b2World::Step 后把 body 位置/角度写回 TransformComponent → 场景树渲染跟随。
// 坐标: b2Vec2 = 画布坐标(y 向下, 像素), 与 Transform.position(x,y) 直接映射。
#include "Platform/Export.h"
#include "ECS/Types.h"
#include <glm/glm.hpp>
#include <set>
#include <utility>
#include <vector>

class b2World;

class MIKAN_API Physics2DSystem {
public:
    static Physics2DSystem& GetInstance();

    void Initialize();        // Physics2DManager::Initialize(创建 b2World)
    void Update(float dt);    // 同步 + step + 写回
    void ClearBodies();       // 销毁全部 2D 刚体(场景重建/清理时调用)
    // 销毁单个实体的 b2Body(实体销毁路径挂钩;实体无刚体/世界已失效时安全无操作)
    void RemoveBody(ECS::Entity e);
    // 调试绘制: 遍历带 RigidBody2D+Collider2D 的实体, 画碰撞体线框(由 Canvas2D 世界层调用)
    void RenderDebug(class Renderer2D& r2d);

private:
    Physics2DSystem() = default;
    ~Physics2DSystem() = default;
    Physics2DSystem(const Physics2DSystem&) = delete;
    Physics2DSystem& operator=(const Physics2DSystem&) = delete;

    void VisitEntity(ECS::Entity e);
    void CreateBody(ECS::Entity e);
    void SyncTransform(ECS::Entity e);
    // 轮询 Box2D 接触/传感器事件 → 派发 Contact2DComponent 回调(Step 后、Transform 写回后调用)
    void DispatchContactEvents();
    void DispatchToEntity(ECS::Entity e, ECS::Entity other, bool enter);
    // 补偿 Box2D 3.0.x 丢失的传感器 end 事件(见 DispatchContactEvents 注释)
    bool ShapesAABBOverlap(ECS::Entity a, ECS::Entity b);

    // 上帧仍重叠的传感器实体对(归一化 a<b);用于 end 事件补偿
    std::set<std::pair<ECS::Entity, ECS::Entity>> m_ActiveSensorOverlaps;
};
