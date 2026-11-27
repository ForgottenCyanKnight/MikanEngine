#include "SceneSerializer.h"
#include "Core/AtomicFile.h"
#include "Core/Utf8Path.h"
#include "Core/InputGlobals.h"
#include "Core/Log.h"
#include "Core/RuntimeCapabilities.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Rendering/Camera.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/ScriptSystem.h"
#include "ECS/Systems/VmdSystem.h"
#include "SceneRenderer.h"
#include "ECS/PhysicsSystem.h"
#include "PhysicsManager.h"
#include "UI/UiDocument.h"
#include "Core/Physics2DSystem.h"
#include "Core/TilemapSystem.h"
#include "SceneJsonView.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <functional>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#elif defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
#include <SDL3/SDL_iostream.h>
#endif

    //
extern std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr;

bool Core::WriteAssetFileAtomically(const std::string& filepath,
                                   const std::string& contents,
                                   const char* kind)
{
    static std::atomic<uint64_t> writeSequence{0};
    if (filepath.empty()) {
        LOGW("[SceneSerializer] %s save rejected: empty path", kind);
        return false;
    }

    const std::filesystem::path targetPath = Utf8Path(filepath);
    const uint64_t sequence =
        writeSequence.fetch_add(1, std::memory_order_relaxed) + 1u;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();

    std::filesystem::path temporaryPath = targetPath;
    temporaryPath += ".tmp." + std::to_string(tick) + "." + std::to_string(sequence);

    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOGE("[SceneSerializer] %s save failed to open temporary file: %s",
                     kind, filepath.c_str());
        return false;
    }

    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    if (!file.good()) {
        file.close();
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        LOGE("[SceneSerializer] %s save failed while writing: %s",
                     kind, filepath.c_str());
        return false;
    }
    file.close();
    if (file.fail()) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        LOGE("[SceneSerializer] %s save failed while closing: %s",
                     kind, filepath.c_str());
        return false;
    }

#ifdef _WIN32
    // Flush the completed temporary file before replacing the target. The
    // replacement itself is performed by MoveFileEx so an existing scene is
    // never removed before the new contents are ready.
    HANDLE temporaryHandle = CreateFileW(
        temporaryPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporaryHandle == INVALID_HANDLE_VALUE ||
        !FlushFileBuffers(temporaryHandle)) {
        if (temporaryHandle != INVALID_HANDLE_VALUE) CloseHandle(temporaryHandle);
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        LOGE("[SceneSerializer] %s save failed to flush temporary file: %s",
                     kind, filepath.c_str());
        return false;
    }
    CloseHandle(temporaryHandle);

    if (!MoveFileExW(temporaryPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        LOGE("[SceneSerializer] %s save failed to replace target: %s",
                     kind, filepath.c_str());
        return false;
    }
#else
    std::error_code replaceError;
    std::filesystem::rename(temporaryPath, targetPath, replaceError);
    if (replaceError) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        LOGE("[SceneSerializer] %s save failed to replace target: %s",
                     kind, filepath.c_str());
        return false;
    }
#endif

    return true;
}

namespace ECS {
namespace {

using Json = nlohmann::json;
constexpr int kCurrentSceneFormatVersion = 1;

struct SceneUiDocument {
    std::string path;
    UI::UiDocument document;
};

size_t CountUiNodes(const std::vector<UI::UiDocumentNode>& nodes)
{
    size_t count = nodes.size();
    for (const auto& node : nodes) count += CountUiNodes(node.children);
    return count;
}

bool ReadSceneUiDocuments(const Json& scene, bool useSnapshots, size_t entityLimit,
                          std::vector<SceneUiDocument>& documents)
{
    size_t entityCount = scene.at("entities").size();
    if (entityCount > entityLimit) {
        LOGE("[SceneSerializer] Scene exceeds the available entity capacity");
        return false;
    }
    const auto links = scene.find("uiDocuments");
    if (links == scene.end()) return true;
    if (!links->is_array()) {
        LOGE("[SceneSerializer] 'uiDocuments' must be an array of project-relative paths");
        return false;
    }

    const auto snapshots = scene.find("uiDocumentSnapshots");
    auto& project = ProjectManager::GetInstance();
    std::unordered_set<std::string> unique;
    for (const auto& link : *links) {
        if (!link.is_string()) {
            LOGE("[SceneSerializer] Each UI document reference must be a path string");
            return false;
        }
        const auto reference = link.get<std::string>();
        const auto path = Utf8Path(reference);
        if (reference.empty() || reference.find_first_of("\\:") != std::string::npos ||
            reference.find('\0') != std::string::npos || path.has_root_path() ||
            !reference.ends_with(".ui.json") ||
            std::any_of(path.begin(), path.end(), [](const auto& part) { return part == ".." || part == "."; }) ||
            !unique.insert(reference).second) {
            LOGE("[SceneSerializer] Invalid or repeated UI document reference: %s", reference.c_str());
            return false;
        }

        std::string error;
        UI::UiDocument document;
        bool ok = false;
        if (useSnapshots && snapshots != scene.end() && snapshots->contains(reference)) {
            ok = UI::ParseUiDocument(snapshots->at(reference).dump(), document, &error);
        } else {
            ok = UI::LoadUiDocument(project.ResolveAssetPath(reference), document, &error);
        }
        if (!ok) {
            LOGE("[SceneSerializer] Invalid UI document %s: %s", reference.c_str(), error.c_str());
            return false;
        }
        entityCount += 1 + CountUiNodes(document.nodes);
        if (entityCount > entityLimit) {
            LOGE("[SceneSerializer] Scene and UI documents exceed the entity limit");
            return false;
        }
        documents.push_back({reference, std::move(document)});
    }
    return true;
}

Entity ReadSerializedId(const Json& value, bool allowRoot)
{
    if (!value.is_number_integer()) throw std::runtime_error("expected an integer entity ID");
    if (allowRoot && value == -1) return INVALID_ENTITY;
    if (value < 0 || value > INVALID_ENTITY || (!allowRoot && value == INVALID_ENTITY)) {
        throw std::runtime_error("entity ID is out of range");
    }
    return value.get<Entity>();
}

bool IsFiniteFloat(const Json& value)
{
    return value.is_number() && std::abs(value.get<double>()) <= std::numeric_limits<float>::max();
}

bool IsInt(const Json& value)
{
    if (!value.is_number()) return false;
    const double number = value.get<double>();
    return number >= std::numeric_limits<int>::min() &&
           number <= std::numeric_limits<int>::max() && std::trunc(number) == number;
}

bool IsFloatArray(const Json& value, size_t width)
{
    return value.is_array() && value.size() == width &&
           std::all_of(value.begin(), value.end(), IsFiniteFloat);
}

void ValidateField(const Json& object, const char* key, FieldType type)
{
    const auto field = object.find(key);
    if (field == object.end() || type == FieldType::Hidden) return;
    bool valid = false;
    switch (type) {
    case FieldType::Bool:
        // Legacy files may have string-encoded booleans.
        valid = field->is_boolean() || *field == "true" || *field == "false";
        break;
    case FieldType::Int:
    case FieldType::Enum: valid = IsInt(*field); break;
    case FieldType::Float: valid = IsFiniteFloat(*field); break;
    case FieldType::Vec2: valid = IsFloatArray(*field, 2); break;
    case FieldType::Vec3:
    case FieldType::Color3:
    case FieldType::QuatEuler: valid = IsFloatArray(*field, 3); break;
    case FieldType::Vec4:
    case FieldType::Color4: valid = IsFloatArray(*field, 4); break;
    case FieldType::String:
    case FieldType::Path: valid = field->is_string(); break;
    case FieldType::Hidden: break;
    }
    if (!valid) throw std::runtime_error(std::string("invalid field '") + key + "'");
}

void ValidateComponent(const Json& component, const ComponentMeta& meta)
{
    if (!component.is_object()) throw std::runtime_error("component must be an object");
    for (size_t i = 0; i < meta.fieldCount; ++i) {
        const auto& field = meta.fields[i];
        ValidateField(component, field.name, field.type);
    }

    const std::string_view key = meta.serializeKey;
    if (key == "transform") {
        ValidateField(component, "position", FieldType::Vec3);
        ValidateField(component, "rotation", FieldType::Vec4);
        ValidateField(component, "scale", FieldType::Vec3);
    } else if (key == "hierarchy") {
        if (const auto parent = component.find("parent"); parent != component.end()) {
            ReadSerializedId(*parent, true);
        }
    } else if (key == "script") {
        if (const auto params = component.find("params"); params != component.end() && !params->is_object()) {
            throw std::runtime_error("script params must be an object");
        }
    } else if (key == "sprite2d") {
        for (auto* field : {"type", "layer"}) ValidateField(component, field, FieldType::Int);
        for (auto* field : {"width", "height", "labelFontSize"}) ValidateField(component, field, FieldType::Float);
        for (auto* field : {"uv0", "uv1", "anchorMin", "anchorMax"}) ValidateField(component, field, FieldType::Vec2);
        for (auto* field : {"color", "labelColor"}) ValidateField(component, field, FieldType::Vec4);
        for (auto* field : {"texture", "label"}) ValidateField(component, field, FieldType::String);
        ValidateField(component, "isUI", FieldType::Bool);
    } else if (key == "tween") {
        for (auto* field : {"property", "easing"}) ValidateField(component, field, FieldType::Int);
        for (auto* field : {"from", "to", "duration", "delay"}) ValidateField(component, field, FieldType::Float);
        for (auto* field : {"loop", "pingPong"}) ValidateField(component, field, FieldType::Bool);
    } else if (key == "material") {
        for (auto* field : {"albedoPath", "normalPath", "roughnessPath", "metallicPath", "aoPath", "emissivePath"}) {
            ValidateField(component, field, FieldType::Path);
        }
        for (auto* field : {"metallic", "roughness", "ao", "emissiveIntensity"}) {
            ValidateField(component, field, FieldType::Float);
        }
        for (auto* field : {"useAlbedoTexture", "useNormalTexture", "useRoughnessTexture",
                            "useMetallicTexture", "useAOTexture", "useEmissiveTexture"}) {
            ValidateField(component, field, FieldType::Bool);
        }
        for (auto* field : {"albedoSamplerType", "normalSamplerType", "roughnessSamplerType",
                            "metallicSamplerType", "aoSamplerType", "emissiveSamplerType"}) {
            ValidateField(component, field, FieldType::Int);
        }
        ValidateField(component, "albedoColor", FieldType::Vec3);
    }
}

bool ParseSceneDocument(std::string_view text, const char* documentKind, bool prefab, Json& document)
{
    try {
        document = Json::parse(text.begin(), text.end());
        if (!document.is_object()) throw std::runtime_error("root must be an object");
        if (const auto version = document.find("formatVersion"); version != document.end() &&
            (!version->is_number_integer() || *version != kCurrentSceneFormatVersion)) {
            throw std::runtime_error("unsupported formatVersion " + version->dump());
        }
        if (prefab && (!document.contains("name") || !document.at("name").is_string())) {
            throw std::runtime_error("root requires string 'name'");
        }
        if (const auto game = document.find("game"); game != document.end() && !game->is_string()) {
            throw std::runtime_error("'game' must be a string");
        }
        const auto entities = document.find("entities");
        if (entities == document.end() || !entities->is_array()) {
            throw std::runtime_error("root requires an 'entities' array");
        }
        if (entities->size() > MAX_ENTITIES) throw std::runtime_error("entity limit exceeded");

        const auto& components = ComponentRegistry::GetInstance().GetAll();
        std::unordered_set<Entity> ids;
        ids.reserve(entities->size());
        for (size_t index = 0; index < entities->size(); ++index) {
            const auto& record = (*entities)[index];
            try {
                if (!record.is_object() || !record.contains("id")) {
                    throw std::runtime_error("expected an entity with an ID");
                }
                if (!ids.insert(ReadSerializedId(record.at("id"), false)).second) {
                    throw std::runtime_error("duplicate ID");
                }
                if (!record.contains("name") || !record.at("name").is_object() ||
                    !record.at("name").contains("name") || !record.at("name").at("name").is_string()) {
                    throw std::runtime_error("requires string 'name.name'");
                }
                if (!record.contains("transform") || !record.at("transform").is_object()) {
                    throw std::runtime_error("requires a transform object");
                }
                for (const auto& meta : components) {
                    if (!meta.serializeKey) continue;
                    const auto component = record.find(meta.serializeKey);
                    if (component == record.end()) continue;
                    try {
                        ValidateComponent(*component, meta);
                    } catch (const std::exception& error) {
                        throw std::runtime_error(std::string(meta.serializeKey) + ": " + error.what());
                    }
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("entity[" + std::to_string(index) + "]: " + error.what());
            }
        }
        if (prefab && ids.empty()) throw std::runtime_error("prefab requires at least one entity");
        return true;
    } catch (const std::exception& error) {
        LOGE("[SceneSerializer] Invalid %s: %s", documentKind, error.what());
        return false;
    }
}

using EntityRemap = std::map<Entity, Entity>;

size_t CountLiveEntities()
{
    const auto& coordinator = Coordinator::GetInstance();
    size_t count = 0;
    for (Entity entity = 0; entity < MAX_ENTITIES; ++entity) count += coordinator.IsAlive(entity);
    return count;
}

void RebuildHierarchy(const Json& records, const EntityRemap& entityMap)
{
    auto& coordinator = Coordinator::GetInstance();
    for (const auto& record : records) {
        const Entity entity = entityMap.at(ReadSerializedId(record.at("id"), false));
        const auto& parent = SceneJsonView{record}.Find("hierarchy").Find("parent").value;
        if (parent.is_null()) continue;
        const Entity oldParent = ReadSerializedId(parent, true);
        if (oldParent == INVALID_ENTITY) continue;
        const auto mapped = entityMap.find(oldParent);
        if (mapped == entityMap.end() || mapped->second == entity) {
            LOGW("[SceneSerializer] Invalid parent for entity %u, treating as root", entity);
            continue;
        }
        coordinator.GetComponent<HierarchyComponent>(entity).parent = mapped->second;
    }

    // Retain legacy handling for malformed parent graphs by detaching a cycle.
    std::unordered_set<Entity> resolved;
    for (const auto& [id, entity] : entityMap) {
        if (resolved.contains(entity)) continue;
        std::unordered_set<Entity> pending;
        Entity cursor = entity;
        while (cursor != INVALID_ENTITY && !resolved.contains(cursor)) {
            if (!pending.insert(cursor).second) {
                coordinator.GetComponent<HierarchyComponent>(cursor).parent = INVALID_ENTITY;
                LOGW("[SceneSerializer] Detached hierarchy cycle at entity %u", cursor);
                break;
            }
            cursor = coordinator.GetComponent<HierarchyComponent>(cursor).parent;
        }
        resolved.merge(pending);
    }

    for (const auto& [id, entity] : entityMap) {
        const Entity parent = coordinator.GetComponent<HierarchyComponent>(entity).parent;
        if (parent != INVALID_ENTITY) {
            coordinator.GetComponent<HierarchyComponent>(parent).children.push_back(entity);
        }
    }
}

} // namespace

bool SceneSerializer::SaveScene(const std::string& filepath) {
    std::string json = SerializeScene();
    return !json.empty() && Core::WriteAssetFileAtomically(filepath, json, "scene");
}

bool SceneSerializer::LoadScene(const std::string& filepath) {
#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
    SDL_IOStream* io = SDL_IOFromFile(filepath.c_str(), "rb");
    if (io == nullptr) {
        LOGE("[SceneSerializer] Failed to open packaged scene: %s (SDL: %s)", filepath.c_str(), SDL_GetError());
        return false;
    }

    //
    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        LOGE("[SceneSerializer] Failed to get file size: %s", filepath.c_str());
        SDL_CloseIO(io);
        return false;
    }

    //
    std::string jsonContent;
    jsonContent.resize(fileSize);
    size_t bytesRead = SDL_ReadIO(io, jsonContent.data(), fileSize);
    SDL_CloseIO(io);
    
    if (bytesRead != (size_t)fileSize) {
        LOGE("[SceneSerializer] Failed to read complete file: %s", filepath.c_str());
        return false;
    }
    
    LOGI("[SceneSerializer] Loaded packaged scene: %s (%lld bytes)", filepath.c_str(), (long long)fileSize);
    return DeserializeScene(jsonContent);
#else
    //
    std::ifstream file(Utf8Path(filepath));
    if (!file.is_open()) {
        LOGE("[SceneSerializer] Failed to open file: %s", filepath.c_str());
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (file.bad() || buffer.bad()) {
        LOGE("[SceneSerializer] Failed to read file: %s", filepath.c_str());
        return false;
    }
    
    LOGI("[SceneSerializer] Successfully loaded scene: %s", filepath.c_str());
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
    return SerializeSceneImpl(false);
}

std::string SceneSerializer::SerializePlaySnapshot() {
    return SerializeSceneImpl(true);
}

std::string SceneSerializer::SerializeSceneImpl(bool includeUiSnapshots) {
    auto& coordinator = Coordinator::GetInstance();
    auto& scene = SceneECS::GetInstance();
    nlohmann::json links = nlohmann::json::array();
    nlohmann::json snapshots = nlohmann::json::object();
    std::unordered_set<Entity> externalUiEntities;
    for (const auto root : scene.GetRootEntities()) {
        const std::string path = UI::GetUiDocumentPath(root);
        if (path.empty()) continue;
        links.push_back(path);
        std::string documentJson;
        std::string error;
        if (!UI::SerializeUiDocument(root, documentJson, &error)) {
            LOGE("[SceneSerializer] Could not capture UI document %s: %s", path.c_str(), error.c_str());
            return {};
        }
        if (includeUiSnapshots) {
            snapshots[path] = nlohmann::json::parse(documentJson);
        }
        std::vector<Entity> pending = {root};
        while (!pending.empty()) {
            const Entity entity = pending.back();
            pending.pop_back();
            if (!externalUiEntities.insert(entity).second) continue;
            const auto& children = scene.GetChildren(entity);
            pending.insert(pending.end(), children.begin(), children.end());
        }
    }
    std::vector<Entity> entities;
    
    // collect entities
    for (Entity entity = 0; entity < MAX_ENTITIES; ++entity) {
        if (coordinator.HasComponent<NameComponent>(entity) && !externalUiEntities.contains(entity)) {
            entities.push_back(entity);
        }
    }
    
    std::stringstream json;
    json << "{" << std::endl;
    json << "  \"formatVersion\": " << kCurrentSceneFormatVersion << "," << std::endl;
    if (!links.empty()) json << "  \"uiDocuments\": " << links.dump() << ",\n";
    // 鍦烘櫙鍏宠仈娓告垙妯″潡(椤跺眰 "game" 閿?绌哄垯涓嶈緭鍑?
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    if (!sceneGame.empty()) {
        json << "  \"game\": \"" << EscapeString(sceneGame) << "\"," << std::endl;
    }
    // 编辑器场景视图相机位姿（侧视角等）：加载时优先恢复，避免每次打开场景
    // 都被重置回主相机位置——用户需要摆固定观察位来肉眼判断视锥剔除程度。
    {
        json << "  \"editorCamera\": {" << std::endl;
        json << "    \"position\": [" << g_Camera.Position.x << ", "
             << g_Camera.Position.y << ", " << g_Camera.Position.z << "]," << std::endl;
        json << "    \"yaw\": " << g_Camera.Yaw << "," << std::endl;
        json << "    \"pitch\": " << g_Camera.Pitch << "," << std::endl;
        json << "    \"speed\": " << g_Camera.MovementSpeed << "," << std::endl;
        json << "    \"fov\": " << g_Camera.FieldOfView << "," << std::endl;
        json << "    \"farPlane\": " << g_Camera.FarPlane << "," << std::endl;
        json << "    \"sensitivity\": " << g_Camera.MouseSensitivity << std::endl;
        json << "  }," << std::endl;
    }
    json << "  \"entities\": [" << std::endl;
    
    for (size_t i = 0; i < entities.size(); ++i) {
        json << SerializeEntity(entities[i]);
        if (i < entities.size() - 1) {
            json << ",";
        }
        json << std::endl;
    }
    
    json << "  ]";
    if (!snapshots.empty()) json << ",\n  \"uiDocumentSnapshots\": " << snapshots.dump();
    json << '\n';
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
            LOGE("[SceneSerializer] WARNING: serialize meta missing for SkyboxComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }

    if (coordinator.HasComponent<CloudVolumeComponent>(entity)) {
        std::string g = serializeGeneric(typeid(CloudVolumeComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for CloudVolumeComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<Camera2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(Camera2DComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for Camera2DComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<RigidBody2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(RigidBody2DComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for RigidBody2DComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<Collider2DComponent>(entity)) {
        std::string g = serializeGeneric(typeid(Collider2DComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for Collider2DComponent (skipped)");
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
            LOGE("[SceneSerializer] WARNING: serialize meta missing for TerrainComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }

    if (coordinator.HasComponent<WaterComponent>(entity)) {
        std::string g = serializeGeneric(typeid(WaterComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for WaterComponent (skipped)");
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
            LOGE("[SceneSerializer] WARNING: serialize meta missing for PlayerControllerComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<ECS::ColliderComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::ColliderComponent).name());
        appendComponent(g.empty() ? SerializeColliderComponent(entity) : g);
    }
    
    if (coordinator.HasComponent<ECS::WorldComponent>(entity)) {
        appendComponent(serializeGeneric(typeid(ECS::WorldComponent).name()));
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
            LOGE("[SceneSerializer] WARNING: serialize meta missing for SpriteAnimationComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }
    
    if (coordinator.HasComponent<ECS::TilemapComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::TilemapComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for TilemapComponent (skipped)");
        } else {
            appendComponent(g);
        }
    }

    if (coordinator.HasComponent<ECS::VmdPlayerComponent>(entity)) {
        std::string g = serializeGeneric(typeid(ECS::VmdPlayerComponent).name());
        if (g.empty()) {
            LOGE("[SceneSerializer] WARNING: serialize meta missing for VmdPlayerComponent (skipped)");
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
    return DeserializeSceneImpl(jsonString, false);
}

bool SceneSerializer::DeserializePlaySnapshot(const std::string& jsonString) {
    return DeserializeSceneImpl(jsonString, true);
}

bool SceneSerializer::DeserializeSceneImpl(const std::string& jsonString, bool useUiSnapshots) {
    Json document;
    if (!ParseSceneDocument(jsonString, "scene", false, document)) return false;

    auto& activeScene = SceneECS::GetInstance();
    const size_t availableEntities = MAX_ENTITIES - CountLiveEntities() +
        activeScene.GetHierarchyEntities().size();
    std::vector<SceneUiDocument> uiDocuments;
    try {
        if (!ReadSceneUiDocuments(document, useUiSnapshots, availableEntities, uiDocuments)) return false;
    } catch (const std::exception& error) {
        LOGE("[SceneSerializer] Invalid scene UI documents: %s", error.what());
        return false;
    }

    // All format, reference, field and capacity checks run before replacing
    // the active scene or tearing down its runtime objects.
    ClearScene();
    activeScene.SetSceneGameModule(SceneJsonView{document}.String("game"));
    const auto& records = document.at("entities");
    EntityRemap entityMap;
    for (const auto& record : records) {
        const Entity entity = activeScene.CreateEmpty(record.at("name").at("name").get<std::string>());
        entityMap.emplace(ReadSerializedId(record.at("id"), false), entity);
        DeserializeEntity(entity, {record});
    }
    RebuildHierarchy(records, entityMap);
    LOGI("[SceneSerializer] Deserialized %zu entities", entityMap.size());

    for (const auto& ui : uiDocuments) {
        std::string error;
        if (UI::InstantiateUiDocument(ui.document, ui.path, &error) == INVALID_ENTITY) {
            LOGE("[SceneSerializer] Could not mount UI document %s: %s", ui.path.c_str(), error.c_str());
            return false;
        }
    }
    
    // GPU model renderers require a live Vulkan device. The gameplay-only
    // runner still needs CPU model data for collider auto-fit, but must not
    // create render buffers while deserializing a scene.
    if (!Core::GetRuntimeCapabilities().rendering) {
        LOGW("[SceneSerializer] Gameplay-only runtime: skipped GPU model preload");
    } else if (!g_Standalone2DRuntime) {
        g_SceneRenderer.PreloadModels();
    }
    auto& coordinator = Coordinator::GetInstance();
    auto& scene = SceneECS::GetInstance();
    
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
            g_PhysicsSystemPtr->CreateRigidBodyForEntity(entity, info);
        }
        
        // process children
        auto children = scene.GetChildren(entity);
        for (Entity child : children) {
            processEntity(child);
        }
    };
    
    if (g_PhysicsSystemPtr) {
        for (Entity rootEntity : scene.GetRootEntities()) {
            processEntity(rootEntity);
        }
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

    // 编辑器场景视图相机状态：场景里存有 editorCamera 节（保存时的编辑器相机
    // 位姿）则优先恢复，让用户摆好的观察位（如侧视角判断视锥剔除）跨会话保留；
    // 没有该节（旧场景文件）才走下方的主相机同步回退。
    if (g_Standalone2DRuntime) return true;

    bool editorCameraRestored = false;
    // Older scenes and project switches start with the original editor defaults.
    g_Camera.SetEditorParameters(Camera::SPEED, Camera::DEFAULT_FOV, Camera::DEFAULT_FAR_PLANE);
    {
        try {
            if (document.contains("editorCamera")) {
                const auto& ec = document.at("editorCamera");
                if (ec.is_object()) {
                    auto parameter = [&](const char* name, float fallback) {
                        const auto it = ec.find(name);
                        return it != ec.end() && it->is_number() ? it->get<float>() : fallback;
                    };
                    g_Camera.SetEditorParameters(parameter("speed", Camera::SPEED),
                        parameter("fov", Camera::DEFAULT_FOV),
                        parameter("farPlane", Camera::DEFAULT_FAR_PLANE),
                        parameter("sensitivity", Camera::SENSITIVITY));
                }
                const auto& posIt = ec.find("position");
                const auto& yawIt = ec.find("yaw");
                const auto& pitchIt = ec.find("pitch");
                if (ec.is_object() && posIt != ec.end() && posIt->is_array() &&
                    posIt->size() == 3 && yawIt != ec.end() && pitchIt != ec.end()) {
                    g_Camera.Position = glm::vec3((*posIt)[0].get<float>(),
                                                  (*posIt)[1].get<float>(),
                                                  (*posIt)[2].get<float>());
                    g_Camera.Yaw = yawIt->get<float>();
                    g_Camera.Pitch = pitchIt->get<float>();
                    g_Camera.UpdateCameraVectors();
                    editorCameraRestored = true;
                    LOGI("[SceneSerializer] Editor camera restored: pos(%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f",
                         g_Camera.Position.x, g_Camera.Position.y, g_Camera.Position.z,
                         g_Camera.Yaw, g_Camera.Pitch);
                }
            }
        } catch (const std::exception& error) {
            LOGW("[SceneSerializer] editorCamera node parse failed, fallback to main camera sync: %s",
                 error.what());
        }
    }

    // 递归查找 isMainCamera 实体，把其 position/rotation 同步到编辑器相机 g_Camera（Yaw/Pitch 由朝向反推）。
    if (!editorCameraRestored) {
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
                    LOGI("[SceneSerializer] Editor camera synced to main camera pos(%.2f, %.2f, %.2f)",
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
    }

    return true;
}

void SceneSerializer::DeserializeEntity(Entity entity, SceneJsonView json)
{
    auto& coordinator = Coordinator::GetInstance();
    auto& transform = coordinator.GetComponent<TransformComponent>(entity);
    const auto transformJson = json.Find("transform");
    const auto position = transformJson.FloatArray("position");
    const auto rotation = transformJson.FloatArray("rotation");
    const auto scale = transformJson.FloatArray("scale");
    if (position.size() == 3) transform.position = {position[0], position[1], position[2]};
    if (rotation.size() == 4) transform.rotation = glm::quat(rotation[0], rotation[1], rotation[2], rotation[3]);
    if (scale.size() == 3) transform.scale = {scale[0], scale[1], scale[2]};
    transform.MarkDirty();

    struct HandwrittenEntry {
        std::string_view key;
        void (SceneSerializer::*decode)(Entity, SceneJsonView);
    };
    static constexpr HandwrittenEntry handwritten[] = {
        {"mesh", &SceneSerializer::DeserializeMeshComponent},
        {"render", &SceneSerializer::DeserializeRenderComponent},
        {"camera", &SceneSerializer::DeserializeCameraComponent},
        {"light", &SceneSerializer::DeserializeLightComponent},
        {"material", &SceneSerializer::DeserializeMaterialComponent},
        {"rigidBody", &SceneSerializer::DeserializeRigidBodyComponent},
        {"collider", &SceneSerializer::DeserializeColliderComponent},
        {"voxModel", &SceneSerializer::DeserializeVoxModelComponent},
        {"canvas2d", &SceneSerializer::DeserializeCanvas2DComponent},
        {"sprite2d", &SceneSerializer::DeserializeSprite2DComponent},
        {"textComp", &SceneSerializer::DeserializeTextComponent},
        {"button", &SceneSerializer::DeserializeButtonComponent},
        {"slice9", &SceneSerializer::DeserializeSlice9Component},
        {"tween", &SceneSerializer::DeserializeTweenComponent},
        {"script", &SceneSerializer::DeserializeScriptComponent},
    };
    for (const auto& meta : ComponentRegistry::GetInstance().GetAll()) {
        if (!meta.serializeKey) continue;
        const std::string_view key = meta.serializeKey;
        if (key == "name" || key == "transform" || key == "hierarchy") continue;
        const auto fields = json.Find(key);
        if (!fields.value.is_object()) continue;

        if (meta.fields && key != "script") {
            if (!coordinator.HasComponentByName(entity, meta.typeName)) meta.addTo(entity);
            if (auto* component = coordinator.GetComponentRaw(entity, meta.typeName)) {
                DeserializeComponentByMeta(meta, component, fields);
                if (key == "camera" && !fields.value.contains("showCollisionWireframe") &&
                    fields.value.contains("thirdPersonShowCollisionWireframe")) {
                    static_cast<CameraComponent*>(component)->showCollisionWireframe =
                        fields.Boolean("thirdPersonShowCollisionWireframe");
                }
            }
            continue;
        }

        const auto decoder = std::find_if(std::begin(handwritten), std::end(handwritten),
            [key](const auto& entry) { return entry.key == key; });
        if (decoder != std::end(handwritten)) {
            (this->*decoder->decode)(entity, json);
        } else {
            LOGW("[SceneSerializer] No deserializer for '%s'", meta.serializeKey);
        }
    }
}

std::string SceneSerializer::EscapeString(const std::string& value)
{
    const auto encoded = Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
    return encoded.substr(1, encoded.size() - 2);
}

std::string SceneSerializer::NormalizePath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    
    std::filesystem::path inputPath = Utf8Path(path);
    std::filesystem::path normalizedPath = inputPath.lexically_normal();
    
    //
    std::string result = GenericUtf8String(normalizedPath);
    std::replace(result.begin(), result.end(), '\\', '/');
    
    return result;
}

    //
std::string SceneSerializer::ConvertToRelativePath(const std::string& absolutePath) {
    if (absolutePath.empty()) {
        return absolutePath;
    }

    const std::string normalized = NormalizePath(absolutePath);
    if (normalized.empty()) {
        return normalized;
    }

    const std::filesystem::path inputPath = Utf8Path(normalized);
    const bool isAbsolute =
        inputPath.is_absolute() ||
        (normalized.size() > 1 && normalized[1] == ':');
    if (!isAbsolute) {
        // Already project-relative (or an Android APK-relative path).
        return normalized;
    }

    const auto& projectManager = ProjectManager::GetInstance();

    // Serialization contract: project resources are stored relative to the
    // project root. ResolveAssetPath mirrors this exactly on load, so there
    // is a single path convention regardless of resourceRoot. resourceRoot
    // only scopes the editor content browser (GetAssetsDir), never resolution.
    const std::string projectRelative = projectManager.GetProjectRelativePath(normalized);
    if (!projectRelative.empty()) {
        return projectRelative;
    }

    // Check the project first: projects may live underneath the engine checkout.
    // Only the actual engine resource subtree gets the explicit engine/ prefix.
    std::string engineRoot = NormalizePath(projectManager.GetEngineRoot());
    while (!engineRoot.empty() && engineRoot.back() == '/') engineRoot.pop_back();
    if (!engineRoot.empty() && normalized.rfind(engineRoot + "/engine/", 0) == 0) {
        return normalized.substr(engineRoot.size() + 1);
    }
    // External files remain absolute; they are not silently redirected to a
    // repository-wide assets directory.
    return normalized;
}

void SceneSerializer::ClearScene() {
    auto& coordinator = Coordinator::GetInstance();
    auto& scene = SceneECS::GetInstance();

    // 脚本实例先于实体销毁（OnDestroy 释放资源）
    ScriptSystem::GetInstance().DestroyAll();
    VmdSystem::GetInstance().Clear();
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().ClearAll();
    
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

    EntityRemap ids;
    for (size_t i = 0; i < tree.size(); ++i) ids.emplace(tree[i], static_cast<Entity>(i));

    Json document = {{"formatVersion", kCurrentSceneFormatVersion},
                     {"name", scene.GetName(rootEntity)}, {"entities", Json::array()}};
    try {
        for (Entity entity : tree) {
            auto record = Json::parse(SerializeEntity(entity));
            record["id"] = ids.at(entity);
            const auto& hierarchy = coordinator.GetComponent<HierarchyComponent>(entity);
            const auto parent = ids.find(hierarchy.parent);
            auto& hierarchyJson = record["hierarchy"];
            hierarchyJson["parent"] = parent == ids.end() ? INVALID_ENTITY : parent->second;
            auto& children = hierarchyJson["children"] = Json::array();
            for (Entity child : hierarchy.children) {
                if (const auto mapped = ids.find(child); mapped != ids.end()) {
                    children.push_back(mapped->second);
                }
            }
            document["entities"].push_back(std::move(record));
        }
    } catch (const std::exception& error) {
        LOGE("[Prefab] Could not serialize %s: %s", filepath.c_str(), error.what());
        return false;
    }

    const std::string prefabJson = document.dump(2, ' ', false, Json::error_handler_t::replace) + '\n';
    if (!Core::WriteAssetFileAtomically(filepath, prefabJson, "prefab")) {
        LOGE("[Prefab] FAILED to save output: %s", filepath.c_str());
        return false;
    }
    LOGI("[Prefab] saved %zu entities -> %s", tree.size(), filepath.c_str());
    return true;
}

Entity SceneSerializer::InstantiatePrefab(const std::string& filepath, Entity parent) {
    std::ifstream file(Utf8Path(filepath), std::ios::binary);
    if (!file.is_open()) {
        LOGE("[Prefab] FAILED to open: %s", filepath.c_str());
        return INVALID_ENTITY;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (file.bad() || buffer.bad()) return INVALID_ENTITY;
    Json document;
    if (!ParseSceneDocument(buffer.str(), "prefab", true, document)) return INVALID_ENTITY;

    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();
    if (parent != INVALID_ENTITY && !coordinator.HasComponent<HierarchyComponent>(parent)) {
        LOGE("[Prefab] Invalid parent for %s", filepath.c_str());
        return INVALID_ENTITY;
    }

    const auto& records = document.at("entities");
    if (records.size() > MAX_ENTITIES - CountLiveEntities()) {
        LOGE("[Prefab] Not enough entity capacity for %s", filepath.c_str());
        return INVALID_ENTITY;
    }

    EntityRemap entityMap;
    std::vector<Entity> created;
    created.reserve(records.size());
    Entity root = INVALID_ENTITY;
    try {
        for (const auto& record : records) {
            const Entity entity = scene.CreateEmpty(record.at("name").at("name").get<std::string>());
            created.push_back(entity);
            entityMap.emplace(ReadSerializedId(record.at("id"), false), entity);
            DeserializeEntity(entity, {record});
        }
        RebuildHierarchy(records, entityMap);
        if (const auto canonical = entityMap.find(0); canonical != entityMap.end()) {
            root = canonical->second;
        } else {
            // Authored prefabs can use any serialized entity ID. Standard
            // saves normalize the root to 0, so check that convention first.
            const auto firstRoot = std::find_if(created.begin(), created.end(),
                [&](Entity entity) { return scene.GetParent(entity) == INVALID_ENTITY; });
            if (firstRoot == created.end()) throw std::runtime_error("prefab has no root entity");
            root = *firstRoot;
        }
        if (parent != INVALID_ENTITY) scene.SetParent(root, parent);
    } catch (const std::exception& error) {
        for (Entity entity : created) scene.DestroyEntity(entity);
        LOGE("[Prefab] Could not instantiate %s: %s", filepath.c_str(), error.what());
        return INVALID_ENTITY;
    }

    ScriptSystem::GetInstance().InstantiateAll(false);
    LOGI("[Prefab] instantiated %zu entities from %s (root=%u)", entityMap.size(), filepath.c_str(), root);
    return root;
}

// ===== 脚本组件（Unity 式玩法挂载）序列化/反序列化 =====
// 场景 JSON 形如 {"script":{"scriptName":"ExampleScript","params":{"speedDegPerSec":45}}}
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

void SceneSerializer::DeserializeScriptComponent(Entity entity, SceneJsonView json)
{
    auto& coordinator = Coordinator::GetInstance();
    const auto script = json.Find("script");
    ScriptComponent component;
    component.scriptName = script.String("scriptName");
    component.paramsJson = script.Token("params");
    coordinator.AddComponent(entity, std::move(component));
}

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
        case FieldType::Path:   json << "\"" << EscapeString(ConvertToRelativePath(*(const std::string*)fp)) << "\""; break;
        case FieldType::Enum:   json << *(const int*)fp; break;
        default: break;
        }
    }
    return json.str();
}

void SceneSerializer::DeserializeComponentByMeta(const ComponentMeta& meta, void* comp, SceneJsonView json)
{
    for (size_t i = 0; i < meta.fieldCount; ++i) {
        const auto& field = meta.fields[i];
        const auto value = json.value.find(field.name);
        if (field.type == FieldType::Hidden || value == json.value.end()) continue;

        void* member = static_cast<char*>(comp) + field.offset;
        switch (field.type) {
        case FieldType::Bool: *static_cast<bool*>(member) = json.Boolean(field.name); break;
        case FieldType::Int:
        case FieldType::Enum: *static_cast<int*>(member) = value->get<int>(); break;
        case FieldType::Float: *static_cast<float*>(member) = value->get<float>(); break;
        case FieldType::Vec2:
            *static_cast<glm::vec2*>(member) = {(*value)[0].get<float>(), (*value)[1].get<float>()};
            break;
        case FieldType::Vec3:
        case FieldType::Color3:
            *static_cast<glm::vec3*>(member) = {
                (*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()};
            break;
        case FieldType::Vec4:
        case FieldType::Color4:
            *static_cast<glm::vec4*>(member) = {
                (*value)[0].get<float>(), (*value)[1].get<float>(),
                (*value)[2].get<float>(), (*value)[3].get<float>()};
            break;
        case FieldType::QuatEuler:
            *static_cast<glm::quat*>(member) = glm::quat(glm::radians(glm::vec3{
                (*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()}));
            break;
        case FieldType::String:
        case FieldType::Path: *static_cast<std::string*>(member) = value->get<std::string>(); break;
        case FieldType::Hidden: break;
        }
    }
}

} // namespace ECS
