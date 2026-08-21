// Physics2DManager.cpp - 2D 物理管理器(Box2D 3.x 句柄式)
#include "Core/Physics2DManager.h"
#include "box2d/box2d.h"

#include <iostream>
#include <cstring>

Physics2DManager& Physics2DManager::GetInstance() {
    static Physics2DManager instance;
    return instance;
}

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

b2WorldId* Physics2DManager::GetWorldIdPtr() {
    return reinterpret_cast<b2WorldId*>(m_worldStorage);
}
