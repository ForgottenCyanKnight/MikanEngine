bool PhysicsManager::QueryOrientedBox(const glm::vec3& center,
                                      const glm::quat& rotation,
                                      const glm::vec3& halfExtents,
                                      std::vector<JPH::BodyID>& outBodyIDs,
                                      JPH::BodyID ignoreBodyID) const {
    outBodyIDs.clear();
    if (!physicsSystem) return false;

    const bool finiteCenter = std::isfinite(center.x) &&
                              std::isfinite(center.y) &&
                              std::isfinite(center.z);
    const bool finiteExtents = std::isfinite(halfExtents.x) &&
                               std::isfinite(halfExtents.y) &&
                               std::isfinite(halfExtents.z);
    const bool finiteRotation = std::isfinite(rotation.x) &&
                                std::isfinite(rotation.y) &&
                                std::isfinite(rotation.z) &&
                                std::isfinite(rotation.w);
    if (!finiteCenter || !finiteExtents || !finiteRotation ||
        halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f) {
        return false;
    }

    const float rotationLengthSquared = rotation.x * rotation.x +
                                        rotation.y * rotation.y +
                                        rotation.z * rotation.z +
                                        rotation.w * rotation.w;
    if (!std::isfinite(rotationLengthSquared) || rotationLengthSquared < 1e-8f) {
        return false;
    }

    const glm::quat normalizedRotation = glm::normalize(rotation);
    const JPH::BoxShape queryShape(JPH::Vec3(halfExtents.x,
                                             halfExtents.y,
                                             halfExtents.z),
                                   0.0f);
    const JPH::RMat44 queryTransform = JPH::RMat44::sRotationTranslation(
        JPH::Quat(normalizedRotation.x,
                  normalizedRotation.y,
                  normalizedRotation.z,
                  normalizedRotation.w),
        JPH::RVec3(center.x, center.y, center.z));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    // CollideShape performs the broad-phase candidate lookup and then tests the
    // actual query BoxShape against every candidate in the narrow phase. This
    // is intentionally different from an AABB-only query: a rotated hitbox
    // must not damage bodies that only overlap its enclosing AABB.
    const auto& narrowPhase = physicsSystem->GetNarrowPhaseQuery();
    if (ignoreBodyID.IsInvalid()) {
        narrowPhase.CollideShape(&queryShape,
                                 JPH::Vec3::sOne(),
                                 queryTransform,
                                 settings,
                                 JPH::RVec3::sZero(),
                                 collector);
    } else {
        const JPH::IgnoreSingleBodyFilter bodyFilter(ignoreBodyID);
        narrowPhase.CollideShape(&queryShape,
                                 JPH::Vec3::sOne(),
                                 queryTransform,
                                 settings,
                                 JPH::RVec3::sZero(),
                                 collector,
                                 {},
                                 {},
                                 bodyFilter);
    }

    outBodyIDs.reserve(collector.mHits.size());
    std::unordered_set<uint32_t> uniqueBodyIDs;
    uniqueBodyIDs.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const JPH::BodyID bodyID = hit.mBodyID2;
        if (bodyID.IsInvalid() || !uniqueBodyIDs.insert(BodyIDKey(bodyID)).second) {
            continue;
        }
        outBodyIDs.push_back(bodyID);
    }
    return !outBodyIDs.empty();
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

void PhysicsManager::QueueCollisionEvent(CollisionEventType type,
                                          JPH::BodyID body1,
                                          JPH::BodyID body2) {
    if (body1.IsInvalid() || body2.IsInvalid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_collisionMutex);
    const uint64_t pairKey = CollisionPairKey(body1, body2);
    if (type == CollisionEventType::Enter) {
        auto [it, inserted] = m_activeCollisionPairCounts.emplace(pairKey, 1u);
        if (!inserted) {
            ++it->second;
            // 一个 body pair 可能有多个接触 manifold；对外只报告一次 Enter。
            return;
        }
    } else if (type == CollisionEventType::Stay) {
        // 某些形状组合可能先收到 Persisted；将其视为首次建立接触，避免
        // 上层只收到 Stay 而遗漏 Enter。
        if (m_activeCollisionPairCounts.find(pairKey) == m_activeCollisionPairCounts.end()) {
            m_activeCollisionPairCounts.emplace(pairKey, 1u);
            type = CollisionEventType::Enter;
        }
    } else {
        auto it = m_activeCollisionPairCounts.find(pairKey);
        if (it == m_activeCollisionPairCounts.end()) {
            return;
        }
        if (it->second > 1u) {
            --it->second;
            return;
        }
        m_activeCollisionPairCounts.erase(it);
    }
    m_collisionEvents.push_back({type, body1, body2});
}

void PhysicsManager::DispatchCollisionEvents() {
    std::vector<CollisionEvent> events;
    CollisionListener* listener = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_collisionMutex);
        events.swap(m_collisionEvents);
        listener = collisionListener;
    }

    if (!listener) {
        return;
    }

    for (const CollisionEvent& event : events) {
        switch (event.type) {
        case CollisionEventType::Enter:
            listener->OnCollisionEnter(event.body1, event.body2);
            break;
        case CollisionEventType::Exit:
            listener->OnCollisionExit(event.body1, event.body2);
            break;
        case CollisionEventType::Stay:
            listener->OnCollisionStay(event.body1, event.body2);
            break;
        }
    }
}

void PhysicsManager::SetCollisionListener(CollisionListener* listener) {
    std::lock_guard<std::mutex> lock(m_collisionMutex);
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

        if (std::find(persistentStaticBodies.begin(), persistentStaticBodies.end(), bodyID) !=
            persistentStaticBodies.end()) {
            continue;
        }
        
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

