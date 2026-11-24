// Physics2DManager.cpp - 2D 物理管理器(Box2D 3.x 句柄式)
#include "Core/Log.h"
#include "Core/LogStream.h"
#include "Core/Physics2DManager.h"
#include "box2d/box2d.h"

#include <cmath>
#include <cstring>

Physics2DManager& Physics2DManager::GetInstance() {
    static Physics2DManager instance;
    return instance;
}

namespace {

Physics2DManager::BodyHandle EncodeBodyId(b2BodyId id) {
    Physics2DManager::BodyHandle handle = 0;
    static_assert(sizeof(handle) >= sizeof(id), "BodyHandle must hold b2BodyId");
    std::memcpy(&handle, &id, sizeof(id));
    return handle;
}

b2BodyId DecodeBodyId(Physics2DManager::BodyHandle handle) {
    b2BodyId id{};
    static_assert(sizeof(handle) >= sizeof(id), "BodyHandle must hold b2BodyId");
    std::memcpy(&id, &handle, sizeof(id));
    return id;
}

Physics2DManager::JointHandle EncodeJointId(b2JointId id) {
    Physics2DManager::JointHandle handle = 0;
    static_assert(sizeof(handle) >= sizeof(id), "JointHandle must hold b2JointId");
    std::memcpy(&handle, &id, sizeof(id));
    return handle;
}

b2JointId DecodeJointId(Physics2DManager::JointHandle handle) {
    b2JointId id{};
    static_assert(sizeof(handle) >= sizeof(id), "JointHandle must hold b2JointId");
    std::memcpy(&id, &handle, sizeof(id));
    return id;
}

// 世界锚点 → 体 local 锚点(def 需要局部坐标)
b2Vec2 WorldToLocal(b2BodyId body, glm::vec2 worldPoint) {
    const b2Vec2 local = b2Body_GetLocalPoint(body, { worldPoint.x, worldPoint.y });
    return { local.x, local.y };
}

} // namespace

void Physics2DManager::Initialize() {
    if (m_initialized) return;
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = { 0.0f, 50.0f }; // 像素/s²(0.5 m/s² @ 100px/m; 轻重力, 跳跃高/下落慢)
    b2WorldId* world = GetWorldIdPtr();
    *world = b2CreateWorld(&def);
    m_initialized = b2World_IsValid(*world);
    LOGSTREAM(Info) << "[Physics2D] Box2D world initialized (gravity " << GetGravity().x
              << ", " << GetGravity().y << ")";
}

void Physics2DManager::Shutdown() {
    if (!m_initialized) return;
    b2WorldId* world = GetWorldIdPtr();
    b2DestroyWorld(*world);
    std::memset(m_worldStorage, 0, sizeof(m_worldStorage));
    m_initialized = false;
}

bool Physics2DManager::IsInitialized() const {
    return m_initialized;
}

void Physics2DManager::Update(float deltaTime) {
    if (!m_initialized) return;
    b2World_Step(*GetWorldIdPtr(), deltaTime, 4);
}

void Physics2DManager::SetGravity(glm::vec2 gravity) {
    if (m_initialized) b2World_SetGravity(*GetWorldIdPtr(), { gravity.x, gravity.y });
}

glm::vec2 Physics2DManager::GetGravity() const {
    if (!m_initialized) return glm::vec2(0.0f, 980.0f);
    b2Vec2 g = b2World_GetGravity(*reinterpret_cast<const b2WorldId*>(m_worldStorage));
    return glm::vec2(g.x, g.y);
}

Physics2DManager::BodyHandle Physics2DManager::CreateBoxBody(
    glm::vec2 centerMeters,
    glm::vec2 halfSizeMeters,
    bool dynamic,
    float density,
    float friction,
    std::uintptr_t userData) {
    if (!m_initialized) return 0;

    const b2WorldId world = *GetWorldIdPtr();
    if (!b2World_IsValid(world)) return 0;

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type = dynamic ? b2_dynamicBody : b2_staticBody;
    bodyDef.position = { centerMeters.x, centerMeters.y };
    bodyDef.userData = reinterpret_cast<void*>(userData);
    const b2BodyId body = b2CreateBody(world, &bodyDef);
    if (!b2Body_IsValid(body)) return 0;

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.density = density;
    shapeDef.friction = friction;
    const b2Polygon polygon = b2MakeBox(halfSizeMeters.x, halfSizeMeters.y);
    b2CreatePolygonShape(body, &shapeDef, &polygon);
    return EncodeBodyId(body);
}

bool Physics2DManager::IsBodyValid(BodyHandle body) const {
    return m_initialized && body != 0 && b2Body_IsValid(DecodeBodyId(body));
}

glm::vec2 Physics2DManager::GetBodyPosition(BodyHandle body) const {
    if (!IsBodyValid(body)) return {};
    const b2Vec2 position = b2Body_GetPosition(DecodeBodyId(body));
    return { position.x, position.y };
}

void Physics2DManager::SetBodyTransform(BodyHandle body, glm::vec2 positionMeters) {
    if (!IsBodyValid(body)) return;
    b2Body_SetTransform(DecodeBodyId(body), { positionMeters.x, positionMeters.y }, b2Rot_identity);
}

void Physics2DManager::SetBodyLinearVelocity(BodyHandle body, glm::vec2 velocityMetersPerSecond) {
    if (!IsBodyValid(body)) return;
    b2Body_SetLinearVelocity(DecodeBodyId(body), { velocityMetersPerSecond.x, velocityMetersPerSecond.y });
}

void Physics2DManager::SetBodyAngularVelocity(BodyHandle body, float angularVelocity) {
    if (!IsBodyValid(body)) return;
    b2Body_SetAngularVelocity(DecodeBodyId(body), angularVelocity);
}

void Physics2DManager::ApplyForceToCenter(BodyHandle body, glm::vec2 force, bool wake) {
    if (!IsBodyValid(body)) return;
    b2Body_ApplyForceToCenter(DecodeBodyId(body), { force.x, force.y }, wake);
}

void Physics2DManager::ApplyLinearImpulseToCenter(BodyHandle body, glm::vec2 impulse, bool wake) {
    if (!IsBodyValid(body)) return;
    b2Body_ApplyLinearImpulseToCenter(DecodeBodyId(body), { impulse.x, impulse.y }, wake);
}

float Physics2DManager::GetBodyMass(BodyHandle body) const {
    if (!IsBodyValid(body)) return 0.0f;
    return b2Body_GetMass(DecodeBodyId(body));
}

int Physics2DManager::GetBodyContactCapacity(BodyHandle body) const {
    if (!IsBodyValid(body)) return 0;
    return b2Body_GetContactCapacity(DecodeBodyId(body));
}

glm::vec2 Physics2DManager::GetBodyLinearVelocity(BodyHandle body) const {
    if (!IsBodyValid(body)) return {};
    const b2Vec2 v = b2Body_GetLinearVelocity(DecodeBodyId(body));
    return { v.x, v.y };
}

bool Physics2DManager::IsBodyAwake(BodyHandle body) const {
    if (!IsBodyValid(body)) return false;
    return b2Body_IsAwake(DecodeBodyId(body));
}

void Physics2DManager::WakeBody(BodyHandle body) {
    if (!IsBodyValid(body)) return;
    b2Body_SetAwake(DecodeBodyId(body), true);
}

// ===== 关节 =====

Physics2DManager::JointHandle Physics2DManager::CreateDistanceJoint(
    BodyHandle a, BodyHandle b,
    glm::vec2 worldAnchorA, glm::vec2 worldAnchorB,
    float length, float hertz, float dampingRatio, bool collideConnected) {
    if (!IsBodyValid(a) || !IsBodyValid(b)) return 0;
    const b2BodyId bodyA = DecodeBodyId(a);
    const b2BodyId bodyB = DecodeBodyId(b);
    b2DistanceJointDef def = b2DefaultDistanceJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = WorldToLocal(bodyA, worldAnchorA);
    def.localAnchorB = WorldToLocal(bodyB, worldAnchorB);
    def.length = length;
    if (hertz > 0.0f) {
        def.enableSpring = true;
        def.hertz = hertz;
        def.dampingRatio = dampingRatio;
    }
    def.collideConnected = collideConnected;
    return EncodeJointId(b2CreateDistanceJoint(*GetWorldIdPtr(), &def));
}

Physics2DManager::JointHandle Physics2DManager::CreateRevoluteJoint(
    BodyHandle a, BodyHandle b, glm::vec2 worldAnchor,
    bool enableLimit, float lowerAngle, float upperAngle,
    bool enableMotor, float maxMotorTorque, float motorSpeed,
    bool collideConnected) {
    if (!IsBodyValid(a) || !IsBodyValid(b)) return 0;
    const b2BodyId bodyA = DecodeBodyId(a);
    const b2BodyId bodyB = DecodeBodyId(b);
    b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = WorldToLocal(bodyA, worldAnchor);
    def.localAnchorB = WorldToLocal(bodyB, worldAnchor);
    def.enableLimit = enableLimit;
    def.lowerAngle = lowerAngle;
    def.upperAngle = upperAngle;
    def.enableMotor = enableMotor;
    def.maxMotorTorque = maxMotorTorque;
    def.motorSpeed = motorSpeed;
    def.collideConnected = collideConnected;
    return EncodeJointId(b2CreateRevoluteJoint(*GetWorldIdPtr(), &def));
}

Physics2DManager::JointHandle Physics2DManager::CreateWeldJoint(
    BodyHandle a, BodyHandle b, glm::vec2 worldAnchor, bool collideConnected) {
    if (!IsBodyValid(a) || !IsBodyValid(b)) return 0;
    const b2BodyId bodyA = DecodeBodyId(a);
    const b2BodyId bodyB = DecodeBodyId(b);
    b2WeldJointDef def = b2DefaultWeldJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = WorldToLocal(bodyA, worldAnchor);
    def.localAnchorB = WorldToLocal(bodyB, worldAnchor);
    def.collideConnected = collideConnected;
    return EncodeJointId(b2CreateWeldJoint(*GetWorldIdPtr(), &def));
}

Physics2DManager::JointHandle Physics2DManager::CreatePrismaticJoint(
    BodyHandle a, BodyHandle b, glm::vec2 worldAnchor, glm::vec2 worldAxis,
    bool enableLimit, float lowerTranslation, float upperTranslation,
    bool enableMotor, float maxMotorForce, float motorSpeed,
    bool collideConnected) {
    if (!IsBodyValid(a) || !IsBodyValid(b)) return 0;
    const float axisLen = glm::length(worldAxis);
    if (axisLen < 1e-5f) return 0;
    const b2BodyId bodyA = DecodeBodyId(a);
    const b2BodyId bodyB = DecodeBodyId(b);
    b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = WorldToLocal(bodyA, worldAnchor);
    def.localAnchorB = WorldToLocal(bodyB, worldAnchor);
    // def 需要体 A 局部系下的轴: 把世界方向的两端映射到 local 再归一化
    const b2Vec2 localP0 = b2Body_GetLocalPoint(bodyA, { 0.0f, 0.0f });
    const b2Vec2 localP1 = b2Body_GetLocalPoint(bodyA, { worldAxis.x / axisLen, worldAxis.y / axisLen });
    b2Vec2 axis{ localP1.x - localP0.x, localP1.y - localP0.y };
    const float axisLocalLen = std::sqrt(axis.x * axis.x + axis.y * axis.y);
    if (axisLocalLen < 1e-5f) return 0;
    axis = { axis.x / axisLocalLen, axis.y / axisLocalLen };
    def.localAxisA = axis;
    def.enableLimit = enableLimit;
    def.lowerTranslation = lowerTranslation;
    def.upperTranslation = upperTranslation;
    def.enableMotor = enableMotor;
    def.maxMotorForce = maxMotorForce;
    def.motorSpeed = motorSpeed;
    def.collideConnected = collideConnected;
    return EncodeJointId(b2CreatePrismaticJoint(*GetWorldIdPtr(), &def));
}

Physics2DManager::JointHandle Physics2DManager::CreateMouseJoint(
    BodyHandle ground, BodyHandle moved, glm::vec2 target,
    float maxForce, float hertz, float dampingRatio) {
    if (!IsBodyValid(ground) || !IsBodyValid(moved)) return 0;
    const b2BodyId movedId = DecodeBodyId(moved);
    // 零接触/零重力下创建即睡的体不会被关节力唤醒,显式置醒
    b2Body_SetAwake(movedId, true);
    b2MouseJointDef def = b2DefaultMouseJointDef();
    def.bodyIdA = DecodeBodyId(ground);
    def.bodyIdB = movedId;
    def.target = { target.x, target.y };
    def.maxForce = maxForce;
    def.hertz = hertz;
    def.dampingRatio = dampingRatio;
    return EncodeJointId(b2CreateMouseJoint(*GetWorldIdPtr(), &def));
}

void Physics2DManager::SetMouseJointTarget(JointHandle joint, glm::vec2 target) {
    if (!IsJointValid(joint)) return;
    const b2JointId id = DecodeJointId(joint);
    b2MouseJoint_SetTarget(id, { target.x, target.y });
    // Box2D 3.x 按岛总能量睡眠: 被拖拽体静止足够久会随岛入睡, 关节不再施力。
    // 真实拖拽(持续更新目标)必须显式唤醒体 B。
    b2Body_SetAwake(b2Joint_GetBodyB(id), true);
}

void Physics2DManager::DestroyJoint(JointHandle joint) {
    if (!IsJointValid(joint)) return;
    b2DestroyJoint(DecodeJointId(joint));
}

bool Physics2DManager::IsJointValid(JointHandle joint) const {
    return m_initialized && joint != 0 && b2Joint_IsValid(DecodeJointId(joint));
}

b2WorldId* Physics2DManager::GetWorldIdPtr() {
    return reinterpret_cast<b2WorldId*>(m_worldStorage);
}
