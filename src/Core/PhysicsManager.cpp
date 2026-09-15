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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Factory.h>
#include <cstdarg>
#include <cstdint>
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>
#include "Core/JobSystem.h"
#include "Core/ProjectManager.h"
#include "ModelLoader.h"

namespace Physics {

namespace {

// Jolt keeps its dependency/barrier graph, while actual execution is routed
// through the engine-wide scheduler. This removes the second native thread
// pool without changing Jolt's synchronization contract.
class SharedJoltJobSystem final : public JPH::JobSystemWithBarrier {
private:
    class SharedJob final : public JPH::JobSystem::Job {
    public:
        SharedJob(const char* name,
                  JPH::ColorArg color,
                  SharedJoltJobSystem* system,
                  const JPH::JobSystem::JobFunction& function,
                  JPH::uint32 dependencyCount)
            : JPH::JobSystem::Job(name, color, system, function, dependencyCount) {}
    };

public:
    explicit SharedJoltJobSystem(std::size_t workerCount)
        : JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers),
          m_MaxConcurrency(static_cast<int>(std::min<std::size_t>(
              workerCount + 1u,
              static_cast<std::size_t>(std::numeric_limits<int>::max())))) {}

    int GetMaxConcurrency() const override { return m_MaxConcurrency; }

    JPH::JobHandle CreateJob(
        const char* name,
        JPH::ColorArg color,
        const JPH::JobSystem::JobFunction& function,
        JPH::uint32 dependencyCount = 0) override
    {
        auto* job = new SharedJob(name, color, this, function, dependencyCount);
        JPH::JobHandle handle(job);
        if (dependencyCount == 0) QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(JPH::JobSystem::Job* job) override
    {
        job->AddRef();
        try {
            ::JobSystem::GetInstance().Submit([job] {
                try {
                    job->Execute();
                } catch (...) {
                    job->Release();
                    throw;
                }
                job->Release();
            });
        } catch (...) {
            job->Release();
            throw;
        }
    }

    void QueueJobs(JPH::JobSystem::Job** jobs, JPH::uint jobCount) override
    {
        for (JPH::uint index = 0; index < jobCount; ++index) {
            QueueJob(jobs[index]);
        }
    }

    void FreeJob(JPH::JobSystem::Job* job) override
    {
        delete static_cast<SharedJob*>(job);
    }

private:
    int m_MaxConcurrency = 1;
};

uint32_t BodyIDKey(const JPH::BodyID& bodyID) {
    return bodyID.GetIndexAndSequenceNumber();
}

uint64_t CollisionPairKey(const JPH::BodyID& body1, const JPH::BodyID& body2) {
    uint32_t first = BodyIDKey(body1);
    uint32_t second = BodyIDKey(body2);
    if (first > second) {
        std::swap(first, second);
    }
    return (static_cast<uint64_t>(first) << 32u) | second;
}

#include "PhysicsManagerCollisionShapes.inl"

} // namespace

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

#include "PhysicsManagerLifecycle.inl"

#include "PhysicsManagerBodies.inl"

#include "PhysicsManagerQueries.inl"

} // namespace Physics
