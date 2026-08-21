// Physics2DSystem.cpp - 2D 物理系统: 实体 ↔ b2Body 同步(Box2D 3.x 句柄式)
// 坐标换算: Box2D 世界用米, 场景用画布像素。kPixelsPerMeter = 100(1px = 0.01m),
// 否则 80px 箱子会成为 80m 巨物(质量巨大, 力推不动, 重力失控)。
// 写回时注意: Sprite2D 渲染 position 是左下角, b2Body 位置是中心 → 减半尺寸。
#include "Core/Physics2DSystem.h"
#include "Core/Physics2DManager.h"
#include "Core/RenderGlobals.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Rendering/Renderer2D.h"
#include "box2d/box2d.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <tuple>
#include <vector>

namespace {
constexpr float kPixelsPerMeter = 100.0f; // 像素/米(1px = 0.01m)

// rb.body(void*) 容纳 b2BodyId(值类型, 8 字节)
b2BodyId BodyIdOf(void* storage) {
    b2BodyId id;
    std::memcpy(&id, &storage, sizeof(id));
    return id;
}
void StoreBodyId(void*& storage, b2BodyId id) {
    std::memcpy(&storage, &id, sizeof(id));
}

// shape → 实体(b2Body 的 userData 在 CreateBody 时存了 uintptr_t 实体 id;无效/未挂实体返回 INVALID_ENTITY)
ECS::Entity EntityOfShape(b2ShapeId shapeId) {
    if (!b2Shape_IsValid(shapeId)) return ECS::INVALID_ENTITY;
    b2BodyId body = b2Shape_GetBody(shapeId);
    if (!b2Body_IsValid(body)) return ECS::INVALID_ENTITY;
    void* ud = b2Body_GetUserData(body);
    return ud ? (ECS::Entity)(uintptr_t)ud : ECS::INVALID_ENTITY;
}
}

Physics2DSystem& Physics2DSystem::GetInstance() {
    static Physics2DSystem instance;
    return instance;
}

void Physics2DSystem::Initialize() {
    Physics2DManager::GetInstance().Initialize();
}

void Physics2DSystem::VisitEntity(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::RigidBody2DComponent>(e)) {
        auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
        if (!rb.body) {
            CreateBody(e);          // 首次: 创建 b2Body
        } else {
            SyncTransform(e);       // 已有: 写回 Transform(位置/角度)
        }
    }
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (const auto& child : sceneECS.GetChildren(e)) {
        VisitEntity(child);
    }
}

void Physics2DSystem::CreateBody(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
    if (!b2World_IsValid(world)) return;

    auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);

    b2BodyDef bd = b2DefaultBodyDef();
    // 显式映射刚体类型(不可强转: 本引擎 Static=0/Dynamic=1/Kinematic=2,
    // Box2D 为 static=0/kinematic=1/dynamic=2, 强转会互换 Dynamic↔Kinematic)
    switch (rb.type) {
    case ECS::RigidBody2DComponent::Type::Static:    bd.type = b2_staticBody;    break;
    case ECS::RigidBody2DComponent::Type::Kinematic: bd.type = b2_kinematicBody; break;
    case ECS::RigidBody2DComponent::Type::Dynamic:
    default:                                         bd.type = b2_dynamicBody;    break;
    }
    bd.position = { t.position.x / kPixelsPerMeter, t.position.y / kPixelsPerMeter };
    bd.fixedRotation = rb.fixedRotation;
    bd.gravityScale = rb.gravityScale;
    glm::vec3 euler = glm::eulerAngles(t.rotation);
    bd.rotation = b2MakeRot(euler.z);
    bd.userData = (void*)(uintptr_t)e;

    b2BodyId body = b2CreateBody(world, &bd);
    StoreBodyId(rb.body, body);

    // 碰撞体(shape)
    if (coordinator.HasComponent<ECS::Collider2DComponent>(e)) {
        auto& col = coordinator.GetComponent<ECS::Collider2DComponent>(e);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = col.density;
        sd.friction = col.friction;
        sd.restitution = col.restitution;
        sd.isSensor = col.isTrigger;
        // 开启接触/传感器事件供 Contact2DComponent 回调(默认已开启, 此处显式声明意图)。
        // 事件标志由接触双方形状 OR 合并: 静态体形状的设置被 Box2D 忽略, 但静态体与
        // 动态/运动学体接触时事件照常产生, 静态体实体同样会收到回调。
        sd.enableContactEvents = true;
        sd.enableSensorEvents = true;
        if (col.shape == ECS::Collider2DComponent::Shape::Box) {
            b2Polygon poly = b2MakeBox(col.size.x * 0.5f / kPixelsPerMeter,
                                        col.size.y * 0.5f / kPixelsPerMeter);
            b2CreatePolygonShape(body, &sd, &poly);
        } else {
            b2Circle circle;
            circle.center = { col.offset.x / kPixelsPerMeter, col.offset.y / kPixelsPerMeter };
            circle.radius = col.radius / kPixelsPerMeter;
            b2CreateCircleShape(body, &sd, &circle);
        }
    }
}

void Physics2DSystem::SyncTransform(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
    b2BodyId body = BodyIdOf(rb.body);
    if (!b2Body_IsValid(body)) return;

    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
    b2Vec2 p = b2Body_GetPosition(body);
    // 米 → 画布像素
    const float px = p.x * kPixelsPerMeter;
    const float py = p.y * kPixelsPerMeter;
    // Sprite2D 渲染 position 是左下角, b2Body 位置是中心 → 减半尺寸
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(e)) {
        auto& s = coordinator.GetComponent<ECS::Sprite2DComponent>(e);
        t.position = glm::vec3(px - s.width * 0.5f, py - s.height * 0.5f, t.position.z);
    } else {
        t.position = glm::vec3(px, py, t.position.z);
    }
    // 角度 → 绕 z 旋转
    b2Rot rot = b2Body_GetRotation(body);
    t.rotation = glm::quat(glm::vec3(0.0f, 0.0f, b2Rot_GetAngle(rot)));
}

void Physics2DSystem::Update(float dt) {
    // 1. 推进物理
    Physics2DManager::GetInstance().Update(dt);
    // 2. 同步实体(b2Body 创建 + Transform 写回)
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (const auto& root : sceneECS.GetRootEntities()) {
        VisitEntity(root);
    }
    // 3. 派发接触/触发事件(在写回之后: 回调读到的是物理结算后的最新位置)
    DispatchContactEvents();
}

void Physics2DSystem::ClearBodies() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
    if (!b2World_IsValid(world)) return;

    std::vector<ECS::Entity> toClear;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::RigidBody2DComponent>(e))
            toClear.push_back(e);
        for (const auto& c : ECS::SceneECS::GetInstance().GetChildren(e)) visit(c);
    };
    for (const auto& root : ECS::SceneECS::GetInstance().GetRootEntities()) visit(root);

    for (ECS::Entity e : toClear) {
        auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
        if (rb.body) {
            b2BodyId body = BodyIdOf(rb.body);
            if (b2Body_IsValid(body)) b2DestroyBody(body);
            rb.body = nullptr;
        }
    }
    std::cout << "[Physics2D] Cleared " << toClear.size() << " bodies" << std::endl;
    m_ActiveSensorOverlaps.clear(); // 旧实体 id 全部失效, 重叠集合必须清空
}

// 取实体第一个 2D 碰撞形状(b2ShapeId);无刚体/无效时返回无效句柄
namespace {
b2ShapeId FirstShapeOf(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::RigidBody2DComponent>(e)) return b2ShapeId{ 0, 0, 0 };
    auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
    if (!rb.body) return b2ShapeId{ 0, 0, 0 };
    b2BodyId body = BodyIdOf(rb.body);
    if (!b2Body_IsValid(body)) return b2ShapeId{ 0, 0, 0 };
    b2ShapeId shape;
    if (b2Body_GetShapes(body, &shape, 1) > 0) return shape;
    return b2ShapeId{ 0, 0, 0 };
}
}

// 两个实体第一个形状的 AABB 是否重叠(用于传感器 end 补偿)。
// 注意: b2Shape_GetAABB 返回 shape->aabb(含 ±b2_speculativeDistance 推测边距, 非严格精确盒)。
// 判定方向仍然严格: Box2D 在 fatAABB(= aabb + 更大边距)分离时销毁接触并丢 end 事件,
// 而 fatAABB 分离 ⇒ aabb 必然已分离, 因此补偿判定不会早于真实分离, 无"误补发";
// 若 aabb 重叠但形状分离, 接触未销毁, Box2D 会经 stoppedTouching 路径正常发 end 事件兜底。
bool Physics2DSystem::ShapesAABBOverlap(ECS::Entity a, ECS::Entity b) {
    b2ShapeId sa = FirstShapeOf(a);
    b2ShapeId sb = FirstShapeOf(b);
    if (!b2Shape_IsValid(sa) || !b2Shape_IsValid(sb)) return false;
    b2AABB aa = b2Shape_GetAABB(sa);
    b2AABB bb = b2Shape_GetAABB(sb);
    return aa.lowerBound.x <= bb.upperBound.x && aa.upperBound.x >= bb.lowerBound.x &&
           aa.lowerBound.y <= bb.upperBound.y && aa.upperBound.y >= bb.lowerBound.y;
}

void Physics2DSystem::DispatchContactEvents() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
    if (!b2World_IsValid(world)) return;

    // 1. 收集事件为纯实体对(先拷贝再派发: 回调内销毁 body 不影响已收集数据)
    struct ContactPair { ECS::Entity a, b; bool enter; };
    std::vector<ContactPair> pairs;
    std::set<std::tuple<ECS::Entity, ECS::Entity, bool>> seen; // 无向实体对 + 方向, 同帧去重
    std::set<std::pair<ECS::Entity, ECS::Entity>> sensorNow;   // 本帧 Box2D 确认重叠的 sensor 对
    std::set<std::pair<ECS::Entity, ECS::Entity>> sensorEndNow; // 本帧收到 sensor end 的对(已正常派发)

    auto addPair = [&](ECS::Entity a, ECS::Entity b, bool enter) {
        if (a == ECS::INVALID_ENTITY || b == ECS::INVALID_ENTITY || a == b) return;
        const ECS::Entity lo = a < b ? a : b;
        const ECS::Entity hi = a < b ? b : a;
        auto key = std::make_tuple(lo, hi, enter);
        if (!seen.insert(key).second) return;
        pairs.push_back({ a, b, enter });
    };
    auto pairKey = [](ECS::Entity a, ECS::Entity b) {
        return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    };

    // 普通碰撞(两个非传感器形状 begin/end touch)
    b2ContactEvents ce = b2World_GetContactEvents(world);
    for (int i = 0; i < ce.beginCount; ++i) {
        addPair(EntityOfShape(ce.beginEvents[i].shapeIdA), EntityOfShape(ce.beginEvents[i].shapeIdB), true);
    }
    for (int i = 0; i < ce.endCount; ++i) {
        addPair(EntityOfShape(ce.endEvents[i].shapeIdA), EntityOfShape(ce.endEvents[i].shapeIdB), false);
    }

    // 传感器重叠(isTrigger 形状进入/离开; sensorShapeId=传感器, visitorShapeId=进入者)
    b2SensorEvents se = b2World_GetSensorEvents(world);
    for (int i = 0; i < se.beginCount; ++i) {
        ECS::Entity s = EntityOfShape(se.beginEvents[i].sensorShapeId);
        ECS::Entity v = EntityOfShape(se.beginEvents[i].visitorShapeId);
        addPair(s, v, true);
        if (s != ECS::INVALID_ENTITY && v != ECS::INVALID_ENTITY) sensorNow.insert(pairKey(s, v));
    }
    for (int i = 0; i < se.endCount; ++i) {
        ECS::Entity s = EntityOfShape(se.endEvents[i].sensorShapeId);
        ECS::Entity v = EntityOfShape(se.endEvents[i].visitorShapeId);
        addPair(s, v, false);
        if (s != ECS::INVALID_ENTITY && v != ECS::INVALID_ENTITY) sensorEndNow.insert(pairKey(s, v));
    }

    // 2. 补偿 Box2D 3.0.x 丢失的传感器 end 事件(上游 issue #867, v3.1.0 才修复):
    // 接触因 fatAABB 分离走 broadphase 销毁路径(simDisjoint)时不产生 sensor end 事件,
    // 表现为 onExit 静默丢失(高速穿过传感器时必现)。
    // 机制: 上帧仍重叠、本帧未收到 begin/end 的对, 用精确 AABB 判定真实重叠状态——
    //   已分离 → 手动补发 onExit(立即, 无误报);
    //   AABB 仍重叠 → Box2D 静默保留该接触, 维持 active 下帧再判。
    for (auto it = m_ActiveSensorOverlaps.begin(); it != m_ActiveSensorOverlaps.end();) {
        const auto key = *it;
        // 本帧已有明确结论(begin=仍重叠 / end=已分离并派发)的对不补偿
        if (sensorNow.count(key) || sensorEndNow.count(key)) {
            ++it;
            continue;
        }
        const bool overlap = ShapesAABBOverlap(key.first, key.second);
        if (!overlap) {
            // 接触已分离但 end 事件被 Box2D 丢弃 → 补发
            DispatchToEntity(key.first, key.second, false);
            DispatchToEntity(key.second, key.first, false);
            it = m_ActiveSensorOverlaps.erase(it);
        } else {
            sensorNow.insert(key); // 仍重叠(Box2D 静默), 保持 active
            ++it;
        }
    }
    m_ActiveSensorOverlaps = std::move(sensorNow); // 作为下帧"上帧仍重叠"

    if (pairs.empty()) return;

    // 3. 派发: 双方实体各触发一次 onEnter/onExit(哪边挂了组件哪边收到)
    // 注意: 若 onEnter 回调内销毁了对方实体(b2Body), 该接触被 Box2D 直接销毁,
    // 后续不再产生 end 事件 —— 此时需在 onEnter 里自行处理"退出"逻辑。
    for (const auto& p : pairs) {
        DispatchToEntity(p.a, p.b, p.enter);
        DispatchToEntity(p.b, p.a, p.enter);
    }
}

void Physics2DSystem::DispatchToEntity(ECS::Entity e, ECS::Entity other, bool enter) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::Contact2DComponent>(e)) return;
    auto& c = coordinator.GetComponent<ECS::Contact2DComponent>(e);
    if (!c.enabled) return;
    // 按值拷贝再调用: 回调内移除/销毁自身组件或实体时, 本函数持有的引用/捕获不会失效
    // (ComponentArray::RemoveData 用尾元素 move 覆盖槽位, 引用会悬空)
    std::function<void(ECS::Entity)> cb = enter ? c.onEnter : c.onExit;
    if (cb) cb(other);
}

void Physics2DSystem::RemoveBody(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::RigidBody2DComponent>(e)) return;
    auto& rb = coordinator.GetComponent<ECS::RigidBody2DComponent>(e);
    if (!rb.body) return;
    b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
    if (!b2World_IsValid(world)) return; // 世界已销毁(引擎关闭中), 句柄随组件一并丢弃
    b2BodyId body = BodyIdOf(rb.body);
    if (b2Body_IsValid(body)) b2DestroyBody(body);
    rb.body = nullptr;
}

namespace {
// 画矩形线框(4 条细条, 线宽 2px)
void DrawRectOutline(Renderer2D& r2d, glm::vec2 center, glm::vec2 size, const glm::vec4& col, int layer) {
    const float t = 2.0f;
    const glm::vec2 min = center - size * 0.5f;
    r2d.DrawRect({ min.x, min.y - t }, { size.x, t }, col, layer);              // 上边
    r2d.DrawRect({ min.x, min.y + size.y }, { size.x, t }, col, layer);        // 下边
    r2d.DrawRect({ min.x - t, min.y }, { t, size.y }, col, layer);             // 左边
    r2d.DrawRect({ min.x + size.x, min.y }, { t, size.y }, col, layer);        // 右边
}

// 画圆线框(16 段折线, 每段旋转细条)
void DrawCircleOutline(Renderer2D& r2d, glm::vec2 center, float radius, const glm::vec4& col, int layer) {
    constexpr int kSeg = 16;
    constexpr float kTwoPi = 6.28318530718f;
    VkDescriptorSet white = r2d.GetWhiteTexture();
    const float w = 2.0f;
    float px = center.x + radius, py = center.y;
    for (int i = 1; i <= kSeg; ++i) {
        const float a = (float)i / kSeg * kTwoPi;
        const float x = center.x + cosf(a) * radius;
        const float y = center.y + sinf(a) * radius;
        const glm::vec2 mid((px + x) * 0.5f, (py + y) * 0.5f);
        const glm::vec2 d(x - px, y - py);
        const float len = glm::length(d);
        if (len > 0.001f) {
            const float ang = atan2f(d.y, d.x);
            const glm::vec2 cA(cosf(ang), sinf(ang)), cB(-sinf(ang), cosf(ang));
            const glm::vec2 h = cA * (len * 0.5f), n = cB * (w * 0.5f);
            Quad2D q;
            q.p0 = mid - h - n; q.p1 = mid + h - n; q.p2 = mid + h + n; q.p3 = mid - h + n;
            q.uv0 = { 0.0f, 0.0f }; q.uv1 = { 1.0f, 1.0f };
            q.color = col; q.texture = white; q.layer = layer;
            r2d.DrawQuad(q);
        }
        px = x; py = y;
    }
}
} // namespace

void Physics2DSystem::RenderDebug(Renderer2D& r2d) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    const glm::vec4 solidCol(0.2f, 0.9f, 0.4f, 0.9f);   // 绿色: 实体碰撞体
    const glm::vec4 triggerCol(0.3f, 0.8f, 1.0f, 0.9f); // 青色: 触发器
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::RigidBody2DComponent>(e) &&
            coordinator.HasComponent<ECS::Collider2DComponent>(e)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
            auto& col = coordinator.GetComponent<ECS::Collider2DComponent>(e);
            const glm::vec2 center(t.position.x + col.offset.x, t.position.y + col.offset.y);
            const glm::vec4 color = col.isTrigger ? triggerCol : solidCol;
            if (col.shape == ECS::Collider2DComponent::Shape::Box) {
                DrawRectOutline(r2d, center, col.size, color, 20);
            } else {
                DrawCircleOutline(r2d, center, col.radius, color, 20);
            }
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
}

