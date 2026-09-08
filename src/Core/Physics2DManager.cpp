// Physics2DManager.cpp - 2D 物理管理器(Box2D 3.x 句柄式)
#include "Core/Physics2DManager.h"
#include "box2d/box2d.h"

#include <iostream>
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

} // namespace

void Physics2DManager::Initialize() {
    if (m_initialized) return;
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = { 0.0f, 50.0f }; // 像素/s²(0.5 m/s² @ 100px/m; 轻重力, 跳跃高/下落慢)
    b2WorldId* world = GetWorldIdPtr();
    *world = b2CreateWorld(&def);
    m_initialized = b2World_IsValid(*world);
    std::cout << "[Physics2D] Box2D world initialized (gravity " << GetGravity().x
              << ", " << GetGravity().y << ")" << std::endl;
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

b2WorldId* Physics2DManager::GetWorldIdPtr() {
    return reinterpret_cast<b2WorldId*>(m_worldStorage);
}
