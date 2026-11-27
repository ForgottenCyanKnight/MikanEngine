#pragma once

#include "Platform/Export.h"
#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <vector>

namespace Input {

struct GamePointerEdge {
    glm::vec2 uv;
    bool down;
};

// Game View image in SDL window coordinates. The standalone view uses the full client area.
MIKAN_API void SetEditorGamePointerRect(const glm::vec4& rect);
MIKAN_API glm::vec2 GamePointerUv(glm::vec2 windowPosition, SDL_Window* window);
MIKAN_API void RecordGamePointerEdge(const SDL_MouseButtonEvent& event, SDL_Window* window);
MIKAN_API void ClearGamePointerEdges();
MIKAN_API const std::vector<GamePointerEdge>& GetGamePointerEdges();

} // namespace Input
