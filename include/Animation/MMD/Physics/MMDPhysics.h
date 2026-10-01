#pragma once
#include "Animation/MMD/Model/MMDNode.h"
#include "Animation/MMD/Model/PMXFile.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <memory>

namespace mmd {
class MMDModel;
class MMDPhysics;
class MMDRigidBody {
public:
    MMDRigidBody() = default;
    ~MMDRigidBody();
    bool Create(const PMXRigidBody&, MMDModel*, MMDNode*);
    void Destroy();
    JPH::Body* GetRigidBody() const { return m_body; }
    uint16_t GetGroup() const { return m_group; }
    uint16_t GetGroupMask() const { return m_mask; }
    void SetActivation(bool);
    void ResetTransform();
    void Reset();
    void ReflectGlobalTransform();
    void CalcLocalTransform(float elapsed = 0);
    void ApplyDamping(float elapsed);
    glm::mat4 GetTransform();
    MMDNode* GetNode() const { return m_node; }
private:
    MMDPhysics* m_physics = nullptr;
    JPH::Body* m_body = nullptr;
    MMDNode* m_node = nullptr;
    PMXRigidBody::Operation m_type = PMXRigidBody::Operation::Static;
    uint16_t m_group = 0, m_mask = 0;
    glm::mat4 m_offset{1.0f}, m_initial{1.0f};
    glm::mat4 m_inverseOffset{1.0f};
    float m_linearDamping=0, m_angularDamping=0, m_dampingStep=-1;
    float m_linearRetention=1, m_angularRetention=1;
};
class MMDJoint {
public:
    ~MMDJoint();
    bool CreateJoint(const PMXJoint&, MMDRigidBody*, MMDRigidBody*);
    void Destroy();
    JPH::Constraint* GetConstraint() const { return m_constraint; }
private:
    JPH::Ref<JPH::Constraint> m_constraint;
};
// PMX bodies use model-space MMD units, just as the source project did.
// The world borrows the engine's registered Jolt types; it never owns Factory.
class MMDPhysics {
public:
    MMDPhysics();
    ~MMDPhysics();
    bool Create();
    void Destroy();
    void SetFPS(float);
    float GetFPS() const;
    void SetMaxSubStepCount(int);
    int GetMaxSubStepCount() const;
    void Update(float);
    void ResetAccumulator();
    int GetLastSubStepCount() const;
    uint64_t GetSimulationStepCount() const;
    double GetDroppedTime() const;
    void AddRigidBody(MMDRigidBody*);
    void RemoveRigidBody(MMDRigidBody*);
    void AddJoint(MMDJoint*);
    void RemoveJoint(MMDJoint*);
    void SetVelocity(const glm::vec3&);
    void SetAngularVelocity(const glm::vec3&);
    void ApplyForce(const glm::vec3&);
    void ApplyTorque(const glm::vec3&);
    JPH::PhysicsSystem* GetDynamicsWorld() const;
    float GetStep() const;
    void GetBodyTransforms(std::vector<glm::mat4>& out) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
