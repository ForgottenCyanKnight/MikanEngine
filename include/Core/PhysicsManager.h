#pragma once
#include "Platform/Export.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Physics {

// 物理管理器类，封装JoltPhysics功能
class MIKAN_API PhysicsManager {
public:
    PhysicsManager();
    ~PhysicsManager();
    
    void Initialize();    void Shutdown();
    
    void Update(float deltaTime);
    
    // 带相机位置的更新方法，用于清理远距离刚体
    void Update(float deltaTime, const glm::vec3& cameraPos);
    
    // 刚体信息结构
    struct MIKAN_API RigidBodyInfo {
        enum class Type {
            Static,
            Dynamic,
            Kinematic
        };
        
        Type type = Type::Dynamic;
        glm::vec3 position = glm::vec3(0.0f);
        glm::vec3 rotation = glm::vec3(0.0f); // 欧拉角（度）
        glm::vec3 size = glm::vec3(1.0f);
        glm::quat orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // 四元数旋转（用于 OBB）
        float mass = 1.0f;
        bool isTrigger = false;
        float restitution = 0.5f; // 弹性系数
        bool useGravity = true;   // 是否受重力影响
        
        // 碰撞体类型
        enum class ShapeType {
            Box,
            Sphere,
            Capsule,
            OBB,  // 有向包围盒
            Mesh  // 从模型生成的碰撞体
        };
        
        ShapeType shapeType = ShapeType::Box;
        
        // 从模型生成碰撞体的相关字段
        bool fromModel = false;
        std::string modelPath = "";
        float collisionPrecision = 0.01f;  // 碰撞体生成精度，值越小精度越高
        bool useConvexHull = true;  // 是否使用凸包生成碰撞体
        int maxConvexHullVertices = 256;  // 凸包最大顶点数
        bool generatePerSubmesh = false;  // 是否为每个子网格生成独立的碰撞体
    };
    
    // 创建刚体
    JPH::BodyID CreateRigidBody(const RigidBodyInfo& info);
    
    // 移除刚体
    void RemoveRigidBody(JPH::BodyID bodyID);
    
    // 获取刚体
    JPH::Body* GetRigidBody(JPH::BodyID bodyID);
    
    // 设置刚体位置
    void SetRigidBodyPosition(JPH::BodyID bodyID, const glm::vec3& position);
    
    // 设置刚体旋转
    void SetRigidBodyRotation(JPH::BodyID bodyID, const glm::vec3& eulerAngles);
    
    // 设置刚体四元数旋转（用于 OBB）
    void SetRigidBodyOrientation(JPH::BodyID bodyID, const glm::quat& orientation);
    
    // 设置刚体缩放（返回新的 BodyID，如果刚体被重建）
    // isOBB: 是否为 OBB 模式（OBB 模式下保持 BoxShape，通过刚体旋转控制方向）
    JPH::BodyID SetRigidBodyScale(JPH::BodyID bodyID, const glm::vec3& scale, bool isOBB = false);
    
    // 获取刚体位置
    glm::vec3 GetRigidBodyPosition(JPH::BodyID bodyID) const;
    
    // 获取刚体旋转（欧拉角）
    glm::vec3 GetRigidBodyRotation(JPH::BodyID bodyID) const;
    
    // 应用力到刚体
    void ApplyForce(JPH::BodyID bodyID, const glm::vec3& force);
    
    // 应用冲量到刚体
    void ApplyImpulse(JPH::BodyID bodyID, const glm::vec3& impulse);
    
    // 设置刚体速度
    void SetLinearVelocity(JPH::BodyID bodyID, const glm::vec3& velocity);
    
    // 获取刚体速度
    glm::vec3 GetLinearVelocity(JPH::BodyID bodyID) const;
    
    // 设置刚体弹性
    void SetRestitution(JPH::BodyID bodyID, float restitution);
    
    // 获取刚体弹性
    float GetRestitution(JPH::BodyID bodyID) const;

    // 设置刚体重力因子(0=不受重力,1=正常重力);即时生效,无需重建刚体
    void SetRigidBodyGravityFactor(JPH::BodyID bodyID, float factor);

    // 检查刚体是否仍存在于物理系统中(被 CleanupDistantBodies 等移除后返回 false)
    bool IsRigidBodyValid(JPH::BodyID bodyID) const;

    // 唤醒被移动/新建的静态体附近(与其形状 AABB 相交)的睡眠动态体。
    // 用途:静态体位移不会自动唤醒与其接触的睡眠动态体(Jolt 只对 Kinematic 移动做自动唤醒),
    // 导致被支撑物"悬浮"。用 BroadPhase 查询精确限定在受影响范围内,而非唤醒全部。
    void WakeBodiesNear(JPH::BodyID movedBody);
    
    // 碰撞回调接口
    class MIKAN_API CollisionListener {
    public:
        virtual ~CollisionListener() = default;
        virtual void OnCollisionEnter(JPH::BodyID body1, JPH::BodyID body2) = 0;
        virtual void OnCollisionExit(JPH::BodyID body1, JPH::BodyID body2) = 0;
        virtual void OnCollisionStay(JPH::BodyID body1, JPH::BodyID body2) = 0;
    };
    
    // 设置碰撞监听器
    void SetCollisionListener(CollisionListener* listener);
    
    // 清理远距离刚体：对距离相机过远的刚体启用休眠，清除y轴-1000以下的对象
    void CleanupDistantBodies(const glm::vec3& cameraPos);
    
    // 碰撞层定义（公开以便在内部类中使用）
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer TRIGGER = 2;
    
private:
    JPH::PhysicsSystem* physicsSystem;
    JPH::JobSystemThreadPool* jobSystem;
    JPH::TempAllocatorImpl* tempAllocator;
    
    // 碰撞层接口（必须在 PhysicsSystem 生命周期内保持有效）
    JPH::BroadPhaseLayerInterface* broadPhaseLayerInterface;
    JPH::ObjectVsBroadPhaseLayerFilter* objectVsBroadPhaseLayerFilter;
    JPH::ObjectLayerPairFilter* objectLayerPairFilter;
    
    // 碰撞监听器
    CollisionListener* collisionListener = nullptr;
};

} // namespace Physics
