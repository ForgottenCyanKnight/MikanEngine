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
        if (info.modelPath.empty()) {
            printf("[PhysicsManager] Mesh collision skipped: model path is empty\n");
            return JPH::BodyID();
        }

        // Jolt 的 MeshShape 只能作为静态几何使用。动态/运动学 Mesh
        // 自动退化为凸包；静态 Mesh 只有显式启用 useConvexHull 时才走
        // 凸包/缓存路径，这样关闭凸包就能保留桥洞、门洞等凹形空间。
        // generatePerSubmesh 只在凸包路径中生效，不能覆盖 useConvexHull=false。
        const bool buildConvexHull =
            info.type != RigidBodyInfo::Type::Static || info.useConvexHull;
        if (buildConvexHull) {
            if (info.type != RigidBodyInfo::Type::Static && !info.useConvexHull) {
                printf("[PhysicsManager] Mesh collision for dynamic/kinematic body uses convex hull: %s\n",
                       info.modelPath.c_str());
            }
            LoadOrBuildCollisionShape(info, shape);
        } else {
            printf("[PhysicsManager] Static mesh collision uses original triangles (holes preserved): %s\n",
                   info.modelPath.c_str());
            const ModelLoadResult result = ModelLoader::LoadModelWithTextures(info.modelPath);
            // 保留模型的真实三角面，不再用 AABB/盒子近似。
            JPH::VertexList meshVertices;
            JPH::IndexedTriangleList meshTriangles;
            size_t vertexCount = 0;
            size_t triangleCount = 0;
            for (const auto& subMesh : result.meshData.subMeshes) {
                vertexCount += subMesh.vertices.size();
                triangleCount += subMesh.indices.size() / 3;
            }
            meshVertices.reserve(vertexCount);
            meshTriangles.reserve(triangleCount);

            for (const auto& subMesh : result.meshData.subMeshes) {
                const uint32_t baseVertex = static_cast<uint32_t>(meshVertices.size());
                for (const auto& vertex : subMesh.vertices) {
                    meshVertices.push_back(JPH::Float3{
                        vertex.Position.x, vertex.Position.y, vertex.Position.z});
                }
                for (size_t i = 0; i + 2 < subMesh.indices.size(); i += 3) {
                    const uint32_t i0 = subMesh.indices[i];
                    const uint32_t i1 = subMesh.indices[i + 1];
                    const uint32_t i2 = subMesh.indices[i + 2];
                    if (i0 >= subMesh.vertices.size() ||
                        i1 >= subMesh.vertices.size() ||
                        i2 >= subMesh.vertices.size()) {
                        continue;
                    }
                    meshTriangles.emplace_back(
                        baseVertex + i0, baseVertex + i1, baseVertex + i2, 0);
                }
            }

            if (!meshVertices.empty() && !meshTriangles.empty()) {
                JPH::MeshShapeSettings settings(
                    std::move(meshVertices), std::move(meshTriangles));
                settings.mMaxTrianglesPerLeaf = 8;
                JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
                if (shapeResult.IsValid()) {
                    shape = shapeResult.Get();
                    printf("[PhysicsManager] Mesh collision created: %s vertices=%zu triangles=%zu\n",
                           info.modelPath.c_str(), vertexCount, triangleCount);
                } else {
                    printf("[PhysicsManager] Mesh collision build failed: %s (%s)\n",
                           info.modelPath.c_str(), shapeResult.GetError().c_str());
                }
            } else {
                printf("[PhysicsManager] Mesh collision has no valid triangles: %s\n",
                       info.modelPath.c_str());
            }
        }

        if (!shape) {
            printf("[PhysicsManager] Mesh rigid body not created: %s\n",
                   info.modelPath.c_str());
            return JPH::BodyID();
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

    // 胶囊体用于角色/敌人时只允许平移，不允许碰撞冲量把角色翻滚。
    // 角色仍可通过 SetRigidBodyOrientation 显式改变朝向；这里仅限制物理
    // 求解器产生的旋转，避免死亡动画结束后胶囊体旋入地面/墙角造成穿模。
    if (motionType == JPH::EMotionType::Dynamic &&
        info.shapeType == RigidBodyInfo::ShapeType::Capsule) {
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                                JPH::EAllowedDOFs::TranslationY |
                                JPH::EAllowedDOFs::TranslationZ;
    }
    
    // 动态刚体使用连续线性碰撞，避免小/高速物体在离散步进中跨过薄碰撞体。
    // 静态和运动学体保持离散模式；它们的位置由场景/脚本控制，不应走动态 CCD 路径。
    settings.mMotionQuality = motionType == JPH::EMotionType::Dynamic
        ? JPH::EMotionQuality::LinearCast
        : JPH::EMotionQuality::Discrete;
    
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
        // 运动学刚体仍由 Transform 驱动，但碰撞响应使用场景配置的质量。
        // 之前这里固定为 1000，导致编辑器/场景中的 mass 字段对运动学方块无效。
        const float kinematicMass = info.mass > 0.0f ? info.mass : 1.0f;
        settings.mMassPropertiesOverride.mMass = kinematicMass;
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

JPH::BodyID PhysicsManager::CreateStaticMeshBody(
    const std::vector<glm::vec3>& vertices,
    const std::vector<uint32_t>& indices) {
    if (!physicsSystem) return JPH::BodyID();
    if (vertices.empty() || indices.size() < 3) {
        printf("[PhysicsManager] Static mesh collision skipped: empty mesh\n");
        return JPH::BodyID();
    }

    JPH::VertexList meshVertices;
    meshVertices.reserve(vertices.size());
    for (const glm::vec3& vertex : vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            printf("[PhysicsManager] Static mesh collision skipped: non-finite vertex\n");
            return JPH::BodyID();
        }
        meshVertices.push_back(JPH::Float3{vertex.x, vertex.y, vertex.z});
    }

    JPH::IndexedTriangleList meshTriangles;
    meshTriangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        meshTriangles.emplace_back(i0, i1, i2, 0);
    }

    if (meshTriangles.empty()) {
        printf("[PhysicsManager] Static mesh collision skipped: no valid triangles\n");
        return JPH::BodyID();
    }

    JPH::MeshShapeSettings shapeSettings(std::move(meshVertices), std::move(meshTriangles));
    shapeSettings.mMaxTrianglesPerLeaf = 8;
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (!shapeResult.IsValid()) {
        printf("[PhysicsManager] Static mesh collision build failed: %s\n",
               shapeResult.GetError().c_str());
        return JPH::BodyID();
    }

    JPH::BodyCreationSettings bodySettings(
        shapeResult.Get(),
        JPH::RVec3(0.0f, 0.0f, 0.0f),
        JPH::Quat(0.0f, 0.0f, 0.0f, 1.0f),
        JPH::EMotionType::Static,
        NON_MOVING);
    bodySettings.mMotionQuality = JPH::EMotionQuality::Discrete;
    bodySettings.mAllowSleeping = true;
    bodySettings.mIsSensor = false;
    bodySettings.mFriction = 0.8f;
    bodySettings.mRestitution = 0.0f;
    bodySettings.mGravityFactor = 0.0f;

    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    const JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(
        bodySettings, JPH::EActivation::Activate);
    if (bodyID.IsInvalid()) {
        printf("[PhysicsManager] Static mesh collision body creation failed\n");
        return bodyID;
    }

    persistentStaticBodies.push_back(bodyID);
    WakeBodiesNear(bodyID);
    printf("[PhysicsManager] Persistent static mesh collision created: vertices=%zu triangles=%zu\n",
           vertices.size(), indices.size() / 3);
    return bodyID;
}

JPH::BodyID PhysicsManager::CreateStaticHeightFieldBody(
    const std::vector<float>& samples,
    uint32_t sampleCount,
    const glm::vec3& offset,
    const glm::vec3& scale,
    const glm::vec3& position,
    const glm::quat& orientation) {
    if (!physicsSystem) return JPH::BodyID();

    constexpr uint32_t kMinSampleCount = 4u;
    constexpr uint32_t kMaxSampleCount = 16384u;
    if (sampleCount < kMinSampleCount || sampleCount > kMaxSampleCount ||
        static_cast<size_t>(sampleCount) >
            std::numeric_limits<size_t>::max() / static_cast<size_t>(sampleCount)) {
        printf("[PhysicsManager] Static heightfield collision skipped: invalid sample count %u\n",
               sampleCount);
        return JPH::BodyID();
    }

    const size_t expectedSampleCount = static_cast<size_t>(sampleCount) * sampleCount;
    if (samples.size() != expectedSampleCount) {
        printf("[PhysicsManager] Static heightfield collision skipped: sample count mismatch\n");
        return JPH::BodyID();
    }

    const auto finiteVector = [](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!finiteVector(offset) || !finiteVector(scale) || !finiteVector(position) ||
        scale.x <= 0.000001f || scale.y <= 0.000001f || scale.z <= 0.000001f) {
        printf("[PhysicsManager] Static heightfield collision skipped: invalid transform\n");
        return JPH::BodyID();
    }

    for (float sample : samples) {
        if (!std::isfinite(sample)) {
            printf("[PhysicsManager] Static heightfield collision skipped: non-finite sample\n");
            return JPH::BodyID();
        }
    }

    const float orientationLength = glm::length(orientation);
    if (!std::isfinite(orientationLength) || orientationLength <= 0.000001f) {
        printf("[PhysicsManager] Static heightfield collision skipped: invalid rotation\n");
        return JPH::BodyID();
    }
    const glm::quat normalizedOrientation = orientation / orientationLength;

    // HeightFieldShape stores a block hierarchy and compresses each block's
    // samples. Use larger blocks when the sample count is aligned; use 2 for
    // common 257x257 maps so the shape only rounds up by one sample.
    const uint32_t blockSize = sampleCount % 8u == 0u ? 8u : 2u;
    JPH::HeightFieldShapeSettings shapeSettings(
        samples.data(),
        JPH::Vec3(offset.x, offset.y, offset.z),
        JPH::Vec3(scale.x, scale.y, scale.z),
        sampleCount);
    shapeSettings.mBlockSize = blockSize;
    shapeSettings.mBitsPerSample = 8u;
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (!shapeResult.IsValid()) {
        printf("[PhysicsManager] Static heightfield collision build failed: %s\n",
               shapeResult.GetError().c_str());
        return JPH::BodyID();
    }

    JPH::BodyCreationSettings bodySettings(
        shapeResult.Get(),
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(normalizedOrientation.x, normalizedOrientation.y,
                  normalizedOrientation.z, normalizedOrientation.w),
        JPH::EMotionType::Static,
        NON_MOVING);
    bodySettings.mMotionQuality = JPH::EMotionQuality::Discrete;
    bodySettings.mAllowSleeping = true;
    bodySettings.mIsSensor = false;
    bodySettings.mFriction = 0.8f;
    bodySettings.mRestitution = 0.0f;
    bodySettings.mGravityFactor = 0.0f;

    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    const JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(
        bodySettings, JPH::EActivation::Activate);
    if (bodyID.IsInvalid()) {
        printf("[PhysicsManager] Static heightfield collision body creation failed\n");
        return bodyID;
    }

    persistentStaticBodies.push_back(bodyID);
    WakeBodiesNear(bodyID);
    const size_t approximateTriangleCount =
        static_cast<size_t>(sampleCount - 1u) * (sampleCount - 1u) * 2u;
    printf("[PhysicsManager] Persistent heightfield collision created: samples=%ux%u "
           "approx_triangles=%zu block=%u\n",
           sampleCount, sampleCount, approximateTriangleCount, blockSize);
    return bodyID;
}

void PhysicsManager::RemoveRigidBody(JPH::BodyID bodyID) {
    if (bodyID.IsInvalid()) return;

    persistentStaticBodies.erase(
        std::remove(persistentStaticBodies.begin(), persistentStaticBodies.end(), bodyID),
        persistentStaticBodies.end());

    if (!physicsSystem) return;
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    if (!bodyInterface.IsAdded(bodyID)) return;
    bodyInterface.RemoveBody(bodyID);
    bodyInterface.DestroyBody(bodyID);
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
    const glm::vec3 absoluteScale = glm::abs(scale);
    if (absoluteScale != glm::vec3(1.0f)) {
        JPH::Vec3 joltScale(absoluteScale.x, absoluteScale.y, absoluteScale.z);
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
