#include "Animation/MMD/Physics/MMDPhysics.h"
#include "Animation/MMD/Model/MMDModel.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <algorithm>
#include <cmath>

namespace mmd {
namespace {
JPH::Vec3 V(const glm::vec3& p) { return {p.x,p.y,p.z}; }
JPH::RVec3 P(const glm::vec3& p) { return {p.x,p.y,p.z}; }
JPH::Quat Q(const glm::quat& q) { return {q.x,q.y,q.z,q.w}; }
glm::vec3 G(JPH::Vec3Arg v) { return {v.GetX(),v.GetY(),v.GetZ()}; }
glm::mat4 Transform(JPH::RVec3Arg p, JPH::QuatArg q) {
    return glm::translate(glm::mat4(1),glm::vec3(p.GetX(),p.GetY(),p.GetZ())) *
        glm::mat4_cast(glm::quat(q.GetW(),q.GetX(),q.GetY(),q.GetZ()));
}
glm::mat4 PMXTransform(const glm::vec3& p,const glm::vec3& r) {
    return glm::translate(glm::mat4(1),glm::vec3(p.x,p.y,-p.z)) *
        glm::rotate(glm::mat4(1),-r.y,glm::vec3(0,1,0)) * glm::rotate(glm::mat4(1),-r.x,glm::vec3(1,0,0)) * glm::rotate(glm::mat4(1),r.z,glm::vec3(0,0,1));
}
glm::mat4 PMXJointTransform(const glm::vec3& p,const glm::vec3& r) {
    // Saba's joints use btMatrix3x3::setEulerZYX; bodies use Y-X-Z instead.
    return glm::translate(glm::mat4(1),glm::vec3(p.x,p.y,-p.z)) *
        glm::rotate(glm::mat4(1),r.z,glm::vec3(0,0,1)) *
        glm::rotate(glm::mat4(1),-r.y,glm::vec3(0,1,0)) *
        glm::rotate(glm::mat4(1),-r.x,glm::vec3(1,0,0));
}
class BroadPhase final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(l); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l.GetValue()?"MMD moving":"MMD static"; }
#endif
};
class BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer l,JPH::BroadPhaseLayer b) const override { return l!=0 || b.GetValue()!=0; }
};
class PairFilter final : public JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b) const override { return a!=0 || b!=0; }
};
class PMXFilter final : public JPH::GroupFilter {
public:
    bool CanCollide(const JPH::CollisionGroup& a,const JPH::CollisionGroup& b) const override {
        if(a.GetGroupID()>15 || b.GetGroupID()>15) return true;
        // Match Saba/Bullet's raw PMX mask, in both directions.
        return (a.GetSubGroupID() & (1u << b.GetGroupID())) != 0 &&
               (b.GetSubGroupID() & (1u << a.GetGroupID())) != 0;
    }
};
class PmxContactMaterials final : public JPH::ContactListener {
public:
    static void Combine(const JPH::Body& a,const JPH::Body& b,JPH::ContactSettings& settings) {
        settings.mCombinedFriction=std::clamp(a.GetFriction()*b.GetFriction(),0.0f,10.0f);
        settings.mCombinedRestitution=a.GetRestitution()*b.GetRestitution();
    }
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings& settings) override {
        Combine(a,b,settings);
    }
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings& settings) override {
        Combine(a,b,settings);
    }
};
}
struct MMDPhysics::Impl {
    BroadPhase broad;
    BroadFilter broadFilter;
    PairFilter pairFilter;
    PmxContactMaterials contacts;
    JPH::PhysicsSystem world;
    JPH::TempAllocatorImpl allocator{16*1024*1024};
    JPH::JobSystemSingleThreaded jobs{2048};
    std::vector<MMDRigidBody*> bodies;
    float fps=120;
    double accumulator=0, droppedTime=0;
    uint64_t simulationSteps=0;
    int maxSteps=10, lastSteps=0;
};
MMDPhysics::MMDPhysics() = default;
MMDPhysics::~MMDPhysics() { Destroy(); }
bool MMDPhysics::Create() {
    Destroy();
    if (!JPH::Factory::sInstance) return false;
    m_impl=std::make_unique<Impl>();
    m_impl->world.Init(8192,0,16384,8192,m_impl->broad,m_impl->broadFilter,m_impl->pairFilter);
    m_impl->world.SetGravity(JPH::Vec3(0,-98.0f,0));
    m_impl->world.SetContactListener(&m_impl->contacts);
    auto settings=m_impl->world.GetPhysicsSettings();
    settings.mNumVelocitySteps=10;settings.mNumPositionSteps=2;
    settings.mMinVelocityForRestitution=0.2f;
    m_impl->world.SetPhysicsSettings(settings);
    // Source-project ground, in model coordinates. No scene-world scale leaks.
    JPH::BodyCreationSettings ground(new JPH::BoxShape(JPH::Vec3(10000,1,10000)),
        JPH::RVec3(0,-1,0),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
    ground.mFriction=0.5f;
    m_impl->world.GetBodyInterface().CreateAndAddBody(ground,JPH::EActivation::DontActivate);
    return true;
}
void MMDPhysics::Destroy() { m_impl.reset(); }
JPH::PhysicsSystem* MMDPhysics::GetDynamicsWorld() const { return m_impl ? &m_impl->world : nullptr; }
void MMDPhysics::SetFPS(float f) { if(m_impl && std::isfinite(f) && f>0) m_impl->fps=f; }
float MMDPhysics::GetFPS() const { return m_impl ? m_impl->fps : 120; }
float MMDPhysics::GetStep() const { return 1/GetFPS(); }
void MMDPhysics::GetBodyTransforms(std::vector<glm::mat4>& out) const {
    out.clear();if(!m_impl) return;
    for(auto* body:m_impl->bodies) out.push_back(body->GetTransform());
}
void MMDPhysics::SetMaxSubStepCount(int n) { if(m_impl) m_impl->maxSteps=std::clamp(n,1,120); }
int MMDPhysics::GetMaxSubStepCount() const { return m_impl ? m_impl->maxSteps : 10; }
int MMDPhysics::GetLastSubStepCount() const { return m_impl?m_impl->lastSteps:0; }
uint64_t MMDPhysics::GetSimulationStepCount() const { return m_impl?m_impl->simulationSteps:0; }
double MMDPhysics::GetDroppedTime() const { return m_impl?m_impl->droppedTime:0; }
void MMDPhysics::Update(float dt) {
    if(!m_impl) return;
    m_impl->lastSteps=0;
    if(!std::isfinite(dt) || dt<=0) return;
    const float step=GetStep();
    m_impl->accumulator+=dt;
    const double available=std::floor((m_impl->accumulator+1e-7)/step);
    m_impl->accumulator=std::max(0.0,m_impl->accumulator-available*step);
    const int steps=static_cast<int>(std::min(available,double(m_impl->maxSteps)));
    m_impl->droppedTime+=(available-steps)*step;
    if(steps==0) return;
    // Bullet advances each kinematic target over all substeps of this update.
    for(auto* body:m_impl->bodies) body->CalcLocalTransform(steps*step);
    for(int i=0;i<steps;++i) {
        for(auto* body:m_impl->bodies) body->ApplyDamping(step);
        m_impl->world.Update(step,1,&m_impl->allocator,&m_impl->jobs);
    }
    m_impl->lastSteps=steps;m_impl->simulationSteps+=steps;
}
void MMDPhysics::ResetAccumulator() { if(m_impl) m_impl->accumulator=0; }
void MMDPhysics::AddRigidBody(MMDRigidBody* b) {
    if(!b || !b->GetRigidBody()) return;
    m_impl->bodies.push_back(b);
    m_impl->world.GetBodyInterface().AddBody(b->GetRigidBody()->GetID(),JPH::EActivation::Activate);
}
void MMDPhysics::RemoveRigidBody(MMDRigidBody* b) {
    if(!m_impl || !b || !b->GetRigidBody()) return;
    auto& bi=m_impl->world.GetBodyInterface(); const auto id=b->GetRigidBody()->GetID();
    if(bi.IsAdded(id)) bi.RemoveBody(id);
    bi.DestroyBody(id);
    std::erase(m_impl->bodies,b);
}
void MMDPhysics::AddJoint(MMDJoint* j) {
    if(!j || !j->GetConstraint()) return;
    auto* c=static_cast<JPH::TwoBodyConstraint*>(j->GetConstraint());
    m_impl->world.AddConstraint(c);
}
void MMDPhysics::RemoveJoint(MMDJoint* j) {
    if(!m_impl || !j || !j->GetConstraint()) return;
    auto* c=static_cast<JPH::TwoBodyConstraint*>(j->GetConstraint());
    m_impl->world.RemoveConstraint(c);
}
void MMDPhysics::SetVelocity(const glm::vec3& v) { for(auto b:m_impl->bodies) if(b->GetRigidBody()->IsDynamic()) m_impl->world.GetBodyInterface().SetLinearVelocity(b->GetRigidBody()->GetID(),V(v)); }
void MMDPhysics::SetAngularVelocity(const glm::vec3& v) { for(auto b:m_impl->bodies) if(b->GetRigidBody()->IsDynamic()) m_impl->world.GetBodyInterface().SetAngularVelocity(b->GetRigidBody()->GetID(),V(v)); }
void MMDPhysics::ApplyForce(const glm::vec3& v) { for(auto b:m_impl->bodies) if(b->GetRigidBody()->IsDynamic()) m_impl->world.GetBodyInterface().AddForce(b->GetRigidBody()->GetID(),V(v)); }
void MMDPhysics::ApplyTorque(const glm::vec3& v) { for(auto b:m_impl->bodies) if(b->GetRigidBody()->IsDynamic()) m_impl->world.GetBodyInterface().AddTorque(b->GetRigidBody()->GetID(),V(v)); }
MMDRigidBody::~MMDRigidBody() { Destroy(); }
bool MMDRigidBody::Create(const PMXRigidBody& p,MMDModel* model,MMDNode* node) {
    Destroy(); m_physics=model->GetMMDPhysics(); m_node=node; m_type=p.m_op;
    m_group=std::min<uint16_t>(p.m_group,15); m_mask=p.m_collisionGroup;
    m_initial=PMXTransform(p.m_translate,p.m_rotate);
    m_offset=node ? glm::inverse(node->GetGlobalTransform())*m_initial : m_initial;
    m_inverseOffset=glm::inverse(m_offset);
    m_linearDamping=std::clamp(p.m_translateDimmer,0.0f,1.0f);
    m_angularDamping=std::clamp(p.m_rotateDimmer,0.0f,1.0f);m_dampingStep=-1;
    JPH::RefConst<JPH::Shape> shape;
    const auto size=glm::max(p.m_shapeSize,glm::vec3(0.001f));
    switch(p.m_shape) {
    case PMXRigidBody::Shape::Sphere: shape=new JPH::SphereShape(size.x); break;
    case PMXRigidBody::Shape::Box: shape=new JPH::BoxShape(V(size),std::min(0.05f,glm::min(size.x,glm::min(size.y,size.z))*0.5f)); break;
    case PMXRigidBody::Shape::Capsule: shape=new JPH::CapsuleShape(size.y*0.5f,size.x); break;
    default: return false;
    }
    const bool dynamic=m_type!=PMXRigidBody::Operation::Static;
    JPH::BodyCreationSettings s(shape,P(glm::vec3(m_initial[3])),Q(glm::quat_cast(m_initial)),
        dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Kinematic,1);
    // Applied explicitly per substep using Bullet's exponential/additional damping.
    s.mLinearDamping=0;s.mAngularDamping=0;
    s.mRestitution=std::clamp(p.m_repulsion,0.0f,1.0f); s.mFriction=std::max(0.0f,p.m_friction);
    s.mAllowSleeping=false;
    if(dynamic) {
        s.mOverrideMassProperties=JPH::EOverrideMassProperties::MassAndInertiaProvided;
        const float mass=std::max(0.001f,p.m_mass);s.mMassPropertiesOverride.mMass=mass;
        glm::vec3 half=size;
        if(p.m_shape==PMXRigidBody::Shape::Capsule) half=glm::vec3(size.x,size.x+size.y*0.5f,size.x);
        glm::vec3 inertia=mass/3*glm::vec3(half.y*half.y+half.z*half.z,half.x*half.x+half.z*half.z,half.x*half.x+half.y*half.y);
        if(p.m_shape==PMXRigidBody::Shape::Sphere) inertia=glm::vec3(0.4f*mass*size.x*size.x);
        s.mMassPropertiesOverride.mInertia=JPH::Mat44::sScale(V(inertia));
        s.mMotionQuality=JPH::EMotionQuality::Discrete;
    }
    s.mCollisionGroup=JPH::CollisionGroup(new PMXFilter,m_group,m_mask);
    m_body=m_physics->GetDynamicsWorld()->GetBodyInterface().CreateBody(s);
    if(node && dynamic) node->SetPhysicsAffected(true);
    return m_body!=nullptr;
}
void MMDRigidBody::Destroy() { if(m_body && m_physics) m_physics->RemoveRigidBody(this); m_body=nullptr; m_physics=nullptr; }
void MMDRigidBody::SetActivation(bool a) { if(!m_body) return; auto& bi=m_physics->GetDynamicsWorld()->GetBodyInterface(); if(a) bi.ActivateBody(m_body->GetID()); else bi.DeactivateBody(m_body->GetID()); }
glm::mat4 MMDRigidBody::GetTransform() { return m_body?Transform(m_body->GetPosition(),m_body->GetRotation()):m_initial; }
void MMDRigidBody::ResetTransform() {
    if(!m_body) return;
    const auto t=m_node?m_node->GetGlobalTransform()*m_offset:m_initial;
    auto& bi=m_physics->GetDynamicsWorld()->GetBodyInterface();
    bi.SetPositionAndRotation(m_body->GetID(),P(glm::vec3(t[3])),Q(glm::quat_cast(t)),JPH::EActivation::Activate);
    bi.SetLinearAndAngularVelocity(m_body->GetID(),JPH::Vec3::sZero(),JPH::Vec3::sZero());
}
void MMDRigidBody::Reset() { ResetTransform(); }
void MMDRigidBody::CalcLocalTransform(float elapsed) {
    if(!m_body || !m_node || m_type!=PMXRigidBody::Operation::Static) return;
    if(elapsed<=0) elapsed=m_physics->GetStep();
    const auto t=m_node->GetGlobalTransform()*m_offset;
    m_physics->GetDynamicsWorld()->GetBodyInterface().MoveKinematic(m_body->GetID(),P(glm::vec3(t[3])),Q(glm::quat_cast(t)),elapsed);
}
void MMDRigidBody::ApplyDamping(float elapsed) {
    if(!m_body || !m_body->IsDynamic()) return;
    if(m_dampingStep!=elapsed) {
        m_linearRetention=std::pow(1-m_linearDamping,elapsed);
        m_angularRetention=std::pow(1-m_angularDamping,elapsed);m_dampingStep=elapsed;
    }
    auto linear=m_body->GetLinearVelocity()*m_linearRetention;
    auto angular=m_body->GetAngularVelocity()*m_angularRetention;
    // btRigidBody::applyDamping with Saba's additionalDamping=true defaults.
    if(linear.LengthSq()<0.01f && angular.LengthSq()<0.01f) {linear*=0.005f;angular*=0.005f;}
    auto trim=[](JPH::Vec3 velocity,float damping) {
        const float speed=velocity.Length();
        return speed<damping ? (speed>0.005f?velocity*(1-0.005f/speed):JPH::Vec3::sZero()) : velocity;
    };
    linear=trim(linear,m_linearDamping);angular=trim(angular,m_angularDamping);
    // Bullet predictUnconstraintMotion damps before the solver integrates gravity.
    m_body->SetLinearVelocity(linear);m_body->SetAngularVelocity(angular);
}
void MMDRigidBody::ReflectGlobalTransform() {
    if(!m_body || !m_node || m_type==PMXRigidBody::Operation::Static) return;
    auto t=GetTransform()*m_inverseOffset;
    if(m_type==PMXRigidBody::Operation::DynamicAndBoneMerge) t[3]=m_node->GetGlobalTransform()[3];
    m_node->SetGlobalTransform(t);m_node->UpdateChildTransform();
}
MMDJoint::~MMDJoint() { Destroy(); }
void MMDJoint::Destroy() { m_constraint=nullptr; }
bool MMDJoint::CreateJoint(const PMXJoint& p,MMDRigidBody* a,MMDRigidBody* b) {
    if(!a || !b || !a->GetRigidBody() || !b->GetRigidBody() || a==b) return false;
    if(!a->GetRigidBody()->IsDynamic() && !b->GetRigidBody()->IsDynamic()) return false;
    if(p.m_type!=PMXJoint::JointType::SpringDOF6 && p.m_type!=PMXJoint::JointType::DOF6) return false;
    JPH::SixDOFConstraintSettings s;
    s.mSpace=JPH::EConstraintSpace::WorldSpace;
    const auto t=PMXJointTransform(p.m_translate,p.m_rotate);
    s.mPosition1=s.mPosition2=P(glm::vec3(t[3]));
    s.mAxisX1=s.mAxisX2=V(glm::vec3(t[0])); s.mAxisY1=s.mAxisY2=V(glm::vec3(t[1]));
    s.mSwingType=JPH::ESwingType::Pyramid;
    for(int i=0;i<3;++i) {
        const float lo=i==2?-p.m_translateUpperLimit[i]:p.m_translateLowerLimit[i];
        const float hi=i==2?-p.m_translateLowerLimit[i]:p.m_translateUpperLimit[i];
        const auto linearAxis=static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
        const auto angularAxis=static_cast<JPH::SixDOFConstraintSettings::EAxis>(i+3);
        // Bullet/PMX use lower > upper for a free axis; Jolt uses that for fixed.
        if(p.m_translateLowerLimit[i]>p.m_translateUpperLimit[i]) s.MakeFreeAxis(linearAxis);
        else s.SetLimitedAxis(linearAxis,lo,hi);
        if(p.m_rotateLowerLimit[i]>p.m_rotateUpperLimit[i]) s.MakeFreeAxis(angularAxis);
        else s.SetLimitedAxis(angularAxis,
            i<2?-p.m_rotateUpperLimit[i]:p.m_rotateLowerLimit[i],
            i<2?-p.m_rotateLowerLimit[i]:p.m_rotateUpperLimit[i]);
        if(p.m_type==PMXJoint::JointType::SpringDOF6) for(int angular=0;angular<2;++angular) {
            const int axis=i+angular*3;
            const float stiffness=angular?p.m_springRotateFactor[i]:p.m_springTranslateFactor[i];
            const auto worldAxis=V(glm::vec3(t[i]));
            const auto inverseMass=[&](const JPH::Body* body) {
                if(!body->IsDynamic()) return 0.0f;
                return angular ? worldAxis.Dot(body->GetInverseInertia().Multiply3x3(worldAxis))
                    : body->GetMotionProperties()->GetInverseMass();
            };
            const float invMass=inverseMass(a->GetRigidBody());
            const float invMassB=inverseMass(b->GetRigidBody());
            const float effectiveMass=1.0f/std::max(1e-6f,invMass+invMassB);
            if(stiffness>0) s.mMotorSettings[axis].mSpringSettings=JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping,stiffness,2*std::sqrt(stiffness*effectiveMass));
        }
    }
    auto c=static_cast<JPH::SixDOFConstraint*>(s.Create(*a->GetRigidBody(),*b->GetRigidBody()));
    for(int i=0;i<6;++i) {
        const float k=i<3?p.m_springTranslateFactor[i]:p.m_springRotateFactor[i-3];
        if(p.m_type==PMXJoint::JointType::SpringDOF6 && k>0) c->SetMotorState(static_cast<JPH::SixDOFConstraintSettings::EAxis>(i),JPH::EMotorState::Position);
    }
    c->SetTargetPositionCS(JPH::Vec3::sZero()); c->SetTargetOrientationCS(JPH::Quat::sIdentity());
    m_constraint=c; return true;
}
}
