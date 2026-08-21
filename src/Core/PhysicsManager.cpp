#include "PhysicsManager.h"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Factory.h>
#include <thread>
#include <cstdarg>
#include <cstdint>
#include <glm/gtc/quaternion.hpp>
#include "ModelLoader.h"

namespace Physics {

// JoltPhysics trace callback function
void JoltTrace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// 自定义 ObjectLayerPairFilter - 优化：减少不必要的碰撞检测
class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        // 使用 PhysicsManager 中定义的碰撞层常量
        // 触发器不与触发器碰撞
        if (inObject1 == Physics::PhysicsManager::TRIGGER && inObject2 == Physics::PhysicsManager::TRIGGER) return false;
        
        // 静态物体之间不碰撞
        if (inObject1 == Physics::PhysicsManager::NON_MOVING && inObject2 == Physics::PhysicsManager::NON_MOVING) return false;
        
        // 其他情况都可以碰撞
        return true;
    }
};

// 自定义 BroadPhaseLayerInterface - 优化：支持 3 个碰撞层
class BroadPhaseLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
public:
    virtual JPH::uint GetNumBroadPhaseLayers() const override {
        return 3; // 支持 NON_MOVING, MOVING, TRIGGER
    }
    
    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        // 将 ObjectLayer 映射到 BroadPhaseLayer
        return JPH::BroadPhaseLayer(inLayer);
    }
    
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        // 使用 if-else 替代 switch，因为 BroadPhaseLayer 不能用于 switch
        if (inLayer == JPH::BroadPhaseLayer(0)) return "NON_MOVING";
        else if (inLayer == JPH::BroadPhaseLayer(1)) return "MOVING";
        else if (inLayer == JPH::BroadPhaseLayer(2)) return "TRIGGER";
        else return "UNKNOWN";
    }
#endif
};

// 自定义ObjectVsBroadPhaseLayerFilter
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        // 简单的逻辑：所有对象层都可以与所有BroadPhase层碰撞
        return true;
    }
};

PhysicsManager::PhysicsManager() : 
    physicsSystem(nullptr),
    jobSystem(nullptr),
    tempAllocator(nullptr),
    broadPhaseLayerInterface(nullptr),
    objectVsBroadPhaseLayerFilter(nullptr),
    objectLayerPairFilter(nullptr)
{
}

PhysicsManager::~PhysicsManager() {
    Shutdown();
}

// 碰撞回调实现
class JoltCollisionListener : public JPH::ContactListener {
private:
    Physics::PhysicsManager* physicsManager;
public:
    JoltCollisionListener(Physics::PhysicsManager* manager) : physicsManager(manager) {}
    
    virtual JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset, const JPH::CollideShapeResult& collisionResult) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    
    virtual void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
    
    virtual void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
    
    virtual void OnContactRemoved(const JPH::SubShapeIDPair& subShapePair) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
};

void PhysicsManager::Initialize() {
    // 初始化JoltPhysics
    JPH::RegisterDefaultAllocator();
    
    // 设置Trace回调函数
    JPH::Trace = Physics::JoltTrace;
    
    // 创建Factory
    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
    }
    
    // 注册所有类型
    JPH::RegisterTypes();
    
    // 创建临时分配器
    tempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    
    // 创建作业系统线程池 - 优化：使用所有可用的 CPU 核心
    uint32_t num_threads = std::max(1u, std::thread::hardware_concurrency());
    jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, num_threads);
    
    // 创建碰撞层接口（作为成员变量，确保在 PhysicsSystem 生命周期内保持有效）
    broadPhaseLayerInterface = new BroadPhaseLayerInterfaceImpl();
    objectVsBroadPhaseLayerFilter = new ObjectVsBroadPhaseLayerFilterImpl();
    objectLayerPairFilter = new ObjectLayerPairFilterImpl();
    
    // 创建物理系统
    physicsSystem = new JPH::PhysicsSystem();
    
    // 初始化物理系统 - 优化：减少 maxBodyPairs 和 maxContactConstraints 以提升性能
    // 参数：maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints, broadPhaseLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter
    physicsSystem->Init(1024, 0, 512, 512, 
                        *broadPhaseLayerInterface,
                        *objectVsBroadPhaseLayerFilter,
                        *objectLayerPairFilter);
    
    // 设置碰撞监听器
    physicsSystem->SetBodyActivationListener(nullptr);
    physicsSystem->SetContactListener(nullptr);
    
    // 设置重力（向下为负 Y 轴）
    physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    
    printf("[PhysicsManager] Initialized successfully (%d threads)\n", num_threads);
}

void PhysicsManager::Shutdown() {
    if (physicsSystem) {
        delete physicsSystem;
        physicsSystem = nullptr;
    }
    
    // 删除碰撞层接口
    delete broadPhaseLayerInterface;
    broadPhaseLayerInterface = nullptr;
    
    delete objectVsBroadPhaseLayerFilter;
    objectVsBroadPhaseLayerFilter = nullptr;
    
    delete objectLayerPairFilter;
    objectLayerPairFilter = nullptr;
    
    delete jobSystem;
    jobSystem = nullptr;
    
    delete tempAllocator;
    tempAllocator = nullptr;
    
    if (JPH::Factory::sInstance) {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

void PhysicsManager::Update(float deltaTime) {
    if (!physicsSystem) return;
    
    // 优化：使用固定时间步长进行物理更新，提升稳定性和性能
    const float fixedDeltaTime = 1.0f / 60.0f; // 60Hz 固定步长
    const int collisionSteps = 1;
    
    // 累积时间
    static float timeAccumulator = 0.0f;
    timeAccumulator += deltaTime;
    
    // 限制最大累积时间，避免螺旋死亡（spiral of death）
    const float maxAccumulatorTime = 0.25f; // 最多累积 0.25 秒
    if (timeAccumulator > maxAccumulatorTime) {
        timeAccumulator = maxAccumulatorTime;
    }
    
    // 多次执行固定步长的物理更新
    while (timeAccumulator >= fixedDeltaTime) {
        physicsSystem->Update(fixedDeltaTime, collisionSteps, tempAllocator, jobSystem);
        timeAccumulator -= fixedDeltaTime;
    }
}

void PhysicsManager::Update(float deltaTime, const glm::vec3& cameraPos) {
    // 先执行物理更新
    Update(deltaTime);
    
    // 清理远距离刚体
    CleanupDistantBodies(cameraPos);
}

JPH::BodyID PhysicsManager::CreateRigidBody(const RigidBodyInfo& info) {
    if (!physicsSystem) return JPH::BodyID();
    
    // 创建碰撞形状
    JPH::ShapeRefC shape;
    switch (info.shapeType) {
    case RigidBodyInfo::ShapeType::Box:
        shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
        break;
    case RigidBodyInfo::ShapeType::Sphere:
        shape = new JPH::SphereShape(info.size.x * 0.5f);
        break;
    case RigidBodyInfo::ShapeType::Capsule:
        shape = new JPH::CapsuleShape(info.size.y * 0.5f, info.size.x * 0.5f);
        break;
    case RigidBodyInfo::ShapeType::OBB: {
        // 创建 OBB（有向包围盒）- 直接使用 BoxShape，旋转由刚体控制
        // 注意：不使用 RotatedTranslatedShape，避免双重旋转
        JPH::BoxShape* boxShape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
        shape = boxShape;
        break;
    }
    case RigidBodyInfo::ShapeType::Mesh: {
        // 从模型生成碰撞体
        if (!info.modelPath.empty()) {
            // 加载模型获取顶点数据
            ModelLoadResult result = ModelLoader::LoadModelWithTextures(info.modelPath);
            
            // 收集所有子网格的顶点
            std::vector<glm::vec3> allVertices;
            for (const auto& subMesh : result.meshData.subMeshes) {
                for (const auto& vertex : subMesh.vertices) {
                    allVertices.push_back(vertex.Position);
                }
            }
            
            if (!allVertices.empty()) {
                if (info.useConvexHull) {
                    // 使用凸包生成碰撞体
                    // 限制顶点数量
                    int maxVerts = std::min((int)allVertices.size(), info.maxConvexHullVertices);
                    
                    std::vector<JPH::Vec3> joltVertices;
                    joltVertices.reserve(maxVerts);
                    for (int i = 0; i < maxVerts; ++i) {
                        joltVertices.push_back(JPH::Vec3(allVertices[i].x, allVertices[i].y, allVertices[i].z));
                    }
                    
                    // 创建凸包形状 - 使用正确的构造函数
                    JPH::ConvexHullShapeSettings settings(joltVertices.data(), (int)joltVertices.size());
                    settings.mHullTolerance = info.collisionPrecision;
                    
                    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
                    if (shapeResult.IsValid()) {
                        shape = shapeResult.Get();
                    } else {
                        // 如果凸包生成失败，使用默认的盒子形状
                        shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
                    }
                } else {
                    // 未来可以实现非凸碰撞体的支持
                    // 暂时使用默认的盒子形状
                    shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
                }
            } else {
                // 如果没有顶点数据，使用默认的盒子形状
                shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
            }
        } else {
            // 如果没有模型路径，使用默认的盒子形状
            shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
        }
        break;
    }
    default:
        shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        break;
    }
    
    // 设置刚体属性
    JPH::EMotionType motionType = info.type == RigidBodyInfo::Type::Static ? JPH::EMotionType::Static :
                                  info.type == RigidBodyInfo::Type::Kinematic ? JPH::EMotionType::Kinematic :
                                  JPH::EMotionType::Dynamic;
    
    // 优化：使用正确的碰撞层
    JPH::ObjectLayer objectLayer = info.type == RigidBodyInfo::Type::Static ? NON_MOVING : 
                                   info.isTrigger ? TRIGGER : MOVING;
    
    // 计算旋转四元数
    glm::quat rotation;
    if (info.shapeType == RigidBodyInfo::ShapeType::OBB) {
        // OBB 模式：直接使用 orientation 作为旋转
        rotation = info.orientation;
    } else {
        // 其他模式：使用欧拉角
        rotation = glm::quat(glm::radians(info.rotation));
    }
    
    // 创建 BodyCreationSettings
    JPH::BodyCreationSettings settings(shape, 
                                       JPH::RVec3(info.position.x, info.position.y, info.position.z), 
                                       JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), 
                                       motionType, 
                                       objectLayer);
    
    settings.mIsSensor = info.isTrigger;
    
    // 优化：使用 Discrete MotionQuality 以获得最佳性能
    settings.mMotionQuality = JPH::EMotionQuality::Discrete;
    
    // 优化：启用刚体休眠以减少不必要的计算（Kinematic 不允许休眠）
    if (motionType != JPH::EMotionType::Kinematic) {
        settings.mAllowSleeping = true;
    } else {
        settings.mAllowSleeping = false;  // Kinematic 刚体不能休眠
    }
    
    // 设置摩擦力和弹性
    settings.mFriction = 0.5f; // 摩擦系数
    settings.mRestitution = info.restitution; // 弹性系数
    
    // 设置重力因子（0=不受重力影响，1=正常重力）
    settings.mGravityFactor = info.useGravity ? 1.0f : 0.0f;
    
    // 设置质量属性
    if (motionType == JPH::EMotionType::Dynamic) {
        // 动态刚体：设置质量
        settings.mMassPropertiesOverride.mMass = info.mass;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    } else if (motionType == JPH::EMotionType::Kinematic) {
        // 运动学刚体：设置一个合理的质量（用于碰撞计算）
        settings.mMassPropertiesOverride.mMass = 1000.0f;  // 大质量，不易被撞飞
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    }
    
    // 创建并添加刚体
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);

    // 静态体创建后局部唤醒附近动态体:新出现的静态体若支撑着睡眠动态体,接触检测需要重新建立
    if (motionType == JPH::EMotionType::Static) {
        WakeBodiesNear(bodyID);
    }

    return bodyID;
}

void PhysicsManager::RemoveRigidBody(JPH::BodyID bodyID) {
    if (physicsSystem) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.RemoveBody(bodyID);
        bodyInterface.DestroyBody(bodyID);
    }
}

JPH::Body* PhysicsManager::GetRigidBody(JPH::BodyID bodyID) {
    if (!physicsSystem) return nullptr;
    // 注意：JoltPhysics不直接通过BodyInterface获取Body指针
    // 需要使用BodyLock来安全访问
    return nullptr;
}

void PhysicsManager::SetRigidBodyPosition(JPH::BodyID bodyID, const glm::vec3& position) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // 设置位置
        bodyInterface.SetPosition(bodyID, JPH::RVec3(position.x, position.y, position.z), JPH::EActivation::Activate);

        // 移动静态体会使原本被它支撑的睡眠动态体失去接触而不自知(悬浮),局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

void PhysicsManager::SetRigidBodyRotation(JPH::BodyID bodyID, const glm::vec3& eulerAngles) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        glm::quat rotation = glm::quat(glm::radians(eulerAngles));
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // 设置旋转
        bodyInterface.SetRotation(bodyID, JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), JPH::EActivation::Activate);

        // 旋转静态体同样可能改变支撑关系,局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

void PhysicsManager::SetRigidBodyOrientation(JPH::BodyID bodyID, const glm::quat& orientation) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // Jolt 的 Quat 构造函数参数顺序是 (x, y, z, w)
        bodyInterface.SetRotation(bodyID, JPH::Quat(orientation.x, orientation.y, orientation.z, orientation.w), JPH::EActivation::Activate);

        // 旋转静态体可能改变支撑关系,局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

JPH::BodyID PhysicsManager::SetRigidBodyScale(JPH::BodyID bodyID, const glm::vec3& scale, bool isOBB) {
    if (!physicsSystem || bodyID.IsInvalid()) {
        return bodyID;
    }
    
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    
    // 检查刚体是否存在
    if (!bodyInterface.IsAdded(bodyID)) {
        return bodyID;
    }
    
    // 获取当前的 Shape
    const JPH::Shape* currentShape = bodyInterface.GetShape(bodyID);
    if (!currentShape) {
        return bodyID;
    }
    
    // 获取当前的位置和旋转
    JPH::RVec3 position = bodyInterface.GetPosition(bodyID);
    JPH::Quat rotation = bodyInterface.GetRotation(bodyID);
    
    // 获取运动类型和对象层
    JPH::EMotionType motionType = bodyInterface.GetMotionType(bodyID);
    JPH::ObjectLayer objectLayer = bodyInterface.GetObjectLayer(bodyID);
    
    // 获取当前是否为传感器
    bool isSensor = bodyInterface.IsSensor(bodyID);
    
    // 检查当前 Shape 是否已经是 ScaledShape
    const JPH::ScaledShape* scaledShape = dynamic_cast<const JPH::ScaledShape*>(currentShape);
    const JPH::Shape* innerShape = scaledShape ? scaledShape->GetInnerShape() : currentShape;
    
    // 创建新的 ScaledShape（如果缩放不是单位缩放）
    JPH::ShapeRefC newShape;
    if (scale != glm::vec3(1.0f)) {
        JPH::Vec3 joltScale(scale.x, scale.y, scale.z);
        newShape = new JPH::ScaledShape(innerShape, joltScale);
    } else {
        // 如果缩放是单位缩放，直接使用内部 Shape
        newShape = innerShape;
    }
    
    // 更新刚体的 Shape（不重建刚体）
    bodyInterface.SetShape(bodyID, newShape, true, JPH::EActivation::Activate);

    // 静态体形状/缩放变化可能改变支撑关系,局部唤醒附近动态体
    if (motionType == JPH::EMotionType::Static) {
        WakeBodiesNear(bodyID);
    }
    
    // 返回原来的 BodyID（不需要改变）
    return bodyID;
}

glm::vec3 PhysicsManager::GetRigidBodyPosition(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
        return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    }
    return glm::vec3(0.0f);
}

glm::vec3 PhysicsManager::GetRigidBodyRotation(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::Quat rot = bodyInterface.GetRotation(bodyID);
        glm::quat glmRot(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
        return glm::degrees(glm::eulerAngles(glmRot));
    }
    return glm::vec3(0.0f);
}

void PhysicsManager::ApplyForce(JPH::BodyID bodyID, const glm::vec3& force) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.AddForce(bodyID, JPH::Vec3(force.x, force.y, force.z));
    }
}

void PhysicsManager::ApplyImpulse(JPH::BodyID bodyID, const glm::vec3& impulse) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.AddImpulse(bodyID, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }
}

void PhysicsManager::SetLinearVelocity(JPH::BodyID bodyID, const glm::vec3& velocity) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetLinearVelocity(bodyID, JPH::Vec3(velocity.x, velocity.y, velocity.z));
    }
}

glm::vec3 PhysicsManager::GetLinearVelocity(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::Vec3 vel = bodyInterface.GetLinearVelocity(bodyID);
        return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
    }
    return glm::vec3(0.0f);
}

void PhysicsManager::SetRestitution(JPH::BodyID bodyID, float restitution) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetRestitution(bodyID, restitution);
    }
}

float PhysicsManager::GetRestitution(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        return bodyInterface.GetRestitution(bodyID);
    }
    return 0.0f;
}

void PhysicsManager::SetRigidBodyGravityFactor(JPH::BodyID bodyID, float factor) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetGravityFactor(bodyID, factor);
    }
}

bool PhysicsManager::IsRigidBodyValid(JPH::BodyID bodyID) const {
    if (!physicsSystem || bodyID.IsInvalid()) return false;
    JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
    return bodyInterface.IsAdded(bodyID);
}

// 收集与查询 AABB 相交的 body(用于静态体移动后的局部唤醒)
class WakeBodyCollector : public JPH::CollideShapeBodyCollector {
public:
    void AddHit(const JPH::BodyID& inBodyID) override {
        if (mCount < 1024) {
            mBodies[mCount++] = inBodyID;
        }
    }
    JPH::BodyID mBodies[1024];
    int mCount = 0;
};

void PhysicsManager::WakeBodiesNear(JPH::BodyID movedBody) {
    if (!physicsSystem || movedBody.IsInvalid()) return;
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    if (!bodyInterface.IsAdded(movedBody)) return;

    // 被移动静态体的世界 AABB,略微膨胀以覆盖紧贴的接触体
    const JPH::Shape* shape = bodyInterface.GetShape(movedBody);
    if (!shape) return;
    JPH::AABox worldBounds = shape->GetLocalBounds().Transformed(bodyInterface.GetCenterOfMassTransform(movedBody));
    worldBounds.ExpandBy(JPH::Vec3::sReplicate(0.1f));

    // 只查询与该 AABB 相交的 body(空间索引,不是全遍历)
    WakeBodyCollector collector;
    physicsSystem->GetBroadPhaseQuery().CollideAABox(worldBounds, collector);

    // 唤醒命中的睡眠动态体
    for (int i = 0; i < collector.mCount; ++i) {
        const JPH::BodyID id = collector.mBodies[i];
        if (id == movedBody) continue;
        if (bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;
        if (!bodyInterface.IsActive(id)) {
            bodyInterface.ActivateBody(id);
        }
    }
}

void PhysicsManager::SetCollisionListener(CollisionListener* listener) {
    collisionListener = listener;
}

void PhysicsManager::CleanupDistantBodies(const glm::vec3& cameraPos) {
    if (!physicsSystem) return;
    
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    
    // 遍历所有刚体
    JPH::BodyIDVector bodyIDs;
    physicsSystem->GetBodies(bodyIDs);
    
    std::vector<JPH::BodyID> bodiesToRemove;
    int bodiesPutToSleep = 0;
    
    for (JPH::BodyID bodyID : bodyIDs) {
        if (bodyID.IsInvalid()) continue;
        
        // 获取刚体位置
        JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
        glm::vec3 bodyPos(pos.GetX(), pos.GetY(), pos.GetZ());
        
        // 检查是否在y轴-1000以下
        if (bodyPos.y < -1000.0f) {
            bodiesToRemove.push_back(bodyID);
            continue;
        }
        
        // 计算到相机的距离
        float distance = glm::distance(bodyPos, cameraPos);
        
        // 如果距离超过阈值（例如500单位），启用休眠
        const float sleepDistanceThreshold = 500.0f;
        if (distance > sleepDistanceThreshold) {
            // 获取刚体的运动类型
            JPH::EMotionType motionType = bodyInterface.GetMotionType(bodyID);
            
            // 只有动态刚体可以休眠
            if (motionType == JPH::EMotionType::Dynamic) {
                // 检查刚体是否已经休眠
                if (!bodyInterface.IsActive(bodyID)) {
                    // 已经休眠，跳过
                    continue;
                }
                
                // 让刚体进入休眠状态
                bodyInterface.DeactivateBody(bodyID);
                bodiesPutToSleep++;
            }
        }
        
        // 额外的清除条件：距离相机过远的刚体也直接移除
        const float removeDistanceThreshold = 1000.0f;
        if (distance > removeDistanceThreshold) {
            bodiesToRemove.push_back(bodyID);
        }
    }
    
    // 移除y轴-1000以下和距离过远的刚体
    for (JPH::BodyID bodyID : bodiesToRemove) {
        RemoveRigidBody(bodyID);
    }
    
    if (!bodiesToRemove.empty()) {
        printf("[PhysicsManager] Removed %d bodies (below y=-1000 or too far)\n", (int)bodiesToRemove.size());
    }
    
    if (bodiesPutToSleep > 0) {
        printf("[PhysicsManager] Put %d bodies to sleep\n", bodiesPutToSleep);
    }
}

} // namespace Physics
