// 碰撞凸包缓存：沿用 BVH 的磁头/版本/计数布局思路，但缓存的是每个凸包的最终顶点。
// 版本号变化或源模型时间戳/大小变化时自动回退到重新生成。
constexpr uint32_t kCollisionCacheMagic = 0x4D4B4343u; // "MKCC"
constexpr uint32_t kCollisionCacheVersion = 1u;
constexpr uint32_t kMaxCollisionCacheHulls = 1000000u;

struct CollisionCacheStamp {
    uint64_t fileSize = 0;
    uint64_t writeTime = 0;
    bool valid = false;
};

struct CollisionHullCacheEntry {
    uint32_t subMeshIndex = 0;
    std::vector<glm::vec3> points;
};

std::filesystem::path ResolveCollisionSourcePath(const std::string& modelPath) {
    const std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(modelPath);
    std::filesystem::path resolvedPath(resolved);
    if (!resolved.empty() && std::filesystem::exists(resolvedPath)) return resolvedPath;
    return resolvedPath;
}

CollisionCacheStamp GetCollisionCacheStamp(const std::string& modelPath) {
    CollisionCacheStamp stamp;
    const std::filesystem::path sourcePath = ResolveCollisionSourcePath(modelPath);
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(sourcePath, error);
    if (error) return stamp;

    const auto writeTime = std::filesystem::last_write_time(sourcePath, error);
    if (error) return stamp;

    stamp.fileSize = static_cast<uint64_t>(size);
    stamp.writeTime = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    stamp.valid = true;
    return stamp;
}

std::string GetCollisionCachePath(const std::string& modelPath) {
    const std::filesystem::path sourcePath(modelPath);
    const std::filesystem::path cacheDir = sourcePath.parent_path() / "bvh";
#ifndef __ANDROID__
    std::error_code error;
    std::filesystem::create_directories(cacheDir, error);
#endif
    return (cacheDir / (sourcePath.stem().string() + "_collision.bin")).string();
}

template <typename T>
bool WriteCollisionValue(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

template <typename T>
bool ReadCollisionValue(std::ifstream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

void AppendUniqueCollisionPoint(std::vector<glm::vec3>& points, const glm::vec3& point) {
    for (const glm::vec3& existing : points) {
        const glm::vec3 delta = existing - point;
        if (glm::dot(delta, delta) <= 1.0e-12f) return;
    }
    points.push_back(point);
}

// 将 submesh 顶点压缩到 Jolt 凸包允许的输入规模。
// 不是按文件顺序截断，而是优先保留外轮廓极值，再用空间网格补足采样点。
std::vector<glm::vec3> ReduceCollisionHullPoints(const std::vector<glm::vec3>& source,
                                                 int maxPoints) {
    std::vector<glm::vec3> finitePoints;
    finitePoints.reserve(source.size());
    for (const glm::vec3& point : source) {
        if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
            finitePoints.push_back(point);
        }
    }

    if (finitePoints.size() <= static_cast<size_t>(maxPoints)) return finitePoints;

    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& point : finitePoints) {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }

    std::vector<glm::vec3> reduced;
    reduced.reserve(static_cast<size_t>(maxPoints));

    const std::array<glm::vec3, 14> directions = {
        glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
        glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        glm::vec3(1, 1, 1), glm::vec3(1, 1, -1),
        glm::vec3(1, -1, 1), glm::vec3(1, -1, -1),
        glm::vec3(-1, 1, 1), glm::vec3(-1, 1, -1),
        glm::vec3(-1, -1, 1), glm::vec3(-1, -1, -1)
    };
    for (const glm::vec3& direction : directions) {
        size_t bestIndex = 0;
        float bestProjection = -std::numeric_limits<float>::max();
        for (size_t i = 0; i < finitePoints.size(); ++i) {
            const float projection = glm::dot(finitePoints[i], direction);
            if (projection > bestProjection) {
                bestProjection = projection;
                bestIndex = i;
            }
        }
        AppendUniqueCollisionPoint(reduced, finitePoints[bestIndex]);
    }

    const glm::vec3 extent = boundsMax - boundsMin;
    const int gridResolution = std::max(
        2, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(maxPoints) * 2.0))));
    std::unordered_set<uint64_t> occupiedCells;
    occupiedCells.reserve(static_cast<size_t>(gridResolution * gridResolution * gridResolution));

    for (const glm::vec3& point : finitePoints) {
        if (reduced.size() >= static_cast<size_t>(maxPoints)) break;

        glm::vec3 normalized(0.0f);
        normalized.x = extent.x > 1.0e-6f ? (point.x - boundsMin.x) / extent.x : 0.0f;
        normalized.y = extent.y > 1.0e-6f ? (point.y - boundsMin.y) / extent.y : 0.0f;
        normalized.z = extent.z > 1.0e-6f ? (point.z - boundsMin.z) / extent.z : 0.0f;
        normalized = glm::clamp(normalized, glm::vec3(0.0f), glm::vec3(1.0f));

        const int x = std::min(gridResolution - 1,
                               static_cast<int>(normalized.x * gridResolution));
        const int y = std::min(gridResolution - 1,
                               static_cast<int>(normalized.y * gridResolution));
        const int z = std::min(gridResolution - 1,
                               static_cast<int>(normalized.z * gridResolution));
        const uint64_t key = static_cast<uint64_t>(x) |
            (static_cast<uint64_t>(y) << 21) |
            (static_cast<uint64_t>(z) << 42);
        if (occupiedCells.insert(key).second) {
            AppendUniqueCollisionPoint(reduced, point);
        }
    }

    if (reduced.size() < static_cast<size_t>(maxPoints)) {
        const size_t stride = std::max<size_t>(1, finitePoints.size() /
            static_cast<size_t>(maxPoints));
        for (size_t i = 0; i < finitePoints.size() && reduced.size() < static_cast<size_t>(maxPoints); i += stride) {
            AppendUniqueCollisionPoint(reduced, finitePoints[i]);
        }
    }

    if (reduced.size() > static_cast<size_t>(maxPoints)) {
        reduced.resize(static_cast<size_t>(maxPoints));
    }
    return reduced;
}

bool CreateCollisionHull(const std::vector<glm::vec3>& points, float precision,
                         JPH::ShapeRefC& outShape,
                         std::vector<glm::vec3>* outFinalHullPoints = nullptr) {
    if (points.size() < 4 || points.size() > static_cast<size_t>(JPH::ConvexHullShape::cMaxPointsInHull)) {
        return false;
    }

    std::vector<JPH::Vec3> joltPoints;
    joltPoints.reserve(points.size());
    for (const glm::vec3& point : points) {
        joltPoints.emplace_back(point.x, point.y, point.z);
    }

    JPH::ConvexHullShapeSettings settings(joltPoints.data(), static_cast<int>(joltPoints.size()));
    settings.mHullTolerance = std::max(0.0f, precision);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (!result.IsValid()) return false;

    outShape = result.Get();
    if (outFinalHullPoints == nullptr) return true;
    if (outShape->GetSubType() != JPH::EShapeSubType::ConvexHull) return false;

    const auto* hull = static_cast<const JPH::ConvexHullShape*>(outShape.GetPtr());
    const JPH::Vec3 centerOfMass = hull->GetCenterOfMass();
    outFinalHullPoints->clear();
    outFinalHullPoints->reserve(hull->GetNumPoints());
    for (uint32_t i = 0; i < hull->GetNumPoints(); ++i) {
        JPH::Float3 point;
        (hull->GetPoint(i) + centerOfMass).StoreFloat3(&point);
        outFinalHullPoints->emplace_back(point.x, point.y, point.z);
    }
    return !outFinalHullPoints->empty();
}

bool LoadCollisionHullCache(const std::string& modelPath,
                            const CollisionCacheStamp& sourceStamp,
                            float precision, int maxHullVertices,
                            bool generatePerSubmesh,
                            std::vector<CollisionHullCacheEntry>& outEntries) {
    const std::string cachePath = GetCollisionCachePath(modelPath);
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic = 0, version = 0, subMeshCount = 0, hullCount = 0;
    uint32_t cachedMaxHullVertices = 0, cachedPerSubmesh = 0;
    uint64_t sourceSize = 0, sourceWriteTime = 0;
    float cachedPrecision = 0.0f;
    if (!ReadCollisionValue(file, magic) || !ReadCollisionValue(file, version) ||
        !ReadCollisionValue(file, sourceSize) || !ReadCollisionValue(file, sourceWriteTime) ||
        !ReadCollisionValue(file, subMeshCount) || !ReadCollisionValue(file, hullCount) ||
        !ReadCollisionValue(file, cachedMaxHullVertices) || !ReadCollisionValue(file, cachedPrecision) ||
        !ReadCollisionValue(file, cachedPerSubmesh)) {
        return false;
    }

    if (magic != kCollisionCacheMagic || version != kCollisionCacheVersion ||
        subMeshCount == 0 || hullCount == 0 || hullCount > kMaxCollisionCacheHulls ||
        cachedMaxHullVertices != static_cast<uint32_t>(maxHullVertices) ||
        cachedPerSubmesh != (generatePerSubmesh ? 1u : 0u) ||
        cachedPrecision != precision ||
        (sourceStamp.valid && (sourceSize != sourceStamp.fileSize ||
                               sourceWriteTime != sourceStamp.writeTime))) {
        std::cout << "[PhysicsManager] Collision cache mismatch, rebuilding: " << cachePath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(44, std::ios::beg); // header: 2*u32 + 2*u64 + 4*u32 + 1*f32
    if (!file || fileSize < 44) return false;

    std::vector<CollisionHullCacheEntry> entries;
    entries.reserve(hullCount);
    for (uint32_t i = 0; i < hullCount; ++i) {
        CollisionHullCacheEntry entry;
        uint32_t pointCount = 0;
        if (!ReadCollisionValue(file, entry.subMeshIndex) || !ReadCollisionValue(file, pointCount) ||
            pointCount < 4 || pointCount > static_cast<uint32_t>(JPH::ConvexHullShape::cMaxPointsInHull)) {
            return false;
        }
        entry.points.resize(pointCount);
        for (glm::vec3& point : entry.points) {
            if (!ReadCollisionValue(file, point.x) || !ReadCollisionValue(file, point.y) ||
                !ReadCollisionValue(file, point.z) ||
                !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                return false;
            }
        }
        entries.push_back(std::move(entry));
    }

    if (file.tellg() != fileSize) return false;
    outEntries = std::move(entries);
    std::cout << "[PhysicsManager] Collision cache loaded: " << cachePath
              << " hulls=" << outEntries.size() << std::endl;
    return true;
}

bool SaveCollisionHullCache(const std::string& modelPath,
                            const CollisionCacheStamp& sourceStamp,
                            uint32_t subMeshCount, float precision,
                            int maxHullVertices, bool generatePerSubmesh,
                            const std::vector<CollisionHullCacheEntry>& entries) {
#ifdef __ANDROID__
    (void)modelPath;
    (void)sourceStamp;
    (void)subMeshCount;
    (void)precision;
    (void)maxHullVertices;
    (void)generatePerSubmesh;
    (void)entries;
    return false;
#else
    const std::string cachePath = GetCollisionCachePath(modelPath);
    std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[PhysicsManager] Failed to save collision cache: " << cachePath << std::endl;
        return false;
    }

    const uint32_t hullCount = static_cast<uint32_t>(entries.size());
    const uint32_t cachedMaxHullVertices = static_cast<uint32_t>(maxHullVertices);
    const uint32_t cachedPerSubmesh = generatePerSubmesh ? 1u : 0u;
    if (!WriteCollisionValue(file, kCollisionCacheMagic) ||
        !WriteCollisionValue(file, kCollisionCacheVersion) ||
        !WriteCollisionValue(file, sourceStamp.fileSize) ||
        !WriteCollisionValue(file, sourceStamp.writeTime) ||
        !WriteCollisionValue(file, subMeshCount) ||
        !WriteCollisionValue(file, hullCount) ||
        !WriteCollisionValue(file, cachedMaxHullVertices) ||
        !WriteCollisionValue(file, precision) ||
        !WriteCollisionValue(file, cachedPerSubmesh)) {
        return false;
    }

    for (const CollisionHullCacheEntry& entry : entries) {
        const uint32_t pointCount = static_cast<uint32_t>(entry.points.size());
        if (!WriteCollisionValue(file, entry.subMeshIndex) ||
            !WriteCollisionValue(file, pointCount)) {
            return false;
        }
        for (const glm::vec3& point : entry.points) {
            if (!WriteCollisionValue(file, point.x) || !WriteCollisionValue(file, point.y) ||
                !WriteCollisionValue(file, point.z)) {
                return false;
            }
        }
    }
    file.close();

    std::cout << "[PhysicsManager] Collision cache saved: " << cachePath
              << " hulls=" << entries.size() << std::endl;
    return true;
#endif
}

bool CreateCompoundFromChildShapes(const std::vector<CollisionHullCacheEntry>& entries,
                                   const std::vector<JPH::ShapeRefC>& childShapes,
                                   PhysicsManager::RigidBodyInfo::Type bodyType,
                                   JPH::ShapeRefC& outShape) {
    if (entries.empty() || entries.size() != childShapes.size()) return false;
    if (childShapes.size() == 1) {
        outShape = childShapes.front();
        return true;
    }

    if (bodyType == PhysicsManager::RigidBodyInfo::Type::Static) {
        JPH::StaticCompoundShapeSettings settings;
        for (size_t i = 0; i < childShapes.size(); ++i) {
            settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                              childShapes[i].GetPtr(), entries[i].subMeshIndex);
        }
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) return false;
        outShape = result.Get();
        return true;
    }

    JPH::MutableCompoundShapeSettings settings;
    for (size_t i = 0; i < childShapes.size(); ++i) {
        settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                          childShapes[i].GetPtr(), entries[i].subMeshIndex);
    }
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (!result.IsValid()) return false;
    outShape = result.Get();
    return true;
}

bool CreateCompoundCollisionShape(const std::vector<CollisionHullCacheEntry>& entries,
                                  PhysicsManager::RigidBodyInfo::Type bodyType,
                                  float precision,
                                  JPH::ShapeRefC& outShape) {
    if (entries.empty()) return false;

    std::vector<JPH::ShapeRefC> childShapes;
    childShapes.reserve(entries.size());
    for (const CollisionHullCacheEntry& entry : entries) {
        JPH::ShapeRefC childShape;
        if (!CreateCollisionHull(entry.points, precision, childShape)) return false;
        childShapes.push_back(childShape);
    }
    return CreateCompoundFromChildShapes(entries, childShapes, bodyType, outShape);
}

bool BuildCollisionShapeFromModel(const MeshData& meshData,
                                  const PhysicsManager::RigidBodyInfo& info,
                                  int maxHullVertices,
                                  std::vector<CollisionHullCacheEntry>& outEntries,
                                  JPH::ShapeRefC& outShape) {
    outEntries.clear();
    std::vector<JPH::ShapeRefC> childShapes;
    const float precision = std::max(0.0f, info.collisionPrecision);

    if (info.generatePerSubmesh) {
        outEntries.reserve(meshData.subMeshes.size());
        childShapes.reserve(meshData.subMeshes.size());
        for (size_t subMeshIndex = 0; subMeshIndex < meshData.subMeshes.size(); ++subMeshIndex) {
            std::vector<glm::vec3> sourcePoints;
            sourcePoints.reserve(meshData.subMeshes[subMeshIndex].vertices.size());
            for (const Vertex& vertex : meshData.subMeshes[subMeshIndex].vertices) {
                sourcePoints.push_back(vertex.Position);
            }

            const std::vector<glm::vec3> reducedPoints =
                ReduceCollisionHullPoints(sourcePoints, maxHullVertices);
            CollisionHullCacheEntry entry;
            entry.subMeshIndex = static_cast<uint32_t>(subMeshIndex);
            JPH::ShapeRefC childShape;
            if (CreateCollisionHull(reducedPoints, precision, childShape, &entry.points)) {
                outEntries.push_back(std::move(entry));
                childShapes.push_back(childShape);
            }
        }
    } else {
        std::vector<glm::vec3> sourcePoints;
        size_t totalVertexCount = 0;
        for (const auto& subMesh : meshData.subMeshes) totalVertexCount += subMesh.vertices.size();
        sourcePoints.reserve(totalVertexCount);
        for (const auto& subMesh : meshData.subMeshes) {
            for (const Vertex& vertex : subMesh.vertices) sourcePoints.push_back(vertex.Position);
        }

        CollisionHullCacheEntry entry;
        entry.subMeshIndex = 0;
        const std::vector<glm::vec3> reducedPoints =
            ReduceCollisionHullPoints(sourcePoints, maxHullVertices);
        JPH::ShapeRefC childShape;
        if (CreateCollisionHull(reducedPoints, precision, childShape, &entry.points)) {
            outEntries.push_back(std::move(entry));
            childShapes.push_back(childShape);
        }
    }

    if (outEntries.empty()) return false;
    return CreateCompoundFromChildShapes(outEntries, childShapes, info.type, outShape);
}

bool LoadOrBuildCollisionShape(const PhysicsManager::RigidBodyInfo& info,
                               JPH::ShapeRefC& outShape) {
    const int maxHullVertices = std::clamp(
        info.maxConvexHullVertices, 4, JPH::ConvexHullShape::cMaxPointsInHull);
    const CollisionCacheStamp sourceStamp = GetCollisionCacheStamp(info.modelPath);
    std::vector<CollisionHullCacheEntry> entries;

    if (LoadCollisionHullCache(info.modelPath, sourceStamp,
                               info.collisionPrecision, maxHullVertices,
                               info.generatePerSubmesh, entries) &&
        CreateCompoundCollisionShape(entries, info.type,
                                     std::max(0.0f, info.collisionPrecision), outShape)) {
        return true;
    }

    std::cout << "[PhysicsManager] Collision cache miss, generating from model: "
              << info.modelPath << std::endl;
    const ModelLoadResult result = ModelLoader::LoadModelWithTextures(info.modelPath);
    if (!BuildCollisionShapeFromModel(result.meshData, info, maxHullVertices, entries, outShape)) {
        std::cerr << "[PhysicsManager] Failed to generate convex collision: "
                  << info.modelPath << std::endl;
        return false;
    }

    SaveCollisionHullCache(info.modelPath, sourceStamp,
                           static_cast<uint32_t>(result.meshData.subMeshes.size()),
                           info.collisionPrecision, maxHullVertices,
                           info.generatePerSubmesh, entries);
    std::cout << "[PhysicsManager] Convex collision generated: " << info.modelPath
              << " hulls=" << entries.size() << std::endl;
    return true;
}
