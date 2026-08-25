#include "SceneSerializer.h"
#include "Core/EngineConfig.h"
#include "Core/InputGlobals.h"
#include "Core/Log.h"
#include "Core/RuntimeCapabilities.h"
#include "Rendering/Camera.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/ScriptSystem.h"
#include "SceneRenderer.h"
#include "ECS/PhysicsSystem.h"
#include "PhysicsManager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <regex>
#include <functional>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_iostream.h>
#include <android/asset_manager.h>
#include <android/native_activity.h>
#define MAX_PATH 1024
#endif

    //
extern std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr;

// g_SceneRenderer 必须全局声明：写在 namespace ECS 内会把变量限定成 ECS::g_SceneRenderer（8 字节 COMMON 符号），
// 与全局 2208 字节对象分离 → 反序列化用空 map → PreloadModels find 崩（2026-08-22 定位）
extern ::SceneRenderer g_SceneRenderer;
namespace ECS {

bool SceneSerializer::SaveScene(const std::string& filepath) {
    std::string json = SerializeScene();
    
    std::ofstream file(std::filesystem::u8path(filepath));
    if (!file.is_open()) {
        return false;
    }
    
    file << json;
    file.close();
    return true;
}

bool SceneSerializer::LoadScene(const std::string& filepath) {
#ifdef __ANDROID__
    //
    SDL_IOStream* io = SDL_IOFromFile(filepath.c_str(), "rb");
    if (io == nullptr) {
        printf("[SceneSerializer] Failed to open file: %s, SDL Error: %s\n", filepath.c_str(), SDL_GetError());
        LOGE("[SceneSerializer] Failed to open Android asset: %s (SDL: %s)", filepath.c_str(), SDL_GetError());
        return false;
    }

    //
    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        printf("[SceneSerializer] Failed to get file size: %s\n", filepath.c_str());
        SDL_CloseIO(io);
        return false;
    }

    //
    std::string jsonContent;
    jsonContent.resize(fileSize);
    size_t bytesRead = SDL_ReadIO(io, jsonContent.data(), fileSize);
    SDL_CloseIO(io);
    
    if (bytesRead != (size_t)fileSize) {
        printf("[SceneSerializer] Failed to read complete file: %s\n", filepath.c_str());
        return false;
    }
    
    printf("[SceneSerializer] Successfully loaded scene from Android assets: %s\n", filepath.c_str());
    LOGI("[SceneSerializer] LoadScene OK from Android assets: %s (%lld bytes)", filepath.c_str(), (long long)fileSize);
    return DeserializeScene(jsonContent);
#else
    //
    std::ifstream file(std::filesystem::u8path(filepath));
    if (!file.is_open()) {
        printf("[SceneSerializer] Failed to open file: %s\n", filepath.c_str());
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    
    printf("[SceneSerializer] Successfully loaded scene: %s\n", filepath.c_str());
    return DeserializeScene(buffer.str());
#endif
}

std::string SceneSerializer::OpenFileDialog() {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    CHAR szFile[260] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(szFile);
    }
#endif
    return "";
}

std::string SceneSerializer::OpenTextureDialog() {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    CHAR szFile[260] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Texture Files (*.png;*.jpg;*.jpeg;*.ktx2)\0*.png;*.jpg;*.jpeg;*.ktx2\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(szFile);
    }
#endif
    return "";
}

std::string SceneSerializer::SaveFileDialog() {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    CHAR szFile[260] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameA(&ofn)) {
        std::string filePath(szFile);
        if (filePath.find(".json") == std::string::npos) {
            filePath += ".json";
        }
        return filePath;
    }
#endif
    return "";
}

std::string SceneSerializer::SerializeScene() {
    auto& coordinator = Coordinator::GetInstance();
    std::vector<Entity> entities;
    
    // collect entities
    for (Entity entity = 0; entity < MAX_ENTITIES; ++entity) {
        if (coordinator.HasComponent<NameComponent>(entity)) {
            entities.push_back(entity);
        }
    }
    
    std::stringstream json;
    json << "{" << std::endl;
    // 鍦烘櫙鍏宠仈娓告垙妯″潡(椤跺眰 "game" 閿?绌哄垯涓嶈緭鍑?
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    if (!sceneGame.empty()) {
        json << "  \"game\": \"" << EscapeString(sceneGame) << "\"," << std::endl;
    }
    json << "  \"entities\": [" << std::endl;
    
    for (size_t i = 0; i < entities.size(); ++i) {
        json << SerializeEntity(entities[i]);
        if (i < entities.size() - 1) {
            json << ",";
        }
        json << std::endl;
    }
    
    json << "  ]" << std::endl;
    json << "}" << std::endl;
    
    return json.str();
}

std::string SceneSerializer::SerializeEntity(Entity entity) {
    std::stringstream json;
    json << "    {" << std::endl;
    json << "      \"id\": " << entity << "," << std::endl;
    
    // serialize components
    auto& coordinator = Coordinator::GetInstance();
    bool firstComponent = true;
    
    auto appendComponent = [&](const std::string& componentJson) {
        if (!firstComponent) {
            json << "," << std::endl;
        }
        json << componentJson;
        firstComponent = false;
    };

    // 閫氱敤鍙嶅皠搴忓垪鍖?鏈夊瓧娈佃〃 + serializeKey 鐨勭粍浠?;鍚﹀垯杩斿洖绌轰覆鐢辫皟鐢ㄦ柟鍥為€€鎵嬪啓
    auto serializeGeneric = [&](const char* typeName) -> std::string {
        auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeName);
        if (!meta || !meta->fields || !meta->serializeKey) return std::string();
        void* comp = coordinator.GetComponentRaw(entity, meta->typeName);
        if (!comp) return std::string();
        std::stringstream s;
        s << "      \"" << meta->serializeKey << "\": {" << std::endl
          << SerializeComponentByMeta(*meta, comp) << std::endl
          << "      }";
        return s.str();
    };
    
    if (coordinator.HasComponent<NameComponent>(entity)) {
        appendComponent(SerializeNameComponent(entity));
    }
    
    if (coordinator.HasComponent<TransformComponent>(entity)) {
        appendComponent(SerializeTransformComponent(entity));
    }
    
    if (coordinator.HasComponent<HierarchyComponent>(entity)) {
        appendComponent(SerializeHierarchyComponent(entity));
    }
    
    if (coordinator.HasComponent<MeshComponent>(entity)) {
        std::string g = serializeGeneric(typeid(MeshComponent).name());
        appendComponent(g.empty() ? SerializeMeshComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<RenderComponent>(entity)) {
        std::string g = serializeGeneric(typeid(RenderComponent).name());
        appendComponent(g.empty() ? SerializeRenderComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<CameraComponent>(entity)) {
        std::string g = serializeGeneric(typeid(CameraComponent).name());
        appendComponent(g.empty() ? SerializeCameraComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<LightComponent>(entity)) {
        std::string g = serializeGeneric(typeid(LightComponent).name());
        appendComponent(g.empty() ? SerializeLightComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<SkyboxComponent>(entity)) {
        std::string g = serializeGeneric(typeid(SkyboxComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for SkyboxComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<Camera2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(Camera2DComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for Camera2DComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<RigidBody2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(RigidBody2DComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for RigidBody2DComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<Collider2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(Collider2DComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for Collider2DComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<MaterialComponent>(entity)) {
        appendComponent(SerializeMaterialComponent(entity));
    }

    if (coordinator.HasComponent<TerrainComponent>(entity)) {
        std::string g = serializeGeneric(typeid(TerrainComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for TerrainComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }

    if (coordinator.HasComponent<WaterComponent>(entity)) {
        std::string g = serializeGeneric(typeid(WaterComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for WaterComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
        appendComponent(SerializeRigidBodyComponent(entity));
    }

    if (coordinator.HasComponent<ECS::PlayerControllerComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::PlayerControllerComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for PlayerControllerComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<ECS::ColliderComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::ColliderComponent).name());
        appendComponent(g.empty() ? SerializeColliderComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::VoxModelComponent).name());
        appendComponent(g.empty() ? SerializeVoxModelComponent(entity) : g);
    }

    if (coordinator.HasComponent<ScriptComponent>(entity)) {
        appendComponent(SerializeScriptComponent(entity));
    }
    
    if (coordinator.HasComponent<ECS::Canvas2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::Canvas2DComponent).name());
        appendComponent(g.empty() ? SerializeCanvas2DComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
        appendComponent(SerializeSprite2DComponent(entity));
    }
    
    if (coordinator.HasComponent<ECS::TextComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::TextComponent).name());
        appendComponent(g.empty() ? SerializeTextComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::ButtonComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::ButtonComponent).name());
        appendComponent(g.empty() ? SerializeButtonComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::Slice9Component>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::Slice9Component).name());
        appendComponent(g.empty() ? SerializeSlice9Component(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::TweenComponent>(entity)) {
        appendComponent(SerializeTweenComponent(entity));
    }
    
    if (coordinator.HasComponent<ECS::SpriteAnimationComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::SpriteAnimationComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for SpriteAnimationComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<ECS::TilemapComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::TilemapComponent).name());
        if (g.empty()) {
            printf("[SceneSerializer] WARNING: serialize meta missing for TilemapComponent (skipped)\n");
        } else {
            appendComponent(g);
        }
    }
    
    json << std::endl << "    }";
    return json.str();
}

std::string SceneSerializer::SerializeNameComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<NameComponent>(entity);
    
    std::stringstream json;
    json << "      \"name\": {" << std::endl;
    json << "        \"name\": \"" << EscapeString(component.name) << "\"" << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeTransformComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<TransformComponent>(entity);
    
    std::stringstream json;
    json << "      \"transform\": {" << std::endl;
    json << "        \"position\": [" << component.position.x << ", " << component.position.y << ", " << component.position.z << "]," << std::endl;
    json << "        \"rotation\": [" << component.rotation.w << ", " << component.rotation.x << ", " << component.rotation.y << ", " << component.rotation.z << "]," << std::endl;
    json << "        \"scale\": [" << component.scale.x << ", " << component.scale.y << ", " << component.scale.z << "]" << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeHierarchyComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<HierarchyComponent>(entity);
    
    std::stringstream json;
    json << "      \"hierarchy\": {" << std::endl;
    json << "        \"parent\": " << component.parent << "," << std::endl;
    json << "        \"children\": [";
    
    for (size_t i = 0; i < component.children.size(); ++i) {
        json << component.children[i];
        if (i < component.children.size() - 1) {
            json << ", ";
        }
    }
    
    json << "]" << std::endl;
    json << "      }";
    return json.str();
}

bool SceneSerializer::DeserializeScene(const std::string& jsonString) {
    //
    ClearScene();

    // 椤跺眰 "game" 閿? 褰撳墠鍦烘櫙鍏宠仈鐨勬父鎴忔ā鍧? 鍔犺浇鍚庣敱寪曟搸鑷姩娲?
    {
        std::string gameVal = ExtractValue(jsonString, "game");
        if (!gameVal.empty() && gameVal.front() == '"' && gameVal.back() == '"')
            gameVal = gameVal.substr(1, gameVal.size() - 2);
        ECS::SceneECS::GetInstance().SetSceneGameModule(gameVal);
    }

    //
    std::string entitiesArray = ExtractValue(jsonString, "entities");
    if (entitiesArray.empty()) {
        printf("[SceneSerializer] No entities found in JSON\n");
        return false;
    }
    
    //
    std::map<int, Entity> entityMap;
    size_t start = 0;
    
    while ((start = entitiesArray.find("{", start)) != std::string::npos) {
        // match brace
        int braceCount = 1;
        size_t end = start + 1;
        while (end < entitiesArray.size() && braceCount > 0) {
            if (entitiesArray[end] == '{') braceCount++;
            else if (entitiesArray[end] == '}') braceCount--;
            end++;
        }
        
        if (braceCount == 0 && end <= entitiesArray.size()) {
            std::string entityJson = entitiesArray.substr(start, end - start);
            Entity entity = DeserializeEntity(entityJson, entityMap);
            start = end;
        } else {
            break;
        }
    }
    
    printf("[SceneSerializer] Deserialized %zu entities\n", entityMap.size());
    
    //
    for (auto& pair : entityMap) {
        Entity entity = pair.second;
        auto& coordinator = Coordinator::GetInstance();
        
        if (coordinator.HasComponent<HierarchyComponent>(entity)) {
            auto& hierarchy = coordinator.GetComponent<HierarchyComponent>(entity);
            if (hierarchy.parent != INVALID_ENTITY) {
                // find parent in map
                auto parentIt = entityMap.find(hierarchy.parent);
                    hierarchy.parent = parentIt->second;
    //
                    auto& parentHierarchy = coordinator.GetComponent<HierarchyComponent>(parentIt->second);
                    parentHierarchy.children.push_back(entity);
                }
            }
        }
    
    // GPU model renderers require a live Vulkan device. The gameplay-only
    // runner still needs CPU model data for collider auto-fit, but must not
    // create render buffers while deserializing a scene.
    if (Core::GetRuntimeCapabilities().rendering) {
        g_SceneRenderer.PreloadModels();
    } else {
        printf("[SceneSerializer] Gameplay-only runtime: skipped GPU model preload\n");
    }
    auto& coordinator = Coordinator::GetInstance();
    auto& scene = SceneECS::GetInstance();
    auto rootEntities = scene.GetRootEntities();
    
    // save syncWithModel settings and disable
    std::map<Entity, bool> syncSettings;
    
    //
    // recursive process entity (rebuild rigid bodies)
    std::function<void(Entity)> processEntity = [&](Entity entity) {
        if (coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
            // save syncWithModel and disable temporarily
            auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(entity);
            syncSettings[entity] = rigidBody.syncWithModel;
            rigidBody.syncWithModel = false;
            
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            
    //
            Physics::PhysicsManager::RigidBodyInfo info;
            info.type = (rigidBody.type == ECS::RigidBodyComponent::Type::Static) ? 
                Physics::PhysicsManager::RigidBodyInfo::Type::Static : 
                (rigidBody.type == ECS::RigidBodyComponent::Type::Kinematic) ? 
                Physics::PhysicsManager::RigidBodyInfo::Type::Kinematic : 
                Physics::PhysicsManager::RigidBodyInfo::Type::Dynamic;
            
            switch (rigidBody.shapeType) {
            case ECS::RigidBodyComponent::ShapeType::Box:
                info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Box;
                break;
            case ECS::RigidBodyComponent::ShapeType::Sphere:
                info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Sphere;
                break;
            case ECS::RigidBodyComponent::ShapeType::Capsule:
                info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Capsule;
                break;
            case ECS::RigidBodyComponent::ShapeType::OBB:
                info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::OBB;
                info.orientation = transform.rotation;
    //
                info.size = rigidBody.size;
                break;
            case ECS::RigidBodyComponent::ShapeType::Mesh:
                info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Mesh;
                info.fromModel = true;
                if (coordinator.HasComponent<ECS::MeshComponent>(entity)) {
                    info.modelPath = coordinator.GetComponent<ECS::MeshComponent>(entity).modelPath;
                }
                if (!rigidBody.collisionModelPath.empty()) {
                    info.modelPath = rigidBody.collisionModelPath;
                }
                info.collisionPrecision = rigidBody.collisionPrecision;
                info.useConvexHull = rigidBody.useConvexHull;
                info.maxConvexHullVertices = rigidBody.maxConvexHullVertices;
                info.generatePerSubmesh = rigidBody.generatePerSubmesh;
                break;
            }
            
    //
            if (rigidBody.shapeType != ECS::RigidBodyComponent::ShapeType::OBB) {
                info.size = rigidBody.size;
            }
            info.position = transform.position +
                transform.rotation * (rigidBody.offset * transform.scale);
            info.rotation = glm::eulerAngles(transform.rotation);
            info.mass = rigidBody.mass;
            info.restitution = rigidBody.restitution;
            info.isTrigger = rigidBody.isTrigger;
            info.useGravity = rigidBody.useGravity;
            
            // create physics body
            if (g_PhysicsSystemPtr) {
                g_PhysicsSystemPtr->CreateRigidBodyForEntity(entity, info);
            }
        }
        
        // process children
        auto children = scene.GetChildren(entity);
        for (Entity child : children) {
            processEntity(child);
        }
    };
    
    //
    for (Entity rootEntity : rootEntities) {
        processEntity(rootEntity);
    }
    
    //
    for (auto& [entity, syncWithModel] : syncSettings) {
        if (coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
            coordinator.GetComponent<ECS::RigidBodyComponent>(entity).syncWithModel = syncWithModel;
        }
    }

    // 场景加载完成：实例化脚本组件（quiet=true：插件 DLL 尚未加载时脚本工厂未注册属预期，
    // 引擎在 Activate 游戏模块后二次补齐并告警）
    ScriptSystem::GetInstance().InstantiateAll(true);

    // 2026-08-17：场景加载/重载完成后，编辑器相机以游戏主相机为准（用户要求：不在原地停留）——
    // 递归查找 isMainCamera 实体，把其 position/rotation 同步到编辑器相机 g_Camera（Yaw/Pitch 由朝向反推）。
    {
        auto& scene = SceneECS::GetInstance();
        std::function<bool(Entity)> syncEditorCam = [&](Entity e) -> bool {
            if (coordinator.HasComponent<CameraComponent>(e) &&
                coordinator.HasComponent<TransformComponent>(e)) {
                auto& cam = coordinator.GetComponent<CameraComponent>(e);
                if (cam.isMainCamera) {
                    auto& tf = coordinator.GetComponent<TransformComponent>(e);
                    g_Camera.Position = tf.position;
                    glm::vec3 front = tf.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                    g_Camera.Yaw = glm::degrees(std::atan2(front.z, front.x));
                    g_Camera.Pitch = glm::degrees(std::asin(glm::clamp(front.y, -1.0f, 1.0f)));
                    g_Camera.UpdateCameraVectors();
                    printf("[SceneSerializer] Editor camera synced to main camera pos(%.2f, %.2f, %.2f)\n",
                           tf.position.x, tf.position.y, tf.position.z);
                    return true;
                }
            }
            for (auto child : scene.GetChildren(e)) {
                if (syncEditorCam(child)) return true;
            }
            return false;
        };
        for (auto rootEntity : scene.GetRootEntities()) {
            if (syncEditorCam(rootEntity)) break;
        }
    }

    return true;
}

Entity SceneSerializer::DeserializeEntity(const std::string& jsonString, std::map<int, Entity>& entityMap) {
    //
    std::string idStr = ExtractValue(jsonString, "id");
    int id = std::stoi(idStr);

    
    // create new entity
    auto& scene = SceneECS::GetInstance();
    Entity entity = scene.CreateEmpty("Entity");
    entityMap[id] = entity;

    auto& coordinator = Coordinator::GetInstance();
    // 通用反射反序列化(有字段表 + serializeKey 的组件);返回 false 时由调用方回退手写
    auto deserializeGeneric = [&](const char* typeName, const char* key) -> bool {
        auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeName);
        if (!meta || !meta->fields || !meta->serializeKey) return false;
        std::string compJson = ExtractValue(jsonString, key);
        if (compJson.empty()) return true; // 组件 JSON 为空,无需处理
        if (!coordinator.HasComponentByName(entity, meta->typeName)) {
            meta->addTo(entity);
        }
        void* comp = coordinator.GetComponentRaw(entity, meta->typeName);
        if (comp) {
            DeserializeComponentByMeta(*meta, comp, compJson);

            // 2026-08：上一版曾把全局碰撞体显示误命名为第三人称相机字段。
            // CameraComponent 现在通过通用反射反序列化，因此在这里完成一次性迁移。
            if (std::strcmp(meta->serializeKey, "camera") == 0 &&
                ExtractValue(compJson, "showCollisionWireframe").empty() &&
                !ExtractValue(compJson, "thirdPersonShowCollisionWireframe").empty()) {
                auto* camera = static_cast<CameraComponent*>(comp);
                camera->showCollisionWireframe =
                    ExtractBoolValue(compJson, "thirdPersonShowCollisionWireframe");
            }
        }
        return true;
    };

    // ===== 反射表驱动反序列化（新增组件只需注册 ComponentRegistry，无需改本函数）=====
    // 遍历注册表：serializeKey 非空且 JSON 含该键 → 先走通用反射；
    // 反射失败（组件字段表缺失/需特殊逻辑）→ 手写回退表。
    {
        struct HandwrittenEntry { const char* key; void (SceneSerializer::*fn)(Entity, const std::string&); };
        static const HandwrittenEntry kHandwritten[] = {
            { "mesh",      &SceneSerializer::DeserializeMeshComponent },
            { "render",    &SceneSerializer::DeserializeRenderComponent },
            { "camera",    &SceneSerializer::DeserializeCameraComponent },
            { "light",     &SceneSerializer::DeserializeLightComponent },
            { "material",  &SceneSerializer::DeserializeMaterialComponent },
            { "rigidBody", &SceneSerializer::DeserializeRigidBodyComponent },
            { "collider",  &SceneSerializer::DeserializeColliderComponent },
            { "voxModel",  &SceneSerializer::DeserializeVoxModelComponent },
            { "canvas2d",  &SceneSerializer::DeserializeCanvas2DComponent },
            { "sprite2d",  &SceneSerializer::DeserializeSprite2DComponent },
            { "textComp",  &SceneSerializer::DeserializeTextComponent },
            { "button",    &SceneSerializer::DeserializeButtonComponent },
            { "slice9",    &SceneSerializer::DeserializeSlice9Component },
            { "tween",     &SceneSerializer::DeserializeTweenComponent },
        };
        const auto& all = ECS::ComponentRegistry::GetInstance().GetAll();
        for (const auto& meta : all) {
            if (!meta.serializeKey) continue;
            // 基础组件（name/transform/hierarchy）在下方手写处理，跳过反射分派
            if (std::strcmp(meta.serializeKey, "name") == 0 ||
                std::strcmp(meta.serializeKey, "transform") == 0 ||
                std::strcmp(meta.serializeKey, "hierarchy") == 0) continue;
            // 脚本组件：手写反序列化（params 需保留为对象原文，不走通用字段反射）
            if (std::strcmp(meta.serializeKey, "script") == 0) {
                DeserializeScriptComponent(entity, jsonString);
                continue;
            }
            const std::string keyPattern = std::string("\"") + meta.serializeKey + "\":";
            if (jsonString.find(keyPattern) == std::string::npos) continue;
            if (deserializeGeneric(meta.typeName, meta.serializeKey)) continue;
            bool handled = false;
            for (const auto& h : kHandwritten) {
                if (std::strcmp(h.key, meta.serializeKey) == 0) {
                    (this->*h.fn)(entity, jsonString);
                    handled = true;
                    break;
                }
            }
            if (!handled) {
                printf("[SceneSerializer] WARNING: no deserializer for '%s' (skipped)\n", meta.serializeKey);
            }
        }
    }

    // 基础组件（无反射字段表，固定手写；name/transform/hierarchy 为必填基础）
    if (jsonString.find("\"name\":") != std::string::npos) {
        DeserializeNameComponent(entity, jsonString);
    }
    
    if (jsonString.find("\"transform\":") != std::string::npos) {
        DeserializeTransformComponent(entity, jsonString);
    }
    
    if (jsonString.find("\"hierarchy\":") != std::string::npos) {
        DeserializeHierarchyComponent(entity, jsonString, entityMap);
    }

    return entity;
}

void SceneSerializer::DeserializeNameComponent(Entity entity, const std::string& jsonString) {

    std::string nameJson = ExtractValue(jsonString, "name");
    std::string name = ExtractValue(nameJson, "name");
    
    //
    if (!name.empty() && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    
    auto& scene = SceneECS::GetInstance();
    scene.SetName(entity, name);
}

void SceneSerializer::DeserializeTransformComponent(Entity entity, const std::string& jsonString) {

    std::string transformJson = ExtractValue(jsonString, "transform");
    
    std::string positionStr = ExtractValue(transformJson, "position");
    std::string rotationStr = ExtractValue(transformJson, "rotation");
    std::string scaleStr = ExtractValue(transformJson, "scale");
    
    //
    std::vector<float> position = ParseFloatArray(positionStr);
    if (position.size() == 3) {
        auto& scene = SceneECS::GetInstance();
        scene.SetPosition(entity, glm::vec3(position[0], position[1], position[2]));
    }
    
    //
    std::vector<float> rotation = ParseFloatArray(rotationStr);
    if (rotation.size() == 4) {
        auto& coordinator = Coordinator::GetInstance();
        if (coordinator.HasComponent<TransformComponent>(entity)) {
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            transform.rotation = glm::quat(rotation[0], rotation[1], rotation[2], rotation[3]);
            transform.MarkDirty(); // 鍙嶅簭鍒楀寲鍐欏叆鏃嬭浆,涓栫晫鐭╅樀缂撳瓨闇€澶辨晥
        }
    }
    
    //
    std::vector<float> scale = ParseFloatArray(scaleStr);
    if (scale.size() == 3) {
        auto& scene = SceneECS::GetInstance();
        scene.SetScale(entity, glm::vec3(scale[0], scale[1], scale[2]));
    }
}

void SceneSerializer::DeserializeHierarchyComponent(Entity entity, const std::string& jsonString, std::map<int, Entity>& entityMap) {

    std::string hierarchyJson = ExtractValue(jsonString, "hierarchy");
    std::string parentStr = ExtractValue(hierarchyJson, "parent");
    
    if (parentStr.empty()) {
        return;
    }
    
    unsigned int parentId = std::stoul(parentStr);
    
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<HierarchyComponent>(entity)) {
        coordinator.AddComponent<HierarchyComponent>(entity, {});
    }
    
    auto& hierarchy = coordinator.GetComponent<HierarchyComponent>(entity);
    
    if (parentId == UINT32_MAX) {
        hierarchy.parent = INVALID_ENTITY;
    } else {
        hierarchy.parent = (Entity)parentId;
    }
}

std::string SceneSerializer::EscapeString(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '\\': result += "\\\\";
break;
            case '"': result += "\\\"";
break;
            case '\n': result += "\\n";
break;
            case '\r': result += "\\r";
break;
            case '\t': result += "\\t";
break;
            default: result += c;
break;
        }
    }
    return result;
}

std::string SceneSerializer::UnescapeString(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            switch (str[i + 1]) {
                case '\\': result += '\\'; i++; break;
                case '"': result += '"'; i++; break;
                case 'n': result += '\n'; i++; break;
                case 'r': result += '\r'; i++; break;
                case 't': result += '\t'; i++; break;
                default: result += str[i]; break;
            }
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string SceneSerializer::ExtractValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) { return ""; }
    pos += searchKey.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\\t' || json[pos] == '\\n' || json[pos] == '\\r')) { pos++; }
    if (pos >= json.size()) { return ""; }
    if (json[pos] == '{') {
        int braceCount = 1; size_t end = pos + 1;
        while (end < json.size() && braceCount > 0) {
            if (json[end] == '{') braceCount++;
            else if (json[end] == '}') braceCount--;
            end++;
        }
        return json.substr(pos, end - pos);
    }
    if (json[pos] == '[') {
        int bracketCount = 1; size_t end = pos + 1;
        while (end < json.size() && bracketCount > 0) {
            if (json[end] == '[') bracketCount++;
            else if (json[end] == ']') bracketCount--;
            end++;
        }
        return json.substr(pos, end - pos);
    }
    if (json[pos] == '"') {
        size_t end = json.find('"', pos + 1);
        while (end != std::string::npos && end > 0 && json[end - 1] == '\\') { end = json.find('"', end + 1); }
        if (end == std::string::npos) { return ""; }
        return json.substr(pos, end - pos + 1);
    } else {
        size_t end = pos;
        if (json.substr(pos, 4) == "true") { return "true"; }
        if (json.substr(pos, 5) == "false") { return "false"; }
        if (json.substr(pos, 4) == "null") { return "null"; }
        while (end < json.size() && (std::isdigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E')) { end++; }
        return json.substr(pos, end - pos);
    }
}

std::vector<float> SceneSerializer::ParseFloatArray(const std::string& arrayStr) {
    std::vector<float> result;
    size_t start = arrayStr.find('[');
    size_t end = arrayStr.find(']');
    if (start == std::string::npos || end == std::string::npos) {
        return result;
    }
    
    std::string values = arrayStr.substr(start + 1, end - start - 1);
    std::stringstream ss(values);
    std::string token;
    
    while (std::getline(ss, token, ',')) {
    //
        token = std::regex_replace(token, std::regex("^\\s+|\\s+$", std::regex_constants::ECMAScript), "");
        if (!token.empty()) {
            try {
                result.push_back(std::stof(token));
            } catch (...) {
    //
            }
        }
    }
    
    return result;
}

bool SceneSerializer::ExtractBoolValue(const std::string& json, const std::string& key) {
    std::string value = ExtractValue(json, key);
    //
    if (!value.empty() && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value == "true";
}

std::string SceneSerializer::NormalizePath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    
    std::filesystem::path inputPath = std::filesystem::u8path(path);
    std::filesystem::path normalizedPath = inputPath.lexically_normal();
    
    //
    std::string result = normalizedPath.generic_u8string();
    std::replace(result.begin(), result.end(), '\\', '/');
    
    return result;
}

    //
std::string SceneSerializer::ConvertToRelativePath(const std::string& absolutePath) {
    if (absolutePath.empty()) {
        return absolutePath;
    }
    
    std::string normalized = NormalizePath(absolutePath);
    
    //
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Engine-owned resources are serialized with the explicit engine/ prefix
    // so project scenes do not accidentally register or copy built-in assets.
    const std::string engineRoot = NormalizePath(
        ProjectManager::GetInstance().GetEngineRoot());
    if (!engineRoot.empty() &&
        normalized.rfind(engineRoot + "/", 0) == 0) {
        return normalized.substr(engineRoot.size() + 1);
    }
    
    //
    size_t assetsPos = normalized.find("assets/");
    if (assetsPos != std::string::npos) {
        std::string relativePath = normalized.substr(assetsPos);
    //
        while (relativePath.find("../") == 0 || relativePath.find("./") == 0) {
            relativePath = relativePath.substr(3);
        }
        
        //
        relativePath = EngineConfig::StripAssetPrefix(relativePath);
    //
        return relativePath;
    }
    
    //
    //
    //
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath = NormalizePath(buffer);
    std::replace(exePath.begin(), exePath.end(), '\\', '/');
    
    //
    std::string projectRoot = exePath;
    size_t pos = projectRoot.find("/build/");
    if (pos != std::string::npos) {
        projectRoot = projectRoot.substr(0, pos);
    } else {
    //
    //
            pos = projectRoot.find("/build/");
        if (pos != std::string::npos) {
            projectRoot = projectRoot.substr(0, pos);
        }
    }
    
    //
    if (normalized.find(projectRoot) == 0) {
        std::string relativePath = normalized.substr(projectRoot.length());
    //
        if (!relativePath.empty() && relativePath[0] == '/') {
            relativePath = relativePath.substr(1);
        }
        return relativePath;
    }
#endif
    
    //
    return absolutePath;
}



void SceneSerializer::ClearScene() {
    auto& coordinator = Coordinator::GetInstance();
    auto& scene = SceneECS::GetInstance();

    // 脚本实例先于实体销毁（OnDestroy 释放资源）
    ScriptSystem::GetInstance().DestroyAll();
    
    //
    std::vector<Entity> entities = scene.GetRootEntities();
    
    //
    std::function<void(Entity)> deleteEntityRecursive;
    deleteEntityRecursive = [&scene, &coordinator, &deleteEntityRecursive](Entity entity) {
    //
        if (!coordinator.HasComponent<NameComponent>(entity)) {
            return;
        }
        
        std::vector<Entity> children = scene.GetChildren(entity);
        for (Entity child : children) {
            deleteEntityRecursive(child);
        }
        scene.DestroyEntity(entity);
    };
    
    for (Entity entity : entities) {

        deleteEntityRecursive(entity);
    }

}

// ===== 预制体（Unity 式可复用实体模板）=====
bool SceneSerializer::SavePrefab(Entity rootEntity, const std::string& filepath) {
    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<NameComponent>(rootEntity)) return false;

    // 收集子树（DFS，root 在首位）
    std::vector<Entity> tree;
    std::function<void(Entity)> visit = [&](Entity e) {
        tree.push_back(e);
        for (Entity c : scene.GetChildren(e)) visit(c);
    };
    visit(rootEntity);
    if (tree.empty()) return false;

    // id 归一化：原实体 -> 0..N（0=根）
    std::map<Entity, int> idMap;
    for (size_t i = 0; i < tree.size(); ++i) idMap[tree[i]] = (int)i;

    std::stringstream out;
    out << "{\n  \"name\": \"" << EscapeString(scene.GetName(rootEntity)) << "\",\n  \"entities\": [\n";
    for (size_t i = 0; i < tree.size(); ++i) {
        std::string entityJson = SerializeEntity(tree[i]);

        // 顶层 id -> 归一化
        {
            const std::string oldKey = "\"id\": " + std::to_string(tree[i]);
            const std::string newKey = "\"id\": " + std::to_string(idMap[tree[i]]);
            const size_t pos = entityJson.find(oldKey);
            if (pos != std::string::npos) entityJson.replace(pos, oldKey.size(), newKey);
        }
        // hierarchy.parent -> 归一化（树内实体映射；根/树外 = 4294967295 = INVALID）
        {
            auto& h = coordinator.GetComponent<HierarchyComponent>(tree[i]);
            const std::string oldKey = "\"parent\": " + std::to_string(h.parent);
            const bool inTree = (h.parent != INVALID_ENTITY && idMap.count(h.parent) > 0);
            const std::string newKey = "\"parent\": " + (inTree ? std::to_string(idMap[h.parent]) : std::string("4294967295"));
            const size_t pos = entityJson.find(oldKey);
            if (pos != std::string::npos) entityJson.replace(pos, oldKey.size(), newKey);
        }

        out << entityJson;
        if (i + 1 < tree.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";

    std::ofstream file(std::filesystem::u8path(filepath), std::ios::trunc);
    if (!file.is_open()) {
        fprintf(stderr, "[Prefab] FAILED to open output: %s\n", filepath.c_str());
        return false;
    }
    file << out.str();
    file.close();
    printf("[Prefab] saved %zu entities -> %s\n", tree.size(), filepath.c_str());
    return true;
}

Entity SceneSerializer::InstantiatePrefab(const std::string& filepath, Entity parent) {
    std::ifstream file(std::filesystem::u8path(filepath), std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[Prefab] FAILED to open: %s\n", filepath.c_str());
        return INVALID_ENTITY;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string jsonString = buffer.str();

    std::string entitiesArray = ExtractValue(jsonString, "entities");
    if (entitiesArray.empty()) {
        fprintf(stderr, "[Prefab] no entities in %s\n", filepath.c_str());
        return INVALID_ENTITY;
    }

    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();
    std::map<int, Entity> entityMap; // prefab 文件 id -> 场景实体

    size_t start = 0;
    while ((start = entitiesArray.find("{", start)) != std::string::npos) {
        int braceCount = 1;
        size_t end = start + 1;
        while (end < entitiesArray.size() && braceCount > 0) {
            if (entitiesArray[end] == '{') braceCount++;
            else if (entitiesArray[end] == '}') braceCount--;
            end++;
        }
        if (braceCount != 0 || end > entitiesArray.size()) break;
        DeserializeEntity(entitiesArray.substr(start, end - start), entityMap);
        start = end;
    }
    if (entityMap.empty()) {
        fprintf(stderr, "[Prefab] no entities deserialized from %s\n", filepath.c_str());
        return INVALID_ENTITY;
    }

    // hierarchy 挂接：prefab 文件 id 引用 -> 场景实体；根(0)挂到目标父（INVALID=场景根）
    for (auto& [pid, entity] : entityMap) {
        if (!coordinator.HasComponent<HierarchyComponent>(entity)) continue;
        auto& h = coordinator.GetComponent<HierarchyComponent>(entity);
        if (h.parent != INVALID_ENTITY && entityMap.count((int)h.parent) > 0) {
            scene.SetParent(entity, entityMap[(int)h.parent]);
        } else if (pid == 0 && parent != INVALID_ENTITY) {
            scene.SetParent(entity, parent);
        }
    }

    // 补齐新实体的脚本实例（幂等）
    ScriptSystem::GetInstance().InstantiateAll(false);

    Entity root = entityMap[0];
    printf("[Prefab] instantiated %zu entities from %s (root=%u)\n", entityMap.size(), filepath.c_str(), root);
    return root;
}

// ===== 脚本组件（Unity 式玩法挂载）序列化/反序列化 =====
// 场景 JSON 形如 {"script":{"scriptName":"RotateScript","params":{"speedDegPerSec":45}}}
// params 为对象原文；运行时实例按脚本参数字段表（GetParamFields）回填/刷新。
std::string SceneSerializer::SerializeScriptComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<ScriptComponent>(entity)) return "";
    auto& sc = coordinator.GetComponent<ScriptComponent>(entity);

    // 运行中有实例时，用实例当前参数字段重新生成 paramsJson（保存最新值，如编辑器改过参数）
    if (sc.runtime) {
        std::string fresh;
        if (ScriptSystem::GetInstance().SerializeParams(sc.runtime, fresh)) {
            sc.paramsJson = fresh;
        }
    }

    std::stringstream s;
    s << "      \"script\": {" << std::endl;
    s << "        \"scriptName\": \"" << EscapeString(sc.scriptName) << "\"";
    if (!sc.paramsJson.empty() && sc.paramsJson != "{}") {
        s << "," << std::endl << "        \"params\": " << sc.paramsJson;
    }
    s << std::endl << "      }";
    return s.str();
}

void SceneSerializer::DeserializeScriptComponent(Entity entity, const std::string& jsonString) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<ScriptComponent>(entity)) {
        coordinator.AddComponent<ScriptComponent>(entity, ScriptComponent{});
    }
    auto& sc = coordinator.GetComponent<ScriptComponent>(entity);
    std::string name = ExtractValue(jsonString, "scriptName");
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    sc.scriptName = name;
    sc.paramsJson = ExtractValue(jsonString, "params"); // 对象原文（可为空）
}

// ===== 閫氱敤鍙嶅皠搴忓垪鍖?瀛楁鍙嶅皠 1b) =====
// 鎸?ComponentMeta 瀛楁琛ㄥ簭鍒楀寲缁勪欢瀛楁(杩斿洖瀛楁 JSON 琛?涓嶅甫缁勪欢 key 鍖呰９;Hidden 瀛楁璺宠繃)
std::string SceneSerializer::SerializeComponentByMeta(const ComponentMeta& meta, const void* comp) {
    std::stringstream json;
    bool first = true;
    for (size_t i = 0; i < meta.fieldCount; ++i) {
        const FieldMeta& f = meta.fields[i];
        if (f.type == FieldType::Hidden) continue;
        const void* fp = static_cast<const char*>(comp) + f.offset;
        if (!first) json << "," << std::endl;
        first = false;
        json << "        \"" << f.name << "\": ";
        switch (f.type) {
        case FieldType::Bool:   json << (*(const bool*)fp ? "true" : "false"); break;
        case FieldType::Int:    json << *(const int*)fp; break;
        case FieldType::Float:  json << *(const float*)fp; break;
        case FieldType::Vec2: {
            const auto& v = *(const glm::vec2*)fp;
            json << "[" << v.x << ", " << v.y << "]";
            break;
        }
        case FieldType::Vec3:
        case FieldType::Color3: {
            const auto& v = *(const glm::vec3*)fp;
            json << "[" << v.x << ", " << v.y << ", " << v.z << "]";
            break;
        }
        case FieldType::Vec4:
        case FieldType::Color4: {
            const auto& v = *(const glm::vec4*)fp;
            json << "[" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << "]";
            break;
        }
        case FieldType::QuatEuler: {
            const auto& q = *(const glm::quat*)fp;
            glm::vec3 e = glm::degrees(glm::eulerAngles(q));
            json << "[" << e.x << ", " << e.y << ", " << e.z << "]";
            break;
        }
        case FieldType::String: json << "\"" << EscapeString(*(const std::string*)fp) << "\""; break;
        case FieldType::Enum:   json << *(const int*)fp; break;
        default: break;
        }
    }
    return json.str();
}

// 鎸?ComponentMeta 瀛楁琛ㄤ粠 JSON 鍙嶅簭鍒楀寲缁勪欢瀛楁(瀛楁缂哄け鏃朵繚鐣欓粯璁ゅ€?鍏煎鏃у瓨妗?
void SceneSerializer::DeserializeComponentByMeta(const ComponentMeta& meta, void* comp, const std::string& jsonString) {
    for (size_t i = 0; i < meta.fieldCount; ++i) {
        const FieldMeta& f = meta.fields[i];
        if (f.type == FieldType::Hidden) continue;
        void* fp = static_cast<char*>(comp) + f.offset;
        switch (f.type) {
        case FieldType::Bool: {
            std::string s = ExtractValue(jsonString, f.name);
            if (!s.empty()) *(bool*)fp = ExtractBoolValue(jsonString, f.name);
            break;
        }
        case FieldType::Int: {
            std::string s = ExtractValue(jsonString, f.name);
            if (!s.empty()) *(int*)fp = std::stoi(s);
            break;
        }
        case FieldType::Float: {
            std::string s = ExtractValue(jsonString, f.name);
            if (!s.empty()) *(float*)fp = std::stof(s);
            break;
        }
        case FieldType::Vec2:
        case FieldType::Color3: {
            std::string s = ExtractValue(jsonString, f.name);
            auto arr = ParseFloatArray(s);
            if (arr.size() >= 2) { float* p = (float*)fp; p[0] = arr[0]; p[1] = arr[1]; }
            break;
        }
        case FieldType::Vec3: {
            std::string s = ExtractValue(jsonString, f.name);
            auto arr = ParseFloatArray(s);
            if (arr.size() >= 3) { float* p = (float*)fp; p[0] = arr[0]; p[1] = arr[1]; p[2] = arr[2]; }
            break;
        }
        case FieldType::Vec4:
        case FieldType::Color4: {
            std::string s = ExtractValue(jsonString, f.name);
            auto arr = ParseFloatArray(s);
            if (arr.size() >= 4) { float* p = (float*)fp; p[0] = arr[0]; p[1] = arr[1]; p[2] = arr[2]; p[3] = arr[3]; }
            break;
        }
        case FieldType::QuatEuler: {
            std::string s = ExtractValue(jsonString, f.name);
            auto arr = ParseFloatArray(s);
            if (arr.size() >= 3) {
                *(glm::quat*)fp = glm::quat(glm::radians(glm::vec3(arr[0], arr[1], arr[2])));
            }
            break;
        }
        case FieldType::String: {
            std::string s = ExtractValue(jsonString, f.name);
            if (!s.empty() && s.front() == '"' && s.back() == '"') {
                s = UnescapeString(s.substr(1, s.size() - 2));
            }
            *(std::string*)fp = s;
            break;
        }
        case FieldType::Enum: {
            std::string s = ExtractValue(jsonString, f.name);
            if (!s.empty()) *(int*)fp = std::stoi(s);
            break;
        }
        default: break;
        }
    }
}

} // namespace ECS

