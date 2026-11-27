#include "UI/UiDocument.h"

#include "Core/AtomicFile.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"

#include <SDL3/SDL.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace UI {
namespace {

using Json = nlohmann::json;
using ECS::Entity;
using ECS::EntityHandle;
using ECS::INVALID_ENTITY;

constexpr int kFormatVersion = 1;
constexpr int kMaxNodeDepth = 32; // Matches Canvas2D traversal.
constexpr std::array<std::string_view, 3> kSpriteTypes{"rect", "image", "button"};
constexpr std::array<std::string_view, 3> kTextModes{"bitmap", "sdf", "msdf"};

struct NodeIdentity {
    EntityHandle handle;
    std::string id;
    std::string action;
};

struct UiInstance {
    EntityHandle root;
    std::string path;
    std::unordered_map<Entity, NodeIdentity> nodes;
};

std::unordered_map<Entity, UiInstance> g_instances;

void Report(std::string* error, std::string message = {}) {
    if (error) *error = std::move(message);
}

ECS::Coordinator& Ecs() {
    return ECS::Coordinator::GetInstance();
}

ECS::SceneECS& Scene() {
    return ECS::SceneECS::GetInstance();
}

const UiInstance* Instance(Entity canvas) {
    const auto found = g_instances.find(canvas);
    return found != g_instances.end() && Ecs().IsAlive(found->second.root) &&
        Ecs().HasComponent<ECS::Canvas2DComponent>(canvas) ? &found->second : nullptr;
}

std::string AssetPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty()) return path;
    if (path.front() == '/' || path.find(':') != std::string::npos)
        throw std::runtime_error("UI document references must use project-relative paths");
    if (path.find('\0') != std::string::npos || path.back() == '/')
        throw std::runtime_error("Invalid UI document path");

    std::string result;
    const std::string_view view(path);
    for (size_t start = 0; start < view.size();) {
        const size_t end = view.find('/', start);
        const auto part = end == std::string_view::npos ? view.substr(start) : view.substr(start, end - start);
        if (part == "..")
            throw std::runtime_error("UI document reference escapes the project");
        if (!part.empty() && part != ".") {
            if (!result.empty()) result += '/';
            result += part;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (result.empty()) throw std::runtime_error("Empty UI document path");
    return result;
}

template<class T>
void ReadValue(const Json& json, T& value) {
    if constexpr (std::is_same_v<T, int>) {
        if (!json.is_number_integer())
            throw std::runtime_error("expected integer");
        const double number = json.get<double>();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
            throw std::runtime_error("integer out of range");
    } else if constexpr (std::is_same_v<T, float>) {
        if (!json.is_number()) throw std::runtime_error("expected number");
    }
    value = json.get<T>();
    if constexpr (std::is_same_v<T, float>) {
        if (!std::isfinite(value)) throw std::runtime_error("number out of range");
    }
}

template<glm::length_t Size, glm::qualifier Q>
void ReadValue(const Json& json, glm::vec<Size, float, Q>& value) {
    if (!json.is_array() || json.size() != Size)
        throw std::runtime_error("incorrect vector length");
    for (glm::length_t i = 0; i < Size; ++i) ReadValue(json.at(i), value[i]);
}

template<class T>
Json WriteValue(const T& value) {
    if constexpr (std::is_same_v<T, float>) {
        if (!std::isfinite(value)) throw std::runtime_error("Cannot serialize a nonfinite UI value");
    }
    return value;
}

template<glm::length_t Size, glm::qualifier Q>
Json WriteValue(const glm::vec<Size, float, Q>& value) {
    auto result = Json::array();
    for (glm::length_t i = 0; i < Size; ++i) result.push_back(WriteValue(value[i]));
    return result;
}

class Reader {
public:
    explicit Reader(const Json& data) : data_(data) {
        if (!data.is_object()) throw std::runtime_error("expected object");
    }

    template<class T>
    void operator()(const char* key, T& value) const {
        const auto field = data_.find(key);
        if (field == data_.end()) return;
        try {
            ReadValue(*field, value);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(key) + ": " + e.what());
        }
    }

    template<class T, size_t N>
    void Enum(const char* key, T& value, const std::array<std::string_view, N>& names) const {
        const auto field = data_.find(key);
        if (field == data_.end()) return;
        if (field->is_string()) {
            const auto& name = field->get_ref<const std::string&>();
            const auto nameIt = std::find(names.begin(), names.end(), name);
            if (nameIt != names.end()) {
                value = static_cast<T>(nameIt - names.begin());
                return;
            }
        } else if (field->is_number_integer()) {
            int index = -1;
            ReadValue(*field, index);
            if (index >= 0 && static_cast<size_t>(index) < N) {
                value = static_cast<T>(index);
                return;
            }
        }
        throw std::runtime_error(std::string("Unsupported value of ") + key);
    }

private:
    const Json& data_;
};

class Writer {
public:
    explicit Writer(Json& data) : data_(data) {}

    template<class T>
    void operator()(const char* key, const T& value) const {
        data_[key] = WriteValue(value);
    }

    template<class T, size_t N>
    void Enum(const char* key, T value, const std::array<std::string_view, N>& names) const {
        data_[key] = names.at(static_cast<size_t>(value));
    }

private:
    Json& data_;
};

template<class Archive, class Canvas>
void CanvasFields(const Archive& field, Canvas& canvas) {
    field("name", canvas.name);
    field("width", canvas.canvas.width);
    field("height", canvas.canvas.height);
    field("stretchToViewport", canvas.canvas.stretchToViewport);
    field("layer", canvas.canvas.layer);
    field("respectSafeArea", canvas.canvas.respectSafeArea);
    field("scaleWithViewport", canvas.canvas.scaleWithViewport);
}

template<class Archive, class Node>
void NodeFields(const Archive& field, Node& node) {
    field("id", node.id);
    field("name", node.name);
    field("position", node.position);
    field("scale", node.scale);
    field("rotation", node.rotation);
    field("visible", node.visible);
    if constexpr (std::is_const_v<Node>) {
        if (!node.action.empty()) field("action", node.action);
    } else {
        field("action", node.action);
    }
}

template<class Archive, class Sprite>
void SpriteFields(const Archive& field, Sprite& sprite) {
    field.Enum("type", sprite.type, kSpriteTypes);
    field("width", sprite.width);
    field("height", sprite.height);
    field("uv0", sprite.uv0);
    field("uv1", sprite.uv1);
    field("color", sprite.color);
    field("texture", sprite.texture);
    field("layer", sprite.layer);
    field("anchorMin", sprite.anchorMin);
    field("anchorMax", sprite.anchorMax);
    field("label", sprite.label);
    field("labelFontSize", sprite.labelFontSize);
    field("labelColor", sprite.labelColor);
}

template<class Archive, class Text>
void TextFields(const Archive& field, Text& text) {
    field("text", text.text);
    field("fontSize", text.fontSize);
    field("color", text.color);
    field("layer", text.layer);
    field.Enum("renderMode", text.renderMode, kTextModes);
    field("anchorMin", text.anchorMin);
    field("anchorMax", text.anchorMax);
    field("pivot", text.pivot);
}

template<class Archive, class Button>
void ButtonFields(const Archive& field, Button& button) {
    field("width", button.width);
    field("height", button.height);
    field("fillColor", button.fillColor);
    field("hoverColor", button.hoverColor);
    field("textColor", button.textColor);
    field("text", button.text);
    field("fontSize", button.fontSize);
    field("layer", button.layer);
    field("anchorMin", button.anchorMin);
    field("anchorMax", button.anchorMax);
    field("pivot", button.pivot);
}

template<class Archive, class Slice>
void SliceFields(const Archive& field, Slice& slice) {
    field("texture", slice.texture);
    field("width", slice.width);
    field("height", slice.height);
    field("border", slice.border);
    field("uv0", slice.uv0);
    field("uv1", slice.uv1);
    field("color", slice.color);
    field("layer", slice.layer);
}

UiDocumentNode ReadNode(const Json& data, int depth) {
    if (depth > kMaxNodeDepth)
        throw std::runtime_error("UI document exceeds the Canvas2D hierarchy depth limit");
    UiDocumentNode node;
    NodeFields(Reader(data), node);
    if (node.name.empty()) node.name = node.id;
    if (data.contains("sprite")) SpriteFields(Reader(data.at("sprite")), node.sprite.emplace());
    if (data.contains("text")) TextFields(Reader(data.at("text")), node.text.emplace());
    if (data.contains("button")) ButtonFields(Reader(data.at("button")), node.button.emplace());
    if (data.contains("slice9")) SliceFields(Reader(data.at("slice9")), node.slice9.emplace());
    if (const auto children = data.find("children"); children != data.end()) {
        if (!children->is_array()) throw std::runtime_error("children must be an array");
        for (const auto& child : *children) node.children.push_back(ReadNode(child, depth + 1));
    }
    return node;
}

void ValidateNodes(const std::vector<UiDocumentNode>& nodes, int depth,
                   std::unordered_set<std::string>& ids) {
    for (const auto& node : nodes) {
        if (depth > kMaxNodeDepth)
            throw std::runtime_error("UI document exceeds the Canvas2D hierarchy depth limit");
        if (node.id.empty()) throw std::runtime_error("UI node requires a nonempty id");
        if (!ids.insert(node.id).second) throw std::runtime_error("Duplicate UI node id: " + node.id);
        if (ids.size() >= ECS::MAX_ENTITIES) throw std::runtime_error("UI document exceeds ECS capacity");
        ValidateNodes(node.children, depth + 1, ids);
    }
}

void Validate(const UiDocument& document) {
    if (!std::isfinite(document.canvas.width) || !std::isfinite(document.canvas.height) ||
        document.canvas.width <= 0.0f || document.canvas.height <= 0.0f)
        throw std::runtime_error("Canvas dimensions must be positive");
    std::unordered_set<std::string> ids;
    ValidateNodes(document.nodes, 0, ids);
}

template<class Component>
void AddWidget(Entity entity, const std::optional<Component>& value, bool visible) {
    if (!value) return;
    auto component = *value;
    component.isUI = true;
    component.visible = visible;
    Ecs().AddComponent(entity, std::move(component));
}

void AddNode(const UiDocumentNode& node, Entity parent, UiInstance& instance) {
    const Entity entity = Scene().CreateEmpty(node.name.empty() ? node.id : node.name);
    Scene().SetParent(entity, parent);
    Scene().SetPosition(entity, {node.position.x, node.position.y, 0.0f});
    Scene().SetScale(entity, {node.scale.x, node.scale.y, 1.0f});
    Scene().SetRotationEuler(entity, {0.0f, 0.0f, node.rotation});
    AddWidget(entity, node.sprite, node.visible);
    AddWidget(entity, node.text, node.visible);
    AddWidget(entity, node.button, node.visible);
    AddWidget(entity, node.slice9, node.visible);
    if (!node.visible && !node.sprite && !node.text && !node.button && !node.slice9) {
        ECS::RenderComponent group;
        group.visible = false;
        Ecs().AddComponent(entity, group);
    }
    instance.nodes.emplace(entity, NodeIdentity{Ecs().GetEntityHandle(entity), node.id, node.action});
    for (const auto& child : node.children) AddNode(child, entity, instance);
}

template<class T>
std::optional<T> Widget(Entity entity) {
    if (!Ecs().HasComponent<T>(entity)) return std::nullopt;
    const auto& widget = Ecs().GetComponent<T>(entity);
    if (!widget.isUI) throw std::runtime_error("Canvas contains a world-space widget: " + Scene().GetName(entity));
    return widget;
}

ECS::Signature CapturableComponents(bool isCanvas) {
    ECS::Signature allowed;
    allowed.set(Ecs().GetComponentType<ECS::NameComponent>());
    allowed.set(Ecs().GetComponentType<ECS::TransformComponent>());
    allowed.set(Ecs().GetComponentType<ECS::HierarchyComponent>());
    if (isCanvas) {
        allowed.set(Ecs().GetComponentType<ECS::Canvas2DComponent>());
    } else {
        allowed.set(Ecs().GetComponentType<ECS::Sprite2DComponent>());
        allowed.set(Ecs().GetComponentType<ECS::TextComponent>());
        allowed.set(Ecs().GetComponentType<ECS::ButtonComponent>());
        allowed.set(Ecs().GetComponentType<ECS::Slice9Component>());
        allowed.set(Ecs().GetComponentType<ECS::RenderComponent>());
    }
    return allowed;
}

void CaptureNode(Entity entity, int depth, const UiInstance& previous, UiInstance& next,
                 std::unordered_set<std::string>& ids, unsigned& nextId,
                 const ECS::Signature& allowed, UiDocumentNode& node) {
    if (depth > kMaxNodeDepth)
        throw std::runtime_error("UI document exceeds the Canvas2D hierarchy depth limit");
    node.name = Scene().GetName(entity);
    if ((Ecs().GetEntitySignature(entity) & ~allowed).any())
        throw std::runtime_error("UI document cannot store scene components on: " + node.name);

    const auto existing = previous.nodes.find(entity);
    const auto handle = Ecs().GetEntityHandle(entity);
    if (existing != previous.nodes.end() && existing->second.handle == handle) {
        node.id = existing->second.id;
        node.action = existing->second.action;
    } else {
        do {
            node.id = "node-" + std::to_string(nextId++);
        } while (!ids.insert(node.id).second);
    }
    next.nodes.emplace(entity, NodeIdentity{handle, node.id, node.action});
    node.position = glm::vec2(Scene().GetPosition(entity));
    node.scale = glm::vec2(Scene().GetScale(entity));
    node.rotation = Scene().GetRotationEuler(entity).z;
    node.visible = Scene().IsVisible(entity);
    node.sprite = Widget<ECS::Sprite2DComponent>(entity);
    node.text = Widget<ECS::TextComponent>(entity);
    node.button = Widget<ECS::ButtonComponent>(entity);
    node.slice9 = Widget<ECS::Slice9Component>(entity);
    for (const Entity child : Scene().GetChildren(entity)) {
        auto& entry = node.children.emplace_back();
        CaptureNode(child, depth + 1, previous, next, ids, nextId, allowed, entry);
    }
}

UiDocument Capture(Entity canvas, UiInstance& next) {
    if (!Ecs().HasComponent<ECS::Canvas2DComponent>(canvas))
        throw std::runtime_error("Choose an existing Canvas2D entity");
    if (Scene().GetParent(canvas) != INVALID_ENTITY)
        throw std::runtime_error("An external UI canvas must be a scene root");
    if ((Ecs().GetEntitySignature(canvas) & ~CapturableComponents(true)).any())
        throw std::runtime_error("UI document cannot store additional scene components on the canvas root");

    const UiInstance empty;
    const auto* instance = Instance(canvas);
    const auto& previous = instance ? *instance : empty;
    next.path = previous.path;
    next.root = Ecs().GetEntityHandle(canvas);
    std::unordered_set<std::string> ids;
    for (const auto& [entity, identity] : previous.nodes) ids.insert(identity.id);
    unsigned nextId = 1;
    UiDocument document;
    document.name = Scene().GetName(canvas);
    document.canvas = Ecs().GetComponent<ECS::Canvas2DComponent>(canvas);
    const auto allowed = CapturableComponents(false);
    for (const Entity child : Scene().GetChildren(canvas)) {
        auto& node = document.nodes.emplace_back();
        CaptureNode(child, 0, previous, next, ids, nextId, allowed, node);
    }
    Validate(document);
    return document;
}

Json WriteNode(const UiDocumentNode& node) {
    auto data = Json::object();
    NodeFields(Writer(data), node);
    if (node.sprite) SpriteFields(Writer(data["sprite"]), *node.sprite);
    if (node.text) TextFields(Writer(data["text"]), *node.text);
    if (node.button) ButtonFields(Writer(data["button"]), *node.button);
    if (node.slice9) SliceFields(Writer(data["slice9"]), *node.slice9);
    if (!node.children.empty()) {
        data["children"] = Json::array();
        for (const auto& child : node.children) data["children"].push_back(WriteNode(child));
    }
    return data;
}

std::string RenderJson(const UiDocument& document) {
    Json json = {{"formatVersion", kFormatVersion}, {"nodes", Json::array()}};
    CanvasFields(Writer(json["canvas"]), document);
    for (const auto& node : document.nodes) json["nodes"].push_back(WriteNode(node));
    return json.dump(2) + "\n";
}

} // namespace

bool ParseUiDocument(std::string_view json, UiDocument& document, std::string* error) {
    try {
        const auto data = Json::parse(json.begin(), json.end(), [](int depth, Json::parse_event_t, Json&) {
            if (depth > (kMaxNodeDepth + 1) * 2 + 6)
                throw std::runtime_error("UI JSON is nested too deeply");
            return true;
        });
        int version = 0;
        const Reader rootFields(data);
        rootFields("formatVersion", version);
        if (version != kFormatVersion)
            throw std::runtime_error("Unsupported UI document formatVersion: " + std::to_string(version));

        UiDocument parsed;
        CanvasFields(Reader(data.at("canvas")), parsed);
        const auto& nodes = data.at("nodes");
        if (!nodes.is_array()) throw std::runtime_error("nodes must be an array");
        for (const auto& node : nodes) parsed.nodes.push_back(ReadNode(node, 0));
        Validate(parsed);
        document = std::move(parsed);
        Report(error);
        return true;
    } catch (const std::exception& e) {
        Report(error, e.what());
        return false;
    }
}

bool LoadUiDocument(const std::string& resolvedPath, UiDocument& document, std::string* error) {
    size_t bytes = 0;
    const std::unique_ptr<void, decltype(&SDL_free)> data(
        SDL_LoadFile(resolvedPath.c_str(), &bytes), &SDL_free);
    if (!data) {
        Report(error, "Could not open " + resolvedPath + ": " + SDL_GetError());
        return false;
    }
    std::string detail;
    if (!ParseUiDocument(std::string_view(static_cast<const char*>(data.get()), bytes),
                         document, error ? &detail : nullptr)) {
        Report(error, resolvedPath + ": " + detail);
        return false;
    }
    Report(error);
    return true;
}

Entity InstantiateUiDocument(const UiDocument& document, const std::string& projectRelativePath,
                             std::string* error) {
    Entity canvas = INVALID_ENTITY;
    try {
        Validate(document);
        UiInstance instance;
        instance.path = AssetPath(projectRelativePath);
        std::erase_if(g_instances, [](const auto& entry) { return !Ecs().IsAlive(entry.second.root); });
        canvas = Scene().CreateEmpty(document.name);
        Ecs().AddComponent(canvas, document.canvas);
        instance.root = Ecs().GetEntityHandle(canvas);
        for (const auto& node : document.nodes) AddNode(node, canvas, instance);
        g_instances.insert_or_assign(canvas, std::move(instance));
        Report(error);
        return canvas;
    } catch (const std::exception& e) {
        if (canvas != INVALID_ENTITY) Scene().DestroyEntityHierarchy(canvas);
        Report(error, e.what());
        return INVALID_ENTITY;
    }
}

bool LinkUiDocument(Entity canvas, const std::string& projectRelativePath, std::string* error) {
    try {
        const auto path = AssetPath(projectRelativePath);
        if (path.empty()) throw std::runtime_error("Choose a project-relative UI document path");
        const auto existing = FindUiDocumentByPath(path);
        if (existing != INVALID_ENTITY && existing != canvas)
            throw std::runtime_error("UI document is already open in this scene");
        UiInstance next;
        Capture(canvas, next);
        next.path = path;
        g_instances.insert_or_assign(canvas, std::move(next));
        Report(error);
        return true;
    } catch (const std::exception& e) {
        Report(error, e.what());
        return false;
    }
}

std::string GetUiDocumentPath(Entity canvas) {
    const auto* instance = Instance(canvas);
    return instance ? instance->path : std::string{};
}

Entity FindUiDocumentRoot(Entity entity) {
    for (size_t depth = 0; entity != INVALID_ENTITY && depth < ECS::MAX_ENTITIES; ++depth) {
        if (Instance(entity)) return entity;
        entity = Scene().GetParent(entity);
    }
    return INVALID_ENTITY;
}

Entity FindUiDocumentByPath(const std::string& projectRelativePath) {
    std::string path;
    try {
        path = AssetPath(projectRelativePath);
    } catch (const std::exception&) {
        return INVALID_ENTITY;
    }
    if (path.empty()) return INVALID_ENTITY;
    for (const auto& [entity, instance] : g_instances) {
        if (instance.path == path && Instance(entity)) return entity;
    }
    return INVALID_ENTITY;
}

Entity FindUiDocumentNode(Entity canvas, std::string_view id) {
    if (const auto* instance = Instance(canvas)) {
        for (const auto& [entity, node] : instance->nodes) {
            if (node.id == id && Ecs().IsAlive(node.handle) && FindUiDocumentRoot(entity) == canvas)
                return entity;
        }
    }
    return INVALID_ENTITY;
}

std::string GetUiDocumentAction(Entity entity) {
    if (const auto* instance = Instance(FindUiDocumentRoot(entity))) {
        const auto found = instance->nodes.find(entity);
        if (found != instance->nodes.end() && Ecs().IsAlive(found->second.handle))
            return found->second.action;
    }
    return {};
}

bool SerializeUiDocument(Entity canvas, std::string& json, std::string* error) {
    try {
        UiInstance next;
        json = RenderJson(Capture(canvas, next));
        Report(error);
        return true;
    } catch (const std::exception& e) {
        Report(error, e.what());
        return false;
    }
}

bool SaveUiDocument(Entity canvas, const std::string& resolvedPath, std::string* error) {
    try {
        UiInstance next;
        const std::string json = RenderJson(Capture(canvas, next));
        if (!Core::WriteAssetFileAtomically(resolvedPath, json, "ui document")) {
            Report(error, "Could not save UI document: " + resolvedPath);
            return false;
        }
        g_instances.insert_or_assign(canvas, std::move(next));
        Report(error);
        return true;
    } catch (const std::exception& e) {
        Report(error, e.what());
        return false;
    }
}

} // namespace UI
