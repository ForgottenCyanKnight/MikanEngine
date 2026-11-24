#pragma once
// Physics2DManager.h - 2D 物理管理器(Box2D 3.x 句柄式 API 封装)
// 封装 b2World 生命周期与 Step;坐标约定: b2Vec2 = 画布坐标(左下原点, y 向下, 像素),
// 与 Canvas2D/TransformComponent 直接映射(Physics2DSystem 负责 米↔像素 换算)。
// 刚体/碰撞体由 ECS 组件(RigidBody2DComponent/Collider2DComponent)经 Physics2DSystem 驱动。
#include "Platform/Export.h"
#include <cstdint>
#include <glm/glm.hpp>

struct b2WorldId;

class MIKAN_API Physics2DManager {
public:
    static Physics2DManager& GetInstance();

    using BodyHandle = std::uint64_t;

    // 创建 b2World(默认重力 (0, 980) 像素/s², 约 9.8m/s² @ 100px/m)
    void Initialize();
    void Shutdown();
    bool IsInitialized() const;

    // 推进物理
    void Update(float deltaTime);

    void SetGravity(glm::vec2 gravity);
    glm::vec2 GetGravity() const;

    // 插件侧 2D 物理接口。Box2D 调用统一在 Game.dll 内完成，避免插件重复链接
    // Box2D 静态库后跨 DLL 操作同一个 world 导致运行时冲突。
    BodyHandle CreateBoxBody(glm::vec2 centerMeters,
                             glm::vec2 halfSizeMeters,
                             bool dynamic,
                             float density,
                             float friction,
                             std::uintptr_t userData);
    bool IsBodyValid(BodyHandle body) const;
    glm::vec2 GetBodyPosition(BodyHandle body) const;
    void SetBodyTransform(BodyHandle body, glm::vec2 positionMeters);
    void SetBodyLinearVelocity(BodyHandle body, glm::vec2 velocityMetersPerSecond);
    void SetBodyAngularVelocity(BodyHandle body, float angularVelocity);
    void ApplyForceToCenter(BodyHandle body, glm::vec2 force, bool wake);
    void ApplyLinearImpulseToCenter(BodyHandle body, glm::vec2 impulse, bool wake);
    float GetBodyMass(BodyHandle body) const;
    int GetBodyContactCapacity(BodyHandle body) const;
    glm::vec2 GetBodyLinearVelocity(BodyHandle body) const;
    bool IsBodyAwake(BodyHandle body) const;
    void WakeBody(BodyHandle body);

    // ===== 关节(句柄式; 锚点/轴用世界坐标传入,内部换算为 local; 单位与刚体一致) =====
    using JointHandle = std::uint64_t;

    // 距离关节: 两体锚点间保持 length(hertz<=0 刚性约束, >0 弹簧)
    JointHandle CreateDistanceJoint(BodyHandle a, BodyHandle b,
                                    glm::vec2 worldAnchorA, glm::vec2 worldAnchorB,
                                    float length, float hertz, float dampingRatio,
                                    bool collideConnected);
    // 旋转铰链: worldAnchor 处转轴; 可选角度限位(rad)与马达
    JointHandle CreateRevoluteJoint(BodyHandle a, BodyHandle b, glm::vec2 worldAnchor,
                                    bool enableLimit, float lowerAngle, float upperAngle,
                                    bool enableMotor, float maxMotorTorque, float motorSpeed,
                                    bool collideConnected);
    // 焊接(默认最大刚度, 两体刚性绑定)
    JointHandle CreateWeldJoint(BodyHandle a, BodyHandle b, glm::vec2 worldAnchor,
                                bool collideConnected);
    // 棱柱(滑轨): worldAxis 为世界空间滑动轴(自动归一化), 可选平移限位与马达
    JointHandle CreatePrismaticJoint(BodyHandle a, BodyHandle b, glm::vec2 worldAnchor,
                                     glm::vec2 worldAxis,
                                     bool enableLimit, float lowerTranslation, float upperTranslation,
                                     bool enableMotor, float maxMotorForce, float motorSpeed,
                                     bool collideConnected);
    // 鼠标关节: ground 通常为静态体, moved 被拉向 target(软约束)
    JointHandle CreateMouseJoint(BodyHandle ground, BodyHandle moved, glm::vec2 target,
                                 float maxForce, float hertz, float dampingRatio);
    // 更新鼠标关节目标点并唤醒体B(Box2D 3.x 按岛能量睡眠, 更新目标必须唤醒才生效)
    void SetMouseJointTarget(JointHandle joint, glm::vec2 target);
    void DestroyJoint(JointHandle joint);
    bool IsJointValid(JointHandle joint) const;

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
