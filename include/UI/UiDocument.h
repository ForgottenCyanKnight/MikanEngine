#pragma once

#include "ECS/Components.h"
#include "ECS/Types.h"
#include "Platform/Export.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UI {

struct UiDocumentNode {
    std::string id;
    std::string name;
    std::string action;
    glm::vec2 position{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f;
    bool visible = true;
    std::optional<ECS::Sprite2DComponent> sprite;
    std::optional<ECS::TextComponent> text;
    std::optional<ECS::ButtonComponent> button;
    std::optional<ECS::Slice9Component> slice9;
    std::vector<UiDocumentNode> children;
};

struct UiDocument {
    std::string name = "Canvas";
    ECS::Canvas2DComponent canvas;
    std::vector<UiDocumentNode> nodes;
};

MIKAN_API bool ParseUiDocument(std::string_view json, UiDocument& document,
                                std::string* error = nullptr);
MIKAN_API bool LoadUiDocument(const std::string& resolvedPath, UiDocument& document,
                                std::string* error = nullptr);

// A non-empty project-relative path makes the canvas an external scene reference.
// An empty path also supports standalone instances owned by a game or editor preview.
MIKAN_API ECS::Entity InstantiateUiDocument(const UiDocument& document,
                                             const std::string& projectRelativePath = {},
                                             std::string* error = nullptr);
MIKAN_API bool LinkUiDocument(ECS::Entity canvas, const std::string& projectRelativePath,
                                std::string* error = nullptr);
MIKAN_API std::string GetUiDocumentPath(ECS::Entity canvas);
MIKAN_API ECS::Entity FindUiDocumentRoot(ECS::Entity entity);
MIKAN_API ECS::Entity FindUiDocumentByPath(const std::string& projectRelativePath);
MIKAN_API ECS::Entity FindUiDocumentNode(ECS::Entity canvas, std::string_view id);
MIKAN_API std::string GetUiDocumentAction(ECS::Entity entity);

MIKAN_API bool SerializeUiDocument(ECS::Entity canvas, std::string& json,
                                     std::string* error = nullptr);
MIKAN_API bool SaveUiDocument(ECS::Entity canvas, const std::string& resolvedPath,
                                std::string* error = nullptr);

} // namespace UI
